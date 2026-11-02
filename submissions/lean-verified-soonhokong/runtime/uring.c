/* io_uring transport: moves bytes between sockets and the Lean request handler, nothing else.
 *
 * One thread drives several rings. Each ring owns a SO_REUSEPORT listener, a table of direct
 * (fixed) socket descriptors and a ring of provided receive buffers. A ring's descriptor table
 * may not exceed RLIMIT_NOFILE (65,535 on the box), and direct descriptors do not use the
 * process file table, so the rings together hold rings x 65,535 connections. Rings use
 * DEFER_TASKRUN: completions are produced only when this thread enters the ring, so each loop
 * iteration collects a batch of input from every connection that has some.
 *
 * Per iteration: reap completions; hand every connection's new input to Lean in one call
 * (Feed.Server.batch), which parses it, answers the complete requests and reports how many bytes
 * it consumed; queue the answers as sends. The transport never parses HTTP. */
#include "rt.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif

#define MAX_RINGS 16
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 65536   /* more than a ring's connections: a burst never spills into the kernel's
                              overflow list, which allocates per completion when memory is shortest */
#define NBUF 512           /* provided receive buffers per ring: reads are issued for at most this many */
#define BUFSZ 1024
#define READ_ROUNDS 8      /* read rounds per iteration (each up to NBUF reads per ring) */
#define CQE_BUDGET 8192    /* completions handled per ring per iteration */
#define MAX_IN 65536       /* a connection's buffered, unanswered input */

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_SHUTDOWN, OP_PROVIDE, OP_POLL, OP_POLL_REMOVE };
enum { C_FREE = 0, C_OPEN, C_CLOSING };
enum { F_POLL = 1, F_SENDING = 2, F_CLOSE_AFTER = 4, F_HELD = 8, F_PEER_EOF = 16, F_SPILL = 32, F_RQ = 64 };

typedef struct oq { struct oq *next; lean_object *buf; u32_t off, len; } oq;

typedef struct conn {
  u32_t gen;
  u8_t state, flags;
  u16_t _pad;
  u32_t in_len, in_cap;
  i32_t chunk;              /* index in the current batch, or -1 */
  u8_t *in;                 /* input not yet consumed by the handler */
  lean_object *out;         /* the response being sent (owned reference) */
  u32_t out_off, out_len;
  oq *q;                    /* responses waiting for the in-flight send */
} conn;

typedef struct ring {
  int fd, lfd, idx, reg_idx;
  u32_t *sq_khead, *sq_ktail, *sq_kflags, *sq_array, sq_mask, sq_entries, sq_tail, to_submit;
  struct io_uring_sqe *sqes;
  u32_t *cq_khead, *cq_ktail, cq_mask;
  struct io_uring_cqe *cqes;
  struct io_uring_buf_ring *br;   /* provided buffer ring, or 0 when using PROVIDE_BUFFERS */
  u16_t br_tail;
  u8_t *bufs;
  u32_t nslots, nopen;
  conn *conns;
  u8_t hot, accept_armed;
  u16_t nrecycle;                 /* PROVIDE_BUFFERS mode: buffers to give back */
  u16_t *recycle;
  u32_t *rq, rq_head, nrq;        /* connections reported readable, waiting for a read (FIFO) */
  u32_t reads_out;                /* reads submitted and not completed: each may hold a buffer */
} ring;

static ring rings[MAX_RINGS];
static int nrings;
static int epfd, evfd;
static u64_t start_s;
static lean_object *g_secret;
static u64_t st_batches, st_reqchunks, st_iters, st_enters, st_sleeps;
static u64_t st_nobufs, st_rearms, st_max_chunks, st_iter_max_ns, st_batch_ns, st_reap_ns, st_overflow;

/* implemented in Lean: Feed.Server.batch */
extern lean_object *feed_batch(lean_object *chunks, uint64_t now, uint64_t uptime, uint8_t writes_ok, lean_object *key);
extern lean_object *feed_make_key(lean_object *secret);

static inline int sys_setup(u32_t e, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, e, p); }
static inline int sys_enter(ring *r, u32_t sub, u32_t minc, u32_t flags) {
  if (r->reg_idx >= 0) return (int)syscall(__NR_io_uring_enter, r->reg_idx, sub, minc, flags | IORING_ENTER_REGISTERED_RING, NULL, 0);
  return (int)syscall(__NR_io_uring_enter, r->fd, sub, minc, flags, NULL, 0);
}
static inline int sys_register(int fd, u32_t op, void *arg, u32_t n) { return (int)syscall(__NR_io_uring_register, fd, op, arg, n); }

static inline u64_t ud(u8_t op, u8_t ri, u32_t gen, u32_t slot) { return (u64_t)op << 56 | (u64_t)ri << 48 | (u64_t)(gen & 0xffff) << 32 | slot; }

static void ring_flush(ring *r) {
  while (r->to_submit) {
    int rc = sys_enter(r, r->to_submit, 0, IORING_ENTER_GETEVENTS);
    if (rc < 0) { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EBUSY) return; perror("io_uring_enter"); exit(1); }
    r->to_submit -= (u32_t)rc;
    if (rc == 0) return;
  }
}
static struct io_uring_sqe *sqe_get(ring *r) {
  u32_t head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  if (r->sq_tail - head >= r->sq_entries) {
    ring_flush(r);
    head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
    if (r->sq_tail - head >= r->sq_entries) { fprintf(stderr, "fatal: submission queue full\n"); exit(1); }
  }
  struct io_uring_sqe *s = &r->sqes[r->sq_tail & r->sq_mask];
  memset(s, 0, sizeof *s);
  r->sq_tail++;
  r->to_submit++;
  __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
  return s;
}

