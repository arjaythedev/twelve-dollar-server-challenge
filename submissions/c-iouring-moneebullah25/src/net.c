#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "util.h"

#define NONE32             UINT32_MAX
#define NONE16             UINT16_MAX
#define MAX_RINGS          16
#define SLOTS_PER_RING_MAX 65535
#define ACCEPT_RESERVE     4096 /* free slots per ring below which accept switches to one-shots (capped at slots/4) */
#define ONESHOT_ACCEPTS    64   /* one-shot accepts kept in flight near a full ring */
#define SMALL_RING         1024 /* tables this small never use multishot accept */
#define GEN_MASK           0xffffffu
#define SWEEP_INTERVAL_NS  10000000000LL
#define GUARD_INTERVAL_NS  250000000LL
#define SHED_PER_TICK      2000
#define LISTEN_BACKLOG     4096

/* user_data: type:8 | generation:24 | index:32 */
enum op {
    OP_ACCEPT = 1, /* multishot */
    OP_ACCEPT1,    /* one-shot */
    OP_RECV,
    OP_SEND,
    OP_SEND100,
    OP_SHUTDOWN,
    OP_CLOSE,
    OP_CANCEL,
    OP_RINGPOLL,
    OP_PROVIDE
};

static inline uint64_t ud(enum op op, uint32_t gen, uint32_t idx) {
    return (uint64_t)op << 56 | (uint64_t)(gen & GEN_MASK) << 32 | idx;
}
static inline enum op ud_op(uint64_t u) {
    return (enum op)(u >> 56);
}
static inline uint32_t ud_gen(uint64_t u) {
    return (uint32_t)(u >> 32) & GEN_MASK;
}
static inline uint32_t ud_idx(uint64_t u) {
    return (uint32_t)u;
}

enum conn_state { ST_FREE = 0, ST_OPEN, ST_CLOSING };

enum conn_flag {
    CF_RECV_ARMED = 1 << 0, /* a multishot receive is outstanding */
    CF_SEND_INFLIGHT = 1 << 1,
    CF_HAS_INPUT = 1 << 2, /* in_bid or partial holds unprocessed bytes */
    CF_IN_QUEUE = 1 << 3,  /* queued in the ready FIFO */
    CF_CLOSE_AFTER_SEND = 1 << 4,
    CF_IN_REARM = 1 << 5, /* queued in the ENOBUFS re-arm FIFO */
    CF_HELD = 1 << 6,     /* parked until writes are released */
    CF_WAIT_OUT = 1 << 7, /* parked until a send buffer is free */
};

struct conn {
    uint32_t gen;
    uint8_t state;
    uint8_t flags;
    uint16_t ring;
    uint32_t slot;
    uint32_t last_active; /* loop seconds */
    uint32_t out_chunk;
    uint16_t in_bid;      /* parked receive buffer, or NONE16 */
    uint16_t in_off;      /* parse offset into it */
    uint16_t in_len;      /* bytes in it */
    uint16_t partial_len; /* bytes in `partial` */
    char *partial;        /* heap copy of an incomplete request (rare) */
    uint32_t partial_cap;
    uint32_t sent_100; /* a 100 Continue went out for the current request */
};

struct chunk_meta {
    uint32_t conn;
    uint32_t gen;
    uint32_t off;
    uint32_t len;
    uint32_t sent;
};

struct fifo {
    uint32_t *v;
    uint32_t head, tail, cap; /* cap is a power of two */
};

struct ring {
    struct io_uring r;
    uint32_t idx;
    uint32_t slots;
    uint32_t used;
    char *bufs;
    uint64_t *ret_bitmap; /* buffer ids returned this iteration */
    uint32_t ret_n;
    uint32_t returned_this_iter;
    struct fifo rearm;
    uint8_t accept_armed;   /* a multishot accept is outstanding */
    uint8_t cancel_pending; /* its cancellation was issued */
    uint8_t poll_armed;
    uint8_t needs_reap;
    uint32_t oneshots; /* one-shot accepts outstanding */
};

struct net {
    const struct config *cfg;
    struct app *app;
    struct db *db;
    int listen_fd;
    struct ring rings[MAX_RINGS];
    uint32_t nrings;
    uint32_t slots_per_ring;
    struct conn *conns;
    uint32_t nconns;
    uint32_t open_conns;

    char *chunks;
    struct chunk_meta *chunk_meta;
    uint32_t *chunk_free;
    uint32_t nchunk_free;

    struct fifo ready, wait_out, held;

    /* per-iteration request bookkeeping */
    struct app_resp *resps;
    uint32_t *resp_conn;
    uint32_t nresps;

    int64_t start_ns;
    uint32_t now_s;
    int acc_ring; /* the ring accepting right now, or -1 */
    int accept_paused;
    int shedding;
    uint32_t shed_cursor;
    int64_t next_guard_ns, next_sweep_ns, next_stats_ns;
    uint64_t cg_limit_bytes; /* 0 when no cgroup memory limit applies */
    char cg_current_path[256];
    unsigned last_cqes;

    struct {
        uint64_t accepts, closes, requests, iterations, wakeups, cqes, enobufs, rearms, short_sends, send_errors,
            bad_requests, too_large, partial_timeouts, shed, accept_errors, mem_pauses, buffers_returned, provide_sqes;
        uint64_t max_open;
        uint64_t iter_ns_max, iters_over_20ms, commit_ns_max, batch_ns_max;
        uint32_t batch_max;
    } st;
};

static volatile sig_atomic_t stop_requested;
static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

/* ------------------------------------------------------------------ fifo */

static int fifo_init(struct fifo *f, uint32_t min_cap) {
    uint32_t cap = 1024;
    while (cap < min_cap + 1) cap <<= 1;
    f->v = malloc((size_t)cap * sizeof *f->v);
    f->head = f->tail = 0;
    f->cap = cap;
    return f->v ? 0 : -1;
}
static inline int fifo_empty(const struct fifo *f) {
    return f->head == f->tail;
}
static inline uint32_t fifo_len(const struct fifo *f) {
    return f->tail - f->head;
}
static inline int fifo_push(struct fifo *f, uint32_t v) {
    if (f->tail - f->head >= f->cap) return -1;
    f->v[f->tail++ & (f->cap - 1)] = v;
    return 0;
}
static inline uint32_t fifo_pop(struct fifo *f) {
    return f->v[f->head++ & (f->cap - 1)];
}

/* ------------------------------------------------------------------ sqe helpers */

static struct io_uring_sqe *sqe_get(struct ring *rg) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&rg->r);
    if (!sqe) {
        int rc = io_uring_submit(&rg->r);
        if (rc < 0 && rc != -EBUSY) log_msg("io_uring_submit (full sq): %s", strerror(-rc));
        sqe = io_uring_get_sqe(&rg->r);
        if (!sqe) {
            log_msg("fatal: no sqe after submit");
            abort();
        }
    }
    return sqe;
}

