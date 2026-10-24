/* io/loop.c (sha256 5f115cb12bea), developed separately for this entry. */
/*
 * loop.c - see loop.h. Two backends share one core:
 *
 *   core      connection table (32 B/conn), size-classed buffer pool, batch building,
 *             partial-input stitching, staging + group commit, backpressure, timeouts,
 *             memory-pressure policy.
 *   io_uring  raw syscalls (no liburing). N rings (each table <= RLIMIT_NOFILE, so 65,535
 *             slots under the challenge's limit), all driven by one thread: ring 0 waits,
 *             rings 1..N-1 signal ring 0 through a registered eventfd that ring 0 polls
 *             with a multishot POLL_ADD. Multishot accept into direct descriptors,
 *             multishot recv from a provided-buffer ring, sends with MSG_WAITALL.
 *   kqueue    macOS dev backend, same semantics with read()/write().
 */
#define _GNU_SOURCE
#include "loop.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#define BE_URING 1
#include <linux/io_uring.h>
#include <sys/epoll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#else
#define BE_KQUEUE 1
#include <sys/event.h>
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ------------------------------------------------------------------ buffers */

typedef struct buf {
    struct buf *next;
    uint32_t len;           /* bytes in d[] */
    uint32_t cap;
    uint32_t off;           /* send progress */
    uint32_t base;          /* out buffers: absolute stream position of d[0] */
    uint32_t conn, gen;     /* in-flight sends: owner */
    uint16_t refs;          /* pending SEND_ZC notifications */
    uint8_t cls, done, zc;
    char d[];
} buf_t;

#define NCLS 9              /* 256 B .. 64 KiB */
#define POOL_KEEP 256

/* ------------------------------------------------------------------ conns */

enum {
    F_OPEN = 1, F_INB = 2, F_SENDING = 4, F_EOF = 8, F_CLOSE_AFTER = 16, F_PAUSED = 32,
    F_READY = 64, F_DEAD = 128, F_TOUCHED = 256, F_RECV = 512, F_RST = 1024,
    F_RECVING = 2048,               /* poll mode: a one-shot recv is in flight */
    F_AGAIN = 4096,                 /* poll mode: more readiness (or ENOBUFS) seen while/after receiving */
    F_LINGER = 8192,                /* lingering close: our FIN is out (SHUT_WR), draining until the peer's EOF */
    F_MORE = 16384,                 /* input arrived after the close decision (a pipelining peer): linger */
};

typedef struct {
    uint32_t gen;
    uint16_t fl;
    uint16_t tick;          /* last activity, coarse seconds (wrapping) */
    uint32_t bidx;          /* 1-based index into the batch while F_INB */
    uint32_t rel;           /* absolute stream position released for sending */
    buf_t *in;              /* unconsumed input (partial requests), NULL when idle */
    buf_t *out;             /* written, not yet handed to the kernel; NULL when idle */
} conn_t;
_Static_assert(sizeof(conn_t) == 32, "conn_t must stay 32 bytes");

typedef struct { uint32_t conn, gen; } cref;
typedef struct { uint64_t seq; uint32_t conn, gen, end; } staged_t;
typedef struct { uint64_t t; uint16_t ring; uint16_t bid; uint8_t src; } bmeta;   /* src: 0 conn->in, 1 pbuf, 2 arena; t: rx time (realtime ns) */

typedef struct { void *p; uint32_t n, cap; size_t sz; } vec_t;
static void *vpush(vec_t *v)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 256;
        v->p = realloc(v->p, v->cap * v->sz);
        if (!v->p) abort();
    }
    return (char *)v->p + (size_t)v->n++ * v->sz;
}

/* ------------------------------------------------------------------ io_uring ring */

#ifdef BE_URING
enum { OP_ACCEPT = 1, OP_RECV = 2, OP_SEND = 3, OP_EVFD = 4, OP_IGN = 5, OP_POLL = 6, OP_RECV1 = 7 };

typedef struct ring {
    int fd, ridx;
    unsigned *sq_head, *sq_tail, *sq_flags, sq_mask, sq_entries, sq_ltail, to_submit;
    struct io_uring_sqe *sqes;
    unsigned *cq_head, *cq_tail, *cq_flags, cq_mask;
    struct io_uring_cqe *cqes;
    void *sq_map, *cq_map; size_t sq_map_sz, cq_map_sz, sqe_sz;
    struct io_uring_buf_ring *br;
    size_t br_sz;
    char *bufmem;
    uint16_t br_tail, br_dirty;
    int evfd;
    uint32_t open, cap;
    uint32_t singles;               /* single-shot accepts in flight */
    uint8_t accepting, reap, legacy;
    uint8_t recv1;                  /* poll mode: one-shot recvs queued during this reap */
} ring_t;
#endif

struct loop {
    loop_cfg cfg;
    loop_stats st;
    int lfd;
    volatile int stop;
    conn_t *tab;
    size_t tab_n, tab_bytes;
    uint32_t hi;                    /* highest conn id ever used + 1 (sweep bound) */
    buf_t *pool[NCLS];
    uint32_t pool_n[NCLS];

    /* batch */
    vec_t vin, vmeta;               /* loop_in[], bmeta[] */
    vec_t touched, staged, ready, closing, rearm;
    uint32_t staged_head;
    uint64_t seq;
    int in_batch;

    uint32_t now_s;
    uint64_t now_ns, next_house_ns;
    uint32_t sweep_pos, shed_pos;
    int mem_state;                  /* 0 ok, 1 not accepting, 2 shedding */
    uint64_t real_ns;               /* CLOCK_REALTIME at this pass (rx timestamps are realtime) */
    uint32_t rng;
    int rst_ok;                     /* SO_LINGER via io_uring setsockopt works */
    long tcp_mem_pressure;          /* tcp_mem[1] in pages, 0 if unknown */
    uint32_t cap0;                  /* cap derived at startup */
    uint32_t cap_shrunk_s;          /* when the cap last shrank (now_s), 0 never */
    long avail_last;
    vec_t fop_done;                 /* readiness backends: completed file ops (fop_rec*) awaiting callbacks */
    int engaged, cap_parked, tick4;
    float f_ceiling;
    float h_ewma;                   /* shed_headroom: available memory, EWMA ~1 s (MiB) */
    uint32_t slots_cap;             /* descriptor slots we can hold */
    uint64_t cpu_us_last, wall_ns_last;   /* cpu_us_last holds /proc/stat total ticks in 1-CPU mode */
    long idle_last;
    uint64_t closed_last;
    int busy_src_set, busy_cpu;     /* busy_source(): -1 box, N one CPU, -3 cgroup quota, -2 rusage */
    float busy_quota;

#ifdef BE_URING
    ring_t *rings;
    unsigned nrings, slot_bits;     /* nrings: maximum; rings are created on demand */
    unsigned nlive, ring_per;       /* rings created so far, slots per ring */
    int acc_ring;                   /* ring holding the multishot accept, -1 none */
    int want_accept;
    int busy, more, ring_wait_poll_active;
    int pollmode;                   /* idle conns hold a multishot POLL_ADD; data via one-shot recv */
    int no_fdinstall;               /* IORING_OP_FIXED_FD_INSTALL unavailable: SHUTDOWN via io-wq */
    struct msghdr rx_msg;           /* template for multishot RECVMSG (rx_timestamps) */
    int epoll;                      /* 1: the epoll fallback backend is in use */
    int ep, ep_listen_on, ep_emfile;
#else
    int kq;
    vec_t kchg;                     /* struct kevent[] */
#endif
    /* readiness backends (epoll, kqueue): conn id = fd */
    buf_t **kinfl;                  /* in-flight (partially written) send per fd */
    char *arena; size_t arena_cap, arena_used;
};

static inline conn_t *C(loop_t *L, uint32_t id) { return &L->tab[id]; }

/* a submitted file op: user_data = pointer | 8 | OP_SEND (16-byte aligned; send buffers never have bit 3) */
typedef struct fop_rec { loop_file_cb cb; void *ud; int res; } fop_rec;

static uint64_t mono_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ buffer pool */

static buf_t *buf_get(loop_t *L, size_t need)
{
    unsigned k = 0;
    while (k < NCLS && (256u << k) < need) k++;
    buf_t *b;
    if (k < NCLS) {
        if ((b = L->pool[k])) { L->pool[k] = b->next; L->pool_n[k]--; }
        else {
            b = malloc(sizeof *b + (256u << k));
            if (!b) abort();
            b->cap = 256u << k;
        }
        b->cls = (uint8_t)k;
    } else {
        b = malloc(sizeof *b + need);
        if (!b) abort();
        b->cap = (uint32_t)need;
        b->cls = 255;
    }
    b->next = NULL; b->len = b->off = b->base = 0; b->refs = 0; b->done = 0; b->zc = 0;
    return b;
}

static void buf_put(loop_t *L, buf_t *b)
{
    if (!b) return;
    if (b->cls < NCLS && L->pool_n[b->cls] < POOL_KEEP) {
        b->next = L->pool[b->cls]; L->pool[b->cls] = b; L->pool_n[b->cls]++;
    } else free(b);
}

static buf_t *buf_append(loop_t *L, buf_t *b, const void *p, size_t n)
{
    /* input buffers keep LOOP_SLACK spare bytes after the data (parser over-read contract) */
    if (!b) { b = buf_get(L, n + LOOP_SLACK < 512 ? 512 : n + LOOP_SLACK); }
    else if (b->cap - b->len < n + LOOP_SLACK) {
        size_t want = (size_t)b->len + n + LOOP_SLACK;
        if (want < (size_t)b->cap * 2) want = (size_t)b->cap * 2;
        buf_t *nb = buf_get(L, want);
        memcpy(nb->d, b->d, b->len);
        nb->len = b->len; nb->base = b->base;
        buf_put(L, b);
        b = nb;
    }
    memcpy(b->d + b->len, p, n);
    b->len += (uint32_t)n;
    return b;
}

/* ------------------------------------------------------------------ backend decls */

static void return_pbuf(loop_t *L, uint16_t ring, uint16_t bid);

static int  be_init(loop_t *L);
static void be_free(loop_t *L);
static void be_send(loop_t *L, uint32_t id, conn_t *c, buf_t *sb);
static void be_close(loop_t *L, uint32_t id, conn_t *c);
static void be_pause(loop_t *L, uint32_t id, conn_t *c);
static void be_resume(loop_t *L, uint32_t id, conn_t *c);
static int  be_wait_and_reap(loop_t *L);
static void be_after_batch(loop_t *L);
static void be_accept_ctl(loop_t *L, int on);
static void be_shutdown_wr(loop_t *L, uint32_t id, conn_t *c);

/* ------------------------------------------------------------------ core: close / send */

static void conn_close_now(loop_t *L, uint32_t id)
{
    conn_t *c = C(L, id);
    if (!(c->fl & F_OPEN)) return;
    be_close(L, id, c);
    buf_put(L, c->in); buf_put(L, c->out);
    c->in = c->out = NULL;
    c->gen++;
    c->fl = 0;
    L->st.open--; L->st.closed++;
}

/* Close at a safe point (never while the batch array may reference the conn). */
#ifdef LOOP_DEBUG
#define conn_kill(L, id) (fprintf(stderr, "kill %u at line %d\n", (unsigned)(id), __LINE__), conn_kill_(L, id))
static void conn_kill_(loop_t *L, uint32_t id)
#else
static void conn_kill(loop_t *L, uint32_t id)
#endif
{
    conn_t *c = C(L, id);
    if (!(c->fl & F_OPEN) || (c->fl & F_DEAD)) return;
    c->fl |= F_DEAD;
    cref *r = vpush(&L->closing); r->conn = id; r->gen = c->gen;
}

static inline uint32_t out_unsent(conn_t *c) { return c->out ? c->out->len : 0; }

static void maybe_finish(loop_t *L, uint32_t id, conn_t *c)
{
    /* Close-after-flush: nothing in flight, nothing written-but-unsent. */
    if (!(c->fl & (F_CLOSE_AFTER | F_EOF)) || (c->fl & F_SENDING) || out_unsent(c)) return;
    /* Lingering close: send our FIN (we stay the active closer, so TIME_WAIT is ours) but keep the socket
     * until the peer's EOF or linger_s, discarding input. The socket never becomes an orphan (counted
     * against tcp_max_orphans, reset when over it), and requests the peer pipelined before seeing our
     * Connection: close are read and dropped instead of being answered with RST. */
    /* linger_close 0 (auto, nginx's `lingering_close on`): only when the peer may still be sending, i.e. input
     * arrived after the close decision or is unconsumed; a peer that waits for our response (mload, k6) gets a
     * plain server-first close, which saves 4-8 us per reconnect (explore/reconnect). 1: always. -1: never. */
    int may_send = (c->fl & F_MORE) || (c->in && c->in->len);
    if (L->cfg.linger_close >= 0 && (L->cfg.linger_close > 0 || may_send) && !(c->fl & (F_EOF | F_LINGER | F_DEAD))) {
        c->fl |= F_LINGER;
        c->tick = (uint16_t)L->now_s;
        be_shutdown_wr(L, id, c);
        return;
    }
    if (!(c->fl & F_LINGER) || (c->fl & F_EOF)) conn_kill(L, id);
}

static void kick(loop_t *L, uint32_t id, conn_t *c)
{
    if (c->fl & (F_SENDING | F_DEAD)) return;
    buf_t *o = c->out;
    if (!o) { maybe_finish(L, id, c); return; }
    uint32_t n = c->rel - o->base;
    if (n > o->len) n = o->len;            /* never: rel is bounded by written data */
    if (!n) return;
    if (n < o->len) {                       /* unreleased tail (deferred commit) stays */
        buf_t *t = buf_get(L, o->len - n);
        memcpy(t->d, o->d + n, o->len - n);
        t->len = o->len - n; t->base = o->base + n;
        o->len = n;
        c->out = t;
    } else c->out = NULL;
    o->off = 0; o->conn = id; o->gen = c->gen;
    c->fl |= F_SENDING;
    L->st.sends++;
    be_send(L, id, c, o);
}

