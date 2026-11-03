#include "rt.h"
/* io_uring transport. One thread drives up to MAX_RINGS independent rings. Each ring owns a
 * SO_REUSEPORT listener, a table of direct (fixed) socket descriptors and a provided-buffer
 * ring. Direct descriptors do not occupy the process file table, so the connection capacity is
 * rings x per-ring table size rather than RLIMIT_NOFILE alone. Rings are created with
 * DEFER_TASKRUN: completions are produced only when this thread asks, so each loop iteration
 * collects a batch of requests that is then served by one SQLite transaction.
 *
 * Rings come up on demand: ring 0 is created at startup and an additional ring is initialized
 * only when the open connections of a running ring fill its descriptor table. Idle rings
 * therefore reserve no kernel memory and small machines (CI, containers) never hit ENOMEM at
 * startup; if io_uring_setup refuses the large CQ ring with ENOMEM, ring_init retries once
 * with a smaller one before giving up.
 *
 * Receives are steady-state multishot RECV over a provided buffer ring. When a multishot
 * receive ends (no IORING_CQE_F_MORE, or -ENOBUFS because the buffer ring ran dry), the
 * connection is queued on a bounded FIFO and re-armed only after the used buffers have been
 * recycled and published, so neither the socket receive queue nor the buffer ring stalls. */

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

#define MAX_RINGS 10
#define NLISTEN   8
#define ALL_LISTENERS ((1u << NLISTEN) - 1u)
#define ACCEPT_NEXT 8192    /* grow / arm the next ring this many slots before full */
#define ACCEPT_STOP 4096    /* cancel multishot accept this many slots before full */
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 32768
#define CQ_ENTRIES_MIN 16384
#define NBUF 8192           /* provided receive buffers per ring */
#define BUFSZ 2048
#define CQE_BUDGET 32768    /* completions handled per ring per iteration */
#define BATCH_CAP  16384    /* max jobs answered per batch; excess waits on pend FIFO */
#ifdef STATS
static u64 st_iters, st_enters, st_cqes, st_jobs, st_sleeps, st_waits, st_enobufs, st_max_turn_ns;
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif
/* Older 6.1-era libc headers predate these; values from the uapi header. */
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1U << 6)
#endif
#ifndef IORING_ENTER_EXT_ARG_REG
#define IORING_ENTER_EXT_ARG_REG (1U << 7)
#endif
#ifndef IORING_REGISTER_RING_FDS
#define IORING_REGISTER_RING_FDS 20
#endif
#ifndef IORING_ENTER_REGISTERED_RING
#define IORING_ENTER_REGISTERED_RING (1U << 4)
#endif
/* Timed wait: io_uring_enter(min_complete, timeout). 64 completions or 1 ms — bigger
 * batches share one COMMIT. Newer kernels cap the deferred task work one enter runs
 * in proportion to min_complete, so the wait must ask for the batch it wants. */
#define WAIT_MIN 512
#define WAIT_MS  1
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif
#ifndef IORING_POLL_ADD_MULTI
#define IORING_POLL_ADD_MULTI (1U << 0)
#endif
#ifndef POLLIN
#define POLLIN 0x0001
#endif

enum { OP_ACCEPT = 1, OP_RECV, OP_RECV_ONE, OP_SEND, OP_CLOSE, OP_SHUTDOWN, OP_CANCEL, OP_EVFD };
enum { C_FREE = 0, C_OPEN, C_CLOSING };

typedef struct conn {
  u16 gen;
  u8 state, recv_armed, sending, close_after, hold, pending, draining;
  u16 in_jobs;            /* parsed requests currently waiting in jobs[] for deliver() */
  u32 in_len, in_cap;
  u8 *in;                 /* bytes of a request that is not complete yet */
  obuf *out_head, *out_tail;
} conn;

typedef struct ring {
  int fd, efd, idx;
  int enter_fd;
  u32 enter_flags;
  u32 *sq_khead, *sq_ktail, *sq_kflags, *sq_array, sq_mask, sq_entries, sq_tail, to_submit;
  struct io_uring_sqe *sqes;
  u32 *cq_khead, *cq_ktail, cq_mask;
  struct io_uring_cqe *cqes;
  struct io_uring_buf_ring *br;
  u16 br_tail;
  u8 *bufs;
  u32 nslots;
  u32 nopen;              /* slots currently in state C_OPEN or C_CLOSING */
  conn *conns;
  u32 *drain_q;           /* FIFO of slots waiting for bounded single-shot MSG_DONTWAIT reads */
  u32 drain_head, drain_tail, drain_mask;
  u8 hot;
  u8 accept_armed;        /* bitmask of NLISTEN listeners armed on this ring */
  u8 accept_cancelling;   /* bitmask of NLISTEN listeners with cancel in flight */
  u32 reads;              /* single-shot reads in flight on this ring (<= NBUF) */
} ring;

static ring rings[MAX_RINGS];
static int nrings;
static int epfd;
static int lfds[NLISTEN];
static const char *g_host;  /* remembered so rings can come up later on demand */
static int g_port;
static u32 g_slots;
static void ring_grow(void);   /* brings up the next ring; defined in the setup section */

