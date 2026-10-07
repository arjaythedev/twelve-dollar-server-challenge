/* Load generator for capacity tests. Each virtual user follows the loop of bench/load.js with its own
 * keep-alive connection: GET /feed, think 3-7 s, GET /posts/:id, think 3-8 s, like (15%), create a
 * post (2%), idle 5-15 s. Users start over the ramp, then hold. A user costs about 1 KB, so one machine
 * can run hundreds of thousands of them, which k6 cannot.
 *
 *   loadgen <host> <port> <users> <ramp_s> <hold_s> <threads> <tokens.json>
 *
 * THINK=<factor> scales every think time (THINK=0 measures maximum throughput). SRC_IPS=<ip,ip,...>
 * spreads connections over local source addresses: one address gives at most about 64,000 connections
 * to one server port. Prints rates and latency percentiles every 10 s, then TOTAL and HOLD lines. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static struct sockaddr_in addr;
static int NV, NT;
static double RAMP, HOLD;
static char **tok, **uname;
static int ntok;
static char hosthdr[64];
static double t0;

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

// latency histogram: buckets of 1us up to 1ms, then log-ish
#define NB 4096
static int bucket(double s) {
    double us = s * 1e6;
    if (us < 1000) return (int)us;
    int b = 1000 + (int)(log(us / 1000.0) / log(1.01));
    return b >= NB ? NB - 1 : b;
}
static double bucket_val(int b) { return b < 1000 ? b * 1e-6 : 1e-3 * pow(1.01, b - 1000 + 1); }

typedef struct { _Atomic uint64_t h[NB]; _Atomic uint64_t n, fail, connfail; } stats_t;
static stats_t win, tot, hold;  // window (reset on print), total, hold-phase

static void rec(double lat, int ok, double t) {
    int b = bucket(lat);
    atomic_fetch_add_explicit(&win.h[b], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&tot.h[b], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&win.n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&tot.n, 1, memory_order_relaxed);
    if (!ok) { atomic_fetch_add(&win.fail, 1); atomic_fetch_add(&tot.fail, 1); }
    if (t - t0 > RAMP) {
        atomic_fetch_add_explicit(&hold.h[b], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&hold.n, 1, memory_order_relaxed);
        if (!ok) atomic_fetch_add(&hold.fail, 1);
    }
}
static double pct(stats_t *s, double p) {
    uint64_t n = 0;
    for (int i = 0; i < NB; i++) n += s->h[i];
    if (!n) return 0;
    uint64_t want = (uint64_t)ceil(n * p), c = 0;
    for (int i = 0; i < NB; i++) { c += s->h[i]; if (c >= want) return bucket_val(i); }
    return 0;
}
static double maxv(stats_t *s) { for (int i = NB - 1; i >= 0; i--) if (s->h[i]) return bucket_val(i); return 0; }

enum { S_IDLE, S_CONNECTING, S_WAITRESP };
enum { A_FEED, A_POST, A_LIKE, A_CREATE, A_IDLE };

typedef struct {
    int id, fd, state, action, iter;
    double next;      // time of next action
    double sent;      // request send time
    int heapi;
    char *rb; int rlen;
    long post_id;
    unsigned seed;
    int will_like, will_create;
    char req[1024]; int reqlen;
} vu_t;

typedef struct { vu_t **h; int n, cap; int ep; } thr_t;

static double rnd(vu_t *v) { return rand_r(&v->seed) / (double)RAND_MAX; }
static double THINK = 1.0;
static double between(vu_t *v, double lo, double hi) { return THINK * (lo + rnd(v) * (hi - lo)); }

static void hswap(thr_t *t, int a, int b) { vu_t *x = t->h[a]; t->h[a] = t->h[b]; t->h[b] = x; t->h[a]->heapi = a; t->h[b]->heapi = b; }
static void hup(thr_t *t, int i) { while (i && t->h[(i - 1) / 2]->next > t->h[i]->next) { hswap(t, i, (i - 1) / 2); i = (i - 1) / 2; } }
static void hdown(thr_t *t, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->n && t->h[l]->next < t->h[m]->next) m = l;
        if (r < t->n && t->h[r]->next < t->h[m]->next) m = r;
        if (m == i) return;
        hswap(t, i, m);
        i = m;
    }
}
static void hpush(thr_t *t, vu_t *v) { v->heapi = t->n; t->h[t->n++] = v; hup(t, v->heapi); }
static vu_t *hpop(thr_t *t) { vu_t *v = t->h[0]; t->n--; if (t->n) { t->h[0] = t->h[t->n]; t->h[0]->heapi = 0; hdown(t, 0); } v->heapi = -1; return v; }

static void next_after(thr_t *t, vu_t *v, int ok);
static void schedule(thr_t *t, vu_t *v, double at, int action) { v->next = at; v->action = action; v->state = S_IDLE; hpush(t, v); }

static void build_req(vu_t *v) {
    int ti = v->id % ntok;
    switch (v->action) {
    case A_FEED: v->reqlen = sprintf(v->req, "GET /feed HTTP/1.1\r\nHost: %s\r\nUser-Agent: Grafana k6/2.3.0\r\n\r\n", hosthdr); break;
    case A_POST: v->reqlen = sprintf(v->req, "GET /posts/%ld HTTP/1.1\r\nHost: %s\r\nUser-Agent: Grafana k6/2.3.0\r\n\r\n", v->post_id, hosthdr); break;
    case A_LIKE: v->reqlen = sprintf(v->req, "POST /posts/%ld/like HTTP/1.1\r\nHost: %s\r\nUser-Agent: Grafana k6/2.3.0\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\n\r\n", v->post_id, hosthdr, tok[ti]); break;
    case A_CREATE: {
        char body[256];
        int bl = sprintf(body, "{\"body\":\"%s says hi at 2026-10-07T12:00:00.000Z (VU %d, iter %d)\"}", uname[ti], v->id + 1, v->iter);
        v->reqlen = sprintf(v->req, "POST /posts HTTP/1.1\r\nHost: %s\r\nUser-Agent: Grafana k6/2.3.0\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", hosthdr, tok[ti], bl, body);
        break; }
    }
}

static struct in_addr src_ips[64];
static int nsrc;

static void conn_open(thr_t *t, vu_t *v) {
    v->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(v->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (nsrc) {
        setsockopt(v->fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr = src_ips[v->id % nsrc]};
        bind(v->fd, (struct sockaddr *)&sa, sizeof sa);
    }
    connect(v->fd, (struct sockaddr *)&addr, sizeof addr);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = v};
    epoll_ctl(t->ep, EPOLL_CTL_ADD, v->fd, &ev);
    v->state = S_CONNECTING;
}

static void rb_free(vu_t *v) { free(v->rb); v->rb = NULL; v->rlen = 0; }
static void conn_drop(thr_t *t, vu_t *v) { if (v->fd >= 0) { epoll_ctl(t->ep, EPOLL_CTL_DEL, v->fd, NULL); close(v->fd); v->fd = -1; } rb_free(v); }

static void do_send(thr_t *t, vu_t *v) {
    build_req(v);
    v->sent = now();
    v->rlen = 0;
    if (!v->rb) v->rb = malloc(65537);
    ssize_t w = send(v->fd, v->req, v->reqlen, MSG_NOSIGNAL);
    v->state = S_WAITRESP;
    if (w != v->reqlen) { rec(now() - v->sent, 0, now()); conn_drop(t, v); next_after(t, v, 0); }
}

static void next_after(thr_t *t, vu_t *v, int ok) {
    double tn = now();
    if (v->rb && v->state == S_WAITRESP) rb_free(v);
    switch (v->action) {
    case A_FEED:
        if (!ok) { schedule(t, v, tn + between(v, 3, 7), A_FEED); return; }
        schedule(t, v, tn + between(v, 3, 7), A_POST); return;
    case A_POST:
        v->will_like = rnd(v) < 0.15; v->will_create = rnd(v) < 0.02;
        if (v->will_like) { schedule(t, v, tn + between(v, 3, 8), A_LIKE); return; }
        if (v->will_create) { schedule(t, v, tn + between(v, 3, 8), A_CREATE); return; }
        v->iter++; schedule(t, v, tn + between(v, 3, 8) + between(v, 5, 15), A_FEED); return;
    case A_LIKE:
        if (v->will_create) { schedule(t, v, tn, A_CREATE); return; }
        v->iter++; schedule(t, v, tn + between(v, 5, 15), A_FEED); return;
    case A_CREATE:
        v->iter++; schedule(t, v, tn + between(v, 5, 15), A_FEED); return;
    }
}

static void on_resp(thr_t *t, vu_t *v) {
    for (;;) {
        char scratch[512];
        ssize_t r = v->rb ? recv(v->fd, v->rb + v->rlen, 65536 - v->rlen, 0) : recv(v->fd, scratch, sizeof scratch, 0);
        if (r > 0) { v->rlen += r; continue; }
        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            if (v->state == S_WAITRESP) { rec(now() - v->sent, 0, now()); next_after(t, v, 0); }
            conn_drop(t, v); return;
        }
        break;
    }
    if (v->state != S_WAITRESP) return;
    v->rb[v->rlen] = 0;
    char *he = strstr(v->rb, "\r\n\r\n");
    if (!he) return;
    char *cl = strcasestr(v->rb, "content-length:");
    int clen = cl && cl < he ? atoi(cl + 15) : 0;
    if (v->rlen < (he + 4 - v->rb) + clen) return;
    double tn = now();
    int status = atoi(v->rb + 9);
    int ok = 0;
    switch (v->action) {
    case A_FEED: ok = status == 200; if (ok) {
        long ids[20]; int n = 0; char *p = he + 4;
        while (n < 20 && (p = strstr(p, "{\"id\":"))) { ids[n++] = atol(p + 6); p += 6; }
        if (n) v->post_id = ids[rand_r(&v->seed) % n]; else ok = 0; } break;
    case A_POST: ok = status == 200; break;
    case A_LIKE: ok = status == 200 || status == 201; break;
    case A_CREATE: ok = status == 201; break;
    }
    if (!ok && atomic_load(&tot.fail) < 5) fprintf(stderr, "bad response action=%d: %.200s\n", v->action, v->rb);
    rec(tn - v->sent, ok, tn);
    v->rlen = 0;
    next_after(t, v, ok);
}

static void *thread_main(void *arg) {
    thr_t *t = arg;
    struct epoll_event evs[512];
    for (;;) {
        double tn = now();
        while (t->n && t->h[0]->next <= tn) {
            vu_t *v = hpop(t);
            if (v->fd < 0) { conn_open(t, v); continue; }
            do_send(t, v);
        }
        int timeout = t->n ? (int)ceil((t->h[0]->next - now()) * 1000) : 100;
        if (timeout < 0) timeout = 0;
        if (timeout > 100) timeout = 100;
        int n = epoll_wait(t->ep, evs, 512, timeout);
        for (int i = 0; i < n; i++) {
            vu_t *v = evs[i].data.ptr;
            if (v->fd < 0) continue;
            if (v->state == S_CONNECTING) {
                if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
                    atomic_fetch_add(&tot.connfail, 1); rec(0, 0, now()); conn_drop(t, v);
                    schedule(t, v, now() + 1, v->action); continue;
                }
                if (evs[i].events & EPOLLOUT) { do_send(t, v); continue; }
            }
            if (evs[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) on_resp(t, v);
        }
    }
    return NULL;
}

static char *slurp(const char *p) { FILE *f = fopen(p, "r"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); char *b = malloc(n + 1); if (fread(b, 1, n, f) != (size_t)n) { perror(p); exit(1); } b[n] = 0; fclose(f); return b; }

int main(int argc, char **argv) {
    if (argc < 8) { fprintf(stderr, "usage\n"); return 1; }
    struct addrinfo *ai, hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    if (getaddrinfo(argv[1], argv[2], &hints, &ai)) { perror("gai"); return 1; }
    addr = *(struct sockaddr_in *)ai->ai_addr;
    snprintf(hosthdr, sizeof hosthdr, "%s:%s", argv[1], argv[2]);
    if (getenv("THINK")) THINK = atof(getenv("THINK"));
    if (getenv("SRC_IPS")) { char *ips = strdup(getenv("SRC_IPS")); for (char *tok = strtok(ips, ","); tok && nsrc < 64; tok = strtok(NULL, ",")) inet_pton(AF_INET, tok, &src_ips[nsrc++]); }
    NV = atoi(argv[3]); RAMP = atof(argv[4]); HOLD = atof(argv[5]); NT = atoi(argv[6]);
    char *js = slurp(argv[7]);
    tok = malloc(sizeof(char *) * 30000); uname = malloc(sizeof(char *) * 30000);
    for (char *p = js; (p = strstr(p, "\"username\":\"")); ) {
        p += 12; char *e = strchr(p, '"'); *e = 0; uname[ntok] = p; p = e + 1;
        p = strstr(p, "\"token\":\"") + 9; e = strchr(p, '"'); *e = 0; tok[ntok++] = p; p = e + 1;
    }
    fprintf(stderr, "%d tokens, %d vus, ramp %.0fs hold %.0fs, %d threads\n", ntok, NV, RAMP, HOLD, NT);
    thr_t *th = calloc(NT, sizeof *th);
    for (int i = 0; i < NT; i++) { th[i].cap = NV / NT + 2; th[i].h = calloc(th[i].cap, sizeof(vu_t *)); th[i].ep = epoll_create1(0); }
    t0 = now();
    for (int i = 0; i < NV; i++) {
        vu_t *v = calloc(1, sizeof *v);
        v->id = i; v->fd = -1; v->seed = i * 2654435761u + 1; v->rb = NULL;
        thr_t *t = &th[i % NT];
        // ramping-vus: VU i becomes active at i/NV*RAMP; first iteration sleeps U(0,5)
        double start = t0 + RAMP * i / NV;
        v->next = start + between(v, 0, 5);
        v->action = A_FEED;
        hpush(t, v);
    }
    // connections are opened lazily at first action (k6 also connects on first request)
    pthread_t pt;
    for (int i = 0; i < NT; i++) pthread_create(&pt, NULL, thread_main, &th[i]);
    double end = t0 + RAMP + HOLD, last = t0;
    while (now() < end) {
        sleep(10);
        double tn = now();
        uint64_t n = atomic_exchange(&win.n, 0), f = atomic_exchange(&win.fail, 0);
        printf("t=%4.0fs rps=%7.0f p50=%7.2fms p95=%7.2fms p99=%7.2fms max=%7.1fms fail=%lu\n", tn - t0, n / (tn - last),
               pct(&win, .5) * 1e3, pct(&win, .95) * 1e3, pct(&win, .99) * 1e3, maxv(&win) * 1e3, f);
        fflush(stdout);
        for (int i = 0; i < NB; i++) atomic_store(&win.h[i], 0);
        last = tn;
    }
    printf("TOTAL n=%lu fail=%lu (%.3f%%) connfail=%lu p50=%.2fms p95=%.2fms p99=%.2fms max=%.1fms\n", (unsigned long)tot.n, (unsigned long)tot.fail,
           100.0 * tot.fail / (tot.n ? tot.n : 1), (unsigned long)tot.connfail, pct(&tot, .5) * 1e3, pct(&tot, .95) * 1e3, pct(&tot, .99) * 1e3, maxv(&tot) * 1e3);
    printf("HOLD  n=%lu fail=%lu (%.3f%%) p50=%.2fms p95=%.2fms p99=%.2fms max=%.1fms\n", (unsigned long)hold.n, (unsigned long)hold.fail,
           100.0 * hold.fail / (hold.n ? hold.n : 1), pct(&hold, .5) * 1e3, pct(&hold, .95) * 1e3, pct(&hold, .99) * 1e3, maxv(&hold) * 1e3);
    return 0;
}