/* Called by a backend when an in-flight send buffer is fully written (or failed). */
static void send_done(loop_t *L, buf_t *sb, int err)
{
    uint32_t id = sb->conn;
    conn_t *c = C(L, id);
    int live = (c->fl & F_OPEN) && c->gen == sb->gen;
    sb->done = 1;
    if (!sb->refs) buf_put(L, sb);
    if (!live) return;
    c->fl &= ~F_SENDING;
    if (err) { conn_kill(L, id); return; }
    c->tick = (uint16_t)L->now_s;
    kick(L, id, c);
    if ((c->fl & F_PAUSED) && out_unsent(c) <= L->cfg.out_hiwat / 2) {
        c->fl &= ~F_PAUSED;
        be_resume(L, id, c);
    }
    if (c->in && c->in->len && !(c->fl & (F_READY | F_INB | F_PAUSED)) && out_unsent(c) < L->cfg.out_hiwat) {
        c->fl |= F_READY;
        cref *r = vpush(&L->ready); r->conn = id; r->gen = c->gen;
    }
}

/* ------------------------------------------------------------------ core: incoming data */

static void conn_open(loop_t *L, uint32_t id)
{
    conn_t *c = C(L, id);
    if (c->fl & F_OPEN) {                   /* defensive: kernel reused a slot we think is live */
        buf_put(L, c->in); buf_put(L, c->out);
        c->in = c->out = NULL; L->st.open--;
    }
    c->gen++;
    c->fl = F_OPEN;
    c->tick = (uint16_t)L->now_s;
    c->bidx = 0; c->rel = 0; c->in = c->out = NULL;
    if (id + 1 > L->hi) L->hi = id + 1;
    L->st.open++; L->st.accepted++;
}

/* New bytes for conn id. src 1 = provided buffer (ring, bid) that must be returned by
 * the caller after the batch if we keep pointing at it; returns 1 if we kept it. */
static int conn_data(loop_t *L, uint32_t id, const char *p, uint32_t n, uint8_t src, uint16_t ring, uint16_t bid, uint64_t ts)
{
    conn_t *c = C(L, id);
    c->tick = (uint16_t)L->now_s;
    L->st.recvs++;
    if (c->fl & (F_CLOSE_AFTER | F_DEAD)) { c->fl |= F_MORE; return 0; }   /* closing: drop input (and linger) */
    if (out_unsent(c) >= L->cfg.out_hiwat || (c->fl & F_PAUSED)) {
        c->in = buf_append(L, c->in, p, n);                   /* backpressure: stash */
        if (c->in->len > L->cfg.max_in && !(c->fl & F_PAUSED)) {
            c->fl |= F_PAUSED; L->st.refused_pause++;
            be_pause(L, id, c);
        }
        return 0;
    }
    if (c->fl & F_INB) {
        loop_in *e = (loop_in *)L->vin.p + (c->bidx - 1);
        bmeta *m = (bmeta *)L->vmeta.p + (c->bidx - 1);
        if (ts && ts < m->t) m->t = ts;
        if (m->src) {                                         /* second chunk this batch: stitch */
            c->in = buf_append(L, c->in, e->data, e->len);
            if (m->src == 1) return_pbuf(L, m->ring, m->bid);
            m->src = 0;
        }
        c->in = buf_append(L, c->in, p, n);
        return 0;
    }
    c->fl |= F_INB;
    loop_in *e = vpush(&L->vin);
    bmeta *m = vpush(&L->vmeta);
    c->bidx = L->vin.n;
    e->conn = id; e->consumed = 0;
    m->t = ts ? ts : L->real_ns;
    if (c->in && c->in->len) {
        c->in = buf_append(L, c->in, p, n);
        m->src = 0;
        return 0;
    }
    e->data = p; e->len = n;
    m->src = src; m->ring = ring; m->bid = bid;
    return src == 1;
}

static void conn_eof(loop_t *L, uint32_t id)
{
    conn_t *c = C(L, id);
    if (!(c->fl & F_OPEN) || (c->fl & F_DEAD)) return;
    c->fl |= F_EOF;
    if (!(c->fl & F_INB)) maybe_finish(L, id, c);
}

/* ------------------------------------------------------------------ core: batch */

static void release_staged(loop_t *L, uint64_t seq)
{
    staged_t *s = L->staged.p;
    while (L->staged_head < L->staged.n && s[L->staged_head].seq <= seq) {
        staged_t *e = &s[L->staged_head++];
        conn_t *c = C(L, e->conn);
        if (!(c->fl & F_OPEN) || c->gen != e->gen) continue;
        if ((int32_t)(e->end - c->rel) > 0) c->rel = e->end;
        kick(L, e->conn, c);
    }
    if (L->staged_head == L->staged.n) L->staged_head = L->staged.n = 0;
}

void loop_commit(loop_t *L, uint64_t seq) { release_staged(L, seq); }

/* Re-present this conn's unconsumed input on the next pass without new bytes (e.g. a request the app
 * left unconsumed while a write gate was closed). */
void loop_represent(loop_t *L, loop_conn id)
{
    conn_t *c = C(L, id);
    if (!(c->fl & F_OPEN) || (c->fl & (F_READY | F_INB | F_DEAD)) || !c->in || !c->in->len) return;
    c->fl |= F_READY;
    cref *r = vpush(&L->ready); r->conn = id; r->gen = c->gen;
}

static void run_batch(loop_t *L)
{
    /* conns with stashed input that are ready to be re-presented */
    cref *rd = L->ready.p;
    for (uint32_t i = 0; i < L->ready.n; i++) {
        conn_t *c = C(L, rd[i].conn);
        if (!(c->fl & F_OPEN) || c->gen != rd[i].gen) continue;
        c->fl &= ~F_READY;
        if ((c->fl & (F_INB | F_DEAD | F_CLOSE_AFTER)) || !c->in || !c->in->len) continue;
        if (out_unsent(c) >= L->cfg.out_hiwat) continue;
        c->fl |= F_INB;
        loop_in *e = vpush(&L->vin); bmeta *m = vpush(&L->vmeta);
        c->bidx = L->vin.n; e->conn = rd[i].conn; e->consumed = 0; m->src = 0; m->t = L->real_ns;
    }
    L->ready.n = 0;

    uint32_t n = L->vin.n;
    if (!n) return;
    loop_in *v = L->vin.p;
    bmeta *m = L->vmeta.p;
    /* drop dead conns; point stitched entries at conn->in */
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        conn_t *c = C(L, v[i].conn);
        if (c->fl & F_DEAD) {
            c->fl &= ~F_INB;
            if (m[i].src == 1) return_pbuf(L, m[i].ring, m[i].bid);
            continue;
        }
        if (m[i].src == 0) { v[i].data = c->in->d; v[i].len = c->in->len; }
        v[i].qdelay_us = L->real_ns > m[i].t ? (uint32_t)((L->real_ns - m[i].t) / 1000) : 0;
        if (k != i) { v[k] = v[i]; m[k] = m[i]; }
        c->bidx = k + 1;
        k++;
    }
    n = k;
    L->st.batches++; L->st.batch_items += n;

    L->in_batch = 1;
    int r = n ? L->cfg.on_batch(L, v, (int)n, L->cfg.ud) : LOOP_COMMITTED;
    L->in_batch = 0;

    /* consumption: keep leftovers, return provided buffers */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = v[i].conn;
        conn_t *c = C(L, id);
        c->fl &= ~F_INB;
        uint32_t used = v[i].consumed < v[i].len ? v[i].consumed : v[i].len;
        uint32_t rem = v[i].len - used;
        if (m[i].src) {
            if (rem && !(c->fl & (F_CLOSE_AFTER | F_EOF))) c->in = buf_append(L, c->in, v[i].data + used, rem);
            if (m[i].src == 1) return_pbuf(L, m[i].ring, m[i].bid);
        } else if (c->in) {
            if (rem && !(c->fl & (F_CLOSE_AFTER | F_EOF))) {
                if (used) memmove(c->in->d, c->in->d + used, rem);
                c->in->len = rem;
            } else { buf_put(L, c->in); c->in = NULL; }
        }
        if (c->in && c->in->len > L->cfg.max_in) {
            /* unconsumed input over the limit: the app may be holding a request (write gate) with
             * pipelined ones behind it, so pause reading; only a runaway backlog is fatal */
            if (c->in->len > 4 * L->cfg.max_in) { conn_kill(L, id); continue; }
            if (!(c->fl & F_PAUSED)) { c->fl |= F_PAUSED; L->st.refused_pause++; be_pause(L, id, c); }
        } else if ((c->fl & F_PAUSED) && (!c->in || c->in->len <= L->cfg.max_in / 2) && out_unsent(c) <= L->cfg.out_hiwat / 2) {
            c->fl &= ~F_PAUSED;                               /* backlog consumed: read again */
            be_resume(L, id, c);
        }
        if (c->in && used && !(c->fl & F_READY)) {          /* app stopped early: re-present */
            c->fl |= F_READY;
            cref *q = vpush(&L->ready); q->conn = id; q->gen = c->gen;
        }
    }

    /* staging / group commit */
    cref *t = L->touched.p;
    for (uint32_t i = 0; i < L->touched.n; i++) {
        conn_t *c = C(L, t[i].conn);
        c->fl &= ~F_TOUCHED;
        if (!(c->fl & F_OPEN) || c->gen != t[i].gen || !c->out) continue;
        staged_t *s = vpush(&L->staged);
        s->seq = L->seq; s->conn = t[i].conn; s->gen = c->gen; s->end = c->out->base + c->out->len;
    }
    L->touched.n = 0;
    uint64_t seq = L->seq++;
    if (r == LOOP_COMMITTED) release_staged(L, seq);

    for (uint32_t i = 0; i < n; i++) {
        conn_t *c = C(L, v[i].conn);
        if (c->fl & (F_EOF | F_CLOSE_AFTER)) maybe_finish(L, v[i].conn, c);
    }
    L->vin.n = L->vmeta.n = 0;
}

static void process_closing(loop_t *L)
{
    cref *r = L->closing.p;
    for (uint32_t i = 0; i < L->closing.n; i++) {
        conn_t *c = C(L, r[i].conn);
        if ((c->fl & F_OPEN) && c->gen == r[i].gen) conn_close_now(L, r[i].conn);
    }
    L->closing.n = 0;
}

/* ------------------------------------------------------------------ core: app API */

uint64_t loop_batch_seq(loop_t *L) { return L->seq; }

static void touch(loop_t *L, uint32_t id, conn_t *c)
{
    if (c->fl & F_TOUCHED) return;
    c->fl |= F_TOUCHED;
    cref *r = vpush(&L->touched); r->conn = id; r->gen = c->gen;
}

char *loop_wbuf(loop_t *L, loop_conn id, size_t need)
{
    conn_t *c = C(L, id);
    buf_t *o = c->out;
    if (!o) {
        o = buf_get(L, need < 2048 ? 2048 : need);
        o->base = c->rel;                     /* out == NULL means everything written was released */
        c->out = o;
    } else if (o->cap - o->len < need) {
        size_t want = (size_t)o->len + need;
        if (want < (size_t)o->cap * 2) want = (size_t)o->cap * 2;
        buf_t *nb = buf_get(L, want);
        memcpy(nb->d, o->d, o->len);
        nb->len = o->len; nb->base = o->base;
        buf_put(L, o);
        c->out = o = nb;
    }
    return o->d + o->len;
}

void loop_wcommit(loop_t *L, loop_conn id, size_t n)
{
    conn_t *c = C(L, id);
    c->out->len += (uint32_t)n;
    touch(L, id, c);
}

int loop_write(loop_t *L, loop_conn id, const void *p, size_t n)
{
    conn_t *c = C(L, id);
    if (!(c->fl & F_OPEN) || (c->fl & F_DEAD)) return -1;
    memcpy(loop_wbuf(L, id, n), p, n);
    loop_wcommit(L, id, n);
    return 0;
}

void loop_close(loop_t *L, loop_conn id)
{
    conn_t *c = C(L, id);
    if (c->fl & F_OPEN) c->fl |= F_CLOSE_AFTER;
    if (!L->in_batch) maybe_finish(L, id, c);
}

static int conn_idle(conn_t *c)
{
    return (c->fl & ~F_RECV) == F_OPEN && !c->in && !c->out;
}

unsigned loop_shed_idle(loop_t *L, unsigned n)
{
    unsigned done = 0;
    uint32_t hi = L->hi ? L->hi : 1, scanned = 0;
    while (done < n && scanned < hi) {
        uint32_t id = L->shed_pos++ % hi;
        scanned++;
        conn_t *c = C(L, id);
        if (conn_idle(c) && (uint16_t)((uint16_t)L->now_s - c->tick) >= L->cfg.shed_min_idle_s) {
            if (!L->cfg.shed_fin) c->fl |= F_RST;
            conn_kill(L, id); done++;
        }
    }
    L->st.shed += done;
    return done;
}

const loop_stats *loop_get_stats(loop_t *L) { return &L->st; }
uint64_t loop_now_ns(loop_t *L) { return L->now_ns; }

int loop_close_hint(loop_t *L, loop_conn id)
{
    (void)id;
    float f = L->st.close_frac;
    if (f <= 0) return 0;
    L->rng ^= L->rng << 13; L->rng ^= L->rng >> 17; L->rng ^= L->rng << 5;
    if ((float)(L->rng >> 8) < f * 16777216.0f) { L->st.hint_closes++; return 1; }
    return 0;
}
void loop_stop(loop_t *L) { L->stop = 1; }

/* ------------------------------------------------------------------ core: housekeeping */