static inline int sys_setup(u32 e, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, e, p); }
static inline int sys_enter(int fd, u32 sub, u32 minc, u32 flags) { return (int)syscall(__NR_io_uring_enter, fd, sub, minc, flags, NULL, 0); }
static inline int sys_register(int fd, u32 op, void *arg, u32 n) { return (int)syscall(__NR_io_uring_register, fd, op, arg, n); }

static inline u64 ud(u8 op, u8 ri, u16 gen, u32 slot) { return (u64)op << 56 | (u64)ri << 48 | (u64)gen << 32 | slot; }

static void ring_flush(ring *r) {
  if (!r->to_submit) return;
  u32 n = r->to_submit;
  r->to_submit = 0;
  int rc;
  do rc = sys_enter(r->enter_fd, n, 0, IORING_ENTER_GETEVENTS | r->enter_flags);
  while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    r->to_submit = (u32)(r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE));
    if (errno == EBUSY || errno == EAGAIN) return;
    perror("io_uring_enter");
    exit(1);
  }
}
static struct io_uring_sqe *sqe_get(ring *r) {
  u32 head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  if (r->sq_tail - head >= r->sq_entries) {
    ring_flush(r);
    head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
    if (r->sq_tail - head >= r->sq_entries) {
      int rc;
      do rc = sys_enter(r->enter_fd, 0, 0, IORING_ENTER_GETEVENTS | r->enter_flags);
      while (rc < 0 && errno == EINTR);
      r->to_submit = (u32)(r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE));
      do rc = sys_enter(r->enter_fd, r->to_submit, 0, IORING_ENTER_GETEVENTS | r->enter_flags);
      while (rc < 0 && errno == EINTR);
      r->to_submit = (u32)(r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE));
      head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
      if (r->sq_tail - head >= r->sq_entries) { fprintf(stderr, "fatal: submission queue full\n"); exit(1); }
    }
  }
  struct io_uring_sqe *s = &r->sqes[r->sq_tail & r->sq_mask];
  memset(s, 0, sizeof *s);
  r->sq_tail++;
  r->to_submit++;
  __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
  return s;
}

static void arm_accept_ring(ring *r) {
  for (u32 l = 0; l < NLISTEN; l++) {
    u8 bit = (u8)(1u << l);
    if (!(r->accept_armed & bit)) {
      struct io_uring_sqe *s = sqe_get(r);
      s->opcode = IORING_OP_ACCEPT;
      s->fd = lfds[l];
      s->ioprio = IORING_ACCEPT_MULTISHOT;
      s->file_index = IORING_FILE_INDEX_ALLOC;
      s->user_data = ud(OP_ACCEPT, (u8)r->idx, 0, l);
      r->accept_armed |= bit;
    }
  }
}
static void cancel_accept_ring(ring *r) {
  for (u32 l = 0; l < NLISTEN; l++) {
    u8 bit = (u8)(1u << l);
    if ((r->accept_armed & ~r->accept_cancelling) & bit) {
      struct io_uring_sqe *s = sqe_get(r);
      s->opcode = IORING_OP_ASYNC_CANCEL;
      s->fd = -1;
      s->addr = ud(OP_ACCEPT, (u8)r->idx, 0, l);
      s->user_data = ud(OP_CANCEL, (u8)r->idx, 0, l);
      r->accept_cancelling |= bit;
    }
  }
}
static void arm_recv(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_RECV;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->ioprio = IORING_RECV_MULTISHOT;
  s->buf_group = 0;
  s->user_data = ud(OP_RECV, (u8)r->idx, c->gen, slot);
  c->recv_armed = 1;
}
static void arm_recv_oneshot(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_RECV;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  s->msg_flags = MSG_DONTWAIT;
  s->user_data = ud(OP_RECV_ONE, (u8)r->idx, c->gen, slot);
  c->recv_armed = 1;
  r->reads++;
}
static void submit_send(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  obuf *b = c->out_head;
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_SEND;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE;
  s->addr = (u64)(uintptr_t)b->data;
  s->len = b->len;
  s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
  s->user_data = ud(OP_SEND, (u8)r->idx, c->gen, slot);
  c->sending = 1;
}
/* Close: shut the socket down (ends the multishot receive), then release the slot. */
static void conn_close(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) return;
  if (c->sending) { c->close_after = 1; return; }
  c->state = C_CLOSING;
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_SHUTDOWN;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
  s->len = SHUT_RDWR;
  s->user_data = ud(OP_SHUTDOWN, (u8)r->idx, c->gen, slot);
  s = sqe_get(r);
  s->opcode = IORING_OP_CLOSE;
  s->file_index = slot + 1;
  s->user_data = ud(OP_CLOSE, (u8)r->idx, c->gen, slot);
}
static void conn_reset(conn *c) {
  free(c->in);
  c->in = 0;
  c->in_len = c->in_cap = 0;
  for (obuf *b = c->out_head; b;) { obuf *n = b->next; obuf_release(b); b = n; }
  c->out_head = c->out_tail = 0;
  c->recv_armed = c->sending = c->close_after = c->hold = c->pending = c->draining = 0;
  c->in_jobs = 0;
}
static void conn_send(ring *r, u32 slot, obuf *b) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) { obuf_release(b); return; }
  if (b->refs > 1) { b->refs--; b = obuf_clone(b); }
  b->next = 0;
  if (c->out_tail) c->out_tail->next = b; else c->out_head = b;
  c->out_tail = b;
  if (!c->sending) submit_send(r, slot);
}