/* ---- buffers ---------------------------------------------------------------------------- */
static inline void buf_recycle(ring *r, u16_t bid) {
  if (r->br) {
    struct io_uring_buf *b = &r->br->bufs[r->br_tail & (NBUF - 1)];
    b->addr = (u64_t)(uintptr_t)(r->bufs + (size_t)bid * BUFSZ);
    b->len = BUFSZ;
    b->bid = bid;
    r->br_tail++;
  } else {
    r->recycle[r->nrecycle++] = bid;
  }
}
static void buf_publish(ring *r) {
  if (r->br) { __atomic_store_n(&r->br->tail, r->br_tail, __ATOMIC_RELEASE); return; }
  /* PROVIDE_BUFFERS: give back runs of consecutive buffer ids */
  u16_t n = r->nrecycle;
  r->nrecycle = 0;
  for (u16_t i = 0; i < n;) {
    u16_t j = i + 1;
    while (j < n && r->recycle[j] == r->recycle[j - 1] + 1) j++;
    struct io_uring_sqe *s = sqe_get(r);
    s->opcode = IORING_OP_PROVIDE_BUFFERS;
    s->fd = (int)(j - i);
    s->addr = (u64_t)(uintptr_t)(r->bufs + (size_t)r->recycle[i] * BUFSZ);
    s->len = BUFSZ;
    s->off = r->recycle[i];
    s->buf_group = 0;
    s->flags = IOSQE_CQE_SKIP_SUCCESS;
    s->user_data = ud(OP_PROVIDE, (u8_t)r->idx, 0, 0);
    i = j;
  }
}

/* ---- connections ------------------------------------------------------------------------ */
static void arm_accept(ring *r) {
  if (r->nopen + 16 >= r->nslots) return;   /* table nearly full: accept again after closes */
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_ACCEPT;
  s->fd = r->lfd;
  s->ioprio = IORING_ACCEPT_MULTISHOT;
  s->file_index = IORING_FILE_INDEX_ALLOC;
  s->user_data = ud(OP_ACCEPT, (u8_t)r->idx, 0, 0);
  r->accept_armed = 1;
}
/* Reads. An idle connection waits on a multishot poll, which holds no buffer (and less kernel
 * memory than an armed multishot receive: on a 2 GB machine the kernel's memory per connection
 * is what limits the number of users). When the poll reports it readable it joins its ring's read
 * queue; after the completions of an iteration are in, one read is issued per queued connection,
 * never more on a ring than it has buffers, and the reads complete during the submit. A burst of
 * any size waits in the read queue rather than in the kernel's receive path. */
static void arm_poll(ring *r, u32_t slot) {
  conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_POLL_ADD;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE;
  s->len = IORING_POLL_ADD_MULTI;
  s->poll32_events = POLLIN | POLLRDHUP;
  s->user_data = ud(OP_POLL, (u8_t)r->idx, c->gen, slot);
  c->flags |= F_POLL;
}
/* The queue holds each open connection at most once (F_RQ); a slot closed while queued leaves
 * its entry behind (skipped, or a harmless read for the slot's next connection), so the queue has
 * room for two entries per slot. */
static inline void rq_push(ring *r, u32_t slot) {
  conn *c = &r->conns[slot];
  if ((c->flags & F_RQ) || r->nrq >= 2 * r->nslots) return;
  c->flags |= F_RQ;
  r->rq[(r->rq_head + r->nrq++) % (2 * r->nslots)] = slot;
}
static void submit_read(ring *r, u32_t slot) {
  conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_RECV;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  s->len = BUFSZ;
  s->msg_flags = MSG_DONTWAIT;
  s->user_data = ud(OP_RECV, (u8_t)r->idx, c->gen, slot);
  r->reads_out++;
}
/* one read for each queued connection while the ring has buffers; returns how many */
static u32_t issue_reads(ring *r) {
  u32_t n = 0;
  while (r->nrq && r->reads_out < NBUF) {
    u32_t slot = r->rq[r->rq_head];
    r->rq_head = (r->rq_head + 1) % (2 * r->nslots);
    r->nrq--;
    conn *c = &r->conns[slot];
    c->flags &= ~F_RQ;
    if (c->state != C_OPEN) continue;
    submit_read(r, slot);
    n++;
  }
  return n;
}
static void submit_send(ring *r, u32_t slot) {
  conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_SEND;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE;
  s->addr = (u64_t)(uintptr_t)(lean_sarray_cptr(c->out) + c->out_off);
  s->len = c->out_len;
  s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
  s->user_data = ud(OP_SEND, (u8_t)r->idx, c->gen, slot);
  c->flags |= F_SENDING;
}
static void conn_release_output(conn *c) {
  if (c->out) { lean_dec(c->out); c->out = 0; }
  for (oq *q = c->q; q;) { oq *n = q->next; lean_dec(q->buf); free(q); q = n; }
  c->q = 0;
}
static void conn_free(ring *r, conn *c);

/* Close: remove the connection's poll first while it is armed (a request on a direct descriptor
 * keeps the socket open after its slot is closed), then release the direct descriptor. */
