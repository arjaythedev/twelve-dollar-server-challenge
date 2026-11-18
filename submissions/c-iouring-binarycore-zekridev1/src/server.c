#define _GNU_SOURCE
#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <fcntl.h>

#define bc_idle_seconds 90
#define bc_sweep_ns 2000000000ULL
#define bc_stats_ns 10000000000ULL
#define bc_batch_wait 64
#define bc_batch_wait_ms 2
#define bc_pend_max ((16 * 1024) + (1024 * 1024))
#define bc_reads_per_batch 16384
#define bc_answers_per_batch 2048

enum {
    bc_ud_accept = 1,
    bc_ud_recv = 2,
    bc_ud_send = 3,
    bc_ud_close = 4,
    bc_ud_cancel = 6,
    bc_ud_poll = 7
};
#define bc_ud(op, ring, slot, gen) \
    (((uint64_t)(op) << 56) | ((uint64_t)(ring) << 48) | \
     ((uint64_t)(slot) << 32) | (uint64_t)(gen))
#define bc_ud_op(u)   ((unsigned)((u) >> 56))
#define bc_ud_ring(u) ((unsigned)((u) >> 48) & 0xff)
#define bc_ud_slot(u) ((unsigned)((u) >> 32) & 0xffff)
#define bc_ud_gen(u)  ((uint32_t)(u))

/* Per-reply entry. */
struct bc_reply {
    struct bc_conn *c;
    uint32_t gen;
    struct bc_arena *a;
    uint32_t off, len;
    unsigned char db, close;
};

/* Per-list entry. */
struct bc_cref {
    struct bc_conn *c;
    uint32_t gen;
};

static uint64_t bc_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static volatile sig_atomic_t bc_stop = 0;
static void bc_sig_handler(int sig) { (void)sig; bc_stop = 1; }

/* ---------- config ---------- */

static long bc_env_long(const char *name, long def) {
    const char *v = getenv(name);
    return v && *v ? atol(v) : def;
}

int bc_config_read(struct bc_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->host = getenv("HOST");
    if (!cfg->host || !*cfg->host) cfg->host = "0.0.0.0";
    cfg->port = getenv("PORT");
    if (!cfg->port || !*cfg->port) cfg->port = "80";
    cfg->db_path = getenv("SQLITE_PATH");
    cfg->secret = getenv("JWT_SECRET");
    if (!cfg->db_path || !*cfg->db_path || !cfg->secret || !*cfg->secret) {
        fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
        return -1;
    }
    cfg->cache_kib = (int)bc_env_long("CACHE_KIB", 8192);
    cfg->backlog = (int)bc_env_long("BACKLOG", 4096);
    cfg->n_rings = 1;
    cfg->sq_entries = 4096;
    cfg->cq_entries = 65536;
    cfg->nslots = 65535;
    cfg->nbufs = 4096;
    cfg->buf_size = 1024;
    cfg->max_conns = (unsigned)bc_env_long("MAX_CONNS", 500000);
    return 0;
}

/* ---------- arena helpers ---------- */

static inline struct bc_arena *bc_batch_arena_get(struct bc_server *s) {
    return bc_arena_get(&s->arena_pool, 1u << 20);
}
static inline void bc_batch_arena_put(struct bc_server *s, struct bc_arena *a) {
    bc_arena_put(&s->arena_pool, a);
}
static inline uint8_t *bc_batch_reserve(struct bc_server *s, unsigned n) {
    if (!s->cur_arena || s->cur_arena->used + n > s->cur_arena->cap) {
        s->cur_arena = bc_batch_arena_get(s);
        if (!s->cur_arena) return NULL;
    }
    uint8_t *p = s->cur_arena->data + s->cur_arena->used;
    s->cur_arena->used += n;
    return p;
}

/* ---------- connection lifecycle ---------- */

static inline struct bc_conn_pool *bc_conn_pool_of(struct bc_server *s,
                                                   unsigned ring) {
    return &s->conn_pools[ring];
}

