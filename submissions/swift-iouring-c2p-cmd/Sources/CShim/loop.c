/* One thread, io_uring: accepts, multishot receives into a provided-buffer ring, group-committed
 * SQLite batches, sends. HTTP parsing and every handler live in Swift (one callback, lp_handler);
 * this file only moves bytes and decides when a batch commits.
 *
 * A pass: wait for WAIT_MIN completions or WAIT_NS, drain them (requests are answered as they
 * are read), commit the SQLite transaction the answers shared, then send every response. Nothing
 * from a pass is sent before its COMMIT returns.
 *
 * A ring's table of direct descriptors is as large as RLIMIT_NOFILE. When it is nearly full,
 * accepting moves to the next ring with room, adding one if needed (up to MAX_RINGS), so the
 * connection count is not capped by that limit. One thread drives every ring. */
#define _GNU_SOURCE
#include "shim.h"
#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef unsigned char u8;
typedef uint64_t u64;

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_SHUTDOWN, OP_CLOSE, OP_READ };

/* Single-shot accepts kept in flight, spread over the listeners. Each is re-armed only after a
 * pass sees it complete, so this bounds the accept rate: a ramp of thousands of connections a
 * second needs hundreds in flight, or the listen queues fill and clients wait out SYN retries. */
#define ACCEPT_DEPTH 512
/* SO_REUSEPORT listeners on the one port: each has its own accept queue (somaxconn deep). */
#define LISTENERS 8
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 65536
/* A request is a few hundred bytes, a POST with its body under 2 KB; a longer one fills its
 * buffer and is read again. BUF_COUNT also bounds the single-shot reads in flight on a ring. */
#define BUF_SIZE 2048
#define BUF_COUNT 1024
/* A pass reads up to READS_PER_PASS times but answers at most ANSWERS_PER_PASS reads; the rest
 * wait on their connection (in `partial`) on the ready list. */
#define READS_PER_PASS 16384
#define ANSWERS_PER_PASS 2048
#define MAX_RINGS 16
/* Each pass waits on ring 0 for this many completions or WAIT_NS. Larger passes share one COMMIT. */
#define WAIT_MIN 256
#define WAIT_NS 1000000
#define POOL_MAX 2048
#define OUT_CAP 8192

typedef struct {
  unsigned short gen;
  u8 open, close_after, held, eof, closing, sending, queued, recving, ready, in_batch, answered;
  unsigned batch_mark, sent;
  lp_ob out;     /* responses not yet handed to the kernel */
  lp_ob wire;    /* the one send in flight */
  lp_ob partial; /* an incomplete request, or bytes waiting for a later pass's answers */
} conn;

typedef struct {
  struct io_uring ring;
  struct io_uring_buf_ring *br;
  u8 *bufmem;
  unsigned br_pending;
  conn *conns;
  unsigned ri;
  unsigned nopen;
  unsigned nreads; /* single-shot reads in flight: each holds at most one receive buffer */
  unsigned accepts[LISTENERS];
  unsigned depth;
  /* Accepting moves on while this many slots are still free (more than the accepts in flight):
   * an accept that finds the table full has already taken the connection off the listen queue
   * and would drop it. */
  unsigned margin;
  int accepting;
  int full; /* not accepting here: the table is (nearly) full */
} ring_t;

typedef struct {
  u64 *a;
  unsigned cap, head, n; /* cap is a power of two */
} fifo;

typedef struct {
  u64 *a;
  unsigned cap, n;
} vec;

static ring_t *rings[MAX_RINGS];
static unsigned nrings;
static lp_handler handler;
static unsigned slots;
static int listen_fds[LISTENERS];
static unsigned answers_left;
static int need_ring;  /* no ring is accepting: spill at the end of the batch */
static long spill_after; /* when adding a ring failed: don't retry before this (ms) */

static fifo rq;       /* readable connections waiting for a read */
static fifo ready;    /* connections with bytes read but not yet answered */
static vec held, held_spare; /* connections whose next request is a write waiting out a hold */
static vec touched;   /* connections with something to send or close after this batch */

static lp_ob pool[POOL_MAX];
static unsigned pool_n;

