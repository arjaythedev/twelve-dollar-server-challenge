/* server.c: one thread, several io_uring rings, one SQLite transaction per batch.
 *
 * A batch is one wake-up of the loop: every completion is handled (requests parsed in place in
 * the kernel's receive buffers and answered into an arena), the batch's transaction commits,
 * and only then are the responses handed to the kernel, one send each, straight from the arena.
 * See README.md for the design. */
#define _GNU_SOURCE /* sync_file_range */
#include "uring.h"

#include <arpa/inet.h>
#include <sys/wait.h>
#include <malloc.h>

#include "sqlite3.c"

#include "app.h"

#define MAX_RINGS 16
#define NLISTEN 8
#define LISTEN_BACKLOG 4096
#define ACCEPT_NEXT_MAX 8192 /* make the next ring this many connections before the current one is full */
#define ACCEPT_STOP_MAX 6144 /* stop accepting on a ring this many slots before its table is full */
#define RECV_BUF_SIZE_DEFAULT 1024 /* a request is ~150-400 bytes; a longer one spans buffers */
#define ARENA_CAP (1u << 20)
#define ARENA_POOL 16 /* arenas kept for reuse (1 MB each); a burst maps more and unmaps them after */
#define READS_PER_PASS 16384
#define IDLE_NS 100000000ULL
/* the batch shape (tunable for experiments: ANSWERS_PER_PASS, WAIT_MIN, WAIT_US) */
static unsigned answers_per_pass = 2048, wait_min = 256;
static int quickack_off = 1; /* QUICKACK_OFF: the listeners start in delayed-ACK mode (see main) */
static int poll_idle = 0; /* IDLE_POLL=1: idle sockets wait on a multishot poll instead of a multishot receive */
static int eventfd_wake = 0; /* EVENTFD_WAKE=1: other rings wake ring 0 through an eventfd (more, smaller batches: more CPU) */
static unsigned recv_buf_size = RECV_BUF_SIZE_DEFAULT;
/* Optional soft wall-clock budget for processing a pre-existing ready backlog.
 * Experimental: 0 disables it. Does not interrupt a SQLite transaction or send. */
static unsigned backlog_slice_us = 0;
static uint64_t sliced_ready_passes;
static uint64_t wait_ns = 1000000ULL;
#define PEND_MAX (HTTP_MAX_HEAD + HTTP_MAX_BODY)
#define STATS_NS 10000000000ULL

enum { UD_ACCEPT = 1, UD_RECV, UD_READ, UD_SEND, UD_SHUTDOWN, UD_CLOSE, UD_CANCEL, UD_POLL, UD_EVENTFD, UD_DELACK };
#define UD(op, ring, gen, slot) (((uint64_t)(op) << 56) | ((uint64_t)(ring) << 48) | ((uint64_t)(gen) << 32) | (uint64_t)(slot))
#define UD_OP(u) ((unsigned)((u) >> 56))
#define UD_RING(u) ((unsigned)((u) >> 48) & 0xff)
#define UD_GEN(u) ((uint16_t)((u) >> 32))
#define UD_SLOT(u) ((uint32_t)(u))
/* a connection reference on a list: ring, generation, slot */
#define REF(ring, gen, slot) UD(0, ring, gen, slot)

enum {
  CF_OPEN = 1,
  CF_CLOSING = 2,
  CF_SENDING = 4,
  CF_REPLIED = 8,      /* a reply waits for this batch's commit */
  CF_CLOSE_AFTER = 16, /* close once the output is sent */
  CF_HELD = 32,        /* a write waiting out a checkpoint hold (on the held list) */
  CF_READY = 64,       /* bytes read but not yet answered (on the ready list) */
  CF_RQ = 128,         /* on the read queue */
  CF_RECVING = 256,    /* a single-shot read in flight */
  CF_MULTI = 512,      /* the multishot receive is armed */
  CF_POLL = 1024,      /* a multishot POLL_ADD is armed */
  CF_POLL_REARM = 2048, /* a poll ended: rearm after this batch */
  CF_FRESH = 4096,     /* this pass's arrival, still in its receive buffer (on the fresh list) */
  CF_EOF = 8192        /* the client closed behind a fresh arrival: finish once it is answered */
};

struct arena {
  struct arena *next;
  uint32_t refs, used, cap;
  uint8_t data[];
};

struct conn {
  uint16_t gen;
  uint16_t flags;
  uint32_t pend_len; /* partial or waiting input, rare */
  uint32_t pend_cap;
  uint32_t pad; /* CF_FRESH: its place on the ring's fresh list */
  uint8_t *pend;
  struct arena *send_arena;
};

struct rs {
  struct ring r;
  struct conn *conns;
  unsigned open;
  uint8_t accept_armed; /* one bit per listener */
  uint8_t created;
  int efd; /* eventfd for reporting this ring's CQEs to ring 0; -1 if unused */
};

struct reply {
  uint64_t ref;
  struct arena *a;
  uint32_t off, len;
  uint8_t db, close, late;
};

static struct rs R[MAX_RINGS];
static unsigned NR, NR_MAX, acc_ring;
static unsigned ring_slots, accept_next, accept_stop, recv_bufs;
/* Two limits on the number of connections, both from the box's memory: a connection costs
 * ~4.2 KB of kernel memory whatever the server does (the TCP socket, its inode and file), so the
 * memory, not the server, decides how many there can be. On a box like the benchmark one 320,000
 * left 458 MB available, and 340,000 ended with the box out of memory and off the network.
 *
 * CONN_LIMIT: the most connections open at once, of both kinds below (0: no limit). At that
 * limit the listen queues go down to one entry, so the kernel drops the SYNs of the users over
 * it (they wait in connect()) and does not keep thousands of established sockets nobody will
 * accept; a connection that still gets through is answered 503 and closed. Refusing every one of
 * them that way was tried: accepting a connection to refuse it costs what accepting it to serve
 * it costs, at the moment the box has least to spare. */
static unsigned conn_limit, open_total;
static int listen_short;
static uint64_t refused_total;
/* KEEP_LIMIT: the most connections kept alive at once (0: every one is kept, up to CONN_LIMIT);
 * by default 30 MB of connections (~7,000) under CONN_LIMIT. A user who arrives when that many
 * are open is still served, on a brief connection: it is accepted on a ring of its own, its one
 * request is answered with "Connection: close", and the close is linked behind the reply's send,
 * so the client dials again for its next request and the connection holds its 4 KB for
 * milliseconds instead of for the run. The box's memory then stays where KEEP_LIMIT connections
 * put it, however many users there are. The price is a handshake and a close for every request
 * of the users past the limit (~50 us of CPU on the test box, three times a request on a kept
 * connection), which is why it is the exception and not the rule.
 *
 * The brief ring uses ordinary descriptors, not direct ones. On this kernel (6.8) a direct
 * descriptor that is closed is only released once every request submitted before its close has
 * ended, and on the other rings each connection's multishot receive is such a request: a
 * connection closed there keeps its memory until the receives of the connections accepted before
 * its close have ended, which under a steady load is the end of the run (measured: 2,100 closes
 * a second beside 20,000 kept connections took 950 MB in 110 s). An ordinary descriptor is
 * released with its last reference, and a connection that has been read has none left but the
 * table's. */
static unsigned keep_limit;
static unsigned BR = MAX_RINGS; /* the brief ring's index (MAX_RINGS: there is none) */
static int brief_on;            /* new connections go to the brief ring */
static unsigned brief_slots, brief_room; /* its table (indexed by descriptor number), and how many it may hold */
static unsigned brief_wait_ms = 5000;    /* BRIEF_WAIT_MS: a brief connection that has not sent a whole request by then is closed */
static uint64_t brief_total, brief_expired;
struct bwait { uint64_t ref; uint32_t ms; };
static struct bwait *bw; /* brief connections with a read in flight, oldest first */
static unsigned bw_head, bw_n, bw_cap;
#define BRIEF_BUFS 2048
#define BRIEF_BACKLOG 256
static int listen_now = LISTEN_BACKLOG; /* what the listen queues are set to */
static int ring_grow_failed;
static int LFD[NLISTEN];
static struct app A;
static int writes_ok = 1;

static struct arena *pool;
static unsigned pool_n;
static struct arena *cur;
static struct arena *batch_arenas[512];
static unsigned n_batch_arenas;
static struct reply *replies;
static unsigned nrep, rep_cap;
static uint64_t *ready, *held, *rq, *poll_rearms;
static unsigned n_poll_rearms, poll_rearms_cap;
static unsigned nready, ready_cap, nheld, held_cap, nrq, rq_head, rq_cap;
static unsigned answers_left;

/* Newest first (FRESH_FIRST, default 1). A request answered more than 40 ms after it arrived costs
 * the kernel a delayed ACK (a packet of its own, from a timer), takes the socket out of
 * delayed-ACK mode, and so costs a second ACK on arrival of that connection's next request. Oldest
 * first, one pause (a preemption, a disk wait, the last seconds of the ramp) makes every request
 * behind it late, each late request costs those two packets on top of its own, and above ~80 % of
 * the CPU that is more than the spare capacity: the backlog never drains. So a pass answers this
 * pass's arrivals first, the newest of them if they exceed the pass (ANSWERS_PER_PASS, sized to
 * keep a pass near 10 ms), and the backlog, oldest first, with what is left (at least
 * FRESH_RESERVE). An arrival waits in its receive buffer on its ring's fresh list until the pass
 * has seen every ring; one that does not make the pass moves to pend and the ready list. */
static int fresh_first = 1;
static unsigned fresh_reserve = 24;
/* How much a pass takes (ANSWERS_PER_PASS=0, the default: sized by time; a number: that many).
 * A reply must leave within the kernel's 40 ms; DEADLINE_MS (32) is that with a margin. The loop
 * measures what an answer costs it (pass plus enter, per answer, over the passes that did not
 * wait): cost_ns. With n arrivals spread over the `age` since the last pass, the newest k all
 * leave in time if age * k / n + k * cost <= deadline: that many are answered now, all of them
 * if that holds for n. What the pass then takes from the backlog fills it to PASS_MS (8) of work
 * and no more: after a pass of P the next one finds P of arrivals and needs about P to answer
 * them, so 8 leaves half the deadline for a pass that starts late (at 16, any hiccup while a
 * backlog was being answered sent arrivals to it again). A fixed count spilled
 * arrivals that could still have made it (384 was ~11 ms of arrivals at 30k requests a second,
 * so any pause over 12 ms sent requests to the backlog and so to a delayed ACK each). */
static uint64_t deadline_ns = 32000000ULL, pass_ns = 8000000ULL;
static double cost_ns = 30000.0;
static unsigned pass_answers; /* answers produced by the current pass */
static unsigned fresh_min = 32;
/* When the backlog is answered. While arrivals do not fit their passes, a request answered from
 * the backlog sends an arrival there in its place, in that pass or the next: the backlog is no
 * shorter, one more request is late and one more delayed ACK is paid. In the last seconds of the
 * ramp at 320k on the test box, 92,000 requests went through a backlog that never held more than
 * 36,000. So the backlog waits until no arrival has been sent to it for BACKLOG_QUIET_MS (30; 0:
 * it takes what every pass leaves), and then takes what the arrivals leave. A lasting overload
 * cannot starve it: once its oldest request has waited BACKLOG_AGE_MS (20 s; 0: always) it gets
 * FRESH_RESERVE answers of every pass, and half the pass once that request has waited twice as
 * long, so nothing comes near a client's timeout. */
