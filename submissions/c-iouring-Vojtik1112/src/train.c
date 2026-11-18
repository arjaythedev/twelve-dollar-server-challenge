/* train.c: Realistic concurrent pipe=1 hot-tail PGO workload driver (Rule 10: synthetic DB). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
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

static int send_all_blocking(int fd, const char *p, size_t n) {
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

static int drain_n_blocking(int fd, int expected) {
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

enum { NCON = 128, NTOK = 128 };

struct tconn {
    int fd;
    uint64_t rng;
    int max_pid;
    int waiting;
    char rbuf[32768];
    size_t rlen;
    char wbuf[1024];
    size_t wlen;
    size_t woff;
};

static char g_tokens[NTOK][512];

static void prep_next(struct tconn *c) {
    c->rng = c->rng * 6364136223846793005ULL + 1442695040888963407ULL;
    unsigned x = (unsigned)(c->rng >> 32) % 1000u;
    int hot_lo = c->max_pid >= 20 ? c->max_pid - 19 : 1;
    int pid = ((c->rng & 31u) != 0)
              ? (hot_lo + (int)((c->rng >> 12) % 20u))
              : (1 + (int)((c->rng >> 12) % (unsigned)c->max_pid));
    int ti = (int)((c->rng >> 20) & (NTOK - 1));
    int n;
    if (x < 455u) {
        n = snprintf(c->wbuf, sizeof c->wbuf, "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
    } else if (x < 910u) {
        n = snprintf(c->wbuf, sizeof c->wbuf, "GET /posts/%d HTTP/1.1\r\nHost: localhost\r\n\r\n", pid);
    } else if (x < 985u) {
        n = snprintf(c->wbuf, sizeof c->wbuf,
                     "POST /posts/%d/like HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n",
                     pid, g_tokens[ti]);
    } else {
        const char *jb = "{\"body\":\"pgo training keepalive server post\"}";
        n = snprintf(c->wbuf, sizeof c->wbuf,
                     "POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n%s",
                     g_tokens[ti], strlen(jb), jb);
    }
    c->wlen = (size_t)n;
    c->woff = 0;
    c->waiting = 1;
}

static int check_complete_resp(struct tconn *c) {
    c->rbuf[c->rlen] = '\0';
    char *hdr_end = strstr(c->rbuf, "\r\n\r\n");
    if (!hdr_end) return 0;
    size_t hlen = (size_t)(hdr_end - c->rbuf) + 4;
    long clen = 0;
    char *cl = strstr(c->rbuf, "Content-Length: ");
    if (cl && cl < hdr_end) clen = atol(cl + 16);
    if (c->rlen < hlen + (size_t)clen) return 0;
    size_t tot = hlen + (size_t)clen;
    if (c->rlen > tot) memmove(c->rbuf, c->rbuf + tot, c->rlen - tot);
    c->rlen -= tot;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <port> <secret> <nreqs> [max_pid]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *secret = argv[2];
    int nreqs = atoi(argv[3]);
    int max_pid = argc > 4 ? atoi(argv[4]) : 40000;

    struct hmac_key key;
    hmac_key_init(&key, (const uint8_t *)secret, strlen(secret));
    const char hdr[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    for (int i = 0; i < NTOK; i++) {
        int uid = 1 + i * 7;
        char pl[160], pl_b64[256], msg[400], sig_b64[64];
        int pn = snprintf(pl, sizeof pl, "{\"sub\":\"%d\",\"username\":\"user_%d\",\"exp\":4102444800}", uid, uid);
        b64url_encode((const uint8_t *)pl, (size_t)pn, pl_b64);
        int mn = snprintf(msg, sizeof msg, "%s.%s", hdr, pl_b64);
        uint8_t sig[32];
        hmac_sha256(&key, (const uint8_t *)msg, (size_t)mn, sig);
        b64url_encode(sig, 32, sig_b64);
        snprintf(g_tokens[i], sizeof g_tokens[i], "%s.%s", msg, sig_b64);
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

    /* Exercise health, error, and escaped paths cleanly on a single blocking socket first. */
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) return 1;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        char edge[2048];
        const char *esc_post = "{\"body\":\"pgo \\\"escaped\\\" body \\u0041\"}";
        int el = snprintf(edge, sizeof edge,
            "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /posts/abc HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /posts/99999999 HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"
            "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 2\r\n\r\n{}"
            "POST /posts/1/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer bad.tok.sig\r\nContent-Length: 0\r\n\r\n"
            "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n%s"
            , g_tokens[0], g_tokens[0], strlen(esc_post), esc_post);
        if (send_all_blocking(fd, edge, (size_t)el) != 0 || drain_n_blocking(fd, 7) != 0) return 1;
        close(fd);
    }

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) return 1;
    struct tconn *conns = calloc(NCON, sizeof(*conns));
    if (!conns) return 1;

    for (int i = 0; i < NCON; i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return 1;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) return 1;
        conns[i].fd = fd;
        conns[i].rng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)(i + 1) * 0xbf58476d1ce4e5b9ULL);
        conns[i].max_pid = max_pid;
        prep_next(&conns[i]);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = &conns[i]};
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    }

    struct epoll_event evs[NCON];
    int done = 0;
    while (done < nreqs) {
        int n = epoll_wait(ep, evs, NCON, 5000);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < n; k++) {
            struct tconn *c = (struct tconn *)evs[k].data.ptr;
            if (evs[k].events & (EPOLLERR | EPOLLHUP)) return 1;
            if (evs[k].events & EPOLLOUT) {
                while (c->woff < c->wlen) {
                    ssize_t w = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff, MSG_NOSIGNAL);
                    if (w > 0) c->woff += (size_t)w;
                    else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    else return 1;
                }
            }
            if (evs[k].events & EPOLLIN) {
                for (;;) {
                    if (c->rlen >= sizeof(c->rbuf) - 1) return 1;
                    ssize_t r = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - 1 - c->rlen, 0);
                    if (r > 0) {
                        c->rlen += (size_t)r;
                        while (check_complete_resp(c)) {
                            done++;
                            if (done < nreqs) {
                                prep_next(c);
                                while (c->woff < c->wlen) {
                                    ssize_t w = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff, MSG_NOSIGNAL);
                                    if (w > 0) c->woff += (size_t)w;
                                    else break;
                                }
                            }
                        }
                    } else if (r == 0) {
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        return 1;
                    }
                }
            }
        }
    }

    for (int i = 0; i < NCON; i++) close(conns[i].fd);
    free(conns);
    close(ep);
    return done >= nreqs ? 0 : 1;
}