static void bc_conn_close(struct bc_server *s, struct bc_conn *c) {
    if (!(c->flags & bc_cf_open) || (c->flags & bc_cf_closing)) return;
    c->flags |= (uint16_t)bc_cf_closing;
    struct bc_uring_ring *r = &s->uring.rings[c->ring];
    bc_uring_shutdown_close(r, c->slot,
        bc_ud(bc_ud_close, c->ring, c->slot, c->gen),
        bc_ud(bc_ud_poll, c->ring, c->slot, c->gen),
        bc_ud(bc_ud_cancel, c->ring, c->slot, c->gen));
    s->closes++;
}

static void bc_conn_free(struct bc_server *s, struct bc_conn *c) {
    struct bc_conn_pool *p = bc_conn_pool_of(s, c->ring);
    if (c->pend) { free(c->pend); c->pend = NULL; }
    c->pend_len = c->pend_cap = 0;
    if (c->send_arena) {
        bc_batch_arena_put(s, c->send_arena);
        c->send_arena = NULL;
    }
    c->gen++;
    c->flags = 0;
    p->open--;
}

static void bc_arm_poll(struct bc_server *s, struct bc_conn *c) {
    struct bc_uring_ring *r = &s->uring.rings[c->ring];
    bc_uring_poll_conn(r, c->slot,
        bc_ud(bc_ud_poll, c->ring, c->slot, c->gen));
    c->flags &= (uint16_t)~bc_cf_rearm;
}

static void bc_queue_read(struct bc_server *s, struct bc_conn *c) {
    if (c->flags & bc_cf_rq) return;
    c->flags |= (uint16_t)bc_cf_rq;
    if (s->nrq == s->rq_cap) {
        s->rq_cap = s->rq_cap ? s->rq_cap * 2 : 1024;
        s->rq = (struct bc_cref *)realloc(s->rq, s->rq_cap * sizeof *s->rq);
        if (!s->rq) { perror("realloc rq"); _exit(1); }
    }
    s->rq[s->nrq++] = (struct bc_cref){c, c->gen};
}

static unsigned bc_issue_reads(struct bc_server *s) {
    unsigned budget = bc_reads_per_batch, end = s->nrq;
    while (s->rq_head < end && budget) {
        struct bc_cref e = s->rq[s->rq_head++];
        struct bc_conn *c = e.c;
        if (c->gen != e.gen ||
            (c->flags & (bc_cf_open | bc_cf_closing)) != bc_cf_open) continue;
        struct bc_conn_pool *p = bc_conn_pool_of(s, c->ring);
        struct bc_uring_ring *r = &s->uring.rings[c->ring];
        if ((c->flags & bc_cf_recving) || r->bufs_out >= r->br_entries) {
            if (s->nrq == s->rq_cap) {
                s->rq_cap = s->rq_cap ? s->rq_cap * 2 : 1024;
                s->rq = (struct bc_cref *)realloc(s->rq, s->rq_cap * sizeof *s->rq);
                if (!s->rq) { perror("realloc rq"); _exit(1); }
            }
            s->rq[s->nrq++] = (struct bc_cref){c, c->gen};
            continue;
        }
        c->flags = (uint16_t)((c->flags & ~bc_cf_rq) | bc_cf_recving);
        r->bufs_out++;
        budget--;
        bc_uring_recv_once(r, c->slot,
            bc_ud(bc_ud_recv, c->ring, c->slot, c->gen));
    }
    if (s->rq_head == s->nrq) s->rq_head = s->nrq = 0;
    else if (s->rq_head > s->nrq / 2) {
        memmove(s->rq, s->rq + s->rq_head,
                (s->nrq - s->rq_head) * sizeof *s->rq);
        s->nrq -= s->rq_head;
        s->rq_head = 0;
    }
    return bc_reads_per_batch - budget;
}

