/* net.c: the io_uring transport under app.goose. Sockets, rings and byte buffers only.
 *
 * Nothing here looks inside a request. Goose drives every pass:
 *
 *   net_wait()             submit what the last pass queued, wait for completions, reap them
 *   net_next(ev, data)     the next connection with new bytes; they are appended to `data`
 *   net_reply/keep/hold/close   what Goose decided for that connection
 *   net_flush(committed)   after the pass's COMMIT: hand every response to the kernel
 *
 * Design (see README.md): one thread, SINGLE_ISSUER + DEFER_TASKRUN rings, accepted sockets as
 * direct descriptors (so LimitNOFILE caps a ring's table, not the server), a new ring whenever
 * one fills, eight SO_REUSEPORT listeners, single-shot accepts kept in flight, one multishot
 * receive per connection into a provided-buffer ring, one send in flight per connection, and
 * SHUTDOWN hard-linked to CLOSE. */
#define _GNU_SOURCE
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "goose.h"

typedef struct { const uint8_t *data; int64_t len; } sl_bytes;

/* The flat event Goose reads: which connection, and (for Goose's own bookkeeping) its kind. */
#pragma pack(push, 1)
typedef struct { int64_t conn; int64_t kind; } NetEv;
#pragma pack(pop)

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_SHUTDOWN, OP_CLOSE };
#define LISTENERS 8
#define ACCEPT_DEPTH 512
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 65536
#define BUF_SIZE 2048
#define BUF_COUNT 1024
#define MAX_RINGS 16
#define WAIT_MIN 256
#define WAIT_NS 1000000
static long wait_ns = WAIT_NS; /* GOOSE_WAIT_US overrides it (test-only knob for comparisons) */
#define POOL_MAX 2048
#define OUT_CAP 8192

enum {
  F_OPEN = 1, F_CLOSE_AFTER = 2, F_HELD = 4, F_EOF = 8,
  F_CLOSING = 16, F_SENDING = 32, F_IN_BATCH = 64, F_ANSWERED = 128,
};

typedef struct Buf { uint32_t len, cap; uint8_t d[]; } Buf;

typedef struct {
  uint16_t gen;
  uint16_t flags;
  uint32_t mark; /* out->len when this pass first touched the connection */
  uint32_t sent;
  uint32_t _pad;
  Buf *out;      /* responses not yet handed to the kernel */
  Buf *wire;     /* the one send in flight */
  Buf *partial;  /* bytes of a request not complete yet (or held) */
} Conn;

typedef struct {
  struct io_uring ring;
  struct io_uring_buf_ring *br;
  uint8_t *bufmem;
  unsigned br_pending; /* buffers added since the last advance */
  Conn *conns;
  uint32_t slots, open, depth, margin;
  bool accepting, full;
  uint32_t accepts[LISTENERS];
  uint32_t *rearm;
  uint32_t nrearm, caprearm;
} Ring;

/* A completion that carries bytes (or the end of a stream), in completion order. */
enum { P_DATA = 0, P_EOF = 1, P_REPLAY = 2 };
typedef struct { uint32_t slot; uint16_t gen; uint8_t ring; uint8_t kind; uint16_t bid; uint16_t _p; int32_t len; } Pend;

static Ring *rings[MAX_RINGS];
static int nrings;
static int listen_fd[LISTENERS];
static uint32_t slots_per_ring;
static Buf *pool[POOL_MAX];
static int npool;
static uint64_t *touched; /* (ring << 32) | slot */
static uint32_t ntouched, captouched;
static Pend *pend;
static uint32_t npend, cappend, ipend;
static uint64_t *held; /* (ring << 48) | (gen << 32) | slot */
static uint32_t nheld, capheld;
static bool need_ring;
static struct timespec spill_after;
static int64_t cur_replay = -1; /* a replayed connection whose EOF is still owed a close */

static void die(const char *what, int err) {
  fprintf(stderr, "%s: %s\n", what, strerror(err));
  exit(1);
}

static void *grow(void *p, uint32_t *cap, size_t elem) {
  uint32_t n = *cap ? *cap * 2 : 1024;
  void *q = realloc(p, n * elem);
  if (!q) die("out of memory", ENOMEM);
  *cap = n;
  return q;
}