static inline struct conn *conn_at(struct net *n, uint32_t idx) {
    return &n->conns[idx];
}
static inline struct ring *ring_of(struct net *n, const struct conn *c) {
    return &n->rings[c->ring];
}
static inline uint32_t conn_index(struct net *n, uint32_t ring, uint32_t slot) {
    return ring * n->slots_per_ring + slot;
}
static inline char *recv_buf(struct net *n, struct ring *rg, uint16_t bid) {
    return rg->bufs + (size_t)bid * n->cfg->recv_buf_size;
}

static void buf_return(struct net *n, struct ring *rg, uint16_t bid) {
    rg->ret_bitmap[bid >> 6] |= 1ull << (bid & 63);
    rg->ret_n++;
    rg->returned_this_iter++;
    n->st.buffers_returned++;
}

static void provide_run(struct net *n, struct ring *rg, uint32_t start, uint32_t len) {
    struct io_uring_sqe *sqe = sqe_get(rg);
    io_uring_prep_provide_buffers(sqe, recv_buf(n, rg, (uint16_t)start), (int)n->cfg->recv_buf_size, (int)len, 0,
                                  (int)start);
    sqe->user_data = ud(OP_PROVIDE, 0, rg->idx);
    n->st.provide_sqes++;
}

/* Gives buffers back to the kernel: runs of consecutive ids become one SQE. */
static void flush_returned_buffers(struct net *n, struct ring *rg) {
    if (!rg->ret_n) return;
    uint32_t words = (n->cfg->recv_bufs + 63) / 64;
    uint32_t run_start = 0, run_len = 0;
    for (uint32_t w = 0; w < words; w++) {
        uint64_t bits = rg->ret_bitmap[w];
        if (!bits && !run_len) continue;
        rg->ret_bitmap[w] = 0;
        for (uint32_t b = 0; b < 64; b++) {
            uint32_t bid = w * 64 + b;
            if (bits & (1ull << b)) {
                if (!run_len) run_start = bid;
                run_len++;
            } else if (run_len) {
                provide_run(n, rg, run_start, run_len);
                run_len = 0;
            }
        }
    }
    if (run_len) provide_run(n, rg, run_start, run_len);
    rg->ret_n = 0;
}

static void arm_recv(struct net *n, uint32_t idx) {
    struct conn *c = conn_at(n, idx);
    struct ring *rg = ring_of(n, c);
    struct io_uring_sqe *sqe = sqe_get(rg);
    io_uring_prep_recv_multishot(sqe, (int)c->slot, NULL, 0, 0);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->buf_group = 0;
    sqe->user_data = ud(OP_RECV, c->gen, idx);
    c->flags |= CF_RECV_ARMED;
}

static void arm_accept(struct net *n, struct ring *rg) {
    struct io_uring_sqe *sqe = sqe_get(rg);
    io_uring_prep_multishot_accept_direct(sqe, n->listen_fd, NULL, NULL, 0);
    sqe->user_data = ud(OP_ACCEPT, 0, rg->idx);
    rg->accept_armed = 1;
    rg->cancel_pending = 0;
}

static void arm_accept_oneshot(struct net *n, struct ring *rg) {
    struct io_uring_sqe *sqe = sqe_get(rg);
    io_uring_prep_accept_direct(sqe, n->listen_fd, NULL, NULL, 0, IORING_FILE_INDEX_ALLOC);
    sqe->user_data = ud(OP_ACCEPT1, 0, rg->idx);
    rg->oneshots++;
}

static void cancel_accept(struct net *n, struct ring *rg) {
    struct io_uring_sqe *sqe = sqe_get(rg);
    io_uring_prep_cancel64(sqe, ud(OP_ACCEPT, 0, rg->idx), 0);
    sqe->user_data = ud(OP_CANCEL, 0, rg->idx);
    rg->cancel_pending = 1;
}

static inline uint32_t ring_free(const struct ring *rg) {
    return rg->slots - rg->used;
}

static void arm_ring_poll(struct net *n, struct ring *rg) {
    struct io_uring_sqe *sqe = sqe_get(&n->rings[0]);
    io_uring_prep_poll_multishot(sqe, rg->r.ring_fd, POLLIN);
    sqe->user_data = ud(OP_RINGPOLL, 0, rg->idx);
    rg->poll_armed = 1;
}

/* Exactly one ring accepts at a time. While it has more than `reserve` free
 * slots it runs a multishot accept. A multishot accept that finds the table
 * full fails with -ENFILE *after* accepting the socket, which the kernel then
 * drops, and it can drain a deep backlog faster than a cancel lands. So near
 * the end of a table the ring switches to one-shot accepts, never more in
 * flight than free slots, and the next ring takes over when it is full. */
static void update_accept(struct net *n) {
    /* Small tables (tests) use one-shots only: a burst can fill them before a
     * cancel lands. Large ones keep multishot until the last `reserve` slots. */
    uint32_t reserve = n->slots_per_ring <= SMALL_RING          ? n->slots_per_ring
                       : n->slots_per_ring / 4 < ACCEPT_RESERVE ? n->slots_per_ring / 4
                                                                : ACCEPT_RESERVE;
    if (n->acc_ring >= 0) {
        struct ring *rg = &n->rings[n->acc_ring];
        if (rg->accept_armed) {
            if ((n->accept_paused || ring_free(rg) <= reserve) && !rg->cancel_pending) cancel_accept(n, rg);
            return; /* the accept's final completion re-runs this */
        }
        if (rg->oneshots) return; /* let them land first */
        n->acc_ring = -1;
    }
    if (n->accept_paused) return;
    uint32_t best = 0, best_i = 0;
    for (uint32_t i = 0; i < n->nrings; i++) {
        if (ring_free(&n->rings[i]) > best) {
            best = ring_free(&n->rings[i]);
            best_i = i;
        }
    }
    if (best == 0) return; /* every table is full: clients wait in the backlog */
    n->acc_ring = (int)best_i;
    struct ring *rg = &n->rings[best_i];
    if (best > reserve) {
        arm_accept(n, rg);
    } else {
        uint32_t want = best < ONESHOT_ACCEPTS ? best : ONESHOT_ACCEPTS;
        while (rg->oneshots < want) arm_accept_oneshot(n, rg);
    }
}

/* ------------------------------------------------------------------ chunks */

static uint32_t chunk_alloc(struct net *n, uint32_t conn_idx) {
    if (!n->nchunk_free) return NONE32;
    uint32_t id = n->chunk_free[--n->nchunk_free];
    struct chunk_meta *m = &n->chunk_meta[id];
    m->conn = conn_idx;
    m->gen = conn_at(n, conn_idx)->gen;
    m->off = m->len = m->sent = 0;
    return id;
}

