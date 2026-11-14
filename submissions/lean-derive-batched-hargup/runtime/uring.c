#include "rt.h"
/* io_uring transport. One thread drives several rings. Each ring owns a SO_REUSEPORT listener,
 * a table of direct (fixed) socket descriptors and a provided-buffer ring. Direct descriptors
 * do not occupy the process file table, so the number of connections is bounded by
 * rings x per-ring table size rather than by RLIMIT_NOFILE. Rings are created with
 * DEFER_TASKRUN: completions are produced only when this thread asks, so each loop iteration
 * collects a batch of requests that is then served by one SQLite transaction.
 *
 * Receives follow Feed.Transport (spec/Feed/Transport.lean): each connection has one multishot
 * receive while buffers last. One that ends (ENOBUFS after a burst) is not re-armed at once:
 * its connection joins its ring's queue, and each pass re-arms at most NBUF queued connections
 * per ring, oldest first (Feed.Transport.issueRound). The transitions are the generated tables
 * RECV_NEXT/RECV_ACT (gen/transport_gen.h); the sizing constants come from Feed.Transport.sizing. */

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

#include "transport_gen.h"  /* MAX_RINGS, SLOTS_MAX, NBUF, BUFSZ, CQE_BUDGET, MAX_JOBS; RECV_NEXT/RECV_ACT */

#ifndef SQ_ENTRIES
#define SQ_ENTRIES 4096    /* test builds use 4 or 8 to exercise the SQ backlog */
#endif
#define CQ_ENTRIES 32768

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_SHUTDOWN };
/* Connection lifecycle: C_CLOSING = shutdown submitted; C_SHUT = its completion was handled and
 * the close submitted; C_FREE once the close completed. The close is submitted only from the
 * shutdown's completion, so it can never run before the shutdown (see conn_close). */
enum { C_FREE = 0, C_OPEN, C_CLOSING, C_SHUT };

typedef struct conn {
  u16 gen;
  u8 state, recv, sending, close_after, hold, pending;   /* recv: Feed.Transport.Recv (R_*) */
  u32 in_len, in_cap;
  u8 *in;                 /* bytes of a request that is not complete yet */
  obuf *out_head, *out_tail;
} conn;

typedef struct ring {
  int fd, efd, lfd, idx;
  u32 *sq_khead, *sq_ktail, *sq_kflags, *sq_array, sq_mask, sq_entries;
  u32 sq_tail;              /* entries written into the kernel SQ; published to the kernel only in ring_enter */
  struct io_uring_sqe *bl;  /* backlog: entries waiting for room in the kernel SQ, in submission order */
  u32 bl_head, bl_tail, bl_cap;
  struct io_uring_sqe *sqes;
  u32 *cq_khead, *cq_ktail, cq_mask;
  struct io_uring_cqe *cqes;
  struct io_uring_buf_ring *br;
  u16 br_tail;
  u8 *bufs;
  u32 nslots;
  conn *conns;
  u8 hot;
  u8 accept_armed;
  u64 *rq;                  /* queue of connections whose receive ended: gen << 32 | slot, FIFO */
  u32 rq_head, rq_tail, rq_cap;
} ring;

#ifdef STATS
/* -DSTATS (diagnostic builds only): counters printed every 10 s. */
static u64 st_iters, st_enters, st_cqes, st_jobs, st_sleeps, st_waits;
static u64 st_enobufs, st_mshot_end, st_rearms, st_overflow, st_accepts, st_closes, st_enter_err, st_max_iter_cqes;
static u64 st_queued_max, st_tot[6], st_backlog, st_backlog_max;
static u64 st_close_early;   /* close completions seen before their shutdown completion (must stay 0) */
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif

static ring rings[MAX_RINGS];
static int nrings;
static int epfd;
static i64 now_s;           /* wall clock seconds, refreshed every iteration */

