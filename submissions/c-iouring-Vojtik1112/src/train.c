/* train.c: Synthetic PGO workload driver (Rule 10: uses synthetic DB, no seed data). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sha256.h"

static unsigned b64url_encode(const uint8_t *in, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned o = 0;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = T[(v >> 6) & 63];
    }
    out[o] = '\0';
    return o;
}

static int connect_loop(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int drain_n_responses(int fd, int expected) {
    char buf[65536];
    size_t have = 0;
    int got = 0;
    while (got < expected) {
        ssize_t r = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        have += (size_t)r;
        buf[have] = '\0';
        size_t pos = 0;
        while (got < expected) {
            char *hdr_end = strstr(buf + pos, "\r\n\r\n");
            if (!hdr_end) break;
            size_t hlen = (size_t)(hdr_end - (buf + pos)) + 4;
            long clen = 0;
            char *cl = strstr(buf + pos, "Content-Length: ");
            if (cl && cl < hdr_end) clen = atol(cl + 16);
            if (have - pos < hlen + (size_t)clen) break;
            pos += hlen + (size_t)clen;
            got++;
        }
        if (pos > 0) {
            memmove(buf, buf + pos, have - pos);
            have -= pos;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <port> <secret> <nreqs>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *secret = argv[2];
    int nreqs = atoi(argv[3]);

    struct hmac_key key;
    hmac_key_init(&key, (const uint8_t *)secret, strlen(secret));
    const char hdr[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    enum { NTOK = 64 };
    char tokens[NTOK][512];
    for (int i = 0; i < NTOK; i++) {
        int uid = 1 + i * 7;
        char pl[160], pl_b64[256], msg[400], sig_b64[64];
        int pn = snprintf(pl, sizeof pl, "{\"sub\":\"%d\",\"username\":\"user_%d\",\"exp\":4102444800}", uid, uid);
        b64url_encode((const uint8_t *)pl, (size_t)pn, pl_b64);
        int mn = snprintf(msg, sizeof msg, "%s.%s", hdr, pl_b64);
        uint8_t sig[32];
        hmac_sha256(&key, (const uint8_t *)msg, (size_t)mn, sig);
        b64url_encode(sig, 32, sig_b64);
        snprintf(tokens[i], sizeof tokens[i], "%s.%s", msg, sig_b64);
    }

    enum { NCON = 16, PIPE = 16 };
    int fds[NCON];
    for (int c = 0; c < NCON; c++) {
        fds[c] = connect_loop(port);
        if (fds[c] < 0) {
            perror("connect");
            return 1;
        }
    }

    // Exercise health + error paths cleanly on fd 0.
    {
        const char *edge =
            "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /posts/abc HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /posts/99999999 HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"
            "POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}"
            "POST /posts/1/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer bad.tok.sig\r\nContent-Length: 0\r\n\r\n";
        if (send_all(fds[0], edge, strlen(edge)) == 0) drain_n_responses(fds[0], 6);
    }

    char batch[32768];
    unsigned long rng = 123456789UL;
    int done = 0;
    while (done < nreqs) {
        for (int c = 0; c < NCON && done < nreqs; c++) {
            int pos = 0;
            int cnt = 0;
            for (int p = 0; p < PIPE && done < nreqs; p++, done++) {
                rng = rng * 6364136223846793005ULL + 1ULL;
                unsigned x = (unsigned)(rng >> 32) % 1000u;
                int pid = 1 + (int)((rng >> 12) % 40000u);
                int ti = (int)((rng >> 4) & (NTOK - 1));
                if (x < 460u) {
                    pos += snprintf(batch + pos, sizeof(batch) - (size_t)pos,
                                    "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
                } else if (x < 920u) {
                    pos += snprintf(batch + pos, sizeof(batch) - (size_t)pos,
                                    "GET /posts/%d HTTP/1.1\r\nHost: localhost\r\n\r\n", pid);
                } else if (x < 985u) {
                    pos += snprintf(batch + pos, sizeof(batch) - (size_t)pos,
                                    "POST /posts/%d/like HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n",
                                    pid, tokens[ti]);
                } else {
                    const char *jb = "{\"body\":\"pgo training \\\"escaped\\\" post\"}";
                    pos += snprintf(batch + pos, sizeof(batch) - (size_t)pos,
                                    "POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n%s",
                                    tokens[ti], strlen(jb), jb);
                }
                cnt++;
            }
            if (send_all(fds[c], batch, (size_t)pos) != 0) return 1;
            if (drain_n_responses(fds[c], cnt) != 0) return 1;
        }
    }

    for (int c = 0; c < NCON; c++) close(fds[c]);
    return 0;
}