static inline u64 ud(unsigned op, unsigned ri, unsigned gen, unsigned slot) {
  return (u64)op << 56 | (u64)ri << 48 | (u64)gen << 32 | slot;
}
static inline u64 ent(unsigned ri, unsigned gen, unsigned slot) { return (u64)ri << 48 | (u64)gen << 32 | slot; }
static inline unsigned ent_ri(u64 e) { return (unsigned)(e >> 48) & 0xff; }
static inline unsigned ent_gen(u64 e) { return (unsigned)(e >> 32) & 0xffff; }
static inline unsigned ent_slot(u64 e) { return (unsigned)e; }

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- buffers ---- */

void lp_ob_grow(lp_ob *o, unsigned need) {
  if (o->cap - o->len >= need) return;
  unsigned nc = o->cap * 2;
  if (nc < o->len + need) nc = o->len + need;
  if (nc < 256) nc = 256;
  u8 *p = realloc(o->p, nc);
  if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
  o->p = p;
  o->cap = nc;
}

static void ob_take(lp_ob *o) {
  if (pool_n) {
    *o = pool[--pool_n];
    o->len = 0;
  } else {
    o->p = malloc(OUT_CAP);
    if (!o->p) { fprintf(stderr, "out of memory\n"); abort(); }
    o->len = 0;
    o->cap = OUT_CAP;
  }
}

static void ob_give(lp_ob *o) {
  if (!o->p) return;
  if (pool_n < POOL_MAX && o->cap <= 4 * OUT_CAP) {
    pool[pool_n++] = *o;
  } else {
    free(o->p);
  }
  memset(o, 0, sizeof *o);
}

static void ob_free(lp_ob *o) {
  free(o->p);
  memset(o, 0, sizeof *o);
}

static void ob_append(lp_ob *o, const u8 *p, unsigned n) {
  lp_ob_grow(o, n);
  memcpy(o->p + o->len, p, n);
  o->len += n;
}

static void fifo_init(fifo *f, unsigned cap) {
  f->a = malloc(cap * sizeof *f->a);
  if (!f->a) abort();
  f->cap = cap;
  f->head = f->n = 0;
}

static void fifo_push(fifo *f, u64 v) {
  if (f->n == f->cap) {
    u64 *a = malloc(f->cap * 2 * sizeof *a);
    if (!a) abort();
    for (unsigned i = 0; i < f->n; i++) a[i] = f->a[(f->head + i) & (f->cap - 1)];
    free(f->a);
    f->a = a;
    f->head = 0;
    f->cap *= 2;
  }
  f->a[(f->head + f->n++) & (f->cap - 1)] = v;
}

static u64 fifo_pop(fifo *f) {
  u64 v = f->a[f->head++ & (f->cap - 1)];
  f->n--;
  return v;
}

static void vec_push(vec *v, u64 e) {
  if (v->n == v->cap) {
    unsigned nc = v->cap ? v->cap * 2 : 1024;
    u64 *a = realloc(v->a, nc * sizeof *a);
    if (!a) abort();
    v->a = a;
    v->cap = nc;
  }
  v->a[v->n++] = e;
}

/* Frees a connection's buffers and resets it, keeping its generation. */
static void conn_reset(conn *c, unsigned short gen, int open) {
  ob_give(&c->out);
  ob_give(&c->wire);
  ob_free(&c->partial);
  memset(c, 0, sizeof *c);
  c->gen = gen;
  c->open = (u8)open;
}

/* ---- provided buffers ---- */

static inline void buf_add(ring_t *r, unsigned bid) {
  io_uring_buf_ring_add(r->br, r->bufmem + (size_t)bid * BUF_SIZE, BUF_SIZE, (unsigned short)bid, BUF_COUNT - 1, r->br_pending++);
}

static inline void buf_publish(ring_t *r) {
  if (r->br_pending) {
    io_uring_buf_ring_advance(r->br, (int)r->br_pending);
    r->br_pending = 0;
  }
}

/* Registers buffer group 0. Ubuntu's 6.8.0-136 and later kernels invert the check on the
 * reserved words (Launchpad #2162843): they refuse all zeroes and accept a nonzero resv[0], so a
 * refused registration is tried once more that way. A correct kernel never gets there. */