static inline void buf_recycle(ring *r, u16 bid) {
  struct io_uring_buf *b = &r->br->bufs[r->br_tail & (NBUF - 1)];
  b->addr = (u64)(uintptr_t)(r->bufs + (size_t)bid * BUFSZ);
  b->len = BUFSZ;
  b->bid = bid;
  r->br_tail++;
}
static inline void buf_publish(ring *r) { __atomic_store_n(&r->br->tail, r->br_tail, __ATOMIC_RELEASE); }

/* ---- per-iteration request batch ---------------------------------------------------- */

#define MAX_JOBS BATCH_CAP
#define PEND_CAP 1048576
#define PEND_MASK (PEND_CAP - 1)
static job_t jobs[MAX_JOBS];
static int njobs;
static u32 pend[PEND_CAP];    /* FIFO ring of connections holding complete requests */
static u32 pend_head, pend_tail;
static inline u32 conn_id(int ri, u32 slot) { return (u32)ri << 16 | slot; }

static const u8 RESP_400[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n{\"error\":\"malformed request\"}\r\n";

/* Parse and decode as many complete requests as possible from p[0..n). Returns bytes used. */
static inline void conn_hold(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  c->hold = 1;
  if (!c->pending) { c->pending = 1; pend[pend_tail++ & PEND_MASK] = conn_id(r->idx, slot); }
}
static u32 parse_requests(ring *r, u32 slot, const u8 *p, u32 n) {
  conn *c = &r->conns[slot];
  u32 used = 0;
  while (used < n && !c->hold && c->state == C_OPEN) {
    req_t rq;
    int k = http_parse(p + used, n - used, &rq);
    if (k == 0) break;
    if (njobs >= BATCH_CAP) { conn_hold(r, slot); break; }
    job_t *j = &jobs[njobs++];
    memset(j, 0, sizeof *j);
    j->conn = conn_id(r->idx, slot);
    j->gen = c->gen;
    c->in_jobs++;
    if (k < 0) {
      j->resp = obuf_static(RESP_400, sizeof RESP_400 - 1);
      j->close_after = 1;
      used = n;
      conn_hold(r, slot);
      break;
    }
    j->close_after = rq.close;
    app_decode(&rq, j);
    used += (u32)k;
    if (j->is_write || j->close_after) conn_hold(r, slot);   /* later requests wait for this batch */
  }
  return used;
}
static void conn_keep(conn *c, const u8 *p, u32 n) {
  if (!n) {
    free(c->in);
    c->in = 0;
    c->in_len = c->in_cap = 0;
    return;
  }
  if (c->in == p) { c->in_len = n; return; }
  if (n > c->in_cap) {
    u32 cap = n < 512 ? 512 : n * 2;
    u8 *m = malloc(cap);
    if (!m) abort();
    memcpy(m, p, n);
    free(c->in);
    c->in = m;
    c->in_cap = cap;
  } else memmove(c->in, p, n);
  c->in_len = n;
}
static void on_data(ring *r, u32 slot, const u8 *p, u32 n) {
  conn *c = &r->conns[slot];
  if (c->in_len || c->hold) {
    if (c->in_len + n > 65536) { conn_close(r, slot); return; }
    if (c->in_len + n > c->in_cap) {
      u32 cap = (c->in_len + n) < 512 ? 512 : (c->in_len + n) * 2;
      u8 *m = realloc(c->in, cap);
      if (!m) abort();
      c->in = m;
      c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, p, n);
    c->in_len += n;
    if (c->hold) return;
    u32 used = parse_requests(r, slot, c->in, c->in_len);
    conn_keep(c, c->in + used, c->in_len - used);
  } else {
    u32 used = parse_requests(r, slot, p, n);
    if (used < n) conn_keep(c, p + used, n - used);
  }
}

static inline void drain_push(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  if (c->draining || c->state != C_OPEN) return;
  c->draining = 1;
  r->drain_q[r->drain_tail++ & r->drain_mask] = slot;
}

static void handle_cqe(ring *r, struct io_uring_cqe *e) {
  u64 u = e->user_data;
  u8 op = (u8)(u >> 56);
  u16 gen = (u16)(u >> 32);
  u32 slot = (u32)u;
  int res = e->res;
  switch (op) {
  case OP_ACCEPT:
    if (!(e->flags & IORING_CQE_F_MORE)) {
      u8 bit = (u8)(1u << (slot & 7u));
      r->accept_armed &= (u8)~bit;
      r->accept_cancelling &= (u8)~bit;
    }
    if (res >= 0) {
      conn *c = &r->conns[res];
      if (c->state != C_FREE) conn_reset(c);
      c->gen++;
      c->state = C_OPEN;
      r->nopen++;
      arm_recv(r, (u32)res);
    } else if (res != -ECANCELED && res != -EINTR) {
      if (res == -ENFILE || res == -EMFILE) ring_grow();   /* descriptor table ran out */
      static time_t last;
      time_t t = time(0);
      if (t != last) { last = t; fprintf(stderr, "accept: %s\n", strerror(-res)); }
    }
    return;
  case OP_RECV: {
    conn *c = &r->conns[slot];
    int has_buf = (e->flags & IORING_CQE_F_BUFFER) != 0;
    u16 bid = (u16)(e->flags >> IORING_CQE_BUFFER_SHIFT);
    if (c->gen != gen || c->state != C_OPEN) { if (has_buf) buf_recycle(r, bid); return; }
    if (!(e->flags & IORING_CQE_F_MORE)) c->recv_armed = 0;
    if (res > 0) {
      on_data(r, slot, r->bufs + (size_t)bid * BUFSZ, (u32)res);
      buf_recycle(r, bid);
      if (!c->recv_armed && c->state == C_OPEN) drain_push(r, slot);
    } else {
      if (has_buf) buf_recycle(r, bid);
      /* Multishot ended with -ENOBUFS: switch to the bounded FIFO single-shot drain queue
       * instead of re-arming multishot into an exhausted buffer ring. */
      if (res == -ENOBUFS && c->state == C_OPEN) { STAT(st_enobufs++); drain_push(r, slot); }
      else if (res == 0) { if (c->in_jobs || c->hold || c->pending) c->close_after = 1; else conn_close(r, slot); }
      else conn_close(r, slot);
    }
    return;
  }
  case OP_RECV_ONE: {
    conn *c = &r->conns[slot];
    int has_buf = (e->flags & IORING_CQE_F_BUFFER) != 0;
    u16 bid = (u16)(e->flags >> IORING_CQE_BUFFER_SHIFT);
    if (r->reads) r->reads--;
    if (c->gen != gen || c->state != C_OPEN) { if (has_buf) buf_recycle(r, bid); return; }
    c->recv_armed = 0;
    if (res > 0) {
      on_data(r, slot, r->bufs + (size_t)bid * BUFSZ, (u32)res);
      buf_recycle(r, bid);
      if (c->state == C_OPEN) {
        if ((u32)res == BUFSZ) drain_push(r, slot);        /* filled buffer: keep draining in FIFO rounds */
        else arm_recv(r, slot);                            /* short read: socket drained, back to multishot */
      }
    } else {
      if (has_buf) buf_recycle(r, bid);
      if ((res == -EAGAIN || res == -EWOULDBLOCK) && c->state == C_OPEN)
        arm_recv(r, slot);
      else if (res == -ENOBUFS && c->state == C_OPEN)
        drain_push(r, slot);
      else if (res == 0) {
        if (c->in_jobs || c->hold || c->pending) c->close_after = 1;
        else conn_close(r, slot);
      }
      else
        conn_close(r, slot);
    }
    return;
  }
  case OP_SEND: {
    conn *c = &r->conns[slot];
    if (c->gen != gen) return;
    obuf *b = c->out_head;
    c->sending = 0;
    if (b && res > 0 && (u32)res < b->len) {   /* short send: continue with the rest */
      b->data += res;
      b->len -= (u32)res;
      submit_send(r, slot);
      return;
    }
    if (b) {
      c->out_head = b->next;
      if (!c->out_head) c->out_tail = 0;
      obuf_release(b);
    }
    if (res < 0) { c->close_after = 0; for (obuf *x = c->out_head; x;) { obuf *nx = x->next; obuf_release(x); x = nx; } c->out_head = c->out_tail = 0; conn_close(r, slot); return; }
    if (c->out_head) submit_send(r, slot);
    else if (c->close_after) { c->close_after = 0; conn_close(r, slot); }
    return;
  }
  case OP_CLOSE: {
    conn *c = &r->conns[slot];
    if (c->gen == gen && c->state == C_CLOSING) { conn_reset(c); c->state = C_FREE; if (r->nopen) r->nopen--; }
    if (res < 0) fprintf(stderr, "close: %s\n", strerror(-res));
    return;
  }
  case OP_SHUTDOWN:
  case OP_CANCEL:
    return;   /* only failures are reported; the linked close still runs */
  case OP_EVFD: {
    int ri = (int)slot;
    if (ri > 0 && ri < nrings) {
      u64 v;
      ssize_t k = read(rings[ri].efd, &v, sizeof v);
      (void)k;
      rings[ri].hot = 1;
    }
    if (!(e->flags & IORING_CQE_F_MORE) && ri > 0 && ri < nrings && rings[ri].efd >= 0) {
      struct io_uring_sqe *s = sqe_get(&rings[0]);
      s->opcode = IORING_OP_POLL_ADD;
      s->fd = rings[ri].efd;
      s->len = IORING_POLL_ADD_MULTI;
      s->poll32_events = POLLIN;
      s->user_data = ud(OP_EVFD, 0, 0, (u32)ri);
    }
    return;
  }
  }
}

static int reap(ring *r) {
  u32 head = *r->cq_khead, tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
  int n = 0;
  while (head != tail && n < CQE_BUDGET) {
    struct io_uring_cqe cqe = r->cqes[head & r->cq_mask];
    head++;
    n++;
    __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE);
    handle_cqe(r, &cqe);
    if ((n & 255) == 0) buf_publish(r);
    if (head == tail) tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
  }
  __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE);
  return n;
}

