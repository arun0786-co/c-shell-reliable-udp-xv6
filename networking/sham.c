/* S.H.A.M.: a reliable byte stream over UDP.
 *
 * Both programs drive a connection the same way: write bytes into the send
 * buffer with sham_write, and call sham_poll in a loop. sham_poll waits for a
 * packet, a timer or the caller's own descriptor, then cuts the send buffer
 * into packets as far as the windows allow, retransmits what timed out and
 * hands in-order data to the deliver callback. */
#include "sham.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HEADER_LEN sizeof(struct sham_header)
#define NO_DEADLINE INT64_MAX

static FILE *log_file;

void sham_log_open(const char *filename)
{
    const char *on = getenv("RUDP_LOG");
    if (on == NULL || strcmp(on, "1") != 0)
        return;
    log_file = fopen(filename, "w");
    if (log_file == NULL)
        perror(filename);
}

void sham_log(const char *fmt, ...)
{
    if (log_file == NULL)
        return;
    struct timeval tv;
    struct tm tm;
    char when[32];
    gettimeofday(&tv, NULL);
    time_t secs = tv.tv_sec;
    localtime_r(&secs, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(log_file, "[%s.%06ld] [LOG] ", when, (long)tv.tv_usec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}

void sham_seed(void)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 16));
}

int sham_parse_port(const char *s)
{
    char *end;
    errno = 0;
    long port = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || port < 1 || port > 65535)
        return -1;
    return (int)port;
}

int sham_parse_loss(const char *s, double *rate)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || !(v >= 0.0 && v <= 1.0))
        return -1;
    *rate = v;
    return 0;
}

/* timers use the monotonic clock, which never jumps when the wall clock is
   changed; only log timestamps use the time of day */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* sequence numbers wrap around at 2^32, so compare them by their difference */
