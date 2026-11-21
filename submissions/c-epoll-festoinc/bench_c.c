// bench_c.c: a small, fast load generator for the feed API.
//
// It keeps N keep-alive connections, follows the challenge request mix
// (feed -> post -> 15% like -> 2% create) with no think time, and measures
// request throughput and latency. Written to drive more load than k6 can on a
// few cores, so we can see the server's real CPU limit.
//
//   gcc -O2 -march=native bench_c.c -o bench_c
//   taskset -c 1-3 ./bench_c <connections> <seconds> [base_url] [tokens.json]
//
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONN 100000
#define RBUF 16384
#define WBUF 2048

typedef struct {
    int fd;
    int state;          // 0 = connecting, 1 = request outstanding
    int step;           // 0 feed, 1 post, 2 maybe like/create
    char *token;
    char rbuf[RBUF];
    size_t rlen;
    size_t expected;    // full response length once headers are parsed
    int status;
    char wbuf[WBUF];
    size_t wlen, woff;
    uint64_t t0;
} CConn;

static CConn *conns[MAX_CONN];
static int nconns;
static const char *host = "127.0.0.1";
static int port = 3000;
static int epfd;
static uint64_t total, errors, sum_us, max_us;
static uint64_t hist[10002]; // 100 us buckets, last is 1 s+
static volatile int running = 1;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void die(const char *m) { perror(m); exit(1); }

static void set_events(CConn *c, uint32_t ev) {
    struct epoll_event e;
    e.events = ev;
    e.data.ptr = c;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &e);
}

static long parse_content_length(const char *p, size_t n) {
    const char *end = p + n;
    for (const char *q = p; q + 16 < end; q++) {
        if (memcmp(q, "Content-Length: ", 16) == 0) {
            long v = 0;
            q += 16;
            while (q < end && *q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
            return v;
        }
    }
    return -1;
}

static int parse_status(const char *p, size_t n) {
    if (n < 12 || memcmp(p, "HTTP/1.1 ", 9) != 0) return 0;
    return (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
}

static void send_next(CConn *c);

static void complete(CConn *c) {
    uint64_t us = (now_ns() - c->t0) / 1000;
    total++;
    if (c->status < 200 || c->status >= 300) errors++;
    sum_us += us;
    if (us > max_us) max_us = us;
    uint64_t b = us / 100;
    if (b > 10000) b = 10000;
    hist[b]++;
    c->rlen = 0;
    c->expected = 0;
    c->status = 0;
    send_next(c);
}

static void on_readable(CConn *c) {
    for (;;) {
        if (c->rlen == sizeof(c->rbuf)) { c->rlen = 0; c->expected = 0; }
        ssize_t n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen, 0);
        if (n > 0) {
            c->rlen += (size_t)n;
            if (!c->expected) {
                char *he = memmem(c->rbuf, c->rlen, "\r\n\r\n", 4);
                if (he) {
                    size_t hlen = (size_t)(he - c->rbuf) + 4;
                    long cl = parse_content_length(c->rbuf, hlen);
                    if (cl < 0) { c->rlen = 0; continue; }
                    c->expected = hlen + (size_t)cl;
                    c->status = parse_status(c->rbuf, hlen);
                }
            }
            if (c->expected && c->rlen >= c->expected) {
                complete(c);
                if (c->wlen) return; // waiting for EPOLLOUT
                continue;
            }
            continue;
        }
        if (n == 0) { // server closed; reconnect
            close(c->fd);
            c->fd = -1;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close(c->fd);
        c->fd = -1;
        return;
    }
}

static void flush_write(CConn *c) {
    while (c->woff < c->wlen) {
        ssize_t n = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff, MSG_NOSIGNAL);
        if (n > 0) { c->woff += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            set_events(c, EPOLLIN | EPOLLOUT);
            return;
        }
        close(c->fd);
        c->fd = -1;
        return;
    }
    c->wlen = c->woff = 0;
    set_events(c, EPOLLIN);
}

static void send_next(CConn *c) {
    int id = 500000;
    const char *mode = getenv("MODE");
    if (mode) {
        int n;
        if (!strcmp(mode, "health"))
            n = snprintf(c->wbuf, WBUF, "GET /health HTTP/1.1\r\nHost: %s:%d\r\n\r\n", host, port);
        else if (!strcmp(mode, "post"))
            n = snprintf(c->wbuf, WBUF, "GET /posts/%d HTTP/1.1\r\nHost: %s:%d\r\n\r\n", id, host, port);
        else
            n = snprintf(c->wbuf, WBUF, "GET /feed HTTP/1.1\r\nHost: %s:%d\r\n\r\n", host, port);
        c->wlen = (size_t)n; c->woff = 0; c->t0 = now_ns();
        flush_write(c);
        return;
    }
    char *p = c->wbuf;
    const char *tok = c->token ? c->token : "";
    int n = 0;
    switch (c->step) {
        case 0:
            n = snprintf(p, WBUF,
                "GET /feed HTTP/1.1\r\nHost: %s:%d\r\n\r\n", host, port);
            c->step = 1;
            break;
        case 1:
            n = snprintf(p, WBUF,
                "GET /posts/%d HTTP/1.1\r\nHost: %s:%d\r\n\r\n", id, host, port);
            c->step = 2;
            break;
        case 2:
            if (rand() % 100 < 15) {
                int lid = getenv("LIKE_RANDOM") ? 1 + rand() % 500000 : id;
                n = snprintf(p, WBUF,
                    "POST /posts/%d/like HTTP/1.1\r\nHost: %s:%d\r\n"
                    "Authorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", lid, host, port, tok);
                c->step = 3;
            } else {
                c->step = 3;
                send_next(c);
                return;
            }
            break;
        default:
            if (rand() % 100 < 2) {
                char body[128];
                int bl = snprintf(body, sizeof(body), "{\"body\":\"bench %lu\"}", (unsigned long)(total & 0xfffff));
                n = snprintf(p, WBUF,
                    "POST /posts HTTP/1.1\r\nHost: %s:%d\r\n"
                    "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\n\r\n%s", host, port, tok, bl, body);
            } else {
                c->step = 0;
                send_next(c);
                return;
            }
            c->step = 0;
            break;
    }
    c->wlen = (size_t)n;
    c->woff = 0;
    c->t0 = now_ns();
    flush_write(c);
}

static void connect_conn(CConn *c) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) die("socket");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &a.sin_addr);
    c->fd = fd;
    c->state = 0;
    c->rlen = 0;
    c->expected = 0;
    c->wlen = c->woff = 0;
    struct epoll_event e;
    e.events = EPOLLOUT;
    e.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e) < 0) die("epoll_ctl add");
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
        c->state = 1;
        send_next(c);
        set_events(c, EPOLLIN | EPOLLOUT);
    } else if (errno == EINPROGRESS) {
        set_events(c, EPOLLOUT);
    } else {
        die("connect");
    }
}

