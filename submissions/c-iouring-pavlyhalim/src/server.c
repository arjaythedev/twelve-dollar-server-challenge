/* server.c: one thread, several io_uring rings, one SQLite transaction per batch.
 *
 * A batch is one wake-up of the loop: every completion is handled (requests parsed in place
 * in the kernel's receive buffers, answered into an arena), the batch's transaction commits,
 * and only then are the responses handed to the kernel, one send each, straight from the arena.
 */
#define _GNU_SOURCE
#include "sqlite3.c"

#include "sys.h"
#include "app.h"

#define MAX_RINGS 16
#define ARENA_CAP (1u << 20)
#define ARENA_POOL 64
#define IDLE_SECONDS 90
#define SWEEP_NS 2000000000ULL
#define STATS_NS 10000000000ULL
#define VALVE_NS 200000000ULL
#define BATCH_WAIT 64 /* completions to wait for when busy */
#define BATCH_WAIT_MS 2
#define PEND_MAX (HTTP_MAX_HEAD + HTTP_MAX_BODY)

enum { UD_ACCEPT = 1, UD_RECV = 2, UD_SEND = 3, UD_CLOSE = 4, UD_EVFD = 5, UD_CANCEL = 6, UD_POLL = 7 };
#define UD(op, ring, slot, gen) \
  (((uint64_t)(op) << 56) | ((uint64_t)(ring) << 48) | ((uint64_t)(slot) << 32) | (uint64_t)(gen))
#define UD_OP(u) ((unsigned)((u) >> 56))
#define UD_RING(u) ((unsigned)((u) >> 48) & 0xff)
#define UD_SLOT(u) ((unsigned)((u) >> 32) & 0xffff)
#define UD_GEN(u) ((uint32_t)(u))

enum {
  CF_OPEN = 1,
  CF_SENDING = 2,
  CF_CLOSING = 4,
  CF_CLOSE_AFTER = 8,
  CF_REPLIED = 16, /* a reply is waiting for this batch's commit */
  CF_READY = 32,   /* on the ready list */
  CF_HELD = 64,    /* a write waiting for the WAL gate */
  CF_REARM = 128,  /* the multishot poll ended; arm it again after the batch */
  CF_RQ = 256,     /* on the read queue: readable, a read not yet issued */
  CF_RECVING = 512 /* a read in flight */
};

struct arena {
  struct arena *next;
  uint32_t refs, used, cap;
  uint8_t data[];
};

struct conn {
  uint32_t gen;
  uint16_t flags;
  uint16_t ring;
  uint32_t slot;
  uint32_t last_active;
  uint8_t *pend; /* partial or pipelined input, rare */
  uint32_t pend_len, pend_cap;
  struct arena *send_arena;
};

struct rs {
  struct ring r;
  struct conn *conns;
  int efd;          /* rings 1..N-1: signalled by the kernel when the ring has completions */
  unsigned open;
  uint8_t accept_armed;      /* one bit per listener: its multishot accept is armed on this ring */
  uint8_t accept_cancelling; /* one bit per listener: its cancel is in flight */
  uint8_t populated; /* conns, receive buffers and rearm list faulted in (on the first arm) */
  struct conn **rearm;
  unsigned nrearm, rearm_cap;
  unsigned reads; /* reads in flight: at most one receive buffer each */
};

struct reply {
  struct conn *c;
  uint32_t gen;
  struct arena *a;
  uint32_t off, len;
  uint8_t db, close;
};
struct cref {
  struct conn *c;
  uint32_t gen;
};

static struct rs R[MAX_RINGS];
static unsigned NR;     /* rings created so far; they are created in order, as connections need them */
static unsigned NR_MAX; /* rings the connection target asks for */
static unsigned ring_slots, ring_bufs, ring_buf_size;
static int ring_grow_failed;
static struct app A;
static uint32_t now_s;
static int writes_ok = 1;

static struct arena *pool;
static unsigned pool_n;
static struct arena *cur;
static struct arena *batch_arenas[256];
static unsigned n_batch_arenas;
static struct reply *replies;
static unsigned nrep, rep_cap;
static struct cref *ready, *held;
static unsigned nready, ready_cap, nheld, held_cap;
/* Readable connections waiting for a read, first in first out (rq_head..nrq). Reads are issued at the
 * end of each batch, never more per ring than it has receive buffers, so a burst of any size waits
 * here instead of making the kernel fail reads with ENOBUFS.
 * Drain first: after a burst, reading runs ahead of answering. A read costs 1-2 us; a request waiting
 * in its socket costs a 3-4 KB network buffer, and 170k of them (650 MB) put TCP far past its hard
 * limit (177 MB on 2 GB), where the kernel prunes and drops, and that work on the one vCPU left too
 * little to ever catch up (wall5: 5x the CPU per request until the OOM kill). So up to
 * READS_PER_BATCH reads a turn, but only ANSWERS_PER_BATCH answers: the rest of what is read waits in
 * the connection's pend, a copy the size of the request, on the ready list. */
#define READS_PER_BATCH 16384
#define ANSWERS_PER_BATCH 2048
static struct cref *rq;
static unsigned rq_head, nrq, rq_cap;
static unsigned answers_left; /* this turn's */

static struct {
  uint64_t batches, cqes, reqs_sent, closes, accepts, rearms, enobufs, short_sends, valve_closes, rq_max, cq_overflow;
  uint64_t max_batch, max_turn_ns, max_sweep_ns, max_commit_ns;
} S;

/* ---------- arenas ---------- */

static struct arena *arena_get(void) {
  struct arena *a = pool;
  if (a) {
    pool = a->next;
    pool_n--;
  } else {
    a = malloc(sizeof *a + ARENA_CAP);
    if (!a) { perror("malloc arena"); exit(1); }
    a->cap = ARENA_CAP;
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
    free(a);
  }
}
static uint8_t *arena_reserve(unsigned n) {
  if (!cur || cur->used + n > cur->cap) {
    cur = arena_get();
    cur->refs = 1; /* the batch's own reference */
    if (n_batch_arenas == 256) { fprintf(stderr, "batch too large\n"); exit(1); }
    batch_arenas[n_batch_arenas++] = cur;
  }
  return cur->data + cur->used;
}

