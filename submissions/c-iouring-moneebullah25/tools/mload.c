/* mload: a closed-loop load generator with the bench/load.js user model.
 *
 * One virtual user = one keep-alive connection running: GET /feed, think
 * U(3,7) s, GET /posts/:id (a random post from the feed), think U(3,8) s,
 * POST like with p=0.15, POST create with p=0.02, idle U(5,15) s, repeat.
 * The first iteration starts after U(0,5) s. Think times scale with
 * --think-scale (0 = no think time, a pure throughput loop).
 *
 * Memory per VU is ~1 KB: responses are parsed as they stream in and only
 * the status, the content length and the post ids of a feed are kept.
 * Source addresses rotate over 127.0.0.1 .. 127.0.0.N so that more than
 * ~28k connections fit in the ephemeral port range.
 *
 *   mload --port 80 --vus 100000 --duration 300 --ramp 60 --tokens ../../seed/tokens.json
 */
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define EP_FEED   0
#define EP_POST   1
#define EP_LIKE   2
#define EP_CREATE 3
#define EP_COUNT  4
#define FEED_IDS  20
#define HBUF      512

enum vu_state { V_IDLE, V_CONNECTING, V_SENDING, V_READING };

struct hist { /* 10 us buckets to 1 s, then 1 ms buckets to 60 s */
    uint32_t fine[100000];
    uint32_t coarse[60000];
    uint64_t count, over, sum_us, max_us;
};

struct vu {
    int fd;
    uint8_t state, step, endpoint, hdr_done;
    uint32_t id;
    uint32_t iter;
    uint32_t last_ids[FEED_IDS];
    uint8_t n_ids;
    int64_t deadline_ns; /* when idle: wake-up time */
    int64_t req_start_ns;
    const char *tok;
    uint32_t tok_len;
    const char *user;
    uint32_t user_len;
    /* request being sent */
    char req[1024];
    uint32_t req_len, req_off;
    /* response parsing */
    char hbuf[HBUF];
    uint32_t hlen;
    uint64_t body_left;
    int status;
    /* streaming scan for "id": in feed bodies */
    uint8_t scan_state;
    uint64_t scan_num;
    int64_t post_id;
    uint32_t heap_pos;
    uint32_t rng;
};

struct heap {
    uint32_t *v;
    uint32_t n;
};

static struct {
    const char *host;
    int port;
    uint32_t vus;
    double duration, ramp, think_scale;
    uint32_t src_ips;
    const char *tokens_path;
    int report_every;
    uint32_t max_errors;
} opt = {.host = "127.0.0.1",
         .port = 3000,
         .vus = 100,
         .duration = 60,
         .ramp = 10,
         .think_scale = 1.0,
         .src_ips = 8,
         .tokens_path = "../../seed/tokens.json",
         .report_every = 10,
         .max_errors = UINT32_MAX};

static struct vu *vus;
static struct heap timers;
static int epfd;
static struct hist hist_all, hist_ep[EP_COUNT];
static uint64_t errors, errors_by_kind[8], requests, ep_requests[EP_COUNT], ep_bad[EP_COUNT], connects;
static char *tokens_blob;
static const char **tok_ptr, **user_ptr;
static uint32_t *tok_len, *user_len, ntokens;
static int64_t t_start;
static struct sockaddr_in dst;

static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static double urand(struct vu *v) {
    return (double)xorshift(&v->rng) / 4294967296.0;
}
static double between(struct vu *v, double lo, double hi) {
    return lo + urand(v) * (hi - lo);
}

static void hist_add(struct hist *h, uint64_t us) {
    h->count++;
    h->sum_us += us;
    if (us > h->max_us) h->max_us = us;
    if (us < 1000000) {
        h->fine[us / 10]++;
    } else if (us < 60000000) {
        h->coarse[us / 1000]++;
    } else {
        h->over++;
    }
}