static void chunk_free(struct net *n, uint32_t id) {
    n->chunk_free[n->nchunk_free++] = id;
    /* a parked connection can continue now */
    while (!fifo_empty(&n->wait_out)) {
        uint32_t idx = fifo_pop(&n->wait_out);
        struct conn *c = conn_at(n, idx);
        if (!(c->flags & CF_WAIT_OUT)) continue;
        c->flags &= (uint8_t)~CF_WAIT_OUT;
        if (c->state == ST_OPEN && !(c->flags & CF_IN_QUEUE)) {
            c->flags |= CF_IN_QUEUE;
            fifo_push(&n->ready, idx);
        }
        break;
    }
}

static inline char *chunk_buf(struct net *n, uint32_t id) {
    return n->chunks + (size_t)id * n->cfg->out_chunk_size;
}

static void submit_send(struct net *n, uint32_t conn_idx) {
    struct conn *c = conn_at(n, conn_idx);
    struct chunk_meta *m = &n->chunk_meta[c->out_chunk];
    struct io_uring_sqe *sqe = sqe_get(ring_of(n, c));
    io_uring_prep_send(sqe, (int)c->slot, chunk_buf(n, c->out_chunk) + m->off + m->sent, m->len - m->sent,
                       MSG_NOSIGNAL);
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->user_data = ud(OP_SEND, c->gen, c->out_chunk);
    c->flags |= CF_SEND_INFLIGHT;
}

/* ------------------------------------------------------------------ connections */

static void release_input(struct net *n, struct conn *c) {
    if (c->in_bid != NONE16) {
        buf_return(n, ring_of(n, c), c->in_bid);
        c->in_bid = NONE16;
    }
    if (c->partial) {
        free(c->partial);
        c->partial = NULL;
        c->partial_cap = 0;
    }
    c->in_off = c->in_len = c->partial_len = 0;
    c->flags &= (uint8_t)~CF_HAS_INPUT;
}

static void close_direct(struct net *n, uint32_t idx) {
    struct conn *c = conn_at(n, idx);
    struct io_uring_sqe *sqe = sqe_get(ring_of(n, c));
    io_uring_prep_close_direct(sqe, c->slot);
    sqe->user_data = ud(OP_CLOSE, c->gen, idx);
}

/* Begins closing: releases input, then either shuts the socket down so the
 * armed receive terminates (its final completion issues the close), or closes
 * the slot right away. A send in flight keeps its buffer until it completes. */
static void conn_close(struct net *n, uint32_t idx) {
    struct conn *c = conn_at(n, idx);
    if (c->state != ST_OPEN) return;
    c->state = ST_CLOSING;
    release_input(n, c);
    if (c->out_chunk != NONE32 && !(c->flags & CF_SEND_INFLIGHT)) {
        chunk_free(n, c->out_chunk);
        c->out_chunk = NONE32;
    }
    n->st.closes++;
    if (c->flags & CF_RECV_ARMED) {
        struct io_uring_sqe *sqe = sqe_get(ring_of(n, c));
        io_uring_prep_shutdown(sqe, (int)c->slot, SHUT_RDWR);
        sqe->flags |= IOSQE_FIXED_FILE;
        sqe->user_data = ud(OP_SHUTDOWN, c->gen, idx);
    } else {
        close_direct(n, idx);
    }
}

static void on_closed(struct net *n, uint32_t idx) {
    struct conn *c = conn_at(n, idx);
    struct ring *rg = ring_of(n, c);
    c->state = ST_FREE;
    c->gen = (c->gen + 1) & GEN_MASK;
    c->flags = 0;
    rg->used--;
    n->open_conns--;
    update_accept(n);
}

static void on_accepted(struct net *n, struct ring *rg, uint32_t slot) {
    uint32_t idx = conn_index(n, rg->idx, slot);
    struct conn *c = conn_at(n, idx);
    if (c->state != ST_FREE) {
        /* should be impossible: the kernel only hands out free slots */
        log_msg("accept into busy slot ring=%u slot=%u state=%u", rg->idx, slot, c->state);
        return;
    }
    c->state = ST_OPEN;
    c->flags = 0;
    c->ring = (uint16_t)rg->idx;
    c->slot = slot;
    c->last_active = n->now_s;
    c->out_chunk = NONE32;
    c->in_bid = NONE16;
    c->in_off = c->in_len = c->partial_len = 0;
    c->partial = NULL;
    c->partial_cap = 0;
    c->sent_100 = 0;
    rg->used++;
    n->open_conns++;
    if (n->open_conns > n->st.max_open) n->st.max_open = n->open_conns;
    n->st.accepts++;
    arm_recv(n, idx);
}

static void queue_ready(struct net *n, uint32_t idx) {
    struct conn *c = conn_at(n, idx);
    if (c->state != ST_OPEN || (c->flags & (CF_IN_QUEUE | CF_SEND_INFLIGHT | CF_HELD | CF_WAIT_OUT))) return;
    c->flags |= CF_IN_QUEUE;
    fifo_push(&n->ready, idx);
}

/* Moves the unread part of the parked receive buffer into a heap copy and
 * returns the buffer to the pool. Returns -1 on allocation failure. */
static int move_to_partial(struct net *n, struct conn *c) {
    struct ring *rg = ring_of(n, c);
    c->partial_cap = n->cfg->max_head + n->cfg->max_body;
    c->partial = malloc(c->partial_cap);
    if (!c->partial) return -1;
    c->partial_len = 0;
    if (c->in_bid != NONE16) {
        uint32_t have = (uint32_t)c->in_len - c->in_off;
        memcpy(c->partial, recv_buf(n, rg, c->in_bid) + c->in_off, have);
        c->partial_len = (uint16_t)have;
        buf_return(n, rg, c->in_bid);
        c->in_bid = NONE16;
        c->in_off = c->in_len = 0;
    }
    return 0;
}

/* Appends received bytes to the connection's input. A second buffer for the
 * same connection is folded into the heap copy so there is at most one parked
 * receive buffer per connection. Returns -1 when the input limit is exceeded. */
static int append_input(struct net *n, uint32_t idx, uint16_t bid, uint32_t len) {
    struct conn *c = conn_at(n, idx);
    struct ring *rg = ring_of(n, c);
    if (!(c->flags & CF_HAS_INPUT)) {
        c->in_bid = bid;
        c->in_off = 0;
        c->in_len = (uint16_t)len;
        c->flags |= CF_HAS_INPUT;
        return 0;
    }
    if (!c->partial && move_to_partial(n, c)) {
        buf_return(n, rg, bid);
        return -1;
    }
    if ((uint32_t)c->partial_len + len > c->partial_cap) {
        buf_return(n, rg, bid);
        return -1;
    }
    memcpy(c->partial + c->partial_len, recv_buf(n, rg, bid), len);
    c->partial_len = (uint16_t)(c->partial_len + len);
    buf_return(n, rg, bid);
    return 0;
}