static int setup_bufs(ring_t *r) {
  size_t bytes = BUF_COUNT * sizeof(struct io_uring_buf);
  void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return -errno;
  r->bufmem = malloc((size_t)BUF_COUNT * BUF_SIZE);
  if (!r->bufmem) { munmap(m, bytes); return -ENOMEM; }
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (unsigned long)m;
  reg.ring_entries = BUF_COUNT;
  reg.bgid = 0;
  int rc = 0;
  for (int t = 0; t < 2; t++) {
    rc = io_uring_register_buf_ring(&r->ring, &reg, 0);
    if (rc != -EINVAL) break;
    reg.resv[0] = 1;
  }
  if (rc < 0) {
    free(r->bufmem);
    munmap(m, bytes);
    return rc;
  }
  r->br = m;
  io_uring_buf_ring_init(r->br);
  for (unsigned i = 0; i < BUF_COUNT; i++) buf_add(r, i);
  buf_publish(r);
  return 0;
}

/* ---- submissions ---- */

/* Room for `room` entries (a linked pair must not be split across submissions). If the kernel
 * can't take the queue yet (its completion queue is overflowing) it flushes completions and
 * tries again; only if that keeps failing is the entry dropped, rather than stalling every
 * connection. */
static struct io_uring_sqe *get_sqe(ring_t *r, unsigned room) {
  int tries = 0;
  while (io_uring_sq_space_left(&r->ring) < room) {
    if (io_uring_submit(&r->ring) < 0) io_uring_get_events(&r->ring);
    if (++tries == 1000) {
      fprintf(stderr, "submission queue stuck full; dropping an entry\n");
      return NULL;
    }
  }
  return io_uring_get_sqe(&r->ring);
}

/* The steady state: one multishot receive per connection, one completion per request. */
static void arm_recv(ring_t *r, unsigned slot, unsigned gen) {
  struct io_uring_sqe *s = get_sqe(r, 1);
  if (!s) return;
  io_uring_prep_recv_multishot(s, (int)slot, NULL, 0, 0);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  io_uring_sqe_set_data64(s, ud(OP_RECV, r->ri, gen, slot));
}

/* One read of a queued connection. MSG_DONTWAIT: if nothing is left it ends with EAGAIN. */
static void arm_read(ring_t *r, unsigned slot, unsigned gen) {
  struct io_uring_sqe *s = get_sqe(r, 1);
  if (!s) return;
  io_uring_prep_recv(s, (int)slot, NULL, 0, MSG_DONTWAIT);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  io_uring_sqe_set_data64(s, ud(OP_READ, r->ri, gen, slot));
}

static void arm_send(ring_t *r, unsigned slot, conn *c) {
  struct io_uring_sqe *s = get_sqe(r, 1);
  if (!s) return;
  io_uring_prep_send(s, (int)slot, c->wire.p + c->sent, c->wire.len - c->sent, MSG_NOSIGNAL);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(s, ud(OP_SEND, r->ri, c->gen, slot));
}

static void arm_close(ring_t *r, unsigned slot, conn *c) {
  c->closing = 1;
  /* The shutdown ends the multishot receive or the read in flight; the close frees the slot. */
  struct io_uring_sqe *s = get_sqe(r, 2);
  if (!s) return;
  io_uring_prep_shutdown(s, (int)slot, SHUT_RDWR);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  io_uring_sqe_set_data64(s, ud(OP_SHUTDOWN, r->ri, c->gen, slot));
  s = io_uring_get_sqe(&r->ring);
  io_uring_prep_close_direct(s, slot);
  io_uring_sqe_set_data64(s, ud(OP_CLOSE, r->ri, c->gen, slot));
}

static void fill_accepts(ring_t *r) {
  for (unsigned li = 0; li < LISTENERS; li++) {
    while (r->accepts[li] < r->depth / LISTENERS) {
      struct io_uring_sqe *s = get_sqe(r, 1);
      if (!s) return;
      io_uring_prep_accept_direct(s, listen_fds[li], NULL, NULL, 0, IORING_FILE_INDEX_ALLOC);
      io_uring_sqe_set_data64(s, ud(OP_ACCEPT, r->ri, 0, li));
      r->accepts[li]++;
    }
  }
}

static void arm_accept(ring_t *r) {
  r->accepting = 1;
  fill_accepts(r);
}

/* ---- connections ---- */

static inline void mark(ring_t *r, unsigned slot, conn *c, unsigned out_len) {
  if (!c->in_batch) {
    c->in_batch = 1;
    c->batch_mark = out_len;
    vec_push(&touched, ent(r->ri, 0, slot));
  }
}

/* The client is done sending (EOF, an error). A held write or bytes on the ready list are still
 * owed answers: then it closes once they are answered. */