static inline uint64_t ud(uint64_t op, uint64_t ri, uint64_t gen, uint64_t slot) {
  return op << 56 | ri << 48 | gen << 32 | slot;
}

/* ---- buffers ---- */

static Buf *buf_take(void) {
  if (npool) return pool[--npool];
  Buf *b = malloc(sizeof(Buf) + OUT_CAP);
  if (!b) die("out of memory", ENOMEM);
  b->len = 0;
  b->cap = OUT_CAP;
  return b;
}

static void buf_give(Buf **bp) {
  Buf *b = *bp;
  if (!b) return;
  *bp = NULL;
  if (npool < POOL_MAX && b->cap <= 4 * OUT_CAP) {
    b->len = 0;
    pool[npool++] = b;
  } else {
    free(b);
  }
}

static inline void buf_append(Buf **bp, const void *p, size_t n) {
  if (!n) return;
  Buf *b = *bp;
  if (!b) b = *bp = buf_take();
  if (b->len + n > b->cap) {
    size_t cap = b->cap;
    while (b->len + n > cap) cap *= 2;
    b = realloc(b, sizeof(Buf) + cap);
    if (!b) die("out of memory", ENOMEM);
    b->cap = (uint32_t)cap;
    *bp = b;
  }
  memcpy(b->d + b->len, p, n);
  b->len += (uint32_t)n;
}

/* ---- submission helpers ---- */

/* A free SQE, flushing the queue to the kernel when it is full. */
static struct io_uring_sqe *sqe_get(Ring *r) {
  struct io_uring_sqe *s = io_uring_get_sqe(&r->ring);
  for (int tries = 0; !s && tries < 1000; tries++) {
    if (io_uring_submit(&r->ring) < 0) io_uring_get_events(&r->ring);
    s = io_uring_get_sqe(&r->ring);
  }
  if (!s) fprintf(stderr, "submission queue stuck full; dropping an entry\n");
  return s;
}

static void arm_recv(Ring *r, int ri, uint32_t slot, uint16_t gen) {
  struct io_uring_sqe *s = sqe_get(r);
  if (!s) return;
  io_uring_prep_recv_multishot(s, (int)slot, NULL, 0, 0);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  io_uring_sqe_set_data64(s, ud(OP_RECV, ri, gen, slot));
}

static void arm_send(Ring *r, int ri, uint32_t slot, Conn *c) {
  struct io_uring_sqe *s = sqe_get(r);
  if (!s) return;
  io_uring_prep_send(s, (int)slot, c->wire->d + c->sent, c->wire->len - c->sent, MSG_NOSIGNAL);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(s, ud(OP_SEND, ri, c->gen, slot));
}

static void arm_close(Ring *r, int ri, uint32_t slot, Conn *c) {
  c->flags |= F_CLOSING;
  /* Both entries in one submission: a linked pair must not be split. */
  if (io_uring_sq_space_left(&r->ring) < 2) io_uring_submit(&r->ring);
  struct io_uring_sqe *s = sqe_get(r);
  if (!s) return;
  io_uring_prep_shutdown(s, (int)slot, SHUT_RDWR);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  io_uring_sqe_set_data64(s, ud(OP_SHUTDOWN, ri, c->gen, slot));
  struct io_uring_sqe *cl = sqe_get(r);
  if (!cl) return;
  io_uring_prep_close_direct(cl, slot);
  io_uring_sqe_set_data64(cl, ud(OP_CLOSE, ri, c->gen, slot));
}

static void fill_accepts(Ring *r, int ri) {
  for (int li = 0; li < LISTENERS; li++) {
    while (r->accepts[li] < r->depth / LISTENERS) {
      struct io_uring_sqe *s = sqe_get(r);
      if (!s) return;
      io_uring_prep_accept_direct(s, listen_fd[li], NULL, NULL, 0, IORING_FILE_INDEX_ALLOC);
      io_uring_sqe_set_data64(s, ud(OP_ACCEPT, ri, 0, li));
      r->accepts[li]++;
    }
  }
}

/* ---- rings ---- */

/* Ubuntu's 6.8.0-136 and later kernels refuse a buffer-ring registration whose reserved words are
 * all zero (Launchpad #2162843, an inverted check); a refused registration is retried once with
 * resv[0] = 1, which a correct kernel never sees. */