static void conn_close(ring *r, u32_t slot) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) return;
  if (c->flags & F_SENDING) { c->flags |= F_CLOSE_AFTER; return; }
  c->state = C_CLOSING;
  struct io_uring_sqe *s;
  if (c->flags & F_POLL) {
    s = sqe_get(r);
    s->opcode = IORING_OP_POLL_REMOVE;
    s->addr = ud(OP_POLL, (u8_t)r->idx, c->gen, slot);
    s->flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
    s->user_data = ud(OP_POLL_REMOVE, (u8_t)r->idx, c->gen, slot);
  }
  s = sqe_get(r);
  s->opcode = IORING_OP_CLOSE;
  s->file_index = slot + 1;
  s->flags = IOSQE_CQE_SKIP_SUCCESS;
  s->user_data = ud(OP_CLOSE, (u8_t)r->idx, c->gen, slot);
  /* the slot is reused only after the kernel released it: an accept into it reports the new
   * socket, so freeing the record now is safe (stale completions carry the old generation) */
  conn_free(r, c);
}
static void conn_free(ring *r, conn *c) {
  conn_release_output(c);
  free(c->in);
  c->in = 0;
  c->in_len = c->in_cap = 0;
  c->flags = 0;
  c->chunk = -1;
  c->state = C_FREE;
  c->gen++;
  r->nopen--;
  if (!r->accept_armed) arm_accept(r);
}
static void conn_send(ring *r, u32_t slot, lean_object *buf, u32_t off, u32_t len) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN || !len) { lean_dec(buf); return; }
  if (!(c->flags & F_SENDING)) {
    c->out = buf; c->out_off = off; c->out_len = len;
    submit_send(r, slot);
    return;
  }
  oq *q = malloc(sizeof *q);
  if (!q) abort();
  q->next = 0; q->buf = buf; q->off = off; q->len = len;
  oq **t = &c->q;
  while (*t) t = &(*t)->next;
  *t = q;
}

/* ---- the batch ---------------------------------------------------------------------------- */
#define MAX_CHUNKS 65536
static lean_object *chunk_arr;              /* Array ByteArray being built */
static u32_t chunk_ref[MAX_CHUNKS];         /* ring << 16 | slot */
static u32_t chunk_gen[MAX_CHUNKS];
static u32_t nchunks;
static u32_t *held;                         /* these lists hold each connection at most once: */
static u32_t nheld;
static u32_t *rearm;                        /* sized for every connection slot at startup */
static u32_t nrearm;
/* A batch takes at most max_batch connections; input beyond that waits, in arrival order, for
 * the next iterations. Capping it lower (FEED_MAX_BATCH, for measurements) made a saturated
 * server slower to catch up, so by default a batch takes everything that arrived. */
static u32_t max_batch = MAX_CHUNKS;
/* Under a backlog a batch is capped so that it takes about BATCH_TARGET_NS: the input that arrived
 * since the last iteration goes first, the backlog (connections that did not fit before, oldest
 * first) fills what is left. New requests are then answered within an iteration or two of a few
 * milliseconds, before the kernel's delayed-ACK timer (40 ms) would add a packet to each of them,
 * so a short overload (the end of a ramp of new users) does not turn into a slower steady state;
 * the backlog drains with the capacity left over. Without a backlog batches are far below the cap.
 * FEED_BATCH_TARGET_US overrides the target (0: no cap). */
static u64_t BATCH_TARGET_NS = 6000000ull;
static u32_t batch_cap = MAX_CHUNKS;
static u64_t per_chunk_ns;   /* running estimate of a batch's time per connection */
static u32_t *spilled, *snap;
static u32_t nspilled;

static inline u32_t cref(int ri, u32_t slot) { return (u32_t)ri << 16 | slot; }

/* the connection's input for this batch: a Lean ByteArray holding its unconsumed bytes */
static lean_object **chunk_slot(ring *r, u32_t slot) {
  conn *c = &r->conns[slot];
  if (c->chunk < 0) {
    if (nchunks >= batch_cap) return 0;
    lean_object *b = lean_alloc_sarray(1, 0, c->in_len + 512);
    if (c->in_len) { memcpy(lean_sarray_cptr(b), c->in, c->in_len); lean_to_sarray(b)->m_size = c->in_len; }
    /* the buffer only carries input from one batch to the next; kept for the connection's
     * lifetime (one for every connection that ever waited, which includes every writer outside
     * a write window) these buffers grew the heap by about 600 bytes per connection over a run */
    free(c->in);
    c->in = 0;
    c->in_len = c->in_cap = 0;
    chunk_arr = lean_array_push(chunk_arr, b);
    c->chunk = (i32_t)nchunks;
    chunk_ref[nchunks] = cref(r->idx, slot);
    chunk_gen[nchunks] = c->gen;
    nchunks++;
  }
  return &lean_to_array(chunk_arr)->m_data[c->chunk];
}
static void on_data(ring *r, u32_t slot, const u8_t *p, u32_t n) {
  conn *c = &r->conns[slot];
  lean_object **b = chunk_slot(r, slot);
  if (!b) {   /* batch full: keep the bytes for the next one */
    if (c->in_len + n > MAX_IN) { conn_close(r, slot); return; }
    if (c->in_len + n > c->in_cap) {
      u32_t cap = (c->in_len + n) * 2;
      u8_t *m = realloc(c->in, cap);
      if (!m) abort();
      c->in = m; c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, p, n);
    c->in_len += n;
    if (!(c->flags & F_SPILL)) { c->flags |= F_SPILL; spilled[nspilled++] = cref(r->idx, slot); }
    return;
  }
  if (lean_sarray_size(*b) + n > MAX_IN) { conn_close(r, slot); return; }
  *b = ba_reserve(*b, n);
  memcpy(ba_end(*b), p, n);
  ba_grow(*b, n);
}