static void bc_stash(struct bc_conn *c, const uint8_t *data, unsigned len) {
    if (len == 0) {
        c->pend_len = 0;
        free(c->pend);
        c->pend = NULL;
        c->pend_cap = 0;
        return;
    }
    if (data == c->pend) { c->pend_len = len; return; }
    if (c->pend && data >= c->pend && data < c->pend + c->pend_cap) {
        memmove(c->pend, data, len);
        c->pend_len = len;
        return;
    }
    if (c->pend_cap < len) {
        unsigned cap = c->pend_cap ? c->pend_cap : 512;
        while (cap < len) cap *= 2;
        uint8_t *np = (uint8_t *)realloc(c->pend, cap);
        if (!np) { perror("realloc pend"); _exit(1); }
        c->pend = np;
        c->pend_cap = cap;
    }
    memcpy(c->pend, data, len);
    c->pend_len = len;
}

static void bc_conn_feed(struct bc_server *s, struct bc_conn *c,
                         const uint8_t *data, unsigned len) {
    const uint8_t *buf;
    unsigned n;
    if (c->pend_len) {
        if (len) {
            if (c->pend_len + len > c->pend_cap) {
                unsigned cap = c->pend_cap ? c->pend_cap : 512;
                while (cap < c->pend_len + len) cap *= 2;
                uint8_t *np = (uint8_t *)realloc(c->pend, cap);
                if (!np) { perror("realloc pend"); _exit(1); }
                c->pend = np;
                c->pend_cap = cap;
            }
            memcpy(c->pend + c->pend_len, data, len);
            c->pend_len += len;
        }
        buf = c->pend;
        n = c->pend_len;
    } else {
        buf = data;
        n = len;
    }
    while (n) {
        if (c->flags & (bc_cf_sending | bc_cf_replied | bc_cf_closing |
                        bc_cf_held)) break;
        struct bc_req rq;
        int rc = bc_http_parse(buf, n, &rq);
        if (rc == 0) {
            if (n > bc_pend_max) { bc_conn_close(s, c); n = 0; }
            break;
        }
        if (rc < 0) {
            uint8_t *free = bc_batch_reserve(s, bc_l_400_bad);
            if (free) {
                memcpy(free, bc_r_400_bad, bc_l_400_bad);
                s->cur_arena->used += bc_l_400_bad;
                if (s->nrep == s->rep_cap) {
                    s->rep_cap = s->rep_cap ? s->rep_cap * 2 : 1024;
                    s->replies = (struct bc_reply *)realloc(
                        s->replies, s->rep_cap * sizeof *s->replies);
                    if (!s->replies) { perror("realloc replies"); _exit(1); }
                }
                s->replies[s->nrep++] = (struct bc_reply){
                    c, c->gen, s->cur_arena,
                    (uint32_t)(free - s->cur_arena->data),
                    bc_l_400_bad, 0, 1 };
                c->flags |= (uint16_t)(bc_cf_replied | bc_cf_close_after);
            }
            n = 0;
            break;
        }
        uint8_t *free = bc_batch_reserve(s, bc_app_resp_max);
        struct bc_resp rp;
        if (!free) break;
        bc_app_handle(&s->app, &rq, free, s->writes_ok, &rp);
        if (rp.held) {
            c->flags |= (uint16_t)bc_cf_held;
            break;
        }
        s->cur_arena->used += (uint32_t)((rp.p + rp.n) - free);
        if (s->nrep == s->rep_cap) {
            s->rep_cap = s->rep_cap ? s->rep_cap * 2 : 1024;
            s->replies = (struct bc_reply *)realloc(
                s->replies, s->rep_cap * sizeof *s->replies);
            if (!s->replies) { perror("realloc replies"); _exit(1); }
        }
        s->replies[s->nrep++] = (struct bc_reply){
            c, c->gen, s->cur_arena,
            (uint32_t)(rp.p - s->cur_arena->data), rp.n, rp.db, rp.close };
        c->flags |= (uint16_t)bc_cf_replied;
        if (rp.close) c->flags |= (uint16_t)bc_cf_close_after;
        buf += rq.total_len;
        n -= rq.total_len;
    }
    bc_stash(c, buf, n);
}

/* ---------- completion handlers ---------- */