static int register_buf_ring(struct io_uring *ring, struct io_uring_buf_ring *br) {
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (uint64_t)(uintptr_t)br;
  reg.ring_entries = BUF_COUNT;
  reg.bgid = 0;
  int rc = io_uring_register_buf_ring(ring, &reg, 0);
  if (rc == -EINVAL) {
    reg.resv[0] = 1;
    rc = io_uring_register_buf_ring(ring, &reg, 0);
  }
  return rc;
}

static Ring *ring_new(uint32_t slots, int *err) {
  Ring *r = calloc(1, sizeof *r);
  if (!r) { *err = ENOMEM; return NULL; }
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL |
            IORING_SETUP_CQSIZE;
  p.cq_entries = CQ_ENTRIES;
  int rc = io_uring_queue_init_params(SQ_ENTRIES, &r->ring, &p);
  if (rc < 0) { *err = -rc; free(r); return NULL; }
  rc = io_uring_register_files_sparse(&r->ring, slots);
  if (rc < 0) { *err = -rc; io_uring_queue_exit(&r->ring); free(r); return NULL; }
  size_t brsz = BUF_COUNT * sizeof(struct io_uring_buf);
  r->br = mmap(NULL, brsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  r->bufmem = malloc((size_t)BUF_COUNT * BUF_SIZE);
  r->conns = calloc(slots, sizeof(Conn));
  if (r->br == MAP_FAILED || !r->bufmem || !r->conns) {
    *err = ENOMEM;
    io_uring_queue_exit(&r->ring);
    free(r);
    return NULL;
  }
  io_uring_buf_ring_init(r->br);
  rc = register_buf_ring(&r->ring, r->br);
  if (rc < 0) { *err = -rc; io_uring_queue_exit(&r->ring); free(r); return NULL; }
  for (int i = 0; i < BUF_COUNT; i++)
    io_uring_buf_ring_add(r->br, r->bufmem + (size_t)i * BUF_SIZE, BUF_SIZE, (unsigned short)i,
                          io_uring_buf_ring_mask(BUF_COUNT), i);
  io_uring_buf_ring_advance(r->br, BUF_COUNT);
  r->slots = slots;
  /* In flight at once: at most a sixteenth of the table, and always less than the margin. */
  uint32_t d = slots / 16;
  if (d < LISTENERS) d = LISTENERS;
  if (d > ACCEPT_DEPTH) d = ACCEPT_DEPTH;
  r->depth = d / LISTENERS * LISTENERS;
  /* Accepting moves on while this many slots are still free: an accept that finds the table full
   * has already taken the connection off the listen queue and would drop it. */
  uint32_t m = slots / 8;
  if (m < r->depth + 1) m = r->depth + 1;
  if (m > 2 * ACCEPT_DEPTH) m = 2 * ACCEPT_DEPTH;
  if (m > slots / 4) m = slots / 4;
  r->margin = m;
  return r;
}

static void arm_accept(Ring *r, int ri) {
  r->accepting = true;
  fill_accepts(r, ri);
}

/* A ring's table is (nearly) full: accept on the next ring with room, adding one if needed. */
static bool spill(void) {
  for (int i = 0; i < nrings; i++)
    if (rings[i]->accepting) return true;
  for (int i = 0; i < nrings; i++) {
    if (!rings[i]->full) { arm_accept(rings[i], i); return true; }
  }
  if (nrings >= MAX_RINGS) {
    fprintf(stderr, "connection limit reached (%d rings)\n", nrings);
    return false;
  }
  int err = 0;
  Ring *r = ring_new(slots_per_ring, &err);
  if (!r) {
    fprintf(stderr, "cannot add ring: %s\n", strerror(err));
    return false;
  }
  int ri = nrings;
  rings[nrings++] = r;
  arm_accept(r, ri);
  fprintf(stderr, "ring %d added (%u slots each)\n", ri, slots_per_ring);
  return true;
}

/* ---- per-connection bookkeeping ---- */

static inline Conn *conn_of(int64_t id, Ring **rp, int *rip, uint32_t *slotp) {
  uint32_t ri = (uint32_t)((uint64_t)id >> 48) & 0xff;
  uint16_t gen = (uint16_t)((uint64_t)id >> 32);
  uint32_t slot = (uint32_t)id;
  if (ri >= (uint32_t)nrings) return NULL;
  Ring *r = rings[ri];
  if (slot >= r->slots) return NULL;
  Conn *c = &r->conns[slot];
  if (c->gen != gen || !(c->flags & F_OPEN) || (c->flags & F_CLOSING)) return NULL;
  if (rp) *rp = r;
  if (rip) *rip = (int)ri;
  if (slotp) *slotp = slot;
  return c;
}

static inline void mark(int ri, uint32_t slot, Conn *c) {
  if (c->flags & F_IN_BATCH) return;
  c->flags |= F_IN_BATCH;
  c->mark = c->out ? c->out->len : 0;
  if (ntouched == captouched) touched = grow(touched, &captouched, sizeof *touched);
  touched[ntouched++] = (uint64_t)ri << 32 | slot;
}

static inline void push_pend(int ri, uint32_t slot, uint16_t gen, uint8_t kind, uint16_t bid, int32_t len) {
  if (npend == cappend) pend = grow(pend, &cappend, sizeof *pend);
  Pend *p = &pend[npend++];
  p->slot = slot; p->gen = gen; p->ring = (uint8_t)ri; p->kind = kind; p->bid = bid; p->len = len;
}

static inline void rearm_later(Ring *r, uint32_t slot) {
  if (r->nrearm == r->caprearm) r->rearm = grow(r->rearm, &r->caprearm, sizeof *r->rearm);
  r->rearm[r->nrearm++] = slot;
}

static inline void buf_return(Ring *r, uint16_t bid) {
  io_uring_buf_ring_add(r->br, r->bufmem + (size_t)bid * BUF_SIZE, BUF_SIZE, bid,
                        io_uring_buf_ring_mask(BUF_COUNT), (int)r->br_pending);
  r->br_pending++;
}

static inline void buf_publish(Ring *r) {
  if (r->br_pending) {
    io_uring_buf_ring_advance(r->br, (int)r->br_pending);
    r->br_pending = 0;
  }
}

/* ---- the pass ---- */

static void drain(int ri) {
  Ring *r = rings[ri];
  struct io_uring_cqe *cqe;
  unsigned head, n = 0;
  io_uring_for_each_cqe(&r->ring, head, cqe) {
    n++;
    uint64_t u = cqe->user_data;
    unsigned op = (unsigned)(u >> 56);
    uint16_t gen = (uint16_t)(u >> 32);
    uint32_t slot = (uint32_t)u;
    int res = cqe->res;
    unsigned flags = cqe->flags;
    switch (op) {
    case OP_ACCEPT:
      r->accepts[slot]--;
      if (res >= 0) {
        Conn *c = &r->conns[res];
        uint16_t g = (uint16_t)(c->gen + 1);
        memset(c, 0, sizeof *c);
        c->gen = g;
        c->flags = F_OPEN;
        arm_recv(r, ri, (uint32_t)res, g);
        r->open++;
        if (r->open + r->margin >= r->slots) r->full = true;
      } else if (res == -ENFILE) {
        r->full = true;
      }
      break;
    case OP_RECV: {
      Conn *c = &r->conns[slot];
      bool has_buf = flags & IORING_CQE_F_BUFFER;
      uint16_t bid = (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT);
      if (c->gen != gen || !(c->flags & F_OPEN) || (c->flags & F_CLOSING)) {
        if (has_buf) buf_return(r, bid);
        break;
      }
      if (res > 0) {
        if (!has_buf) { rearm_later(r, slot); break; }
        push_pend(ri, slot, gen, P_DATA, bid, res);
        if (!(flags & IORING_CQE_F_MORE)) rearm_later(r, slot);
      } else if (res == -ENOBUFS) {
        rearm_later(r, slot);
      } else {
        /* EOF or error: finish what was asked, then close (in order with the data before it). */
        if (has_buf) buf_return(r, bid);
        push_pend(ri, slot, gen, P_EOF, 0, 0);
      }
      break;
    }
    case OP_SEND: {
      Conn *c = &r->conns[slot];
      if (c->gen != gen || !(c->flags & F_SENDING)) break;
      if (res < 0) {
        c->flags &= ~F_SENDING;
        buf_give(&c->wire);
        buf_give(&c->out);
        c->flags |= F_CLOSE_AFTER;
        mark(ri, slot, c);
      } else {
        c->sent += (uint32_t)res;
        if (c->sent < c->wire->len) {
          arm_send(r, ri, slot, c);
        } else {
          c->flags &= ~F_SENDING;
          buf_give(&c->wire);
          if ((c->out && c->out->len) || (c->flags & F_CLOSE_AFTER)) mark(ri, slot, c);
          else buf_give(&c->out);
        }
      }
      break;
    }
    case OP_CLOSE: {
      Conn *c = &r->conns[slot];
      if (c->gen == gen && (c->flags & F_OPEN)) {
        uint16_t g = c->gen;
        buf_give(&c->out);
        buf_give(&c->wire);
        buf_give(&c->partial);
        memset(c, 0, sizeof *c);
        c->gen = g;
        r->open--;
      }
      if (r->full && r->open + 2 * r->margin < r->slots) {
        r->full = false;
        r->accepting = true;
      }
      break;
    }
    default:
      break;
    }
  }
  io_uring_cq_advance(&r->ring, n);
  if (r->accepting) {
    if (r->full) {
      /* Move on; the accepts still in flight here fit in the margin. */
      r->accepting = false;
      need_ring = true;
    } else {
      fill_accepts(r, ri);
    }
  }
}

/* Submits what the last pass queued and collects completions. Ring 0 goes last and waits for
 * WAIT_MIN completions or WAIT_NS, so a busy pass shares one COMMIT across many requests. */
void net_wait(void) {
  for (int ri = nrings - 1; ri >= 0; ri--) {
    Ring *r = rings[ri];
    int rc;
    if (ri == 0) {
      struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = wait_ns};
      struct io_uring_cqe *cqe;
      rc = io_uring_submit_and_wait_timeout(&r->ring, &cqe, WAIT_MIN, &ts, NULL);
    } else {
      rc = io_uring_submit_and_get_events(&r->ring);
    }
    if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EBUSY && rc != -EAGAIN)
      fprintf(stderr, "io_uring_enter: %s\n", strerror(-rc));
  }
  npend = ipend = 0;
  for (int ri = 0; ri < nrings; ri++) drain(ri);
}