static void send_100_continue(struct net *n, struct conn *c) {
    static const char msg[] = "HTTP/1.1 100 Continue\r\n\r\n";
    struct io_uring_sqe *sqe = sqe_get(ring_of(n, c));
    io_uring_prep_send(sqe, (int)c->slot, msg, sizeof msg - 1, MSG_NOSIGNAL);
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->user_data = ud(OP_SEND100, 0, 0);
    c->sent_100 = 1;
}

/* Consumes `used` bytes of the connection's input after a request was handled. */
static void consume_input(struct net *n, struct conn *c, uint32_t used) {
    if (c->partial) {
        uint32_t rest = c->partial_len - used;
        if (rest) {
            memmove(c->partial, c->partial + used, rest);
            c->partial_len = (uint16_t)rest;
        } else {
            free(c->partial);
            c->partial = NULL;
            c->partial_cap = 0;
            c->partial_len = 0;
            c->flags &= (uint8_t)~CF_HAS_INPUT;
        }
        return;
    }
    c->in_off = (uint16_t)(c->in_off + used);
    if (c->in_off >= c->in_len) {
        buf_return(n, ring_of(n, c), c->in_bid);
        c->in_bid = NONE16;
        c->in_off = c->in_len = 0;
        c->flags &= (uint8_t)~CF_HAS_INPUT;
    }
}

/* Takes one request off a connection's input and runs it inside the current
 * batch. The response buffer is filled now (reads) or by app_end_batch (writes). */
static void process_conn(struct net *n, uint32_t idx) {
    struct conn *c = conn_at(n, idx);
    c->flags &= (uint8_t)~CF_IN_QUEUE;
    if (c->state != ST_OPEN || !(c->flags & CF_HAS_INPUT)) return;
    if (c->flags & (CF_SEND_INFLIGHT | CF_HELD | CF_WAIT_OUT)) return;

    const char *data;
    uint32_t len;
    if (c->partial) {
        data = c->partial;
        len = c->partial_len;
    } else {
        data = recv_buf(n, ring_of(n, c), c->in_bid) + c->in_off;
        len = (uint32_t)c->in_len - c->in_off;
    }
    struct http_req req;
    enum http_parse_status ps = http_parse(data, len, n->cfg->max_head, n->cfg->max_body, &req);
    if (ps == HTTP_PARTIAL) {
        /* keep a private copy so the receive buffer goes back to the pool */
        if (!c->partial && move_to_partial(n, c)) {
            conn_close(n, idx);
            return;
        }
        if (req.head_len && req.expect_100 && !c->sent_100) send_100_continue(n, c);
        return;
    }

    uint32_t chunk = chunk_alloc(n, idx);
    if (chunk == NONE32) {
        c->flags |= CF_WAIT_OUT;
        fifo_push(&n->wait_out, idx);
        return;
    }
    struct app_resp *resp = &n->resps[n->nresps];
    resp->buf = chunk_buf(n, chunk);
    resp->cap = n->cfg->out_chunk_size;
    resp->off = resp->len = 0;

    if (ps != HTTP_COMPLETE) {
        resp->close_after = 1;
        if (ps == HTTP_BAD) {
            n->st.bad_requests++;
            app_error_response(resp, 400, "bad request");
        } else {
            n->st.too_large++;
            app_error_response(resp, 413, "request too large");
        }
        release_input(n, c);
    } else {
        resp->close_after = !req.keep_alive;
        n->st.requests++;
        enum app_result r = app_handle(n->app, &req, resp);
        if (r == APP_HELD) {
            chunk_free(n, chunk);
            c->flags |= CF_HELD;
            fifo_push(&n->held, idx);
            return;
        }
        consume_input(n, c, req.total_len);
        c->sent_100 = 0;
    }
    if (resp->close_after) c->flags |= CF_CLOSE_AFTER_SEND;
    c->out_chunk = chunk;
    c->last_active = n->now_s;
    n->resp_conn[n->nresps++] = idx;
}

/* ------------------------------------------------------------------ completions */

static void on_send_cqe(struct net *n, struct io_uring_cqe *cqe) {
    uint32_t chunk = ud_idx(cqe->user_data);
    struct chunk_meta *m = &n->chunk_meta[chunk];
    struct conn *c = conn_at(n, m->conn);
    if (c->gen != ud_gen(cqe->user_data) || c->state == ST_FREE) {
        chunk_free(n, chunk); /* the connection is gone */
        return;
    }
    c->flags &= (uint8_t)~CF_SEND_INFLIGHT;
    if (cqe->res < 0) {
        n->st.send_errors++;
        chunk_free(n, chunk);
        c->out_chunk = NONE32;
        if (c->state == ST_OPEN) conn_close(n, m->conn);
        return;
    }
    m->sent += (uint32_t)cqe->res;
    if (m->sent < m->len) {
        n->st.short_sends++;
        if (c->state == ST_OPEN) {
            submit_send(n, m->conn);
            return;
        }
    }
    chunk_free(n, chunk);
    c->out_chunk = NONE32;
    if (c->state != ST_OPEN) return;
    if (c->flags & CF_CLOSE_AFTER_SEND) {
        conn_close(n, m->conn);
        return;
    }
    if (c->flags & CF_HAS_INPUT) queue_ready(n, m->conn); /* a pipelined request is waiting */
}

static void on_recv_cqe(struct net *n, struct io_uring_cqe *cqe) {
    uint32_t idx = ud_idx(cqe->user_data);
    struct conn *c = conn_at(n, idx);
    struct ring *rg = ring_of(n, c);
    int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
    uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    int more = (cqe->flags & IORING_CQE_F_MORE) != 0;

    if (c->gen != ud_gen(cqe->user_data) || c->state == ST_FREE) {
        if (has_buf) buf_return(n, rg, bid);
        return;
    }
    if (!more) c->flags &= (uint8_t)~CF_RECV_ARMED;

    if (c->state == ST_CLOSING) {
        if (has_buf) buf_return(n, rg, bid);
        if (!more) close_direct(n, idx);
        return;
    }
    if (cqe->res > 0 && has_buf) {
        c->last_active = n->now_s;
        if (append_input(n, idx, bid, (uint32_t)cqe->res)) {
            n->st.too_large++;
            conn_close(n, idx);
            return;
        }
        queue_ready(n, idx);
        if (!more) arm_recv(n, idx);
        return;
    }
    if (has_buf) buf_return(n, rg, bid);
    if (cqe->res == -ENOBUFS) {
        n->st.enobufs++;
        if (!(c->flags & CF_IN_REARM)) {
            c->flags |= CF_IN_REARM;
            fifo_push(&rg->rearm, idx);
        }
        return;
    }
    if (cqe->res == 0 || cqe->res < 0) {
        conn_close(n, idx); /* EOF, reset, or an unexpected error */
        return;
    }
    if (!more) arm_recv(n, idx); /* data without a buffer should not happen; stay armed */
}

