/* server.c: Single-threaded multi-ring io_uring HTTP server with in-process SQLite B-tree engine.
 *
 * Key optimizations:
 * - Pre-created io_uring instances with IORING_SETUP_SINGLE_ISSUER | DEFER_TASKRUN | TASKRUN_FLAG
 *   and MADV_HUGEPAGE provided buffer rings; Ring 0 polls ck_efd via IORING_OP_READ.
 * - Single combined submit+collect enter per active ring (zero worker eventfds, zero mid-pass enters).
 * - TCP_QUICKACK=0 on SO_REUSEPORT listeners + LATEACK IORING_OP_URING_CMD setsockopt refresh.
 * - Zero-malloc pre-parsed & JWT-verified request queue + bump arena; inline fast path + pipelining.
 * - Pure PCACHE2 64MB FastCache + "feed-wal" over "unix-excl" + nice-19 non-blocking PASSIVE checkpointer.
 * - Compact 24B struct conn + 2048-buf rings + mmap arena pool (<80 MB base RSS). */
#define _GNU_SOURCE
#include <ctype.h>
#include <getopt.h>
#include <sys/stat.h>

#include "uring.h"
#include "json.h"
#include "sha256.h"
#include "jwt.h"
#include "http.h"
#include "sqlite3.c"
#include "btree.h"
#include "db.h"
#include "app.h"

#define MAX_RINGS 12
#define NLISTEN 8
#define ACCEPT_DEPTH 512u
#define ARENA_CAP ((1u << 20) - (uint32_t)sizeof(struct arena))
#define ARENA_POOL 4
#define IDLE_SECONDS 300
#define GOVERNOR_NS 2000000000ULL
#define IDLE_SWEEP_NS 30000000000ULL
#define PEND_MAX (HTTP_MAX_HEAD + HTTP_MAX_BODY)
#define PEND_NONE 0xFFFFu
#define READS_PER_PASS 16384u

enum {
    UD_ACCEPT = 1,
    UD_RECV = 2,
    UD_READ = 3,
    UD_SEND = 4,
    UD_SHUTDOWN = 5,
    UD_CLOSE = 6,
    UD_CKPT_EFD = 7,
    UD_CANCEL = 8,
    UD_OPT = 9
};

#define UD(op, ring, slot, gen) \
    (((uint64_t)(op) << 56) | ((uint64_t)(ring) << 48) | ((uint64_t)(slot) << 32) | (uint64_t)(gen))
#define UD_OP(u)   ((unsigned)((u) >> 56))
#define UD_RING(u) ((unsigned)((u) >> 48) & 0xffu)
#define UD_SLOT(u) ((unsigned)((u) >> 32) & 0xffffu)
#define UD_GEN(u)  ((uint32_t)(u))

enum {
    CF_OPEN        = 1 << 0,
    CF_SENDING     = 1 << 1,
    CF_CLOSING     = 1 << 2,
    CF_CLOSE_AFTER = 1 << 3,
    CF_REPLIED     = 1 << 4,
    CF_READY       = 1 << 5,
    CF_HELD        = 1 << 6,
    CF_REARM       = 1 << 7,
    CF_MULTI       = 1 << 8,
    CF_RQ          = 1 << 9,
    CF_RECVING     = 1 << 10
};

struct arena {
    struct arena *next;
    uint32_t refs;
    uint32_t used;
    uint32_t cap;
    uint8_t data[];
};

struct pbuf {
    uint8_t *buf;
    uint32_t len;
    uint32_t cap;
    uint16_t next_free;
};

struct conn {
    uint32_t gen;
    uint32_t last_active;
    uint16_t flags;
    uint8_t ring;
    uint8_t pad;
    uint16_t pend_idx;
    uint16_t pad2;
    struct arena *send_arena;
};

struct rs {
    struct ring r;
    struct conn *conns;
    unsigned open;
    unsigned max_slot;
    unsigned recvs_in_flight;
    unsigned accept_in_flight[NLISTEN];
    unsigned accept_total;
    uint8_t accept_full;
    uint8_t created;
    struct conn **rearm;
    unsigned nrearm;
    unsigned rearm_cap;
};

struct reply {
    struct conn *c;
    uint32_t gen;
    uint32_t req_ms;
    struct arena *a;
    uint8_t *p;
    unsigned len;
    uint8_t db;
    uint8_t close;
};

struct qitem {
    struct conn *c;
    uint32_t gen;
    uint32_t ms;
    struct parsed_req pr;
};

struct cref {
    struct conn *c;
    uint32_t gen;
};

static struct rs R[MAX_RINGS];
static unsigned NR = 6;
static unsigned NR_MAX = MAX_RINGS;
static unsigned active_rings = 1;
static unsigned ring_slots = 65471;
static unsigned ring_bufs = 2048;
static unsigned ring_buf_size = 1024;
static int lfd[NLISTEN] = {-1, -1, -1, -1, -1, -1, -1, -1};
static unsigned accept_ring = 0;
static unsigned accept_margin = 4096;
static int defer_taskrun = 1;
static int ring_grow_failed = 0;

static struct app A;
static uint32_t now_s = 0;
static uint32_t now_ms = 0;
static volatile sig_atomic_t g_stop = 0;
static uint64_t g_ck_efd_buf = 0;

static struct arena *arena_pool = NULL;
static unsigned arena_pool_n = 0;
static struct arena *cur_arena = NULL;

static struct qarena g_qa = {NULL, 0, 0};
static struct qarena g_qa_swap = {NULL, 0, 0};

static struct pbuf *pend_pool = NULL;
static unsigned pend_cap_n = 0;
static uint16_t pend_free_head = PEND_NONE;
static unsigned pend_free_n = 0;

static struct reply *replies = NULL;
static unsigned nreplies = 0, replies_cap = 0;

static struct qitem *ready = NULL;
static unsigned nready = 0, ready_cap = 0;

static struct qitem *held = NULL;
static unsigned nheld = 0, held_cap = 0;

static struct cref *rq = NULL;
static unsigned rq_head = 0, nrq = 0, rq_cap = 0;
static uint64_t reads_done = 0;

static unsigned answers_per_pass = 256;
static unsigned answers_left = 256;
static unsigned wait_min = 64;
static uint64_t wait_ns_cfg = 1000000ULL;       /* 1 ms */
static uint64_t write_win_ns = 15000000ULL;     /* 15 ms */
static uint32_t late_ack_ms = 30;
static int late_ack_enabled = 1;
static uint32_t lifo_age_ms = 8000;
static uint64_t last_write_commit_ns = 0;
static uint64_t last_batch_cqes = 0;
static int in_run_ready = 0;

static inline unsigned conn_slot(const struct conn *c) {
    return (unsigned)(c - R[c->ring].conns);
}

/* ---------------- mmap arena management ---------------- */

static struct arena *arena_get(void) {
    if (arena_pool) {
        struct arena *a = arena_pool;
        arena_pool = a->next;
        arena_pool_n--;
        a->next = NULL;
        a->refs = 1;
        a->used = 0;
        return a;
    }
    size_t map_sz = sizeof(struct arena) + ARENA_CAP;
    void *m = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap arena"); exit(1); }
    struct arena *a = (struct arena *)m;
    a->next = NULL;
    a->refs = 1;
    a->used = 0;
    a->cap = ARENA_CAP;
    return a;
}

static inline void arena_unref(struct arena *a) {
    if (!a) return;
    if (--a->refs == 0) {
        if (a == cur_arena) {
            a->used = 0;
            a->refs = 1;
            return;
        }
        if (arena_pool_n < ARENA_POOL && a->cap == ARENA_CAP) {
            a->next = arena_pool;
            arena_pool = a;
            arena_pool_n++;
        } else {
            munmap(a, sizeof(*a) + a->cap);
        }
    }
}

static uint8_t *arena_space(struct arena **ap) {
    if (!cur_arena || cur_arena->cap - cur_arena->used < RESP_MAX) {
        if (cur_arena) arena_unref(cur_arena);
        cur_arena = arena_get();
    }
    *ap = cur_arena;
    return cur_arena->data + cur_arena->used;
}