/* A replayed connection got its held bytes back; if its client already finished sending, it
 * closes once that request is answered (unless it was held again). */
static void finish_replay(void) {
  if (cur_replay < 0) return;
  Ring *r; int ri; uint32_t slot;
  Conn *c = conn_of(cur_replay, &r, &ri, &slot);
  cur_replay = -1;
  if (c && (c->flags & F_EOF) && !(c->flags & F_HELD)) {
    c->flags |= F_CLOSE_AFTER;
    mark(ri, slot, c);
  }
}

/* The next connection with bytes to serve: its earlier partial bytes and the new ones are
 * appended to `data`. false when the pass has nothing more. */
bool net_next(NetEv *ev, gs_rref data) {
  finish_replay();
  while (ipend < npend) {
    Pend *p = &pend[ipend++];
    Ring *r = rings[p->ring];
    Conn *c = &r->conns[p->slot];
    bool live = c->gen == p->gen && (c->flags & F_OPEN) && !(c->flags & F_CLOSING);
    if (p->kind == P_DATA) {
      const uint8_t *bytes = r->bufmem + (size_t)p->bid * BUF_SIZE;
      if (!live || (c->flags & F_CLOSE_AFTER)) { buf_return(r, p->bid); continue; }
      if (c->flags & F_HELD) {
        /* A held write is waiting at the front: everything behind it waits too. */
        buf_append(&c->partial, bytes, (size_t)p->len);
        buf_return(r, p->bid);
        continue;
      }
      if (c->partial && c->partial->len) {
        gs_bld_append(data, c->partial->d, c->partial->len);
        buf_give(&c->partial);
      }
      gs_bld_append(data, bytes, p->len);
      buf_return(r, p->bid);
      ev->conn = (int64_t)((uint64_t)p->ring << 48 | (uint64_t)p->gen << 32 | p->slot);
      ev->kind = P_DATA;
      return true;
    }
    if (p->kind == P_EOF) {
      if (!live) continue;
      if (c->flags & F_HELD) {
        c->flags |= F_EOF; /* its held write is still owed an answer */
      } else {
        c->flags |= F_CLOSE_AFTER;
        mark(p->ring, p->slot, c);
      }
      continue;
    }
    /* P_REPLAY: a write hold ended; serve what the connection kept. */
    if (!live || !(c->flags & F_HELD)) continue;
    c->flags &= ~F_HELD;
    if (c->partial && c->partial->len) gs_bld_append(data, c->partial->d, c->partial->len);
    buf_give(&c->partial);
    ev->conn = (int64_t)((uint64_t)p->ring << 48 | (uint64_t)p->gen << 32 | p->slot);
    ev->kind = P_REPLAY;
    cur_replay = ev->conn;
    return true;
  }
  for (int ri = 0; ri < nrings; ri++) buf_publish(rings[ri]);
  return false;
}