static void on_accept_cqe(struct net *n, struct io_uring_cqe *cqe, int oneshot) {
    struct ring *rg = &n->rings[ud_idx(cqe->user_data)];
    int more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    if (oneshot) {
        rg->oneshots--;
    } else if (!more) {
        rg->accept_armed = 0;
        rg->cancel_pending = 0;
    }
    if (cqe->res >= 0) {
        on_accepted(n, rg, (uint32_t)cqe->res);
    } else if (cqe->res != -ECANCELED) {
        n->st.accept_errors++;
        if (cqe->res != -ENFILE && cqe->res != -EMFILE && cqe->res != -ECONNABORTED) {
            log_msg("accept: %s", strerror(-cqe->res));
        }
    }
    if (oneshot) {
        /* keep the one-shot window full while this ring still has room */
        if (n->acc_ring == (int)rg->idx && !n->accept_paused && ring_free(rg) > rg->oneshots &&
            rg->oneshots < ONESHOT_ACCEPTS) {
            arm_accept_oneshot(n, rg);
        } else if (!rg->oneshots) {
            update_accept(n);
        }
        return;
    }
    /* a multishot accept keeps going; check after each one whether this ring
     * is close enough to full that the one-shot window must take over */
    update_accept(n);
}

static void handle_cqe(struct net *n, struct ring *rg, struct io_uring_cqe *cqe) {
    switch (ud_op(cqe->user_data)) {
    case OP_RECV: on_recv_cqe(n, cqe); break;
    case OP_SEND: on_send_cqe(n, cqe); break;
    case OP_ACCEPT: on_accept_cqe(n, cqe, 0); break;
    case OP_ACCEPT1: on_accept_cqe(n, cqe, 1); break;
    case OP_CLOSE: {
        uint32_t idx = ud_idx(cqe->user_data);
        struct conn *c = conn_at(n, idx);
        if (c->gen == ud_gen(cqe->user_data) && c->state == ST_CLOSING) on_closed(n, idx);
        break;
    }
    case OP_RINGPOLL: {
        struct ring *other = &n->rings[ud_idx(cqe->user_data)];
        other->needs_reap = 1;
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            other->poll_armed = 0;
            arm_ring_poll(n, other);
        }
        break;
    }
    case OP_CANCEL: break; /* the cancelled accept's own completion drives update_accept */
    case OP_PROVIDE:
        if (cqe->res < 0) log_msg("provide_buffers ring %u: %s", rg->idx, strerror(-cqe->res));
        break;
    case OP_SHUTDOWN:
    case OP_SEND100:
    default: break;
    }
}

static void reap(struct net *n, struct ring *rg) {
    if (rg->idx != 0 && !io_uring_cq_ready(&rg->r)) io_uring_get_events(&rg->r);
    struct io_uring_cqe *cqe;
    unsigned head, count = 0;
    io_uring_for_each_cqe(&rg->r, head, cqe) {
        handle_cqe(n, rg, cqe);
        count++;
    }
    io_uring_cq_advance(&rg->r, count);
    n->st.cqes += count;
    n->last_cqes += count;
    rg->needs_reap = 0;
}

/* ------------------------------------------------------------------ periodic work */

static uint64_t read_meminfo_available_kb(void) {
    int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return UINT64_MAX;
    char buf[2048];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return UINT64_MAX;
    buf[r] = 0;
    const char *p = strstr(buf, "MemAvailable:");
    if (!p) return UINT64_MAX;
    return strtoull(p + 13, NULL, 10);
}

static uint64_t read_u64_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return UINT64_MAX;
    char buf[64];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return UINT64_MAX;
    buf[r] = 0;
    if (buf[0] == 'm') return UINT64_MAX; /* "max" */
    return strtoull(buf, NULL, 10);
}

/* Finds our cgroup's memory.max so a MemoryMax= limit (used to emulate the
 * 2 GB box) is honoured as well as the real MemAvailable. */
static void detect_cgroup_limit(struct net *n) {
    int fd = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[512];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return;
    buf[r] = 0;
    char *line = strstr(buf, "0::");
    if (!line) return;
    char *path = line + 3;
    char *nl = strchr(path, '\n');
    if (nl) *nl = 0;
    char max_path[256];
    snprintf(max_path, sizeof max_path, "/sys/fs/cgroup%s/memory.max", path);
    uint64_t lim = read_u64_file(max_path);
    if (lim == UINT64_MAX) return;
    n->cg_limit_bytes = lim;
    snprintf(n->cg_current_path, sizeof n->cg_current_path, "/sys/fs/cgroup%s/memory.current", path);
    log_msg("cgroup memory.max = %llu MB (%s)", (unsigned long long)(lim >> 20), max_path);
}

static uint64_t available_mb(struct net *n) {
    uint64_t kb = read_meminfo_available_kb();
    uint64_t mb = kb == UINT64_MAX ? UINT64_MAX : kb >> 10;
    if (n->cg_limit_bytes) {
        uint64_t cur = read_u64_file(n->cg_current_path);
        if (cur != UINT64_MAX) {
            uint64_t cg_mb = cur >= n->cg_limit_bytes ? 0 : (n->cg_limit_bytes - cur) >> 20;
            if (cg_mb < mb) mb = cg_mb;
        }
    }
    return mb;
}

static void shed_idle(struct net *n) {
    uint32_t closed = 0;
    for (uint32_t i = 0; i < n->nconns && closed < SHED_PER_TICK; i++) {
        uint32_t idx = n->shed_cursor++;
        if (n->shed_cursor >= n->nconns) n->shed_cursor = 0;
        struct conn *c = conn_at(n, idx);
        if (c->state == ST_OPEN && !(c->flags & (CF_HAS_INPUT | CF_SEND_INFLIGHT)) && c->last_active + 1 < n->now_s) {
            conn_close(n, idx);
            closed++;
        }
    }
    n->st.shed += closed;
}

static void memory_guard(struct net *n) {
    uint64_t mb = available_mb(n);
    if (mb == UINT64_MAX) return;
    int pause = mb < n->cfg->mem_stop_accept_mb;
    if (!pause && n->accept_paused && mb < n->cfg->mem_stop_accept_mb + 32) pause = 1; /* hysteresis */
    if (pause != n->accept_paused) {
        n->accept_paused = pause;
        if (pause) n->st.mem_pauses++;
        log_msg("memory guard: %llu MB available, accept %s", (unsigned long long)mb, pause ? "paused" : "resumed");
        update_accept(n);
    }
    if (mb < n->cfg->mem_shed_mb) shed_idle(n);
}