static int seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint32_t random_isn(void)
{
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

static int lose_packet(double rate)
{
    return rate > 0 && rand() / ((double)RAND_MAX + 1) < rate;
}

static uint32_t seg_end(const struct sham_segment *s)
{
    return s->seq + s->len + ((s->flags & FIN_FLAG) ? 1 : 0);
}

/* TCP's rwnd = RcvBuffer - (LastByteRcvd - LastByteRead). In-order data is
   handed to the application at once, so only out-of-order data takes space. */
static uint16_t recv_window(const struct sham_conn *c)
{
    uint32_t used = 0;
    for (int i = 0; i < c->nooo; i++) {
        uint32_t end = c->ooo[i].seq + c->ooo[i].len - c->rcv_nxt;
        if (end > used)
            used = end;
    }
    return (uint16_t)(RCV_BUFFER - used);
}

static void socket_error(struct sham_conn *c, int err)
{
    /* a datagram that could not go out is no different from one lost on the
       way, and retransmission takes care of both */
    if (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS || err == EINTR)
        return;
    if (err == ECONNREFUSED) {
        /* an ICMP "port unreachable" came back: the peer's program is gone */
        if (c->lingering)
            c->closed = 1;
        else
            c->error = "connection refused (the peer is not running)";
        return;
    }
    c->error = strerror(err);
}

static void send_packet(struct sham_conn *c, uint32_t seq, uint16_t flags,
                        const char *data, size_t len)
{
    unsigned char buf[HEADER_LEN + MAX_PAYLOAD];
    struct sham_header h;
    h.seq_num = htonl(seq);
    h.ack_num = htonl((flags & ACK_FLAG) ? c->rcv_nxt : 0);
    h.flags = htons(flags);
    h.window_size = htons(recv_window(c));
    memcpy(buf, &h, HEADER_LEN);
    if (len > 0)
        memcpy(buf + HEADER_LEN, data, len);
    if (send(c->fd, buf, HEADER_LEN + len, 0) < 0)
        socket_error(c, errno);
}

static void send_syn(struct sham_conn *c)
{
    send_packet(c, c->iss, SYN_FLAG, NULL, 0);
    sham_log("SND SYN SEQ=%u", c->iss);
}

static void send_synack(struct sham_conn *c)
{
    send_packet(c, c->iss, SYN_FLAG | ACK_FLAG, NULL, 0);
    sham_log("SND SYN-ACK SEQ=%u ACK=%u", c->iss, c->rcv_nxt);
}

static void send_syn_ack_ack(struct sham_conn *c)
{
    send_packet(c, c->iss + 1, ACK_FLAG, NULL, 0);
    sham_log("SND ACK FOR SYN");
}

static void send_ack(struct sham_conn *c)
{
    send_packet(c, c->snd_nxt, ACK_FLAG, NULL, 0);
    sham_log("SND ACK=%u WIN=%u", c->rcv_nxt, recv_window(c));
}

static void send_segment(struct sham_conn *c, struct sham_segment *s)
{
    send_packet(c, s->seq, s->flags | ACK_FLAG, s->data, s->len);
    s->sent_ms = now_ms();
}

static void retransmit_oldest(struct sham_conn *c, const char *why)
{
    struct sham_segment *s = &c->flight[0];
    sham_log("%s SEQ=%u", why, s->seq);
    send_segment(c, s);
    if (s->flags & FIN_FLAG)
        sham_log("RETX FIN SEQ=%u", s->seq);
    else
        sham_log("RETX DATA SEQ=%u LEN=%u", s->seq, (unsigned)s->len);
    c->retransmissions++;
}

/* sends new packets from the send buffer while both windows allow it */
static void transmit(struct sham_conn *c)
{
    while (c->nflight < WINDOW_PACKETS && c->error == NULL) {
        struct sham_segment *s = &c->flight[c->nflight];
        uint32_t in_flight = c->snd_nxt - c->snd_una;
        if (c->snd_len > 0) {
            size_t n = c->snd_len < MAX_PAYLOAD ? c->snd_len : MAX_PAYLOAD;
            /* flow control: LastByteSent - LastByteAcked <= advertised window.
               With nothing in flight one packet may go anyway, as a probe, so
               a stale small window cannot stall the connection for good. */
            if (in_flight > 0 && in_flight + n > c->snd_wnd)
                break;
            s->seq = c->snd_nxt;
            s->flags = 0;
            s->len = (uint16_t)n;
            memcpy(s->data, c->sndbuf + c->snd_start, n);
            c->snd_start += n;
            c->snd_len -= n;
            if (c->snd_len == 0)
                c->snd_start = 0;
            c->snd_nxt += (uint32_t)n;
            c->nflight++;
            send_segment(c, s);
            sham_log("SND DATA SEQ=%u LEN=%zu", s->seq, n);
        } else if (c->fin_queued && !c->fin_sent) {
            /* the FIN takes a sequence number of its own, so its ACK is
               distinguishable from an ACK of the last data byte */
            s->seq = c->snd_nxt;
            s->flags = FIN_FLAG;
            s->len = 0;
            c->snd_nxt += 1;
            c->fin_sent = 1;
            c->nflight++;
            send_segment(c, s);
            sham_log("SND FIN SEQ=%u", s->seq);
        } else {
            break;
        }
    }
}

static void handle_ack(struct sham_conn *c, uint32_t ack, uint16_t win)
{
    if (seq_before(ack, c->snd_una) || seq_before(c->snd_nxt, ack))
        return; /* older than what we know, or for data never sent */
    if (ack != c->snd_una) {
        /* cumulative: everything before ack has arrived */
        int k = 0;
        while (k < c->nflight && !seq_before(ack, seg_end(&c->flight[k]))) {
            if (c->flight[k].flags & FIN_FLAG)
                c->fin_acked = 1;
            k++;
        }
        memmove(c->flight, c->flight + k, (size_t)(c->nflight - k) * sizeof c->flight[0]);
        c->nflight -= k;
        c->snd_una = ack;
    }
    if (win != c->snd_wnd) {
        sham_log("FLOW WIN UPDATE=%u", win);
        c->snd_wnd = win;
    }
}

static void deliver(struct sham_conn *c, const char *data, size_t len)
{
    c->rcv_nxt += (uint32_t)len;
    if (c->deliver != NULL)
        c->deliver(c->ctx, data, len);
}

/* hands over buffered packets that the latest arrival made contiguous */
static void drain_ooo(struct sham_conn *c)
{
    int k = 0;
    while (k < c->nooo && !seq_before(c->rcv_nxt, c->ooo[k].seq)) {
        struct sham_segment *s = &c->ooo[k];
        if (seq_before(c->rcv_nxt, s->seq + s->len)) {
            size_t skip = c->rcv_nxt - s->seq;
            deliver(c, s->data + skip, s->len - skip);
        }
        k++;
    }
    memmove(c->ooo, c->ooo + k, (size_t)(c->nooo - k) * sizeof c->ooo[0]);
    c->nooo -= k;
}

static void keep_ooo(struct sham_conn *c, uint32_t seq, const char *data, size_t len)
{
    int i = 0;
    while (i < c->nooo && seq_before(c->ooo[i].seq, seq))
        i++;
    if (i < c->nooo && c->ooo[i].seq == seq)
        return; /* already buffered */
    if (c->nooo == OOO_SLOTS)
        return; /* no room; the sender will send it again */
    memmove(c->ooo + i + 1, c->ooo + i, (size_t)(c->nooo - i) * sizeof c->ooo[0]);
    c->ooo[i].seq = seq;
    c->ooo[i].flags = 0;
    c->ooo[i].len = (uint16_t)len;
    memcpy(c->ooo[i].data, data, len);
    c->nooo++;
}

static void handle_data(struct sham_conn *c, uint32_t seq, const char *data, size_t len)
{
    sham_log("RCV DATA SEQ=%u LEN=%zu", seq, len);
    uint32_t end = seq + (uint32_t)len;
    if (c->fin_rcvd || !seq_before(c->rcv_nxt, end)) {
        /* a copy of data we already have: our ACK was lost or slow */
    } else if (!seq_before(c->rcv_nxt, seq)) {
        size_t skip = c->rcv_nxt - seq;
        deliver(c, data + skip, len - skip);
        drain_ooo(c);
    } else if (end - c->rcv_nxt <= RCV_BUFFER) {
        keep_ooo(c, seq, data, len);
    }
    /* anything past the window is dropped; the ACK below repeats where the
       gap starts and how much room there is */
    send_ack(c);
}

static void handle_fin(struct sham_conn *c, uint32_t seq)
{
    sham_log("RCV FIN SEQ=%u", seq);
    if (!c->fin_rcvd && seq == c->rcv_nxt) {
        c->fin_rcvd = 1;
        c->rcv_nxt++;
        if (c->fin_sent)
            c->active_close = 1;
    }
    if (c->fin_rcvd && seq + 1 == c->rcv_nxt) {
        send_packet(c, c->snd_nxt, ACK_FLAG, NULL, 0);
        sham_log("SND ACK FOR FIN");
    } else {
        send_ack(c); /* FIN ahead of missing data: wait for the data first */
    }
}

static void handle_packet(struct sham_conn *c, const unsigned char *buf, size_t n)
{
    struct sham_header h;
    if (n < HEADER_LEN)
        return;
    memcpy(&h, buf, HEADER_LEN); /* copy, don't cast: buf need not be aligned */
    uint32_t seq = ntohl(h.seq_num);
    uint32_t ack = ntohl(h.ack_num);
    uint16_t flags = ntohs(h.flags);
    uint16_t win = ntohs(h.window_size);
    const char *data = (const char *)buf + HEADER_LEN;
    size_t len = n - HEADER_LEN;

    if (flags & SYN_FLAG) {
        /* the peer repeats its handshake packet because our reply was lost */
        if (!c->is_server && (flags & ACK_FLAG))
            send_syn_ack_ack(c);
        else if (c->is_server && !(flags & ACK_FLAG) && seq == c->irs)
            send_synack(c);
        return;
    }
    if (len > 0 && lose_packet(c->loss_rate)) {
        sham_log("DROP DATA SEQ=%u", seq);
        return;
    }
    c->last_heard = now_ms();
    if (flags & ACK_FLAG) {
        if (len == 0 && !(flags & FIN_FLAG))
            sham_log("RCV ACK=%u", ack);
        handle_ack(c, ack, win);
    }
    if (len > 0)
        handle_data(c, seq, data, len);
    if (flags & FIN_FLAG)
        handle_fin(c, seq + (uint32_t)len);
}

void sham_init(struct sham_conn *c, double loss_rate, sham_deliver_fn deliver, void *ctx)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->loss_rate = loss_rate;
    c->deliver = deliver;
    c->ctx = ctx;
    c->snd_wnd = RCV_BUFFER;
}