static inline int sys_setup(u32 e, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, e, p); }
#ifndef FAULT_ENTER
static inline int sys_enter(int fd, u32 sub, u32 minc, u32 flags) { return (int)syscall(__NR_io_uring_enter, fd, sub, minc, flags, NULL, 0); }
#else
/* Test builds only (build_remote.sh mode "fault"): io_uring_enter behind a fault injector, so
 * the real loop runs against the real kernel with refusals it must survive:
 *   EBUSY whenever a ring's CQ holds completions, on 20 % of calls, and then on EVERY call until
 *         that CQ's head has advanced (only consuming completions can end it);
 *   EAGAIN or ENOMEM on 5 % of calls each, before anything is submitted;
 *   a partial submission on 15 % of calls (the kernel is asked for fewer entries), and, on every
 *         call whose published entries include a shutdown, a submission that stops right after
 *         it (as allocation failure in io_submit_sqes can), so the shutdown is in the kernel and
 *         everything after it is not.
 * Counts go to stderr as "fault: ..." lines. */
static u64 fault_x = 0x2545F4914F6CDD1Dull, fault_n[5];
static u32 fault_head[MAX_RINGS];
static u8 fault_busy[MAX_RINGS];
static u32 fault_rand(void) { fault_x ^= fault_x << 13; fault_x ^= fault_x >> 7; fault_x ^= fault_x << 17; return (u32)(fault_x >> 11); }
static int sys_enter(int fd, u32 sub, u32 minc, u32 flags) {
  ring *r = 0;
  for (int i = 0; i < nrings; i++) if (rings[i].fd == fd) r = &rings[i];
  if (r && r->conns) {
    static time_t last;
    if (time(0) != last) { last = time(0); fprintf(stderr, "fault: ebusy %llu eagain %llu enomem %llu partial %llu cut_after_shutdown %llu\n", fault_n[0], fault_n[1], fault_n[2], fault_n[3], fault_n[4]); }
    u32 head = __atomic_load_n(r->cq_khead, __ATOMIC_ACQUIRE), tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
    if (fault_busy[r->idx] && head == fault_head[r->idx]) { fault_n[0]++; errno = EBUSY; return -1; }
    fault_busy[r->idx] = 0;
    u32 x = fault_rand() % 100;
    if (head != tail && x < 20) { fault_busy[r->idx] = 1; fault_head[r->idx] = head; fault_n[0]++; errno = EBUSY; return -1; }
    if (sub && x >= 20 && x < 25) { fault_n[1]++; errno = EAGAIN; return -1; }
    if (sub && x >= 25 && x < 30) { fault_n[2]++; errno = ENOMEM; return -1; }
    if (sub > 1 && x >= 30 && x < 45) { fault_n[3]++; sub = 1 + fault_rand() % (sub - 1); }
    u32 kh = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i + 1 < sub; i++)
      if (r->sqes[(kh + i) & r->sq_mask].opcode == IORING_OP_SHUTDOWN) { fault_n[4]++; sub = i + 1; break; }
  }
  return (int)syscall(__NR_io_uring_enter, fd, sub, minc, flags, NULL, 0);
}
#endif
#ifndef FAULT_PBUF_RESV
static inline int sys_register(int fd, u32 op, void *arg, u32 n) { return (int)syscall(__NR_io_uring_register, fd, op, arg, n); }
#else
/* Test builds only: behave like the affected Ubuntu kernels (below): refuse a provided-buffer ring
 * registration whose first reserved word is zero, and accept one where it is not (passing a copy
 * with the word cleared to this, correct, kernel). */
static int sys_register(int fd, u32 op, void *arg, u32 n) {
  if (op == IORING_REGISTER_PBUF_RING) {
    struct io_uring_buf_reg reg = *(struct io_uring_buf_reg *)arg;
    if (reg.resv[0] == 0) { errno = EINVAL; return -1; }
    reg.resv[0] = 0;
    return (int)syscall(__NR_io_uring_register, fd, op, &reg, n);
  }
  return (int)syscall(__NR_io_uring_register, fd, op, arg, n);
}
#endif

static inline u64 ud(u8 op, u8 ri, u16 gen, u32 slot) { return (u64)op << 56 | (u64)ri << 48 | (u64)gen << 32 | slot; }