static void sweep_partials(struct net *n) {
    for (uint32_t i = 0; i < n->nconns; i++) {
        struct conn *c = conn_at(n, i);
        if (c->state == ST_OPEN && (c->flags & CF_HAS_INPUT) && !(c->flags & (CF_HELD | CF_WAIT_OUT)) &&
            c->last_active + n->cfg->partial_timeout_s < n->now_s) {
            n->st.partial_timeouts++;
            conn_close(n, i);
        }
    }
}

static void print_stats(struct net *n) {
    static uint64_t last_req, last_cpu_us;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    uint64_t cpu_us = (uint64_t)ru.ru_utime.tv_sec * 1000000 + (uint64_t)ru.ru_utime.tv_usec +
                      (uint64_t)ru.ru_stime.tv_sec * 1000000 + (uint64_t)ru.ru_stime.tv_usec;
    uint64_t dreq = n->st.requests - last_req, dcpu = cpu_us - last_cpu_us;
    struct db_stats ds;
    struct app_stats as;
    db_get_stats(n->db, &ds);
    app_get_stats(n->app, &as);
    uint32_t held_now = fifo_len(&n->held);
    log_msg(
        "stats: open=%u max=%llu req=%llu (+%llu) cpu_us/req=%.1f (total %.1f) rss=%ld MB avail=%llu MB iters=%llu "
        "wakeups=%llu "
        "cqes=%llu enobufs=%llu rearms=%llu short=%llu senderr=%llu bad=%llu big=%llu shed=%llu "
        "accerr=%llu chunks_free=%u held=%u wal=%u ckpts=%llu ckpt_avg_ms=%.1f ckpt_max_ms=%.1f holds=%llu "
        "hold_max_ms=%.1f hold_to=%llu fallback=%llu batches=%llu writes=%llu cfail=%llu "
        "feed=%llu post=%llu like=%llu create=%llu health=%llu 2xx=%llu 4xx=%llu 5xx=%llu "
        "iter_max_ms=%.1f iters_over_20ms=%llu batch_max=%u batch_max_ms=%.1f endbatch_max_ms=%.1f",
        n->open_conns, (unsigned long long)n->st.max_open, (unsigned long long)n->st.requests, (unsigned long long)dreq,
        dreq ? (double)dcpu / (double)dreq : 0.0, n->st.requests ? (double)cpu_us / (double)n->st.requests : 0.0,
        ru.ru_maxrss / 1024, (unsigned long long)available_mb(n), (unsigned long long)n->st.iterations,
        (unsigned long long)n->st.wakeups, (unsigned long long)n->st.cqes, (unsigned long long)n->st.enobufs,
        (unsigned long long)n->st.rearms, (unsigned long long)n->st.short_sends, (unsigned long long)n->st.send_errors,
        (unsigned long long)n->st.bad_requests, (unsigned long long)n->st.too_large, (unsigned long long)n->st.shed,
        (unsigned long long)n->st.accept_errors, n->nchunk_free, held_now, ds.wal_frames,
        (unsigned long long)ds.checkpoints,
        ds.checkpoints ? (double)ds.checkpoint_ns_total / (double)ds.checkpoints / 1e6 : 0.0,
        (double)ds.checkpoint_ns_max / 1e6, (unsigned long long)ds.holds, (double)ds.hold_ns_max / 1e6,
        (unsigned long long)ds.hold_timeouts, (unsigned long long)ds.feed_fallbacks,
        (unsigned long long)as.write_batches, (unsigned long long)as.writes, (unsigned long long)as.commit_failures,
        (unsigned long long)as.feed, (unsigned long long)as.post_get, (unsigned long long)as.like,
        (unsigned long long)as.post_create, (unsigned long long)as.health, (unsigned long long)as.status_2xx,
        (unsigned long long)as.status_4xx, (unsigned long long)as.status_5xx, (double)n->st.iter_ns_max / 1e6,
        (unsigned long long)n->st.iters_over_20ms, n->st.batch_max, (double)n->st.batch_ns_max / 1e6,
        (double)n->st.commit_ns_max / 1e6);
    n->st.iter_ns_max = n->st.batch_ns_max = n->st.commit_ns_max = 0;
    n->st.batch_max = 0;
    last_req = n->st.requests;
    last_cpu_us = cpu_us;
}

/* ------------------------------------------------------------------ setup */

