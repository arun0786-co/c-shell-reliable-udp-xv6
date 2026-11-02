/* chat mode, shared by the client and the server */
#include "sham.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void on_sigint(int sig)
{
    interrupted = 1;
}

/* prints the peer's messages, labelling the start of each line */
void chat_print(void *ctx, const char *data, size_t len)
{
    struct chat *chat = ctx;
    while (len > 0) {
        if (chat->line_start)
            printf("[%s]: ", chat->peer);
        const char *nl = memchr(data, '\n', len);
        size_t n = nl ? (size_t)(nl - data) + 1 : len;
        fwrite(data, 1, n, stdout);
        chat->line_start = nl != NULL;
        data += n;
        len -= n;
    }
    fflush(stdout);
}

/* Sends the complete lines in buf and returns the length of the unfinished
   one left at the front. "/quit" sets *quit instead of being sent. */
static size_t send_lines(struct sham_conn *c, char *buf, size_t have, int *quit)
{
    size_t start = 0;
    for (size_t i = 0; i < have && !*quit; i++) {
        if (buf[i] != '\n')
            continue;
        size_t n = i - start;
        if (n > 0 && buf[start + n - 1] == '\r')
            n--;
        if (n == 5 && memcmp(buf + start, "/quit", 5) == 0)
            *quit = 1;
        else
            sham_write(c, buf + start, i + 1 - start);
        start = i + 1;
    }
    if (*quit)
        return 0;
    have -= start;
    memmove(buf, buf + start, have);
    if (have == MAX_PAYLOAD) { /* a line longer than the buffer goes in pieces */
        sham_write(c, buf, have);
        have = 0;
    }
    return have;
}

/* Runs until both sides have closed. Typing /quit, end of input or Ctrl-C
   starts the FIN handshake; so does the peer leaving. */
int chat_run(struct sham_conn *c, struct chat *chat)
{
    /* no SA_RESTART, so Ctrl-C interrupts select and the loop sees it */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    fprintf(stderr, "connected, type /quit to leave\n");
    /* read() and not fgets(): stdio could pull several lines into its own
       buffer, where select() cannot see them */
    char line[MAX_PAYLOAD];
    size_t have = 0;
    int reading = 1;
    for (;;) {
        int ready, quit = 0;
        /* stdin is read only when a whole buffer fits in the send buffer, so
           a full window holds the typist back instead of losing lines */
        int watch = reading && sham_write_space(c) >= sizeof line;
        int r = sham_poll(c, watch ? STDIN_FILENO : -1, &ready);
        if (r != 0)
            return r < 0 ? -1 : 0;
        if (ready) {
            ssize_t n = read(STDIN_FILENO, line + have, sizeof line - have);
            if (n > 0) {
                have = send_lines(c, line, have + (size_t)n, &quit);
            } else if (n == 0 || errno != EINTR) {
                if (have > 0)
                    sham_write(c, line, have); /* last line had no newline */
                quit = 1;
            }
        }
        if (interrupted)
            quit = 1;
        if (reading && c->fin_rcvd) {
            fprintf(stderr, "%s left the chat\n", chat->peer);
            quit = 1;
        }
        if (reading && quit) {
            reading = 0;
            sham_shutdown(c);
        }
    }
}