/* Appends one response (head then body) to the connection's output. */
void net_reply(int64_t id, sl_bytes head, sl_bytes body) {
  Ring *r; int ri; uint32_t slot;
  Conn *c = conn_of(id, &r, &ri, &slot);
  if (!c) return;
  mark(ri, slot, c);
  c->flags |= F_ANSWERED;
  Buf **o = &c->out;
  size_t need = (size_t)head.len + (size_t)body.len;
  if (!*o) *o = buf_take();
  if ((*o)->len + need <= (*o)->cap) {
    memcpy((*o)->d + (*o)->len, head.data, (size_t)head.len);
    memcpy((*o)->d + (*o)->len + head.len, body.data, (size_t)body.len);
    (*o)->len += (uint32_t)need;
  } else {
    buf_append(o, head.data, (size_t)head.len);
    buf_append(o, body.data, (size_t)body.len);
  }
}

/* Keeps the bytes of an incomplete request until more arrive. */
void net_keep(int64_t id, sl_bytes rest) {
  Conn *c = conn_of(id, NULL, NULL, NULL);
  if (!c) return;
  if (c->partial) c->partial->len = 0;
  buf_append(&c->partial, rest.data, (size_t)rest.len);
}

/* A write that must wait for a checkpoint hold: it and everything behind it stay unread. */
void net_hold(int64_t id, sl_bytes rest) {
  Conn *c = conn_of(id, NULL, NULL, NULL);
  if (!c) return;
  if (c->partial) c->partial->len = 0;
  buf_append(&c->partial, rest.data, (size_t)rest.len);
  if (!(c->flags & F_HELD)) {
    c->flags |= F_HELD;
    if (nheld == capheld) held = grow(held, &capheld, sizeof *held);
    held[nheld++] = (uint64_t)id;
  }
}