static void handle_cqe(ring *r, struct io_uring_cqe *e) {
  u64_t u = e->user_data;
  u8_t op = (u8_t)(u >> 56);
  u16_t gen = (u16_t)(u >> 32);
  u32_t slot = (u32_t)u;
  int res = e->res;
  switch (op) {
  case OP_ACCEPT:
    if (res >= 0) {
      if ((u32_t)res >= r->nslots) return;
      conn *c = &r->conns[res];
      if (c->state != C_FREE) { /* cannot happen: the kernel allocated a free slot */ }
      c->state = C_OPEN;
      c->flags = 0;
      c->chunk = -1;
      r->nopen++;
      arm_poll(r, (u32_t)res);
    } else if (res != -ECANCELED && res != -EINTR) {
      static time_t last;
      if (time(0) != last) { last = time(0); fprintf(stderr, "accept: %s\n", strerror(-res)); }
    }
    if (!(e->flags & IORING_CQE_F_MORE)) r->accept_armed = 0;
    return;
  case OP_POLL: {
    conn *c = &r->conns[slot];
    if ((u16_t)c->gen != gen || c->state != C_OPEN) return;
    if (!(e->flags & IORING_CQE_F_MORE)) {   /* the poll ended: arm it again after this batch */
      c->flags &= ~F_POLL;
      if (res != -ECANCELED) rearm[nrearm++] = cref(r->idx, slot);
    }
    if (res > 0) rq_push(r, slot);
    return;
  }
  case OP_RECV: {
    conn *c = &r->conns[slot];
    int has_buf = (e->flags & IORING_CQE_F_BUFFER) != 0;
    u16_t bid = (u16_t)(e->flags >> IORING_CQE_BUFFER_SHIFT);
    if (r->reads_out) r->reads_out--;
    if ((u16_t)c->gen != gen || c->state != C_OPEN) { if (has_buf) buf_recycle(r, bid); return; }
    if (res > 0) {
      on_data(r, slot, r->bufs + (size_t)bid * BUFSZ, (u32_t)res);
      buf_recycle(r, bid);
      /* a full buffer may have left more in the socket: read again */
      if (res == BUFSZ && c->state == C_OPEN) rq_push(r, slot);
    } else {
      if (has_buf) buf_recycle(r, bid);
      if (res == -EAGAIN) {}             /* nothing to read after all */
      else if (res == -ENOBUFS) { st_nobufs++; rq_push(r, slot); }
      else if (res == 0) {
        /* the peer finished sending; answer what it sent, then close */
        c->flags |= F_PEER_EOF;
        if (c->chunk < 0 && !(c->flags & F_SENDING) && !c->in_len) conn_close(r, slot);
      } else conn_close(r, slot);
    }
    return;
  }
  case OP_SEND: {
    conn *c = &r->conns[slot];
    if ((u16_t)c->gen != gen || c->state == C_FREE) return;
    c->flags &= ~F_SENDING;
    if (res > 0 && (u32_t)res < c->out_len) {   /* short send: continue with the rest */
      c->out_off += (u32_t)res;
      c->out_len -= (u32_t)res;
      submit_send(r, slot);
      return;
    }
    if (c->out) { lean_dec(c->out); c->out = 0; }
    if (res < 0) { conn_release_output(c); c->flags &= ~F_CLOSE_AFTER; conn_close(r, slot); return; }
    if (c->q) {
      oq *q = c->q;
      c->q = q->next;
      c->out = q->buf; c->out_off = q->off; c->out_len = q->len;
      free(q);
      submit_send(r, slot);
      return;
    }
    if (c->flags & (F_CLOSE_AFTER | F_PEER_EOF)) { c->flags &= ~F_CLOSE_AFTER; if (c->chunk < 0 && !(c->flags & (F_HELD | F_SPILL))) conn_close(r, slot); }
    return;
  }
  case OP_CLOSE: {
    conn *c = &r->conns[slot];
    if ((u16_t)c->gen == gen && c->state == C_CLOSING) conn_free(r, c);
    if (!r->accept_armed) arm_accept(r);
    return;
  }
  case OP_SHUTDOWN:
  case OP_PROVIDE:
  case OP_POLL_REMOVE:
    return;
  }
}

static int reap(ring *r) {
  u32_t head = *r->cq_khead, tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
  int n = 0;
  while (head != tail && n < CQE_BUDGET) {
    handle_cqe(r, &r->cqes[head & r->cq_mask]);
    head++;
    n++;
    if (head == tail) tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
  }
  __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE);
  return n;
}

/* ---- setup ---------------------------------------------------------------------------------- */
static int listen_socket(const char *host, int port) {
  struct sockaddr_storage ss;
  socklen_t sl;
  memset(&ss, 0, sizeof ss);
  struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
  struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
  int fam;
  if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) { fam = AF_INET; a4->sin_family = AF_INET; a4->sin_port = htons((u16_t)port); sl = sizeof *a4; }
  else if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) { fam = AF_INET6; a6->sin6_family = AF_INET6; a6->sin6_port = htons((u16_t)port); sl = sizeof *a6; }
  else if (!strcmp(host, "localhost")) { fam = AF_INET; a4->sin_family = AF_INET; a4->sin_port = htons((u16_t)port); a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); sl = sizeof *a4; }
  else { fprintf(stderr, "fatal: HOST %s is not an IP address\n", host); exit(1); }
  int fd = socket(fam, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) { perror("socket"); exit(1); }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* inherited by accepted sockets */
  if (bind(fd, (struct sockaddr *)&ss, sl) < 0) { perror("bind"); exit(1); }
  if (listen(fd, 65535) < 0) { perror("listen"); exit(1); }
  return fd;
}