/* ---- submission ------------------------------------------------------------------------
 * An SQE goes straight into the kernel SQ while that has room and nothing waits in the ring's
 * backlog; otherwise it goes to the backlog, keeping submission order. Entries are published
 * to the kernel, and io_uring_enter is called, only in ring_enter, at the event loop's safe
 * points: never from inside a completion handler, so no completion is ever half-handled while
 * the kernel runs. No entry is linked to another (IOSQE_IO_LINK): the kernel may consume a
 * prefix of what is published (io_submit_sqes stops at an allocation failure), so an ordering
 * that matters is enforced by submitting the second operation from the first one's completion.
 * A pointer returned by sqe_get is valid only until the next sqe_get on the ring (the backlog
 * may move when it grows): callers fill one entry at a time. */
static inline u32 sq_room(ring *r) { return r->sq_entries - (r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE)); }
static inline int bl_empty(ring *r) { return r->bl_head == r->bl_tail; }
static inline int ring_unsubmitted(ring *r) { return !bl_empty(r) || r->sq_tail != __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE); }
static struct io_uring_sqe *bl_slot(ring *r) {
  if (r->bl_tail - r->bl_head == r->bl_cap) {
    u32 cap = r->bl_cap ? r->bl_cap * 2 : 1024;
    struct io_uring_sqe *q = malloc(sizeof *q * cap);
    if (!q) abort();
    for (u32 i = 0; i < r->bl_cap; i++) q[i] = r->bl[(r->bl_head + i) & (r->bl_cap - 1)];
    free(r->bl);
    r->bl = q;
    r->bl_tail -= r->bl_head;
    r->bl_head = 0;
    r->bl_cap = cap;
  }
  struct io_uring_sqe *s = &r->bl[r->bl_tail++ & (r->bl_cap - 1)];
  memset(s, 0, sizeof *s);
  STAT(st_backlog++);
  STAT(st_backlog_max = r->bl_tail - r->bl_head > st_backlog_max ? r->bl_tail - r->bl_head : st_backlog_max);
  return s;
}
static inline struct io_uring_sqe *sq_slot(ring *r) {
  struct io_uring_sqe *s = &r->sqes[r->sq_tail++ & r->sq_mask];
  memset(s, 0, sizeof *s);
  return s;
}
static struct io_uring_sqe *sqe_get(ring *r) { return bl_empty(r) && sq_room(r) >= 1 ? sq_slot(r) : bl_slot(r); }
/* Move backlog entries, in order, into the kernel SQ while it has room. */
static void bl_drain(ring *r) {
  while (!bl_empty(r) && sq_room(r)) r->sqes[r->sq_tail++ & r->sq_mask] = r->bl[r->bl_head++ & (r->bl_cap - 1)];
}
/* Publish and submit everything that fits, and run completions. Errors the kernel reports for a
 * momentary shortage (no memory for a request, a busy CQ) are not fatal: exiting would drop every
 * connection, which clients see as refused connections until a restart. The entries stay where
 * they are (the kernel's SQ head says what it consumed), and the caller still reaps the CQ:
 * consuming completions is what frees a full CQ. Returns 0, or -1 after a transient failure. */
static int enter_transient(int e) { return e == EAGAIN || e == EBUSY || e == ENOMEM || e == ETIME || e == EINTR; }
static int ring_enter(ring *r) {
  bl_drain(r);
  __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
  u32 n = r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  int rc;
  do rc = sys_enter(r->fd, n, 0, IORING_ENTER_GETEVENTS);
  while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    if (!enter_transient(errno)) { perror("io_uring_enter"); exit(1); }
    STAT(st_enter_err++);
    static time_t last;
    if (time(0) != last) { last = time(0); fprintf(stderr, "io_uring_enter: %s (retrying)\n", strerror(errno)); }
    return -1;
  }
  return 0;
}
static void backoff(void) { struct timespec q = {0, 1000000}; nanosleep(&q, 0); }