static uint64_t hist_pct(const struct hist *h, double p) {
    uint64_t target = (uint64_t)((double)h->count * p);
    if (!h->count) return 0;
    if (target >= h->count) target = h->count - 1;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < 100000; i++) {
        acc += h->fine[i];
        if (acc > target) return (uint64_t)i * 10;
    }
    for (uint32_t i = 0; i < 60000; i++) {
        acc += h->coarse[i];
        if (acc > target) return (uint64_t)i * 1000;
    }
    return h->max_us;
}

/* ------------------------------------------------------------------ timers */

static void heap_swap(uint32_t a, uint32_t b) {
    uint32_t t = timers.v[a];
    timers.v[a] = timers.v[b];
    timers.v[b] = t;
    vus[timers.v[a]].heap_pos = a;
    vus[timers.v[b]].heap_pos = b;
}
static void heap_up(uint32_t i) {
    while (i && vus[timers.v[(i - 1) / 2]].deadline_ns > vus[timers.v[i]].deadline_ns) {
        heap_swap(i, (i - 1) / 2);
        i = (i - 1) / 2;
    }
}
static void heap_down(uint32_t i) {
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < timers.n && vus[timers.v[l]].deadline_ns < vus[timers.v[m]].deadline_ns) m = l;
        if (r < timers.n && vus[timers.v[r]].deadline_ns < vus[timers.v[m]].deadline_ns) m = r;
        if (m == i) return;
        heap_swap(i, m);
        i = m;
    }
}
static void timer_add(uint32_t vi, int64_t when) {
    vus[vi].deadline_ns = when;
    vus[vi].heap_pos = timers.n;
    timers.v[timers.n++] = vi;
    heap_up(vus[vi].heap_pos);
}
static int timer_peek(uint32_t *vi) {
    if (!timers.n) return 0;
    *vi = timers.v[0];
    return 1;
}
static void timer_pop(void) {
    timers.n--;
    if (timers.n) {
        timers.v[0] = timers.v[timers.n];
        vus[timers.v[0]].heap_pos = 0;
        heap_down(0);
    }
}

/* ------------------------------------------------------------------ user model */

static void vu_sleep(struct vu *v, double lo, double hi) {
    double s = between(v, lo, hi) * opt.think_scale;
    v->state = V_IDLE;
    timer_add(v->id, now_ns() + (int64_t)(s * 1e9));
}

static void vu_fail(struct vu *v, int kind);
static void vu_connect(struct vu *v);

static void build_request(struct vu *v, int ep) {
    char *r = v->req;
    int n;
    v->endpoint = (uint8_t)ep;
    switch (ep) {
    case EP_FEED: n = snprintf(r, sizeof v->req, "GET /feed HTTP/1.1\r\nHost: %s\r\n\r\n", opt.host); break;
    case EP_POST:
        n = snprintf(r, sizeof v->req, "GET /posts/%lld HTTP/1.1\r\nHost: %s\r\n\r\n", (long long)v->post_id, opt.host);
        break;
    case EP_LIKE:
        n = snprintf(r, sizeof v->req,
                     "POST /posts/%lld/like HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %.*s\r\n"
                     "Content-Type: application/json\r\nContent-Length: 0\r\n\r\n",
                     (long long)v->post_id, opt.host, (int)v->tok_len, v->tok);
        break;
    default: {
        char body[256];
        time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);
        int bl = snprintf(body, sizeof body,
                          "{\"body\":\"%.*s says hi at %04d-%02d-%02dT%02d:%02d:%02d.000Z (VU %u, iter %u)\"}",
                          (int)v->user_len, v->user, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                          tm.tm_min, tm.tm_sec, v->id + 1, v->iter);
        n = snprintf(r, sizeof v->req,
                     "POST /posts HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %.*s\r\n"
                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                     opt.host, (int)v->tok_len, v->tok, bl, body);
    }
    }
    v->req_len = (uint32_t)n;
    v->req_off = 0;
    v->hlen = 0;
    v->hdr_done = 0;
    v->body_left = 0;
    v->status = 0;
    v->scan_state = 0;
    v->n_ids = 0;
}