static int open_socket(struct sham_conn *c, const char *host, const char *port)
{
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV | (host == NULL ? AI_PASSIVE : 0);
    int err = getaddrinfo(host, port, &hints, &ai);
    if (err != 0) {
        c->error = gai_strerror(err);
        return -1;
    }
    c->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (c->fd < 0
        || (host == NULL ? bind(c->fd, ai->ai_addr, ai->ai_addrlen)
                         : connect(c->fd, ai->ai_addr, ai->ai_addrlen)) < 0
        || fcntl(c->fd, F_SETFL, O_NONBLOCK) < 0) {
        c->error = strerror(errno);
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    return 0;
}

/* waits until fd is readable or the deadline passes; 1 if readable */
static int wait_readable(int fd, int64_t deadline)
{
    fd_set fds;
    struct timeval tv, *tvp = NULL;
    if (deadline != NO_DEADLINE) {
        int64_t left = deadline - now_ms();
        if (left < 0)
            left = 0;
        tv.tv_sec = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)(left % 1000 * 1000);
        tvp = &tv;
    }
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    return select(fd + 1, &fds, NULL, NULL, tvp) > 0;
}

/* reads one datagram, waiting until the deadline. Returns its length, 0 on
   timeout and -1 on error. */
static ssize_t next_packet(struct sham_conn *c, int64_t deadline, unsigned char *buf,
                           struct sockaddr_in *from)
{
    for (;;) {
        socklen_t fromlen = sizeof *from;
        ssize_t n = from ? recvfrom(c->fd, buf, HEADER_LEN + MAX_PAYLOAD, 0,
                                    (struct sockaddr *)from, &fromlen)
                         : recv(c->fd, buf, HEADER_LEN + MAX_PAYLOAD, 0);
        if (n >= (ssize_t)HEADER_LEN)
            return n;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            socket_error(c, errno);
            if (c->error != NULL)
                return -1;
        }
        if (n < 0 && !wait_readable(c->fd, deadline) && now_ms() >= deadline)
            return 0;
    }
}