/* Kernels newer than the box's 6.8 charge ring memory to RLIMIT_MEMLOCK (8 MB under systemd and
 * on CI runners), where eight rings of this size do not fit: on ENOMEM a ring is made with half
 * the entries, down to an eighth, and if a later ring cannot be made at all the server runs on the
 * rings it has. Returns 0, or -1 when this ring could not be made. */
static int ring_init(ring *r, int idx, u32_t slots, const char *host, int port) {
  struct io_uring_params p;
  int fd = -1;
  for (u32_t div = 1; div <= 8 && fd < 0; div *= 2) {
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL |
              IORING_SETUP_TASKRUN_FLAG;
    p.cq_entries = CQ_ENTRIES / div;
    fd = sys_setup(SQ_ENTRIES / div, &p);
    if (fd < 0 && errno != ENOMEM) break;
  }
  if (fd < 0) {
    if (idx == 0) { perror("io_uring_setup"); exit(1); }
    fprintf(stderr, "transport: ring %d not made (%s); serving on %d rings\n", idx, strerror(errno), idx);
    return -1;
  }
  r->fd = fd;
  r->idx = idx;
  r->reg_idx = -1;
  size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(u32_t);
  size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  if (cq_sz > sq_sz) sq_sz = cq_sz;
  u8_t *m = mmap(0, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  if (m == MAP_FAILED) { perror("mmap sq"); exit(1); }
  r->sq_khead = (u32_t *)(m + p.sq_off.head);
  r->sq_ktail = (u32_t *)(m + p.sq_off.tail);
  r->sq_kflags = (u32_t *)(m + p.sq_off.flags);
  r->sq_mask = *(u32_t *)(m + p.sq_off.ring_mask);
  r->sq_entries = *(u32_t *)(m + p.sq_off.ring_entries);
  r->sq_array = (u32_t *)(m + p.sq_off.array);
  for (u32_t i = 0; i < r->sq_entries; i++) r->sq_array[i] = i;
  r->sq_tail = *r->sq_ktail;
  r->cq_khead = (u32_t *)(m + p.cq_off.head);
  r->cq_ktail = (u32_t *)(m + p.cq_off.tail);
  r->cq_mask = *(u32_t *)(m + p.cq_off.ring_mask);
  r->cqes = (struct io_uring_cqe *)(m + p.cq_off.cqes);
  r->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  if (r->sqes == MAP_FAILED) { perror("mmap sqes"); exit(1); }
  /* use a registered ring descriptor for io_uring_enter */
  struct io_uring_rsrc_update up = {.offset = -1U, .data = (u64_t)fd};
  if (getenv("FEED_REG_RING") && sys_register(fd, IORING_REGISTER_RING_FDS, &up, 1) == 1) r->reg_idx = (int)up.offset;
  /* direct descriptor table */
  struct io_uring_rsrc_register rr;
  memset(&rr, 0, sizeof rr);
  rr.nr = slots;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  if (sys_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr) < 0) { perror("register files"); exit(1); }
  r->nslots = slots;
  r->conns = calloc(slots, sizeof(conn));
  r->rq = malloc(2 * slots * sizeof(u32_t));
  if (!r->conns || !r->rq) abort();
  for (u32_t i = 0; i < slots; i++) r->conns[i].chunk = -1;
  /* receive buffers: a provided-buffer ring, else the older PROVIDE_BUFFERS operation */
  r->bufs = mmap(0, (size_t)NBUF * BUFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->bufs == MAP_FAILED) { perror("mmap bufs"); exit(1); }
  r->br = mmap(0, NBUF * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->br == MAP_FAILED) { perror("mmap buf ring"); exit(1); }
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (u64_t)(uintptr_t)r->br;
  reg.ring_entries = NBUF;
  reg.bgid = 0;
  int brc = getenv("FEED_NO_PBUF") ? (errno = ENOSYS, -1) : sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1);   /* FEED_NO_PBUF: for measurements */
  if (brc < 0 && errno == EINVAL) {
    /* Ubuntu's 6.8 kernels from 6.8.0-136 (and the cloud-image kernel 6.8.0-1067) invert the check of the reserved
     * fields and reject zero ones (Launchpad #2162843, found by c-iouring-pavlyhalim #245); they
     * accept a nonzero reserved byte, and a correct kernel never gets here. */
    reg.resv[0] = 1;
    brc = sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  }
  if (brc < 0) {
    munmap(r->br, NBUF * sizeof(struct io_uring_buf));
    r->br = 0;
    r->recycle = malloc(sizeof(u16_t) * NBUF);
    if (!r->recycle) abort();
    if (idx == 0) fprintf(stderr, "transport: provided-buffer rings unavailable (%s), using PROVIDE_BUFFERS\n", strerror(errno));
  }
  r->br_tail = 0;
  for (u32_t i = 0; i < NBUF; i++) buf_recycle(r, (u16_t)i);
  buf_publish(r);
  /* one eventfd for all rings: with DEFER_TASKRUN it fires when a ring gets work */
  if (sys_register(fd, IORING_REGISTER_EVENTFD, &evfd, 1) < 0) { perror("register eventfd"); exit(1); }
  r->lfd = listen_socket(host, port);
  arm_accept(r);
  r->hot = 1;
  return 0;
}

/* Termination. A listener with an accept still pending in a ring can outlive the process on some
 * kernels and keep taking connections from the next instance (SO_REUSEPORT), so on SIGTERM/SIGINT
 * the listeners are shut down and every connection closed before exiting. */