#ifdef __linux__
static long read_num(const char *path)
{
    char b[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    if (b[0] < '0' || b[0] > '9') return -1;     /* "max" */
    return atol(b);
}
#endif

/* Available memory in MiB: min(host MemAvailable, cgroup headroom incl. reclaimable file pages). */
static long avail_mb(void)
{
    long best = -1;
#ifdef __linux__
    char b[4096];
    int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0) {
            b[n] = 0;
            /* MemAvailable counts reclaimable slab as available, but socket inodes/dentries (~1 KB per
             * connection: exp19, exp42) sit there and are pinned while the socket lives. Subtract the growth
             * of SReclaimable since the first reading: that's the pinned socket part; the startup baseline
             * (~110 MB of genuinely reclaimable dentries) stays available (conn-policy). */
            static long sr0 = -1;
            char *p = strstr(b, "MemAvailable:"), *q = strstr(b, "SReclaimable:");
            long sr = q ? atol(q + 13) : 0;
            if (sr0 < 0) sr0 = sr;
            if (p) best = (atol(p + 13) - (sr > sr0 ? sr - sr0 : 0)) / 1024;
        }
    }
    long max = read_num("/sys/fs/cgroup/memory.max"), cur = read_num("/sys/fs/cgroup/memory.current");
    if (max > 0 && cur >= 0) {
        long inactive = 0;
        fd = open("/sys/fs/cgroup/memory.stat", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, b, sizeof b - 1);
            close(fd);
            if (n > 0) {
                /* page cache counts as available like MemAvailable does: all inactive_file, half of
                 * active_file (a freshly written WAL sits in active_file and is reclaimable) */
                b[n] = 0; char *p = strstr(b, "\ninactive_file "); if (p) inactive = atol(p + 15);
                p = strstr(b, "\nactive_file "); if (p) inactive += atol(p + 13) / 2;
            }
        }
        long cg = (max - cur + inactive) / (1024 * 1024);
        if (best < 0 || cg < best) best = cg;
    }
#endif
    return best;
}

/* TCP memory in pages (/proc/net/sockstat "TCP: ... mem N"), -1 if unavailable. */
/* TCP memory in pages and orphan count (/proc/net/sockstat "TCP: inuse N orphan N tw N alloc N mem N").
 * Orphans are reset by the kernel above tcp_max_orphans (8192 on the box, halved under tcp_mem pressure). */
static long tcp_mem_pages(uint32_t *orphans)
{
#ifdef __linux__
    char b[1024];
    int fd = open("/proc/net/sockstat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *p = strstr(b, "TCP:");
    if (!p) return -1;
    char *o = strstr(p, " orphan ");
    if (o) *orphans = (uint32_t)atol(o + 8);
    if (!(p = strstr(p, " mem "))) return -1;
    return atol(p + 5);
#else
    (void)orphans;
    return -1;
#endif
}

/* Parse the aggregate "cpu " line of /proc/stat (user nice system idle iowait irq softirq steal ...).
 * idle = idle + iowait: on 6.8 a task sleeping in io_cqring_wait with armed multishot ops is accounted
 * as iowait (exp31: 53-60% "iowait" on an idle box with an idle disk), so iowait must count as idle.
 * steal is excluded from both: on a VM it's time the hypervisor took, not our load.
 * Pressure-stall (PSI) io is never consulted, for the same reason. */
#ifdef __linux__
static void procstat_cpu(const char *b, long *idle, long *total)
{
    /* b points at a "cpu " (aggregate) or "cpuN " line */
    *idle = -1; *total = 0;
    if (strncmp(b, "cpu", 3)) return;
    char *p = (char *)b + 3;
    while (*p >= '0' && *p <= '9') p++;
    if (*p != ' ') return;
    long v[8] = {0};
    for (int k = 0; k < 8; k++) v[k] = strtol(p, &p, 10);
    *idle = v[3] + v[4];
    *total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6];
}
#endif

#ifdef BE_URING
static void arm_recv(loop_t *L, uint32_t id, conn_t *c);
static void issue_recv1(loop_t *L, uint32_t id, conn_t *c);
#endif
/* TcpExt ListenOverflows / ListenDrops from /proc/net/netstat (rising while parked = stalled clients). */
static void listen_stats(loop_t *L)
{
#ifdef __linux__
    char b[8192];
    int fd = open("/proc/net/netstat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return;
    b[n] = 0;
    char *hdr = strstr(b, "TcpExt:"), *val = hdr ? strstr(hdr + 7, "TcpExt:") : NULL;
    if (!val) return;
    char *he = strchr(hdr, '\n');
    if (!he) return;
    /* walk names and values in step */
    char *hp = hdr + 7, *vp = val + 7;
    while (hp < he) {
        while (*hp == ' ') hp++;
        char *ne = hp; while (*ne && *ne != ' ' && *ne != '\n') ne++;
        long v = strtol(vp, &vp, 10);
        if (ne - hp == 15 && !memcmp(hp, "ListenOverflows", 15)) L->st.listen_overflows = (uint64_t)v;
        else if (ne - hp == 11 && !memcmp(hp, "ListenDrops", 11)) L->st.listen_drops = (uint64_t)v;
        hp = ne;
    }
#else
    (void)L;
#endif
}

/* Which CPU signal the guard uses, decided once:
 *   1 CPU online (the scored box)          -> aggregate "cpu" line of /proc/stat (includes softirq)
 *   affinity is exactly one CPU N           -> that CPU's "cpuN" line (rigs pinning the server to one CPU)
 *   cgroup cpu.max quota <= 1.5 CPUs        -> cgroup cpu.stat usage / quota (docker --cpus 1)
 *   otherwise                               -> getrusage (process only) */
#ifdef __linux__
static long cg_usage_us(void)
{
    char b[512];
    int fd = open("/sys/fs/cgroup/cpu.stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *p = strstr(b, "usage_usec ");
    return p ? atol(p + 11) : -1;
}
#endif

static void busy_source(loop_t *L)
{
    L->busy_src_set = 1;
    L->busy_cpu = -2;
    int force = L->cfg.busy_source;                   /* 0 auto, 1 rusage, 2 box, 3 cgroup quota, 4 pinned cpuN */
    if (force == 1) return;
#ifdef __linux__
    if (force == 2 || (!force && sysconf(_SC_NPROCESSORS_ONLN) == 1)) { L->busy_cpu = -1; return; }
    cpu_set_t set;
    if ((!force || force == 4) && sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) == 1) {
        for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &set)) { L->busy_cpu = i; return; }
    }
    if (force == 4) { fprintf(stderr, "loop: busy_source=4 but affinity isn't a single CPU; using rusage\n"); return; }
    char b[64];
    int fd = open("/sys/fs/cgroup/cpu.max", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0 && b[0] >= '0' && b[0] <= '9') {
            b[n] = 0; char *e;
            long q = strtol(b, &e, 10), per = strtol(e, NULL, 10);
            if (q > 0 && per > 0 && (force == 3 || (float)q / (float)per <= 1.5f) && cg_usage_us() >= 0) { L->busy_cpu = -3; L->busy_quota = (float)q / (float)per; }
        }
    }
#endif
}

static void housekeeping(loop_t *L)
{
    /* timeouts: scan 1/8 of the used table per 250 ms tick -> full pass every 2 s */
    uint32_t hi = L->hi, step = hi / 8 + 64;
    for (uint32_t k = 0; k < step && hi; k++) {
        uint32_t id = L->sweep_pos++;
        if (id >= hi) { L->sweep_pos = 0; break; }
        conn_t *c = C(L, id);
        if (!(c->fl & F_OPEN) || (c->fl & F_DEAD)) continue;
        uint16_t age = (uint16_t)((uint16_t)L->now_s - c->tick);
#ifdef BE_URING
        /* watchdog: an open conn that has had no recv armed for >= 2 s is stranded (a lost
         * re-arm would leave its requests unread forever); re-arm it and count it */
        if (!L->epoll && !(c->fl & (F_RECV | F_PAUSED | F_EOF | F_CLOSE_AFTER)) && age >= 2) {
            arm_recv(L, id, c);
            L->st.stranded++;
        }
        if (!L->epoll && L->pollmode && (c->fl & F_AGAIN) && !(c->fl & (F_RECVING | F_PAUSED | F_EOF | F_CLOSE_AFTER)) && age >= 2) {
            issue_recv1(L, id, c);
            L->st.stranded++;
        }
#endif
        if (c->fl & F_LINGER) {
            if (age >= L->cfg.linger_s) conn_kill(L, id);
            continue;
        }
        if (conn_idle(c)) {
            if (L->cfg.idle_timeout_s && age >= L->cfg.idle_timeout_s) conn_kill(L, id);
        } else if (L->cfg.stall_timeout_s && age >= L->cfg.stall_timeout_s) conn_kill(L, id);
    }
    /* --- connection-cap policy (once per second) --- */
    long a = -1;
    if (L->cfg.conn_policy || L->cfg.mem_accept_floor_mb || L->cfg.mem_shed_floor_mb) {
        a = avail_mb();
        if (a >= 0) L->st.avail_mb = (uint32_t)a;
    }
    if (L->cfg.conn_policy && ++L->tick4 >= 4) {
        L->tick4 = 0;
        /* busy fraction. On a 1-CPU machine use the whole CPU (/proc/stat), because receive softirq and
         * handshake work for our sockets run there but are not billed to the process; elsewhere (dev
         * hosts with many CPUs) fall back to our own getrusage. */
        long idle = -1, total = 0;
        if (!L->busy_src_set) busy_source(L);
#ifdef __linux__
        if (L->busy_cpu >= -1) {                       /* whole box (-1) or the one CPU we're pinned to (N) */
            char b[16384]; int fd = open("/proc/stat", O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t n = read(fd, b, sizeof b - 1); close(fd);
                if (n > 0) {
                    b[n] = 0;
                    char key[24]; const char *line = b;
                    if (L->busy_cpu >= 0) { snprintf(key, sizeof key, "\ncpu%d ", L->busy_cpu); line = strstr(b, key); if (line) line++; }
                    if (line) procstat_cpu(line, &idle, &total);
                }
            }
        } else if (L->busy_cpu == -3) {                /* cgroup quota: usage over quota x wall */
            long u = cg_usage_us();
            if (u >= 0) {
                if (L->wall_ns_last)
                    L->st.busy = (float)((uint64_t)u - L->cpu_us_last) * 1000.0f / ((float)(L->now_ns - L->wall_ns_last) * L->busy_quota);
                L->cpu_us_last = (uint64_t)u; L->wall_ns_last = L->now_ns;
                idle = -2;                             /* handled */
            }
        }
#endif
        L->st.busy_src = L->busy_cpu;
        if (idle == -2) {
        } else if (idle >= 0) {
            if (L->wall_ns_last && total > (long)L->cpu_us_last)
                L->st.busy = 1.0f - (float)(idle - L->idle_last) / (float)(total - (long)L->cpu_us_last);
            L->cpu_us_last = (uint64_t)total; L->idle_last = idle; L->wall_ns_last = L->now_ns;
        } else {
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            uint64_t cpu = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
            if (L->wall_ns_last) L->st.busy = (float)(cpu - L->cpu_us_last) * 1000.0f / (float)(L->now_ns - L->wall_ns_last);
            L->cpu_us_last = cpu; L->wall_ns_last = L->now_ns;
        }
        uint64_t bpc = L->cfg.conn_bytes;
        if (L->cfg.conn_cap) L->st.cap = L->cfg.conn_cap;
        else if (a >= 0) {
            long res = (long)L->cfg.reserve_mb;
            if (!L->cap0) {
                long room = (long)L->st.open * (long)bpc / (1024 * 1024) + a - res;   /* what our sockets may use in total */
                L->cap0 = room > 0 ? (uint32_t)((uint64_t)room * 1024 * 1024 / bpc) : 0;
                L->st.cap = L->cap0;
            } else if (a < res && a <= L->avail_last) {
                /* below the reserve and still falling: shrink towards open - deficit, but by at most 1% of the
                 * startup cap per second. Memory falls for two reasons: more connections (the cap should bind)
                 * or CPU saturation queueing unread requests (+4 KB each). In the second case a fast shrink
                 * turned into a shed -> reconnect -> CPU spiral (arena, 540k users), so the excess parks
                 * instead (see the guard below); the 100/60 MB floors stay the hard backstop. */
                uint32_t shrink = (uint32_t)((uint64_t)(res - a) * 1024 * 1024 / bpc);
                uint32_t target = L->st.open > shrink ? L->st.open - shrink : 0;
                uint32_t step = L->cap0 / 100 + 1;
                if (target < L->st.cap) {
                    L->st.cap = L->st.cap - target > step ? L->st.cap - step : target;
                    if (L->st.cap < L->cap0 / 2) L->st.cap = L->cap0 / 2;
                    L->cap_shrunk_s = L->now_s;
                }
            } else if (L->st.cap < L->cap0 && a > res + 32) {    /* recover only with 32 MB of hysteresis */
                L->st.cap += L->cap0 / 100 + 1;
                if (L->st.cap > L->cap0) L->st.cap = L->cap0;
            }
            L->avail_last = a;
        }
        {   /* never above the descriptors we can hold: the rings' direct-descriptor tables (each <=
             * RLIMIT_NOFILE), or the fd table minus a few for the process's own files */
            uint64_t slots = L->tab_n > 64 ? L->tab_n - 64 : 0;
#ifdef BE_URING
            if (!L->epoll) slots = (uint64_t)L->nrings * (L->ring_per - 16);   /* rings are added on demand */
#endif
            L->slots_cap = (uint32_t)slots;
            if (L->st.cap > slots) L->st.cap = (uint32_t)slots;
            if (L->cfg.cap_ceiling && L->st.cap > L->cfg.cap_ceiling) L->st.cap = L->cfg.cap_ceiling;
        }
        /* CPU guard: falling behind costs +4 KB per unread request and its wait counts as latency */
        if (L->st.busy > L->cfg.guard_freeze) { if (L->f_ceiling > L->st.close_frac) L->f_ceiling = L->st.close_frac; }
        else if (L->st.busy < L->cfg.guard_relax) { L->f_ceiling += 0.05f; if (L->f_ceiling > 1) L->f_ceiling = 1; }
        listen_stats(L);
        /* parked at the cap: size the accept queue so a queued client waits <= ~50 ms (closes free slots at
         * `rate`); a bigger queue would hold clients that think they're connected (their wait counts as
         * latency), a tiny one SYN-ACKs everyone and drops their ACKs (RTO-long stalls). Re-listen() only on
         * a > 25% change; Linux resizes a live listener and keeps queued connections. */
        uint32_t rate = (uint32_t)(L->st.closed - L->closed_last);
        L->closed_last = L->st.closed;
        uint32_t want = (uint32_t)L->cfg.backlog;
        if (L->cap_parked && L->cfg.backlog_parked_dynamic > 0) {
            want = rate / 20;
            if (want < 256) want = 256;
            if (want > (uint32_t)L->cfg.backlog) want = (uint32_t)L->cfg.backlog;
        }
        if (want != L->st.backlog_now && (want * 4 > L->st.backlog_now * 5 || want * 4 < L->st.backlog_now * 3 || want == (uint32_t)L->cfg.backlog)) {
            if (listen(L->lfd, (int)want) == 0) L->st.backlog_now = want;
        }
    }
    float f = 0;
    uint32_t cap = L->st.cap;
    if (L->cfg.conn_policy && L->cfg.shed_headroom && a >= 0) {
        /* static f(headroom), every 250 ms tick on an EWMA of ~1 s. Shedding trades CPU for memory, so only
         * memory moves it; the 100/60 MB floors below stay the backstop. Park only at the descriptor limit. */
        L->h_ewma = L->h_ewma > 0 ? 0.75f * L->h_ewma + 0.25f * (float)a : (float)a;
        float hi = (float)L->cfg.shed_hi_mb, lo = (float)L->cfg.shed_lo_mb;
        if (L->h_ewma < hi) f = L->cfg.shed_fmax * (hi - L->h_ewma) / (hi > lo ? hi - lo : 1);
        f = f < 0 ? 0 : f > L->cfg.shed_fmax ? L->cfg.shed_fmax : f;
        cap = L->st.cap = L->slots_cap;
        int park = cap && L->st.open >= cap;
        if (park != L->cap_parked) { L->cap_parked = park; if (L->mem_state == 0) be_accept_ctl(L, !park); }
    } else if (L->cfg.conn_policy && cap) {
        float lo = L->cfg.policy_release * (float)cap, open = (float)L->st.open;
        if (!L->engaged && open >= L->cfg.policy_engage * (float)cap) L->engaged = 1;
        if (L->engaged && open < lo) L->engaged = 0;
        if (L->engaged) f = (open - lo) / ((float)cap - lo);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        /* the CPU guard limits f inside the band, but never at the cap: persistent parking fails p99
         * whatever the backlog (exp41: 1.3-1.9 s), so at >= 0.95 cap reconnect CPU is spent instead */
        /* ...but only at a stable cap: one that shrank in the last 10 s did so because memory fell, often
         * from CPU saturation itself, and spending CPU on reconnects there feeds the spiral. At a moving cap
         * the guard holds and the excess parks. */
        int cap_stable = !L->cap_shrunk_s || (uint32_t)(L->now_s - L->cap_shrunk_s) > 10;
        if (f > L->f_ceiling && !(cap_stable && open >= 0.95f * (float)cap)) f = L->f_ceiling;
        /* hard cap: park new users (single-shot accepts drain; SYNs then wait in the backlog/retry) */
        int park = L->st.open >= cap;
        if (park != L->cap_parked) { L->cap_parked = park; if (L->mem_state == 0) be_accept_ctl(L, !park); }
    }
    long tp = tcp_mem_pages(&L->st.tcp_orphans);
    if (tp >= 0) {
        L->st.tcp_mem_pages = (uint32_t)tp;
        if (L->tcp_mem_pressure) {   /* tcp_mem[1]: the kernel starts squeezing socket buffers here */
            float g = ((float)tp - 0.7f * (float)L->tcp_mem_pressure) / (0.3f * (float)L->tcp_mem_pressure);
            if (g > f) f = g > 1 ? 1 : g;
        }
    }
    L->st.close_frac = f;
    if (a >= 0 && (L->cfg.mem_accept_floor_mb || L->cfg.mem_shed_floor_mb)) {
        int st = L->mem_state;
        /* hysteresis: 32 MiB above each floor before leaving a state */
        if (L->cfg.mem_shed_floor_mb && a < (long)L->cfg.mem_shed_floor_mb) st = 2;
        else if (L->cfg.mem_accept_floor_mb && a < (long)L->cfg.mem_accept_floor_mb) st = st > 1 && a < (long)L->cfg.mem_shed_floor_mb + 32 ? 2 : 1;
        else if (st && a > (long)(st == 2 ? L->cfg.mem_shed_floor_mb : L->cfg.mem_accept_floor_mb) + 32) st = st == 2 && L->cfg.mem_accept_floor_mb && a < (long)L->cfg.mem_accept_floor_mb + 32 ? 1 : 0;
        if ((st == 0) != (L->mem_state == 0)) be_accept_ctl(L, st == 0 && !L->cap_parked);
        L->mem_state = st;
        if (st == 2) loop_shed_idle(L, L->st.open / 50 + 1);
    }
    L->st.accepting = L->mem_state == 0 && !L->cap_parked;
}