static void vu_send(struct vu *v) {
    if (v->fd < 0) {
        vu_connect(v);
        return;
    }
    v->state = V_SENDING;
    v->req_start_ns = now_ns();
    for (;;) {
        ssize_t w = send(v->fd, v->req + v->req_off, v->req_len - v->req_off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN) return;
            vu_fail(v, 1);
            return;
        }
        v->req_off += (uint32_t)w;
        if (v->req_off == v->req_len) {
            v->state = V_READING;
            return;
        }
    }
}

/* Advances the user loop after a response (or a failure) at the current step. */
static void vu_next(struct vu *v, int failed) {
    switch (v->step) {
    case 0: /* feed done */
        if (failed || v->n_ids == 0) {
            v->step = 0;
            vu_sleep(v, 3, 7);
            return;
        }
        v->post_id = v->last_ids[xorshift(&v->rng) % v->n_ids];
        v->step = 1;
        vu_sleep(v, 3, 7);
        return;
    case 1: /* post done */
        v->step = 2;
        vu_sleep(v, 3, 8);
        return;
    case 2: /* like done or skipped */ v->step = 3; break;
    case 3: /* create done or skipped */
        v->step = 0;
        v->iter++;
        vu_sleep(v, 5, 15);
        return;
    default: break;
    }
    /* step 3: maybe create */
    if (urand(v) < 0.02) {
        build_request(v, EP_CREATE);
        vu_send(v);
    } else {
        vu_next(v, 0);
    }
}

static void vu_wake(struct vu *v) {
    switch (v->step) {
    case 0:
        build_request(v, EP_FEED);
        vu_send(v);
        return;
    case 1:
        build_request(v, EP_POST);
        vu_send(v);
        return;
    case 2:
        if (urand(v) < 0.15) {
            build_request(v, EP_LIKE);
            vu_send(v);
        } else {
            vu_next(v, 0);
        }
        return;
    default: vu_next(v, 0);
    }
}

static void vu_close(struct vu *v) {
    if (v->fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, v->fd, NULL);
        close(v->fd);
        v->fd = -1;
    }
}

static void vu_fail(struct vu *v, int kind) {
    errors++;
    errors_by_kind[kind & 7]++;
    if (v->state == V_SENDING || v->state == V_READING) ep_bad[v->endpoint]++;
    vu_close(v);
    if (errors >= opt.max_errors) {
        fprintf(stderr, "too many errors, stopping\n");
        exit(2);
    }
    /* like k6: the iteration continues; the next request reconnects */
    if (v->state == V_CONNECTING) {
        v->step = 0;
        vu_sleep(v, 3, 7);
    } else {
        vu_next(v, 1);
    }
}

static void vu_connect(struct vu *v) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        vu_fail(v, 2);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (opt.src_ips > 1) {
        struct sockaddr_in src = {.sin_family = AF_INET};
        src.sin_addr.s_addr = htonl(0x7f000001u + (v->id % opt.src_ips));
#ifdef IP_BIND_ADDRESS_NO_PORT
        setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
#endif
        if (bind(fd, (struct sockaddr *)&src, sizeof src)) {
            perror("bind source");
            close(fd);
            vu_fail(v, 2);
            return;
        }
    }
    v->fd = fd;
    v->state = V_CONNECTING;
    connects++;
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.u32 = v->id};
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    if (connect(fd, (struct sockaddr *)&dst, sizeof dst) < 0 && errno != EINPROGRESS) {
        vu_fail(v, 2);
    }
}