static unsigned backlog_age_ms = 20000, backlog_quiet_ms = 30;
static uint32_t now_ms;       /* when this pass started, ms (ages are differences: the wrap is harmless) */
static uint32_t spill_ms;     /* when an arrival last went to the backlog */
static unsigned pass_reserve; /* the backlog's share of this pass */
static int backlog_parked;    /* the last pass left the backlog alone: it is no reason not to wait for completions */
static unsigned late_ms = 20; /* LATE_MS: a pass that starts this long after the last one answers late */
static int delack_restore = 1; /* DELACK_RESTORE: a late answer puts its socket back into delayed-ACK mode */
struct fresh { uint32_t slot, len; uint16_t gen, bid; };
static struct fresh *FR[MAX_RINGS];
static unsigned fr_head[MAX_RINGS], fr_n[MAX_RINGS], fr_cap;
static int collecting; /* inside the pass's reap: arrivals go to the fresh lists */
static int feed_late;  /* the answers being produced are late */
static struct { uint64_t fresh, spilled, late_passes, restores, restore_err, reserved; } FS;

static struct {
  uint64_t batches, cqes, reqs, accepts, closes, enobufs, reads, deferred, rq_max, ready_max, max_batch, cq_overflow, cancels;
  uint64_t poll_events, poll_ends, poll_rearmed, eventfd_events;
  uint64_t max_turn_ns, max_commit_ns, max_gap_ns; /* longest batch, longest commit, longest time outside a batch while busy */
} S;
static uint64_t reads_done; /* single-shot reads that returned data or EAGAIN */

/* ---------- arenas ---------- */

/* Arenas are their own mappings, not heap blocks: one a burst needed beyond the pool goes back to
 * the kernel when its last send completes, instead of staying in a heap that never shrinks. */
static unsigned arenas_mapped;
static struct arena *arena_map(void) {
  void *m = mmap(NULL, sizeof(struct arena) + ARENA_CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) { perror("mmap arena"); exit(1); }
  struct arena *a = m;
  a->cap = ARENA_CAP;
  arenas_mapped++;
  return a;
}
static struct arena *arena_get(void) {
  struct arena *a = pool;
  if (a) {
    pool = a->next;
    pool_n--;
  } else {
    a = arena_map();
  }
  a->refs = 0;
  a->used = 0;
  return a;
}
static void arena_unref(struct arena *a) {
  if (--a->refs) return;
  if (pool_n < ARENA_POOL) {
    a->next = pool;
    pool = a;
    pool_n++;
  } else {
    munmap(a, sizeof(struct arena) + ARENA_CAP);
    arenas_mapped--;
  }
}
static uint8_t *arena_reserve(unsigned n) {
  if (!cur || cur->used + n > cur->cap) {
    cur = arena_get();
    cur->refs = 1; /* the batch's own reference */
    if (n_batch_arenas == 512) { fprintf(stderr, "batch too large\n"); exit(1); }
    batch_arenas[n_batch_arenas++] = cur;
  }
  return cur->data + cur->used;
}

static void prewarm(void) {
  for (unsigned i = 0; i < 8; i++) {
    struct arena *a = arena_map();
    memset(a->data, 0, ARENA_CAP);
    a->next = pool;
    pool = a;
    pool_n++;
  }
  rep_cap = ready_cap = held_cap = rq_cap = 32768;
  replies = calloc(rep_cap, sizeof *replies);
  ready = calloc(ready_cap, sizeof *ready);
  held = calloc(held_cap, sizeof *held);
  rq = calloc(rq_cap, sizeof *rq);
  if (!replies || !ready || !held || !rq) { perror("calloc"); exit(1); }
}

/* ---------- lists ---------- */

static void list_push(uint64_t **l, unsigned *n, unsigned *cap, uint64_t v) {
  if (*n == *cap) {
    *cap *= 2;
    *l = realloc(*l, *cap * sizeof **l);
    if (!*l) { perror("realloc"); exit(1); }
  }
  (*l)[(*n)++] = v;
}
static void reply_push(uint64_t ref, struct arena *a, uint32_t off, uint32_t len, int db, int close) {
  if (nrep == rep_cap) {
    rep_cap *= 2;
    replies = realloc(replies, rep_cap * sizeof *replies);
    if (!replies) { perror("realloc"); exit(1); }
  }
  replies[nrep++] = (struct reply){ref, a, off, len, (uint8_t)db, (uint8_t)close, (uint8_t)feed_late};
}

/* ---------- connections ---------- */