/* ------------------------------------------------------------------ listen socket */

static int make_listener(const loop_cfg *cfg)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (cfg->nodelay > 0) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (cfg->rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg->rcvbuf, sizeof cfg->rcvbuf);
    if (cfg->sndbuf) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cfg->sndbuf, sizeof cfg->sndbuf);
#ifdef SO_TIMESTAMPNS
    if (cfg->rx_timestamps) setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one);   /* inherited */
#endif
#ifdef TCP_DEFER_ACCEPT
    if (cfg->defer_accept) setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &cfg->defer_accept, sizeof cfg->defer_accept);
#endif
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cfg->port) };
    inet_pton(AF_INET, cfg->host, &sa.sin_addr);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, cfg->backlog) < 0) {
        int e = errno; close(fd); return -e;
    }
    return fd;
}

/* ------------------------------------------------------------------ lifecycle */

loop_t *loop_new(const loop_cfg *in)
{
    loop_t *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->cfg = *in;
    loop_cfg *c = &L->cfg;
    if (!c->host) c->host = "0.0.0.0";
    if (!c->port) c->port = 80;
    if (!c->backlog) c->backlog = 4096;
    if (!c->max_conns) c->max_conns = 600000;
    if (!c->sq_entries) c->sq_entries = 1024;
    if (!c->bufs) c->bufs = 512;
    if (!c->buf_size) c->buf_size = 2048;
    if (!c->batch) c->batch = 128;
    if (!c->wait_us) c->wait_us = 2000;
    if (!c->max_in) c->max_in = 64 * 1024;
    if (!c->out_hiwat) c->out_hiwat = 256 * 1024;
    if (!c->stall_timeout_s) c->stall_timeout_s = 30;
    if (!c->nodelay) c->nodelay = 1;
    if (!c->accept_singles) c->accept_singles = 256;
    if (!c->shed_min_idle_s) c->shed_min_idle_s = 20;
    if (!c->conn_bytes) c->conn_bytes = 4400 + sizeof(conn_t);
    if (!c->reserve_mb) c->reserve_mb = 250;
    if (!c->cap_ceiling) c->cap_ceiling = 380000;
    if (!c->linger_s) c->linger_s = 5;
    if (!c->policy_engage) c->policy_engage = 0.85f;
    if (!c->policy_release) c->policy_release = 0.75f;
    if (!c->shed_hi_mb) c->shed_hi_mb = 400;
    if (!c->shed_lo_mb) c->shed_lo_mb = 200;
    if (!c->shed_fmax) c->shed_fmax = 0.5f;   /* never shed more than half: a deficit shedding can't fix pinned f at 1 and emptied the server (exp71); the 100/60 MB floors handle real emergencies */
    if (!c->guard_freeze) c->guard_freeze = 0.92f;
    if (!c->guard_relax) c->guard_relax = 0.85f;
    if (c->conn_cap) c->conn_policy = 1;
    if (!c->poll_wait_us) c->poll_wait_us = 1000;
    if (!c->max_cqes) c->max_cqes = 2048;
    L->vin.sz = sizeof(loop_in); L->vmeta.sz = sizeof(bmeta);
    L->touched.sz = L->ready.sz = L->closing.sz = L->rearm.sz = sizeof(cref);
    L->staged.sz = sizeof(staged_t);
    L->fop_done.sz = sizeof(fop_rec *);
    L->now_ns = mono_ns(); L->now_s = (uint32_t)(L->now_ns / 1000000000ull);
    L->rng = 0x9e3779b9u;
    L->rst_ok = 1;
    L->f_ceiling = 1;
#ifdef __linux__
    {   /* tcp_mem is only visible in the initial network namespace; 0 = unknown */
        int fd = open("/proc/sys/net/ipv4/tcp_mem", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { char b[128]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
            if (n > 0) { b[n] = 0; char *e; strtol(b, &e, 10); L->tcp_mem_pressure = strtol(e, NULL, 10); } }
    }
#endif
    L->lfd = c->listen_fd > 0 ? c->listen_fd : make_listener(c);
    L->st.backlog_now = (uint32_t)c->backlog;
    if (L->lfd < 0) { errno = -L->lfd; free(L); return NULL; }
    int r = be_init(L);
    if (r < 0) { close(L->lfd); free(L); errno = -r; return NULL; }
    return L;
}

/* readiness backends: run the ops now (blocking), queue callbacks for after the batch */
static void sync_fops(loop_t *L, const loop_fop *ops, int n)
{
    int failed = 0;
    for (int i = 0; i < n; i++) {
        const loop_fop *o = &ops[i];
        long r;
        if (failed) r = -ECANCELED;
        else {
            switch (o->op) {
            case LOOP_FOP_READ: r = pread(o->fd, o->buf, o->len, (off_t)o->off); break;
            case LOOP_FOP_WRITE: r = pwrite(o->fd, o->buf, o->len, (off_t)o->off); break;
#ifdef __linux__
            case LOOP_FOP_FDATASYNC: r = fdatasync(o->fd); break;
#endif
            default: r = fsync(o->fd); break;
            }
            if (r < 0) r = -errno;
            /* a short read/write breaks a chain, as io_uring does */
            else if ((o->op == LOOP_FOP_READ || o->op == LOOP_FOP_WRITE) && (uint32_t)r < o->len && o->link) failed = 1;
        }
        if (r < 0 && o->link) failed = 1;
        else if (!o->link) failed = 0;
        fop_rec *f = malloc(sizeof *f);
        if (!f) abort();
        f->cb = o->cb; f->ud = o->ud; f->res = (int)r;
        *(fop_rec **)vpush(&L->fop_done) = f;
    }
}

static void deliver_fops(loop_t *L)
{
    /* callbacks may submit more ops: take the current list first */
    vec_t v = L->fop_done;
    L->fop_done.p = NULL; L->fop_done.n = L->fop_done.cap = 0;
    for (uint32_t i = 0; i < v.n; i++) {
        fop_rec *f = ((fop_rec **)v.p)[i];
        L->st.file_ops++;
        if (f->cb) f->cb(L, f->ud, f->res);
        free(f);
    }
    free(v.p);
}

int loop_run(loop_t *L)
{
    signal(SIGPIPE, SIG_IGN);
    be_accept_ctl(L, 1);
    while (!L->stop) {
        int r = be_wait_and_reap(L);
        if (r < 0) return r;
        L->now_ns = mono_ns(); L->now_s = (uint32_t)(L->now_ns / 1000000000ull);
        { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); L->real_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
        run_batch(L);
        process_closing(L);
        if (L->cfg.on_tick) L->cfg.on_tick(L, L->cfg.ud);
        if (L->now_ns >= L->next_house_ns) {
            L->next_house_ns = L->now_ns + 250000000ull;
            housekeeping(L);
            process_closing(L);
        }
        if (L->fop_done.n) deliver_fops(L);
        be_after_batch(L);
    }
    return 0;
}

void loop_free(loop_t *L)
{
    if (!L) return;
    be_free(L);
    if (L->lfd >= 0) close(L->lfd);
    for (uint32_t i = 0; i < L->hi; i++) { buf_put(L, L->tab[i].in); buf_put(L, L->tab[i].out); }
    for (int k = 0; k < NCLS; k++) while (L->pool[k]) { buf_t *b = L->pool[k]; L->pool[k] = b->next; free(b); }
    if (L->tab) munmap(L->tab, L->tab_bytes);
    free(L->vin.p); free(L->vmeta.p); free(L->touched.p); free(L->staged.p);
    free(L->ready.p); free(L->closing.p); free(L->rearm.p);
    free(L);
}

static int alloc_table(loop_t *L, size_t n)
{
    L->tab_n = n;
    L->tab_bytes = (n * sizeof(conn_t) + 4095) & ~(size_t)4095;
    L->tab = mmap(NULL, L->tab_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#ifdef MADV_HUGEPAGE
    if (L->tab != MAP_FAILED && L->cfg.hugepages) madvise(L->tab, L->tab_bytes, MADV_HUGEPAGE);
#endif
    return L->tab == MAP_FAILED ? (L->tab = NULL, -ENOMEM) : 0;
}

/* ================================================================== io_uring backend */
#ifdef BE_URING

static int sys_setup(unsigned n, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, n, p); }
static int sys_register(int fd, unsigned op, void *arg, unsigned nr) { return (int)syscall(__NR_io_uring_register, fd, op, arg, nr); }

static int ring_enter(loop_t *L, ring_t *r, unsigned min, unsigned flags, uint64_t wait_ns)
{
    struct __kernel_timespec ts = { .tv_sec = (long long)(wait_ns / 1000000000ull), .tv_nsec = (long long)(wait_ns % 1000000000ull) };
    struct io_uring_getevents_arg arg = { .sigmask = 0, .sigmask_sz = _NSIG / 8, .ts = (uint64_t)(uintptr_t)&ts };
    unsigned sub = r->to_submit;
    if (sub) __atomic_store_n(r->sq_tail, r->sq_ltail, __ATOMIC_RELEASE);
    r->to_submit = 0;
    L->st.enters++;
    int ret = (int)syscall(__NR_io_uring_enter, r->ridx, sub, min,
                           flags | IORING_ENTER_REGISTERED_RING | (wait_ns ? IORING_ENTER_EXT_ARG : 0),
                           wait_ns ? (void *)&arg : NULL, wait_ns ? sizeof arg : (size_t)0);
    if (ret < 0) ret = -errno;
    return ret;
}

static struct io_uring_sqe *get_sqe(loop_t *L, ring_t *r)
{
    if (r->sq_ltail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >= r->sq_entries) {
        ring_enter(L, r, 0, 0, 0);
        while (r->sq_ltail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >= r->sq_entries)
            ring_enter(L, r, 1, IORING_ENTER_GETEVENTS, 0);   /* SUBMIT_ALL: should not happen */
    }
    struct io_uring_sqe *s = &r->sqes[r->sq_ltail & r->sq_mask];
    memset(s, 0, sizeof *s);
    r->sq_ltail++;
    r->to_submit++;
    return s;
}

static inline uint64_t ud_mk(unsigned op, unsigned ring, uint32_t slot, uint32_t gen)
{
    return (uint64_t)op | (uint64_t)ring << 3 | (uint64_t)slot << 8 | (uint64_t)gen << 32;
}
static inline uint32_t id_mk(loop_t *L, unsigned ring, uint32_t slot) { return ring << L->slot_bits | slot; }
static inline unsigned id_ring(loop_t *L, uint32_t id) { return id >> L->slot_bits; }
static inline uint32_t id_slot(loop_t *L, uint32_t id) { return id & ((1u << L->slot_bits) - 1); }

static struct io_uring_sqe *get_sqe(loop_t *L, ring_t *r);
static void ur_return_pbuf(loop_t *L, uint16_t ri, uint16_t bid)
{
    ring_t *r = &L->rings[ri];
    if (r->legacy) {            /* pre-buffer-ring path: one PROVIDE_BUFFERS SQE per buffer */
        struct io_uring_sqe *s = get_sqe(L, r);
        s->opcode = IORING_OP_PROVIDE_BUFFERS;
        s->fd = 1;
        s->addr = (uint64_t)(uintptr_t)(r->bufmem + (size_t)bid * L->cfg.buf_size);
        s->len = L->cfg.buf_size;
        s->off = bid;
        s->buf_group = 0;
        s->user_data = OP_IGN;
        return;
    }
    struct io_uring_buf *b = &r->br->bufs[r->br_tail & (L->cfg.bufs - 1)];
    b->addr = (uint64_t)(uintptr_t)(r->bufmem + (size_t)bid * L->cfg.buf_size);
    b->len = L->cfg.buf_size;
    b->bid = bid;
    r->br_tail++;
    r->br_dirty = 1;
}

/* Poll mode. A multishot recv parked on an idle socket also allocates an async msghdr (512 B) and an
 * async poll (96 B), neither charged to the cgroup; a multishot POLL_ADD needs only its io_kiocb
 * (armmem.sh: 864 vs 256 B per idle connection). Readiness then triggers one non-blocking one-shot recv
 * (at most one in flight per conn, so data can't be reordered), re-issued while it fills the buffer or
 * more readiness arrived meanwhile. */
static void issue_recv1(loop_t *L, uint32_t id, conn_t *c)
{
    ring_t *r = &L->rings[id_ring(L, id)];
    struct io_uring_sqe *s = get_sqe(L, r);
    s->opcode = IORING_OP_RECV;
    s->fd = (int)id_slot(L, id);
    s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    s->msg_flags = MSG_DONTWAIT;              /* EAGAIN instead of arming another (allocating) poll */
    s->buf_group = 0;
    s->user_data = ud_mk(OP_RECV1, id_ring(L, id), id_slot(L, id), c->gen);
    c->fl = (uint16_t)((c->fl | F_RECVING) & ~F_AGAIN);
    r->recv1 = 1;
}

static void arm_recv(loop_t *L, uint32_t id, conn_t *c)
{
    ring_t *r = &L->rings[id_ring(L, id)];
    struct io_uring_sqe *s = get_sqe(L, r);
    if (L->pollmode) {
        s->opcode = IORING_OP_POLL_ADD;
        s->fd = (int)id_slot(L, id);
        s->flags = IOSQE_FIXED_FILE;
        s->poll32_events = POLLIN | POLLRDHUP;
        s->len = IORING_POLL_ADD_MULTI;
        s->user_data = ud_mk(OP_POLL, id_ring(L, id), id_slot(L, id), c->gen);
        c->fl |= F_RECV;
        return;
    }
    s->opcode = IORING_OP_RECV;
    if (L->cfg.rx_timestamps) { s->opcode = IORING_OP_RECVMSG; s->addr = (uint64_t)(uintptr_t)&L->rx_msg; s->len = 1; }
    {   /* measurement hook (not for production): LOOP_TEST_ARM=poll arms a multishot POLL_ADD instead,
         * =none arms nothing, to compare kernel bytes per idle connection */
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("LOOP_TEST_ARM"); mode = !e ? 0 : !strcmp(e, "poll") ? 1 : !strcmp(e, "none") ? 2 : 0; }
        if (mode == 2) { s->opcode = IORING_OP_NOP; s->fd = -1; s->user_data = ud_mk(OP_IGN, 0, 0, 0); c->fl |= F_RECV; return; }
        if (mode == 1) {
            s->opcode = IORING_OP_POLL_ADD; s->fd = (int)id_slot(L, id); s->flags = IOSQE_FIXED_FILE;
            s->poll32_events = POLLIN | POLLRDHUP; s->len = IORING_POLL_ADD_MULTI;
            s->user_data = ud_mk(OP_IGN, 0, 0, 0); c->fl |= F_RECV; return;
        }
    }
    s->fd = (int)id_slot(L, id);
    s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    s->ioprio = IORING_RECV_MULTISHOT;
    s->buf_group = 0;
    s->user_data = ud_mk(OP_RECV, id_ring(L, id), id_slot(L, id), c->gen);
    c->fl |= F_RECV;
}