static void bc_on_accept(struct bc_server *s, unsigned ring,
                         struct io_uring_cqe *cqe) {
    struct bc_conn_pool *p = bc_conn_pool_of(s, ring);
    if (cqe->res < 0) {
        if (cqe->res != -ENFILE && cqe->res != -EMFILE && cqe->res != -ECANCELED)
            fprintf(stderr, "accept: %s\n", strerror(-cqe->res));
        return;
    }
    unsigned slot = (unsigned)cqe->res;
    struct bc_conn *c = &p->conns[slot];
    if (c->flags & bc_cf_open) {
        fprintf(stderr, "accept: slot %u still open\n", slot);
        return;
    }
    c->flags = (uint16_t)bc_cf_open;
    c->ring = (uint16_t)ring;
    c->slot = slot;
    c->last_active = s->now_s;
    c->pend_len = 0;
    c->send_arena = NULL;
    p->open++;
    s->accepts++;
    bc_arm_poll(s, c);
}

static void bc_on_poll(struct bc_server *s, unsigned ring,
                       struct io_uring_cqe *cqe) {
    unsigned slot = bc_ud_slot(cqe->user_data);
    struct bc_conn_pool *p = bc_conn_pool_of(s, ring);
    struct bc_conn *c = &p->conns[slot];
    if (c->gen != bc_ud_gen(cqe->user_data) ||
        (c->flags & (bc_cf_open | bc_cf_closing)) != bc_cf_open)
        return;
    if (cqe->res < 0) { bc_conn_close(s, c); return; }
    bc_queue_read(s, c);
    if (!(cqe->flags & IORING_CQE_F_MORE)) c->flags |= (uint16_t)bc_cf_rearm;
}

static void bc_on_recv(struct bc_server *s, unsigned ring,
                       struct io_uring_cqe *cqe) {
    unsigned slot = bc_ud_slot(cqe->user_data);
    struct bc_conn_pool *p = bc_conn_pool_of(s, ring);
    struct bc_conn *c = &p->conns[slot];
    int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
    unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    /* bufs_out was incremented when the read was issued (bc_issue_reads).
     * bc_uring_buf_return() below decrements it when the buffer goes back
     * into the ring. Do not decrement here. */
    int live = c->gen == bc_ud_gen(cqe->user_data) &&
               (c->flags & bc_cf_open) && !(c->flags & bc_cf_closing);
    if (live) c->flags &= (uint16_t)~bc_cf_recving;
    if (live && cqe->res > 0) {
        c->last_active = s->now_s;
        if (s->answers_left) {
            s->answers_left--;
            bc_conn_feed(s, c,
                bc_uring_buf_ptr(&s->uring.rings[ring], bid),
                (unsigned)cqe->res);
        } else {
            if (c->pend_len + (unsigned)cqe->res > c->pend_cap) {
                unsigned cap = c->pend_cap ? c->pend_cap : 512;
                while (cap < c->pend_len + (unsigned)cqe->res) cap *= 2;
                uint8_t *np = (uint8_t *)realloc(c->pend, cap);
                if (!np) { perror("realloc pend"); _exit(1); }
                c->pend = np;
                c->pend_cap = cap;
            }
            memcpy(c->pend + c->pend_len,
                   bc_uring_buf_ptr(&s->uring.rings[ring], bid),
                   (unsigned)cqe->res);
            c->pend_len += (unsigned)cqe->res;
            if (!(c->flags & bc_cf_ready)) {
                c->flags |= (uint16_t)bc_cf_ready;
                if (s->nready == s->ready_cap) {
                    s->ready_cap = s->ready_cap ? s->ready_cap * 2 : 1024;
                    s->ready = (struct bc_cref *)realloc(
                        s->ready, s->ready_cap * sizeof *s->ready);
                    if (!s->ready) { perror("realloc ready"); _exit(1); }
                }
                s->ready[s->nready++] = (struct bc_cref){c, c->gen};
            }
        }
    }
    if (has_buf) {
        struct bc_uring_ring *r = &s->uring.rings[ring];
        bc_uring_buf_return(r, bid);
        bc_uring_buf_publish(r);
    }
    if (!live) return;
    if (cqe->res > 0) {
        if ((unsigned)cqe->res == s->uring.rings[ring].buf_size)
            bc_queue_read(s, c);
        return;
    }
    if (cqe->res == -EAGAIN) return;
    if (cqe->res == -ENOBUFS) { bc_queue_read(s, c); return; }
    bc_conn_close(s, c);
}

