/* S.H.A.M. server: receives one file and prints its MD5, or chats */
#include "sham.h"

#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static struct sham_conn conn; /* about 100 KB of buffers: keep it off the stack */

/* where the received stream goes: a file name ended by NUL, then the file */
struct sink {
    char name[256];
    size_t namelen;
    int named;
    char part[300]; /* written here, renamed to name once complete */
    FILE *file;
    const char *error;
};

static void usage(void)
{
    fprintf(stderr, "usage: server <port> [--chat] [loss_rate]\n");
}

/* The name comes from the network, so only its last path component is used:
   a client must not be able to write to ../../anything. */
static int open_output(struct sink *s)
{
    const char *base = strrchr(s->name, '/');
    base = base ? base + 1 : s->name;
    if (*base == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        s->error = "the client sent an unusable file name";
        return -1;
    }
    memmove(s->name, base, strlen(base) + 1);
    snprintf(s->part, sizeof s->part, "%s.part", s->name);
    s->file = fopen(s->part, "wb");
    if (s->file == NULL) {
        s->error = strerror(errno);
        return -1;
    }
    return 0;
}

static void save(void *ctx, const char *data, size_t len)
{
    struct sink *s = ctx;
    if (s->error != NULL)
        return;
    while (!s->named && len > 0) {
        char ch = *data++;
        len--;
        if (ch == '\0') {
            s->named = 1;
            if (open_output(s) < 0)
                return;
        } else if (s->namelen + 1 < sizeof s->name) {
            s->name[s->namelen++] = ch;
        } else {
            s->error = "the client sent a file name that is too long";
            return;
        }
    }
    if (len > 0 && fwrite(data, 1, len, s->file) != len)
        s->error = strerror(errno);
}

static int md5_file(const char *path, char hex[33])
{
    unsigned char buf[8192], digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    size_t n;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = ctx != NULL && EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0)
        ok = EVP_DigestUpdate(ctx, buf, n);
    ok = ok && !ferror(f) && EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);
    fclose(f);
    for (unsigned int i = 0; ok && i < dlen; i++)
        sprintf(hex + 2 * i, "%02x", digest[i]);
    return ok ? 0 : -1;
}

static int receive_file(struct sham_conn *c, struct sink *s)
{
    int r;
    c->expect_data = 1;
    do {
        r = sham_poll(c, -1, NULL);
        /* the client has sent everything; we have nothing to send, so close
           our side straight away */
        if (c->fin_rcvd && !c->fin_queued)
            sham_shutdown(c);
    } while (r == 0);

    if (!c->fin_rcvd) {
        fprintf(stderr, "server: %s\n", c->error);
        return -1;
    }
    /* the whole file is here even if the last ACK of the close got lost */
    if (r < 0)
        fprintf(stderr, "server: closing: %s\n", c->error);
    if (s->error == NULL && !s->named)
        s->error = "the client sent no file name";
    if (s->file != NULL && fclose(s->file) != 0 && s->error == NULL)
        s->error = strerror(errno);
    if (s->error == NULL && rename(s->part, s->name) != 0)
        s->error = strerror(errno);
    if (s->error != NULL) {
        fprintf(stderr, "server: %s\n", s->error);
        if (s->file != NULL)
            remove(s->part);
        return -1;
    }
    char hex[33];
    if (md5_file(s->name, hex) < 0) {
        fprintf(stderr, "server: could not compute the MD5 of %s\n", s->name);
        return -1;
    }
    printf("MD5: %s\n", hex);
    return 0;
}

int main(int argc, char *argv[])
{
    int chat_mode = 0, have_loss = 0;
    double loss = 0.0;
    if (argc < 2 || argc > 4 || sham_parse_port(argv[1]) < 0) {
        usage();
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--chat") == 0 && !chat_mode) {
            chat_mode = 1;
        } else if (!have_loss && sham_parse_loss(argv[i], &loss) == 0) {
            have_loss = 1;
        } else {
            usage();
            return 2;
        }
    }

    sham_seed();
    sham_log_open("server_log.txt");
    struct chat chat = { "Client", 1 };
    struct sink sink;
    memset(&sink, 0, sizeof sink);
    if (chat_mode)
        sham_init(&conn, loss, chat_print, &chat);
    else
        sham_init(&conn, loss, save, &sink);

    int ok;
    if (sham_accept(&conn, argv[1]) < 0) {
        fprintf(stderr, "server: %s\n", conn.error);
        ok = 0;
    } else if (chat_mode) {
        ok = chat_run(&conn, &chat) == 0;
        if (!ok)
            fprintf(stderr, "server: %s\n", conn.error);
    } else {
        ok = receive_file(&conn, &sink) == 0;
    }
    sham_close(&conn);
    return ok ? 0 : 1;
}