/* Feeds response bytes to the per-VU parser; returns 1 when a response ended. */
static int vu_feed_bytes(struct vu *v, const char *p, size_t n) {
    size_t i = 0;
    if (!v->hdr_done) {
        while (i < n && v->hlen < HBUF) {
            v->hbuf[v->hlen++] = p[i++];
            if (v->hlen >= 4 && memcmp(v->hbuf + v->hlen - 4, "\r\n\r\n", 4) == 0) {
                v->hdr_done = 1;
                v->hbuf[v->hlen] = 0;
                v->status = atoi(v->hbuf + 9);
                const char *cl = strstr(v->hbuf, "Content-Length: ");
                v->body_left = cl ? strtoull(cl + 16, NULL, 10) : 0;
                break;
            }
        }
        if (!v->hdr_done) {
            if (v->hlen >= HBUF) {
                vu_fail(v, 3);
                return -1;
            }
            return 0;
        }
    }
    /* body: scan for "id": digits (only feed bodies need it) */
    while (i < n && v->body_left) {
        char c = p[i++];
        v->body_left--;
        if (v->endpoint == EP_FEED) {
            static const char key[] = "\"id\":";
            if (v->scan_state < 5) {
                v->scan_state = (c == key[v->scan_state]) ? (uint8_t)(v->scan_state + 1) : (uint8_t)(c == '"');
                if (v->scan_state == 5) v->scan_num = 0;
            } else if (c >= '0' && c <= '9') {
                v->scan_num = v->scan_num * 10 + (uint64_t)(c - '0');
            } else {
                if (v->n_ids < FEED_IDS) v->last_ids[v->n_ids++] = (uint32_t)v->scan_num;
                v->scan_state = 0;
            }
        }
    }
    if (v->body_left == 0) return 1;
    return 0;
}

static void vu_response_done(struct vu *v) {
    uint64_t us = (uint64_t)((now_ns() - v->req_start_ns) / 1000);
    hist_add(&hist_all, us);
    hist_add(&hist_ep[v->endpoint], us);
    requests++;
    ep_requests[v->endpoint]++;
    int good = (v->endpoint == EP_LIKE)     ? (v->status == 200 || v->status == 201)
               : (v->endpoint == EP_CREATE) ? v->status == 201
                                            : v->status == 200;
    if (!good) {
        errors++;
        errors_by_kind[4]++;
        ep_bad[v->endpoint]++;
    }
    /* Connection: close from the server means we must reconnect next time */
    if (strstr(v->hbuf, "Connection: close")) vu_close(v);
    v->state = V_IDLE;
    vu_next(v, !good);
}

static void vu_readable(struct vu *v) {
    char buf[65536];
    for (;;) {
        ssize_t r = recv(v->fd, buf, sizeof buf, 0);
        if (r < 0) {
            if (errno == EAGAIN) return;
            vu_fail(v, 3);
            return;
        }
        if (r == 0) {
            if (v->state == V_READING || v->state == V_SENDING) {
                vu_fail(v, 3);
            } else {
                vu_close(v); /* server closed an idle keep-alive; reconnect later */
            }
            return;
        }
        if (v->state != V_READING) continue; /* unexpected data on an idle connection: ignore */
        int done = vu_feed_bytes(v, buf, (size_t)r);
        if (done < 0) return;
        if (done) {
            vu_response_done(v);
            return;
        }
    }
}