static volatile sig_atomic_t stop_requested;
static void on_stop(int sig) { (void)sig; stop_requested = 1; }
static void transport_shutdown(void) {
  for (int i = 0; i < nrings; i++) {
    shutdown(rings[i].lfd, SHUT_RDWR);
    close(rings[i].lfd);
    sys_register(rings[i].fd, IORING_UNREGISTER_FILES, 0, 0);
  }
  fprintf(stderr, "transport: stopped\n");
}

static inline int ring_pending(ring *r) {
  return r->to_submit || r->hot || r->nrq ||
         (__atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) ||
         *r->cq_khead != __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
}

static u64_t now_unix(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (u64_t)ts.tv_sec;
}
static u64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64_t)ts.tv_sec * 1000000000ull + (u64_t)ts.tv_nsec;
}
static int writes_ok = 1;

/* Hand the batch to Lean and deliver its replies. */
static void run_batch(void) {
  if (!nchunks) return;
  u64_t now = now_unix();
  lean_object *arr = chunk_arr;
  chunk_arr = lean_alloc_array(0, 256);
  lean_inc(g_secret);
  lean_inc(arr);   /* read below, after the handler consumed its reference */
  lean_object *res = feed_batch(arr, now, now - start_s, (uint8_t)writes_ok, g_secret);
  if (!lean_io_result_is_ok(res)) { lean_io_result_show_error(res); exit(1); }
  lean_object *replies = lean_io_result_get_value(res);
  st_batches++;
  st_reqchunks += nchunks;
  if (nchunks > st_max_chunks) st_max_chunks = nchunks;
  size_t nrep = lean_array_size(replies);
  for (u32_t i = 0; i < nchunks; i++) {
    ring *r = &rings[chunk_ref[i] >> 16];
    u32_t slot = chunk_ref[i] & 0xffff;
    conn *c = &r->conns[slot];
    if (c->gen != chunk_gen[i] || c->state != C_OPEN) continue;
    c->chunk = -1;
    if (i >= nrep) { conn_close(r, slot); continue; }
    lean_object *rep = lean_array_get_core(replies, i);
    lean_object *buf = lean_ctor_get(rep, 0);
    u64_t start = nat_u64(lean_ctor_get(rep, 1)), consumed = nat_u64(lean_ctor_get(rep, 2)), flags = nat_u64(lean_ctor_get(rep, 3));
    lean_object *input = lean_array_get_core(arr, i);
    size_t ilen = lean_sarray_size(input);
    if (consumed > ilen) consumed = ilen;
    /* keep what the handler did not consume */
    size_t rest = ilen - (size_t)consumed;
    if (rest) {
      if (rest > MAX_IN) { conn_close(r, slot); continue; }
      if (rest > c->in_cap) { u8_t *m = realloc(c->in, rest + 256); if (!m) abort(); c->in = m; c->in_cap = (u32_t)rest + 256; }
      memcpy(c->in, lean_sarray_cptr(input) + consumed, rest);
      c->in_len = (u32_t)rest;
    }
    size_t blen = lean_sarray_size(buf);
    if (start < blen) { lean_inc(buf); conn_send(r, slot, buf, (u32_t)start, (u32_t)(blen - start)); }
    if (flags & 1) { conn_close(r, slot); continue; }
    if ((flags & 2) && !(c->flags & F_HELD)) { c->flags |= F_HELD; held[nheld++] = cref(r->idx, slot); }
    if ((c->flags & F_PEER_EOF) && !(c->flags & (F_HELD | F_SPILL | F_SENDING))) conn_close(r, slot);
  }
  lean_dec(res);
  lean_dec(arr);
  nchunks = 0;
}

/* Connections whose input did not fit in the previous batch come first in this one. */
static void present_spill(void) {
  u32_t n = nspilled;
  memcpy(snap, spilled, n * sizeof(u32_t));
  nspilled = 0;
  for (u32_t i = 0; i < n; i++) {
    ring *r = &rings[snap[i] >> 16];
    u32_t slot = snap[i] & 0xffff;
    conn *c = &r->conns[slot];
    if (c->state != C_OPEN || !(c->flags & F_SPILL)) continue;
    if (nchunks >= batch_cap) { spilled[nspilled++] = snap[i]; continue; }
    c->flags &= ~F_SPILL;
    if (c->in_len) chunk_slot(r, slot);
  }
}

/* Connections whose handler deferred complete requests (they follow a write in the same input)
 * get their remaining input again in the next batch, after the write committed. */
static void present_held(void) {
  if (!writes_ok) return;   /* held requests wait for writes to resume */
  u32_t n = nheld;
  memcpy(snap, held, n * sizeof(u32_t));
  nheld = 0;
  for (u32_t i = 0; i < n; i++) {
    ring *r = &rings[snap[i] >> 16];
    u32_t slot = snap[i] & 0xffff;
    conn *c = &r->conns[slot];
    if (c->state != C_OPEN || !(c->flags & F_HELD)) continue;
    c->flags &= ~F_HELD;
    /* no room in this batch (a capped batch under backlog): stay held for the next one */
    if (c->in_len && !chunk_slot(r, slot)) { c->flags |= F_HELD; held[nheld++] = snap[i]; }
  }
}

/* Batching: while busy, an iteration starts at most every period_us; if one finished early, the
 * loop sleeps for the rest of the period so that the next one serves a batch (one wakeup, one
 * transaction, one Lean call and one burst of sends for many requests). An iteration that took
 * longer than the period continues at once, so no time is lost when the CPU is saturated.
 * FEED_PERIOD_US overrides it (for measurements). */
