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
#define BATCH_WAIT 64 /* completions to wait for when busy */
#define BATCH_WAIT_MS 2
#define PEND_MAX (HTTP_MAX_HEAD + HTTP_MAX_BODY)

enum { UD_ACCEPT = 1, UD_RECV = 2, UD_SEND = 3, UD_CLOSE = 4, UD_EVFD = 5 };
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
  CF_REARM = 128,  /* the multishot recv ended; arm it again after the batch */
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
  int lfd;
  int efd;          /* rings 1..N-1: signalled by the kernel when the ring has completions */
  unsigned open;
  uint8_t accept_armed;
  uint8_t need_poll; /* its eventfd fired: enter it to collect completions */
  struct conn **rearm;
  unsigned nrearm, rearm_cap;
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
static unsigned NR;
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

static struct {
  uint64_t batches, cqes, reqs_sent, closes, accepts, rearms, enobufs, short_sends;
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
  sqe_shutdown_close(&rs->r, c->slot, UD(UD_CLOSE, c->ring, c->slot, c->gen));
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

static void arm_recv(struct conn *c) {
  struct rs *rs = conn_rs(c);
  sqe_recv_multishot(ring_sqe(&rs->r), c->slot, UD(UD_RECV, c->ring, c->slot, c->gen));
  c->flags &= ~CF_REARM;
}

static void arm_accept(struct rs *rs, unsigned ri) {
  sqe_accept_multishot(ring_sqe(&rs->r), rs->lfd, UD(UD_ACCEPT, ri, 0, 0));
  rs->accept_armed = 1;
}

static void stash(struct conn *c, const uint8_t *p, unsigned n) {
  if (n == 0) {
    c->pend_len = 0;
    if (c->pend_cap > 65536) { free(c->pend); c->pend = NULL; c->pend_cap = 0; }
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
    unsigned cap = c->pend_cap ? c->pend_cap : 4096;
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
    unsigned cap = c->pend_cap ? c->pend_cap : 4096;
    while (cap < c->pend_len + n) cap *= 2;
    uint8_t *np = realloc(c->pend, cap);
    if (!np) { perror("realloc pend"); exit(1); }
    c->pend = np;
    c->pend_cap = cap;
  }
  memcpy(c->pend + c->pend_len, p, n);
  c->pend_len += n;
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
  if (!(cqe->flags & IORING_CQE_F_MORE)) rs->accept_armed = 0;
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
  arm_recv(c);
}

static void on_recv(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  unsigned slot = UD_SLOT(cqe->user_data);
  struct conn *c = &rs->conns[slot];
  int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
  unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
  if (has_buf) rs->r.bufs_out++;
  int live = c->gen == UD_GEN(cqe->user_data) && (c->flags & CF_OPEN) && !(c->flags & CF_CLOSING);
  if (live && cqe->res > 0) {
    c->last_active = now_s;
    conn_feed(c, ring_buf_ptr(&rs->r, bid), (unsigned)cqe->res);
  }
  if (has_buf) ring_buf_return(&rs->r, bid);
  if (!live) return;
  if (cqe->flags & IORING_CQE_F_MORE) return;
  /* the multishot ended */
  if (cqe->res == -ENOBUFS || cqe->res > 0) {
    if (cqe->res == -ENOBUFS) S.enobufs++;
    c->flags |= CF_REARM;
    if (rs->nrearm == rs->rearm_cap) {
      rs->rearm_cap = rs->rearm_cap ? rs->rearm_cap * 2 : 1024;
      rs->rearm = realloc(rs->rearm, rs->rearm_cap * sizeof *rs->rearm);
      if (!rs->rearm) { perror("realloc"); exit(1); }
    }
    rs->rearm[rs->nrearm++] = c;
    return;
  }
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

static void on_evfd(struct io_uring_cqe *cqe) {
  unsigned ri = UD_RING(cqe->user_data);
  struct rs *rs = &R[ri];
  uint64_t v;
  while (read(rs->efd, &v, sizeof v) > 0) {}
  rs->need_poll = 1;
  if (!(cqe->flags & IORING_CQE_F_MORE)) sqe_poll_multishot(ring_sqe(&R[0].r), rs->efd, UD(UD_EVFD, ri, 0, 0));
}

static unsigned reap(unsigned ri) {
  struct ring *r = &R[ri].r;
  unsigned head = LOAD_RLX(r->cq_head), tail = LOAD_ACQ(r->cq_tail), n = 0;
  while (head != tail) {
    struct io_uring_cqe *cqe = &r->cqes[head & r->cq_mask];
    switch (UD_OP(cqe->user_data)) {
    case UD_RECV: on_recv(ri, cqe); break;
    case UD_SEND: on_send(ri, cqe); break;
    case UD_ACCEPT: on_accept(ri, cqe); break;
    case UD_CLOSE: on_close(ri, cqe); break;
    case UD_EVFD: on_evfd(cqe); break;
    default: break;
    }
    head++;
    n++;
    if ((n & 255) == 0) STORE_REL(r->cq_head, head);
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
  unsigned n = nready;
  nready = 0;
  for (unsigned i = 0; i < n; i++) {
    struct conn *c = ready[i].c;
    if (c->gen != ready[i].gen) continue;
    c->flags &= ~CF_READY;
    if (!(c->flags & CF_OPEN) || (c->flags & (CF_CLOSING | CF_SENDING | CF_HELD))) continue;
    conn_feed(c, NULL, 0);
  }
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

static void stats(uint64_t dt_ns) {
  unsigned open = 0;
  for (unsigned i = 0; i < NR; i++) open += R[i].open;
  T_reqs += S.reqs_sent;
  T_accepts += S.accepts;
  T_closes += S.closes;
  fprintf(stderr, "total: requests=%llu accepts=%llu closes=%llu\n", (unsigned long long)T_reqs,
          (unsigned long long)T_accepts, (unsigned long long)T_closes);
  double dt = (double)dt_ns / 1e9;
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  fprintf(stderr,
          "open=%u req/s=%.0f batches/s=%.0f avg_batch=%.1f max_batch=%llu accepts=%llu closes=%llu "
          "enobufs=%llu rearms=%llu held=%u wal=%d gate=%d ckpt=%llu gates=%llu inline=%llu feed_fallback=%llu errors=%llu "
          "maxrss=%ldMB cpu=%.1fs max_turn=%.1fms max_commit=%.1fms max_sweep=%.1fms\n",
          open, (double)S.reqs_sent / dt, (double)S.batches / dt, S.batches ? (double)S.cqes / (double)S.batches : 0.0,
          (unsigned long long)S.max_batch, (unsigned long long)S.accepts, (unsigned long long)S.closes,
          (unsigned long long)S.enobufs, (unsigned long long)S.rearms, nheld, atomic_load(&A.db.wal_frames), A.db.gate_held,
          (unsigned long long)A.db.n_ckpt, (unsigned long long)A.db.n_gate, (unsigned long long)A.db.n_inline,
          (unsigned long long)A.db.n_fallback, (unsigned long long)A.n_err, ru.ru_maxrss / 1024,
          (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6,
          (double)S.max_turn_ns / 1e6, (double)S.max_commit_ns / 1e6, (double)S.max_sweep_ns / 1e6);
  S.max_turn_ns = S.max_commit_ns = S.max_sweep_ns = 0;
  S.reqs_sent = S.batches = S.cqes = S.max_batch = S.accepts = S.closes = S.enobufs = S.rearms = 0;
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
  NR = (unsigned)((want_conns + nslots - 1) / nslots);
  if (NR < 1) NR = 1;
  if (NR > MAX_RINGS) NR = MAX_RINGS;
  unsigned nbufs = (unsigned)env_long("RECV_BUFS", 2048), bufsz = (unsigned)env_long("RECV_BUF_SIZE", 2048);
  long long mmap_bytes = env_long("MMAP_MB", 0) << 20;
  int cache_kib = (int)env_long("CACHE_KIB", 32768);

  if (app_init(&A, db_path, secret, mmap_bytes, cache_kib) < 0) return 1;

  int backlog = (int)env_long("BACKLOG", 4096);
  for (unsigned i = 0; i < NR; i++) {
    const char *err = "";
    if (ring_init(&R[i].r, 4096, 32768, nslots, nbufs, bufsz, i == 0, &err) < 0) {
      fprintf(stderr, "ring %u: %s: %s\n", i, err, strerror(errno));
      return 1;
    }
    R[i].conns = calloc(nslots, sizeof(struct conn));
    if (!R[i].conns) { perror("calloc"); return 1; }
    madvise(R[i].conns, (size_t)nslots * sizeof(struct conn), MADV_HUGEPAGE);
    R[i].lfd = listen_socket(host, atoi(port), backlog, &err);
    if (R[i].lfd < 0) {
      fprintf(stderr, "listen %s:%s: %s: %s\n", host, port, err, strerror(errno));
      return 1;
    }
    arm_accept(&R[i], i);
    if (i > 0) {
      R[i].efd = ring_register_eventfd(&R[i].r);
      if (R[i].efd < 0) { perror("register eventfd"); return 1; }
      sqe_poll_multishot(ring_sqe(&R[0].r), R[i].efd, UD(UD_EVFD, i, 0, 0));
    }
  }
  fprintf(stderr, "listening on %s:%s: %u rings x %u direct descriptors, %u x %u B receive buffers per ring, "
                  "mmap %lld MB, cache %d KiB\n",
          host, port, NR, nslots, nbufs, bufsz, mmap_bytes >> 20, cache_kib);

  uint64_t t0 = now_ns(), last_sweep = t0, last_stats = t0;
  unsigned last_cqes = 0;
  for (;;) {
    /* 1. publish the other rings' SQEs (sends, re-arms, closes) without waiting */
    for (unsigned i = 1; i < NR; i++)
      if (ring_pending_submit(&R[i].r)) ring_submit(&R[i].r, 0, 0, NULL, 0);

    /* 2. wait on ring 0. The other rings' eventfds are polled from ring 0, so a completion
     * anywhere ends the wait; with DEFER_TASKRUN a ring's completions only appear when it is
     * entered, which happens below for the rings that signalled. */
    unsigned min = last_cqes >= 8 ? BATCH_WAIT : 1;
    unsigned ms = (last_cqes >= 8 || nheld || nready) ? BATCH_WAIT_MS : 1000;
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
    unsigned ncq = reap(0);
    for (unsigned i = 1; i < NR; i++) {
      if (!R[i].need_poll && !ring_pending_submit(&R[i].r)) continue;
      R[i].need_poll = 0;
      ring_poll(&R[i].r);
      ncq += reap(i);
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
        if ((c->flags & (CF_OPEN | CF_CLOSING | CF_REARM)) == (CF_OPEN | CF_REARM)) { arm_recv(c); S.rearms++; }
      }
      rs->nrearm = 0;
      if (!rs->accept_armed && rs->open + 16 < rs->r.nslots) arm_accept(rs, i);
    }
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
    if (t - last_stats > STATS_NS) { stats(t - last_stats); last_stats = t; }
  }
}