/* A multishot accept drains the whole listen queue per wakeup. If the ring's table fills
 * mid-burst the kernel accepts the socket, fails to install it (-ENFILE) and closes it:
 * the client sees a reset. So multishot only runs on a ring with more free slots than the
 * listen backlog can hold; near capacity we fall back to a bounded number of single-shot
 * accepts, each of which is guaranteed a slot. */
static void arm_accept(loop_t *L, int ri, int multi)
{
    ring_t *r = &L->rings[ri];
    struct io_uring_sqe *s = get_sqe(L, r);
    s->opcode = IORING_OP_ACCEPT;
    s->fd = L->lfd;
    s->ioprio = multi ? IORING_ACCEPT_MULTISHOT : 0;
    s->file_index = IORING_FILE_INDEX_ALLOC;
    s->user_data = ud_mk(OP_ACCEPT, (unsigned)ri, multi ? 0 : 1, 0);
    if (multi) { r->accepting = 1; L->acc_ring = ri; }
    else r->singles++;
}

static inline uint32_t ring_free(ring_t *r) { return r->cap - r->open - r->singles; }
/* Headroom a ring must keep while a multishot accept is armed on it. The kernel keeps
 * accepting for as long as handshakes complete during one wait, so this is not bounded by
 * the backlog alone; a quarter of the table (16k slots at 65,535) covers any realistic
 * burst between two reaps. */
static inline uint32_t multi_headroom(loop_t *L, ring_t *r)
{
    uint32_t h = (uint32_t)L->cfg.backlog + 256;
    return h > r->cap / 4 ? h : r->cap / 4;
}

static void cancel_ud(loop_t *L, ring_t *r, uint64_t ud)
{
    struct io_uring_sqe *s = get_sqe(L, r);
    s->opcode = IORING_OP_ASYNC_CANCEL;
    s->addr = ud;
    s->user_data = ud_mk(OP_IGN, 0, 0, 0);
}

static int ring_init(loop_t *L, ring_t *r, unsigned idx, unsigned slots);
static void pick_accept_ring(loop_t *L)
{
    if (!L->want_accept || L->acc_ring >= 0) return;
    uint32_t inflight = 0;
    int bi = -1;
    for (unsigned k = 0; k < L->nlive; k++) {
        ring_t *r = &L->rings[k];
        inflight += r->singles;
        if (L->cfg.accept_multishot && ring_free(r) > multi_headroom(L, r)) { arm_accept(L, (int)k, 1); return; }
        if (bi < 0 && ring_free(r) > 8) bi = (int)k;   /* fill rings in order: few rings to reap per pass */
    }
    if (bi < 0 && !inflight && L->nlive < L->nrings) {
        /* every live ring is full: create the next one now (each ring costs ~1.3 MB of kernel memory
         * for its file table, plus SQ/CQ and recv buffers, so they are not created up front) */
        int r = ring_init(L, &L->rings[L->nlive], L->nlive, L->ring_per);
        if (r < 0) { fprintf(stderr, "loop: ring %u init: %s (staying at %u rings)\n", L->nlive, strerror(-r), L->nlive); L->nrings = L->nlive; }
        else { bi = (int)L->nlive++; L->ring_wait_poll_active = !L->cfg.ring_wait_evfd; }
    }
    /* near capacity: keep a bounded number of single-shot accepts outstanding (each owns
     * a slot), leaving 8 spare for closes the kernel has not executed yet */
    while (bi >= 0 && inflight < L->cfg.accept_singles && ring_free(&L->rings[bi]) > 8) {
        arm_accept(L, bi, 0);
        inflight++;
    }
}

static void ur_accept_ctl(loop_t *L, int on)
{
    L->want_accept = on;
    if (on) pick_accept_ring(L);
    else {
        if (L->acc_ring >= 0)   /* the terminating CQE clears acc_ring */
            cancel_ud(L, &L->rings[L->acc_ring], ud_mk(OP_ACCEPT, (unsigned)L->acc_ring, 0, 0));
        for (unsigned k = 0; k < L->nlive; k++)
            if (L->rings[k].singles) {
                cancel_ud(L, &L->rings[k], ud_mk(OP_ACCEPT, k, 1, 0));
                L->rings[k].sqes[(L->rings[k].sq_ltail - 1) & L->rings[k].sq_mask].cancel_flags = IORING_ASYNC_CANCEL_ALL;
            }
    }
}

static void ur_send(loop_t *L, uint32_t id, conn_t *c, buf_t *sb)
{
    (void)c;
    ring_t *r = &L->rings[id_ring(L, id)];
    struct io_uring_sqe *s = get_sqe(L, r);
    if (!sb->off && L->cfg.send_zc && sb->len >= (uint32_t)L->cfg.send_zc) sb->zc = 1;
    int zc = sb->zc;
    s->opcode = zc ? IORING_OP_SEND_ZC : IORING_OP_SEND;
    s->fd = (int)id_slot(L, id);
    s->flags = IOSQE_FIXED_FILE;
    s->addr = (uint64_t)(uintptr_t)(sb->d + sb->off);
    s->len = sb->len - sb->off;
    s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
    s->user_data = (uint64_t)(uintptr_t)sb | OP_SEND;
    if (zc) sb->refs++;
    L->st.send_bytes += sb->len - sb->off;
}

static void ur_close(loop_t *L, uint32_t id, conn_t *c)
{
    ring_t *r = &L->rings[id_ring(L, id)];
    /* The multishot recv holds a file reference, so the socket only really closes once it
     * terminates: cancel it, then drop the table slot. Both run inline at submit (an
     * IORING_OP_SHUTDOWN would be punted to an io-wq thread, and the slot would stay
     * occupied until it ran). The last reference sends FIN (RST if unread data remains). */
    struct io_uring_sqe *s;
    if ((c->fl & F_RST) && L->rst_ok) {
        /* shedding: SO_LINGER {1,0} makes the close an RST (no TIME_WAIT, no FIN handshake).
         * Socket setsockopt via URING_CMD exists since 6.7; a failure disables it (FIN). */
        static const struct linger lg = { 1, 0 };
        s = get_sqe(L, r);
        s->opcode = IORING_OP_URING_CMD;
        s->fd = (int)id_slot(L, id);
        s->flags = IOSQE_FIXED_FILE;
        s->cmd_op = SOCKET_URING_OP_SETSOCKOPT;
        s->level = SOL_SOCKET;
        s->optname = SO_LINGER;
        s->optlen = sizeof lg;
        s->optval = (uint64_t)(uintptr_t)&lg;
        s->user_data = ud_mk(OP_IGN, 0, 1, 0);
        L->st.rst++;
    }
    if (c->fl & F_RECV) cancel_ud(L, r, ud_mk(L->pollmode ? OP_POLL : OP_RECV, id_ring(L, id), id_slot(L, id), c->gen));
    s = get_sqe(L, r);
    s->opcode = IORING_OP_CLOSE;
    s->file_index = id_slot(L, id) + 1;
    s->user_data = ud_mk(OP_IGN, 0, 0, 0);
    (void)c;
    r->open--;
    if (L->acc_ring < 0) pick_accept_ring(L);
}

