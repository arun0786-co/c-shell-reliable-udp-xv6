/* S.H.A.M. client: sends a file to the server, or chats with it */
#include "sham.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static struct sham_conn conn; /* about 100 KB of buffers: keep it off the stack */

static void usage(void)
{
    fprintf(stderr, "usage: client <server_ip> <server_port> <input_file> <output_file_name> [loss_rate]\n"
                    "       client <server_ip> <server_port> --chat [loss_rate]\n");
}

static double seconds_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (now.tv_nsec - start->tv_nsec) / 1e9;
}

/* The stream starts with the name the server should save under, ended by a
   NUL byte, followed by the file. */
static int send_file(struct sham_conn *c, FILE *in, const char *name)
{
    char buf[MAX_PAYLOAD];
    size_t total = 0;
    int eof = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    sham_write(c, name, strlen(name) + 1);
    for (;;) {
        /* keep the send buffer topped up; the window decides what leaves */
        while (!eof && sham_write_space(c) >= sizeof buf) {
            size_t n = fread(buf, 1, sizeof buf, in);
            sham_write(c, buf, n);
            total += n;
            if (n < sizeof buf) {
                if (ferror(in)) {
                    perror("client: read");
                    return -1;
                }
                eof = 1;
                sham_shutdown(c);
            }
        }
        int r = sham_poll(c, -1, NULL);
        if (r < 0)
            return -1;
        if (r > 0)
            break;
    }
    fprintf(stderr, "sent %zu bytes in %.2f s, %lu packets retransmitted\n",
            total, seconds_since(&start), c->retransmissions);
    return 0;
}

int main(int argc, char *argv[])
{
    double loss = 0.0;
    int chat_mode = argc >= 4 && strcmp(argv[3], "--chat") == 0;
    if (chat_mode ? argc > 5 : (argc < 5 || argc > 6)) {
        usage();
        return 2;
    }
    const char *loss_arg = argc == (chat_mode ? 5 : 6) ? argv[argc - 1] : NULL;
    if (sham_parse_port(argv[2]) < 0) {
        fprintf(stderr, "client: bad port %s\n", argv[2]);
        return 2;
    }
    if (loss_arg != NULL && sham_parse_loss(loss_arg, &loss) < 0) {
        fprintf(stderr, "client: loss_rate must be a number from 0 to 1\n");
        return 2;
    }

    FILE *in = NULL;
    if (!chat_mode) {
        size_t len = strlen(argv[4]);
        if (len == 0 || len > 255) {
            fprintf(stderr, "client: the output file name must be 1 to 255 bytes\n");
            return 2;
        }
        /* open the file before connecting, so a typo fails straight away */
        in = fopen(argv[3], "rb");
        if (in == NULL) {
            perror(argv[3]);
            return 1;
        }
    }

    sham_seed();
    sham_log_open("client_log.txt");
    struct chat chat = { "Server", 1 };
    sham_init(&conn, loss, chat_mode ? chat_print : NULL, &chat);
    int ok = sham_connect(&conn, argv[1], argv[2]) == 0;
    if (ok)
        ok = (chat_mode ? chat_run(&conn, &chat) : send_file(&conn, in, argv[4])) == 0;
    if (!ok && conn.error != NULL)
        fprintf(stderr, "client: %s\n", conn.error);
    sham_close(&conn);
    if (in != NULL)
        fclose(in);
    return ok ? 0 : 1;
}