/* Startup: the arenas a normal batch uses, touched, in the pool, and the per-batch lists sized for
 * a batch larger than any seen (20,303 completions after a stall), so that neither grows at the
 * moment memory is shortest: right after a stall, when the batch is largest. */
#define ARENA_PREWARM 8
#define LIST_PREWARM 32768
static void prewarm_batch_memory(void) {
  for (unsigned i = 0; i < ARENA_PREWARM; i++) {
    struct arena *a = malloc(sizeof *a + ARENA_CAP);
    if (!a) { perror("malloc arena"); exit(1); }
    memset(a, 0, sizeof *a + ARENA_CAP);
    a->cap = ARENA_CAP;
    a->next = pool;
    pool = a;
    pool_n++;
  }
  rep_cap = ready_cap = held_cap = rq_cap = LIST_PREWARM;
  replies = malloc(rep_cap * sizeof *replies);
  ready = malloc(ready_cap * sizeof *ready);
  held = malloc(held_cap * sizeof *held);
  rq = malloc(rq_cap * sizeof *rq);
  if (!replies || !ready || !held || !rq) { perror("malloc"); exit(1); }
  memset(replies, 0, rep_cap * sizeof *replies);
  memset(ready, 0, ready_cap * sizeof *ready);
  memset(held, 0, held_cap * sizeof *held);
  memset(rq, 0, rq_cap * sizeof *rq);
}

/* ---------- lists ---------- */

static void reply_push(struct conn *c, struct arena *a, uint32_t off, uint32_t len, int db, int close) {
  if (nrep == rep_cap) {
    rep_cap = rep_cap ? rep_cap * 2 : 1024;
    replies = realloc(replies, rep_cap * sizeof *replies);
    if (!replies) { perror("realloc"); exit(1); }
  }
  replies[nrep++] = (struct reply){c, c->gen, a, off, len, (uint8_t)db, (uint8_t)close};
}
static void cref_push(struct cref **l, unsigned *n, unsigned *cap, struct conn *c) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 1024;
    *l = realloc(*l, *cap * sizeof **l);
    if (!*l) { perror("realloc"); exit(1); }
  }
  (*l)[(*n)++] = (struct cref){c, c->gen};
}

/* ---------- connections ---------- */

static inline struct rs *conn_rs(struct conn *c) { return &R[c->ring]; }