#define GEN_FDINSTALL 0xfffffffeu    /* OP_IGN marker: CQE of a FIXED_FD_INSTALL for a lingering close */

static void ur_shutdown_wr_slow(loop_t *L, unsigned ring, uint32_t slot)
{
    /* IORING_OP_SHUTDOWN always runs on an io-wq worker (capped at 1 per ring in ring_init): ~+20 us */
    struct io_uring_sqe *s = get_sqe(L, &L->rings[ring]);
    s->opcode = IORING_OP_SHUTDOWN;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE;
    s->len = SHUT_WR;
    s->user_data = ud_mk(OP_IGN, 0, 0, 0);
}

static void ur_shutdown_wr(loop_t *L, uint32_t id, conn_t *c)
{
    (void)c;
    if (L->no_fdinstall) { ur_shutdown_wr_slow(L, id_ring(L, id), id_slot(L, id)); return; }
    /* 6.8+: install the direct descriptor as a normal fd (inline, no io-wq), then shutdown(SHUT_WR) and
     * close() that fd synchronously when the CQE arrives; the table slot keeps the socket alive */
    struct io_uring_sqe *s = get_sqe(L, &L->rings[id_ring(L, id)]);
    s->opcode = IORING_OP_FIXED_FD_INSTALL;
    s->fd = (int)id_slot(L, id);
    s->flags = IOSQE_FIXED_FILE;
    s->user_data = ud_mk(OP_IGN, id_ring(L, id), id_slot(L, id), GEN_FDINSTALL);
}

static int ur_file_submit(loop_t *L, const loop_fop *ops, int n)
{
    ring_t *r = &L->rings[0];
    if ((unsigned)n > r->sq_entries) return -EINVAL;
    /* a link chain must reach the kernel in one submission: make room for all of it first */
    if (r->sq_entries - (r->sq_ltail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE)) < (unsigned)n) {
        ring_enter(L, r, 0, 0, 0);
        if (r->sq_entries - (r->sq_ltail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE)) < (unsigned)n) return -EAGAIN;
    }
    for (int i = 0; i < n; i++) {
        const loop_fop *o = &ops[i];
        fop_rec *f = aligned_alloc(16, sizeof(fop_rec) < 16 ? 16 : (sizeof(fop_rec) + 15) & ~(size_t)15);
        if (!f) abort();
        f->cb = o->cb; f->ud = o->ud;
        struct io_uring_sqe *s = get_sqe(L, r);
        switch (o->op) {
        case LOOP_FOP_READ: s->opcode = IORING_OP_READ; break;
        case LOOP_FOP_WRITE: s->opcode = IORING_OP_WRITE; break;
        default: s->opcode = IORING_OP_FSYNC; if (o->op == LOOP_FOP_FDATASYNC) s->fsync_flags = IORING_FSYNC_DATASYNC; break;
        }
        s->fd = o->fd;
        if (o->op == LOOP_FOP_READ || o->op == LOOP_FOP_WRITE) {
            s->addr = (uint64_t)(uintptr_t)o->buf; s->len = o->len; s->off = o->off;
        }
        /* IOSQE_ASYNC: straight to io-wq (1 bounded worker per ring). Otherwise io_uring tries buffered
         * writes inline and can run the linked follow-ups from task_work on the loop thread: ~5 ms of p99 with
         * 256-page chains (engine, Xeon). */
        s->flags = IOSQE_ASYNC;
        if (o->link && i + 1 < n) s->flags |= IOSQE_IO_LINK;
        s->user_data = (uint64_t)(uintptr_t)f | 8 | OP_SEND;
    }
    return 0;
}

static void ur_pause(loop_t *L, uint32_t id, conn_t *c)
{
    if (c->fl & F_RECV)
        cancel_ud(L, &L->rings[id_ring(L, id)], ud_mk(L->pollmode ? OP_POLL : OP_RECV, id_ring(L, id), id_slot(L, id), c->gen));
}

static void ur_resume(loop_t *L, uint32_t id, conn_t *c)
{
    if (!(c->fl & (F_RECV | F_EOF | F_DEAD))) arm_recv(L, id, c);
    /* poll mode: data may already sit in the socket with no new wakeup coming; drain it */
    if (L->pollmode && !(c->fl & (F_RECVING | F_EOF | F_DEAD))) issue_recv1(L, id, c);
}

static void handle_cqe(loop_t *L, unsigned ri, struct io_uring_cqe *cqe)
{
    uint64_t ud = cqe->user_data;
    int res = cqe->res;
    unsigned op = ud & 7;
    if (op == OP_SEND && (ud & 8)) {                  /* loop_file_submit op */
        fop_rec *f = (fop_rec *)(uintptr_t)(ud & ~(uint64_t)15);
        L->st.file_ops++;
        if (f->cb) f->cb(L, f->ud, res);
        free(f);
        return;
    }
    if (op == OP_SEND) {
        buf_t *sb = (buf_t *)(uintptr_t)(ud & ~(uint64_t)7);
        if (cqe->flags & IORING_CQE_F_NOTIF) {
            if (!--sb->refs && sb->done) buf_put(L, sb);
            return;
        }
        if (sb->zc && !(cqe->flags & IORING_CQE_F_MORE)) sb->refs--;   /* no notification will follow */
        if (sb->done) { if (!sb->refs) buf_put(L, sb); return; }
        conn_t *c = C(L, sb->conn);
        int live = (c->fl & F_OPEN) && c->gen == sb->gen && !(c->fl & F_DEAD);
        if (res > 0 && live && sb->off + (uint32_t)res < sb->len) {
            sb->off += (uint32_t)res;                  /* short send despite WAITALL */
            L->st.partial_sends++;
            ur_send(L, sb->conn, c, sb);
            return;
        }
        send_done(L, sb, res < 0);
        return;
    }
    uint32_t slot = (uint32_t)(ud >> 8) & 0xffffff;
    uint32_t gen = (uint32_t)(ud >> 32);
    unsigned uri = (ud >> 3) & 31;
    ring_t *r = &L->rings[ri];
    switch (op) {
    case OP_RECV: {
        uint32_t id = id_mk(L, uri, slot);
        conn_t *c = C(L, id);
        int live = (c->fl & F_OPEN) && c->gen == gen;
        int more = cqe->flags & IORING_CQE_F_MORE;
        if (cqe->flags & IORING_CQE_F_BUFFER) {
            uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            int kept = 0;
            char *p = r->bufmem + (size_t)bid * L->cfg.buf_size;
            uint64_t ts = 0;
            if (res > 0 && L->cfg.rx_timestamps) {
                /* io_uring_recvmsg_out | name (0) | control | payload */
                struct io_uring_recvmsg_out *o = (struct io_uring_recvmsg_out *)p;
                char *ctl = p + sizeof *o + L->rx_msg.msg_namelen;
                struct msghdr mh = { .msg_control = ctl, .msg_controllen = o->controllen };
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
                    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS) {
                        struct timespec t; memcpy(&t, CMSG_DATA(cm), sizeof t);
                        ts = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
                    }
                p = ctl + L->rx_msg.msg_controllen;
                res = (int)o->payloadlen;               /* 0 = EOF */
            }
            if (live && res > 0)
                kept = conn_data(L, id, p, (uint32_t)res, 1, (uint16_t)ri, bid, ts);
            if (!kept) ur_return_pbuf(L, (uint16_t)ri, bid);
        }
        if (!live) return;
        if (!more) c->fl &= ~F_RECV;
        if (res == 0) { conn_eof(L, id); return; }
        if (res < 0) {
            if (res == -ENOBUFS) {
                L->st.enobufs++;
                cref *q = vpush(&L->rearm); q->conn = id; q->gen = gen;
            } else if (res == -ECANCELED) {
                /* paused for backpressure (resumed by send_done); if already resumed, re-arm */
                if (!(c->fl & F_PAUSED)) { cref *q = vpush(&L->rearm); q->conn = id; q->gen = gen; }
            } else {
#ifdef LOOP_DEBUG
                fprintf(stderr, "recv err %d conn %u\n", res, id);
#endif
                conn_kill(L, id);
            }
            return;
        }
        if (!more && !(c->fl & F_PAUSED)) {           /* multishot ended (e.g. CQ overflow): re-arm */
            cref *q = vpush(&L->rearm); q->conn = id; q->gen = gen;
        }
        return;
    }
    case OP_POLL: {                                   /* poll mode: readiness on an idle conn */
        uint32_t id = id_mk(L, uri, slot);
        conn_t *c = C(L, id);
        if (!(c->fl & F_OPEN) || c->gen != gen) return;
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            c->fl &= ~F_RECV;
            /* poll ended: cancelled for backpressure (resume re-arms) or for another reason: re-arm */
            if (!(c->fl & (F_PAUSED | F_EOF | F_DEAD | F_CLOSE_AFTER))) { cref *q = vpush(&L->rearm); q->conn = id; q->gen = gen; }
        }
        if (res > 0 && !(c->fl & (F_PAUSED | F_EOF | F_DEAD))) {
            if (c->fl & F_RECVING) c->fl |= F_AGAIN;
            else issue_recv1(L, id, c);
            /* wakeups coalesce into one CQE: if the peer's FIN came with the data, one recv returns the
             * data and nothing would report the EOF, so read once more */
            if (res & (POLLRDHUP | POLLHUP | POLLERR)) c->fl |= F_AGAIN;
        }
        return;
    }
    case OP_RECV1: {                                  /* poll mode: one-shot recv */
        uint32_t id = id_mk(L, uri, slot);
        conn_t *c = C(L, id);
        int live = (c->fl & F_OPEN) && c->gen == gen;
        if (cqe->flags & IORING_CQE_F_BUFFER) {
            uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            int kept = 0;
            if (live && res > 0)
                kept = conn_data(L, id, r->bufmem + (size_t)bid * L->cfg.buf_size, (uint32_t)res, 1, (uint16_t)ri, bid, 0);
            if (!kept) ur_return_pbuf(L, (uint16_t)ri, bid);
        }
        if (!live) return;
        c->fl &= ~F_RECVING;
        if (res == 0) { conn_eof(L, id); return; }
        if (res < 0) {
            if (res == -EAGAIN || res == -ECANCELED) {
                if ((c->fl & F_AGAIN) && !(c->fl & (F_PAUSED | F_EOF | F_DEAD))) issue_recv1(L, id, c);
            } else if (res == -ENOBUFS) {
                L->st.enobufs++;
                c->fl |= F_AGAIN;                     /* retried from the credit-limited FIFO */
                cref *q = vpush(&L->rearm); q->conn = id; q->gen = gen;
            } else conn_kill(L, id);
            return;
        }
        /* a full buffer means more may be waiting without a new wakeup; so does readiness seen meanwhile */
        if (((uint32_t)res >= L->cfg.buf_size || (c->fl & F_AGAIN)) && !(c->fl & (F_PAUSED | F_EOF | F_DEAD | F_CLOSE_AFTER)))
            issue_recv1(L, id, c);
        return;
    }
    case OP_ACCEPT: {
        int more = cqe->flags & IORING_CQE_F_MORE;
        if (slot) r->singles--;
        if (res >= 0) {
            uint32_t id = id_mk(L, ri, (uint32_t)res);
            conn_open(L, id);
            r->open++;
            arm_recv(L, id, C(L, id));
            if (!slot && more && ring_free(r) <= multi_headroom(L, r))   /* move on before a burst can overflow */
                cancel_ud(L, r, ud_mk(OP_ACCEPT, ri, 0, 0));
        } else if (res == -ENFILE) fprintf(stderr, "loop: ring %u table full (ENFILE) - a connection was dropped\n", ri);
        if (!slot && !more) {
            r->accepting = 0;
            if (L->acc_ring == (int)ri) L->acc_ring = -1;
        }
        return;
    }
    case OP_EVFD:
        L->rings[uri].reap = 1;
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            struct io_uring_sqe *s = get_sqe(L, &L->rings[0]);
            s->opcode = IORING_OP_POLL_ADD;
            s->fd = L->rings[uri].evfd;
            s->poll32_events = POLLIN;
            s->len = IORING_POLL_ADD_MULTI;
            s->user_data = ud_mk(OP_EVFD, uri, 0, 0);
        }
        return;
    default:
        if (gen == GEN_FDINSTALL) {                   /* lingering close: our side's FIN */
            if (res >= 0) { shutdown(res, SHUT_WR); close(res); }
            else {                                    /* < 6.8, or the fd table is full: slow path */
                if (res == -EINVAL || res == -EOPNOTSUPP) L->no_fdinstall = 1;
                ur_shutdown_wr_slow(L, uri, slot);
            }
            return;
        }
        if (slot == 1 && res < 0 && L->rst_ok) {
            fprintf(stderr, "loop: io_uring SO_LINGER failed (%s); shedding with FIN\n", strerror(-res));
            L->rst_ok = 0;
        }
        return;
    }
}

static unsigned reap(loop_t *L, unsigned ri)
{
    ring_t *r = &L->rings[ri];
    unsigned head = *r->cq_head, tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE), n = 0;
    while (head != tail) {
        if (n == L->cfg.max_cqes) { L->more = 1; r->reap = 1; break; }   /* bound one pass; rest next pass */
        handle_cqe(L, ri, &r->cqes[head & r->cq_mask]);
        head++; n++;
        if (head == tail) {   /* handlers may not post, but pick up anything that raced in */
            __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
            tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
        }
    }
    __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
    return n;
}