static void vu_event(struct vu *v, uint32_t events) {
    if (v->fd < 0) return;
    if (v->state == V_CONNECTING) {
        int err = 0;
        socklen_t el = sizeof err;
        if (events & (EPOLLERR | EPOLLHUP)) {
            getsockopt(v->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            vu_fail(v, 2);
            return;
        }
        if (events & EPOLLOUT) {
            getsockopt(v->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) {
                vu_fail(v, 2);
                return;
            }
            vu_send(v);
        }
        return;
    }
    if (events & (EPOLLERR | EPOLLHUP)) {
        if (v->state == V_IDLE) {
            vu_close(v);
        } else {
            vu_fail(v, 3);
        }
        return;
    }
    if ((events & EPOLLOUT) && v->state == V_SENDING) vu_send(v);
    if (events & EPOLLIN) vu_readable(v);
}

/* ------------------------------------------------------------------ tokens */

static void load_tokens(void) {
    FILE *f = fopen(opt.tokens_path, "rb");
    if (!f) {
        perror(opt.tokens_path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    tokens_blob = malloc((size_t)sz + 1);
    if (!tokens_blob || fread(tokens_blob, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "cannot read tokens\n");
        exit(1);
    }
    fclose(f);
    tokens_blob[sz] = 0;
    uint32_t cap = 1024;
    tok_ptr = malloc(cap * sizeof *tok_ptr);
    user_ptr = malloc(cap * sizeof *user_ptr);
    tok_len = malloc(cap * sizeof *tok_len);
    user_len = malloc(cap * sizeof *user_len);
    const char *p = tokens_blob;
    for (;;) {
        const char *u = strstr(p, "\"username\":\"");
        if (!u) break;
        u += 12;
        const char *ue = strchr(u, '"');
        const char *t = strstr(ue, "\"token\":\"");
        if (!t) break;
        t += 9;
        const char *te = strchr(t, '"');
        if (ntokens == cap) {
            cap *= 2;
            tok_ptr = realloc(tok_ptr, cap * sizeof *tok_ptr);
            user_ptr = realloc(user_ptr, cap * sizeof *user_ptr);
            tok_len = realloc(tok_len, cap * sizeof *tok_len);
            user_len = realloc(user_len, cap * sizeof *user_len);
        }
        user_ptr[ntokens] = u;
        user_len[ntokens] = (uint32_t)(ue - u);
        tok_ptr[ntokens] = t;
        tok_len[ntokens] = (uint32_t)(te - t);
        ntokens++;
        p = te + 1;
    }
    if (!ntokens) {
        fprintf(stderr, "no tokens in %s\n", opt.tokens_path);
        exit(1);
    }
}

/* ------------------------------------------------------------------ reporting */

static const char *ep_name[EP_COUNT] = {"GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts"};

static void report(int final) {
    static uint64_t last_req, last_err;
    static int64_t last_t;
    int64_t t = now_ns();
    double dt = last_t ? (double)(t - last_t) / 1e9 : (double)(t - t_start) / 1e9;
    uint32_t open = 0, idle = 0, active = 0;
    for (uint32_t i = 0; i < opt.vus; i++) {
        if (vus[i].fd >= 0) open++;
        if (vus[i].state == V_IDLE) idle++;
        if (vus[i].state == V_READING || vus[i].state == V_SENDING) active++;
    }
    printf(
        "t=%4.0fs conns=%u active=%u idle=%u req=%llu (%.0f/s) err=%llu p50=%.1fms p95=%.1fms p99=%.1fms max=%.0fms\n",
        (double)(t - t_start) / 1e9, open, active, idle, (unsigned long long)requests,
        (double)(requests - last_req) / dt, (unsigned long long)errors, (double)hist_pct(&hist_all, 0.5) / 1e3,
        (double)hist_pct(&hist_all, 0.95) / 1e3, (double)hist_pct(&hist_all, 0.99) / 1e3,
        (double)hist_all.max_us / 1e3);
    fflush(stdout);
    last_req = requests;
    last_err = errors;
    (void)last_err;
    last_t = t;
    if (!final) return;
    printf("\n%-22s %10s %8s %9s %9s %9s %9s\n", "endpoint", "requests", "failed", "avg ms", "p95 ms", "p99 ms",
           "max ms");
    for (int e = 0; e < EP_COUNT; e++) {
        struct hist *h = &hist_ep[e];
        printf("%-22s %10llu %8llu %9.2f %9.1f %9.1f %9.0f\n", ep_name[e], (unsigned long long)h->count,
               (unsigned long long)ep_bad[e], h->count ? (double)h->sum_us / (double)h->count / 1e3 : 0.0,
               (double)hist_pct(h, 0.95) / 1e3, (double)hist_pct(h, 0.99) / 1e3, (double)h->max_us / 1e3);
    }
    double err_rate = requests ? (double)errors / (double)requests : 0.0;
    uint64_t p95 = hist_pct(&hist_all, 0.95), p99 = hist_pct(&hist_all, 0.99);
    printf("\ntotal requests=%llu errors=%llu (%.3f%%: send=%llu connect=%llu read=%llu status=%llu) connects=%llu "
           "p95=%.1fms p99=%.1fms\n",
           (unsigned long long)requests, (unsigned long long)errors, err_rate * 100,
           (unsigned long long)errors_by_kind[1], (unsigned long long)errors_by_kind[2],
           (unsigned long long)errors_by_kind[3], (unsigned long long)errors_by_kind[4], (unsigned long long)connects,
           (double)p95 / 1e3, (double)p99 / 1e3);
    int pass = p95 < 500000 && p99 < 1000000 && err_rate < 0.01;
    printf("thresholds (p95<500ms, p99<1s, errors<1%%): %s\n", pass ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ main */

static void usage(void) {
    fprintf(stderr, "usage: mload [--host H] [--port P] [--vus N] [--duration S] [--ramp S] [--think-scale F]\n"
                    "             [--src-ips N] [--tokens FILE] [--report-every S] [--max-errors N]\n");
    exit(2);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!val) usage();
        if (!strcmp(a, "--host"))
            opt.host = val;
        else if (!strcmp(a, "--port"))
            opt.port = atoi(val);
        else if (!strcmp(a, "--vus"))
            opt.vus = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(a, "--duration"))
            opt.duration = atof(val);
        else if (!strcmp(a, "--ramp"))
            opt.ramp = atof(val);
        else if (!strcmp(a, "--think-scale"))
            opt.think_scale = atof(val);
        else if (!strcmp(a, "--src-ips"))
            opt.src_ips = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(a, "--tokens"))
            opt.tokens_path = val;
        else if (!strcmp(a, "--report-every"))
            opt.report_every = atoi(val);
        else if (!strcmp(a, "--max-errors"))
            opt.max_errors = (uint32_t)strtoul(val, NULL, 10);
        else
            usage();
        i++;
    }
    if (!opt.vus) usage();
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < opt.vus + 64) {
        fprintf(stderr, "RLIMIT_NOFILE %lu is too low for %u VUs\n", (unsigned long)rl.rlim_cur, opt.vus);
        return 1;
    }
    load_tokens();
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)opt.port);
    if (inet_pton(AF_INET, opt.host, &dst.sin_addr) != 1) {
        fprintf(stderr, "--host must be an IPv4 address\n");
        return 1;
    }
    vus = calloc(opt.vus, sizeof *vus);
    timers.v = malloc((size_t)opt.vus * sizeof *timers.v);
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (!vus || !timers.v || epfd < 0) {
        perror("setup");
        return 1;
    }
    t_start = now_ns();
    for (uint32_t i = 0; i < opt.vus; i++) {
        struct vu *v = &vus[i];
        v->fd = -1;
        v->id = i;
        v->rng = 0x9e3779b9u ^ (i * 2654435761u) ^ (uint32_t)t_start;
        if (!v->rng) v->rng = 1;
        uint32_t ti = i % ntokens;
        v->tok = tok_ptr[ti];
        v->tok_len = tok_len[ti];
        v->user = user_ptr[ti];
        v->user_len = user_len[ti];
        v->state = V_IDLE;
        /* ramp: VU i starts at i * ramp / vus, then waits U(0,5) like k6's first iteration */
        double start = opt.ramp * (double)i / (double)opt.vus + between(v, 0, 5) * opt.think_scale;
        timer_add(i, t_start + (int64_t)(start * 1e9));
    }
    int64_t t_end = t_start + (int64_t)(opt.duration * 1e9);
    int64_t next_report = t_start + (int64_t)opt.report_every * 1000000000LL;
    struct epoll_event evs[1024];
    while (now_ns() < t_end) {
        int64_t now = now_ns();
        uint32_t vi;
        while (timer_peek(&vi) && vus[vi].deadline_ns <= now) {
            timer_pop();
            vu_wake(&vus[vi]);
        }
        int timeout_ms = 1000;
        if (timer_peek(&vi)) {
            int64_t d = vus[vi].deadline_ns - now_ns();
            timeout_ms = d <= 0 ? 0 : (int)(d / 1000000) + 1;
            if (timeout_ms > 1000) timeout_ms = 1000;
        }
        if (now >= next_report) {
            report(0);
            next_report += (int64_t)opt.report_every * 1000000000LL;
        }
        int n = epoll_wait(epfd, evs, 1024, timeout_ms);
        for (int i = 0; i < n; i++) vu_event(&vus[evs[i].data.u32], evs[i].events);
    }
    report(1);
    return 0;
}