/* ---------------- side pending-buffer pool (split/pipelined packets only) ---------------- */

static struct pbuf *pend_ensure(struct conn *c) {
    if (c->pend_idx != PEND_NONE) return &pend_pool[c->pend_idx];
    uint16_t idx;
    if (pend_free_head != PEND_NONE) {
        idx = pend_free_head;
        pend_free_head = pend_pool[idx].next_free;
        pend_free_n--;
    } else {
        if (pend_cap_n >= 65534u) return NULL;
        idx = (uint16_t)pend_cap_n++;
        pend_pool = (struct pbuf *)realloc(pend_pool, (size_t)pend_cap_n * sizeof(*pend_pool));
        if (!pend_pool) { perror("realloc pend_pool"); exit(1); }
        memset(&pend_pool[idx], 0, sizeof(pend_pool[idx]));
    }
    pend_pool[idx].len = 0;
    pend_pool[idx].next_free = PEND_NONE;
    c->pend_idx = idx;
    return &pend_pool[idx];
}

static void pend_release(struct conn *c) {
    if (c->pend_idx == PEND_NONE) return;
    uint16_t idx = c->pend_idx;
    struct pbuf *pb = &pend_pool[idx];
    pb->len = 0;
    if (pb->buf && (pb->cap > 4096u || pend_free_n >= 64u)) {
        free(pb->buf);
        pb->buf = NULL;
        pb->cap = 0;
    }
    pb->next_free = pend_free_head;
    pend_free_head = idx;
    pend_free_n++;
    c->pend_idx = PEND_NONE;
}

static void pend_append(struct conn *c, const uint8_t *buf, unsigned len) {
    if (len == 0) return;
    struct pbuf *pb = pend_ensure(c);
    if (!pb) return;
    if ((uint64_t)pb->len + len > PEND_MAX + HTTP_MAX_HEAD) {
        pend_release(c);
        return;
    }
    if (pb->len + len > pb->cap) {
        uint32_t ncap = pb->cap ? pb->cap * 2u : 2048u;
        while (ncap < pb->len + len) ncap *= 2u;
        uint8_t *np = (uint8_t *)realloc(pb->buf, ncap);
        if (!np) { perror("realloc pb"); exit(1); }
        pb->buf = np;
        pb->cap = ncap;
    }
    memcpy(pb->buf + pb->len, buf, len);
    pb->len += len;
}

static void pend_set(struct conn *c, const uint8_t *buf, unsigned len) {
    if (len == 0) {
        pend_release(c);
        return;
    }
    if (c->pend_idx != PEND_NONE) pend_pool[c->pend_idx].len = 0;
    pend_append(c, buf, len);
}

/* ---------------- queue & arena helpers ---------------- */

static inline void ready_push_item(const struct qitem *qi) {
    struct conn *c = qi->c;
    if (c->flags & (CF_READY | CF_HELD | CF_CLOSING)) return;
    if (nready == ready_cap) {
        ready_cap = ready_cap ? ready_cap * 2u : 1024u;
        ready = (struct qitem *)realloc(ready, ready_cap * sizeof(*ready));
        if (!ready) { perror("realloc ready"); exit(1); }
    }
    c->flags |= CF_READY;
    ready[nready++] = *qi;
}

static inline void held_push_item(const struct qitem *qi) {
    struct conn *c = qi->c;
    if (c->flags & (CF_HELD | CF_CLOSING)) return;
    if (nheld == held_cap) {
        held_cap = held_cap ? held_cap * 2u : 512u;
        held = (struct qitem *)realloc(held, held_cap * sizeof(*held));
        if (!held) { perror("realloc held"); exit(1); }
    }
    c->flags |= CF_HELD;
    held[nheld++] = *qi;
}

static void qarena_compact(void) {
    g_qa_swap.used = 0;
    for (unsigned i = 0; i < nready; i++) {
        struct qitem *q = &ready[i];
        if (q->pr.route == RT_CREATE && q->c->gen == q->gen) {
            uint32_t off;
            qarena_append(&g_qa_swap, g_qa.buf + q->pr.name_off, q->pr.name_len, &off);
            q->pr.name_off = off;
            qarena_append(&g_qa_swap, g_qa.buf + q->pr.body_off, q->pr.body_len, &off);
            q->pr.body_off = off;
        }
    }
    for (unsigned i = 0; i < nheld; i++) {
        struct qitem *q = &held[i];
        if (q->pr.route == RT_CREATE && q->c->gen == q->gen) {
            uint32_t off;
            qarena_append(&g_qa_swap, g_qa.buf + q->pr.name_off, q->pr.name_len, &off);
            q->pr.name_off = off;
            qarena_append(&g_qa_swap, g_qa.buf + q->pr.body_off, q->pr.body_len, &off);
            q->pr.body_off = off;
        }
    }
    struct qarena tmp = g_qa;
    g_qa = g_qa_swap;
    g_qa_swap = tmp;
    g_qa_swap.used = 0;
}

static inline void rearm_push(struct rs *rs, struct conn *c) {
    if (c->flags & (CF_REARM | CF_CLOSING)) return;
    c->flags |= CF_REARM;
    if (rs->nrearm == rs->rearm_cap) {
        rs->rearm_cap = rs->rearm_cap ? rs->rearm_cap * 2u : 1024u;
        rs->rearm = (struct conn **)realloc(rs->rearm, rs->rearm_cap * sizeof(*rs->rearm));
        if (!rs->rearm) { perror("realloc rearm"); exit(1); }
    }
    rs->rearm[rs->nrearm++] = c;
}

static inline void rq_push_raw(struct conn *c, uint32_t gen) {
    if (nrq == rq_cap) {
        if (rq_head > 0) {
            unsigned rem = nrq - rq_head;
            if (rem > 0) memmove(rq, rq + rq_head, rem * sizeof(rq[0]));
            nrq = rem;
            rq_head = 0;
        }
        if (nrq == rq_cap) {
            rq_cap = rq_cap ? rq_cap * 2u : 4096u;
            rq = (struct cref *)realloc(rq, rq_cap * sizeof(*rq));
            if (!rq) { perror("realloc rq"); exit(1); }
        }
    }
    rq[nrq].c = c;
    rq[nrq].gen = gen;
    nrq++;
}

static inline void rq_push(struct conn *c) {
    if (c->flags & (CF_RQ | CF_MULTI | CF_RECVING | CF_CLOSING)) return;
    c->flags |= CF_RQ;
    rq_push_raw(c, c->gen);
}

/* ---------------- connection close & multi-listener accept ---------------- */