static void read_header(const unsigned char *buf, uint32_t *seq, uint32_t *ack,
                        uint16_t *flags, uint16_t *win)
{
    struct sham_header h;
    memcpy(&h, buf, HEADER_LEN);
    *seq = ntohl(h.seq_num);
    *ack = ntohl(h.ack_num);
    *flags = ntohs(h.flags);
    *win = ntohs(h.window_size);
}

int sham_connect(struct sham_conn *c, const char *host, const char *port)
{
    unsigned char buf[HEADER_LEN + MAX_PAYLOAD];
    uint32_t seq, ack;
    uint16_t flags, win;

    if (open_socket(c, host, port) < 0)
        return -1;
    c->iss = random_isn();
    for (int try = 0; try < HANDSHAKE_TRIES; try++) {
        if (try > 0)
            sham_log("TIMEOUT SEQ=%u", c->iss);
        send_syn(c);
        int64_t deadline = now_ms() + RTO_MS;
        ssize_t n;
        while ((n = next_packet(c, deadline, buf, NULL)) > 0) {
            read_header(buf, &seq, &ack, &flags, &win);
            if ((flags & (SYN_FLAG | ACK_FLAG)) != (SYN_FLAG | ACK_FLAG) || ack != c->iss + 1)
                continue;
            sham_log("RCV SYN-ACK SEQ=%u ACK=%u", seq, ack);
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->snd_una = c->snd_nxt = c->iss + 1;
            c->snd_wnd = win;
            c->last_heard = now_ms();
            send_syn_ack_ack(c);
            return c->error ? -1 : 0;
        }
        if (n < 0)
            return -1;
    }
    c->error = "no answer from the server";
    return -1;
}