/* ---- setup ------------------------------------------------------------------------------ */

static int listen_socket(const char *host, int port) {
  struct sockaddr_storage ss;
  socklen_t sl;
  memset(&ss, 0, sizeof ss);
  struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
  struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
  int fam;
  if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) { fam = AF_INET; a4->sin_family = AF_INET; a4->sin_port = htons((u16)port); sl = sizeof *a4; }
  else if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) { fam = AF_INET6; a6->sin6_family = AF_INET6; a6->sin6_port = htons((u16)port); sl = sizeof *a6; }
  else { fprintf(stderr, "fatal: HOST %s is not an IP address\n", host); return -1; }
  int fd = socket(fam, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) { perror("socket"); return -1; }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (bind(fd, (struct sockaddr *)&ss, sl) < 0) { perror("bind"); close(fd); return -1; }
  if (listen(fd, 65535) < 0) { perror("listen"); close(fd); return -1; }
  return fd;
}

static int ring_init(ring *r, int idx, u32 slots) {
  memset(r, 0, sizeof *r);
  r->efd = -1;
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL |
            IORING_SETUP_TASKRUN_FLAG;
  p.cq_entries = CQ_ENTRIES;
  int fd = sys_setup(SQ_ENTRIES, &p);
  if (fd < 0 && errno == ENOMEM) {
    p.cq_entries = CQ_ENTRIES_MIN;   /* small boxes and CI: retry with half the CQ ring */
    fd = sys_setup(SQ_ENTRIES, &p);
  }
  if (fd < 0) { perror("io_uring_setup"); return -1; }
  r->fd = fd;
  r->idx = idx;
  r->enter_fd = fd;
  r->enter_flags = 0;
  size_t ring_sz = p.sq_off.array + (size_t)p.sq_entries * sizeof(u32);
  size_t cq_sz = p.cq_off.cqes + (size_t)p.cq_entries * sizeof(struct io_uring_cqe);
  if (cq_sz > ring_sz) ring_sz = cq_sz;
  u8 *m = mmap(0, ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  if (m == MAP_FAILED) { perror("mmap sq"); goto fail; }
  r->sq_khead = (u32 *)(m + p.sq_off.head);
  r->sq_ktail = (u32 *)(m + p.sq_off.tail);
  r->sq_kflags = (u32 *)(m + p.sq_off.flags);
  r->sq_mask = *(u32 *)(m + p.sq_off.ring_mask);
  r->sq_entries = *(u32 *)(m + p.sq_off.ring_entries);
  r->sq_array = (u32 *)(m + p.sq_off.array);
  for (u32 i = 0; i < r->sq_entries; i++) r->sq_array[i] = i;
  r->sq_tail = *r->sq_ktail;
  r->cq_khead = (u32 *)(m + p.cq_off.head);
  r->cq_ktail = (u32 *)(m + p.cq_off.tail);
  r->cq_mask = *(u32 *)(m + p.cq_off.ring_mask);
  r->cqes = (struct io_uring_cqe *)(m + p.cq_off.cqes);
  r->sqes = mmap(0, (size_t)p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  if (r->sqes == MAP_FAILED) { r->sqes = 0; perror("mmap sqes"); goto fail; }
  /* direct descriptor table (sparse: slots are filled by multishot accept as they arrive) */
  struct io_uring_rsrc_register rr;
  memset(&rr, 0, sizeof rr);
  rr.nr = slots;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  if (sys_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr) < 0) { perror("register files"); goto fail; }
  r->nslots = slots;
  r->conns = calloc(slots, sizeof(conn));
  if (!r->conns) { fprintf(stderr, "ring conns: out of memory\n"); goto fail; }
  u32 dq_cap = 65536;
  r->drain_q = calloc(dq_cap, sizeof(u32));
  if (!r->drain_q) { fprintf(stderr, "ring drain_q: out of memory\n"); goto fail; }
  r->drain_mask = dq_cap - 1;
  /* provided buffer ring */
  r->br = mmap(0, NBUF * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->br == MAP_FAILED) { r->br = 0; perror("mmap buf ring"); goto fail; }
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (u64)(uintptr_t)r->br;
  reg.ring_entries = NBUF;
  reg.bgid = 0;
  int brc = sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  if (brc < 0 && errno == EINVAL) {
    /* Ubuntu 6.8.0-136..147 (LP #2162843) inverted the reserved-field check; retry with resv[0] = 1. */
    reg.resv[0] = 1;
    brc = sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  }
  if (brc < 0) { perror("register buffer ring"); goto fail; }
  r->bufs = mmap(0, (size_t)NBUF * BUFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->bufs == MAP_FAILED) { r->bufs = 0; perror("mmap bufs"); goto fail; }
  r->br_tail = 0;
  r->reads = 0;
  for (u16 i = 0; i < NBUF; i++) buf_recycle(r, i);
  buf_publish(r);
  /* wakeups: the eventfd tells epoll (and ring 0's multishot poll) when this ring has work */
  r->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (r->efd < 0) { perror("eventfd"); goto fail; }
  if (sys_register(fd, IORING_REGISTER_EVENTFD, &r->efd, 1) < 0) { perror("register eventfd"); goto fail; }
  struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u32 = (u32)idx};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, r->efd, &ev) < 0) { perror("epoll_ctl"); goto fail; }
  if (idx > 0) {
    struct io_uring_sqe *s = sqe_get(&rings[0]);
    s->opcode = IORING_OP_POLL_ADD;
    s->fd = r->efd;
    s->len = IORING_POLL_ADD_MULTI;
    s->poll32_events = POLLIN;
    s->user_data = ud(OP_EVFD, 0, 0, (u32)idx);
  }
  struct io_uring_rsrc_update up = { .offset = 0, .data = (u64)fd };
  if (sys_register(fd, IORING_REGISTER_RING_FDS, &up, 1) == 1) {
    r->enter_fd = 0;
    r->enter_flags = IORING_ENTER_REGISTERED_RING;
  }
  r->hot = 1;
  return 0;