static int ur_wait_and_reap(loop_t *L)
{
    ring_t *r0 = &L->rings[0];
    /* Under load wait for `batch` completions or wait_us; when idle sleep until one
     * arrives (bounded so timers still run). */
    unsigned min = L->busy ? L->cfg.batch : 1;
    uint64_t wait = L->busy ? (uint64_t)L->cfg.wait_us * 1000 : 250000000ull;
    if (L->ring_wait_poll_active && wait > (uint64_t)L->cfg.poll_wait_us * 1000) wait = (uint64_t)L->cfg.poll_wait_us * 1000;
    if (L->more || L->vin.n || L->ready.n || L->closing.n) min = 0;   /* never sleep on work we already have */
    L->more = 0;
    int ret = ring_enter(L, r0, min, IORING_ENTER_GETEVENTS, wait);
    if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -EBUSY) return ret;
    L->st.wakeups++;
    unsigned n = reap(L, 0);
    for (unsigned k = 1; k < L->nlive; k++) {
        ring_t *r = &L->rings[k];
        if (L->ring_wait_poll_active && (r->open || r->singles || r->accepting)) r->reap = 1;
        if (!r->reap) continue;
        r->reap = 0;
        ring_enter(L, r, 0, IORING_ENTER_GETEVENTS, 0);
        n += reap(L, k);
    }
    /* Poll mode: readiness CQEs queued one-shot recvs. Submit them now, without waiting: a non-blocking
     * recv on a readable socket completes inline, so its data joins this batch. Otherwise the request
     * waits a whole extra pass (up to wait_us). A few rounds, since recvs that fill a buffer chain. */
    static int no_inline = -1;
    if (no_inline < 0) no_inline = getenv("LOOP_TEST_NO_INLINE_RECV") != NULL;   /* A/B measurement hook */
    for (int round = 0; L->pollmode && !no_inline && round < 4; round++) {
        int any = 0;
        for (unsigned k = 0; k < L->nlive; k++) {
            ring_t *r = &L->rings[k];
            if (!r->recv1) continue;
            r->recv1 = 0; any = 1;
            ring_enter(L, r, 0, IORING_ENTER_GETEVENTS, 0);
            n += reap(L, k);
        }
        if (!any) break;
    }
    L->busy = n > 0;
    return 0;
}

static void ur_after_batch(loop_t *L)
{
    /* re-arm terminated recvs (ENOBUFS / CQ overflow) now that buffers went back */
    /* credit-limited FIFO: re-arming more recvs than there are free buffers just produces
     * another round of ENOBUFS */
    cref *q = L->rearm.p;
    uint32_t i = 0, credit = L->cfg.bufs / 2;
    for (; i < L->rearm.n && credit; i++) {
        conn_t *c = C(L, q[i].conn);
        if (!(c->fl & F_OPEN) || c->gen != q[i].gen || (c->fl & (F_PAUSED | F_EOF | F_DEAD))) continue;
        if (!(c->fl & F_RECV)) { arm_recv(L, q[i].conn, c); L->st.rearms++; }
        if (L->pollmode && (c->fl & F_AGAIN) && !(c->fl & F_RECVING)) { issue_recv1(L, q[i].conn, c); L->st.rearms++; credit--; }
        else if (!L->pollmode) credit--;
    }
    if (i) memmove(q, q + i, (L->rearm.n - i) * sizeof *q);
    L->rearm.n -= i;
    if (L->rearm.n) L->more = 1;
    pick_accept_ring(L);
    for (unsigned k = 0; k < L->nlive; k++) {
        ring_t *r = &L->rings[k];
        if (r->br_dirty && !r->legacy) {
            __atomic_store_n(&r->br->tail, r->br_tail, __ATOMIC_RELEASE);
            r->br_dirty = 0;
        }
        /* ring 0 submits with its wait; others submit now (and run their task work) */
        if (k && r->to_submit) { ring_enter(L, r, 0, IORING_ENTER_GETEVENTS, 0); reap(L, k); }
    }
}

static int ring_init(loop_t *L, ring_t *r, unsigned idx, unsigned slots)
{
    struct io_uring_params p;
    unsigned base = IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL;
    unsigned tries[3] = {
        L->cfg.coop_taskrun ? (IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG)
                            : (IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN),
        IORING_SETUP_COOP_TASKRUN, 0 };
    int fd = -1;
    for (int t = 0; t < 3 && fd < 0; t++) {
        memset(&p, 0, sizeof p);
        p.flags = base | tries[t];
        p.cq_entries = L->cfg.sq_entries * 4;
        fd = sys_setup(L->cfg.sq_entries, &p);
        if (fd < 0 && t == 0) fprintf(stderr, "loop: ring setup flags %#x failed (%s), falling back\n", p.flags, strerror(errno));
    }
    if (fd < 0) return -errno;
    if (!(p.features & IORING_FEAT_EXT_ARG) || !(p.features & IORING_FEAT_SINGLE_MMAP)) { close(fd); return -ENOSYS; }
    r->fd = fd;
    r->sq_entries = p.sq_entries;
    r->sq_map_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cqsz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (cqsz > r->sq_map_sz) r->sq_map_sz = cqsz;
    r->sq_map = mmap(NULL, r->sq_map_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (r->sq_map == MAP_FAILED) return -errno;
    r->cq_map = r->sq_map;
    r->sqe_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqe_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) return -errno;
    char *sq = r->sq_map, *cq = r->cq_map;
    r->sq_head = (unsigned *)(sq + p.sq_off.head);
    r->sq_tail = (unsigned *)(sq + p.sq_off.tail);
    r->sq_flags = (unsigned *)(sq + p.sq_off.flags);
    r->sq_mask = *(unsigned *)(sq + p.sq_off.ring_mask);
    unsigned *arr = (unsigned *)(sq + p.sq_off.array);
    for (unsigned i = 0; i < p.sq_entries; i++) arr[i] = i;
    r->cq_head = (unsigned *)(cq + p.cq_off.head);
    r->cq_tail = (unsigned *)(cq + p.cq_off.tail);
    r->cq_flags = (unsigned *)(cq + p.cq_off.flags);
    r->cq_mask = *(unsigned *)(cq + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);
    r->sq_ltail = *r->sq_tail;

    struct io_uring_rsrc_update up = { .offset = -1U, .data = (uint64_t)fd };
    if (sys_register(fd, IORING_REGISTER_RING_FDS, &up, 1) != 1) return -errno;
    {   /* bound io-wq (SHUTDOWN, and anything else that punts): 1 bounded + 1 unbounded worker */
        unsigned w[2] = { 1, 1 };
        sys_register(fd, IORING_REGISTER_IOWQ_MAX_WORKERS, w, 2);
    }
    r->ridx = (int)up.offset;

    struct io_uring_rsrc_register rr = { .nr = slots, .flags = IORING_RSRC_REGISTER_SPARSE };
    if (sys_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr) < 0) return -errno;
    r->cap = slots;

    /* provided-buffer ring, group 0 */
    unsigned nb = L->cfg.bufs;
    r->br_sz = ((size_t)nb * sizeof(struct io_uring_buf) + 4095) & ~(size_t)4095;
    r->br = mmap(NULL, r->br_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->br == MAP_FAILED) return -errno;
    struct io_uring_buf_reg reg = { .ring_addr = (uint64_t)(uintptr_t)r->br, .ring_entries = nb, .bgid = 0 };
    if (L->cfg.legacy_bufs || sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) {
        if (!L->cfg.legacy_bufs) fprintf(stderr, "loop: PBUF_RING failed (%s), using PROVIDE_BUFFERS\n", strerror(errno));
        r->legacy = 1;
    }
    /* +1 page after the last buffer: LOOP_SLACK over-read room (buffers are contiguous) */
    r->bufmem = mmap(NULL, (size_t)nb * L->cfg.buf_size + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->bufmem == MAP_FAILED) return -errno;
#ifdef MADV_HUGEPAGE
    if (L->cfg.hugepages) madvise(r->bufmem, (size_t)nb * L->cfg.buf_size, MADV_HUGEPAGE);
#endif
    if (r->legacy) {
        struct io_uring_sqe *s = get_sqe(L, r);
        s->opcode = IORING_OP_PROVIDE_BUFFERS;
        s->fd = (int)nb; s->addr = (uint64_t)(uintptr_t)r->bufmem; s->len = L->cfg.buf_size; s->off = 0;
        s->buf_group = 0; s->user_data = ud_mk(OP_IGN, 0, 0, 0);
        if (ring_enter(L, r, 1, IORING_ENTER_GETEVENTS, 0) < 0) return -EIO;
        reap(L, idx);
    } else {
        for (unsigned b = 0; b < nb; b++) ur_return_pbuf(L, (uint16_t)idx, (uint16_t)b);
        __atomic_store_n(&r->br->tail, r->br_tail, __ATOMIC_RELEASE);
    }
    r->br_dirty = 0;

    r->evfd = -1;
    if (idx > 0 && L->cfg.ring_wait_evfd) {
        r->evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (r->evfd < 0 || sys_register(fd, IORING_REGISTER_EVENTFD, &r->evfd, 1) < 0) return -errno;
        struct io_uring_sqe *s = get_sqe(L, &L->rings[0]);
        s->opcode = IORING_OP_POLL_ADD;
        s->fd = r->evfd;
        s->poll32_events = POLLIN;
        s->len = IORING_POLL_ADD_MULTI;
        s->user_data = ud_mk(OP_EVFD, idx, 0, 0);
    }
    return 0;
}

static int ur_init(loop_t *L)
{
    if ((L->cfg.bufs & (L->cfg.bufs - 1)) || L->cfg.bufs > 32768) return -EINVAL;
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
    /* the kernel caps each ring's file table at RLIMIT_NOFILE (and 1<<20) */
    unsigned per = rl.rlim_cur > (1u << 20) ? (1u << 20) : (unsigned)rl.rlim_cur;
    if (per > 65535 && L->cfg.max_conns <= 65535) per = L->cfg.max_conns;
    unsigned nr = L->cfg.rings ? L->cfg.rings : (L->cfg.max_conns + per - 1) / per;
    if (nr < 1) nr = 1;
    if (nr > 31) nr = 31;
    L->slot_bits = 1;
    while ((1u << L->slot_bits) < per) L->slot_bits++;
    if (L->slot_bits > 24 || L->slot_bits + 5 > 32) return -EINVAL;
    L->nrings = nr;
    L->acc_ring = -1;
    L->ring_wait_poll_active = 0;            /* set once a second ring exists */
    L->pollmode = L->cfg.recv_mode == LOOP_RECV_POLL && !L->cfg.rx_timestamps;
    L->rx_msg.msg_controllen = CMSG_SPACE(sizeof(struct timespec));
    int r = alloc_table(L, (size_t)nr << L->slot_bits);
    if (r) return r;
    L->rings = calloc(nr, sizeof(ring_t));
    L->ring_per = per;
    /* ring 0 now; the others when the live ones fill up (pick_accept_ring) */
    r = ring_init(L, &L->rings[0], 0, per);
    if (r < 0) { fprintf(stderr, "loop: ring 0 init: %s\n", strerror(-r)); return r; }
    L->nlive = 1;
    return 0;
}

static void ur_free(loop_t *L)
{
    for (unsigned k = 0; k < L->nrings; k++) {
        ring_t *r = &L->rings[k];
        if (r->sq_map && r->sq_map != MAP_FAILED) munmap(r->sq_map, r->sq_map_sz);
        if (r->sqes && r->sqes != MAP_FAILED) munmap(r->sqes, r->sqe_sz);
        if (r->bufmem && r->bufmem != MAP_FAILED) munmap(r->bufmem, (size_t)L->cfg.bufs * L->cfg.buf_size + 4096);
        if (r->br && r->br != MAP_FAILED) munmap(r->br, r->br_sz);
        if (r->evfd > 0) close(r->evfd);
        if (r->fd > 0) close(r->fd);
    }
    free(L->rings);
}


/* ================================================================== epoll backend (Linux fallback)
 * Used when io_uring is unavailable (ENOSYS under Rosetta, io_uring_disabled, seccomp) or forced with
 * loop_cfg.force_epoll / CURSED_EPOLL=1. Same contract as io_uring; conn id = fd, so it is bounded by
 * RLIMIT_NOFILE. Level-triggered, reads into a per-pass arena (zero-copy entries), writes directly. */

#define EP_EV 1024
#define EP_LISTEN 0xffffffffffffffffull

static void ep_update(loop_t *L, uint32_t id)
{
    conn_t *c = C(L, id);
    struct epoll_event ev = { .events = ((c->fl & (F_PAUSED | F_EOF)) ? 0 : (EPOLLIN | EPOLLRDHUP)) | (L->kinfl[id] ? EPOLLOUT : 0),
                              .data.u64 = (uint64_t)c->gen << 32 | id };
    epoll_ctl(L->ep, EPOLL_CTL_MOD, (int)id, &ev);
}

static void ep_listen(loop_t *L, int on)
{
    struct epoll_event ev = { .events = on ? EPOLLIN : 0, .data.u64 = EP_LISTEN };
    epoll_ctl(L->ep, EPOLL_CTL_MOD, L->lfd, &ev);
    L->ep_listen_on = on;
}

static void ep_accept_ctl(loop_t *L, int on)
{
    L->ep_emfile = 0;
    ep_listen(L, on);
}