static long period_us = 2000;

/* Writes go in at most one batch every write_window_us: a write waits for that batch (reads do
 * not), so a commit carries the writes of several periods and the pages they share (the tail of
 * likes, the head of the feed index, ...) go to the log once instead of once per period.
 * FEED_WRITE_WINDOW_US overrides it. */
static long write_window_us = 10000;
static u64_t last_write_batch;

static void stats_tick(void) {
  static u64_t last;
  static int every = -1;
  if (every < 0) { const char *e = getenv("FEED_STATS"); every = e ? atoi(e) : 0; }
  if (!every) return;
  u64_t now = now_unix();
  if (now - last < (u64_t)every) return;
  if (last) {
    u64_t open = 0;
    for (int i = 0; i < nrings; i++) open += rings[i].nopen;
    fprintf(stderr, "stats: open %llu batches %llu chunks/batch %.1f max %llu iters %llu enters %llu sleeps %llu wal %d pauses %llu"
            " (timeouts %llu) spill %u | iter max %.1f ms, batch %.0f ms, reap %.0f ms, nobufs %llu rearms %llu overflow %llu\n", (unsigned long long)open,
            (unsigned long long)st_batches, st_batches ? (double)st_reqchunks / (double)st_batches : 0, (unsigned long long)st_max_chunks,
            (unsigned long long)st_iters, (unsigned long long)st_enters, (unsigned long long)st_sleeps, wal_frames,
            (unsigned long long)st_pauses, (unsigned long long)st_pause_timeouts, nspilled, (double)st_iter_max_ns / 1e6, (double)st_batch_ns / 1e6, (double)st_reap_ns / 1e6,
            (unsigned long long)st_nobufs, (unsigned long long)st_rearms, (unsigned long long)st_overflow);
  }
  last = now;
  st_batches = st_reqchunks = st_iters = st_enters = st_sleeps = 0;
  st_nobufs = st_rearms = st_max_chunks = st_iter_max_ns = st_batch_ns = st_reap_ns = st_overflow = 0;
}

/* At the memory wall of a 2 GB machine without swap, reclaim takes file-backed pages, the
 * program's own code among them, and the loop then stalls reading its code back from disk.
 * So as much of the code as RLIMIT_MEMLOCK allows (8 MB under systemd) is locked, from its
 * start, where the profile-guided build puts the handler, SQLite and the transport (the Lean
 * runtime follows). MLOCK_ONFAULT locks a page when it first runs, so code that never runs
 * takes no memory. Called after the rings are made: newer kernels charge them to the limit. */
#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 0x01
#endif
static void lock_code(void) {
  struct rlimit rl;
  if (getenv("FEED_NO_LOCK") || getrlimit(RLIMIT_MEMLOCK, &rl)) return;   /* FEED_NO_LOCK: for measurements */
  char exe[512], line[1024];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return;
  exe[n] = 0;
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return;
  unsigned long a = 0, b = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long s, e;
    char perm[8];
    if (sscanf(line, "%lx-%lx %7s", &s, &e, perm) == 3 && perm[2] == 'x' && strstr(line, exe)) { a = s; b = e; break; }
  }
  fclose(f);
  if (!a) return;
  unsigned long len = b - a;
  if (rl.rlim_cur != RLIM_INFINITY && len > rl.rlim_cur) len = rl.rlim_cur;
  for (; len >= (1ul << 20); len /= 2)
    if (syscall(SYS_mlock2, (void *)a, len & ~4095ul, MLOCK_ONFAULT) == 0) {
      fprintf(stderr, "transport: the first %lu KB of the code (of %lu KB) are locked as they run\n", len >> 10, (b - a) >> 10);
      return;
    }
  fprintf(stderr, "transport: code not locked (%s)\n", strerror(errno));
}