static void bc_on_send(struct bc_server *s, unsigned ring,
                       struct io_uring_cqe *cqe) {
    unsigned slot = bc_ud_slot(cqe->user_data);
    struct bc_conn_pool *p = bc_conn_pool_of(s, ring);
    struct bc_conn *c = &p->conns[slot];
    if (c->gen != bc_ud_gen(cqe->user_data)) return;
    c->flags &= (uint16_t)~bc_cf_sending;
    if (c->send_arena) {
        bc_batch_arena_put(s, c->send_arena);
        c->send_arena = NULL;
    }
    if (cqe->res < 0) { bc_conn_close(s, c); return; }
    if (c->flags & bc_cf_close_after) { bc_conn_close(s, c); return; }
    if (c->pend_len && !(c->flags & (bc_cf_ready | bc_cf_held |
                                      bc_cf_closing))) {
        c->flags |= (uint16_t)bc_cf_ready;
        if (s->nready == s->ready_cap) {
            s->ready_cap = s->ready_cap ? s->ready_cap * 2 : 1024;
            s->ready = (struct bc_cref *)realloc(
                s->ready, s->ready_cap * sizeof *s->ready);
            if (!s->ready) { perror("realloc ready"); _exit(1); }
        }
        s->ready[s->nready++] = (struct bc_cref){c, c->gen};
    }
}

static void bc_on_close(struct bc_server *s, unsigned ring,
                        struct io_uring_cqe *cqe) {
    unsigned slot = bc_ud_slot(cqe->user_data);
    struct bc_conn_pool *p = bc_conn_pool_of(s, ring);
    struct bc_conn *c = &p->conns[slot];
    if (c->gen != bc_ud_gen(cqe->user_data)) return;
    if (!(c->flags & bc_cf_closing)) return;
    if (cqe->res == -ENOTCONN || cqe->res == -EPIPE || cqe->res == -ENOTSOCK)
        return;
    bc_conn_free(s, c);
}

/* reap callback: dispatch on op. */
static void bc_cqe_cb(struct bc_uring_ring *r, unsigned slot,
                      uint64_t user_data, struct io_uring_cqe *cqe,
                      void *arg) {
    struct bc_server *s = arg;
    unsigned ring = bc_ud_ring(user_data);
    unsigned op = bc_ud_op(user_data);
    switch (op) {
    case bc_ud_recv:   bc_on_recv(s, ring, cqe); break;
    case bc_ud_poll:   bc_on_poll(s, ring, cqe); break;
    case bc_ud_send:   bc_on_send(s, ring, cqe); break;
    case bc_ud_accept: bc_on_accept(s, ring, cqe); break;
    case bc_ud_close:  bc_on_close(s, ring, cqe); break;
    default: break;
    }
    (void)slot;
}

/* ---------- reply flush ---------- */

static void bc_flush_replies(struct bc_server *s, int commit_failed) {
    for (unsigned i = 0; i < s->nrep; i++) {
        struct bc_reply *rp = &s->replies[i];
        struct bc_conn *c = rp->c;
        if (c->gen != rp->gen) continue;
        c->flags &= (uint16_t)~bc_cf_replied;
        if (!(c->flags & bc_cf_open) || (c->flags & bc_cf_closing)) continue;
        uint8_t *p = rp->a->data + rp->off;
        unsigned len = rp->len;
        if (commit_failed && rp->db) {
            uint8_t *np = bc_batch_reserve(s, bc_l_500);
            if (np) {
                memcpy(np, bc_r_500, bc_l_500);
                s->cur_arena->used += bc_l_500;
                p = np;
                len = bc_l_500;
                rp->a = s->cur_arena;
                rp->off = (uint32_t)(p - s->cur_arena->data);
            }
        }
        struct bc_uring_ring *r = &s->uring.rings[c->ring];
        bc_uring_send(r, c->slot, p, len,
            bc_ud(bc_ud_send, c->ring, c->slot, c->gen));
        rp->a->refs++;          /* one ref for the in-flight send */
        c->send_arena = rp->a;
        c->flags |= (uint16_t)bc_cf_sending;
        s->reqs_sent++;
    }
    s->nrep = 0;
    if (s->cur_arena) {
        bc_batch_arena_put(s, s->cur_arena);
        s->cur_arena = NULL;
    }
}