static inline struct conn *ref_conn(uint64_t ref) { return &R[UD_RING(ref)].conns[UD_SLOT(ref)]; }
static inline int ref_live(uint64_t ref, struct conn *c) {
  return c->gen == UD_GEN(ref) && (c->flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN;
}

static void conn_close(unsigned ri, uint32_t slot, struct conn *c) {
  if ((c->flags & (CF_OPEN | CF_CLOSING)) != CF_OPEN) return;
  c->flags |= CF_CLOSING;
  if (ri == BR) { /* an ordinary descriptor: a read in flight holds the socket open, so it is ended first */
    if (c->flags & CF_RECVING) {
      struct io_uring_sqe *q = ring_sqe(&R[ri].r);
      sqe_cancel_ud(q, UD(UD_READ, ri, c->gen, slot), UD(UD_CANCEL, ri, c->gen, slot));
      q->flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
    }
    sqe_close_fd(ring_sqe(&R[ri].r), (int)slot, UD(UD_CLOSE, ri, c->gen, slot));
  } else if (poll_idle)
    sqe_shutdown_close_poll(&R[ri].r, slot, UD(UD_POLL, ri, c->gen, slot),
                            UD(UD_CANCEL, ri, c->gen, slot), UD(UD_SHUTDOWN, ri, c->gen, slot),
                            UD(UD_CLOSE, ri, c->gen, slot));
  else
    sqe_shutdown_close(&R[ri].r, slot, UD(UD_SHUTDOWN, ri, c->gen, slot), UD(UD_CLOSE, ri, c->gen, slot));
  S.closes++;
}

static void conn_freed(unsigned ri, struct conn *c) {
  c->gen++;
  c->flags = 0;
  free(c->pend);
  c->pend = NULL;
  c->pend_len = c->pend_cap = 0;
  if (c->send_arena) {
    arena_unref(c->send_arena);
    c->send_arena = NULL;
  }
  R[ri].open--;
  open_total--;
}

/* A brief connection's read: one, which waits for the bytes. */
static void brief_recv(unsigned ri, uint32_t slot, struct conn *c) {
  sqe_recv_fd(ring_sqe(&R[ri].r), (int)slot, UD(UD_READ, ri, c->gen, slot));
  c->flags |= CF_RECVING;
  if (bw_head + bw_n == bw_cap) {
    if (bw_head) {
      memmove(bw, bw + bw_head, bw_n * sizeof *bw);
      bw_head = 0;
    } else {
      bw_cap = bw_cap ? bw_cap * 2 : 1024;
      bw = realloc(bw, bw_cap * sizeof *bw);
      if (!bw) { perror("realloc"); exit(1); }
    }
  }
  bw[bw_head + bw_n++] = (struct bwait){REF(ri, c->gen, slot), now_ms};
}
/* Once a pass: a brief connection is there to send one request, and one that has not after
 * BRIEF_WAIT_MS gives its place back. */
static void brief_expire(void) {
  while (bw_n) {
    struct bwait *w = &bw[bw_head];
    struct conn *c = ref_conn(w->ref);
    int waiting = ref_live(w->ref, c) && (c->flags & CF_RECVING);
    if (waiting && now_ms - w->ms < brief_wait_ms) break;
    if (waiting) { conn_close(UD_RING(w->ref), UD_SLOT(w->ref), c); brief_expired++; }
    bw_head++;
    bw_n--;
  }
  if (!bw_n) bw_head = 0;
}

static inline void arm_multishot(unsigned ri, uint32_t slot, struct conn *c) {
  sqe_recv_multishot(ring_sqe(&R[ri].r), slot, UD(UD_RECV, ri, c->gen, slot));
  c->flags |= CF_MULTI;
}

/* Mode-selecting idle registration.  POLL_ADD stays armed while a nonblocking
 * one-shot recv drains the input, so idle sockets avoid an armed recv request. */
static inline void arm_idle(unsigned ri, uint32_t slot, struct conn *c) {
  if (!poll_idle) { arm_multishot(ri, slot, c); return; }
  if (c->flags & (CF_POLL | CF_POLL_REARM)) return;
  sqe_poll_multishot_conn(ring_sqe(&R[ri].r), slot, UD(UD_POLL, ri, c->gen, slot));
  c->flags |= CF_POLL;
}

static inline void queue_read(unsigned ri, uint32_t slot, struct conn *c) {
  if (c->flags & CF_RQ) return;
  c->flags |= CF_RQ;
  list_push(&rq, &nrq, &rq_cap, REF(ri, c->gen, slot));
}

static void pend_append(struct conn *c, const uint8_t *p, uint32_t n) {
  if (c->pend_len + n > c->pend_cap) {
    uint32_t cap = c->pend_cap ? c->pend_cap : 128; /* a GET is ~100 bytes: 512 each was 170 MB with every connection waiting */
    while (cap < c->pend_len + n) cap *= 2;
    uint8_t *np = realloc(c->pend, cap);
    if (!np) { perror("realloc pend"); exit(1); }
    c->pend = np;
    c->pend_cap = cap;
  }
  memcpy(c->pend + c->pend_len, p, n);
  c->pend_len += n;
}
/* Keeps p[0..n) as the connection's waiting input (p may point into pend). */
static void stash(struct conn *c, const uint8_t *p, uint32_t n) {
  if (n == 0) {
    /* nothing waits: give the buffer back (a backlog would otherwise leave every connection it
     * touched holding one for good, ~100 MB after 200k deferred requests) */
    c->pend_len = 0;
    if (c->pend_cap) { free(c->pend); c->pend = NULL; c->pend_cap = 0; }
    return;
  }
  if (p == c->pend) { c->pend_len = n; return; }
  if (c->pend && p > c->pend && p < c->pend + c->pend_cap) {
    memmove(c->pend, p, n);
    c->pend_len = n;
    return;
  }
  c->pend_len = 0;
  pend_append(c, p, n);
}

/* Parses and answers at most one request per batch from the connection's input (data, plus
 * whatever waits in pend); the rest stays in pend. */
static void conn_feed(unsigned ri, uint32_t slot, struct conn *c, const uint8_t *data, uint32_t len) {
  const uint8_t *buf;
  uint32_t n;
  if (c->pend_len) {
    if (len) pend_append(c, data, len);
    buf = c->pend;
    n = c->pend_len;
  } else {
    buf = data;
    n = len;
  }
  while (n) {
    if (c->flags & (CF_SENDING | CF_REPLIED | CF_CLOSING | CF_HELD | CF_CLOSE_AFTER)) break;
    struct req rq_;
    int rc = http_parse(buf, n, &rq_);
    if (rc == 0) {
      if (n > PEND_MAX) { conn_close(ri, slot, c); n = 0; }
      else if (ri == BR && !(c->flags & CF_RECVING)) brief_recv(ri, slot, c); /* the rest of it is still to come */
      break;
    }
    uint8_t *region = arena_reserve(RESP_MAX);
    struct resp rp;
    if (rc < 0) {
      rp.close = 1;
      error_resp(region, 400, "bad request", &rp);
      rp.db = 0;
      n = 0;
    } else {
      if (ri == BR) rq_.close = 1; /* not kept: the reply says so and the close follows it */
      app_handle(&A, &rq_, region, writes_ok, &rp);
      if (rp.held) {
        c->flags |= CF_HELD;
        list_push(&held, &nheld, &held_cap, REF(ri, c->gen, slot));
        break;
      }
      buf += rq_.total_len;
      n -= rq_.total_len;
    }
    cur->used += (uint32_t)((rp.p + rp.n) - region);
    reply_push(REF(ri, c->gen, slot), cur, (uint32_t)(rp.p - cur->data), rp.n, rp.db, rp.close);
    c->flags |= CF_REPLIED;
    if (rp.close) c->flags |= CF_CLOSE_AFTER;
    S.reqs++;
    pass_answers++;
  }
  stash(c, buf, n);
}

static void defer_bytes(unsigned ri, uint32_t slot, struct conn *c, const uint8_t *p, uint32_t n) {
  pend_append(c, p, n);
  if (!(c->flags & (CF_READY | CF_HELD))) {
    c->flags |= CF_READY;
    c->pad = now_ms; /* since when it waits (pad is the fresh-list place only while CF_FRESH) */
    list_push(&ready, &nready, &ready_cap, REF(ri, c->gen, slot));
  }
  S.deferred++;
}

/* EOF or an error from the client: answers still owed go out first. */
static void finish_conn(unsigned ri, uint32_t slot, struct conn *c) {
  if (c->flags & CF_FRESH) { c->flags |= CF_EOF; return; } /* its request has not been looked at yet */
  if (c->flags & (CF_REPLIED | CF_READY | CF_HELD | CF_SENDING)) c->flags |= CF_CLOSE_AFTER;
  else conn_close(ri, slot, c);
}

/* A fresh arrival that does not make this pass (or whose connection got more bytes): its bytes
 * move to pend, the connection to the ready list, the buffer back to the ring. */
static void fresh_spill(unsigned ri, struct fresh *f) {
  struct rs *rs = &R[ri];
  struct conn *c = &rs->conns[f->slot];
  if (!f->len) return; /* already spilled */
  if (c->gen == f->gen && (c->flags & CF_FRESH)) {
    c->flags &= (uint16_t)~CF_FRESH;
    if ((c->flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN) {
      defer_bytes(ri, f->slot, c, ring_buf_ptr(&rs->r, f->bid), f->len);
      FS.spilled++;
      spill_ms = now_ms;
      if (c->flags & CF_EOF) { c->flags &= (uint16_t)~CF_EOF; finish_conn(ri, f->slot, c); }
    }
  }
  ring_buf_add(&rs->r, f->bid);
  f->len = 0;
}

/* Bytes received. Returns 1 if the receive buffer stays in use (the arrival is on the fresh list).
 * Otherwise: answered now if this batch has answers left and nothing older waits on the
 * connection, or kept for a later batch (drain first: reading is cheap, a request left in its
 * socket costs a 3-4 KB kernel buffer). */
static int received(unsigned ri, uint32_t slot, struct conn *c, unsigned bid, uint32_t n) {
  const uint8_t *p = ring_buf_ptr(&R[ri].r, bid);
  if (c->flags & CF_CLOSE_AFTER) return 0;
  if (c->flags & CF_FRESH) fresh_spill(ri, &FR[ri][c->pad]); /* more bytes behind a fresh arrival: in order, through pend */
  if (c->flags & (CF_SENDING | CF_REPLIED | CF_HELD | CF_READY)) { defer_bytes(ri, slot, c, p, n); return 0; }
  if (collecting) {
    if (fr_n[ri] >= (ri == BR && fr_cap > BRIEF_BUFS / 2 ? BRIEF_BUFS / 2 : fr_cap)) { /* the ring's list is full: its oldest cannot make this pass */
      fresh_spill(ri, &FR[ri][fr_head[ri]]);
      fr_head[ri] = (fr_head[ri] + 1) % fr_cap;
      fr_n[ri]--;
    }
    unsigned at = (fr_head[ri] + fr_n[ri]) % fr_cap;
    FR[ri][at] = (struct fresh){slot, n, c->gen, (uint16_t)bid};
    fr_n[ri]++;
    c->pad = at;
    c->flags |= CF_FRESH;
    return 1;
  }
  if (answers_left == 0) { defer_bytes(ri, slot, c, p, n); spill_ms = now_ms; return 0; }
  answers_left--;
  conn_feed(ri, slot, c, p, n);
  return 0;
}

/* Answers this pass's arrivals: all of them if the pass has room, otherwise each ring's newest in
 * proportion, the older ones going to the backlog first so that it stays in arrival order. */
static void fresh_run(uint64_t age_ns) {
  unsigned total = 0, take[MAX_RINGS];
  for (unsigned i = 0; i < NR; i++) total += fr_n[i];
  if (!total) return;
  unsigned budget;
  if (answers_per_pass) { /* a fixed count */
    budget = answers_left;
    if (pass_reserve) budget = budget > pass_reserve ? budget - pass_reserve : 0;
  } else { /* the newest that can still leave within the deadline */
    double need = (double)age_ns + (double)total * cost_ns;
    budget = need <= (double)deadline_ns ? total : (unsigned)((double)deadline_ns * (double)total / need);
    if (budget < fresh_min) budget = fresh_min;
  }
  /* the brief ring's arrivals come out of the budget first: one that waits holds a whole
   * connection (4 KB of kernel memory) until it is answered, a kept connection's request 128 bytes */
  unsigned first = BR < NR ? (fr_n[BR] < budget ? fr_n[BR] : budget) : 0, others = total - (BR < NR ? fr_n[BR] : 0);
  for (unsigned i = 0; i < NR; i++)
    take[i] = i == BR ? first : total <= budget ? fr_n[i] : (unsigned)((uint64_t)fr_n[i] * (budget - first) / (others ? others : 1));
  for (unsigned i = 0; i < NR; i++)
    for (; fr_n[i] > take[i]; fr_n[i]--) {
      fresh_spill(i, &FR[i][fr_head[i]]);
      fr_head[i] = (fr_head[i] + 1) % fr_cap;
    }
  for (unsigned i = 0; i < NR; i++) {
    struct rs *rs = &R[i];
    for (; fr_n[i]; fr_n[i]--) {
      struct fresh *f = &FR[i][fr_head[i]];
      fr_head[i] = (fr_head[i] + 1) % fr_cap;
      if (!f->len) continue; /* spilled when its connection got more bytes */
      struct conn *c = &rs->conns[f->slot];
      if (c->gen == f->gen && (c->flags & CF_FRESH)) {
        c->flags &= (uint16_t)~CF_FRESH;
        if ((c->flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN) {
          if (answers_left) answers_left--;
          conn_feed(i, f->slot, c, ring_buf_ptr(&rs->r, f->bid), f->len);
          FS.fresh++;
          if (c->flags & CF_EOF) { c->flags &= (uint16_t)~CF_EOF; finish_conn(i, f->slot, c); }
        }
      }
      ring_buf_add(&rs->r, f->bid);
      f->len = 0;
    }
    fr_head[i] = 0;
    ring_buf_publish(&rs->r);
  }
}

/* ---------- accepting ---------- */

/* brief: the ring of the connections that are not kept (see keep_limit): a small one, with no
 * direct descriptors to speak of and a table indexed by descriptor number, touched as it is used. */
static int ring_create(unsigned i, int brief) {
  const char *err = "";
  unsigned cq = brief ? 16384 : 65536, slots = brief ? brief_slots : ring_slots, bufs = brief ? BRIEF_BUFS : recv_bufs;
  while (ring_init(&R[i].r, 4096, cq, brief ? 16 : ring_slots, bufs, recv_buf_size,
                   i == 0 || !eventfd_wake, &err) < 0) {
    if (errno != ENOMEM || strcmp(err, "io_uring_setup") || cq <= 8192) {
      fprintf(stderr, "ring %u: %s: %s\n", i, err, strerror(errno));
      return -1;
    }
    cq /= 2;
  }
  R[i].conns = calloc(slots, sizeof(struct conn));
  if (!R[i].conns) { perror("calloc conns"); return -1; }
  if (!brief) prefault(R[i].conns, (size_t)slots * sizeof(struct conn));
  prefault(R[i].r.bufs, (size_t)bufs * recv_buf_size);
  FR[i] = calloc(fr_cap, sizeof(struct fresh));
  if (!FR[i]) { perror("calloc fresh"); return -1; }
  R[i].efd = -1;
  if (i > 0 && eventfd_wake) {
    R[i].efd = ring_register_eventfd(&R[i].r);
    if (R[i].efd < 0) {
      /* All earlier rings are still correct with the legacy enter-each-ring
       * policy. Later rings will be initialized with DEFER_TASKRUN. */
      fprintf(stderr, "ring %u: eventfd registration failed (%s); reverting to legacy ring servicing\n",
              i, strerror(errno));
      eventfd_wake = 0;
    } else {
      sqe_poll_eventfd(ring_sqe(&R[0].r), R[i].efd, UD(UD_EVENTFD, i, 0, 0));
    }
  }
  R[i].created = 1;
  fprintf(stderr, "ring %u: io_uring fd %d, %u CQ entries, %u slots%s\n", i, R[i].r.fd, cq, slots, brief ? " (brief connections)" : "");
  return 0;
}

static void arm_accepts(unsigned ri) {
  struct rs *rs = &R[ri];
  for (unsigned l = 0; l < NLISTEN; l++)
    if (!(rs->accept_armed & (1u << l))) {
      if (ri == BR) sqe_accept_multishot_fd(ring_sqe(&rs->r), LFD[l], UD(UD_ACCEPT, ri, 0, l));
      else sqe_accept_multishot(ring_sqe(&rs->r), LFD[l], UD(UD_ACCEPT, ri, 0, l));
      rs->accept_armed |= (uint8_t)(1u << l);
    }
}
static void cancel_accepts(unsigned ri) {
  struct rs *rs = &R[ri];
  for (unsigned l = 0; l < NLISTEN; l++)
    if (rs->accept_armed & (1u << l)) {
      sqe_cancel_ud(ring_sqe(&rs->r), UD(UD_ACCEPT, ri, 0, l), UD(UD_CANCEL, ri, 0, l));
      S.cancels++;
    }
}

/* Connections fill the rings in order. The next ring is made ACCEPT_NEXT connections before the
 * current one is full, and accepting moves on ACCEPT_STOP slots before the table is full: an
 * accept that finds no free slot has already taken the connection off the listen queue, and the
 * kernel then drops it. */
static void accept_policy(void) {
  unsigned brief_open = BR < NR ? R[BR].open : 0;
  if (conn_limit || BR < NR) {
    /* full: the limit, or the brief ring's own room (which the limit normally comes before) */
    int full = (conn_limit && open_total >= conn_limit) || (BR < NR && brief_open >= brief_room);
    int clear = (!conn_limit || open_total + conn_limit / 64 < conn_limit) && (BR >= NR || brief_open + brief_room / 4 < brief_room);
    if (full != listen_short && (full || clear)) {
      listen_short = full;
      fprintf(stderr, "connection limit %u %s: %u open (%u brief), %llu refused so far\n", conn_limit, full ? "reached" : "cleared",
              open_total, brief_open, (unsigned long long)refused_total);
    }
  }
  if (BR < NR) {
    /* past KEEP_LIMIT kept connections the newcomers are brief ones, until 1/64 of the kept have
     * left. The count is looked at here, once a pass, so what one pass accepts is kept whole: 8
     * and 40 connections past the limit in the ramps measured; after a pause, whatever the listen
     * queues held, at most up to CONN_LIMIT, where the server is where it was before KEEP_LIMIT. */
    unsigned kept = open_total - brief_open;
    int want = kept >= keep_limit;
    if (want != brief_on && (want || kept + keep_limit / 64 < keep_limit)) {
      brief_on = want;
      fprintf(stderr, "keep-alive limit %u %s: %u kept, %llu brief connections so far\n", keep_limit, want ? "reached" : "cleared",
              kept, (unsigned long long)brief_total);
    }
  }
  {
    /* the listen queues: one entry at the limit. While the newcomers are brief ones, BRIEF_BACKLOG
     * each: a connection in a listen queue is a whole socket with its request in it (~6 KB), and
     * if the loop is kept off the CPU for a second or two, that is the moment the box has no
     * memory to spare; the SYNs past the queue are dropped and come again a second later. */
    int want = listen_short ? 1 : brief_on ? BRIEF_BACKLOG : LISTEN_BACKLOG;
    if (want != listen_now) {
      listen_now = want;
      for (unsigned l = 0; l < NLISTEN; l++) listen(LFD[l], want);
    }
  }
  unsigned last = NR - 1 == BR ? 0 : NR - 1; /* the brief ring is ring 1: it is the last one only beside ring 0 */
  if (NR < NR_MAX && !ring_grow_failed && R[last].open + accept_next >= ring_slots) {
    if (ring_create(NR, 0) == 0) NR++;
    else ring_grow_failed = 1;
  }
  unsigned target = NR;
  if (brief_on) target = BR;
  else
    for (unsigned i = 0; i < NR; i++)
      if (i != BR && R[i].open + accept_stop < ring_slots) { target = i; break; }
  if (target == NR) return; /* every ring is full: the backlog waits */
  if (target != acc_ring && R[acc_ring].accept_armed) cancel_accepts(acc_ring);
  acc_ring = target;
  if (R[target].accept_armed != (1u << NLISTEN) - 1) arm_accepts(target);
}

/* ---------- completions ---------- */

/* What a connection over the limit gets: an answer any HTTP client counts as a failed request. A
 * bare close is an EOF on a connection that has not answered yet, which some clients retry on a
 * new connection without counting anything. */
static const char R_503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: 30\r\n"
                            "Connection: close\r\n\r\n{\"error\":\"server at capacity\"}";

static void on_accept(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  if (!(cqe->flags & IORING_CQE_F_MORE)) rs->accept_armed &= (uint8_t)~(1u << UD_SLOT(cqe->user_data));
  if (cqe->res < 0) {
    if (cqe->res != -ECANCELED && cqe->res != -ENFILE && cqe->res != -EMFILE)
      fprintf(stderr, "accept ring %u: %s\n", ri, strerror(-cqe->res));
    return;
  }
  uint32_t slot = (uint32_t)cqe->res;
  int brief = ri == BR; /* then the slot is a descriptor number */
  if (slot >= (brief ? brief_slots : rs->r.nslots)) {
    fprintf(stderr, "accept: slot %u out of range\n", slot);
    if (brief) close((int)slot);
    return;
  }
  struct conn *c = &rs->conns[slot];
  /* a descriptor number comes back only after its close: if that completion is still behind this
   * one in the queue, the connection that had the number is gone all the same */
  if (brief && (c->flags & (CF_OPEN | CF_CLOSING)) == (CF_OPEN | CF_CLOSING)) conn_freed(ri, c);
  if (c->flags & CF_OPEN) { fprintf(stderr, "accept: slot %u still open\n", slot); return; }
  if ((conn_limit && open_total >= conn_limit) || (brief && rs->open >= brief_room)) { /* full: a 503 and the close, at once (see conn_limit) */
    struct io_uring_sqe *q = ring_sqe(&rs->r);
    sqe_send(q, slot, R_503, sizeof R_503 - 1, UD(UD_CANCEL, ri, 0, slot));
    q->flags = (uint8_t)((brief ? 0 : IOSQE_FIXED_FILE) | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS);
    q = ring_sqe(&rs->r);
    if (brief) sqe_close_fd(q, (int)slot, UD(UD_CANCEL, ri, 0, slot));
    else {
      q->opcode = IORING_OP_CLOSE;
      q->file_index = slot + 1;
      q->user_data = UD(UD_CANCEL, ri, 0, slot);
    }
    q->flags = IOSQE_CQE_SKIP_SUCCESS;
    refused_total++;
    return;
  }
  c->flags = CF_OPEN;
  c->pend_len = 0;
  c->send_arena = NULL;
  rs->open++;
  open_total++;
  S.accepts++;
  if (brief) { brief_recv(ri, slot, c); brief_total++; }
  else arm_idle(ri, slot, c);
}

/* A poll completion is a readiness hint, not request data.  The queued
 * MSG_DONTWAIT read is issued in read_rounds.  Delay rearm until the batch
 * ends if the kernel ended the multishot poll. */
static void on_poll(unsigned ri, struct io_uring_cqe *cqe) {
  unsigned slot = UD_SLOT(cqe->user_data);
  struct conn *c = &R[ri].conns[slot];
  if (!ref_live(cqe->user_data, c)) return;
  S.poll_events++;
  if (!(cqe->flags & IORING_CQE_F_MORE)) {
    c->flags &= (uint16_t)~CF_POLL;
    S.poll_ends++;
    if (!(c->flags & CF_POLL_REARM)) {
      c->flags |= CF_POLL_REARM;
      list_push(&poll_rearms, &n_poll_rearms, &poll_rearms_cap, REF(ri, c->gen, slot));
    }
  }
  if (cqe->res < 0) {
    if (cqe->res != -ECANCELED) finish_conn(ri, slot, c);
    return;
  }
  queue_read(ri, slot, c); /* POLLIN or POLLRDHUP: recv determines data or EOF */
}

static void on_recv(unsigned ri, struct io_uring_cqe *cqe, int multishot) {
  struct rs *rs = &R[ri];
  uint32_t slot = UD_SLOT(cqe->user_data);
  struct conn *c = &rs->conns[slot];
  int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
  unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
  int more = (cqe->flags & IORING_CQE_F_MORE) != 0;
  int live = c->gen == UD_GEN(cqe->user_data) && (c->flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN;
  if (live) {
    if (multishot) { if (!more) c->flags &= (uint16_t)~CF_MULTI; }
    else c->flags &= (uint16_t)~CF_RECVING;
  }
  int kept = 0;
  if (live && cqe->res > 0 && has_buf) kept = received(ri, slot, c, bid, (uint32_t)cqe->res);
  if (has_buf && !kept) ring_buf_add(&rs->r, bid);
  if (!multishot && (cqe->res > 0 || cqe->res == -EAGAIN)) reads_done++;
  if (!live) return;
  if (cqe->res > 0) {
    if (multishot) {
      if (!more) queue_read(ri, slot, c); /* it ended (the buffers ran out behind it): read on in bounded steps */
    } else if (ri == BR) {
      /* a brief connection: its answer closes it; if this was not the whole request, conn_feed reads on */
    } else if ((unsigned)cqe->res == rs->r.buf_size) {
      queue_read(ri, slot, c); /* the buffer filled: there may be more */
    } else {
      arm_idle(ri, slot, c); /* drained; poll mode already has a persistent poll */
    }
    return;
  }
  if (cqe->res == -ENOBUFS) {
    S.enobufs++;
    if (ri == BR) brief_recv(ri, slot, c); /* submitted after this pass has given its buffers back */
    else queue_read(ri, slot, c);
    return;
  }
  if (cqe->res == -EAGAIN && ri != BR) { arm_idle(ri, slot, c); return; }
  finish_conn(ri, slot, c); /* EOF or an error */
}

static void on_send(unsigned ri, struct io_uring_cqe *cqe) {
  uint32_t slot = UD_SLOT(cqe->user_data);
  struct conn *c = &R[ri].conns[slot];
  if (c->gen != UD_GEN(cqe->user_data)) return;
  c->flags &= (uint16_t)~CF_SENDING;
  if (c->send_arena) {
    arena_unref(c->send_arena);
    c->send_arena = NULL;
  }
  if (!(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) return;
  if (cqe->res < 0 || (c->flags & CF_CLOSE_AFTER)) { conn_close(ri, slot, c); return; }
  if (c->pend_len && !(c->flags & (CF_READY | CF_HELD))) { /* pipelined input waited for this send */
    c->flags |= CF_READY;
    list_push(&ready, &nready, &ready_cap, REF(ri, c->gen, slot));
  }
}

static void on_close(unsigned ri, struct io_uring_cqe *cqe) {
  struct conn *c = &R[ri].conns[UD_SLOT(cqe->user_data)];
  if (c->gen != UD_GEN(cqe->user_data) || !(c->flags & CF_CLOSING)) return;
  conn_freed(ri, c);
}

static void on_eventfd(struct io_uring_cqe *cqe) {
  unsigned notified_ring = UD_RING(cqe->user_data);
  if (notified_ring >= NR || R[notified_ring].efd < 0) return;
  S.eventfd_events++;
  /* Drain the eventfd's readiness counter.  It is nonblocking and may have
   * already been drained by another eventfd CQE in this batch.  Without this,
   * an eventfd can remain readable and generate unnecessary poll wakeups. */
  uint64_t count;
  (void)read(R[notified_ring].efd, &count, sizeof count);
  /* The multishot eventfd poll normally stays armed.  If it terminates,
   * rearm it on the primary ring for reliable cross-ring wakeups. */
  if (!(cqe->flags & IORING_CQE_F_MORE) && eventfd_wake)
    sqe_poll_eventfd(ring_sqe(&R[0].r), R[notified_ring].efd,
                     UD(UD_EVENTFD, notified_ring, 0, 0));
}

static unsigned reap(unsigned ri) {
  struct ring *r = &R[ri].r;
  if (LOAD_RLX(r->sq_flags) & IORING_SQ_CQ_OVERFLOW) S.cq_overflow++;
  unsigned head = *r->cq_head, tail = LOAD_ACQ(r->cq_tail), n = 0;
  while (head != tail) {
    struct io_uring_cqe *cqe = &r->cqes[head & r->cq_mask];
    switch (UD_OP(cqe->user_data)) {
    case UD_RECV: on_recv(ri, cqe, 1); break;
    case UD_POLL: on_poll(ri, cqe); break;
    case UD_EVENTFD: on_eventfd(cqe); break;
    case UD_READ: on_recv(ri, cqe, 0); break;
    case UD_SEND: on_send(ri, cqe); break;
    case UD_ACCEPT: on_accept(ri, cqe); break;
    case UD_CLOSE: on_close(ri, cqe); break;
    case UD_DELACK: if (cqe->res < 0 && cqe->res != -EBADF && cqe->res != -ECANCELED) FS.restore_err++; break;
    default: break; /* shutdown, cancel: nothing to do */
    }
    head++;
    n++;
    if ((n & 255) == 0) { /* hand back CQ slots and buffers now, not at the end of a long batch */
      STORE_REL(r->cq_head, head);
      ring_buf_publish(r);
    }
  }
  STORE_REL(r->cq_head, head);
  ring_buf_publish(r); /* returned buffers are usable by the kernel at once (the read rounds need them) */
  return n;
}

/* Avoid rearming a terminal poll from inside the CQE drain.  A stale entry
 * is ignored by the socket generation guard when its slot is reused. */
static void rearm_ended_polls(void) {
  for (unsigned i = 0; i < n_poll_rearms; i++) {
    uint64_t ref = poll_rearms[i];
    struct conn *c = ref_conn(ref);
    if (!ref_live(ref, c) || !(c->flags & CF_POLL_REARM)) continue;
    c->flags &= (uint16_t)~CF_POLL_REARM;
    arm_idle(UD_RING(ref), UD_SLOT(ref), c);
    S.poll_rearmed++;
  }
  n_poll_rearms = 0;
}

static unsigned reap_all(void) {
  unsigned n = 0;
  for (unsigned i = 0; i < NR; i++) n += reap(i);
  return n;
}

/* One read per queued connection, never more on a ring than it has receive buffers; the rest
 * stay queued, in order. Returns the reads issued. */
static unsigned issue_reads(unsigned budget) {
  unsigned issued = 0, end = nrq;
  while (rq_head < end && issued < budget) {
    uint64_t ref = rq[rq_head++];
    struct conn *c = ref_conn(ref);
    if (!ref_live(ref, c) || !(c->flags & CF_RQ)) continue;
    unsigned ri = UD_RING(ref);
    struct rs *rs = &R[ri];
    if (c->flags & (CF_RECVING | CF_MULTI)) { c->flags &= (uint16_t)~CF_RQ; continue; }
    c->flags = (uint16_t)((c->flags & ~CF_RQ) | CF_RECVING);
    sqe_recv_once(ring_sqe(&rs->r), UD_SLOT(ref), UD(UD_READ, ri, c->gen, UD_SLOT(ref)));
    issued++;
  }
  if (rq_head == nrq) rq_head = nrq = 0;
  else if (rq_head > nrq / 2) {
    memmove(rq, rq + rq_head, (nrq - rq_head) * sizeof *rq);
    nrq -= rq_head;
    rq_head = 0;
  }
  return issued;
}

/* Reads the queued connections in rounds: a MSG_DONTWAIT read completes inside the submit, so
 * each round is one non-blocking enter per ring, and the request is answered in this batch. */
static unsigned read_rounds(void) {
  unsigned n = 0, budget = READS_PER_PASS;
  while (budget && rq_head < nrq) {
    unsigned k = issue_reads(budget < recv_bufs ? budget : recv_bufs);
    if (!k) break;
    budget -= k;
    S.reads += k;
    uint64_t before = reads_done;
    for (unsigned i = 0; i < NR; i++)
      if (ring_pending_submit(&R[i].r)) ring_enter(&R[i].r, 0, 0);
    n += reap_all();
    if (reads_done == before) break; /* no buffers for any of them: the next batch tries again */
  }
  return n;
}

/* Answers the requests earlier batches left waiting, oldest first. A long pass (a backlog: up to
 * answers_per_pass, ~0.4 s) reaps every reap_every answers, so what arrives meanwhile moves out of
 * the sockets into pend (behind the requests already waiting) and the receive buffers go back
 * before they run out. Without it the buffers ran dry mid-pass, requests sat in their sockets, and
 * each socket holding one counted a page against tcp_mem: past ~30k of them TCP went into memory
 * pressure and pruned queues (CPU, drops, retransmits) until the OOM killer ended it at 285k. */
static unsigned reap_every = 512;
/* ENTER_EVERY (default 128, 0: off): a long pass also enters every ring each time it reaps. With
 * DEFER_TASKRUN a ring's receive work only runs inside io_uring_enter, and 6.8 runs all of it in
 * one call: a pass that does not enter leaves every request that arrives meanwhile in its socket
 * (a 4 KB page against tcp_mem each; ~30k of them put TCP into memory pressure, whose queue pruning
 * froze the box for seconds), and the next enter then runs thousands of receives in one
 * unpreemptible call (one took 411 ms). Small, frequent enters keep both short. */
static unsigned enter_every = 128;
static void run_ready(void) {
  unsigned n = nready, i = 0, since = 0;
  /* Only time-slice an established backlog. Normal batches follow the original fast path. */
  uint64_t deadline = backlog_slice_us && nready > 1024 ? now_ns() + (uint64_t)backlog_slice_us * 1000ULL : 0;
  for (; i < n && answers_left; i++) {
    if (deadline && (i & 127u) == 0 && now_ns() >= deadline) {
      /* Save all unprocessed ready references for the next committed batch.
       * New CQEs are still drained into connection-local pending storage. */
      answers_left = 0;
      sliced_ready_passes++;
      break;
    }
    if (reap_every && ++since >= reap_every) {
      since = 0;
      unsigned keep = answers_left;
      answers_left = 0;
      if (enter_every)
        for (unsigned r = 0; r < NR; r++) ring_enter(&R[r].r, 0, 0);
      reap_all();
      answers_left = keep;
    }
    uint64_t ref = ready[i];
    struct conn *c = ref_conn(ref);
    if (c->gen != UD_GEN(ref)) continue;
    c->flags &= (uint16_t)~CF_READY;
    if (!(c->flags & CF_OPEN) || (c->flags & (CF_CLOSING | CF_SENDING | CF_HELD | CF_REPLIED))) continue;
    answers_left--;
    conn_feed(UD_RING(ref), UD_SLOT(ref), c, NULL, 0);
    if ((c->flags & CF_CLOSE_AFTER) && !(c->flags & (CF_REPLIED | CF_SENDING))) conn_close(UD_RING(ref), UD_SLOT(ref), c);
  }
  memmove(ready, ready + i, (nready - i) * sizeof *ready);
  nready -= i;
}

static void release_held(void) {
  unsigned n = nheld;
  nheld = 0;
  for (unsigned i = 0; i < n; i++) {
    uint64_t ref = held[i];
    struct conn *c = ref_conn(ref);
    if (c->gen != UD_GEN(ref)) continue;
    c->flags &= (uint16_t)~CF_HELD;
    if (!(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
    conn_feed(UD_RING(ref), UD_SLOT(ref), c, NULL, 0);
    if ((c->flags & CF_CLOSE_AFTER) && !(c->flags & (CF_REPLIED | CF_SENDING))) conn_close(UD_RING(ref), UD_SLOT(ref), c);
  }
}

static const char R_500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";

/* After the commit: every reply goes to the kernel as one send from the arena. If the commit
 * failed, every reply that touched the database becomes a 500: no 201 ever claims a write that
 * did not land, and no read ever reports a row that rolled back. */
static void flush_replies(int commit_failed) {
  for (unsigned i = 0; i < nrep; i++) {
    struct reply *rp = &replies[i];
    struct conn *c = ref_conn(rp->ref);
    if (c->gen != UD_GEN(rp->ref)) continue;
    c->flags &= (uint16_t)~CF_REPLIED;
    if (!(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
    struct arena *a = rp->a;
    const uint8_t *p = a->data + rp->off;
    unsigned len = rp->len;
    if (commit_failed && rp->db) {
      p = (const uint8_t *)R_500;
      len = sizeof R_500 - 1;
      c->flags |= CF_CLOSE_AFTER;
    }
    unsigned ri = UD_RING(rp->ref);
    if (ri == BR) {
      /* a brief connection: the reply, and the close right behind it in the same chain, so the FIN
       * reaches the client with the reply and the socket is gone when the send is done */
      uint32_t slot = UD_SLOT(rp->ref);
      int chain = (c->flags & CF_CLOSE_AFTER) && !(c->flags & CF_RECVING);
      struct io_uring_sqe *q = ring_sqe(&R[ri].r);
      sqe_send(q, slot, p, len, UD(UD_SEND, ri, c->gen, slot));
      q->flags = chain ? IOSQE_IO_HARDLINK : 0; /* an ordinary descriptor */
      if (chain) {
        sqe_close_fd(ring_sqe(&R[ri].r), (int)slot, UD(UD_CLOSE, ri, c->gen, slot));
        c->flags |= CF_CLOSING;
        S.closes++;
      }
      a->refs++;
      c->send_arena = a;
      c->flags |= CF_SENDING;
      continue;
    }
    sqe_send(ring_sqe(&R[ri].r), UD_SLOT(rp->ref), p, len, UD(UD_SEND, ri, c->gen, UD_SLOT(rp->ref)));
    if (rp->late && delack_restore) {
      sqe_delack_mode(ring_sqe(&R[ri].r), UD_SLOT(rp->ref), UD(UD_DELACK, ri, c->gen, UD_SLOT(rp->ref)));
      FS.restores++;
    }
    a->refs++;
    c->send_arena = a;
    c->flags |= CF_SENDING;
  }
  nrep = 0;
  for (unsigned i = 0; i < n_batch_arenas; i++) arena_unref(batch_arenas[i]);
  n_batch_arenas = 0;
  cur = NULL;
}

static struct {
  int enabled, normal_rcvbuf, pressure_rcvbuf, current_rcvbuf;
  uint64_t last_sample_ns, samples, changes, cg_current, cg_limit, cg_sock;
  unsigned high_streak, low_streak;
  double psi_some10, psi_full10;
  unsigned long tcp_mem_pages;
} MG;

/* ---------- housekeeping ---------- */

static uint64_t T_reqs, T_accepts, T_closes;
static long majflt_prev;
static int trim_due; /* a backlog freed heap memory: hand it back once the backlog is gone */
static unsigned long long n_trims;
static void stats(uint64_t dt_ns) {
  unsigned open = 0;
  fprintf(stderr, "rings open=");
  for (unsigned i = 0; i < NR; i++) { open += R[i].open; fprintf(stderr, "%s%u%s", i ? "," : "", R[i].open, R[i].accept_armed ? "*" : ""); }
  T_reqs += S.reqs; T_accepts += S.accepts; T_closes += S.closes;
  double dt = (double)dt_ns / 1e9;
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  double cpu = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
  int c_hit = 0, c_miss = 0, hw;
  sqlite3_db_status(A.db.c, SQLITE_DBSTATUS_CACHE_HIT, &c_hit, &hw, 1);
  sqlite3_db_status(A.db.c, SQLITE_DBSTATUS_CACHE_MISS, &c_miss, &hw, 1);
  if (S.deferred || S.enobufs) trim_due = 1;
  else if (trim_due && nready == 0) { malloc_trim(0); trim_due = 0; n_trims++; } /* pend buffers a backlog freed leave holes malloc would keep */
  struct mallinfo2 mi = mallinfo2();
  fprintf(stderr,
          " total_reqs=%llu req/s=%.0f batches/s=%.0f avg_batch=%.1f max_batch=%llu accepts=%llu closes=%llu enobufs=%llu reads=%llu "
          "deferred=%llu rq_max=%llu ready_max=%llu cq_overflow=%llu cancels=%llu wal=%d ckpt_passes=%llu cycles=%llu force=%llu "
          "fallback=%llu errors=%llu cache_miss=%d maxrss=%ldMB cpu=%.2fs max_turn=%.1fms max_commit=%.1fms wal_pwrites=%llu "
          "bulk_max=%.0fms hold_max=%.1fms hold_total=%.1fms hold_timeouts=%llu tail_frames_max=%llu tail_short=%llu passes_max=%llu "
          "wait_max=%.1fms majflt=%ld tail_restarts=%llu begin_busy=%llu heap=%zuMB heap_free=%zuMB arenas=%u trims=%llu slices=%llu "
          "poll_events=%llu poll_ends=%llu poll_rearms=%llu eventfd_events=%llu "
          "fresh=%llu spilled=%llu late_passes=%llu delack_restores=%llu restore_err=%llu refused=%llu cost_us=%.1f reserve_passes=%llu "
          "brief=%llu brief_open=%u brief_expired=%llu\n",
          (unsigned long long)T_reqs, (double)S.reqs / dt, (double)S.batches / dt, S.batches ? (double)S.cqes / (double)S.batches : 0.0,
          (unsigned long long)S.max_batch, (unsigned long long)S.accepts, (unsigned long long)S.closes, (unsigned long long)S.enobufs,
          (unsigned long long)S.reads, (unsigned long long)S.deferred, (unsigned long long)S.rq_max, (unsigned long long)S.ready_max,
          (unsigned long long)S.cq_overflow, (unsigned long long)S.cancels, A.db.wal_frames, (unsigned long long)A.db.n_ckpt,
          (unsigned long long)A.db.n_cycles, (unsigned long long)A.db.n_force, (unsigned long long)A.db.n_fallback,
          (unsigned long long)A.n_err, c_miss, ru.ru_maxrss / 1024, cpu, (double)S.max_turn_ns / 1e6, (double)S.max_commit_ns / 1e6,
          (unsigned long long)wal_pwrites, (double)A.db.bulk_max_ns / 1e6, (double)A.db.hold_max_ns / 1e6,
          (double)A.db.hold_total_ns / 1e6, (unsigned long long)A.db.n_hold_timeout, (unsigned long long)A.db.tail_frames_max,
          (unsigned long long)A.db.n_tail_short, (unsigned long long)A.db.n_passes_max, (double)S.max_gap_ns / 1e6, ru.ru_majflt - majflt_prev,
          (unsigned long long)A.db.n_tail_restart, (unsigned long long)A.db.n_begin_busy, mi.arena >> 20, mi.fordblks >> 20, arenas_mapped, n_trims,
          (unsigned long long)sliced_ready_passes, (unsigned long long)S.poll_events,
          (unsigned long long)S.poll_ends, (unsigned long long)S.poll_rearmed,
          (unsigned long long)S.eventfd_events, (unsigned long long)FS.fresh, (unsigned long long)FS.spilled,
          (unsigned long long)FS.late_passes, (unsigned long long)FS.restores, (unsigned long long)FS.restore_err,
          (unsigned long long)refused_total, cost_ns / 1000.0, (unsigned long long)FS.reserved,
          (unsigned long long)brief_total, BR < NR ? R[BR].open : 0, (unsigned long long)brief_expired);
  memset(&FS, 0, sizeof FS);
  if (MG.enabled) fprintf(stderr,
      " tcp_memgov rcvbuf=%d pressure=%d psi_some10=%.2f psi_full10=%.2f cg_mem=%lluMB cg_max=%lluMB cg_sock=%lluMB tcp_mem_pages=%lu changes=%llu samples=%llu\n",
      MG.current_rcvbuf, MG.current_rcvbuf == MG.pressure_rcvbuf, MG.psi_some10, MG.psi_full10,
      (unsigned long long)(MG.cg_current >> 20), (unsigned long long)(MG.cg_limit >> 20),
      (unsigned long long)(MG.cg_sock >> 20), MG.tcp_mem_pages,
      (unsigned long long)MG.changes, (unsigned long long)MG.samples);
  majflt_prev = ru.ru_majflt;
  memset(&S, 0, sizeof S);
  A.db.bulk_max_ns = A.db.hold_max_ns = A.db.hold_total_ns = A.db.tail_frames_max = 0;
}

static long env_long(const char *name, long def) {
  const char *v = getenv(name);
  return v && *v ? atol(v) : def;
}

/* Opt-in TCP receive-memory governor. It only changes the buffer limits
 * inherited by FUTURE accepted sockets, never existing connections. Sampling
 * cgroup v2 PSI and sock memory is infrequent to avoid hot-path file I/O. */

static int small_read(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  return 0;
}
static uint64_t cg_read_u64(const char *path) {
  char buf[64];
  if (small_read(path, buf, sizeof buf) != 0 || buf[0] < '0' || buf[0] > '9') return 0;
  return strtoull(buf, NULL, 10); /* memory.max may be 'max': treat as unbounded */
}
static int apply_listener_rcvbuf(int requested) {
  for (unsigned i = 0; i < NLISTEN; i++) {
    if (setsockopt(LFD[i], SOL_SOCKET, SO_RCVBUF, &requested, sizeof requested) < 0) {
      fprintf(stderr, "tcp governor: SO_RCVBUF listener %u: %s\n", i, strerror(errno));
      return -1;
    }
  }
  int effective = 0;
  socklen_t len = sizeof effective;
  if (!getsockopt(LFD[0], SOL_SOCKET, SO_RCVBUF, &effective, &len))
    fprintf(stderr, "tcp governor: listener cap requested=%d effective=%d (Linux doubles requested)\n", requested, effective);
  MG.current_rcvbuf = requested;
  MG.changes++;
  return 0;
}
static int memgov_start(void) {
  MG.enabled = env_long("TCP_MEM_GOVERNOR", 1) != 0;
  if (!MG.enabled) return 0;
  MG.normal_rcvbuf = (int)env_long("TCP_RCVBUF_NORMAL", 16384);
  MG.pressure_rcvbuf = (int)env_long("TCP_RCVBUF_PRESSURE", 8192);
  if (MG.pressure_rcvbuf < 4096 || MG.normal_rcvbuf > 1048576 || MG.pressure_rcvbuf > MG.normal_rcvbuf) {
    fputs("TCP_RCVBUF_PRESSURE must be >=4096 and <= TCP_RCVBUF_NORMAL <=1048576\n", stderr);
    return -1;
  }
  return apply_listener_rcvbuf(MG.normal_rcvbuf);
}
static void memgov_tick(uint64_t now) {
  if (!MG.enabled || (MG.last_sample_ns && now - MG.last_sample_ns < 2000000000ULL)) return;
  MG.last_sample_ns = now;
  MG.samples++;
  MG.cg_current = cg_read_u64("/sys/fs/cgroup/memory.current");
  MG.cg_limit = cg_read_u64("/sys/fs/cgroup/memory.max");
  MG.cg_sock = 0;
  MG.psi_some10 = MG.psi_full10 = 0.0;
  char buf[8192];
  int psi_available = 0;
  if (small_read("/sys/fs/cgroup/memory.stat", buf, sizeof buf) == 0) {
    const char *p = strstr(buf, "sock ");
    if (p) MG.cg_sock = strtoull(p + 5, NULL, 10);
  }
  if (small_read("/sys/fs/cgroup/memory.pressure", buf, sizeof buf) == 0) {
    const char *p = strstr(buf, "some avg10=");
    const char *q = strstr(buf, "full avg10=");
    if (p && q && sscanf(p, "some avg10=%lf", &MG.psi_some10) == 1 &&
        sscanf(q, "full avg10=%lf", &MG.psi_full10) == 1) psi_available = 1;
  }
  MG.tcp_mem_pages = 0;
  if (small_read("/proc/net/sockstat", buf, sizeof buf) == 0) {
    const char *tcp = strstr(buf, "TCP:");
    if (tcp) {
      const char *line_end = strchr(tcp, '\n');
      const char *mem = strstr(tcp, " mem ");
      if (mem && (!line_end || mem < line_end)) MG.tcp_mem_pages = strtoul(mem + 5, NULL, 10);
    }
  }
  int high = (psi_available && (MG.psi_full10 >= 2.0 || MG.psi_some10 >= 10.0)) ||
             (MG.cg_limit && MG.cg_current > MG.cg_limit * 85 / 100);
  if (high) { MG.high_streak++; MG.low_streak = 0; }
  else { MG.low_streak++; MG.high_streak = 0; }
  /* Changes affect only new accepts. Never throttle, reject or close a connection. */
  if (MG.high_streak >= 2 && MG.current_rcvbuf != MG.pressure_rcvbuf) {
    if (apply_listener_rcvbuf(MG.pressure_rcvbuf) == 0) MG.high_streak = 0;
  } else if (MG.low_streak >= 4 && MG.current_rcvbuf != MG.normal_rcvbuf) {
    if (apply_listener_rcvbuf(MG.normal_rcvbuf) == 0) MG.low_streak = 0;
  }
}

/* Keeps the binary's code and data in RAM: file-backed pages are what reclaim takes first when
 * memory runs short (no swap), and a loop stalling on major faults for its own code while every
 * connection waits is the last thing wanted at the memory wall. Best effort. */
static size_t lock_code(void) {
  char exe[512];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  FILE *f = fopen("/proc/self/maps", "r");
  if (n <= 0 || !f) { if (f) fclose(f); return 0; }
  exe[n] = 0;
  char line[1024];
  size_t locked = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long lo, hi;
    char perms[8];
    int path = 0;
    if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &path) < 3 || !path) continue;
    line[strcspn(line, "\n")] = 0;
    if (strcmp(line + path, exe)) continue;
    if (mlock((void *)lo, hi - lo) == 0) locked += hi - lo;
  }
  fclose(f);
  return locked;
}

/* SIGTERM/SIGINT: the loop exits between batches (exit() in a handler could deadlock on a
 * lock the interrupted code holds; the instrumented build writes its profile at exit). */
static volatile sig_atomic_t stop_requested;
static void on_term(int sig) { (void)sig; stop_requested = 1; }

/* Build time only: a synthetic database for PGO training (random data, none of the seed). */
static int make_train_db(const char *path) {
  long users = 5000, posts = 40000, likes = 160000;
  sqlite3_initialize();
  unlink(path);
  sqlite3 *c;
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
  if (sqlite3_exec(c, schema, NULL, NULL, NULL) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(c)); return 1; }
  sqlite3_stmt *su, *sp, *sl;
  if (sqlite3_prepare_v2(c, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1, &su, NULL) ||
      sqlite3_prepare_v2(c, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)", -1, &sp, NULL) ||
      sqlite3_prepare_v2(c, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)", -1, &sl, NULL) || !su || !sp || !sl)
    return 1;
  srand(4242);
  char buf[600];
  for (long i = 1; i <= users; i++) {
    snprintf(buf, sizeof buf, "trainer_%ld", i);
    sqlite3_bind_int64(su, 1, i);
    sqlite3_bind_text(su, 2, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(su);
    sqlite3_reset(su);
  }
  static const char *words[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet"};
  for (long i = 1; i <= posts; i++) {
    int n = 0, nw = 5 + rand() % 40;
    for (int w = 0; w < nw && n < 480; w++) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s", w ? " " : "", words[rand() % 10]);
    long t = 1704067200L + i * 60;
    char ts[32], created[40];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", gmtime(&t));
    snprintf(created, sizeof created, "%s.%03dZ", ts, rand() % 1000);
    sqlite3_bind_int64(sp, 1, i);
    sqlite3_bind_int64(sp, 2, 1 + rand() % users);
    sqlite3_bind_text(sp, 3, buf, n, SQLITE_TRANSIENT);
    sqlite3_bind_text(sp, 4, created, -1, SQLITE_TRANSIENT);
    sqlite3_step(sp);
    sqlite3_reset(sp);
  }
  for (long i = 0; i < likes; i++) {
    sqlite3_bind_int64(sl, 1, 1 + rand() % users);
    sqlite3_bind_int64(sl, 2, 1 + rand() % posts);
    sqlite3_step(sl);
    sqlite3_reset(sl);
  }
  sqlite3_finalize(su); sqlite3_finalize(sp); sqlite3_finalize(sl);
  if (sqlite3_exec(c, "COMMIT; ANALYZE; PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL) != SQLITE_OK) return 1;
  sqlite3_close(c);
  return 0;
}

/* Development only: the request handlers in-process, no network, to time them precisely.
 *   bin/server --bench <db copy> [iterations] */
static int bench(const char *db_path, long iters, int only, int k6_state) {
  if (app_init(&A, db_path, "twelve-dollar-challenge", 49152) < 0) return 1;
  if (k6_state) { /* what the load test leaves after a second: short posts by random users, a few likes each */
    sqlite3_stmt *sp, *sl;
    sqlite3_prepare_v2(A.db.c, "INSERT INTO posts (user_id, body) VALUES (?1, ?2)", -1, &sp, NULL);
    sqlite3_prepare_v2(A.db.c, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)", -1, &sl, NULL);
    sqlite3_exec(A.db.c, "BEGIN", NULL, NULL, NULL);
    srand(99);
    for (int i = 0; i < 3000; i++) {
      char body[128];
      int bn = snprintf(body, sizeof body, "quiet_otter_%d says hi at 2026-11-11T16:35:32.464Z (VU %d, iter %d)", rand() % 20000, rand() % 300000, rand() % 50);
      sqlite3_bind_int64(sp, 1, 1 + rand() % 20000);
      sqlite3_bind_text(sp, 2, body, bn, SQLITE_TRANSIENT);
      sqlite3_step(sp);
      sqlite3_reset(sp);
      sqlite3_int64 id = sqlite3_last_insert_rowid(A.db.c);
      for (int k = 0; k < 7; k++) {
        sqlite3_bind_int64(sl, 1, 1 + rand() % 20000);
        sqlite3_bind_int64(sl, 2, id);
        sqlite3_step(sl);
        sqlite3_reset(sl);
      }
    }
    sqlite3_exec(A.db.c, "COMMIT", NULL, NULL, NULL);
    sqlite3_finalize(sp);
    sqlite3_finalize(sl);
  }
  static uint8_t region[RESP_MAX];
  struct resp rp;
  struct req r;
  memset(&r, 0, sizeof r);
  const char *names[] = {"GET /feed", "GET /posts/:id", "POST like", "POST /posts", "mixed batch of 30"};
  for (int which = 0; which < 5; which++) {
    if (only >= 0 && which != only) continue;
    uint64_t t0 = now_ns(), bytes = 0;
    long n = which == 3 ? iters / 10 : iters;
    for (long i = 0; i < n; i++) {
      char path[64], auth[1024], body[256];
      int w = which == 4 ? (i % 30 < 20 ? 0 : i % 30 < 28 ? 1 : i % 30 < 29 ? 2 : 3) : which;
      r.method = w >= 2 ? M_POST : M_GET;
      r.auth = NULL;
      r.body = NULL;
      r.body_len = 0;
      if (w == 0) { r.path = (const uint8_t *)"/feed"; r.path_len = 5; }
      else if (w == 1) { r.path_len = (uint32_t)snprintf(path, sizeof path, "/posts/%ld", 500000 - i % 20); r.path = (const uint8_t *)path; }
      else {
        /* a token for user 1 */
        static char tok[1024];
        static int tok_ok;
        if (!tok_ok) {
          const char *hb = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
          char payload[128], pb[256], msg[512], sb[64];
          snprintf(payload, sizeof payload, "{\"sub\":\"1\",\"username\":\"golden_ember_1\",\"iat\":1,\"exp\":4102444800}");
          /* base64url of the payload */
          static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
          size_t pn = strlen(payload), o = 0, k = 0;
          while (k + 3 <= pn) { uint32_t v = (uint32_t)(uint8_t)payload[k] << 16 | (uint32_t)(uint8_t)payload[k + 1] << 8 | (uint8_t)payload[k + 2]; pb[o++] = B64[v >> 18]; pb[o++] = B64[(v >> 12) & 63]; pb[o++] = B64[(v >> 6) & 63]; pb[o++] = B64[v & 63]; k += 3; }
          if (pn - k == 1) { uint32_t v = (uint32_t)(uint8_t)payload[k] << 16; pb[o++] = B64[v >> 18]; pb[o++] = B64[(v >> 12) & 63]; }
          else if (pn - k == 2) { uint32_t v = (uint32_t)(uint8_t)payload[k] << 16 | (uint32_t)(uint8_t)payload[k + 1] << 8; pb[o++] = B64[v >> 18]; pb[o++] = B64[(v >> 12) & 63]; pb[o++] = B64[(v >> 6) & 63]; }
          pb[o] = 0;
          int mn = snprintf(msg, sizeof msg, "%s.%s", hb, pb);
          uint8_t sig[32];
          hmac_sha256(&A.key, (const uint8_t *)msg, (size_t)mn, sig);
          o = 0; k = 0;
          while (k + 3 <= 32) { uint32_t v = (uint32_t)sig[k] << 16 | (uint32_t)sig[k + 1] << 8 | sig[k + 2]; sb[o++] = B64[v >> 18]; sb[o++] = B64[(v >> 12) & 63]; sb[o++] = B64[(v >> 6) & 63]; sb[o++] = B64[v & 63]; k += 3; }
          { uint32_t v = (uint32_t)sig[30] << 16 | (uint32_t)sig[31] << 8; sb[o++] = B64[v >> 18]; sb[o++] = B64[(v >> 12) & 63]; sb[o++] = B64[(v >> 6) & 63]; }
          sb[o] = 0;
          snprintf(tok, sizeof tok, "%s.%s", msg, sb);
          tok_ok = 1;
        }
        r.auth_len = (uint32_t)snprintf(auth, sizeof auth, "Bearer %s", tok);
        r.auth = (const uint8_t *)auth;
        if (w == 2) { r.path_len = (uint32_t)snprintf(path, sizeof path, "/posts/%ld/like", 1 + i % 400000); r.path = (const uint8_t *)path; }
        else {
          r.path = (const uint8_t *)"/posts"; r.path_len = 6;
          r.body_len = (uint32_t)snprintf(body, sizeof body, "{\"body\":\"bench says hi at %ld (iter %ld)\"}", (long)time(NULL), i);
          r.body = (const uint8_t *)body;
        }
      }
      app_handle(&A, &r, region, 1, &rp);
      bytes += rp.n;
      if (rp.p[9] != '2') { fprintf(stderr, "%s: status %.3s\n", names[which], rp.p + 9); return 1; }
      if (i % 30 == 29) { db_commit(&A.db); db_tick(&A.db); }
    }
    db_commit(&A.db);
    uint64_t dt = now_ns() - t0;
    fprintf(stderr, "%-20s %8.0f ns/op  (%ld ops, %.0f bytes/op)\n", names[which], (double)dt / (double)n, n, (double)bytes / (double)n);
  }
  return 0;
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  setvbuf(stderr, NULL, _IOLBF, 0);
  if (argc == 3 && !strcmp(argv[1], "--make-train-db")) return make_train_db(argv[2]);
  if (argc >= 3 && !strcmp(argv[1], "--bench"))
    return bench(argv[2], argc > 3 ? atol(argv[3]) : 200000, argc > 4 ? atoi(argv[4]) : -1, argc > 5 && !strcmp(argv[5], "k6"));
  const char *host = getenv("HOST"), *port = getenv("PORT"), *db_path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3000";
  if (!db_path || !*db_path || !secret || !*secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }

  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
  }
  unsigned nslots = rl.rlim_cur > 65535 ? 65535 : (unsigned)rl.rlim_cur;
  if (nslots > 1024) nslots -= 64; /* room for the process's own descriptors */
  ring_slots = nslots;
  accept_stop = nslots / 10 < ACCEPT_STOP_MAX ? nslots / 10 : ACCEPT_STOP_MAX;
  accept_next = nslots / 8 < ACCEPT_NEXT_MAX ? nslots / 8 : ACCEPT_NEXT_MAX;
  if (accept_stop < 16) accept_stop = 16;
  if (accept_next <= accept_stop) accept_next = accept_stop + 16;
  recv_bufs = (unsigned)env_long("RECV_BUFS", 8192);
  fresh_first = env_long("FRESH_FIRST", 1) != 0;
  fresh_reserve = (unsigned)env_long("FRESH_RESERVE", 24);
  backlog_age_ms = (unsigned)env_long("BACKLOG_AGE_MS", 20000);
  backlog_quiet_ms = (unsigned)env_long("BACKLOG_QUIET_MS", 30);
  late_ms = (unsigned)env_long("LATE_MS", 20);
  delack_restore = env_long("DELACK_RESTORE", 1) != 0;
  quickack_off = env_long("QUICKACK_OFF", 1) != 0;
  /* newest first: a pass is ~10 ms of work, so what arrives during it is answered inside the
   * delayed-ACK window by the next one, and it needs no reaping or entering mid-pass */
  answers_per_pass = (unsigned)env_long("ANSWERS_PER_PASS", fresh_first ? 0 : 2048);
  deadline_ns = (uint64_t)env_long("DEADLINE_MS", 32) * 1000000ULL;
  pass_ns = (uint64_t)env_long("PASS_MS", 8) * 1000000ULL;
  reap_every = (unsigned)env_long("REAP_EVERY", fresh_first ? 0 : 512);
  enter_every = (unsigned)env_long("ENTER_EVERY", fresh_first ? 0 : 128);
  if (enter_every) reap_every = enter_every;
  if (answers_per_pass && fresh_reserve > answers_per_pass / 2) fresh_reserve = answers_per_pass / 2;
  fr_cap = answers_per_pass ? (answers_per_pass < 64 ? 64 : answers_per_pass) : 4096;
  if (fr_cap > recv_bufs / 2) fr_cap = recv_bufs / 2; /* arrivals stay in their receive buffers until the pass has seen every ring */
  if (fr_cap < 64) fr_cap = 64;
  submit_chunk = (unsigned)env_long("SUBMIT_CHUNK", 128);
  poll_idle = env_long("IDLE_POLL", 0) != 0;
  eventfd_wake = env_long("EVENTFD_WAKE", 0) != 0;
  recv_buf_size = (unsigned)env_long("RECV_SIZE", RECV_BUF_SIZE_DEFAULT);
  if (recv_buf_size < 256 || recv_buf_size > 2048 || (recv_buf_size & (recv_buf_size - 1))) {
    fprintf(stderr, "RECV_SIZE must be a power of 2 between 256 and 2048\n");
    return 1;
  }
  backlog_slice_us = (unsigned)env_long("BACKLOG_SLICE_US", 0); /* 0: a ready backlog is answered without a time limit */
  /* Ring 0 waits for 64 completions or 2 ms: fewer, fuller passes (each pass enters every ring).
   * Measured at 300k connections: ~4 % less loop CPU than 256 / 1 ms, p99 unchanged. */
  wait_min = (unsigned)env_long("WAIT_MIN", 64);
  wait_ns = (uint64_t)env_long("WAIT_US", 2000) * 1000ULL;
  ckpt_frames = (int)env_long("CKPT_FRAMES", 16384);
  ckpt_urgent = (int)env_long("CKPT_URGENT", 32768);
  db_drop_ms = env_long("DB_DROP_MS", 2000);
  ckpt_tail = (int)env_long("CKPT_TAIL", 64);
  hold_max_ns = (uint64_t)env_long("HOLD_MAX_MS", 400) * 1000000ULL;
  wb_kick_ms = env_long("WB_KICK_MS", 250);
  ckpt_log = (int)env_long("CKPT_LOG", 0);
  tail_restart_on = (int)env_long("TAIL_RESTART", 0);
  wal_ra_on = (int)env_long("WAL_READAHEAD", 1);
  long want = env_long("MAX_CONNS", 600000);
  {
    /* what fits beside 600 MB for everything else (the database cache, the rings and buffers, the
     * page cache the log needs, the rest of the system) at 4.25 KB a connection: 329,000 on 2 GB */
    long mem_kb = 0;
    FILE *mf = fopen("/proc/meminfo", "r");
    if (mf) {
      if (fscanf(mf, "MemTotal: %ld kB", &mem_kb) != 1) mem_kb = 0;
      fclose(mf);
    }
    long fit = mem_kb > 700 * 1024 ? (long)((double)(mem_kb - 600 * 1024) / 4.25) : 0;
    long lim = env_long("CONN_LIMIT", fit);
    if (lim < 0) lim = 0;
    if (lim > want) lim = want;
    conn_limit = (unsigned)lim;
    /* kept alive: 30 MB less, ~7,000 fewer (see keep_limit); a CONN_LIMIT set under that is the only limit */
    long keep = env_long("KEEP_LIMIT", mem_kb > 700 * 1024 ? (long)((double)(mem_kb - 630 * 1024) / 4.25) : 0);
    if (keep < 0 || (lim && keep >= lim)) keep = 0;
    keep_limit = (unsigned)keep;
    brief_slots = 65536; /* descriptor numbers stay under the descriptor limit */
    brief_room = rl.rlim_cur > 33280 ? 32768 : rl.rlim_cur > 768 ? (unsigned)rl.rlim_cur - 512 : 256;
    brief_wait_ms = (unsigned)env_long("BRIEF_WAIT_MS", 5000);
  }
  NR_MAX = (unsigned)((want + nslots - 1) / nslots) + (keep_limit ? 1 : 0);
  if (NR_MAX < 1) NR_MAX = 1;
  if (NR_MAX > MAX_RINGS) NR_MAX = MAX_RINGS;
  int cache_kib = (int)env_long("CACHE_KIB", 65536); /* 64 MB: the likes index (24 MB), the post tails and their indexes stay resident */

  if (app_init(&A, db_path, secret, cache_kib) < 0) return 1;

  const char *err = "";
  for (unsigned l = 0; l < NLISTEN; l++) {
    LFD[l] = listen_socket(host, atoi(port), LISTEN_BACKLOG, &err);
    if (LFD[l] < 0) { fprintf(stderr, "listen %s:%s: %s: %s\n", host, port, err, strerror(errno)); return 1; }
    /* TCP_QUICKACK off on a listener: accepted sockets start in delayed-ACK mode, so a connection's
     * first request is acknowledged by its response instead of by a packet of its own (as in
     * fsharp-uring-delneg #437). During the ramp that is one packet less per new connection. */
    if (quickack_off) { int zero = 0; if (setsockopt(LFD[l], IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof zero) < 0) perror("TCP_QUICKACK"); }
  }
  if (memgov_start() < 0) return 1; /* listener buffers before the first accept */
  if (ring_create(0, 0) < 0) return 1;
  NR = 1;
  if (keep_limit && NR_MAX >= 2) { /* the brief ring is ring 1 */
    if (ring_create(1, 1) == 0) { BR = 1; NR = 2; }
    else { fprintf(stderr, "no brief ring: every connection is kept, up to CONN_LIMIT\n"); keep_limit = 0; }
  } else keep_limit = 0;
  /* The rings a full load needs are made now, while the box is idle: a ring is ~1 MB of
   * physically contiguous kernel memory, and asking for that mid-ramp, with a gigabyte of socket
   * slab fragmenting the memory, can stall the loop in compaction. */
  long pre = env_long("PRE_CONNS", 330000); /* pre-create six rings for a 310k+ run; use 430000 for 400k */
  while (NR < NR_MAX && (long)(NR - (BR < MAX_RINGS)) * (long)nslots < pre && ring_create(NR, 0) == 0) NR++;
  prewarm();
  fprintf(stderr, "experiment: idle_poll=%d eventfd_wake=%d recv_size=%u backlog_slice_us=%u pre_conns=%ld "
          "fresh_first=%d answers_per_pass=%u deadline_ms=%u pass_ms=%u fresh_reserve=%u backlog_age_ms=%u backlog_quiet_ms=%u late_ms=%u delack_restore=%d quickack_off=%d conn_limit=%u keep_limit=%u\n",
          poll_idle, eventfd_wake, recv_buf_size, backlog_slice_us, pre, fresh_first, answers_per_pass,
          (unsigned)(deadline_ns / 1000000ULL), (unsigned)(pass_ns / 1000000ULL), fresh_reserve, backlog_age_ms, backlog_quiet_ms,
          late_ms, delack_restore, quickack_off, conn_limit, keep_limit);
  fprintf(stderr, "listening on %s:%s: up to %u rings x %u direct descriptors; cache %d KiB; code locked %zu KiB\n", host, port, NR_MAX,
          nslots, cache_kib, lock_code() >> 10);
  accept_policy();

  uint64_t last_stats = now_ns(), te_prev = 0, t_reap = 0;
  unsigned last_cqes = 0;
  for (;;) {
    /* 1. the other rings: submit what the last batch queued and collect their completions */
    for (unsigned i = 1; i < NR; i++) {
      if (i == BR && !R[i].open && !R[i].accept_armed && !ring_pending_submit(&R[i].r)) continue; /* nothing can happen there */
      if (!eventfd_wake || ring_pending_submit(&R[i].r)) ring_enter(&R[i].r, 0, 0);
    }
    /* 2. ring 0 waits (unless work is already queued): bigger batches share one commit */
    int busy = (nready && !backlog_parked) || (nrq > rq_head) || (nheld && !A.db.writes_held);
    long rc;
    if (busy) rc = ring_enter(&R[0].r, 0, 0);
    else rc = ring_enter(&R[0].r, last_cqes >= 16 ? wait_min : 1, last_cqes || nready ? wait_ns : IDLE_NS); /* a backlog that waits is looked at again soon */
    if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EBUSY && rc != -EAGAIN) {
      fprintf(stderr, "io_uring_enter: %s\n", strerror((int)-rc));
      return 1;
    }
    /* 3. the batch */
    uint64_t t = now_ns();
    now_ms = (uint32_t)(t / 1000000ULL);
    if (last_cqes && te_prev && t - te_prev > S.max_gap_ns) S.max_gap_ns = t - te_prev; /* a busy wait should take ~1 ms */
    A.now = (int64_t)time(NULL);
    writes_ok = !A.db.writes_held;
    if (writes_ok && db_begin_write(&A.db) < 0) writes_ok = 0; /* the tail thread is restarting the log: this batch reads only */
    if (fresh_first && !answers_per_pass) {
      /* what an answer costs: the last pass and its enter, if this enter did not wait (it does not
       * when there is work queued, nor while arrivals are going to a backlog that waits) */
      if ((busy || (backlog_parked && now_ms - spill_ms < backlog_quiet_ms)) && te_prev && pass_answers >= 64) {
        double c = (double)(t - t_reap) / (double)pass_answers;
        if (c > 2.0 * cost_ns) c = 2.0 * cost_ns; /* a pause inside the cycle is not what answers cost */
        cost_ns += (c - cost_ns) * 0.125;
        if (cost_ns < 8000.0) cost_ns = 8000.0;
        if (cost_ns > 120000.0) cost_ns = 120000.0;
      }
      double n = (double)pass_ns / cost_ns;
      answers_left = n < 128.0 ? 128 : n > 4096.0 ? 4096 : (unsigned)n;
    } else {
      answers_left = answers_per_pass;
    }
    pass_answers = 0;
    unsigned ncq;
    if (fresh_first) {
      /* this pass's arrivals first (see fresh_first). They are late if the loop was away too long:
       * the last pass, plus whatever the enters took beyond the wait that was asked for */
      uint64_t asked = busy ? 0 : (last_cqes || nready ? wait_ns : IDLE_NS), out = t - te_prev;
      uint64_t away = te_prev ? (te_prev - t_reap) + (out > asked ? out - asked : 0) : 0;
      int late = away > (uint64_t)late_ms * 1000000ULL;
      t_reap = t;
      if (late) FS.late_passes++;
      now_ms = (uint32_t)(t / 1000000ULL);
      collecting = 1;
      ncq = reap_all();
      collecting = 0;
      /* the backlog's share, if its oldest request has waited long enough (see backlog_age_ms); a
       * head that is no longer waiting counts as old, so that run_ready clears it away */
      pass_reserve = 0;
      if (nready && fresh_reserve) {
        struct conn *h = ref_conn(ready[0]);
        int gone = h->gen != UD_GEN(ready[0]) || !(h->flags & CF_READY);
        uint32_t age = gone ? 0 : now_ms - h->pad;
        if (!backlog_age_ms || gone || age >= backlog_age_ms) {
          pass_reserve = backlog_age_ms && age >= 2 * backlog_age_ms && answers_left / 2 > fresh_reserve ? answers_left / 2 : fresh_reserve;
          FS.reserved++;
        }
      }
      feed_late = late;
      fresh_run(away);
      feed_late = 1; /* whatever waited a pass or more */
      {
        /* the backlog: what the arrivals left, if none of them is still going there (see
         * backlog_quiet_ms), and its share in any case; what it does not use stays for the reads */
        unsigned for_backlog = backlog_quiet_ms && now_ms - spill_ms < backlog_quiet_ms ? 0 : answers_left;
        if (for_backlog < pass_reserve) for_backlog = pass_reserve;
        unsigned rest = answers_left > for_backlog ? answers_left - for_backlog : 0;
        backlog_parked = nready && !for_backlog;
        answers_left = for_backlog;
        run_ready();
        answers_left += rest;
      }
      feed_late = late;
      ncq += read_rounds();
      feed_late = 1;
      if (writes_ok && nheld) release_held();
      feed_late = 0;
    } else {
      run_ready(); /* oldest first: what earlier batches read, before this batch's arrivals can take the answer slots */
      ncq = reap_all();
      ncq += read_rounds();
      if (writes_ok && nheld) release_held();
    }
    uint64_t tc = now_ns();
    int failed = db_commit(&A.db) < 0;
    uint64_t tc2 = now_ns();
    if (tc2 - tc > S.max_commit_ns) S.max_commit_ns = tc2 - tc;
    flush_replies(failed);
    A.db.wrote = 0;
    db_tick(&A.db); /* no transaction is open: the only point where a hold starts or ends */
    for (unsigned i = 0; i < NR; i++) ring_buf_publish(&R[i].r);
    rearm_ended_polls();
    if (bw_n) brief_expire();
    accept_policy();
    last_cqes = ncq;
    S.batches++;
    S.cqes += ncq;
    if (ncq > S.max_batch) S.max_batch = ncq;
    if (nrq - rq_head > S.rq_max) S.rq_max = nrq - rq_head;
    if (nready > S.ready_max) S.ready_max = nready;
    uint64_t te = now_ns();
    te_prev = te;
    memgov_tick(te);
    if (te - t > S.max_turn_ns) S.max_turn_ns = te - t;
    if (t - last_stats > STATS_NS) { stats(t - last_stats); last_stats = t; }
    if (stop_requested) {
      stats(t - last_stats);
      fprintf(stderr, "stopping\n");
      exit(0);
    }
  }
}