static void arm_accept(ring *r) {
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_ACCEPT;
  s->fd = r->lfd;
  s->ioprio = IORING_ACCEPT_MULTISHOT;
  s->file_index = IORING_FILE_INDEX_ALLOC;
  s->user_data = ud(OP_ACCEPT, (u8)r->idx, 0, 0);
  r->accept_armed = 1;
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
  c->recv = R_MULTI;
}
static void rq_push(ring *r, u32 slot) {
  if (r->rq_tail - r->rq_head == r->rq_cap) {
    u32 cap = r->rq_cap ? r->rq_cap * 2 : 1024;
    u64 *q = malloc(sizeof(u64) * cap);
    if (!q) abort();
    for (u32 i = 0; i < r->rq_cap; i++) q[i] = r->rq[(r->rq_head + i) & (r->rq_cap - 1)];
    free(r->rq);
    r->rq = q;
    r->rq_tail -= r->rq_head;
    r->rq_head = 0;
    r->rq_cap = cap;
  }
  r->rq[r->rq_tail++ & (r->rq_cap - 1)] = (u64)r->conns[slot].gen << 32 | slot;
  STAT(st_queued_max = r->rq_tail - r->rq_head > st_queued_max ? r->rq_tail - r->rq_head : st_queued_max);
}
/* Feed.Transport.issueRound: re-arm queued connections from the front of the queue, at most NBUF
 * per ring per pass. Entries of connections that closed (or whose slot was reused) since they
 * were queued are skipped and do not count. */
static void rearm_queued(ring *r) {
  u32 n = 0;
  while (r->rq_head != r->rq_tail && n < NBUF) {
    u64 e = r->rq[r->rq_head++ & (r->rq_cap - 1)];
    u32 slot = (u32)e;
    conn *c = &r->conns[slot];
    if (c->gen != (u16)(e >> 32) || c->state != C_OPEN || c->recv != R_QUEUED) continue;
    arm_recv(r, slot);   /* Feed.Transport.issue: queued -> multi */
    n++;
    STAT(st_rearms++);
  }
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
/* Close: shut the socket down (ends the receive in flight), and once that has completed, close
 * the direct descriptor (handle_cqe, OP_SHUTDOWN), which releases the slot. The two are not
 * linked: a kernel that consumed only the shutdown (a partial submission) would otherwise run the
 * close first, the shutdown being always asynchronous. */
static void conn_close(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) return;
  if (c->sending) { c->close_after = 1; return; }
  c->state = C_CLOSING;
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_SHUTDOWN;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE;
  s->len = SHUT_RDWR;
  s->user_data = ud(OP_SHUTDOWN, (u8)r->idx, c->gen, slot);
}
static void submit_close(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  c->state = C_SHUT;
  struct io_uring_sqe *s = sqe_get(r);
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
  c->recv = R_IDLE;
  c->sending = c->close_after = c->hold = 0;
  c->pending = 0;           /* a pend entry of this generation, if any, is skipped (generation-tagged) */
}
static void conn_send(ring *r, u32 slot, obuf *b) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) { obuf_release(b); return; }
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

static job_t jobs[MAX_JOBS];
static int njobs;
/* Connections holding complete requests for the next batch (gen << 32 | connection id). Held
 * connections can carry over from pass to pass, so this list is bounded only by the number of
 * connections: it grows as needed. */
static u64 *pend, *pend_snap;
static u32 npend, pend_cap, snap_cap;
static void pend_push(u64 e) {
  if (npend == pend_cap) {
    pend_cap = pend_cap ? pend_cap * 2 : 4096;
    if (!(pend = realloc(pend, sizeof(u64) * pend_cap))) abort();
  }
  pend[npend++] = e;
}

static inline u32 conn_id(int ri, u32 slot) { return (u32)ri << 16 | slot; }

/* Implemented by the generated application: decode one request into a job. */
static void app_decode(const req_t *rq, job_t *j);