int sham_accept(struct sham_conn *c, const char *port)
{
    unsigned char buf[HEADER_LEN + MAX_PAYLOAD];
    struct sockaddr_in from;
    uint32_t seq, ack;
    uint16_t flags, win;
    ssize_t n = 0;

    c->is_server = 1;
    if (open_socket(c, NULL, port) < 0)
        return -1;
    do {
        if (next_packet(c, NO_DEADLINE, buf, &from) < 0)
            return -1;
        read_header(buf, &seq, &ack, &flags, &win);
    } while (flags != SYN_FLAG);
    sham_log("RCV SYN SEQ=%u", seq);
    /* from now on the kernel only passes on this client's datagrams */
    if (connect(c->fd, (struct sockaddr *)&from, sizeof from) < 0) {
        c->error = strerror(errno);
        return -1;
    }
    c->irs = seq;
    c->rcv_nxt = seq + 1;
    c->snd_wnd = win;
    c->iss = random_isn();
    c->snd_una = c->snd_nxt = c->iss + 1;

    for (int try = 0; try < HANDSHAKE_TRIES; try++) {
        if (try > 0)
            sham_log("TIMEOUT SEQ=%u", c->iss);
        send_synack(c);
        int64_t deadline = now_ms() + RTO_MS;
        int resend = 0;
        while (!resend && (n = next_packet(c, deadline, buf, NULL)) > 0) {
            read_header(buf, &seq, &ack, &flags, &win);
            if (flags == SYN_FLAG && seq == c->irs) {
                resend = 1; /* our SYN-ACK was lost */
            } else if (!(flags & SYN_FLAG) && (flags & ACK_FLAG) && ack == c->iss + 1) {
                sham_log("RCV ACK FOR SYN");
                c->last_heard = now_ms();
                /* if the handshake ACK was lost, this is already data, and
                   its ACK flag completes the handshake just as well */
                if ((size_t)n > HEADER_LEN || (flags & FIN_FLAG))
                    handle_packet(c, buf, (size_t)n);
                return c->error ? -1 : 0;
            }
        }
        if (n < 0)
            return -1;
    }
    c->error = "the client stopped answering during the handshake";
    return -1;
}

size_t sham_write_space(const struct sham_conn *c)
{
    return c->fin_queued ? 0 : SND_BUFFER - c->snd_len;
}

size_t sham_write(struct sham_conn *c, const void *data, size_t len)
{
    if (c->fin_queued)
        return 0;
    if (c->snd_start + c->snd_len + len > SND_BUFFER && c->snd_start > 0) {
        memmove(c->sndbuf, c->sndbuf + c->snd_start, c->snd_len);
        c->snd_start = 0;
    }
    size_t room = SND_BUFFER - c->snd_start - c->snd_len;
    if (len > room)
        len = room;
    memcpy(c->sndbuf + c->snd_start + c->snd_len, data, len);
    c->snd_len += len;
    return len;
}