fail:
  if (r->efd >= 0) close(r->efd);
  free(r->drain_q);
  free(r->conns);
  if (r->bufs) munmap(r->bufs, (size_t)NBUF * BUFSZ);
  if (r->br) munmap(r->br, NBUF * sizeof(struct io_uring_buf));
  if (r->sqes) munmap(r->sqes, (size_t)p.sq_entries * sizeof(struct io_uring_sqe));
  if (m != MAP_FAILED) munmap(m, ring_sz);
  close(fd);
  memset(r, 0, sizeof *r);
  return -1;
}

/* Bring up one more ring, lazily. Called when a descriptor table fills (or an accept reports
 * -ENFILE) and at most once per second after a failed attempt, so a machine that cannot host
 * another ring keeps serving on the rings it has. */
static void ring_grow(void) {
  if (nrings >= MAX_RINGS) return;
  static i64 next_try;
  if (now_s < next_try) return;
  ring *r = &rings[nrings];
  if (ring_init(r, nrings, g_slots) == 0) {
    nrings++;
    fprintf(stderr, "transport: ring %d up on demand (%u direct descriptors)\n", nrings - 1, (unsigned)g_slots);
  } else {
    next_try = now_s + 1;
  }
}

static void accept_policy(void) {
  if (nrings < MAX_RINGS && rings[nrings - 1].nopen + ACCEPT_NEXT >= rings[nrings - 1].nslots)
    ring_grow();
  int lowest = nrings;
  for (int i = 0; i < nrings; i++) {
    if (rings[i].nopen + ACCEPT_STOP < rings[i].nslots) { lowest = i; break; }
  }
  for (int i = 0; i < nrings; i++) {
    ring *r = &rings[i];
    int room = r->nopen + ACCEPT_STOP < r->nslots;
    int want = room && (i == lowest || (i == lowest + 1 && rings[lowest].nopen + ACCEPT_NEXT >= rings[lowest].nslots));
    if (want && r->accept_armed != ALL_LISTENERS) arm_accept_ring(r);
    else if (!room && r->accept_armed) cancel_accept_ring(r);
  }
}