/* ---------- run lists ---------- */

static void bc_run_lists(struct bc_server *s) {
    unsigned n = s->nready, i = 0;
    for (; i < n && s->answers_left; i++) {
        struct bc_conn *c = s->ready[i].c;
        if (c->gen != s->ready[i].gen) continue;
        c->flags &= (uint16_t)~bc_cf_ready;
        if (!(c->flags & bc_cf_open) ||
            (c->flags & (bc_cf_closing | bc_cf_sending | bc_cf_held)))
            continue;
        s->answers_left--;
        bc_conn_feed(s, c, NULL, 0);
    }
    memmove(s->ready, s->ready + i, (s->nready - i) * sizeof *s->ready);
    s->nready -= i;
}

/* ---------- sweep ---------- */

static void bc_sweep(struct bc_server *s) {
    for (unsigned ri = 0; ri < s->uring.n_rings; ri++) {
        struct bc_conn_pool *p = bc_conn_pool_of(s, ri);
        if (!p->open) continue;
        for (unsigned i = 0; i < p->nslots; i++) {
            struct bc_conn *c = &p->conns[i];
            if ((c->flags & (bc_cf_open | bc_cf_closing)) == bc_cf_open &&
                s->now_s - c->last_active > bc_idle_seconds) {
                bc_conn_close(s, c);
            }
        }
    }
}

/* ---------- stats ---------- */

static void bc_stats(struct bc_server *s, uint64_t dt_ns) {
    unsigned open = 0;
    for (unsigned i = 0; i < s->uring.n_rings; i++)
        open += s->conn_pools[i].open;
    double dt = (double)dt_ns / 1e9;
    fprintf(stderr,
        "open=%u req/s=%.0f batches/s=%.0f accepts=%llu closes=%llu "
        "max_turn=%.1fms max_commit=%.1fms\n",
        open, (double)s->reqs_sent / dt, (double)s->batches / dt,
        (unsigned long long)s->accepts, (unsigned long long)s->closes,
        (double)s->max_turn_ns / 1e6, (double)s->max_commit_ns / 1e6);
    s->batches = s->cqes = s->reqs_sent = s->closes = s->accepts = 0;
    s->max_turn_ns = s->max_commit_ns = 0;
}

/* ---------- init / run / cleanup ---------- */

