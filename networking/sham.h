#ifndef SHAM_H
#define SHAM_H

#include <stddef.h>
#include <stdint.h>

/* S.H.A.M. header. Every datagram is this header, in network byte order,
   followed by up to MAX_PAYLOAD bytes of data. */
struct sham_header {
    uint32_t seq_num;     /* stream position of the first data byte; SYN and FIN use one number each */
    uint32_t ack_num;     /* next byte expected from the peer (cumulative), valid when ACK is set */
    uint16_t flags;
    uint16_t window_size; /* free space in the sender's receive buffer, in bytes */
};

/* the fields line up with no padding, so the header is 12 bytes on the wire */
typedef char sham_header_is_12_bytes[sizeof(struct sham_header) == 12 ? 1 : -1];

#define SYN_FLAG 0x1
#define ACK_FLAG 0x2
#define FIN_FLAG 0x4

#define MAX_PAYLOAD 1024      /* data bytes per packet */
#define WINDOW_PACKETS 10     /* unacknowledged packets allowed in flight */
#define RCV_BUFFER 16384      /* receive buffer, advertised in window_size */
#define SND_BUFFER 65536      /* bytes the application may queue for sending */
#define OOO_SLOTS 32          /* out-of-order packets the receiver keeps */
#define RTO_MS 500            /* retransmission timeout */
#define HANDSHAKE_TRIES 10    /* SYN and SYN-ACK attempts before giving up */
#define PEER_TIMEOUT_MS 10000 /* give up when the peer is silent this long */
#ifndef DUP_ACK_THRESHOLD
#define DUP_ACK_THRESHOLD 3   /* duplicate ACKs that trigger a fast retransmit, 0 to turn it off */
#endif

struct sham_segment {
    uint32_t seq;
    uint16_t flags; /* FIN_FLAG for the closing segment, else 0 */
    uint16_t len;   /* data bytes */
    int64_t sent_ms;
    char data[MAX_PAYLOAD];
};

/* called with each run of in-order data as it arrives */
typedef void (*sham_deliver_fn)(void *ctx, const char *data, size_t len);

struct sham_conn {
    int fd;
    int is_server;
    double loss_rate;
    sham_deliver_fn deliver;
    void *ctx;
    int expect_data;   /* the peer should keep sending, so time out if it goes quiet */
    const char *error; /* why the connection failed */

    /* sending side */
    uint32_t iss, snd_una, snd_nxt; /* first sequence number, oldest unacked, next to send */
    uint16_t snd_wnd;               /* window the peer advertised */
    struct sham_segment flight[WINDOW_PACKETS]; /* sent but not acknowledged, oldest first */
    int nflight;
    int dupacks;
    int recovering;                 /* retransmitted, and waiting for ACKs up to recover */
    uint32_t recover;
    char sndbuf[SND_BUFFER];        /* written by the application, not yet sent */
    size_t snd_start, snd_len;
    int fin_queued, fin_sent, fin_acked;

    /* receiving side */
    uint32_t irs, rcv_nxt;          /* peer's first sequence number, next byte expected */
    struct sham_segment ooo[OOO_SLOTS]; /* arrived ahead of a gap, sorted by seq */
    int nooo;
    int fin_rcvd;

    /* closing */
    int active_close; /* our FIN went out before the peer's arrived */
    int lingering;
    int64_t linger_until;
    int closed;

    int64_t last_heard;
    unsigned long retransmissions;
};

/* sham.c */
void sham_log_open(const char *filename);
void sham_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sham_seed(void);
int sham_parse_port(const char *s);
int sham_parse_loss(const char *s, double *rate);

void sham_init(struct sham_conn *c, double loss_rate, sham_deliver_fn deliver, void *ctx);
int sham_connect(struct sham_conn *c, const char *host, const char *port);
int sham_accept(struct sham_conn *c, const char *port);
size_t sham_write_space(const struct sham_conn *c);
size_t sham_write(struct sham_conn *c, const void *data, size_t len);
void sham_shutdown(struct sham_conn *c);
int sham_poll(struct sham_conn *c, int extra_fd, int *extra_ready);
void sham_close(struct sham_conn *c);

/* chat.c */
struct chat {
    const char *peer; /* "Client" or "Server" */
    int line_start;
};
void chat_print(void *ctx, const char *data, size_t len);
int chat_run(struct sham_conn *c, struct chat *chat);

#endif