static void finish(ring_t *r, unsigned slot, conn *c) {
  if (c->held || c->ready) {
    c->eof = 1;
  } else {
    c->close_after = 1;
    mark(r, slot, c, c->out.len);
  }
}

/* Parses and answers every complete request in data (plus any earlier partial bytes). During a
 * checkpoint hold a write stops it: that request and everything behind it stay on the
 * connection, which goes on the held list, until the hold ends. */
static void serve(ring_t *r, unsigned slot, conn *c, const u8 *data, unsigned dlen) {
  if (c->close_after) return;
  const u8 *buf;
  unsigned blen;
  int own = c->partial.len != 0;
  if (own) {
    if (dlen) ob_append(&c->partial, data, dlen);
    buf = c->partial.p;
    blen = c->partial.len;
  } else {
    buf = data;
    blen = dlen;
  }
  unsigned off = 0;
  while (off < blen) {
    if (!c->out.p) ob_take(&c->out);
    unsigned before = c->out.len, used = 0;
    int rc = handler(buf + off, blen - off, &c->out, &used);
    if (rc == 1) {
      mark(r, slot, c, before);
      c->answered = 1;
      off += used;
    } else if (rc == 0) {
      break;
    } else if (rc == 2) {
      if (!c->held) {
        c->held = 1;
        vec_push(&held, ent(r->ri, c->gen, slot));
      }
      break;
    } else {
      mark(r, slot, c, before);
      c->answered = 1;
      c->close_after = 1;
      off = blen;
      break;
    }
  }
  if (c->out.p && c->out.len == 0 && !c->in_batch) ob_give(&c->out);
  unsigned rest = blen - off;
  if (rest == 0) {
    if (own) ob_free(&c->partial);
  } else if (own) {
    memmove(c->partial.p, c->partial.p + off, rest);
    c->partial.len = rest;
  } else {
    ob_append(&c->partial, data + off, rest);
  }
}

/* Bytes received: answered now if this pass has answers left and nothing older waits on the
 * connection, otherwise kept for a later pass (drain first). */
static void received(ring_t *r, unsigned slot, conn *c, const u8 *data, unsigned len) {
  if (c->close_after) return; /* its answers are done; what follows is dropped */
  if (c->held || c->ready || answers_left == 0) {
    ob_append(&c->partial, data, len);
    if (!c->held && !c->ready) {
      c->ready = 1;
      fifo_push(&ready, ent(r->ri, c->gen, slot));
    }
  } else {
    answers_left--;
    serve(r, slot, c, data, len);
  }
}

static inline void queue_read(ring_t *r, unsigned slot, conn *c) {
  if (!c->queued) {
    c->queued = 1;
    fifo_push(&rq, ent(r->ri, c->gen, slot));
  }
}