static const u8 RESP_400[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n{\"error\":\"malformed request\"}\r\n";

/* Parse and decode as many complete requests as possible from p[0..n). Returns bytes used. */
static inline void conn_hold(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  c->hold = 1;
  if (!c->pending) { c->pending = 1; pend_push((u64)c->gen << 32 | conn_id(r->idx, slot)); }
}
static u32 parse_requests(ring *r, u32 slot, const u8 *p, u32 n) {
  conn *c = &r->conns[slot];
  u32 used = 0;
  while (used < n && !c->hold && c->state == C_OPEN) {
    req_t rq;
    int k = http_parse(p + used, n - used, &rq);
    if (k == 0) break;
    if (njobs >= MAX_JOBS - 1) { conn_hold(r, slot); break; }
    job_t *j = &jobs[njobs++];
    memset(j, 0, sizeof *j);
    j->conn = conn_id(r->idx, slot);
    j->gen = c->gen;
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
  if (!n) { c->in_len = 0; return; }
  if (c->in == p) { c->in_len = n; return; }
  if (n > c->in_cap) {
    u32 cap = n < 4096 ? 4096 : n * 2;
    u8 *m = malloc(cap);
    if (!m) abort();
    if (c->in && c->in_len) memmove(m, p, n); else memcpy(m, p, n);
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
      u32 cap = (c->in_len + n) * 2;
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

static void handle_cqe(ring *r, struct io_uring_cqe *e) {
  u64 u = e->user_data;
  u8 op = (u8)(u >> 56);
  u16 gen = (u16)(u >> 32);
  u32 slot = (u32)u;
  int res = e->res;
  switch (op) {
  case OP_ACCEPT:
    if (res >= 0) {
      STAT(st_accepts++);
      conn *c = &r->conns[res];
      if (c->state != C_FREE) conn_reset(c);
      c->gen++;
      c->state = C_OPEN;
      arm_recv(r, (u32)res);
    } else if (res != -ECANCELED && res != -EINTR) {
      static time_t last;
      if (time(0) != last) { last = time(0); fprintf(stderr, "accept: %s\n", strerror(-res)); }
    }
    if (!(e->flags & IORING_CQE_F_MORE)) r->accept_armed = 0;
    return;
  case OP_RECV: {
    conn *c = &r->conns[slot];
    int has_buf = (e->flags & IORING_CQE_F_BUFFER) != 0;
    u16 bid = (u16)(e->flags >> IORING_CQE_BUFFER_SHIFT);
    if (c->gen != gen || c->state != C_OPEN) { if (has_buf) buf_recycle(r, bid); return; }
    /* classify the completion (Feed.Transport.Ev) */
    u8 ev = res > 0 ? ((e->flags & IORING_CQE_F_MORE) ? EV_MORE : EV_ENDED) : res == -ENOBUFS ? EV_NOBUFS : EV_EOF;
    u8 act = RECV_ACT[c->recv][ev];
    if (act == A_DROP) { if (has_buf) buf_recycle(r, bid); return; }   /* cannot belong to this state */
    c->recv = RECV_NEXT[c->recv][ev];
    STAT(st_enobufs += ev == EV_NOBUFS);
    STAT(st_mshot_end += ev == EV_ENDED);
    if (res > 0) {
      if (has_buf) { on_data(r, slot, r->bufs + (size_t)bid * BUFSZ, (u32)res); buf_recycle(r, bid); }
    } else if (has_buf) buf_recycle(r, bid);
    switch (act) {
    case A_ENQUEUE: if (c->state == C_OPEN) rq_push(r, slot); break;
    case A_CLOSE: conn_close(r, slot); break;
    default: break;
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
    if (c->gen == gen && c->state == C_SHUT) { conn_reset(c); c->state = C_FREE; STAT(st_closes++); }
    else if (c->gen == gen && c->state == C_CLOSING) {   /* cannot happen: the close is only submitted after the shutdown completed */
      STAT(st_close_early++);
      fprintf(stderr, "close completed before its shutdown (slot %u)\n", slot);
    }
    if (res < 0) fprintf(stderr, "close: %s\n", strerror(-res));
    return;
  }
  case OP_SHUTDOWN: {
    /* whatever the result (the peer may already have reset the socket), close the descriptor now */
    conn *c = &r->conns[slot];
    if (c->gen == gen && c->state == C_CLOSING) submit_close(r, slot);
    return;
  }
  }
}

static int reap(ring *r) {
  u32 head = *r->cq_khead, tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
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
  else { fprintf(stderr, "fatal: HOST %s is not an IP address\n", host); exit(1); }
  int fd = socket(fam, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (bind(fd, (struct sockaddr *)&ss, sl) < 0) { perror("bind"); exit(1); }
  if (listen(fd, 65535) < 0) { perror("listen"); exit(1); }
  return fd;
}

static void ring_init(ring *r, int idx, u32 slots, const char *host, int port) {
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL |
            IORING_SETUP_TASKRUN_FLAG;
  p.cq_entries = CQ_ENTRIES;
  int fd = sys_setup(SQ_ENTRIES, &p);
  if (fd < 0) { perror("io_uring_setup"); exit(1); }
  r->fd = fd;
  r->idx = idx;
  size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(u32);
  size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  if (cq_sz > sq_sz) sq_sz = cq_sz;
  u8 *m = mmap(0, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  if (m == MAP_FAILED) { perror("mmap sq"); exit(1); }
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
  r->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  if (r->sqes == MAP_FAILED) { perror("mmap sqes"); exit(1); }
  /* direct descriptor table */
  struct io_uring_rsrc_register rr;
  memset(&rr, 0, sizeof rr);
  rr.nr = slots;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  if (sys_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr) < 0) { perror("register files"); exit(1); }
  r->nslots = slots;
  r->conns = calloc(slots, sizeof(conn));
  if (!r->conns) abort();
  /* provided buffers */
  r->br = mmap(0, NBUF * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->br == MAP_FAILED) { perror("mmap buf ring"); exit(1); }
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (u64)(uintptr_t)r->br;
  reg.ring_entries = NBUF;
  reg.bgid = 0;
  /* Ubuntu's 6.8.0-136 and later kernels invert the check on this registration's reserved words
   * (Launchpad #2162843): they refuse all zeroes and accept a nonzero resv[0]. A refused
   * registration is retried once that way, as #264 and #434 do; a correct kernel never gets there. */
  int rc = sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  if (rc < 0 && errno == EINVAL) {
    reg.resv[0] = 1;
    rc = sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1);
    if (rc == 0) fprintf(stderr, "buffer ring registered with resv[0]=1 (kernel with the inverted check)\n");
  }
  if (rc < 0) { perror("register buffer ring"); exit(1); }
  r->bufs = mmap(0, (size_t)NBUF * BUFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->bufs == MAP_FAILED) { perror("mmap bufs"); exit(1); }
  r->br_tail = 0;
  for (u16 i = 0; i < NBUF; i++) buf_recycle(r, i);
  buf_publish(r);
  /* wakeups */
  r->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (sys_register(fd, IORING_REGISTER_EVENTFD, &r->efd, 1) < 0) { perror("register eventfd"); exit(1); }
  struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u32 = (u32)idx};
  epoll_ctl(epfd, EPOLL_CTL_ADD, r->efd, &ev);
  r->lfd = listen_socket(host, port);
  arm_accept(r);
  r->hot = 1;
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
  if (per > SLOTS_MAX) per = SLOTS_MAX;   /* a slot must fit the 16-bit field of conn_id (Feed.Transport.Sizing.slots_fit) */
  nrings = (int)((want_conns + per - 1) / per);
  if (nrings < 1) nrings = 1;
  if (nrings > MAX_RINGS) nrings = MAX_RINGS;
  epfd = epoll_create1(EPOLL_CLOEXEC);
  for (int i = 0; i < nrings; i++) ring_init(&rings[i], i, (u32)per, host, port);
  fprintf(stderr, "transport: %d rings x %u direct descriptors\n", nrings, (unsigned)per);
}

/* Implemented by the generated application. */
static void app_batch(job_t *jobs, int n);

static void deliver(void) {
  for (int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    ring *r = &rings[j->conn >> 16];
    u32 slot = j->conn & 0xffff;
    conn *c = &r->conns[slot];
    if (c->gen != j->gen || c->state != C_OPEN) { if (j->resp) obuf_release(j->resp); continue; }
    if (j->resp) conn_send(r, slot, j->resp);
    if (j->close_after) conn_close(r, slot);
  }
  njobs = 0;
}
static void release_holds(void) {
  /* connections that stopped parsing at a write (or close) continue in the next batch; new holds
   * go to a fresh list while this one is walked */
  u64 *snap = pend; u32 n = npend, cap = pend_cap;
  pend = pend_snap; pend_cap = snap_cap; npend = 0;
  pend_snap = snap; snap_cap = cap;
  for (u32 i = 0; i < n; i++) {
    ring *r = &rings[(u32)snap[i] >> 16];
    u32 slot = (u32)snap[i] & 0xffff;
    conn *c = &r->conns[slot];
    if (c->gen != (u16)(snap[i] >> 32)) continue;   /* the slot was closed and reused since */
    c->pending = 0;
    c->hold = 0;
    if (c->state != C_OPEN || !c->in_len) continue;
    u32 used = parse_requests(r, slot, c->in, c->in_len);
    conn_keep(c, c->in + used, c->in_len - used);
  }
}

/* After a light pass (fewer than COALESCE_JOBS requests) that left nothing behind (every CQ
 * drained, no queued receive, no held request), let requests accumulate this long before the
 * next one. A pass that stopped at its completion budget, or left work queued, never waits. Every pass pays fixed costs: one io_uring_enter per active ring, a SQLite
 * transaction (BEGIN, cursors, COMMIT and its WAL-index shm locks, each an fcntl), the sleep
 * itself. Measured at 300,000 users on one core, 250 µs gave ~1,600 passes/s of ~20 requests
 * (0.45 io_uring_enter per request); 1 ms gives ~2.6x fewer passes. A pass that is not light
 * never waits, so when the CPU is the bottleneck the window does not apply. */
#ifndef COALESCE_US
#define COALESCE_US 1000
#endif
#ifndef COALESCE_JOBS
#define COALESCE_JOBS 64    /* an iteration with at least this many requests does not wait */
#endif

#ifdef STATS
static void stats_tick(void) {
  static i64 last;
  if (now_s - last < 10) return;
  if (last) fprintf(stderr, "stats: iters %llu enters %llu cqes %llu jobs %llu sleeps %llu waits %llu | jobs/iter %.1f enters/job %.2f"
                    " | enobufs %llu mshot_end %llu rearms %llu overflow %llu accepts %llu closes %llu enter_err %llu max_iter_cqes %llu"
                    " | queued_max %llu backlog %llu backlog_max %llu"
                    " | total enobufs %llu rearms %llu mshot_end %llu backlog %llu enter_err %llu close_early %llu"
                    " | commits %llu frames %llu\n",
                    st_iters, st_enters, st_cqes, st_jobs, st_sleeps, st_waits, st_iters ? (double)st_jobs / st_iters : 0,
                    st_jobs ? (double)st_enters / st_jobs : 0, st_enobufs, st_mshot_end, st_rearms, st_overflow, st_accepts,
                    st_closes, st_enter_err, st_max_iter_cqes, st_queued_max, st_backlog, st_backlog_max,
                    st_tot[0] += st_enobufs, st_tot[1] += st_rearms, st_tot[2] += st_mshot_end,
                    st_tot[4] += st_backlog, st_tot[5] += st_enter_err, st_close_early, st_commits, st_frames);
  else st_tot[0] += st_enobufs, st_tot[1] += st_rearms, st_tot[2] += st_mshot_end,
       st_tot[4] += st_backlog, st_tot[5] += st_enter_err;
  last = now_s;
  st_commits = st_frames = 0;
  st_iters = st_enters = st_cqes = st_jobs = st_sleeps = st_waits = 0;
  st_enobufs = st_mshot_end = st_rearms = st_overflow = st_accepts = st_closes = st_enter_err = st_max_iter_cqes = 0;
  st_queued_max = st_backlog = st_backlog_max = 0;
}
#endif

/* A ring needs an io_uring_enter if it has submissions, deferred completions waiting to run
 * (IORING_SQ_TASKRUN), CQEs not yet reaped, or its eventfd fired. */
static inline int ring_pending(ring *r) {
  STAT(st_overflow += (__atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE) & IORING_SQ_CQ_OVERFLOW) != 0);
  return ring_unsubmitted(r) || r->hot ||
         (__atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) ||
         *r->cq_khead != __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
}

#define ENTER_ROUNDS 4      /* enters per ring per pass while its backlog still has entries */

static void transport_run(void) {
  struct epoll_event evs[MAX_RINGS];
  for (;;) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    now_s = ts.tv_sec;
    int got = 0, served = 0, retry = 0, unsubmitted = 0;
    STAT(st_iters++);
    /* Safe point: submit (the backlog moves into the SQ as the kernel consumes it), run
     * completions, then reap. The CQ is reaped even when the enter failed: an EBUSY caused by a
     * full CQ only ends once completions are consumed. */
    for (int i = 0; i < nrings; i++) {
      ring *r = &rings[i];
      if (!ring_pending(r)) continue;
      int failed = 0;
      for (int k = 0; k < ENTER_ROUNDS; k++) {
        STAT(st_enters++);
        if (ring_enter(r)) { failed = 1; break; }
        if (bl_empty(r)) break;
      }
      r->hot = (u8)failed;   /* deferred completions may still be waiting: try again next pass */
      retry |= failed;
      got += reap(r);
    }
    STAT(st_cqes += (u64)got);
    STAT(st_max_iter_cqes = (u64)got > st_max_iter_cqes ? (u64)got : st_max_iter_cqes);
    for (int i = 0; i < nrings; i++) if (!rings[i].accept_armed) arm_accept(&rings[i]);
    STAT(st_jobs += (u64)njobs);
    served += njobs;
    if (njobs) app_batch(jobs, njobs);
    deliver();
    /* connections held at a write continue now, after their batch committed */
    release_holds();
    if (njobs) { STAT(st_jobs += (u64)njobs); served += njobs; app_batch(jobs, njobs); deliver(); }
    /* buffers recycled in this pass become available before any receive is armed or issued */
    for (int i = 0; i < nrings; i++) buf_publish(&rings[i]);
    int queued = 0, behind = 0;
    for (int i = 0; i < nrings; i++) {
      ring *r = &rings[i];
      rearm_queued(r);
      queued |= r->rq_head != r->rq_tail;
      unsubmitted |= ring_unsubmitted(r);
      behind |= *r->cq_khead != __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE) ||
                (__atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE) & IORING_SQ_CQ_OVERFLOW);
    }
    behind |= queued || npend;
#ifdef STATS
    stats_tick();
#endif
    if (retry) { backoff(); continue; }
    if (got || npend || queued) {
      /* Busy: the queued sends and reads go out with the next io_uring_enter of their ring.
       * After a light iteration, sleeping briefly first lets more requests arrive, so each
       * enter, SQLite transaction and wakeup serves a batch rather than one or two requests.
       * A heavy iteration continues at once: then the work itself is the batching interval. */
#if COALESCE_US > 0
      if (served < COALESCE_JOBS && !behind) {
        struct timespec q = {0, COALESCE_US * 1000};
        nanosleep(&q, 0);
        STAT(st_sleeps++);
      }
#endif
      continue;
    }
    if (unsubmitted) {
      for (int i = 0; i < nrings; i++)
        if (ring_unsubmitted(&rings[i])) { STAT(st_enters++); if (ring_enter(&rings[i])) { rings[i].hot = 1; retry = 1; } }
      if (retry) { backoff(); continue; }
      /* completions produced by that submission, or a backlog that did not fit, are handled now */
      int again = 0;
      for (int i = 0; i < nrings; i++) again |= ring_pending(&rings[i]);
      if (again) continue;
    }
    int n;
    do n = epoll_wait(epfd, evs, MAX_RINGS, -1);
    while (n < 0 && errno == EINTR);
    STAT(st_waits++);
    for (int i = 0; i < n; i++) rings[evs[i].data.u32].hot = 1;
  }
}