static void conn_close(struct conn *c) {
  if (!(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) return;
  c->flags |= CF_CLOSING;
  struct rs *rs = conn_rs(c);
  sqe_shutdown_close(&rs->r, c->slot, UD(UD_CLOSE, c->ring, c->slot, c->gen), UD(UD_POLL, c->ring, c->slot, c->gen),
                     UD(UD_CANCEL, c->ring, c->slot, c->gen));
  S.closes++;
}

static void conn_freed(struct conn *c) {
  c->gen++;
  c->flags = 0;
  if (c->pend) {
    free(c->pend);
    c->pend = NULL;
  }
  c->pend_len = c->pend_cap = 0;
  if (c->send_arena) { /* a send that never completed */
    arena_unref(c->send_arena);
    c->send_arena = NULL;
  }
  conn_rs(c)->open--;
}

static void arm_poll(struct conn *c) {
  struct rs *rs = conn_rs(c);
  sqe_poll_conn(ring_sqe(&rs->r), c->slot, UD(UD_POLL, c->ring, c->slot, c->gen));
  c->flags &= ~CF_REARM;
}

static void queue_read(struct conn *c) {
  if (c->flags & CF_RQ) return;
  c->flags |= CF_RQ;
  cref_push(&rq, &nrq, &rq_cap, c);
}

/* One read per queued connection, up to each ring's free receive buffers; the rest stay queued for
 * the next batch. Returns the reads issued. */
static unsigned issue_reads(void) {
  unsigned budget = READS_PER_BATCH, end = nrq;
  while (rq_head < end && budget) {
    struct cref e = rq[rq_head++];
    struct conn *c = e.c;
    if (c->gen != e.gen || (c->flags & (CF_OPEN | CF_CLOSING)) != CF_OPEN) continue;
    struct rs *rs = conn_rs(c);
    if ((c->flags & CF_RECVING) || rs->reads >= rs->r.br_entries) { /* to the back of the queue */
      cref_push(&rq, &nrq, &rq_cap, c);
      continue;
    }
    c->flags = (uint16_t)((c->flags & ~CF_RQ) | CF_RECVING);
    rs->reads++;
    budget--;
    sqe_recv_once(ring_sqe(&rs->r), c->slot, UD(UD_RECV, c->ring, c->slot, c->gen));
  }
  if (rq_head == nrq) rq_head = nrq = 0;
  else if (rq_head > nrq / 2) { /* keep the array from creeping */
    memmove(rq, rq + rq_head, (nrq - rq_head) * sizeof *rq);
    nrq -= rq_head;
    rq_head = 0;
  }
  S.rq_max = nrq - rq_head > S.rq_max ? nrq - rq_head : S.rq_max;
  return READS_PER_BATCH - budget;
}

/* Accepting. NLISTEN listening sockets share the port (SO_REUSEPORT); the kernel spreads new
 * connections over them, and each has its own accept queue of somaxconn (4,096 on the box), so
 * together they hold ~32k connections through a stall instead of 4k (one listener overflowed 1.8M
 * times in a run with stalls, and every overflow is a SYN the client resends after 1 s, then 3 s;
 * c-iouring-public #158 and Lean #153 run 8). Their multishot accepts are armed only on the ring
 * being filled, and on the next one once the first is within ACCEPT_NEXT slots of full, so
 * connections fill the rings in order and only as many rings are busy as the connection count needs.
 * A ring's accepts are cancelled ACCEPT_STOP slots before its direct-descriptor table is full: an
 * accept that finds the table full makes the kernel reset that client. */
#define ACCEPT_NEXT 8192
#define ACCEPT_STOP 4096 /* the kernel keeps accepting while the loop is busy or stalled: 4096 covers
                          * ~0.8 s at 5,000 new connections a second, for 32k of 523k slots */

#define NLISTEN 8
#define ALL_LISTENERS ((1u << NLISTEN) - 1)
static int LFD[NLISTEN];

/* A ring's connection table and receive buffers (6.6 MB) are faulted in when the ring first takes
 * connections, ACCEPT_NEXT connections before it is needed, so rings that never fill cost nothing. */
static void ring_populate(struct rs *rs) {
  uint64_t t = now_ns();
  size_t nc = (size_t)rs->r.nslots * sizeof(struct conn), nb = (size_t)rs->r.br_entries * rs->r.buf_size;
  prefault(rs->conns, nc);
  prefault(rs->r.bufs, nb);
  rs->rearm_cap = 4096;
  rs->rearm = malloc(rs->rearm_cap * sizeof *rs->rearm);
  if (!rs->rearm) { perror("malloc"); exit(1); }
  memset(rs->rearm, 0, rs->rearm_cap * sizeof *rs->rearm);
  rs->populated = 1;
  fprintf(stderr, "ring %u: %zu KiB faulted in (%.1f ms)\n", (unsigned)(rs - R), (nc + nb) >> 10, (double)(now_ns() - t) / 1e6);
}

/* A ring is made when connections first need it (ring 0 at startup, each later one ACCEPT_NEXT
 * connections before the one before it is full), so rings that are never used cost no kernel
 * memory. Newer kernels than the box's 6.8 charge ring memory to RLIMIT_MEMLOCK (8 MB under
 * systemd): made up front, eight rings with 65,536-entry completion queues did not fit, and CI's
 * runner refused the seventh. The CQ holds one completion per connection on the ring, so a burst
 * of completions never goes to the kernel's overflow list, allocated per completion just when
 * memory is shortest; on ENOMEM from io_uring_setup (nothing allocated yet) the ring is made with
 * half the entries, down to 8,192. */
static int ring_create(unsigned i) {
  const char *err = "";
  unsigned cq = 65536;
  while (ring_init(&R[i].r, 4096, cq, ring_slots, ring_bufs, ring_buf_size, i == 0, &err) < 0) {
    if (errno != ENOMEM || strcmp(err, "io_uring_setup") || cq <= 8192) {
      fprintf(stderr, "ring %u: %s: %s\n", i, err, strerror(errno));
      return -1;
    }
    cq /= 2;
  }
  R[i].conns = mmap(NULL, (size_t)ring_slots * sizeof(struct conn), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (R[i].conns == MAP_FAILED) { perror("mmap conns"); return -1; }
  if (i > 0) {
    R[i].efd = ring_register_eventfd(&R[i].r);
    if (R[i].efd < 0) { perror("register eventfd"); return -1; }
    sqe_poll_multishot(ring_sqe(&R[0].r), R[i].efd, UD(UD_EVFD, i, 0, 0));
  }
  fprintf(stderr, "ring %u: io_uring fd %d, %u CQ entries\n", i, R[i].r.fd, cq);
  return 0;
}

static void arm_accept(struct rs *rs, unsigned ri) {
  if (!rs->populated) ring_populate(rs);
  for (unsigned l = 0; l < NLISTEN; l++)
    if (!(rs->accept_armed & (1u << l))) {
      sqe_accept_multishot(ring_sqe(&rs->r), LFD[l], UD(UD_ACCEPT, ri, l, 0));
      rs->accept_armed |= (uint8_t)(1u << l);
    }
}

static void accept_policy(void) {
  if (NR < NR_MAX && !ring_grow_failed && R[NR - 1].open + ACCEPT_NEXT >= R[NR - 1].r.nslots) {
    if (ring_create(NR) == 0) NR++;
    else ring_grow_failed = 1; /* stay at NR rings; the last one's accepts stop ACCEPT_STOP short of full */
  }
  unsigned lowest = NR;
  for (unsigned i = 0; i < NR; i++)
    if (R[i].open + ACCEPT_STOP < R[i].r.nslots) { lowest = i; break; }
  for (unsigned i = 0; i < NR; i++) {
    struct rs *rs = &R[i];
    int room = rs->open + ACCEPT_STOP < rs->r.nslots;
    int want = room && (i == lowest || (i == lowest + 1 && R[lowest].open + ACCEPT_NEXT >= R[lowest].r.nslots));
    if (want && rs->accept_armed != ALL_LISTENERS) {
      arm_accept(rs, i);
    } else if (!room) {
      for (unsigned l = 0; l < NLISTEN; l++)
        if ((rs->accept_armed & ~rs->accept_cancelling) & (1u << l)) {
          sqe_cancel(ring_sqe(&rs->r), UD(UD_ACCEPT, i, l, 0), UD(UD_CANCEL, i, 0, 0));
          rs->accept_cancelling |= (uint8_t)(1u << l);
        }
    }
  }
}

static void stash(struct conn *c, const uint8_t *p, unsigned n) {
  if (n == 0) { /* pend is the rare path (a burst, a split request): give it back once used */
    c->pend_len = 0;
    free(c->pend);
    c->pend = NULL;
    c->pend_cap = 0;
    return;
  }
  if (p == c->pend) {
    c->pend_len = n;
    return;
  }
  if (p >= c->pend && c->pend && p < c->pend + c->pend_cap) { /* a tail of pend */
    memmove(c->pend, p, n);
    c->pend_len = n;
    return;
  }
  if (c->pend_cap < n) {
    unsigned cap = c->pend_cap ? c->pend_cap : 512;
    while (cap < n) cap *= 2;
    uint8_t *np = realloc(c->pend, cap);
    if (!np) { perror("realloc pend"); exit(1); }
    c->pend = np;
    c->pend_cap = cap;
  }
  memcpy(c->pend, p, n);
  c->pend_len = n;
}

static void pend_append(struct conn *c, const uint8_t *p, unsigned n) {
  if (c->pend_len + n > c->pend_cap) {
    unsigned cap = c->pend_cap ? c->pend_cap : 512; /* a request is 150-600 bytes */
    while (cap < c->pend_len + n) cap *= 2;
    uint8_t *np = realloc(c->pend, cap);
    if (!np) { perror("realloc pend"); exit(1); }
    c->pend = np;
    c->pend_cap = cap;
  }
  memcpy(c->pend + c->pend_len, p, n);
  c->pend_len += n;
}

/* The memory valve. Past ~300k users the box runs out of memory and the kernel kills the server:
 * each connection costs ~4 KB of kernel memory, almost all of it the socket itself, which no
 * setting shrinks. When the memory the kernel could still hand out runs low, a share of responses
 * carries "Connection: close". The client reads its answer and dials again for
 * its next request, after its think time, so no request fails, and for those seconds the socket does
 * not exist. The share follows the headroom: none above VALVE_HI_MB, every response at VALVE_LO_MB.
 * Headroom is MemAvailable minus SReclaimable, as c-iouring-public (#158) measured on the box: the
 * sockets' own dentries and inodes are counted as reclaimable but are pinned while the socket is
 * open. Free memory plus page cache read 160-180 MB when a VM froze for lack of memory.
 * It rises at once and falls by at most VALVE_RELEASE a second: memory comes back seconds after the
 * closes, and dropping straight to zero would let it run out again (it swung 45% -> 0% that way).
 * Sampled every 200 ms; while the headroom falls the share follows where it will be in VALVE_AHEAD_S
 * at the current rate (near the wall it went from 400 MB to the OOM kill in seconds).
 * The cost is a TCP handshake per closed response, and while the old socket finishes closing the
 * client's new one is already open, so near the wall a wave of closes adds sockets before it removes
 * any. With 1,000 MB locked in the test VM, the valve on closed 17k connections in 10 s and the
 * server was OOM-killed at ~59k; the valve off held to ~144k. c-iouring-public (#158) found the same
 * (reconnects cost more than they saved), so it is off unless VALVE=1. */
#define VALVE_HI_MB 400
#define VALVE_LO_MB 200
#define VALVE_RELEASE (UINT32_MAX / 200) /* per 200 ms sample: 2.5 points a second */
#define VALVE_AHEAD_S 2.0
static int meminfo_fd = -1;
static int valve_on; /* VALVE=1 turns it on (headroom is measured and logged either way) */
static uint32_t valve_p;   /* probability that a response closes, in 1/2^32 */
static uint32_t valve_rng = 2463534242u;
static long headroom_mb = -1;
static double headroom_slope; /* MB/s, smoothed */

static long meminfo_kb(const char *buf, const char *key) {
  const char *p = strstr(buf, key);
  return p ? atol(p + strlen(key)) : 0;
}

static void valve_update(void) {
  char buf[4096];
  ssize_t n = meminfo_fd < 0 ? -1 : pread(meminfo_fd, buf, sizeof buf - 1, 0);
  if (n <= 0) return;
  buf[n] = 0;
  long prev = headroom_mb;
  headroom_mb = (meminfo_kb(buf, "MemAvailable:") - meminfo_kb(buf, "SReclaimable:")) >> 10;
  if (prev >= 0) headroom_slope = 0.7 * headroom_slope + 0.3 * (double)(headroom_mb - prev) * (1e9 / VALVE_NS);
  double h = (double)headroom_mb;
  if (headroom_slope < 0) h += headroom_slope * VALVE_AHEAD_S;
  uint32_t want;
  if (!valve_on || h >= VALVE_HI_MB) want = 0;
  else if (h <= VALVE_LO_MB) want = UINT32_MAX;
  else want = (uint32_t)((VALVE_HI_MB - h) / (VALVE_HI_MB - VALVE_LO_MB) * UINT32_MAX);
  if (want >= valve_p) valve_p = want;
  else valve_p = valve_p - want > VALVE_RELEASE ? valve_p - VALVE_RELEASE : want;
}

static inline int valve_close(void) {
  if (!valve_p) return 0;
  valve_rng ^= valve_rng << 13; /* xorshift32 */
  valve_rng ^= valve_rng >> 17;
  valve_rng ^= valve_rng << 5;
  return valve_rng < valve_p;
}

/* Feeds bytes to a connection: parses and answers at most one request per batch, keeps the
 * rest for later. `data` may be NULL to work on what is already buffered. */
static void conn_feed(struct conn *c, const uint8_t *data, unsigned len) {
  const uint8_t *buf;
  unsigned n;
  if (c->pend_len) {
    if (len) pend_append(c, data, len);
    buf = c->pend;
    n = c->pend_len;
  } else {
    buf = data;
    n = len;
  }
  while (n) {
    if (c->flags & (CF_SENDING | CF_REPLIED | CF_CLOSING | CF_HELD)) break;
    struct req rq;
    int rc = http_parse(buf, n, &rq);
    if (rc == 0) {
      if (n > PEND_MAX) { conn_close(c); n = 0; }
      break;
    }
    if (rc < 0) {
      uint8_t *free = arena_reserve(L_400_BAD);
      memcpy(free, R_400_BAD, L_400_BAD);
      cur->used += L_400_BAD;
      reply_push(c, cur, (uint32_t)(free - cur->data), L_400_BAD, 0, 1);
      c->flags |= CF_REPLIED | CF_CLOSE_AFTER;
      n = 0;
      break;
    }
    /* the valve closes only after the last buffered request: a pipelined one is never cut off */
    if (rq.keep_alive && rq.total_len == n && valve_close()) { rq.keep_alive = 0; S.valve_closes++; }
    uint8_t *free = arena_reserve(RESP_MAX);
    struct resp rp;
    app_handle(&A, &rq, free, writes_ok, &rp);
    if (rp.held) {
      c->flags |= CF_HELD;
      cref_push(&held, &nheld, &held_cap, c);
      break;
    }
    cur->used += (uint32_t)((rp.p + rp.n) - free);
    reply_push(c, cur, (uint32_t)(rp.p - cur->data), rp.n, rp.db, rp.close);
    c->flags |= CF_REPLIED;
    if (rp.close) c->flags |= CF_CLOSE_AFTER;
    buf += rq.total_len;
    n -= rq.total_len;
  }
  stash(c, buf, n);
}

/* ---------- completions ---------- */

static void on_accept(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  if (!(cqe->flags & IORING_CQE_F_MORE)) { /* this listener's accept on this ring has ended */
    uint8_t bit = (uint8_t)(1u << UD_SLOT(cqe->user_data));
    rs->accept_armed &= (uint8_t)~bit;
    rs->accept_cancelling &= (uint8_t)~bit;
  }
  if (cqe->res < 0) {
    if (cqe->res != -ENFILE && cqe->res != -EMFILE && cqe->res != -ECANCELED)
      fprintf(stderr, "accept ring %u: %s\n", ri, strerror(-cqe->res));
    return;
  }
  unsigned slot = (unsigned)cqe->res;
  if (slot >= rs->r.nslots) { fprintf(stderr, "accept: slot %u out of range\n", slot); return; }
  struct conn *c = &rs->conns[slot];
  if (c->flags & CF_OPEN) { fprintf(stderr, "accept: slot %u still open\n", slot); return; }
  c->flags = CF_OPEN;
  c->ring = (uint16_t)ri;
  c->slot = slot;
  c->last_active = now_s;
  c->pend_len = 0;
  c->send_arena = NULL;
  rs->open++;
  S.accepts++;
  arm_poll(c);
}

static void rearm_later(struct rs *rs, struct conn *c) {
  c->flags |= CF_REARM;
  if (rs->nrearm == rs->rearm_cap) {
    rs->rearm_cap = rs->rearm_cap ? rs->rearm_cap * 2 : 1024;
    rs->rearm = realloc(rs->rearm, rs->rearm_cap * sizeof *rs->rearm);
    if (!rs->rearm) { perror("realloc"); exit(1); }
  }
  rs->rearm[rs->nrearm++] = c;
}

static void on_poll(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  struct conn *c = &rs->conns[UD_SLOT(cqe->user_data)];
  if (c->gen != UD_GEN(cqe->user_data) || (c->flags & (CF_OPEN | CF_CLOSING)) != CF_OPEN) return;
  if (cqe->res < 0) { conn_close(c); return; }
  queue_read(c); /* readable, or the peer hung up: the read tells which */
  if (!(cqe->flags & IORING_CQE_F_MORE)) rearm_later(rs, c);
}

static void on_recv(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  unsigned slot = UD_SLOT(cqe->user_data);
  struct conn *c = &rs->conns[slot];
  int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
  unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
  if (has_buf) rs->r.bufs_out++;
  rs->reads--;
  int live = c->gen == UD_GEN(cqe->user_data) && (c->flags & CF_OPEN) && !(c->flags & CF_CLOSING);
  if (live) c->flags &= ~CF_RECVING;
  if (live && cqe->res > 0) {
    c->last_active = now_s;
    if (answers_left) {
      answers_left--;
      conn_feed(c, ring_buf_ptr(&rs->r, bid), (unsigned)cqe->res);
    } else { /* this turn's answers are used up: keep the bytes, answer in a later turn */
      pend_append(c, ring_buf_ptr(&rs->r, bid), (unsigned)cqe->res);
      if (!(c->flags & CF_READY)) {
        c->flags |= CF_READY;
        cref_push(&ready, &nready, &ready_cap, c);
      }
    }
  }
  if (has_buf) ring_buf_return(&rs->r, bid);
  if (!live) return;
  if (cqe->res > 0) {
    if ((unsigned)cqe->res == rs->r.buf_size) queue_read(c); /* the buffer filled: there may be more */
    return;
  }
  if (cqe->res == -EAGAIN) return; /* already read; the poll reports new data */
  if (cqe->res == -ENOBUFS) { S.enobufs++; queue_read(c); return; }
  conn_close(c); /* EOF or error */
}

static void on_send(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  struct conn *c = &rs->conns[UD_SLOT(cqe->user_data)];
  if (c->gen != UD_GEN(cqe->user_data)) return;
  c->flags &= ~CF_SENDING;
  if (c->send_arena) {
    arena_unref(c->send_arena);
    c->send_arena = NULL;
  }
  if (cqe->res < 0) { conn_close(c); return; }
  if (c->flags & CF_CLOSE_AFTER) { conn_close(c); return; }
  if (c->pend_len && !(c->flags & (CF_READY | CF_HELD | CF_CLOSING))) {
    c->flags |= CF_READY;
    cref_push(&ready, &nready, &ready_cap, c);
  }
}

static void on_close(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  struct conn *c = &rs->conns[UD_SLOT(cqe->user_data)];
  if (c->gen != UD_GEN(cqe->user_data)) return;
  if (!(c->flags & CF_CLOSING)) return; /* a failed shutdown's CQE; the close still follows */
  if (cqe->res < 0 && cqe->res != -EBADF) {
    /* only the close op carries CLOSING to completion; a shutdown error is ignored */
  }
  if (cqe->res == -ENOTCONN || cqe->res == -EPIPE || cqe->res == -ENOTSOCK) return; /* the shutdown's error */
  conn_freed(c);
}

/* A ring's eventfd fired. Its only job is to end ring 0's wait: the ring's completions are read
 * every batch anyway. The counter is not drained: a multishot poll posts one completion per
 * wake-up of the eventfd, not per level, so reading it back would be a system call for nothing
 * (and the 64-bit counter cannot overflow at a few thousand signals a second). */
static void on_evfd(struct io_uring_cqe *cqe) {
  unsigned ri = UD_RING(cqe->user_data);
  struct rs *rs = &R[ri];
  if (!(cqe->flags & IORING_CQE_F_MORE)) sqe_poll_multishot(ring_sqe(&R[0].r), rs->efd, UD(UD_EVFD, ri, 0, 0));
}

static unsigned reap(unsigned ri) {
  struct ring *r = &R[ri].r;
  if (LOAD_RLX(r->sq_flags) & IORING_SQ_CQ_OVERFLOW) S.cq_overflow++; /* completions waiting in the kernel */
  unsigned head = LOAD_RLX(r->cq_head), tail = LOAD_ACQ(r->cq_tail), n = 0;
  while (head != tail) {
    struct io_uring_cqe *cqe = &r->cqes[head & r->cq_mask];
    switch (UD_OP(cqe->user_data)) {
    case UD_RECV: on_recv(ri, cqe); break;
    case UD_POLL: on_poll(ri, cqe); break;
    case UD_SEND: on_send(ri, cqe); break;
    case UD_ACCEPT: on_accept(ri, cqe); break;
    case UD_CLOSE: on_close(ri, cqe); break;
    case UD_EVFD: on_evfd(cqe); break;
    default: break;
    }
    head++;
    n++;
    if ((n & 255) == 0) {
      /* hand back the CQ slots and the receive buffers already consumed now, not at the end of the
       * batch: a burst bigger than the buffer ring makes the kernel end multishot receives with
       * ENOBUFS, every one of them then needs re-arming, and the longer turn lets a bigger burst in */
      STORE_REL(r->cq_head, head);
      ring_buf_publish(r);
    }
  }
  STORE_REL(r->cq_head, head);
  return n;
}

/* ---------- the batch ---------- */

static void flush_replies(int commit_failed) {
  for (unsigned i = 0; i < nrep; i++) {
    struct reply *rp = &replies[i];
    struct conn *c = rp->c;
    if (c->gen != rp->gen) continue;
    c->flags &= ~CF_REPLIED;
    if (!(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
    struct arena *a = rp->a;
    uint8_t *p = a->data + rp->off;
    unsigned len = rp->len;
    if (commit_failed && rp->db) {
      p = arena_reserve(L_500);
      memcpy(p, R_500, L_500);
      cur->used += L_500;
      a = cur;
      len = L_500;
    }
    struct rs *rs = conn_rs(c);
    sqe_send(ring_sqe(&rs->r), c->slot, p, len, UD(UD_SEND, c->ring, c->slot, c->gen));
    a->refs++;
    c->send_arena = a;
    c->flags |= CF_SENDING;
    S.reqs_sent++;
  }
  nrep = 0;
  for (unsigned i = 0; i < n_batch_arenas; i++) arena_unref(batch_arenas[i]);
  n_batch_arenas = 0;
  cur = NULL;
}

static void run_lists(void) {
  /* oldest first, up to this turn's answers; the rest move to the front for the next turn (entries
   * pushed while this runs land behind them) */
  unsigned n = nready, i = 0;
  for (; i < n && answers_left; i++) {
    struct conn *c = ready[i].c;
    if (c->gen != ready[i].gen) continue;
    c->flags &= ~CF_READY;
    if (!(c->flags & CF_OPEN) || (c->flags & (CF_CLOSING | CF_SENDING | CF_HELD))) continue;
    answers_left--;
    conn_feed(c, NULL, 0);
  }
  memmove(ready, ready + i, (nready - i) * sizeof *ready);
  nready -= i;
  if (writes_ok && nheld) {
    n = nheld;
    nheld = 0;
    for (unsigned i = 0; i < n; i++) {
      struct conn *c = held[i].c;
      if (c->gen != held[i].gen) continue;
      c->flags &= ~CF_HELD;
      if (!(c->flags & CF_OPEN) || (c->flags & CF_CLOSING)) continue;
      conn_feed(c, NULL, 0);
    }
  }
}

static void sweep(void) {
  for (unsigned ri = 0; ri < NR; ri++) {
    struct rs *rs = &R[ri];
    if (!rs->open) continue;
    for (unsigned i = 0; i < rs->r.nslots; i++) {
      struct conn *c = &rs->conns[i];
      if ((c->flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN && now_s - c->last_active > IDLE_SECONDS) conn_close(c);
    }
  }
}

static uint64_t T_reqs, T_accepts, T_closes;

/* the event loop's own CPU time (cpu= is the whole process, the checkpoint thread included) */
static double thread_cpu_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void stats(uint64_t dt_ns) {
  unsigned open = 0;
  for (unsigned i = 0; i < NR; i++) open += R[i].open;
  fprintf(stderr, "rings: open=");
  for (unsigned i = 0; i < NR; i++) fprintf(stderr, "%s%u%s", i ? "," : "", R[i].open, R[i].accept_armed ? "*" : "");
  fprintf(stderr, "\n");
  T_reqs += S.reqs_sent;
  T_accepts += S.accepts;
  T_closes += S.closes;
  fprintf(stderr, "total: requests=%llu accepts=%llu closes=%llu\n", (unsigned long long)T_reqs,
          (unsigned long long)T_accepts, (unsigned long long)T_closes);
  /* SQLite's own page-cache counters for the request connection, reset each window */
  int c_hit = 0, c_miss = 0, c_write = 0, c_used = 0, hw;
  sqlite3_db_status(A.db.c, SQLITE_DBSTATUS_CACHE_HIT, &c_hit, &hw, 1);
  sqlite3_db_status(A.db.c, SQLITE_DBSTATUS_CACHE_MISS, &c_miss, &hw, 1);
  sqlite3_db_status(A.db.c, SQLITE_DBSTATUS_CACHE_WRITE, &c_write, &hw, 1);
  sqlite3_db_status(A.db.c, SQLITE_DBSTATUS_CACHE_USED, &c_used, &hw, 0);
  fprintf(stderr, "sqlite: cache_hit=%d cache_miss=%d cache_write=%d cache_used=%dKB truncates=%llu skipped=%llu wal_frames=%llu wal_writes=%llu\n",
          c_hit, c_miss, c_write, c_used / 1024, (unsigned long long)pcache_truncates,
          (unsigned long long)pcache_truncates_skipped, (unsigned long long)wal_frames, (unsigned long long)wal_writes);
  double dt = (double)dt_ns / 1e9;
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  fprintf(stderr,
          "open=%u req/s=%.0f batches/s=%.0f avg_batch=%.1f max_batch=%llu accepts=%llu closes=%llu "
          "enobufs=%llu rearms=%llu held=%u wal=%d gate=%d ckpt=%llu gates=%llu inline=%llu feed_fallback=%llu errors=%llu "
          "maxrss=%ldMB cpu=%.1fs max_turn=%.1fms max_commit=%.1fms max_sweep=%.1fms headroom=%ldMB valve=%.1f%% valve_closes=%llu "
          "rq_max=%llu cq_overflow=%llu loop_cpu=%.1fs\n",
          open, (double)S.reqs_sent / dt, (double)S.batches / dt, S.batches ? (double)S.cqes / (double)S.batches : 0.0,
          (unsigned long long)S.max_batch, (unsigned long long)S.accepts, (unsigned long long)S.closes,
          (unsigned long long)S.enobufs, (unsigned long long)S.rearms, nheld, atomic_load(&A.db.wal_frames), A.db.gate_held,
          (unsigned long long)A.db.n_ckpt, (unsigned long long)A.db.n_gate, (unsigned long long)A.db.n_inline,
          (unsigned long long)A.db.n_fallback, (unsigned long long)A.n_err, ru.ru_maxrss / 1024,
          (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6,
          (double)S.max_turn_ns / 1e6, (double)S.max_commit_ns / 1e6, (double)S.max_sweep_ns / 1e6, headroom_mb,
          100.0 * valve_p / UINT32_MAX, (unsigned long long)S.valve_closes, (unsigned long long)S.rq_max,
          (unsigned long long)S.cq_overflow, thread_cpu_s());
  S.max_turn_ns = S.max_commit_ns = S.max_sweep_ns = 0;
  S.reqs_sent = S.batches = S.cqes = S.max_batch = S.accepts = S.closes = S.enobufs = S.rearms = S.valve_closes = S.rq_max = S.cq_overflow = 0;
}

static long env_long(const char *name, long def) {
  const char *v = getenv(name);
  return v && *v ? atol(v) : def;
}

/* Build time only: a synthetic database for PGO training (random data, none of the seed). */
static int make_train_db(const char *path) {
  long users = env_long("TRAIN_USERS", 5000), posts = env_long("TRAIN_POSTS", 40000), likes = env_long("TRAIN_LIKES", 160000);
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
  sqlite3_prepare_v2(c, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1, &su, NULL);
  sqlite3_prepare_v2(c, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)", -1, &sp, NULL);
  sqlite3_prepare_v2(c, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)", -1, &sl, NULL);
  srand(4242);
  char buf[600];
  for (long i = 1; i <= users; i++) {
    snprintf(buf, sizeof buf, "user_%ld", i);
    sqlite3_bind_int64(su, 1, i);
    sqlite3_bind_text(su, 2, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(su);
    sqlite3_reset(su);
  }
  static const char *words[] = {"server", "latency", "cheap", "sqlite", "index", "cache", "keepalive", "droplet", "batch", "kernel"};
  for (long i = 1; i <= posts; i++) {
    int n = 0, nw = 5 + rand() % 30;
    for (int w = 0; w < nw && n < 480; w++) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s", w ? " " : "", words[rand() % 10]);
    long t = 1704067200L + i * 60;
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", gmtime(&t));
    char created[40];
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

/* exit() rather than the default action, so the listeners close at once and, in the
 * instrumented build, the profile is written */
static void on_term(int sig) {
  (void)sig;
  exit(0);
}

/* Keeps the server's code and data in RAM. They are file-backed, so when memory runs short the
 * kernel evicts them like any cached file (with no swap, file pages are all reclaim can take, and
 * Ubuntu's multi-generational LRU gives code pages no extra pass), and the event loop then stalls
 * on major faults that read its own code back from disk while every connection's next request
 * waits in the kernel. The binary is static, so its own mappings are all of it; locking the
 * writable one turns its untouched pages into private memory. Best effort: if RLIMIT_MEMLOCK
 * (8 MB under systemd) is too small, the server runs unlocked. Returns bytes locked. */
static size_t lock_code(void) {
  char exe[512];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  FILE *f = fopen("/proc/self/maps", "r");
  if (n <= 0 || !f) {
    if (f) fclose(f);
    return 0;
  }
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
    else fprintf(stderr, "mlock %s: %s\n", perms, strerror(errno));
  }
  fclose(f);
  return locked;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  setvbuf(stderr, NULL, _IOLBF, 0);
  if (getenv("MAKE_TRAIN_DB")) return make_train_db(getenv("MAKE_TRAIN_DB"));
  const char *host = getenv("HOST"), *port = getenv("PORT"), *db_path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3000";
  if (!db_path || !*db_path || !secret || !*secret) {
    fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
    return 1;
  }

  /* file descriptors: raise the soft limit; each ring's direct table may hold at most that many */
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
  }
  unsigned nslots = rl.rlim_cur > 65535 ? 65535 : (unsigned)rl.rlim_cur;
  if (nslots > 256) nslots -= 64; /* leave room for the process's ordinary descriptors */
  long want_conns = env_long("MAX_CONNS", 500000);
  NR_MAX = (unsigned)((want_conns + nslots - 1) / nslots);
  if (NR_MAX < 1) NR_MAX = 1;
  if (NR_MAX > MAX_RINGS) NR_MAX = MAX_RINGS;
  /* 4,096 buffers of 1 KB per ring: a request is 150-600 bytes, and the count, not the size, is what
   * a burst runs out of (a larger request spans buffers and goes through pend) */
  unsigned nbufs = (unsigned)env_long("RECV_BUFS", 4096), bufsz = (unsigned)env_long("RECV_BUF_SIZE", 1024);
  long long mmap_bytes = env_long("MMAP_MB", 0) << 20;
  /* 8 MB of SQLite page cache: memory the kernel can't reclaim, so every MB is ~240 sockets at the
   * wall. Misses are 0.053 a request at 8 MB (0.0017 at 32 MB), each a read from the page cache */
  int cache_kib = (int)env_long("CACHE_KIB", 8192);

  if (app_init(&A, db_path, secret, mmap_bytes, cache_kib) < 0) return 1;

  int backlog = (int)env_long("BACKLOG", 4096);
  {
    const char *err = "";
    for (unsigned l = 0; l < NLISTEN; l++) {
      LFD[l] = listen_socket(host, atoi(port), backlog, &err);
      if (LFD[l] < 0) {
        fprintf(stderr, "listen %s:%s: %s: %s\n", host, port, err, strerror(errno));
        return 1;
      }
    }
  }
  ring_slots = nslots;
  ring_bufs = nbufs;
  ring_buf_size = bufsz;
  if (ring_create(0) < 0) return 1;
  NR = 1;
  fprintf(stderr, "listening on %s:%s: up to %u rings x %u direct descriptors, %u x %u B receive buffers per ring, "
                  "mmap %lld MB, cache %d KiB\n",
          host, port, NR_MAX, nslots, nbufs, bufsz, mmap_bytes >> 20, cache_kib);
  fprintf(stderr, "code locked in RAM: %zu KiB\n", lock_code() >> 10);
  prewarm_batch_memory();
  prefault_stack(256 << 10);
  accept_policy();

  meminfo_fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
  valve_on = env_long("VALVE", 0) != 0;
  valve_update();
  uint64_t t0 = now_ns(), last_sweep = t0, last_stats = t0, last_valve = t0;
  unsigned last_cqes = 0;
  for (;;) {
    /* 1. publish the other rings' SQEs (sends, re-arms, closes) without waiting */
    for (unsigned i = 1; i < NR; i++)
      if (ring_pending_submit(&R[i].r)) ring_submit(&R[i].r, 0, 0, NULL, 0);

    /* 2. wait on ring 0. The other rings' eventfds are polled from ring 0, so a completion
     * anywhere ends the wait. */
    unsigned min = last_cqes >= 8 ? BATCH_WAIT : 1;
    unsigned ms = (last_cqes >= 8 || nheld || nready || nrq > rq_head) ? BATCH_WAIT_MS : 1000;
    long rc = ring_wait(&R[0].r, min, ms);
    if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EBUSY) {
      fprintf(stderr, "io_uring_enter: %s\n", strerror((int)-rc));
      return 1;
    }

    /* 3. the batch */
    uint64_t t = now_ns();
    now_s = (uint32_t)(t / 1000000000ULL);
    A.now = (int64_t)time(NULL);
    writes_ok = db_writes_allowed(&A.db);
    /* Rings 1..N do not defer their task work, so the kernel has already posted their
     * completions into their queues while this thread was in the wait above: they are read
     * straight from shared memory, without entering those rings (that was 4.7 system calls
     * per batch with all 8 rings busy). */
    answers_left = ANSWERS_PER_BATCH;
    unsigned ncq = reap(0);
    for (unsigned i = 1; i < NR; i++) ncq += reap(i);
    /* Reads for the connections the polls just reported, issued and completed now: a MSG_DONTWAIT
     * read finishes during the submit, so its request is answered in the batch that saw it ready.
     * Issued at the end of the batch instead, it waited for the next one, whose wait for 64
     * completions then held it up to 2 ms more (p95 4.7 against 2.7 ms at 20k users). */
    if (issue_reads()) {
      for (unsigned i = 0; i < NR; i++)
        if (ring_pending_submit(&R[i].r)) ring_submit(&R[i].r, 0, 0, NULL, 0);
      for (unsigned i = 0; i < NR; i++) ncq += reap(i);
    }
    run_lists();
    uint64_t tc = now_ns();
    int failed = db_commit(&A.db) < 0;
    uint64_t tc2 = now_ns();
    if (tc2 - tc > S.max_commit_ns) S.max_commit_ns = tc2 - tc;
    flush_replies(failed);
    for (unsigned i = 0; i < NR; i++) {
      struct rs *rs = &R[i];
      ring_buf_publish(&rs->r);
      for (unsigned k = 0; k < rs->nrearm; k++) {
        struct conn *c = rs->rearm[k];
        if ((c->flags & (CF_OPEN | CF_CLOSING | CF_REARM)) == (CF_OPEN | CF_REARM)) { arm_poll(c); S.rearms++; }
      }
      rs->nrearm = 0;
    }
    accept_policy();
    last_cqes = ncq;
    S.batches++;
    S.cqes += ncq;
    if (ncq > S.max_batch) S.max_batch = ncq;

    uint64_t te = now_ns();
    if (te - t > S.max_turn_ns) S.max_turn_ns = te - t;

    /* 4. housekeeping */
    if (t - last_sweep > SWEEP_NS) {
      sweep();
      last_sweep = t;
      uint64_t ts = now_ns() - te;
      if (ts > S.max_sweep_ns) S.max_sweep_ns = ts;
    }
    if (t - last_valve > VALVE_NS) { valve_update(); last_valve = t; }
    if (t - last_stats > STATS_NS) { stats(t - last_stats); last_stats = t; }
  }
}