static void on_cqe(ring_t *r, u64 u, int res, unsigned flags) {
  unsigned op = (unsigned)(u >> 56), gen = ent_gen(u), slot = ent_slot(u);
  int has_buf = flags & IORING_CQE_F_BUFFER;
  unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
  conn *c;
  switch (op) {
  case OP_ACCEPT:
    r->accepts[slot]--;
    if (res >= 0) {
      c = &r->conns[res];
      conn_reset(c, (unsigned short)(c->gen + 1), 1);
      arm_recv(r, (unsigned)res, c->gen);
      r->nopen++;
      if (r->nopen + r->margin >= slots) r->full = 1;
    } else if (res == -ENFILE) {
      r->full = 1;
    }
    break;
  case OP_RECV:
    c = &r->conns[slot];
    if (c->gen != gen || !c->open || c->closing) {
      if (has_buf) buf_add(r, bid);
      break;
    }
    if (res > 0) {
      if (has_buf) {
        received(r, slot, c, r->bufmem + (size_t)bid * BUF_SIZE, (unsigned)res);
        buf_add(r, bid);
      }
      /* The multishot ended (the buffers ran out behind it): read the rest in bounded
       * single-shot reads, which re-arm it once the socket is drained. */
      if (!(flags & IORING_CQE_F_MORE)) queue_read(r, slot, c);
    } else if (res == -ENOBUFS) {
      /* A burst emptied the buffer ring. Not re-armed: re-arming every receive at once repeats
       * the storm. */
      queue_read(r, slot, c);
    } else {
      if (has_buf) buf_add(r, bid);
      finish(r, slot, c);
    }
    break;
  case OP_READ:
    r->nreads--;
    c = &r->conns[slot];
    if (c->gen != gen || !c->open || c->closing) {
      if (has_buf) buf_add(r, bid);
      break;
    }
    c->recving = 0;
    if (res > 0) {
      if (!has_buf) {
        queue_read(r, slot, c);
        break;
      }
      received(r, slot, c, r->bufmem + (size_t)bid * BUF_SIZE, (unsigned)res);
      buf_add(r, bid);
      if (res == BUF_SIZE) queue_read(r, slot, c); /* the buffer filled: there may be more */
      else arm_recv(r, slot, gen);                 /* drained: back to the multishot receive */
    } else if (res == -EAGAIN) {
      arm_recv(r, slot, gen);
    } else if (res == -ENOBUFS) {
      queue_read(r, slot, c);
    } else {
      if (has_buf) buf_add(r, bid);
      finish(r, slot, c);
    }
    break;
  case OP_SEND:
    c = &r->conns[slot];
    if (c->gen != gen || !c->sending) break;
    if (res < 0) {
      c->sending = 0;
      ob_give(&c->wire);
      ob_give(&c->out);
      c->close_after = 1;
      mark(r, slot, c, c->out.len);
    } else {
      c->sent += (unsigned)res;
      if (c->sent < c->wire.len) {
        arm_send(r, slot, c);
      } else {
        c->sending = 0;
        ob_give(&c->wire);
        if (c->out.len || c->close_after) mark(r, slot, c, c->out.len); /* sent after this batch commits */
        else ob_give(&c->out);
      }
    }
    break;
  case OP_CLOSE:
    c = &r->conns[slot];
    if (c->gen == gen && c->open) {
      conn_reset(c, c->gen, 0);
      r->nopen--;
    }
    if (r->full && r->nopen + 2 * r->margin < slots) {
      r->full = 0;
      r->accepting = 1;
    }
    break;
  default:
    break;
  }
}

static void drain(ring_t *r) {
  unsigned head, n = 0;
  struct io_uring_cqe *cqe;
  io_uring_for_each_cqe(&r->ring, head, cqe) {
    n++;
    on_cqe(r, cqe->user_data, cqe->res, cqe->flags);
  }
  io_uring_cq_advance(&r->ring, n);
  buf_publish(r);
  if (r->accepting) {
    if (r->full) {
      /* Move on; the accepts still in flight here fit in the margin. */
      r->accepting = 0;
      need_ring = 1;
    } else {
      fill_accepts(r);
    }
  }
}

static void drain_all(void) {
  for (unsigned i = 0; i < nrings; i++) drain(rings[i]);
}

/* ---- rings ---- */