static void transport_init(const char *host, int port, u32 want_conns) {
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  /* A registered table may not exceed RLIMIT_NOFILE; leave room for ordinary descriptors. */
  u64 per = rl.rlim_cur > 65536 ? 65536 : rl.rlim_cur;
  if (per > 1024) per -= 64;
  if (per > 65535) per = 65535;
  if (per < 64) per = 64;
  g_host = host;
  g_port = port;
  g_slots = (u32)per;
  for (int l = 0; l < NLISTEN; l++) {
    lfds[l] = listen_socket(host, port);
    if (lfds[l] < 0) {
      fprintf(stderr, "fatal: listen socket %d failed\n", l);
      exit(1);
    }
  }
  epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) { perror("epoll_create1"); exit(1); }
  if (ring_init(&rings[0], 0, g_slots) < 0) {
    fprintf(stderr, "fatal: primary ring failed\n");
    exit(1);
  }
  nrings = 1;
  accept_policy();
  fprintf(stderr, "transport: ring 0 up on %s:%d (%u descriptors/ring, %d listeners, %u connections wanted, up to %d rings on demand)\n",
          host, port, (unsigned)per, NLISTEN, (unsigned)want_conns, MAX_RINGS);
}

static void deliver(void) {
  for (int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    ring *r = &rings[j->conn >> 16];
    u32 slot = j->conn & 0xffff;
    conn *c = &r->conns[slot];
    if (c->gen == j->gen && c->in_jobs) c->in_jobs--;
    if (c->gen != j->gen || c->state != C_OPEN) { if (j->resp) obuf_release(j->resp); continue; }
    if (j->resp) conn_send(r, slot, j->resp);
    if (j->close_after || (c->close_after && !c->in_jobs && !c->hold && !c->pending)) conn_close(r, slot);
  }
  njobs = 0;
}
static void release_holds(void) {
  /* Connections held at a write or BATCH_CAP resume in strict FIFO order; if a pass hits
   * BATCH_CAP, the remaining held connections stay at the front of pend[] for the next pass. */
  u32 end = pend_tail;
  while (pend_head != end && njobs < BATCH_CAP) {
    u32 id = pend[pend_head++ & PEND_MASK];
    ring *r = &rings[id >> 16];
    u32 slot = id & 0xffff;
    conn *c = &r->conns[slot];
    c->pending = 0;
    c->hold = 0;
    if (c->state != C_OPEN) continue;
    if (c->in_len) {
      u32 used = parse_requests(r, slot, c->in, c->in_len);
      conn_keep(c, c->in + used, c->in_len - used);
    }
    if (c->close_after && !c->in_jobs && !c->hold && !c->pending) conn_close(r, slot);
  }
}