int64_t net_held(void) { return nheld; }

/* The hold is over: the held connections come back through net_next in this pass. */
void net_release(void) {
  for (uint32_t i = 0; i < nheld; i++) {
    uint64_t id = held[i];
    push_pend((int)(id >> 48) & 0xff, (uint32_t)id, (uint16_t)(id >> 32), P_REPLAY, 0, 0);
  }
  nheld = 0;
}

/* The connection ends once its output drains (a bad request, or the client asked). */
void net_close(int64_t id) {
  Ring *r; int ri; uint32_t slot;
  Conn *c = conn_of(id, &r, &ri, &slot);
  if (!c) return;
  c->flags |= F_CLOSE_AFTER;
  if (c->partial) buf_give(&c->partial);
  mark(ri, slot, c);
}

static const char ERR500[] =
    "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n"
    "{\"error\":\"internal server error\"}";

/* After the pass's COMMIT: every response it produced goes to the kernel. If the commit failed,
 * no response from this pass may claim success: they become 500s and the connections close. */
void net_flush(bool committed) {
  for (uint32_t i = 0; i < ntouched; i++) {
    int ri = (int)(touched[i] >> 32);
    uint32_t slot = (uint32_t)touched[i];
    Ring *r = rings[ri];
    Conn *c = &r->conns[slot];
    if (!committed && (c->flags & F_ANSWERED) && c->out) {
      c->out->len = c->mark;
      buf_append(&c->out, ERR500, sizeof ERR500 - 1);
      c->flags |= F_CLOSE_AFTER;
    }
    c->flags &= ~(F_IN_BATCH | F_ANSWERED);
    if (!(c->flags & F_OPEN) || (c->flags & (F_CLOSING | F_SENDING))) continue;
    if (c->out && c->out->len) {
      c->wire = c->out;
      c->out = NULL;
      c->sent = 0;
      c->flags |= F_SENDING;
      arm_send(r, ri, slot, c);
    } else if (c->flags & F_CLOSE_AFTER) {
      arm_close(r, ri, slot, c);
    }
  }
  ntouched = 0;
  for (int ri = 0; ri < nrings; ri++) {
    Ring *r = rings[ri];
    for (uint32_t i = 0; i < r->nrearm; i++) {
      uint32_t slot = r->rearm[i];
      Conn *c = &r->conns[slot];
      if ((c->flags & F_OPEN) && !(c->flags & F_CLOSING)) arm_recv(r, ri, slot, c->gen);
    }
    r->nrearm = 0;
  }
  if (need_ring) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > spill_after.tv_sec ||
        (now.tv_sec == spill_after.tv_sec && now.tv_nsec >= spill_after.tv_nsec)) {
      /* Retried once a second until some ring accepts again, so accepting never stops. */
      need_ring = !spill();
      if (need_ring) { spill_after = now; spill_after.tv_sec += 1; }
    }
  }
}