/* the FIN goes out after everything already written */
void sham_shutdown(struct sham_conn *c)
{
    c->fin_queued = 1;
}

static int waiting_for_peer(const struct sham_conn *c)
{
    if (c->lingering)
        return 0;
    return c->nflight > 0 || (c->expect_data && !c->fin_rcvd) || (c->fin_sent && !c->fin_rcvd);
}

static void update_close(struct sham_conn *c)
{
    if (c->closed || c->lingering || !(c->fin_acked && c->fin_rcvd))
        return;
    if (c->active_close) {
        /* TIME_WAIT: if our last ACK is lost the peer sends its FIN again,
           and someone has to be here to answer it */
        c->lingering = 1;
        c->linger_until = now_ms() + 2 * RTO_MS;
    } else {
        c->closed = 1;
    }
}

static void check_timers(struct sham_conn *c)
{
    int64_t now = now_ms();
    /* Every packet keeps its own send time, but only the oldest one is
       retransmitted: later packets may well have arrived, and a cumulative
       ACK cannot say. Once the oldest is acknowledged the next one's timer
       takes over, and fires at once if it has already run out. */
    if (c->nflight > 0 && now - c->flight[0].sent_ms >= RTO_MS)
        retransmit_oldest(c, "TIMEOUT");
    if (waiting_for_peer(c) && now - c->last_heard >= PEER_TIMEOUT_MS)
        c->error = "the peer stopped responding";
    if (c->lingering && now >= c->linger_until)
        c->closed = 1;
}

static void read_packets(struct sham_conn *c)
{
    unsigned char buf[HEADER_LEN + MAX_PAYLOAD];
    while (c->error == NULL && !c->closed) {
        ssize_t n = recv(c->fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                socket_error(c, errno);
            return;
        }
        handle_packet(c, buf, (size_t)n);
    }
}

/* Waits for a packet, a timer, or extra_fd (when >= 0) to become readable,
   and handles what happened. Returns 1 once the connection is closed, 0 to
   keep going and -1 on error (c->error says why). */
int sham_poll(struct sham_conn *c, int extra_fd, int *extra_ready)
{
    if (extra_ready != NULL)
        *extra_ready = 0;
    transmit(c);
    update_close(c);
    if (c->error != NULL)
        return -1;
    if (c->closed)
        return 1;

    int64_t now = now_ms(), deadline = NO_DEADLINE;
    if (c->nflight > 0)
        deadline = c->flight[0].sent_ms + RTO_MS;
    if (waiting_for_peer(c) && c->last_heard + PEER_TIMEOUT_MS < deadline)
        deadline = c->last_heard + PEER_TIMEOUT_MS;
    if (c->lingering && c->linger_until < deadline)
        deadline = c->linger_until;

    fd_set fds;
    struct timeval tv, *tvp = NULL;
    if (deadline != NO_DEADLINE) {
        int64_t left = deadline > now ? deadline - now : 0;
        tv.tv_sec = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)(left % 1000 * 1000);
        tvp = &tv;
    }
    FD_ZERO(&fds);
    FD_SET(c->fd, &fds);
    if (extra_fd >= 0)
        FD_SET(extra_fd, &fds);
    int maxfd = extra_fd > c->fd ? extra_fd : c->fd;
    int ready = select(maxfd + 1, &fds, NULL, NULL, tvp);
    if (ready < 0) {
        if (errno == EINTR)
            return 0; /* a signal; let the caller look at it */
        c->error = strerror(errno);
        return -1;
    }
    if (ready > 0 && FD_ISSET(c->fd, &fds))
        read_packets(c);
    if (ready > 0 && extra_fd >= 0 && FD_ISSET(extra_fd, &fds) && extra_ready != NULL)
        *extra_ready = 1;
    check_timers(c);
    transmit(c);
    update_close(c);
    if (c->error != NULL)
        return -1;
    return c->closed;
}

void sham_close(struct sham_conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}