static unsigned clamp(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A new ring with its own descriptor table, buffers and connection records; NULL on failure. */
static ring_t *ring_new(unsigned ri) {
  ring_t *r = calloc(1, sizeof *r);
  if (!r) {
    fprintf(stderr, "out of memory\n");
    return NULL;
  }
  r->ri = ri;
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
  p.cq_entries = CQ_ENTRIES;
  int rc = io_uring_queue_init_params(SQ_ENTRIES, &r->ring, &p);
  if (rc < 0) {
    /* Kernels before 6.1 lack DEFER_TASKRUN: the same loop works with the default task work. */
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = CQ_ENTRIES;
    rc = io_uring_queue_init_params(SQ_ENTRIES, &r->ring, &p);
  }
  if (rc < 0) {
    fprintf(stderr, "io_uring: %s\n", strerror(-rc));
    free(r);
    return NULL;
  }
  if ((rc = io_uring_register_files_sparse(&r->ring, slots)) < 0) {
    fprintf(stderr, "register files: %s\n", strerror(-rc));
    goto fail;
  }
  if ((rc = setup_bufs(r)) < 0) {
    fprintf(stderr, "buffer ring: %s\n", strerror(-rc));
    goto fail;
  }
  r->conns = calloc(slots, sizeof *r->conns);
  if (!r->conns) {
    fprintf(stderr, "out of memory\n");
    goto fail;
  }
  /* In flight at once: at most a sixteenth of the table, and always less than the margin. */
  r->depth = clamp(slots / 16, LISTENERS, ACCEPT_DEPTH) / LISTENERS * LISTENERS;
  /* At most a quarter of a small table, so a full ring can still drain below 2x margin. */
  r->margin = clamp(slots / 8, r->depth + 1, 2 * ACCEPT_DEPTH);
  if (r->margin > slots / 4) r->margin = slots / 4;
  return r;
fail:
  io_uring_queue_exit(&r->ring);
  free(r);
  return NULL;
}

/* A ring's descriptor table is full: accept on the next ring with room, adding one if needed.
 * Returns whether some ring is accepting afterwards. */
static int spill(void) {
  for (unsigned i = 0; i < nrings; i++)
    if (rings[i]->accepting) return 1;
  for (unsigned i = 0; i < nrings; i++) {
    if (!rings[i]->full) {
      arm_accept(rings[i]);
      return 1;
    }
  }
  if (nrings >= MAX_RINGS) {
    fprintf(stderr, "connection limit reached (%u rings)\n", nrings);
    return 0;
  }
  ring_t *r = ring_new(nrings);
  if (!r) {
    fprintf(stderr, "cannot add ring\n");
    return 0;
  }
  arm_accept(r);
  rings[nrings++] = r;
  fprintf(stderr, "ring %u added (%u slots each)\n", r->ri, slots);
  return 1;
}

/* ---- the pass ---- */

static void send_500(lp_ob *o) {
  static const char body[] = "{\"error\":\"internal server error\"}";
  char head[128];
  int n = snprintf(head, sizeof head,
                   "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n",
                   (int)(sizeof body - 1));
  ob_append(o, (const u8 *)head, (unsigned)n);
  ob_append(o, (const u8 *)body, (unsigned)(sizeof body - 1));
}

/* Commits the batch, then hands every response it produced to the kernel. */
static void end_batch(void) {
  int committed = td_commit() == 0;
  /* No transaction is open here: the only point where a checkpoint hold starts or ends. */
  td_tick();
  for (unsigned i = 0; i < touched.n; i++) {
    ring_t *r = rings[ent_ri(touched.a[i])];
    unsigned slot = ent_slot(touched.a[i]);
    conn *c = &r->conns[slot];
    if (!committed && c->answered) {
      /* No response from a rolled-back batch may claim success. */
      if (!c->out.p) ob_take(&c->out);
      c->out.len = c->batch_mark;
      send_500(&c->out);
      c->close_after = 1;
    }
    c->in_batch = 0;
    c->answered = 0;
    if (!c->open || c->closing || c->sending) continue;
    if (c->out.len) {
      c->wire = c->out;
      memset(&c->out, 0, sizeof c->out);
      c->sent = 0;
      c->sending = 1;
      arm_send(r, slot, c);
    } else if (c->close_after) {
      arm_close(r, slot, c);
    }
  }
  touched.n = 0;
  if (need_ring && now_ms() >= spill_after) {
    /* Retried (once a second) until some ring accepts again, so accepting never stops. */
    need_ring = !spill();
    if (need_ring) spill_after = now_ms() + 1000;
  }
}

/* The hold is over: serves the writes it held (and whatever arrived behind them) in this pass,
 * from the bytes kept on each connection. */
static void release_held(void) {
  vec list = held;
  held = held_spare;
  held.n = 0;
  for (unsigned i = 0; i < list.n; i++) {
    ring_t *r = rings[ent_ri(list.a[i])];
    unsigned slot = ent_slot(list.a[i]), gen = ent_gen(list.a[i]);
    conn *c = &r->conns[slot];
    if (c->gen == gen && c->open && !c->closing) {
      c->held = 0;
      serve(r, slot, c, NULL, 0);
      if (c->eof && !c->held) {
        c->close_after = 1;
        mark(r, slot, c, c->out.len);
      }
    }
  }
  held_spare = list;
}

/* Answers connections on the ready list, oldest first, while this pass has answers left. */
static void run_ready(void) {
  while (answers_left > 0 && ready.n) {
    u64 e = fifo_pop(&ready);
    ring_t *r = rings[ent_ri(e)];
    unsigned slot = ent_slot(e), gen = ent_gen(e);
    conn *c = &r->conns[slot];
    if (c->gen != gen || !c->open || c->closing) continue;
    c->ready = 0;
    if (c->held) continue; /* release_held answers it, with everything behind the held write */
    answers_left--;
    serve(r, slot, c, NULL, 0);
    if (c->eof && !c->held) {
      c->close_after = 1;
      mark(r, slot, c, c->out.len);
    }
  }
}

/* Issues one read per queued connection, up to budget and never more on a ring than it has free
 * receive buffers, so the kernel never runs out of them. The rest stay queued, in order. */
static unsigned issue_reads(unsigned budget) {
  unsigned issued = 0, count = rq.n;
  for (unsigned i = 0; i < count && issued < budget; i++) {
    u64 e = fifo_pop(&rq);
    ring_t *r = rings[ent_ri(e)];
    unsigned slot = ent_slot(e), gen = ent_gen(e);
    conn *c = &r->conns[slot];
    if (c->gen != gen || !c->open || c->closing) continue;
    if (c->recving || r->nreads >= BUF_COUNT) {
      fifo_push(&rq, e);
      continue;
    }
    c->queued = 0;
    c->recving = 1;
    r->nreads++;
    issued++;
    arm_read(r, slot, gen);
  }
  return issued;
}

/* Reads the queued connections in rounds. A MSG_DONTWAIT read completes inside the submit, so
 * each round is one non-blocking enter per ring, and a request is answered in the pass that saw
 * it readable. */
static void read_rounds(void) {
  unsigned budget = READS_PER_PASS;
  while (budget > 0 && rq.n) {
    unsigned k = issue_reads(budget);
    if (k == 0) break;
    budget -= k;
    for (unsigned i = 0; i < nrings; i++) {
      if (!io_uring_sq_ready(&rings[i]->ring)) continue;
      int rc = io_uring_submit_and_get_events(&rings[i]->ring);
      if (rc < 0 && rc != -EINTR && rc != -EBUSY && rc != -EAGAIN) fprintf(stderr, "io_uring_enter: %s\n", strerror(-rc));
    }
    drain_all();
  }
}

/* ---- setup ---- */

static int listener(struct in_addr ip, int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) return -1;
  /* Accepted sockets inherit TCP_NODELAY. */
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr = ip;
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
  if (listen(fd, 65535) < 0) return -1;
  return fd;
}