#ifndef COALESCE_US
#define COALESCE_US 200     /* after a light iteration, let requests accumulate this long */
#endif
#ifndef COALESCE_JOBS
#define COALESCE_JOBS 64    /* an iteration with at least this many requests does not wait */
#endif
#ifdef STATS
static void stats_tick(void) {
  static i64 last;
  if (now_s - last < 5) return;
  u32 open = 0, dr = 0;
  for (int i = 0; i < nrings; i++) {
    open += rings[i].nopen;
    dr += rings[i].drain_tail - rings[i].drain_head;
  }
  if (last) fprintf(stderr, "stats: open %u rings %d iters %llu enters %llu cqes %llu jobs %llu enobufs %llu pend %u drain %u max_turn %.1fms | jobs/iter %.1f\n",
                    open, nrings, (unsigned long long)st_iters, (unsigned long long)st_enters, (unsigned long long)st_cqes,
                    (unsigned long long)st_jobs, (unsigned long long)st_enobufs, pend_tail - pend_head, dr,
                    (double)st_max_turn_ns / 1e6, st_iters ? (double)st_jobs / st_iters : 0);
  last = now_s;
  st_iters = st_enters = st_cqes = st_jobs = st_sleeps = st_waits = st_enobufs = st_max_turn_ns = 0;
}
#endif

/* A ring needs an io_uring_enter if it has submissions, deferred completions waiting to run
 * (IORING_SQ_TASKRUN), CQEs not yet reaped, or its eventfd fired. */
static inline int ring_pending(ring *r) {
  return r->to_submit || r->hot ||
         (__atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) ||
         *r->cq_khead != __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
}

/* Timed wait on a ring: return when at least min CQEs are ready or `ms` elapsed.
 * Newer kernels cap the deferred task work one enter runs in proportion to min_complete,
 * so asking for WAIT_MIN completions lets one enter collect a full batch. */
static int ring_wait(ring *r, u32 min, u32 ms) {
  u32 nsub = r->to_submit;
  r->to_submit = 0;
  struct __kernel_timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
  struct io_uring_getevents_arg arg = {0, 0, 0, (u64)(uintptr_t)&ts};
  for (;;) {
    int rc = (int)syscall(__NR_io_uring_enter, r->enter_fd, nsub, min, IORING_ENTER_EXT_ARG | IORING_ENTER_GETEVENTS | r->enter_flags, &arg, sizeof arg);
    nsub = 0;
    if (rc >= 0) return rc;
    if (errno == EINTR || errno == ETIME || errno == EBUSY || errno == EAGAIN) {
      r->to_submit = (u32)(r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE));
      return 0;
    }
    perror("io_uring_enter");
    exit(1);
  }
}