static int open_listener(const struct config *cfg) {
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE};
    struct addrinfo *ai = NULL;
    char port[16];
    snprintf(port, sizeof port, "%d", cfg->port);
    int rc = getaddrinfo(cfg->host, port, &hints, &ai);
    if (rc) {
        log_msg("getaddrinfo(%s): %s", cfg->host, gai_strerror(rc));
        return -1;
    }
    int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_msg("socket: %s", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* Accepted sockets inherit TCP_NODELAY; direct descriptors cannot run setsockopt. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* The previous instance's rings may still be tearing down after a restart. */
    for (int attempt = 0; attempt < 200; attempt++) {
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        if (errno != EADDRINUSE || attempt == 199) {
            log_msg("bind %s:%d: %s", cfg->host, cfg->port, strerror(errno));
            close(fd);
            freeaddrinfo(ai);
            return -1;
        }
        usleep(50000);
    }
    freeaddrinfo(ai);
    if (listen(fd, LISTEN_BACKLOG)) {
        log_msg("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int ring_init(struct net *n, struct ring *rg, uint32_t idx) {
    const struct config *cfg = n->cfg;
    rg->idx = idx;
    rg->slots = n->slots_per_ring;
    /* Ring 0 is where the thread waits, so its task work can be deferred to
     * that wait. The other rings must not use DEFER_TASKRUN: their completions
     * are only posted when their task work runs, and only a posted CQE wakes
     * the poll that ring 0 keeps on their fds (queued-but-deferred work does
     * not). With COOP_TASKRUN their work runs whenever this thread crosses
     * the kernel boundary, including inside ring 0's wait. */
    static const unsigned flag_sets[2][3] = {
        {IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL,
         IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL, 0},
        {IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL,
         IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL, 0},
    };
    const unsigned *flags = flag_sets[idx == 0 ? 0 : 1];
    int rc = -EINVAL;
    for (size_t i = 0; i < 3 && rc < 0; i++) {
        struct io_uring_params p = {.flags = flags[i] | IORING_SETUP_CQSIZE, .cq_entries = 16384};
        rc = io_uring_queue_init_params(4096, &rg->r, &p);
        if (rc == 0 && i) log_msg("ring %u: fallback setup flags 0x%x", idx, flags[i]);
    }
    if (rc < 0) {
        log_msg("io_uring_queue_init: %s", strerror(-rc));
        return -1;
    }
    rc = io_uring_register_ring_fd(&rg->r);
    if (rc < 0) log_msg("ring %u: register_ring_fd: %s (continuing)", idx, strerror(-rc));
    rc = io_uring_register_files_sparse(&rg->r, rg->slots);
    if (rc < 0) {
        log_msg("ring %u: register_files_sparse(%u): %s", idx, rg->slots, strerror(-rc));
        return -1;
    }
    size_t bufs_size = (size_t)cfg->recv_bufs * cfg->recv_buf_size;
    rg->bufs = aligned_alloc(4096, (bufs_size + 4095) & ~(size_t)4095);
    rg->ret_bitmap = calloc((cfg->recv_bufs + 63) / 64, sizeof *rg->ret_bitmap);
    if (!rg->bufs || !rg->ret_bitmap || fifo_init(&rg->rearm, rg->slots)) {
        log_msg("ring %u: out of memory", idx);
        return -1;
    }
    if (!cfg->no_prefault) memset(rg->bufs, 0, bufs_size);
    /* Classic provided buffers: this kernel (Ubuntu 6.8) rejects
     * IORING_REGISTER_PBUF_RING, and one tested path beats two. */
    struct io_uring_sqe *sqe = sqe_get(rg);
    io_uring_prep_provide_buffers(sqe, rg->bufs, (int)cfg->recv_buf_size, (int)cfg->recv_bufs, 0, 0);
    sqe->user_data = ud(OP_PROVIDE, 0, idx);
    struct io_uring_cqe *cqe;
    rc = io_uring_submit_and_wait(&rg->r, 1);
    if (rc < 0 || io_uring_wait_cqe(&rg->r, &cqe) < 0 || cqe->res < 0) {
        log_msg("ring %u: provide_buffers: %s", idx, strerror(rc < 0 ? -rc : -cqe->res));
        return -1;
    }
    io_uring_cqe_seen(&rg->r, cqe);
    return 0;
}

static int net_init(struct net *n) {
    const struct config *cfg = n->cfg;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl)) {
        log_msg("getrlimit: %s", strerror(errno));
        return -1;
    }
    if (cfg->rlimit_nofile) {
        rl.rlim_cur = rl.rlim_max = cfg->rlimit_nofile;
        if (setrlimit(RLIMIT_NOFILE, &rl)) log_msg("setrlimit(%u): %s", cfg->rlimit_nofile, strerror(errno));
    } else if (rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    /* The kernel caps each fixed-file table at the hard RLIMIT_NOFILE. */
    uint32_t cap = rl.rlim_max > SLOTS_PER_RING_MAX ? SLOTS_PER_RING_MAX : (uint32_t)rl.rlim_max;
    uint32_t per_ring = cfg->ring_slots ? cfg->ring_slots : cap;
    if (per_ring > cap) per_ring = cap;
    if (per_ring > cfg->max_conns) per_ring = cfg->max_conns;
    uint32_t nrings = (cfg->max_conns + per_ring - 1) / per_ring;
    if (nrings > MAX_RINGS) {
        nrings = MAX_RINGS;
        log_msg("max_conns %u needs more than %d rings of %u; capping at %u", cfg->max_conns, MAX_RINGS, per_ring,
                nrings * per_ring);
    }
    n->slots_per_ring = per_ring;
    n->nrings = nrings;
    n->nconns = nrings * per_ring;
    log_msg("rlimit nofile soft=%lu hard=%lu -> %u ring(s) x %u slots = %u connections", (unsigned long)rl.rlim_cur,
            (unsigned long)rl.rlim_max, nrings, per_ring, n->nconns);

    n->conns = calloc(n->nconns, sizeof *n->conns);
    size_t chunks_size = (size_t)cfg->out_chunks * cfg->out_chunk_size;
    n->chunks = aligned_alloc(4096, (chunks_size + 4095) & ~(size_t)4095);
    n->chunk_meta = calloc(cfg->out_chunks, sizeof *n->chunk_meta);
    n->chunk_free = malloc((size_t)cfg->out_chunks * sizeof *n->chunk_free);
    n->resps = calloc(cfg->work_budget, sizeof *n->resps);
    n->resp_conn = calloc(cfg->work_budget, sizeof *n->resp_conn);
    if (!n->conns || !n->chunks || !n->chunk_meta || !n->chunk_free || !n->resps || !n->resp_conn ||
        fifo_init(&n->ready, n->nconns) || fifo_init(&n->wait_out, n->nconns) || fifo_init(&n->held, n->nconns)) {
        log_msg("out of memory sizing tables");
        return -1;
    }
    for (uint32_t i = 0; i < cfg->out_chunks; i++) n->chunk_free[i] = cfg->out_chunks - 1 - i;
    n->nchunk_free = cfg->out_chunks;
    for (uint32_t i = 0; i < n->nconns; i++) {
        n->conns[i].in_bid = NONE16;
        n->conns[i].out_chunk = NONE32;
    }
    if (!cfg->no_prefault) {
        /* touch every page now so the first connections do not pay for faults */
        memset(n->chunks, 0, chunks_size);
        for (size_t off = 0; off < (size_t)n->nconns * sizeof *n->conns; off += 4096) {
            ((volatile char *)n->conns)[off] = 0;
        }
    }

    n->listen_fd = open_listener(cfg);
    if (n->listen_fd < 0) return -1;
    for (uint32_t i = 0; i < nrings; i++) {
        if (ring_init(n, &n->rings[i], i)) return -1;
    }
    for (uint32_t i = 1; i < nrings; i++) arm_ring_poll(n, &n->rings[i]);
    detect_cgroup_limit(n);
    update_accept(n);
    for (uint32_t i = 0; i < nrings; i++) io_uring_submit(&n->rings[i].r);
    return 0;
}

static void net_destroy(struct net *n) {
    for (uint32_t i = 0; i < n->nrings; i++) {
        struct ring *rg = &n->rings[i];
        if (rg->r.ring_fd > 0) io_uring_queue_exit(&rg->r);
        free(rg->bufs);
        free(rg->ret_bitmap);
        free(rg->rearm.v);
    }
    if (n->listen_fd >= 0) close(n->listen_fd);
    if (n->conns) {
        for (uint32_t i = 0; i < n->nconns; i++) free(n->conns[i].partial);
    }
    free(n->conns);
    free(n->chunks);
    free(n->chunk_meta);
    free(n->chunk_free);
    free(n->resps);
    free(n->resp_conn);
    free(n->ready.v);
    free(n->wait_out.v);
    free(n->held.v);
}

/* ------------------------------------------------------------------ the loop */

static void run_batch(struct net *n) {
    uint32_t budget = n->cfg->work_budget;
    n->nresps = 0;
    app_begin_batch(n->app);
    while (budget-- && !fifo_empty(&n->ready)) process_conn(n, fifo_pop(&n->ready));
    int64_t t_commit = now_ns();
    app_end_batch(n->app); /* commits every write of the batch before anything is sent */
    int64_t d = now_ns() - t_commit;
    if ((uint64_t)d > n->st.commit_ns_max) n->st.commit_ns_max = (uint64_t)d;
    if (n->nresps > n->st.batch_max) n->st.batch_max = n->nresps;
    for (uint32_t i = 0; i < n->nresps; i++) {
        uint32_t idx = n->resp_conn[i];
        struct conn *c = conn_at(n, idx);
        struct chunk_meta *m = &n->chunk_meta[c->out_chunk];
        m->off = (uint32_t)n->resps[i].off;
        m->len = (uint32_t)n->resps[i].len;
        m->sent = 0;
        submit_send(n, idx);
    }
}

/* Re-arms receives that ended with ENOBUFS, after buffers were returned and
 * only as many as the returned buffers can serve, so a burst cannot drain
 * the pool again and again (the "ENOBUFS storm"). */
static void rearm_enobufs(struct net *n, struct ring *rg) {
    uint32_t budget = rg->returned_this_iter > 32 ? rg->returned_this_iter : 32;
    if (budget > n->cfg->recv_bufs / 2) budget = n->cfg->recv_bufs / 2;
    rg->returned_this_iter = 0;
    while (budget && !fifo_empty(&rg->rearm)) {
        uint32_t idx = fifo_pop(&rg->rearm);
        struct conn *c = conn_at(n, idx);
        if (!(c->flags & CF_IN_REARM)) continue;
        c->flags &= (uint8_t)~CF_IN_REARM;
        if (c->state != ST_OPEN || (c->flags & CF_RECV_ARMED)) continue;
        arm_recv(n, idx);
        n->st.rearms++;
        budget--;
    }
}

int net_run(const struct config *cfg, struct app *app, struct db *db) {
    struct net *n = calloc(1, sizeof *n);
    if (!n) return -1;
    n->cfg = cfg;
    n->app = app;
    n->db = db;
    n->listen_fd = -1;
    n->acc_ring = -1;
    n->start_ns = now_ns();
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (net_init(n)) {
        net_destroy(n);
        free(n);
        return -1;
    }
    log_msg("listening on %s:%d", cfg->host, cfg->port);
    n->next_guard_ns = n->start_ns + GUARD_INTERVAL_NS;
    n->next_sweep_ns = n->start_ns + SWEEP_INTERVAL_NS;
    n->next_stats_ns = cfg->stats_interval_s ? n->start_ns + (int64_t)cfg->stats_interval_s * 1000000000 : INT64_MAX;
    int64_t exit_at = cfg->exit_after_s ? n->start_ns + (int64_t)cfg->exit_after_s * 1000000000 : INT64_MAX;
    struct ring *r0 = &n->rings[0];

    while (!stop_requested) {
        int64_t t = now_ns();
        n->now_s = (uint32_t)((t - n->start_ns) / 1000000000);
        if (t >= exit_at) break;
        if (t >= n->next_guard_ns) {
            n->next_guard_ns = t + GUARD_INTERVAL_NS;
            memory_guard(n);
        }
        if (t >= n->next_sweep_ns) {
            n->next_sweep_ns = t + SWEEP_INTERVAL_NS;
            sweep_partials(n);
        }
        if (t >= n->next_stats_ns) {
            n->next_stats_ns = t + (int64_t)cfg->stats_interval_s * 1000000000;
            print_stats(n);
        }
        if (!fifo_empty(&n->held) && !app_writes_held(app)) {
            while (!fifo_empty(&n->held)) {
                uint32_t idx = fifo_pop(&n->held);
                struct conn *c = conn_at(n, idx);
                c->flags &= (uint8_t)~CF_HELD;
                queue_ready(n, idx);
            }
        }

        /* Wait for a batch of completions, or not at all when work is pending. */
        int pending = !fifo_empty(&n->ready);
        for (uint32_t i = 0; i < n->nrings && !pending; i++) {
            if (io_uring_cq_ready(&n->rings[i].r)) pending = 1;
        }
        unsigned wait_nr;
        struct __kernel_timespec ts;
        if (pending) {
            wait_nr = 0;
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
        } else if (n->last_cqes >= 4) {
            wait_nr = cfg->batch_cqes;
            ts.tv_sec = 0;
            ts.tv_nsec = (long long)cfg->batch_wait_us * 1000;
        } else {
            wait_nr = 1;
            ts.tv_sec = 0;
            ts.tv_nsec = 50000000;
        }
        struct io_uring_cqe *cqe;
        int rc = io_uring_submit_and_wait_timeout(&r0->r, &cqe, wait_nr, &ts, NULL);
        /* -EAGAIN is liburing's "nothing to wait for" with wait_nr == 0 */
        if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EBUSY && rc != -EAGAIN) {
            log_msg("submit_and_wait: %s", strerror(-rc));
        }
        n->last_cqes = 0;

        int64_t t_iter = now_ns();
        reap(n, r0);
        for (uint32_t i = 1; i < n->nrings; i++) {
            if (n->rings[i].needs_reap || io_uring_cq_ready(&n->rings[i].r)) reap(n, &n->rings[i]);
        }
        n->st.iterations++;

        if (!fifo_empty(&n->ready)) {
            int64_t t_batch = now_ns();
            run_batch(n);
            int64_t d = now_ns() - t_batch;
            if ((uint64_t)d > n->st.batch_ns_max) n->st.batch_ns_max = (uint64_t)d;
        }

        for (uint32_t i = 0; i < n->nrings; i++) {
            struct ring *rg = &n->rings[i];
            flush_returned_buffers(n, rg);
            rearm_enobufs(n, rg);
            if (i && io_uring_sq_ready(&rg->r)) {
                rc = io_uring_submit(&rg->r);
                if (rc < 0 && rc != -EBUSY) log_msg("submit ring %u: %s", i, strerror(-rc));
                if (io_uring_cq_ready(&rg->r)) rg->needs_reap = 1;
            }
        }
        int64_t d_iter = now_ns() - t_iter;
        if ((uint64_t)d_iter > n->st.iter_ns_max) n->st.iter_ns_max = (uint64_t)d_iter;
        if (d_iter > 20000000) n->st.iters_over_20ms++;
    }
    log_msg("exiting: open=%u accepts=%llu requests=%llu", n->open_conns, (unsigned long long)n->st.accepts,
            (unsigned long long)n->st.requests);
    if (cfg->stats_interval_s) print_stats(n);
    net_destroy(n);
    free(n);
    return 0;
}