/* ---- startup ---- */

static int listener(uint32_t ip, int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) return -errno;
  /* Accepted sockets inherit TCP_NODELAY. */
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = ip;
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) return -errno;
  if (listen(fd, 65535) < 0) return -errno;
  return fd;
}

/* Raises the soft descriptor limit to the hard one: a ring's file table may be that large. */
static uint32_t descriptor_slots(void) {
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  uint64_t n = rl.rlim_cur;
  if (n > (1u << 20)) n = 1u << 20;
  const char *o = getenv("GOOSE_SLOTS_OVERRIDE"); /* test-only: tiny tables exercise ring spills */
  if (o && atoi(o) > 64) n = (uint64_t)atoi(o);
  return (uint32_t)n;
}

/* Locks the executable's own file-backed pages (its code and read-only data) in RAM. With no
 * swap, reclaim near the memory wall takes file-backed pages first, and the loop would then stall
 * on major faults reading its own code back from disk. The binary is static, so this is a couple
 * of megabytes, well inside systemd's 8 MB RLIMIT_MEMLOCK. Best effort. */
static void lock_code(void) {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return;
  exe[n] = 0;
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char line[4096 + 128];
  size_t locked = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long lo, hi;
    char perms[8];
    int off = 0;
    if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &off) < 3 || !off) continue;
    char *path = line + off;
    path[strcspn(path, "\n")] = 0;
    if (strcmp(path, exe) != 0 || perms[0] != 'r' || perms[1] == 'w') continue;
    if (mlock((void *)lo, hi - lo) == 0) locked += hi - lo;
  }
  fclose(f);
  fprintf(stderr, "locked %zu KB of code in memory\n", locked >> 10);
}

/* 0 on success, or a negative errno. */
int64_t net_init(sl_bytes host, int64_t port) {
  signal(SIGPIPE, SIG_IGN);
  char h[64];
  if (host.len <= 0 || host.len >= (int64_t)sizeof h) return -EINVAL;
  memcpy(h, host.data, (size_t)host.len);
  h[host.len] = 0;
  struct in_addr ip;
  if (inet_pton(AF_INET, h, &ip) != 1) return -EINVAL;
  slots_per_ring = descriptor_slots();
  const char *w = getenv("GOOSE_WAIT_US");
  if (w && atol(w) > 0 && atol(w) < 100000) wait_ns = atol(w) * 1000;
  for (int i = 0; i < LISTENERS; i++) {
    int fd = listener(ip.s_addr, (int)port);
    if (fd < 0) return fd;
    listen_fd[i] = fd;
  }
  int err = 0;
  Ring *r = ring_new(slots_per_ring, &err);
  if (!r) return -err;
  rings[nrings++] = r;
  arm_accept(r, 0);
  lock_code();
  fprintf(stderr, "listening on %s:%lld; io_uring, %u direct descriptors per ring\n", h,
          (long long)port, slots_per_ring);
  return 0;
}