int bc_server_init(struct bc_server *s, const struct bc_config *cfg) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->now_s = (uint32_t)time(NULL);
    s->writes_ok = 1;

    if (bc_app_init(&s->app, cfg->db_path, cfg->secret, cfg->cache_kib) < 0)
        return -1;

    if (bc_uring_init(&s->uring, cfg->n_rings, cfg->sq_entries,
                      cfg->cq_entries, cfg->nslots, cfg->nbufs,
                      cfg->buf_size, NULL) < 0)
        return -1;

    s->conn_pools = (struct bc_conn_pool *)
        calloc(cfg->n_rings, sizeof *s->conn_pools);
    if (!s->conn_pools) return -1;
    for (unsigned i = 0; i < cfg->n_rings; i++) {
        s->conn_pools[i].conns = (struct bc_conn *)
            calloc(cfg->nslots, sizeof(struct bc_conn));
        if (!s->conn_pools[i].conns) return -1;
        s->conn_pools[i].nslots = cfg->nslots;
    }

    s->rep_cap = s->ready_cap = s->rq_cap = 32768;
    s->replies = (struct bc_reply *)malloc(s->rep_cap * sizeof *s->replies);
    s->ready = (struct bc_cref *)malloc(s->ready_cap * sizeof *s->ready);
    s->rq = (struct bc_cref *)malloc(s->rq_cap * sizeof *s->rq);
    if (!s->replies || !s->ready || !s->rq) return -1;

    s->uring.n_listen = 1;
    s->uring.listen_fds[0] = bc_uring_listen_socket(cfg->host,
        atoi(cfg->port), cfg->backlog, NULL);
    if (s->uring.listen_fds[0] < 0) return -1;

    bc_uring_accept_register(&s->uring.rings[0], s->uring.listen_fds[0],
                             0, (void *)(uintptr_t)bc_ud(bc_ud_accept, 0, 0, 0));

    /* Pre-warm batch memory. */
    for (int i = 0; i < 8; i++) {
        struct bc_arena *a = bc_batch_arena_get(s);
        if (a) { memset(a->data, 0, a->cap); bc_batch_arena_put(s, a); }
    }
    return 0;
}

void bc_server_cleanup(struct bc_server *s) {
    bc_db_close(&s->app.db);
    bc_uring_cleanup(&s->uring);
    if (s->conn_pools) {
        for (unsigned i = 0; i < s->cfg.n_rings; i++)
            free(s->conn_pools[i].conns);
        free(s->conn_pools);
    }
    free(s->replies);
    free(s->ready);
    free(s->rq);
}

void bc_server_run(struct bc_server *s) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, bc_sig_handler);
    signal(SIGINT, bc_sig_handler);
    setvbuf(stderr, NULL, _IOLBF, 0);

    uint64_t t0 = bc_now_ns(), last_sweep = t0, last_stats = t0;
    unsigned last_cqes = 0;

    while (!bc_stop) {
        unsigned min = last_cqes >= 8 ? bc_batch_wait : 1;
        unsigned ms = (last_cqes >= 8 || s->nready || s->nrq > s->rq_head)
            ? bc_batch_wait_ms : 1000;
        long rc = bc_uring_wait(&s->uring.rings[0], min, ms);
        if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EBUSY) {
            fprintf(stderr, "io_uring_enter: %s\n", strerror((int)-rc));
            return;
        }

        uint64_t t = bc_now_ns();
        s->now_s = (uint32_t)(t / 1000000000ULL);
        s->app.now = (int64_t)time(NULL);
        s->writes_ok = 1;

        unsigned ncq = bc_uring_reap(&s->uring.rings[0], bc_cqe_cb, s);

        s->answers_left = bc_answers_per_batch;
        if (bc_issue_reads(s)) {
            ncq += bc_uring_reap(&s->uring.rings[0], bc_cqe_cb, s);
        }

        bc_run_lists(s);

        uint64_t tc = bc_now_ns();
        int failed = bc_db_commit(&s->app.db) < 0;
        uint64_t tc2 = bc_now_ns();
        if (tc2 - tc > s->max_commit_ns) s->max_commit_ns = tc2 - tc;

        bc_flush_replies(s, failed);

        for (unsigned ri = 0; ri < s->uring.n_rings; ri++) {
            struct bc_conn_pool *p = bc_conn_pool_of(s, ri);
            for (unsigned i = 0; i < p->nslots; i++) {
                struct bc_conn *c = &p->conns[i];
                if ((c->flags & (bc_cf_open | bc_cf_closing | bc_cf_rearm)) ==
                    (bc_cf_open | bc_cf_rearm)) {
                    bc_arm_poll(s, c);
                }
            }
        }

        s->batches++;
        s->cqes += ncq;

        uint64_t te = bc_now_ns();
        if (te - t > s->max_turn_ns) s->max_turn_ns = te - t;

        if (t - last_sweep > bc_sweep_ns) { bc_sweep(s); last_sweep = t; }
        if (t - last_stats > bc_stats_ns) { bc_stats(s, t - last_stats); last_stats = t; }
    }
}