/* Raises the soft descriptor limit to the hard one. A ring's file table may be as large as it. */
static unsigned descriptor_slots(void) {
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  return rl.rlim_cur < (1u << 20) ? (unsigned)rl.rlim_cur : (1u << 20);
}

int lp_run(const char *host, int port, lp_handler h) {
  handler = h;
  signal(SIGPIPE, SIG_IGN);
  struct in_addr ip;
  if (inet_pton(AF_INET, host, &ip) != 1) {
    fprintf(stderr, "HOST must be an IPv4 address\n");
    return -1;
  }
  slots = descriptor_slots();
  for (int i = 0; i < LISTENERS; i++) {
    listen_fds[i] = listener(ip, port);
    if (listen_fds[i] < 0) {
      fprintf(stderr, "listen %s:%d: %s\n", host, port, strerror(errno));
      return -1;
    }
  }
  fifo_init(&rq, 1024);
  fifo_init(&ready, 1024);
  rings[0] = ring_new(0);
  if (!rings[0]) return -1;
  nrings = 1;
  arm_accept(rings[0]);
  fprintf(stderr, "listening on %s:%d; io_uring, %u direct descriptors per ring\n", host, port, slots);

  struct __kernel_timespec ts = {0, WAIT_NS};
  for (;;) {
    /* Submit what the last batch queued and wait for completions. Deferred task work
     * (completions) only runs inside an enter with GETEVENTS. The other rings submit and collect
     * without waiting; ring 0 goes last and waits, so with several rings a pass still spans up to
     * WAIT_NS instead of spinning, and no ring's sends sit behind the wait. With reads or answers
     * still queued there is work now: ring 0 doesn't wait either. */
    int busy = rq.n || ready.n;
    for (unsigned i = nrings; i-- > 0;) {
      struct io_uring_cqe *cqe;
      int rc = i == 0 && !busy ? io_uring_submit_and_wait_timeout(&rings[0]->ring, &cqe, WAIT_MIN, &ts, NULL)
                               : io_uring_submit_and_get_events(&rings[i]->ring);
      if (rc < 0 && rc != -EINTR && rc != -ETIME && rc != -EBUSY && rc != -EAGAIN) fprintf(stderr, "io_uring_enter: %s\n", strerror(-rc));
    }
    answers_left = ANSWERS_PER_PASS;
    drain_all();
    run_ready();
    read_rounds();
    if (held.n && !td_writes_held()) release_held();
    end_batch();
  }
}