static void ep_try_write(loop_t *L, uint32_t id, buf_t *sb)
{
    while (sb->off < sb->len) {
        ssize_t w = send((int)id, sb->d + sb->off, sb->len - sb->off, MSG_NOSIGNAL);
        if (w > 0) { sb->off += (uint32_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            L->st.partial_sends++;
            if (!L->kinfl[id]) { L->kinfl[id] = sb; ep_update(L, id); }
            return;
        }
        int had = L->kinfl[id] != NULL;
        L->kinfl[id] = NULL;
        if (had) ep_update(L, id);
        send_done(L, sb, 1);
        return;
    }
    int had = L->kinfl[id] != NULL;
    L->kinfl[id] = NULL;
    if (had) ep_update(L, id);
    send_done(L, sb, 0);
}

static void ep_send(loop_t *L, uint32_t id, conn_t *c, buf_t *sb)
{
    (void)c;
    L->st.send_bytes += sb->len;
    ep_try_write(L, id, sb);
}

static void ep_close(loop_t *L, uint32_t id, conn_t *c)
{
    buf_t *sb = L->kinfl[id];
    if (sb) { L->kinfl[id] = NULL; sb->done = 1; buf_put(L, sb); }
    if (c->fl & F_RST) { struct linger lg = { 1, 0 }; setsockopt((int)id, SOL_SOCKET, SO_LINGER, &lg, sizeof lg); L->st.rst++; }
    close((int)id);                         /* also drops it from the epoll set */
    if (L->ep_emfile) { L->ep_emfile = 0; ep_listen(L, 1); }   /* a descriptor is free again */
}

static void ep_pause(loop_t *L, uint32_t id, conn_t *c) { (void)c; ep_update(L, id); }
static void ep_shutdown_wr(loop_t *L, uint32_t id, conn_t *c) { (void)L; (void)c; shutdown((int)id, SHUT_WR); }
static void ep_resume(loop_t *L, uint32_t id, conn_t *c) { (void)c; ep_update(L, id); }

static void ep_accept(loop_t *L)
{
    for (int i = 0; i < 256; i++) {
        int fd = accept4(L->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            /* out of descriptors: stop polling the listener until a close frees one (no busy loop) */
            if ((errno == EMFILE || errno == ENFILE) && L->ep_listen_on) { ep_listen(L, 0); L->ep_emfile = 1; }
            return;
        }
        if ((size_t)fd >= L->tab_n) { close(fd); continue; }
        conn_open(L, (uint32_t)fd);
        struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.u64 = (uint64_t)C(L, (uint32_t)fd)->gen << 32 | (uint32_t)fd };
        if (epoll_ctl(L->ep, EPOLL_CTL_ADD, fd, &ev) < 0) conn_kill(L, (uint32_t)fd);
    }
}

static int ep_wait_and_reap(loop_t *L)
{
    struct epoll_event ev[EP_EV];
    int busy = L->ready.n || L->closing.n || L->fop_done.n;
    int n = epoll_wait(L->ep, ev, EP_EV, busy ? 0 : 250);
    if (n < 0) return errno == EINTR ? 0 : -errno;
    L->st.wakeups++; L->st.enters++;
    L->arena_used = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].data.u64 == EP_LISTEN) { ep_accept(L); continue; }
        uint32_t id = (uint32_t)ev[i].data.u64, gen = (uint32_t)(ev[i].data.u64 >> 32);
        conn_t *c = C(L, id);
        if (!(c->fl & F_OPEN) || c->gen != gen) continue;              /* stale: fd closed and reused */
        uint32_t e = ev[i].events;
        if ((e & EPOLLOUT) && L->kinfl[id]) ep_try_write(L, id, L->kinfl[id]);
        if (!(e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) || (c->fl & (F_DEAD | F_PAUSED | F_EOF))) continue;
        int fd = (int)id;
        for (;;) {
            if (L->arena_cap - L->arena_used < 4096) break;   /* arena full: level-triggered, retry next round */
            char *p = L->arena + L->arena_used;
            size_t room = L->arena_cap - L->arena_used;
            if (room > 65536) room = 65536;
            ssize_t r = recv(fd, p, room, 0);
            if (r > 0) {
                L->arena_used += (size_t)r;
                conn_data(L, id, p, (uint32_t)r, 2, 0, 0, 0);
                if ((size_t)r < room) break;
                continue;
            }
            if (r == 0) { conn_eof(L, id); if (!(c->fl & F_DEAD)) ep_update(L, id); break; }
            if (errno == EAGAIN || errno == EINTR) break;
            conn_kill(L, id);
            break;
        }
    }
    return 0;
}

static void ep_after_batch(loop_t *L) { (void)L; }

static int ep_init(loop_t *L)
{
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
    L->ep = epoll_create1(EPOLL_CLOEXEC);
    if (L->ep < 0) return -errno;
    fcntl(L->lfd, F_SETFL, fcntl(L->lfd, F_GETFL) | O_NONBLOCK);
    struct epoll_event ev = { .events = 0, .data.u64 = EP_LISTEN };
    if (epoll_ctl(L->ep, EPOLL_CTL_ADD, L->lfd, &ev) < 0) return -errno;
    size_t n = rl.rlim_cur > (1u << 20) ? (1u << 20) : rl.rlim_cur;
    int r = alloc_table(L, n);
    if (r) return r;
    L->kinfl = calloc(n, sizeof(buf_t *));
    L->arena_cap = 1 << 20;
    L->arena = malloc(L->arena_cap + LOOP_SLACK);
    return L->kinfl && L->arena ? 0 : -ENOMEM;
}

static void ep_free(loop_t *L)
{
    if (L->kinfl) for (size_t i = 0; i < L->tab_n; i++) if (L->kinfl[i]) free(L->kinfl[i]);
    free(L->kinfl); free(L->arena);
    if (L->ep > 0) close(L->ep);
}

/* ------------------------------------------------------------------ Linux backend dispatch */

static int be_init(loop_t *L)
{
    const char *env = getenv("CURSED_EPOLL");
    if (!L->cfg.force_epoll && !(env && *env == '1')) {
        int r = ur_init(L);
        if (r == 0) return 0;
        fprintf(stderr, "loop: io_uring unavailable (%s), falling back to epoll\n", strerror(-r));
        ur_free(L);
        L->rings = NULL; L->nrings = 0;
        if (L->tab) { munmap(L->tab, L->tab_bytes); L->tab = NULL; }
    }
    L->epoll = 1;
    return ep_init(L);
}
static void be_free(loop_t *L) { if (L->epoll) ep_free(L); else ur_free(L); }
static void be_send(loop_t *L, uint32_t id, conn_t *c, buf_t *sb) { if (L->epoll) ep_send(L, id, c, sb); else ur_send(L, id, c, sb); }
static void be_close(loop_t *L, uint32_t id, conn_t *c) { if (L->epoll) ep_close(L, id, c); else ur_close(L, id, c); }
static void be_pause(loop_t *L, uint32_t id, conn_t *c) { if (L->epoll) ep_pause(L, id, c); else ur_pause(L, id, c); }
static void be_resume(loop_t *L, uint32_t id, conn_t *c) { if (L->epoll) ep_resume(L, id, c); else ur_resume(L, id, c); }
static int  be_wait_and_reap(loop_t *L) { return L->epoll ? ep_wait_and_reap(L) : ur_wait_and_reap(L); }
static void be_after_batch(loop_t *L) { if (L->epoll) ep_after_batch(L); else ur_after_batch(L); }
static void be_accept_ctl(loop_t *L, int on) { if (L->epoll) ep_accept_ctl(L, on); else ur_accept_ctl(L, on); }
static void be_shutdown_wr(loop_t *L, uint32_t id, conn_t *c) { if (L->epoll) ep_shutdown_wr(L, id, c); else ur_shutdown_wr(L, id, c); }
static void return_pbuf(loop_t *L, uint16_t ring, uint16_t bid) { if (!L->epoll) ur_return_pbuf(L, ring, bid); }

const char *loop_backend(loop_t *L) { return L->epoll ? "epoll" : "io_uring"; }

int loop_file_submit(loop_t *L, const loop_fop *ops, int n)
{
    if (n <= 0) return 0;
    if (!L->epoll) return ur_file_submit(L, ops, n);
    sync_fops(L, ops, n);
    return 0;
}


/* ================================================================== kqueue backend */
#else

#define KQ_EV 1024

static void kchg(loop_t *L, int fd, int filt, int flags, void *udata)
{
    struct kevent *k = vpush(&L->kchg);
    EV_SET(k, fd, filt, flags, 0, 0, udata);
}

static void return_pbuf(loop_t *L, uint16_t ring, uint16_t bid) { (void)L; (void)ring; (void)bid; }

static void be_accept_ctl(loop_t *L, int on)
{
    kchg(L, L->lfd, EVFILT_READ, on ? (EV_ADD | EV_ENABLE) : EV_DISABLE, NULL);
}

static void kq_try_write(loop_t *L, uint32_t id, buf_t *sb)
{
    while (sb->off < sb->len) {
        ssize_t w = write((int)id, sb->d + sb->off, sb->len - sb->off);
        if (w > 0) { sb->off += (uint32_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            L->st.partial_sends++;
            L->kinfl[id] = sb;
            kchg(L, (int)id, EVFILT_WRITE, EV_ADD | EV_ONESHOT, (void *)(uintptr_t)sb->gen);
            return;
        }
        L->kinfl[id] = NULL;
        send_done(L, sb, 1);
        return;
    }
    L->kinfl[id] = NULL;
    send_done(L, sb, 0);
}

static void be_send(loop_t *L, uint32_t id, conn_t *c, buf_t *sb)
{
    (void)c;
    L->st.send_bytes += sb->len;
    kq_try_write(L, id, sb);
}

static void be_close(loop_t *L, uint32_t id, conn_t *c)
{
    buf_t *sb = L->kinfl[id];
    if (sb) { L->kinfl[id] = NULL; sb->done = 1; buf_put(L, sb); }
    if (c->fl & F_RST) { struct linger lg = { 1, 0 }; setsockopt((int)id, SOL_SOCKET, SO_LINGER, &lg, sizeof lg); L->st.rst++; }
    close((int)id);                         /* removes its kevents */
    if (L->mem_state == 0) kchg(L, L->lfd, EVFILT_READ, EV_ENABLE, NULL);
}

static void be_pause(loop_t *L, uint32_t id, conn_t *c) { (void)c; kchg(L, (int)id, EVFILT_READ, EV_DISABLE, NULL); }
static void be_shutdown_wr(loop_t *L, uint32_t id, conn_t *c) { (void)L; (void)c; shutdown((int)id, SHUT_WR); }
static void be_resume(loop_t *L, uint32_t id, conn_t *c)
{
    kchg(L, (int)id, EVFILT_READ, EV_ENABLE, (void *)(uintptr_t)c->gen);
}

static void kq_accept(loop_t *L)
{
    for (int i = 0; i < 256; i++) {
        int fd = accept(L->lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) kchg(L, L->lfd, EVFILT_READ, EV_DISABLE, NULL);
            return;
        }
        if ((size_t)fd >= L->tab_n) { close(fd); continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        if (L->cfg.nodelay > 0) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        conn_open(L, (uint32_t)fd);
        kchg(L, fd, EVFILT_READ, EV_ADD, (void *)(uintptr_t)C(L, (uint32_t)fd)->gen);
    }
}

static int be_wait_and_reap(loop_t *L)
{
    struct kevent ev[KQ_EV];
    struct timespec ts = { 0, 250000000 }, zero = { 0, 0 };
    int busy = L->ready.n || L->closing.n || L->fop_done.n;
    int n = kevent(L->kq, L->kchg.p, (int)L->kchg.n, ev, KQ_EV, busy ? &zero : &ts);
    L->kchg.n = 0;
    if (n < 0) return errno == EINTR ? 0 : -errno;
    L->st.wakeups++; L->st.enters++;
    L->arena_used = 0;
    for (int i = 0; i < n; i++) {
        int fd = (int)ev[i].ident;
        if (fd == L->lfd) { kq_accept(L); continue; }
        uint32_t id = (uint32_t)fd;
        conn_t *c = C(L, id);
        uint32_t gen = (uint32_t)(uintptr_t)ev[i].udata;
        if (!(c->fl & F_OPEN) || c->gen != gen) continue;
        if (ev[i].filter == EVFILT_WRITE) {
            buf_t *sb = L->kinfl[id];
            if (sb) kq_try_write(L, id, sb);
            continue;
        }
        if (c->fl & F_DEAD) continue;
        for (;;) {
            if (L->arena_cap - L->arena_used < 4096) break;   /* arena full: level-triggered, retry next round */
            char *p = L->arena + L->arena_used;
            size_t room = L->arena_cap - L->arena_used;
            if (room > 65536) room = 65536;
            ssize_t r = read(fd, p, room);
            if (r > 0) {
                L->arena_used += (size_t)r;
                conn_data(L, id, p, (uint32_t)r, 2, 0, 0, 0);
                if ((size_t)r < room) break;
                continue;
            }
            if (r == 0) { conn_eof(L, id); if (!(c->fl & F_DEAD)) kchg(L, fd, EVFILT_READ, EV_DISABLE, NULL); break; }
            if (errno == EAGAIN || errno == EINTR) break;
            conn_kill(L, id);
            break;
        }
    }
    return 0;
}

static void be_after_batch(loop_t *L) { (void)L; }

static int be_init(loop_t *L)
{
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rlim_t want = rl.rlim_max;
    if (want > 1u << 20) want = 1u << 20;
    rl.rlim_cur = want;
    while (setrlimit(RLIMIT_NOFILE, &rl) < 0 && rl.rlim_cur > 1024) rl.rlim_cur /= 2;
    getrlimit(RLIMIT_NOFILE, &rl);
    L->kq = kqueue();
    if (L->kq < 0) return -errno;
    fcntl(L->lfd, F_SETFL, fcntl(L->lfd, F_GETFL) | O_NONBLOCK);
    L->kchg.sz = sizeof(struct kevent);
    size_t n = rl.rlim_cur;
    int r = alloc_table(L, n);
    if (r) return r;
    L->kinfl = calloc(n, sizeof(buf_t *));
    L->arena_cap = 1 << 20;
    L->arena = malloc(L->arena_cap + LOOP_SLACK);
    return 0;
}

static void be_free(loop_t *L)
{
    for (size_t i = 0; i < L->tab_n; i++) if (L->kinfl[i]) free(L->kinfl[i]);
    free(L->kinfl); free(L->arena); free(L->kchg.p);
    close(L->kq);
}

const char *loop_backend(loop_t *L) { (void)L; return "kqueue"; }

int loop_file_submit(loop_t *L, const loop_fop *ops, int n)
{
    if (n > 0) sync_fops(L, ops, n);
    return 0;
}
#endif