static void transport_run(void) {
  struct epoll_event evs[MAX_RINGS];
  for (;;) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    struct timespec rts;
    clock_gettime(CLOCK_REALTIME_COARSE, &rts);
    now_s = rts.tv_sec;
    int got = 0, served = 0;
    STAT(st_iters++);
    /* Answer held connections from earlier turns first (oldest first), up to BATCH_CAP. */
    release_holds();
    int busy_queues = (pend_head != pend_tail || njobs > 0);
    for (int i = 0; i < nrings; i++) {
      if (rings[i].drain_head != rings[i].drain_tail) busy_queues = 1;
    }
    for (int i = nrings - 1; i >= 0; i--) {
      ring *r = &rings[i];
      if (i == 0 && !busy_queues && got == 0) {
        ring_wait(r, WAIT_MIN, WAIT_MS);
        STAT(st_enters++);
      } else if (ring_pending(r)) {
        u32 nsub = r->to_submit;
        r->to_submit = 0;
        int rc;
        do rc = sys_enter(r->enter_fd, nsub, 0, IORING_ENTER_GETEVENTS | r->enter_flags);
        while (rc < 0 && errno == EINTR);
        if (rc < 0) {
          r->to_submit = (u32)(r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE));
          if (errno != EBUSY && errno != EAGAIN) { perror("io_uring_enter"); exit(1); }
        }
        STAT(st_enters++);
      }
      r->hot = 0;
      got += reap(r);
    }
    /* If ring 0's OP_EVFD woke on a secondary ring during ring_wait, reap that ring now. */
    for (int i = 1; i < nrings; i++) {
      ring *r = &rings[i];
      if (ring_pending(r)) {
        u32 nsub = r->to_submit;
        r->to_submit = 0;
        int rc;
        do rc = sys_enter(r->enter_fd, nsub, 0, IORING_ENTER_GETEVENTS | r->enter_flags);
        while (rc < 0 && errno == EINTR);
        if (rc < 0) {
          r->to_submit = (u32)(r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE));
          if (errno != EBUSY && errno != EAGAIN) { perror("io_uring_enter"); exit(1); }
        }
        STAT(st_enters++);
        r->hot = 0;
        got += reap(r);
      }
    }
    accept_policy();
    for (int i = 0; i < nrings; i++) buf_publish(&rings[i]);
    /* Drain connections whose multishot receive ended, in bounded rounds of MSG_DONTWAIT
     * single-shot reads (up to 16,384 reads per iteration, never more per ring than NBUF).
     * With DEFER_TASKRUN, each round completes inside its non-blocking io_uring_enter, so
     * requests are read out of kernel TCP buffers immediately instead of waiting across turns. */
    int drain_budget = 16384;
    while (drain_budget > 0) {
      int issued = 0;
      for (int i = 0; i < nrings && drain_budget > 0; i++) {
        ring *r = &rings[i];
        while (r->drain_head != r->drain_tail && r->reads < NBUF && drain_budget > 0) {
          u32 slot = r->drain_q[r->drain_head++ & r->drain_mask];
          conn *c = &r->conns[slot];
          c->draining = 0;
          if (c->state == C_OPEN && !c->recv_armed) {
            arm_recv_oneshot(r, slot);
            issued++;
            drain_budget--;
          }
        }
      }
      if (!issued) break;
      for (int i = 0; i < nrings; i++) {
        ring *r = &rings[i];
        if (!r->to_submit) continue;
        ring_flush(r);
        STAT(st_enters++);
        int d = reap(r);
        got += d;
        STAT(st_cqes += (u64)d);
        buf_publish(r);
      }
    }
    STAT(st_jobs += (u64)njobs);
    served += njobs;
    if (njobs) app_batch(jobs, njobs);
    deliver();
    /* Serve held / overflowed requests in bounded batches of BATCH_CAP (up to 16 batches
     * per turn) and flush+reap completed sends between batches so obuf blocks recycle
     * immediately and response heap stays <= 32 MiB. */
    for (int b = 0; b < 15 && pend_head != pend_tail; b++) {
      for (int i = 0; i < nrings; i++) {
        ring *r = &rings[i];
        if (r->to_submit) { ring_flush(r); STAT(st_enters++); }
        int d = reap(r);
        got += d;
        STAT(st_cqes += (u64)d);
        buf_publish(r);
      }
      release_holds();
      if (!njobs) break;
      STAT(st_jobs += (u64)njobs);
      served += njobs;
      app_batch(jobs, njobs);
      deliver();
    }
    int any_draining = 0;
    for (int i = 0; i < nrings; i++) {
      buf_publish(&rings[i]);
      if (rings[i].drain_head != rings[i].drain_tail) any_draining = 1;
    }
#ifdef STATS
    {
      struct timespec ts2;
      clock_gettime(CLOCK_MONOTONIC, &ts2);
      u64 t1 = (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
      u64 t2 = (u64)ts2.tv_sec * 1000000000ULL + (u64)ts2.tv_nsec;
      if (t2 > t1 && t2 - t1 > st_max_turn_ns) st_max_turn_ns = t2 - t1;
    }
    stats_tick();
#endif
    if (got || pend_head != pend_tail || any_draining) {
      continue;
    }
    for (int i = 0; i < nrings; i++) if (rings[i].to_submit) { ring_flush(&rings[i]); STAT(st_enters++); }
    int n;
    do n = epoll_wait(epfd, evs, MAX_RINGS, -1);
    while (n < 0 && errno == EINTR);
    STAT(st_waits++);
    for (int i = 0; i < n; i++) {
      u32 ri = evs[i].data.u32;
      u64 v;
      ssize_t k = read(rings[ri].efd, &v, sizeof v);
      (void)k;
      rings[ri].hot = 1;
    }
  }
}