static void conn_close(struct conn *c) {
    if (!(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) return;
    c->flags |= CF_CLOSING;
    pend_release(c);
    struct ring *r = &R[c->ring].r;
    unsigned slot = conn_slot(c);
    if (c->flags & CF_SENDING) {
        struct io_uring_sqe *s = ring_sqe(r);
        s->opcode = IORING_OP_SHUTDOWN;
        s->flags = IOSQE_FIXED_FILE;
        s->fd = (int)slot;
        s->len = SHUT_RDWR;
        s->user_data = UD(UD_SHUTDOWN, c->ring, slot, c->gen);
    } else {
        sqe_shutdown_close(r, slot,
                           UD(UD_SHUTDOWN, c->ring, slot, c->gen),
                           UD(UD_CLOSE, c->ring, slot, c->gen));
    }
}

static inline void finish_conn(struct conn *c) {
    if (c->flags & (CF_REPLIED | CF_SENDING | CF_READY | CF_HELD)) {
        c->flags |= CF_CLOSE_AFTER;
    } else {
        conn_close(c);
    }
}

static int ring_create_at(unsigned i) {
    if (R[i].created) return 0;
    const char *err = NULL;
    unsigned sq_entries = 4096;
    unsigned cq_entries = ring_slots > 4096 ? ring_slots : 4096;
    if (cq_entries > 65536) cq_entries = 65536;
    while (ring_init(&R[i].r, sq_entries, cq_entries, ring_slots,
                     ring_bufs, ring_buf_size, defer_taskrun, &err) != 0) {
        if (errno == ENOMEM && cq_entries > 8192) {
            cq_entries /= 2;
            continue;
        }
        fprintf(stderr, "ring_init[%u] failed (%s): %s\n", i, err ? err : "err", strerror(errno));
        return -1;
    }
    R[i].conns = (struct conn *)calloc(ring_slots, sizeof(struct conn));
    if (!R[i].conns) { perror("calloc conns"); return -1; }
    for (unsigned s = 0; s < ring_slots; s++) R[i].conns[s].pend_idx = PEND_NONE;
    if (i == 0) {
        prefault(R[i].conns, (size_t)ring_slots * sizeof(struct conn));
        prefault(R[i].r.bufs, (size_t)ring_bufs * ring_buf_size);
    }
    R[i].created = 1;
    return 0;
}

static void fill_accepts_on(unsigned ri) {
    struct rs *rs = &R[ri];
    if (!rs->created || rs->accept_full) return;
    unsigned lim = rs->r.nslots > accept_margin ? rs->r.nslots - accept_margin : rs->r.nslots;
    if (rs->open + rs->accept_total >= lim) return;
    unsigned room = lim - (rs->open + rs->accept_total);
    unsigned avail_for_ring = lim - rs->open;
    unsigned target_total = avail_for_ring < ACCEPT_DEPTH ? avail_for_ring : ACCEPT_DEPTH;
    if (rs->accept_total >= target_total) return;
    unsigned per_l = (target_total + NLISTEN - 1u) / NLISTEN;
    if (per_l < 1u) per_l = 1u;
    for (unsigned l = 0; l < NLISTEN && rs->accept_total < target_total && room > 0; l++) {
        if (lfd[l] < 0) continue;
        while (rs->accept_in_flight[l] < per_l && rs->accept_total < target_total && room > 0) {
            struct io_uring_sqe *s = ring_sqe(&rs->r);
            sqe_accept_once(s, lfd[l], UD(UD_ACCEPT, ri, l, 0));
            rs->accept_in_flight[l]++;
            rs->accept_total++;
            room--;
        }
    }
}

static void maybe_rotate_accept(void) {
    struct rs *cur = &R[accept_ring];
    unsigned lim = cur->r.nslots > accept_margin ? cur->r.nslots - accept_margin : cur->r.nslots;
    if (!cur->accept_full && cur->open < lim) {
        fill_accepts_on(accept_ring);
        return;
    }
    unsigned best = accept_ring;
    unsigned best_open = cur->r.nslots;
    for (unsigned i = 0; i < NR; i++) {
        unsigned rlim = R[i].r.nslots > accept_margin ? R[i].r.nslots - accept_margin : R[i].r.nslots;
        if (R[i].created && !R[i].accept_full && R[i].open < rlim && R[i].open < best_open) {
            best = i;
            best_open = R[i].open;
        }
    }
    if (best == accept_ring && NR < NR_MAX && !ring_grow_failed) {
        if (ring_create_at(NR) == 0) {
            best = NR;
            NR++;
        } else {
            ring_grow_failed = 1;
        }
    }
    if (best != accept_ring) {
        accept_ring = best;
        if (accept_ring + 1u > active_rings) active_rings = accept_ring + 1u;
        fill_accepts_on(accept_ring);
    }
}

/* ---------------- write gating & request execution ---------------- */

static inline int current_writes_ok(uint64_t now) {
    if (unlikely(A.db.writes_held)) return 0;
    if (write_win_ns == 0 || last_write_commit_ns == 0 || A.db.wrote) return 1;
    return (now - last_write_commit_ns) >= write_win_ns;
}

static void queue_reply(struct conn *c, struct arena *a, const struct resp *rp, uint32_t req_ms) {
    if (nreplies == replies_cap) {
        replies_cap = replies_cap ? replies_cap * 2u : 1024u;
        replies = (struct reply *)realloc(replies, replies_cap * sizeof(*replies));
        if (!replies) { perror("realloc replies"); exit(1); }
    }
    if (a) a->refs++;
    c->flags |= CF_REPLIED;
    replies[nreplies++] = (struct reply){
        .c = c,
        .gen = c->gen,
        .req_ms = req_ms,
        .a = a,
        .p = rp->p,
        .len = rp->n,
        .db = rp->db,
        .close = rp->close
    };
}

static int coalesce_tail_bytes(struct conn *c, const uint8_t *buf, unsigned len,
                               struct arena *a, struct resp *rp, int writes_ok, unsigned *pos_out) {
    int handled = 1;
    if (rp->is_static || rp->close || len == 0) {
        *pos_out = 0;
        return handled;
    }
    uint8_t *first_p = rp->p;
    unsigned first_n = rp->n;
    uint8_t any_db = rp->db;
    unsigned pos = 0;
    while (handled < 16 && pos < len && !rp->close) {
        if (a != cur_arena || a->cap - a->used < RESP_MAX) break;
        struct req r2;
        int pr2 = http_parse(buf + pos, len - pos, &r2);
        if (pr2 <= 0) break;
        struct parsed_req sub_pr;
        uint32_t qa_mark = g_qa.used;
        struct resp rp2;
        if (app_parse_and_auth(&A, &r2, &sub_pr, &g_qa, &rp2)) {
            if (sub_pr.is_write && !writes_ok) {
                g_qa.used = qa_mark;
                break;
            }
            uint8_t *next_free = a->data + a->used;
            app_exec_parsed(&A, &sub_pr, &g_qa, next_free, &rp2);
            g_qa.used = qa_mark;
        }
        if (rp2.is_static) memcpy(first_p + first_n, rp2.p, rp2.n);
        else memmove(first_p + first_n, rp2.p, rp2.n);
        first_n += rp2.n;
        a->used = (uint32_t)((first_p - a->data) + first_n);
        pos += r2.total_len;
        handled++;
        any_db |= rp2.db;
        if (rp2.close) rp->close = 1;
    }
    rp->n = first_n;
    rp->db = any_db;
    *pos_out = pos;
    return handled;
}

static void execute_queued_item(struct conn *c, const struct parsed_req *pr, uint32_t req_ms,
                                int writes_ok, unsigned *handled_out) {
    if (unlikely(pr->route == RT_STATIC_ERR)) {
        struct resp rp = {0};
        rp.close = pr->close;
        resp_static(&rp, (int)pr->id);
        A.n_req++;
        queue_reply(c, NULL, &rp, req_ms);
        *handled_out = 1;
        return;
    }
    struct arena *a = NULL;
    uint8_t *free_buf = arena_space(&a);
    struct resp rp;
    app_exec_parsed(&A, pr, &g_qa, free_buf, &rp);
    if (rp.advance > 0) a->used += rp.advance;

    int handled = 1;
    if (!rp.is_static && !rp.close && c->pend_idx != PEND_NONE) {
        struct pbuf *pb = &pend_pool[c->pend_idx];
        unsigned extra = 0;
        handled = coalesce_tail_bytes(c, pb->buf, pb->len, a, &rp, writes_ok, &extra);
        if (extra == pb->len) {
            pend_release(c);
        } else if (extra > 0) {
            memmove(pb->buf, pb->buf + extra, pb->len - extra);
            pb->len -= extra;
        }
    }
    queue_reply(c, rp.is_static ? NULL : a, &rp, req_ms);
    *handled_out = (unsigned)handled;
}

/* Parses first complete HTTP request on an idle connection and either executes inline
 * or enqueues a pre-parsed qitem into ready[] or held[]. Returns number of requests answered inline. */
static int dispatch_conn_buf(struct conn *c, const uint8_t *in_buf, unsigned in_len,
                             int can_inline, int writes_ok) {
    if (!(c->flags & CF_OPEN) ||
        (c->flags & (CF_CLOSING | CF_SENDING | CF_REPLIED | CF_READY | CF_HELD))) {
        if (in_len > 0) pend_append(c, in_buf, in_len);
        return 0;
    }

    const uint8_t *src = in_buf;
    unsigned len = in_len;
    int src_is_pend = 0;
    if (c->pend_idx != PEND_NONE) {
        if (in_len > 0) pend_append(c, in_buf, in_len);
        struct pbuf *pb = &pend_pool[c->pend_idx];
        src = pb->buf;
        len = pb->len;
        src_is_pend = 1;
    }
    if (len == 0) return 0;

    struct req r;
    int pr_rc = http_parse(src, len, &r);
    if (pr_rc == 0) {
        if (!src_is_pend && in_len > 0) pend_append(c, in_buf, in_len);
        return 0;
    }
    if (pr_rc < 0) {
        struct resp rp = {0};
        rp.close = 1;
        resp_static(&rp, SR_400_BAD);
        pend_release(c);
        queue_reply(c, NULL, &rp, now_ms);
        return 1;
    }

    struct parsed_req pr;
    struct resp err_rp;
    uint32_t qa_mark = g_qa.used;
    int ok = app_parse_and_auth(&A, &r, &pr, &g_qa, &err_rp);
    unsigned consumed = r.total_len;

    int inline_now = can_inline && (!ok || !pr.is_write || writes_ok);
    if (inline_now) {
        if (!ok) {
            A.n_req++;
            unsigned rem = len - consumed;
            if (rem == 0) pend_release(c);
            else if (src_is_pend) {
                struct pbuf *pb = &pend_pool[c->pend_idx];
                memmove(pb->buf, pb->buf + consumed, rem);
                pb->len = rem;
            } else {
                pend_set(c, src + consumed, rem);
            }
            queue_reply(c, NULL, &err_rp, now_ms);
            return 1;
        }
        struct arena *a = NULL;
        uint8_t *free_buf = arena_space(&a);
        struct resp rp;
        app_exec_parsed(&A, &pr, &g_qa, free_buf, &rp);
        g_qa.used = qa_mark;
        if (rp.advance > 0) a->used += rp.advance;

        unsigned extra = 0;
        int handled = coalesce_tail_bytes(c, src + consumed, len - consumed, a, &rp, writes_ok, &extra);
        consumed += extra;

        unsigned rem = len - consumed;
        if (rem == 0) {
            pend_release(c);
        } else if (src_is_pend) {
            struct pbuf *pb = &pend_pool[c->pend_idx];
            memmove(pb->buf, pb->buf + consumed, rem);
            pb->len = rem;
        } else {
            pend_set(c, src + consumed, rem);
        }
        queue_reply(c, rp.is_static ? NULL : a, &rp, now_ms);
        return handled;
    }

    /* Enqueue already-parsed & JWT-verified qitem without touching malloc on normal pipe=1. */
    unsigned rem = len - consumed;
    if (rem == 0) {
        pend_release(c);
    } else if (src_is_pend) {
        struct pbuf *pb = &pend_pool[c->pend_idx];
        memmove(pb->buf, pb->buf + consumed, rem);
        pb->len = rem;
    } else {
        pend_set(c, src + consumed, rem);
    }
    struct qitem qi = {.c = c, .gen = c->gen, .ms = now_ms, .pr = pr};
    if (ok && pr.is_write && !writes_ok) held_push_item(&qi);
    else ready_push_item(&qi);
    return 0;
}

/* ---------------- CQE handlers & pure userspace multi-ring reap ---------------- */

static void on_accept(unsigned ri, struct io_uring_cqe *cqe) {
    struct rs *rs = &R[ri];
    unsigned li = UD_SLOT(cqe->user_data);
    if (li < NLISTEN && rs->accept_in_flight[li] > 0) rs->accept_in_flight[li]--;
    if (rs->accept_total > 0) rs->accept_total--;
    if (cqe->res < 0) {
        if (cqe->res == -ENFILE || cqe->res == -EMFILE) rs->accept_full = 1;
        return;
    }
    unsigned slot = (unsigned)cqe->res;
    if (slot >= rs->r.nslots) return;
    struct conn *c = &rs->conns[slot];
    if (c->flags & CF_OPEN) {
        pend_release(c);
        c->flags = 0;
        if (rs->open > 0) rs->open--;
    }
    c->gen++;
    c->flags = CF_OPEN | CF_MULTI;
    c->ring = (uint8_t)ri;
    c->last_active = now_s;
    c->pend_idx = PEND_NONE;
    c->send_arena = NULL;
    rs->open++;
    if (slot + 1u > rs->max_slot) rs->max_slot = slot + 1u;
    if (rs->open + accept_margin >= rs->r.nslots) rs->accept_full = 1;

    struct io_uring_sqe *s = ring_sqe(&rs->r);
    sqe_recv_multishot(s, slot, UD(UD_RECV, ri, slot, c->gen));
}

static void on_recv(unsigned ri, struct io_uring_cqe *cqe, int multishot, int writes_ok) {
    struct rs *rs = &R[ri];
    unsigned slot = UD_SLOT(cqe->user_data);
    struct conn *c = &rs->conns[slot];
    int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
    unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    int more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    int live = (c->gen == UD_GEN(cqe->user_data)) &&
               ((c->flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN);

    if (!multishot) {
        if (rs->recvs_in_flight > 0) rs->recvs_in_flight--;
        if (live) c->flags &= (uint16_t)~CF_RECVING;
        if (cqe->res > 0 || cqe->res == -EAGAIN) reads_done++;
    } else if (live && !more) {
        c->flags &= (uint16_t)~CF_MULTI;
    }

    if (live && cqe->res > 0 && has_buf) {
        c->last_active = now_s;
        const uint8_t *buf = ring_buf_ptr(&rs->r, bid);
        unsigned blen = (unsigned)cqe->res;
        int can_inline = (!in_run_ready && nready == 0 && answers_left > 0);
        int h = dispatch_conn_buf(c, buf, blen, can_inline, writes_ok);
        if (h > 0) {
            answers_left = (unsigned)h >= answers_left ? 0 : answers_left - (unsigned)h;
        }
    }

    if (has_buf) ring_buf_add(&rs->r, bid);

    if (!live) return;
    if (cqe->res > 0) {
        if (multishot) {
            if (!more) rq_push(c);
        } else if ((unsigned)cqe->res == rs->r.buf_size) {
            rq_push(c);
        } else if (!(c->flags & (CF_MULTI | CF_RECVING | CF_RQ))) {
            rearm_push(rs, c);
        }
        return;
    }
    if (cqe->res == 0) {
        finish_conn(c);
        return;
    }
    if (cqe->res == -ENOBUFS) {
        rq_push(c);
        return;
    }
    if (cqe->res == -EAGAIN) {
        if (!(c->flags & (CF_MULTI | CF_RECVING | CF_RQ))) rearm_push(rs, c);
        return;
    }
    if (cqe->res == -ECANCELED) {
        if (!more && !(c->flags & (CF_MULTI | CF_RECVING | CF_RQ))) rearm_push(rs, c);
        return;
    }
    finish_conn(c);
}

static void on_send(unsigned ri, struct io_uring_cqe *cqe, int writes_ok) {
    struct rs *rs = &R[ri];
    struct conn *c = &rs->conns[UD_SLOT(cqe->user_data)];
    if (c->gen != UD_GEN(cqe->user_data)) return;
    c->flags &= (uint16_t)~CF_SENDING;
    if (c->send_arena) {
        arena_unref(c->send_arena);
        c->send_arena = NULL;
    }
    if (c->flags & CF_CLOSING) {
        struct io_uring_sqe *s = ring_sqe(&rs->r);
        unsigned slot = conn_slot(c);
        s->opcode = IORING_OP_CLOSE;
        s->fd = 0;
        s->file_index = slot + 1u;
        s->user_data = UD(UD_CLOSE, c->ring, slot, c->gen);
        return;
    }
    if (cqe->res < 0 || (c->flags & CF_CLOSE_AFTER)) {
        conn_close(c);
        return;
    }
    if (c->pend_idx != PEND_NONE && !(c->flags & (CF_READY | CF_HELD | CF_REPLIED))) {
        (void)dispatch_conn_buf(c, NULL, 0, 0, writes_ok);
    }
}

static void on_close(unsigned ri, struct io_uring_cqe *cqe) {
    struct rs *rs = &R[ri];
    struct conn *c = &rs->conns[UD_SLOT(cqe->user_data)];
    if (c->gen != UD_GEN(cqe->user_data)) return;
    pend_release(c);
    c->flags = 0;
    if (rs->open > 0) rs->open--;
    if (rs->accept_full && rs->open + 2u * accept_margin < rs->r.nslots) {
        rs->accept_full = 0;
    }
}

static uint64_t reap_ring(unsigned ri, int writes_ok) {
    struct ring *r = &R[ri].r;
    uint64_t count = 0;
    unsigned head = *r->cq_head;
    unsigned tail = LOAD_ACQ(r->cq_tail);
    while (head != tail) {
        struct io_uring_cqe *cqe = &r->cqes[head & r->cq_mask];
        unsigned op = UD_OP(cqe->user_data);
        switch (op) {
        case UD_RECV:
            on_recv(ri, cqe, 1, writes_ok);
            break;
        case UD_READ:
            on_recv(ri, cqe, 0, writes_ok);
            break;
        case UD_SEND:
            on_send(ri, cqe, writes_ok);
            break;
        case UD_ACCEPT:
            on_accept(ri, cqe);
            break;
        case UD_CLOSE:
            on_close(ri, cqe);
            break;
        case UD_OPT:
            if (cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP) late_ack_enabled = 0;
            break;
        case UD_CKPT_EFD:
            if (A.db.ck_efd >= 0 && !g_stop) {
                struct io_uring_sqe *s = ring_sqe(&R[0].r);
                sqe_read_efd(s, A.db.ck_efd, &g_ck_efd_buf, UD(UD_CKPT_EFD, 0, 0, 0));
            }
            break;
        case UD_SHUTDOWN:
        case UD_CANCEL:
        default:
            break;
        }
        head++;
        count++;
        if ((count & 255u) == 0) {
            STORE_REL(r->cq_head, head);
            if (r->br_pending > 0) ring_buf_publish(r);
        }
    }
    if (count > 0) STORE_REL(r->cq_head, head);
    if (r->br_pending > 0) ring_buf_publish(r);
    return count;
}

static uint64_t reap_all(int writes_ok) {
    uint64_t total = 0;
    for (unsigned i = 0; i < active_rings; i++) {
        total += reap_ring(i, writes_ok);
    }
    return total;
}

static unsigned issue_reads(unsigned budget) {
    unsigned issued = 0;
    unsigned avail = nrq - rq_head;
    for (unsigned k = 0; k < avail && issued < budget; k++) {
        struct cref e = rq[rq_head++];
        struct conn *c = e.c;
        if (c->gen != e.gen || !(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
        struct rs *rs = &R[c->ring];
        if (c->flags & (CF_MULTI | CF_RECVING)) {
            c->flags &= (uint16_t)~CF_RQ;
            continue;
        }
        if (rs->recvs_in_flight >= ring_bufs) {
            rq_push_raw(e.c, e.gen);
            continue;
        }
        c->flags = (uint16_t)((c->flags & ~CF_RQ) | CF_RECVING);
        rs->recvs_in_flight++;
        unsigned slot = conn_slot(c);
        sqe_recv_once(ring_sqe(&rs->r), slot, UD(UD_READ, c->ring, slot, c->gen));
        issued++;
    }
    if (rq_head == nrq) {
        rq_head = nrq = 0;
    } else if (rq_head > nrq / 2u) {
        memmove(rq, rq + rq_head, (nrq - rq_head) * sizeof(rq[0]));
        nrq -= rq_head;
        rq_head = 0;
    }
    return issued;
}

static uint64_t read_rounds(int writes_ok) {
    uint64_t n = 0;
    unsigned budget = READS_PER_PASS;
    while (budget > 0 && rq_head < nrq) {
        unsigned step = budget < ring_bufs ? budget : ring_bufs;
        unsigned k = issue_reads(step);
        if (k == 0) break;
        budget -= k;
        uint64_t before = reads_done;
        for (unsigned i = 0; i < active_rings; i++) {
            if (ring_needs_enter(&R[i].r)) ring_enter(&R[i].r, 0, 0);
        }
        n += reap_all(writes_ok);
        if (reads_done == before) break;
    }
    return n;
}

static void rearm_all(void) {
    for (unsigned i = 0; i < active_rings; i++) {
        struct rs *rs = &R[i];
        if (!rs->nrearm) continue;
        for (unsigned k = 0; k < rs->nrearm; k++) {
            struct conn *c = rs->rearm[k];
            c->flags &= (uint16_t)~CF_REARM;
            if ((c->flags & (CF_OPEN | CF_CLOSING | CF_MULTI | CF_RECVING | CF_RQ)) == CF_OPEN) {
                c->flags |= CF_MULTI;
                struct io_uring_sqe *s = ring_sqe(&rs->r);
                sqe_recv_multishot(s, conn_slot(c), UD(UD_RECV, c->ring, conn_slot(c), c->gen));
            }
        }
        rs->nrearm = 0;
    }
}

/* ---------------- ready/held schedulers & batch flush ---------------- */

static void drain_held(int writes_ok) {
    if (!writes_ok || nheld == 0) return;
    unsigned out = 0;
    unsigned done = 0;
    for (unsigned i = 0; i < nheld; i++) {
        struct qitem *q = &held[i];
        struct conn *c = q->c;
        if (c->gen != q->gen || !(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
        if (done >= 256u) {
            held[out++] = *q;
            continue;
        }
        c->flags &= (uint16_t)~CF_HELD;
        if (!(c->flags & (CF_SENDING | CF_REPLIED))) {
            unsigned h = 0;
            execute_queued_item(c, &q->pr, q->ms, 1, &h);
            done += h;
            answers_left = h >= answers_left ? 0 : answers_left - h;
            if ((c->flags & CF_CLOSE_AFTER) && !(c->flags & (CF_REPLIED | CF_SENDING | CF_HELD))) {
                conn_close(c);
            }
        } else {
            held[out++] = *q;
            c->flags |= CF_HELD;
        }
    }
    nheld = out;
}

static void run_ready(int writes_ok) {
    if (nready == 0 || answers_left == 0) return;
    in_run_ready = 1;
    unsigned orig_n = nready;
    unsigned old_quota = 0;
    if (orig_n > answers_left) {
        uint32_t oldest_age = now_ms >= ready[0].ms ? (now_ms - ready[0].ms) : 0;
        if (oldest_age >= lifo_age_ms * 2u) old_quota = answers_left;
        else if (oldest_age >= lifo_age_ms) old_quota = (answers_left + 1u) / 2u;
        else old_quota = (answers_left + 7u) / 8u;
    }

    unsigned i = 0;
    unsigned k = orig_n;

    while (i < k && answers_left > 0 && i < old_quota) {
        struct qitem q = ready[i++];
        struct conn *c = q.c;
        if (c->gen != q.gen || !(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
        c->flags &= (uint16_t)~CF_READY;
        if (c->flags & (CF_SENDING | CF_REPLIED | CF_HELD)) continue;
        if (q.pr.is_write && !writes_ok) {
            held_push_item(&q);
            continue;
        }
        unsigned h = 0;
        execute_queued_item(c, &q.pr, q.ms, writes_ok, &h);
        answers_left = h >= answers_left ? 0 : answers_left - h;
        if ((c->flags & CF_CLOSE_AFTER) && !(c->flags & (CF_REPLIED | CF_SENDING | CF_HELD))) {
            conn_close(c);
        }
    }

    while (i < k && answers_left > 0) {
        k--;
        struct qitem q = ready[k];
        struct conn *c = q.c;
        if (c->gen != q.gen || !(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
        c->flags &= (uint16_t)~CF_READY;
        if (c->flags & (CF_SENDING | CF_REPLIED | CF_HELD)) continue;
        if (q.pr.is_write && !writes_ok) {
            held_push_item(&q);
            continue;
        }
        unsigned h = 0;
        execute_queued_item(c, &q.pr, q.ms, writes_ok, &h);
        answers_left = h >= answers_left ? 0 : answers_left - h;
        if ((c->flags & CF_CLOSE_AFTER) && !(c->flags & (CF_REPLIED | CF_SENDING | CF_HELD))) {
            conn_close(c);
        }
    }

    unsigned rem_mid = k - i;
    if (rem_mid > 0 && i > 0) {
        memmove(ready, ready + i, rem_mid * sizeof(ready[0]));
    }
    unsigned new_n = nready - orig_n;
    if (new_n > 0 && rem_mid != orig_n) {
        memmove(ready + rem_mid, ready + orig_n, new_n * sizeof(ready[0]));
    }
    nready = rem_mid + new_n;
    in_run_ready = 0;
}

static void flush_replies(int had_writes, int db_err, uint64_t now) {
    if (had_writes && !db_err) {
        last_write_commit_ns = now;
    }
    for (unsigned i = 0; i < nreplies; i++) {
        struct reply *rp = &replies[i];
        struct conn *c = rp->c;
        if (c->gen != rp->gen || !(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) {
            arena_unref(rp->a);
            continue;
        }
        c->flags &= (uint16_t)~CF_REPLIED;

        const uint8_t *ptr = rp->p;
        unsigned len = rp->len;
        struct arena *keep_arena = rp->a;
        if (db_err && rp->db) {
            int cl = rp->close ? 1 : 0;
            ptr = S_RESP[SR_500][cl];
            len = S_LEN[SR_500][cl];
            arena_unref(keep_arena);
            keep_arena = NULL;
        }

        c->flags |= CF_SENDING;
        if (rp->close) c->flags |= CF_CLOSE_AFTER;
        c->send_arena = keep_arena;

        unsigned slot = conn_slot(c);
        struct ring *r = &R[c->ring].r;
        struct io_uring_sqe *s = ring_sqe(r);
        sqe_send(s, slot, ptr, len, UD(UD_SEND, c->ring, slot, c->gen));

        if (late_ack_enabled && late_ack_ms > 0 && !rp->close &&
            now_ms >= rp->req_ms && (now_ms - rp->req_ms) >= late_ack_ms) {
            struct io_uring_sqe *sopt = ring_sqe(r);
            sqe_setsockopt_quickack0(sopt, slot, UD(UD_OPT, c->ring, slot, c->gen));
        }
    }
    nreplies = 0;
}

/* ---------------- cgroup SO_RCVBUF governor, 300s idle sweep, mlock ---------------- */

static int small_file_read(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return 0;
}

static uint64_t cg_read_u64(const char *path) {
    char buf[64];
    if (small_file_read(path, buf, sizeof buf) != 0 || buf[0] < '0' || buf[0] > '9') return 0;
    return strtoull(buf, NULL, 10);
}

static void apply_listener_rcvbuf(int requested) {
    for (unsigned l = 0; l < NLISTEN; l++) {
        if (lfd[l] >= 0) {
            (void)setsockopt(lfd[l], SOL_SOCKET, SO_RCVBUF, &requested, sizeof requested);
        }
    }
}

static void sweep_idle_and_govern(uint64_t now) {
    static uint64_t last_gov = 0;
    static uint64_t last_idle_sweep = 0;
    static int cur_rcvbuf = 16384;
    if (now - last_gov < GOVERNOR_NS) return;
    last_gov = now;

    uint64_t cg_cur = cg_read_u64("/sys/fs/cgroup/memory.current");
    uint64_t cg_lim = cg_read_u64("/sys/fs/cgroup/memory.max");
    double psi_some = 0.0, psi_full = 0.0;
    int have_psi = 0;
    char pbuf[512];
    if (small_file_read("/sys/fs/cgroup/memory.pressure", pbuf, sizeof pbuf) == 0) {
        const char *p = strstr(pbuf, "some avg10=");
        const char *q = strstr(pbuf, "full avg10=");
        if (p && q && sscanf(p, "some avg10=%lf", &psi_some) == 1 &&
            sscanf(q, "full avg10=%lf", &psi_full) == 1) {
            have_psi = 1;
        }
    }
    int high_cg = (have_psi && (psi_full >= 2.0 || psi_some >= 10.0)) ||
                  (cg_lim > 0 && cg_cur > (cg_lim * 85ULL) / 100ULL);
    int low_cg = (!have_psi || (psi_full < 0.5 && psi_some < 2.0)) &&
                 (cg_lim == 0 || cg_cur < (cg_lim * 75ULL) / 100ULL);
    if (high_cg && cur_rcvbuf != 8192) {
        apply_listener_rcvbuf(8192);
        cur_rcvbuf = 8192;
    } else if (low_cg && cur_rcvbuf != 16384) {
        apply_listener_rcvbuf(16384);
        cur_rcvbuf = 16384;
    }

    if (now - last_idle_sweep < IDLE_SWEEP_NS) return;
    last_idle_sweep = now;

    for (unsigned ri = 0; ri < active_rings; ri++) {
        struct rs *rs = &R[ri];
        if (!rs->created || rs->open == 0) continue;
        unsigned lim = rs->max_slot;
        for (unsigned slot = 0; slot < lim; slot++) {
            struct conn *c = &rs->conns[slot];
            if ((c->flags & (CF_OPEN | CF_CLOSING | CF_SENDING | CF_REPLIED | CF_READY | CF_HELD)) == CF_OPEN) {
                if (now_s - c->last_active >= IDLE_SECONDS) {
                    conn_close(c);
                }
            }
        }
    }
}

static void lock_code(void) {
    char exe[512] = "";
    ssize_t en = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (en <= 0) return;
    exe[en] = '\0';

    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    int mem_fd = -1;
    char line[768];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = "", path[512] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &lo, &hi, perms, path) < 4) continue;
        if (strcmp(path, exe) != 0) continue;
        if (perms[3] != 'p' || perms[1] == 'w' || hi <= lo) continue;
        size_t sz = (size_t)(hi - lo);
        if (mlock((const void *)lo, sz) == 0) continue;
        /* Fallback: convert own binary file-backed pages into non-reclaimable anonymous COW pages. */
        if (mem_fd < 0) mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
        if (mem_fd >= 0) {
            uint8_t page[4096];
            for (size_t off = 0; off < sz; off += 4096u) {
                size_t chunk = (sz - off < 4096u) ? (sz - off) : 4096u;
                memcpy(page, (const void *)(lo + off), chunk);
                (void)pwrite(mem_fd, page, chunk, (off_t)(lo + off));
            }
        }
    }
    if (mem_fd >= 0) close(mem_fd);
    fclose(f);
}

/* ---------------- PGO synthetic DB generator & --bench ---------------- */

static long env_long(const char *name, long def) {
    const char *v = getenv(name);
    return v && *v ? atol(v) : def;
}

static int make_train_db(const char *path) {
    long users = env_long("TRAIN_USERS", 5000);
    long posts = env_long("TRAIN_POSTS", 40000);
    long likes = env_long("TRAIN_LIKES", 160000);
    sqlite3_initialize();
    unlink(path);
    sqlite3 *c = NULL;
    if (sqlite3_open_v2(path, &c, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) return 1;
    const char *schema =
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;"
        "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), PRIMARY KEY (user_id, post_id));"
        "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
        "CREATE INDEX posts_user_id_idx ON posts (user_id);"
        "CREATE INDEX likes_post_id_idx ON likes (post_id);"
        "BEGIN;";
    if (sqlite3_exec(c, schema, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "make_train_db: %s\n", sqlite3_errmsg(c));
        sqlite3_close(c);
        return 1;
    }
    sqlite3_stmt *su = NULL, *sp = NULL, *sl = NULL;
    sqlite3_prepare_v2(c, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1, &su, NULL);
    sqlite3_prepare_v2(c, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)", -1, &sp, NULL);
    sqlite3_prepare_v2(c, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)", -1, &sl, NULL);

    srand(4242);
    char buf[600];
    for (long i = 1; i <= users; i++) {
        int n = snprintf(buf, sizeof buf, "user_%ld", i);
        sqlite3_bind_int64(su, 1, i);
        sqlite3_bind_text(su, 2, buf, n, SQLITE_STATIC);
        sqlite3_step(su);
        sqlite3_reset(su);
    }
    static const char *words[] = {
        "server", "latency", "sqlite", "index", "cache",
        "keepalive", "uring", "batch", "kernel", "btree"
    };
    for (long i = 1; i <= posts; i++) {
        int n = 0, nw = 5 + rand() % 30;
        for (int w = 0; w < nw && n < 480; w++) {
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s", w ? " " : "", words[rand() % 10]);
        }
        time_t t = (time_t)(1704067200L + i * 60);
        struct tm tm;
        gmtime_r(&t, &tm);
        char created[80];
        snprintf(created, sizeof created, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, rand() % 1000);
        sqlite3_bind_int64(sp, 1, i);
        sqlite3_bind_int64(sp, 2, 1 + rand() % users);
        sqlite3_bind_text(sp, 3, buf, n, SQLITE_STATIC);
        sqlite3_bind_text(sp, 4, created, 24, SQLITE_STATIC);
        sqlite3_step(sp);
        sqlite3_reset(sp);
    }
    for (long i = 0; i < likes; i++) {
        sqlite3_bind_int64(sl, 1, 1 + rand() % users);
        sqlite3_bind_int64(sl, 2, 1 + rand() % posts);
        sqlite3_step(sl);
        sqlite3_reset(sl);
    }
    sqlite3_finalize(su);
    sqlite3_finalize(sp);
    sqlite3_finalize(sl);
    if (sqlite3_exec(c, "COMMIT; ANALYZE; PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(c);
        return 1;
    }
    sqlite3_close(c);
    return 0;
}

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

static int run_bench(int argc, char **argv) {
    const char *db_path = argc > 2 ? argv[2] : "./seed/feed.db";
    long nreqs = argc > 3 ? atol(argv[3]) : 200000;
    int cache_kib = (argc > 4 && atoi(argv[4]) > 0) ? atoi(argv[4]) : 0;
    const char *mode = argc > 5 ? argv[5] : "k6";
    const char *secret = getenv("JWT_SECRET");
    if (!secret || !*secret) secret = "bench-secret";

    if (app_init(&A, db_path, secret, cache_kib) != 0) {
        fprintf(stderr, "bench app_init failed on %s\n", db_path);
        return 1;
    }

    int64_t max_pid = 1, max_uid = 1;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(A.db.c, "SELECT max(id) FROM posts", -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) max_pid = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
        if (sqlite3_prepare_v2(A.db.c, "SELECT max(id) FROM users", -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) max_uid = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
    }
    if (max_pid < 1) max_pid = 1;
    if (max_uid < 1) max_uid = 1;

    enum { NTOK = 256 };
    char auth_hdrs[NTOK][512];
    const char hdr_b64[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    for (int i = 0; i < NTOK; i++) {
        int64_t uid = 1 + (i % max_uid);
        char pl[160], pl_b64[256], msg[400], sig_b64[64];
        int pn = snprintf(pl, sizeof pl, "{\"sub\":\"%lld\",\"username\":\"user_%lld\",\"exp\":%lld}",
                          (long long)uid, (long long)uid, (long long)(A.now + 86400));
        b64url_encode((const uint8_t *)pl, (size_t)pn, pl_b64);
        int mn = snprintf(msg, sizeof msg, "%s.%s", hdr_b64, pl_b64);
        uint8_t sig[32];
        hmac_sha256(&A.key, (const uint8_t *)msg, (size_t)mn, sig);
        b64url_encode(sig, 32, sig_b64);
        snprintf(auth_hdrs[i], sizeof auth_hdrs[i], "Bearer %s.%s", msg, sig_b64);
    }

    uint8_t *buf = (uint8_t *)malloc(RESP_MAX);
    if (!buf) return 1;
    char req_buf[1024];

    int do_writes = (strcmp(mode, "read") != 0);
    int hot_tail = (strcmp(mode, "rand") != 0);
    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    for (long w = 0; w < 5000; w++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        int pct = (int)((rng >> 32) % 100);
        int n = 0;
        if (pct < 50) {
            n = snprintf(req_buf, sizeof req_buf, "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
        } else {
            int64_t pid = hot_tail
                          ? (max_pid >= 20 ? max_pid - (int64_t)((rng >> 8) % 20ULL) : max_pid)
                          : (1 + (int64_t)((rng >> 8) % (uint64_t)max_pid));
            n = snprintf(req_buf, sizeof req_buf, "GET /posts/%lld HTTP/1.1\r\nHost: localhost\r\n\r\n", (long long)pid);
        }
        struct req r;
        if (http_parse((const uint8_t *)req_buf, (uint32_t)n, &r) == 1) {
            struct resp rp;
            app_handle(&A, &r, buf, &rp);
        }
        if ((w & 63) == 63) db_end_batch(&A.db, 0);
    }
    db_commit(&A.db);

    uint64_t c_feed = 0, c_post = 0, c_like = 0, c_create = 0;
    uint64_t t0 = now_ns();
    for (long i = 0; i < nreqs; i++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t x = (uint32_t)(rng >> 32) % 1000u;
        int n = 0;
        int64_t pid = hot_tail
                      ? (max_pid >= 20 ? max_pid - (int64_t)((rng >> 8) % 20ULL) : max_pid)
                      : (1 + (int64_t)((rng >> 8) % (uint64_t)max_pid));
        if (!do_writes) {
            if (x < 500u) {
                n = snprintf(req_buf, sizeof req_buf, "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
                c_feed++;
            } else {
                n = snprintf(req_buf, sizeof req_buf, "GET /posts/%lld HTTP/1.1\r\nHost: localhost\r\n\r\n", (long long)pid);
                c_post++;
            }
        } else {
            if (x < 460u) {
                n = snprintf(req_buf, sizeof req_buf, "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
                c_feed++;
            } else if (x < 920u) {
                n = snprintf(req_buf, sizeof req_buf, "GET /posts/%lld HTTP/1.1\r\nHost: localhost\r\n\r\n", (long long)pid);
                c_post++;
            } else if (x < 990u) {
                int ti = (int)((rng >> 20) & (NTOK - 1));
                n = snprintf(req_buf, sizeof req_buf,
                             "POST /posts/%lld/like HTTP/1.1\r\nHost: localhost\r\nAuthorization: %s\r\nContent-Length: 0\r\n\r\n",
                             (long long)pid, auth_hdrs[ti]);
                c_like++;
            } else {
                int ti = (int)((rng >> 20) & (NTOK - 1));
                const char *jb = "{\"body\":\"hello benchmark server challenge post!\"}";
                n = snprintf(req_buf, sizeof req_buf,
                             "POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: %s\r\nContent-Length: %zu\r\n\r\n%s",
                             auth_hdrs[ti], strlen(jb), jb);
                c_create++;
            }
        }
        struct req r;
        struct resp rp;
        if (http_parse((const uint8_t *)req_buf, (uint32_t)n, &r) == 1) {
            app_handle(&A, &r, buf, &rp);
        }
        if ((i & 255) == 255) {
            db_end_batch(&A.db, (uint32_t)(now_ns() / 1000000ULL));
        }
    }
    db_commit(&A.db);
    uint64_t dt = now_ns() - t0;
    double sec = (double)dt / 1e9;
    printf("bench mode=%s n=%ld elapsed=%.3fs throughput=%.0f req/s us_per_req=%.3f (feed=%llu post=%llu like=%llu create=%llu err=%llu)\n",
           mode, nreqs, sec, (double)nreqs / sec, (double)dt / (1000.0 * (double)nreqs),
           (unsigned long long)c_feed, (unsigned long long)c_post,
           (unsigned long long)c_like, (unsigned long long)c_create,
           (unsigned long long)A.n_err);

    free(buf);
    db_close(&A.db);
    return 0;
}

static void on_sigterm(int sig) {
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--make-train-db") == 0) {
        return make_train_db(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "--bench") == 0) {
        return run_bench(argc, argv);
    }

    const char *db_path = getenv("SQLITE_PATH");
    if (!db_path || !*db_path) db_path = "./seed/feed.db";
    const char *secret = getenv("JWT_SECRET");
    if (!secret || !*secret) {
        fprintf(stderr, "JWT_SECRET must be set\n");
        return 1;
    }
    const char *host = getenv("HOST");
    if (!host || !*host) host = "0.0.0.0";
    const char *port_s = getenv("PORT");
    int port = (port_s && *port_s) ? atoi(port_s) : 8080;

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    unsigned nslots_def = (rl.rlim_cur > 65535ULL) ? 65535u : (unsigned)rl.rlim_cur;
    if (nslots_def > 1024u) nslots_def -= 64u;
    if (nslots_def < 64u) nslots_def = 64u;

    NR = (unsigned)env_long("RINGS", 6);
    if (NR < 1) NR = 1;
    if (NR > MAX_RINGS) NR = MAX_RINGS;

    ring_slots = (unsigned)env_long("RING_SLOTS", (long)nslots_def);
    if (ring_slots < 64u) ring_slots = 64u;
    accept_margin = ring_slots / 8u < 4096u ? ring_slots / 8u : 4096u;
    if (accept_margin < 16u) accept_margin = 16u;
    ring_bufs = (unsigned)env_long("RECV_BUFS", 2048);
    ring_buf_size = (unsigned)env_long("BUF_SIZE", 1024);
    answers_per_pass = (unsigned)env_long("ANSWERS_PER_PASS", 256);
    if (answers_per_pass < 16) answers_per_pass = 16;
    wait_min = (unsigned)env_long("WAIT_MIN", 64);
    if (wait_min < 1) wait_min = 1;
    wait_ns_cfg = (uint64_t)env_long("WAIT_US", 1000) * 1000ULL;
    write_win_ns = (uint64_t)env_long("WRITE_WINDOW_US", 15000) * 1000ULL;
    late_ack_ms = (uint32_t)env_long("LATE_ACK_MS", 30);
    lifo_age_ms = (uint32_t)env_long("LIFO_AGE_MS", 8000);
    int cache_kib = (int)env_long("CACHE_KIB", 0);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (app_init(&A, db_path, secret, cache_kib) != 0) {
        fprintf(stderr, "app_init failed for %s\n", db_path);
        return 1;
    }

    lock_code();

    const char *err = NULL;
    int bind_port = port;
    for (unsigned l = 0; l < NLISTEN; l++) {
        lfd[l] = listen_socket(host, bind_port, 4096, &err);
        if (lfd[l] < 0) {
            fprintf(stderr, "listen_socket[%u] failed (%s): %s\n", l, err ? err : "unknown", strerror(errno));
            db_close(&A.db);
            return 1;
        }
        if (l == 0 && bind_port == 0) {
            struct sockaddr_in sin;
            socklen_t slen = sizeof sin;
            if (getsockname(lfd[0], (struct sockaddr *)&sin, &slen) == 0) {
                bind_port = ntohs(sin.sin_port);
            }
        }
    }
    apply_listener_rcvbuf(16384);

    for (unsigned i = 0; i < NR; i++) {
        if (ring_create_at(i) != 0) return 1;
    }

    if (A.db.ck_efd >= 0) {
        struct io_uring_sqe *s = ring_sqe(&R[0].r);
        sqe_read_efd(s, A.db.ck_efd, &g_ck_efd_buf, UD(UD_CKPT_EFD, 0, 0, 0));
    }

    accept_ring = 0;
    fill_accepts_on(0);

    fprintf(stderr, "c-iouring-Vojtik1112 listening on %s:%d (listeners=%u, rings=%u..%u, slots=%u, carry=%d, wwin=%lluus, ans=%u)\n",
            host, bind_port, NLISTEN, NR, NR_MAX, ring_slots, A.db.carry_read_tx,
            (unsigned long long)(write_win_ns / 1000ULL), answers_per_pass);

    unsigned idle_passes = 0;

    while (!g_stop) {
        /* Top-of-loop combined submit+collect: active worker rings flush SENDs/rearms before Ring 0 waits. */
        int worker_ready = 0;
        for (unsigned i = 1; i < active_rings; i++) {
            struct ring *wr = &R[i].r;
            if (ring_needs_enter(wr)) {
                long erc = ring_enter(wr, 0, 0);
                if (erc < 0 && erc != -EINTR && erc != -ETIME && erc != -EBUSY && erc != -EAGAIN) {
                    if (erc == -EBADR) usleep(1000);
                }
            }
            if (*wr->cq_head != LOAD_ACQ(wr->cq_tail)) worker_ready = 1;
        }

        uint64_t t_now = now_ns();
        int due_writes = (nheld > 0) && current_writes_ok(t_now);
        int has_work = worker_ready || nready > 0 || due_writes ||
                       (*R[0].r.cq_head != LOAD_ACQ(R[0].r.cq_tail));

        if (has_work) {
            if (ring_needs_enter(&R[0].r)) ring_enter(&R[0].r, 0, 0);
        } else {
            uint64_t wait_t = wait_ns_cfg;
            unsigned min_c = (last_batch_cqes >= 16) ? wait_min : 1u;
            if (last_batch_cqes < 16) {
                if (active_rings > 1) {
                    wait_t = (idle_passes > 64) ? 5000000ULL : (wait_t && wait_t < 2000000ULL ? wait_t : 1000000ULL);
                } else {
                    wait_t = 0;
                }
            }
            if (nheld > 0 && !A.db.writes_held && write_win_ns > 0 && last_write_commit_ns != 0) {
                uint64_t elapsed = t_now - last_write_commit_ns;
                if (elapsed >= write_win_ns) {
                    min_c = 0;
                    wait_t = 0;
                } else {
                    uint64_t rem = write_win_ns - elapsed;
                    if (wait_t == 0 || rem < wait_t) wait_t = rem;
                }
            }
            long erc = ring_enter(&R[0].r, min_c, wait_t);
            if (erc < 0 && erc != -EINTR && erc != -ETIME && erc != -EAGAIN && erc != -EBUSY) {
                if (erc == -EBADR) usleep(1000);
                else break;
            }
        }

        t_now = now_ns();
        A.now = (int64_t)time(NULL);
        now_s = (uint32_t)A.now;
        now_ms = (uint32_t)(t_now / 1000000ULL);

        if (db_should_yield_hold(&A.db, now_ms) && nready > 0) {
            usleep(200);
            t_now = now_ns();
            now_ms = (uint32_t)(t_now / 1000000ULL);
        }

        db_tick(&A.db, now_ms);
        int writes_ok = current_writes_ok(t_now);
        answers_left = answers_per_pass;

        if (writes_ok && nheld > 0) drain_held(1);
        uint64_t cqe_n = reap_all(writes_ok);
        cqe_n += read_rounds(writes_ok);
        if (nready > 0 && answers_left > 0) run_ready(writes_ok);
        if (writes_ok && nheld > 0) drain_held(1);

        int had_writes = A.db.wrote;
        int db_err = (db_end_batch(&A.db, now_ms) != 0);
        flush_replies(had_writes, db_err, t_now);

        if (nready == 0 && nheld == 0) {
            g_qa.used = 0;
        } else if (g_qa.used > (16u << 20)) {
            qarena_compact();
        }

        rearm_all();
        maybe_rotate_accept();
        sweep_idle_and_govern(t_now);

        last_batch_cqes = cqe_n;
        if (cqe_n > 0) idle_passes = 0;
        else idle_passes++;
    }

    for (unsigned l = 0; l < NLISTEN; l++) {
        if (lfd[l] >= 0) close(lfd[l]);
    }
    db_close(&A.db);
    return 0;
}