static char *read_tokens(const char *path, int *count) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); *count = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) die("malloc");
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("fread");
    fclose(f);
    buf[sz] = 0;
    int cap = 4096, n = 0;
    char **toks = (char **)malloc(sizeof(char *) * (size_t)cap);
    if (!toks) die("malloc");
    const char *p = buf;
    while ((p = strstr(p, "\"token\":\"")) != NULL && n < cap) {
        p += 9;
        const char *e = strchr(p, '"');
        if (!e) break;
        toks[n++] = strndup(p, (size_t)(e - p));
        p = e + 1;
    }
    free(buf);
    *count = n;
    return (char *)toks;
}

int main(int argc, char **argv) {
    nconns = argc > 1 ? atoi(argv[1]) : 500;
    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    if (argc > 3) host = argv[3];
    if (argc > 4) {
        char *u = argv[4];
        char *colon = strrchr(u, ':');
        if (colon) {
            *colon = 0;
            port = atoi(colon + 1);
            host = u;
        } else host = u;
    }
    if (nconns < 1) nconns = 1;
    if (nconns > MAX_CONN) nconns = MAX_CONN;
    const char *tokfile = getenv("TOKENS");
    if (!tokfile) tokfile = "../../seed/tokens.json";
    int ntok = 0;
    char **toks = (char **)read_tokens(tokfile, &ntok);
    if (ntok == 0) fprintf(stderr, "warning: no tokens, like/create will fail\n");

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) die("epoll_create1");
    srand(12345);
    for (int i = 0; i < nconns; i++) {
        CConn *c = (CConn *)calloc(1, sizeof(CConn));
        if (!c) die("calloc");
        c->token = ntok ? toks[i % ntok] : NULL;
        conns[i] = c;
        connect_conn(c);
    }

    struct epoll_event evs[1024];
    uint64_t start = now_ns(), end = start + (uint64_t)seconds * 1000000000ull;
    while (now_ns() < end) {
        int n = epoll_wait(epfd, evs, 1024, 100);
        if (n < 0) { if (errno == EINTR) continue; die("epoll_wait"); }
        for (int i = 0; i < n; i++) {
            CConn *c = (CConn *)evs[i].data.ptr;
            if (!c || c->fd < 0) continue;
            uint32_t e = evs[i].events;
            if (e & (EPOLLERR | EPOLLHUP)) { close(c->fd); c->fd = -1; continue; }
            if (!c->state && (e & EPOLLOUT)) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err) { close(c->fd); c->fd = -1; continue; }
                c->state = 1;
                send_next(c);
                continue;
            }
            if (e & EPOLLOUT) flush_write(c);
            if (c->fd >= 0 && (e & EPOLLIN)) on_readable(c);
        }
    }

    double secs = (double)(now_ns() - start) / 1e9;
    uint64_t p95 = 0, p99 = 0, acc = 0, seen = 0;
    for (uint64_t i = 0; i <= 10000; i++) {
        seen += hist[i];
        if (!p95 && seen >= total * 95 / 100) p95 = i * 100;
        if (!p99 && seen >= total * 99 / 100) p99 = i * 100;
        acc += hist[i];
    }
    printf("connections=%d seconds=%.2f\n", nconns, secs);
    printf("requests=%llu  %.0f req/s  errors=%llu (%.3f%%)\n",
           (unsigned long long)total, total / secs,
           (unsigned long long)errors, total ? 100.0 * errors / total : 0.0);
    printf("latency us: avg=%.0f p95=%llu p99=%llu max=%llu\n",
           total ? (double)sum_us / total : 0.0,
           (unsigned long long)p95, (unsigned long long)p99,
           (unsigned long long)max_us);
    (void)acc;
    return 0;
}