LEAN_EXPORT lean_obj_res feed_transport_run(b_lean_obj_arg secret) {

  lean_inc(secret);
  g_secret = feed_make_key(secret);   /* the HMAC key schedule (Feed.makeKey) */
  const char *host = getenv("HOST"), *ps = getenv("PORT");
  if (!host || !*host) host = "127.0.0.1";   /* the deployment sets HOST (0.0.0.0 on the box) */
  int port = ps && *ps ? atoi(ps) : 3000;
  start_s = now_unix();
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  /* A registered table may not exceed RLIMIT_NOFILE; leave room for ordinary descriptors. */
  u64_t per = rl.rlim_cur > 65536 ? 65536 : rl.rlim_cur;
  if (per > 1024) per -= 64;
  if (per > 65535) per = 65535;
  if (getenv("FEED_PERIOD_US")) period_us = atol(getenv("FEED_PERIOD_US"));
  if (getenv("FEED_WRITE_WINDOW_US")) write_window_us = atol(getenv("FEED_WRITE_WINDOW_US"));
  if (getenv("FEED_BATCH_TARGET_US")) BATCH_TARGET_NS = (u64_t)atoll(getenv("FEED_BATCH_TARGET_US")) * 1000ull;
  if (getenv("FEED_MAX_BATCH")) max_batch = (u32_t)atol(getenv("FEED_MAX_BATCH"));
  if (max_batch < 1 || max_batch > MAX_CHUNKS) max_batch = MAX_CHUNKS;
  const char *want_s = getenv("FEED_MAX_CONNS");
  u64_t want = want_s ? (u64_t)atoll(want_s) : 500000;
  nrings = (int)((want + per - 1) / per);
  if (nrings < 1) nrings = 1;
  if (nrings > MAX_RINGS) nrings = MAX_RINGS;
  evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  epfd = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 0};
  epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev);
  chunk_arr = lean_alloc_array(0, 256);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_stop;
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGINT, &sa, 0);
  signal(SIGPIPE, SIG_IGN);
  for (int i = 0; i < nrings; i++)
    if (ring_init(&rings[i], i, (u32_t)per, host, port)) { nrings = i; break; }
  size_t slots = (size_t)nrings * per;
  held = malloc(slots * sizeof(u32_t));
  rearm = malloc(slots * sizeof(u32_t));
  spilled = malloc(slots * sizeof(u32_t));
  snap = malloc(slots * sizeof(u32_t));
  if (!held || !rearm || !spilled || !snap) abort();
  fprintf(stderr, "transport: %d rings x %llu direct descriptors on %s:%d\n", nrings, (unsigned long long)per, host, port);
  lock_code();
  for (;;) {
    if (stop_requested) { transport_shutdown(); exit(0); }
    u64_t iter_start = mono_ns();
    int got = 0;
    st_iters++;
    for (int i = 0; i < nrings; i++) {
      ring *r = &rings[i];
      if (!ring_pending(r)) continue;
      int rc;
      do rc = sys_enter(r, r->to_submit, 0, IORING_ENTER_GETEVENTS);
      while (rc < 0 && errno == EINTR);
      if (rc < 0 && errno != EAGAIN && errno != EBUSY) { perror("io_uring_enter"); exit(1); }
      st_enters++;
      if (rc > 0) r->to_submit -= (u32_t)rc;
      r->hot = 0;
      if (__atomic_load_n(r->sq_kflags, __ATOMIC_RELAXED) & IORING_SQ_CQ_OVERFLOW) st_overflow++;
      u64_t t0 = mono_ns();
      got += reap(r);
      st_reap_ns += mono_ns() - t0;
    }
    /* reads for the connections reported readable, in rounds while their buffers come back */
    for (int round = 0; round < READ_ROUNDS; round++) {
      int issued = 0;
      for (int i = 0; i < nrings; i++) {
        ring *r = &rings[i];
        if (!r->nrq) continue;
        buf_publish(r);   /* the buffers given back so far, before the reads that take them */
        if (!issue_reads(r)) continue;
        issued = 1;
        int rc;
        do rc = sys_enter(r, r->to_submit, 0, IORING_ENTER_GETEVENTS);
        while (rc < 0 && errno == EINTR);
        if (rc < 0 && errno != EAGAIN && errno != EBUSY) { perror("io_uring_enter"); exit(1); }
        st_enters++;
        if (rc > 0) r->to_submit -= (u32_t)rc;
        got += reap(r);
      }
      if (!issued) break;
    }
    for (int i = 0; i < nrings; i++) if (!rings[i].accept_armed) arm_accept(&rings[i]);
    {
      u64_t t = mono_ns();
      writes_ok = (t - last_write_batch >= (u64_t)write_window_us * 1000) && db_writes_ok(t);
      if (writes_ok) last_write_batch = t;
    }
    present_held();
    present_spill();   /* after this iteration's input and the held writes: the backlog takes what room is left */
    u64_t tb = mono_ns();
    u32_t nb = nchunks;
    run_batch();
    u64_t bt = mono_ns() - tb;
    st_batch_ns += bt;
    if (BATCH_TARGET_NS && nb >= 32 && bt > 0) {
      /* the cap follows a running estimate of the time per connection, so that the first batch
       * after a burst (a stall elsewhere, a wave of new connections) is capped too */
      u64_t per = bt / nb;
      per_chunk_ns = per_chunk_ns ? (7 * per_chunk_ns + per) / 8 : per;
      u64_t c = per_chunk_ns ? BATCH_TARGET_NS / per_chunk_ns : max_batch;
      batch_cap = (u32_t)(c < 256 ? 256 : c > max_batch ? max_batch : c);
    }
    st_rearms += nrearm;
    for (u32_t i = 0; i < nrearm; i++) {
      ring *r = &rings[rearm[i] >> 16];
      u32_t slot = rearm[i] & 0xffff;
      conn *c = &r->conns[slot];
      if (c->state == C_OPEN && !(c->flags & F_POLL)) arm_poll(r, slot);
    }
    nrearm = 0;
    for (int i = 0; i < nrings; i++) buf_publish(&rings[i]);
    stats_tick();
    if (got || nheld || nspilled) {
      u64_t spent = mono_ns() - iter_start, period = (u64_t)period_us * 1000;
      if (spent > st_iter_max_ns) st_iter_max_ns = spent;
      if (spent < period && !nspilled) {
        for (int i = 0; i < nrings; i++) if (rings[i].to_submit) ring_flush(&rings[i]);
        struct timespec q = {0, (long)(period - spent)};
        nanosleep(&q, 0);
        st_sleeps++;
      }
      continue;
    }
    /* idle: submit what is queued, then sleep until some ring has work */
    for (int i = 0; i < nrings; i++) if (rings[i].to_submit) ring_flush(&rings[i]);
    int any = 0;
    for (int i = 0; i < nrings; i++) if (ring_pending(&rings[i])) any = 1;
    if (any) continue;
    struct epoll_event evs[4];
    int n;
    do n = epoll_wait(epfd, evs, 4, 1000);
    while (n < 0 && errno == EINTR);
    u64_t v;
    if (read(evfd, &v, sizeof v) < 0) {}
    for (int i = 0; i < nrings; i++) rings[i].hot = 1;
  }
  return lean_io_result_mk_ok(lean_box(0));
}
