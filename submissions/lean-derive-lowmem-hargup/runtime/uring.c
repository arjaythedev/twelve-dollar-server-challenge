#include "rt.h"
/* io_uring transport. One thread drives several rings. Each ring owns a table of direct
 * (fixed) socket descriptors and a provided-buffer ring. Direct descriptors do not occupy the
 * process file table, so the number of connections is bounded by rings x per-ring table size
 * rather than by RLIMIT_NOFILE. Rings are opened on demand: one listener is shared, exactly one
 * ring accepts at a time, and accepting moves on before that ring's table fills. Rings are
 * created with DEFER_TASKRUN: completions are produced only when this thread asks, so each
 * loop iteration collects a batch of requests that is then served by one SQLite transaction. */

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

#define MAX_RINGS 16
#ifndef SQ_ENTRIES
#define SQ_ENTRIES 4096
#endif
#define CQ_ENTRIES 32768
#define NBUF 512            /* provided receive buffers per ring */
#define BUFSZ 2048
#define CQE_BUDGET 2048     /* completions handled per ring per iteration */
#define ACCEPT_SPARE 2048   /* stop accepting into a ring this many slots before it is full */

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_SHUTDOWN };
enum { C_FREE = 0, C_OPEN, C_CLOSING };

/* Cold framing state lives with the fragment bytes; idle connections gain no fields. */
typedef struct { frame_state frame; u8 data[]; } fragment;
static fragment *fragment_of(u8 *p) { return p ? (fragment *)(p - offsetof(fragment, data)) : 0; }
static frame_state frame_for(const u8 *p) {
  if (p) return fragment_of((u8 *)p)->frame;
  return (frame_state){0};
}

typedef struct conn {
  u16 gen;
  u8 state, recv_armed, sending, close_after, hold, pending;
  u32 in_len, in_cap;
  u8 *in;                 /* bytes of a request that is not complete yet */
  obuf *out_head, *out_tail;
} conn;

typedef struct ring {
  int fd, efd, idx;
  u32 *sq_khead, *sq_ktail, *sq_kflags, *sq_array, sq_mask, sq_entries, sq_tail, to_submit;
  struct io_uring_sqe *sqes;
  u32 *cq_khead, *cq_ktail, cq_mask;
  struct io_uring_cqe *cqes;
  struct io_uring_buf_ring *br;
  u16 br_tail;
  u8 *bufs;
  u32 nslots, nopen;      /* table size; slots in use (open or closing) */
  conn *conns;
  u8 hot;
  u8 accept_armed;
} ring;

static ring rings[MAX_RINGS];
static int nrings;
static int listen_fd, acc_ring;    /* the shared listener; the ring that accepts */
static u32 ring_slots;
static int epfd;
static i64 now_s;           /* wall clock seconds, refreshed every iteration */

#ifdef STATS
static u64 st_iters, st_enters, st_cqes, st_jobs, st_sleeps, st_waits, st_sqes, st_pubs;
static void stats_tick(void) {
  static i64 last;
  if (now_s - last < 10) return;
  if (last) fprintf(stderr, "stats: iters %llu enters %llu cqes %llu jobs %llu sleeps %llu waits %llu | jobs/iter %.1f enters/job %.2f sqes %llu tail-publishes %llu\n",
                    st_iters, st_enters, st_cqes, st_jobs, st_sleeps, st_waits, st_iters ? (double)st_jobs / st_iters : 0,
                    st_jobs ? (double)st_enters / st_jobs : 0, st_sqes, st_pubs);
  last = now_s;
  st_iters = st_enters = st_cqes = st_jobs = st_sleeps = st_waits = st_sqes = st_pubs = 0;
}
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif


static inline int sys_setup(u32 e, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, e, p); }
static inline int sys_enter(int fd, u32 sub, u32 minc, u32 flags) { return (int)syscall(__NR_io_uring_enter, fd, sub, minc, flags, NULL, 0); }
static inline int sys_register(int fd, u32 op, void *arg, u32 n) { return (int)syscall(__NR_io_uring_register, fd, op, arg, n); }

static inline u64 ud(u8 op, u8 ri, u16 gen, u32 slot) { return (u64)op << 56 | (u64)ri << 48 | (u64)gen << 32 | slot; }

/* The shared tail sees only fully initialized entries, immediately before entry. */
static int ring_enter(ring *r) {
  if (r->to_submit) { __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE); STAT(st_pubs++); }
  int rc;
  do { STAT(st_enters++); rc = sys_enter(r->fd, r->to_submit, 0, IORING_ENTER_GETEVENTS); }
  while (rc < 0 && errno == EINTR);
  if (rc < 0) { perror("io_uring_enter"); exit(1); }
  r->to_submit -= (u32)rc;
  return rc;
}
static void ring_flush(ring *r) { if (r->to_submit) (void)ring_enter(r); }
static void sqe_ensure(ring *r, u32 count) {
  u32 head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  while (r->sq_tail - head + count > r->sq_entries) {
    if (!r->to_submit || !ring_enter(r)) { fprintf(stderr, "fatal: submission queue made no progress\n"); exit(1); }
    head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  }
}
typedef struct { struct io_uring_sqe *p; } sqe_reserved;
typedef struct { struct io_uring_sqe *p; } sqe_ready;
static sqe_reserved sqe_reserve(ring *r) {
  sqe_ensure(r, 1);
  struct io_uring_sqe *s = &r->sqes[r->sq_tail & r->sq_mask];
  memset(s, 0, sizeof *s);
  return (sqe_reserved){s};
}
static void sqe_commit(ring *r, sqe_ready ready) {
  (void)ready;
  r->sq_tail++;
  r->to_submit++;
  STAT(st_sqes++);
}
#include "sqe_gen.h"

static void arm_accept(ring *r) {
  sqe_accept(r, listen_fd, ud(OP_ACCEPT, (u8)r->idx, 0, 0));
  r->accept_armed = 1;
}
static void arm_recv(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  sqe_recv(r, (int)slot, ud(OP_RECV, (u8)r->idx, c->gen, slot));
  c->recv_armed = 1;
}
static void submit_send(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  obuf *b = c->out_head;
  sqe_send(r, (int)slot, (u64)(uintptr_t)b->data, b->len, ud(OP_SEND, (u8)r->idx, c->gen, slot));
  c->sending = 1;
}
/* Close: shut the socket down (ends the multishot receive), then release the slot. */
static void conn_close(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) return;
  if (c->sending) { c->close_after = 1; return; }
  c->state = C_CLOSING;
  /* Keep the linked shutdown/close pair in the same publication batch. */
  sqe_ensure(r, 2);
  sqe_shutdown(r, (int)slot, ud(OP_SHUTDOWN, (u8)r->idx, c->gen, slot));
  sqe_close(r, slot + 1, ud(OP_CLOSE, (u8)r->idx, c->gen, slot));
}
static void conn_reset(conn *c) {
  free(fragment_of(c->in));
  c->in = 0;
  c->in_len = c->in_cap = 0;
  for (obuf *b = c->out_head; b;) { obuf *n = b->next; obuf_release(b); b = n; }
  c->out_head = c->out_tail = 0;
  c->recv_armed = c->sending = c->close_after = c->hold = 0;
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

#define MAX_JOBS 65536
static job_t jobs[MAX_JOBS];
static int njobs;
static u32 rearm[MAX_JOBS];   /* connections whose multishot receive must be re-armed */
static int nrearm;
static u32 pend[MAX_JOBS];    /* connections holding complete requests for the next batch */
static int npend;

static inline u32 conn_id(int ri, u32 slot) { return (u32)ri << 16 | slot; }

/* Implemented by the generated application: decode one request into a job. */
static void app_decode(const req_t *rq, job_t *j, u32 slot);

static const u8 RESP_400[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n{\"error\":\"malformed request\"}\r\n";

/* Parse and decode as many complete requests as possible from p[0..n). Returns bytes used. */
static inline void conn_hold(ring *r, u32 slot) {
  conn *c = &r->conns[slot];
  c->hold = 1;
  if (!c->pending) { c->pending = 1; pend[npend++] = conn_id(r->idx, slot); }
}
static u32 parse_requests(ring *r, u32 slot, const u8 *p, u32 n, frame_state *fs) {
  conn *c = &r->conns[slot];
  u32 used = 0;
  while (used < n && !c->hold && c->state == C_OPEN) {
    req_t rq;
    int k = http_parse(p + used, n - used, &rq, fs);
    if (k == 0) break;
    if (njobs >= MAX_JOBS - 1) { fs->phase = FRAME_HEAD; fs->head.scanned = 0; conn_hold(r, slot); break; }
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
    app_decode(&rq, j, njobs - 1);
    used += (u32)k;
    if (j->is_write || j->close_after) conn_hold(r, slot);   /* later requests wait for this batch */
  }
  return used;
}
static void conn_keep(conn *c, const u8 *p, u32 n, const frame_state *fs) {
  if (!n) {
    /* A completed fragmented request must not leave a buffer on an idle connection. */
    free(fragment_of(c->in));
    c->in = 0;
    c->in_len = c->in_cap = 0;
    return;
  }
  if (c->in == p) { c->in_len = n; fragment_of(c->in)->frame = *fs; return; }
  if (n > c->in_cap) {
    u32 cap = n < 4096 ? 4096 : n * 2;
    fragment *f = malloc(sizeof *f + cap);
    if (!f) abort();
    u8 *m = f->data;
    if (c->in && c->in_len) memmove(m, p, n); else memcpy(m, p, n);
    free(fragment_of(c->in));
    c->in = m;
    c->in_cap = cap;
  } else memmove(c->in, p, n);
  c->in_len = n;
  fragment_of(c->in)->frame = *fs;
}
static void on_data(ring *r, u32 slot, const u8 *p, u32 n) {
  conn *c = &r->conns[slot];
  if (c->in_len || c->hold) {
    if (c->in_len + n > 65536) { conn_close(r, slot); return; }
    if (c->in_len + n > c->in_cap) {
      u32 cap = (c->in_len + n) * 2;
      fragment *f = realloc(fragment_of(c->in), sizeof *f + cap);
      if (!f) abort();
      if (!c->in) f->frame = (frame_state){0};
      c->in = f->data;
      c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, p, n);
    c->in_len += n;
    if (c->hold) return;
    frame_state fs = frame_for(c->in);
    u32 used = parse_requests(r, slot, c->in, c->in_len, &fs);
    conn_keep(c, c->in + used, c->in_len - used, &fs);
  } else {
    frame_state fs = {0};
    u32 used = parse_requests(r, slot, p, n, &fs);
    if (used < n) conn_keep(c, p + used, n - used, &fs);
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
      conn *c = &r->conns[res];
      if (c->state != C_FREE) conn_reset(c);
      else r->nopen++;
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
    if (!(e->flags & IORING_CQE_F_MORE)) c->recv_armed = 0;
    if (res > 0) {
      on_data(r, slot, r->bufs + (size_t)bid * BUFSZ, (u32)res);
      buf_recycle(r, bid);
      if (!c->recv_armed && c->state == C_OPEN) rearm[nrearm++] = conn_id(r->idx, slot);
    } else {
      if (has_buf) buf_recycle(r, bid);
      if (res == -ENOBUFS && c->state == C_OPEN) rearm[nrearm++] = conn_id(r->idx, slot);
      else if (res == 0 || res != -ENOBUFS) conn_close(r, slot);
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
    if (c->gen == gen && c->state == C_CLOSING) { conn_reset(c); c->state = C_FREE; r->nopen--; }
    if (res < 0) fprintf(stderr, "close: %s\n", strerror(-res));
    return;
  }
  case OP_SHUTDOWN:
    return;   /* only failures are reported; the linked close still runs */
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
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (bind(fd, (struct sockaddr *)&ss, sl) < 0) { perror("bind"); exit(1); }
  if (listen(fd, 65535) < 0) { perror("listen"); exit(1); }
  return fd;
}

static void ring_init(ring *r, int idx, u32 slots, const char *host, int port) {
  (void)host; (void)port;
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
  if (sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) { perror("register buffer ring"); exit(1); }
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
  r->hot = 1;
}

static void cancel_accept(ring *r) {
  sqe_cancel(r, ud(OP_ACCEPT, (u8)r->idx, 0, 0), ud(OP_SHUTDOWN, (u8)r->idx, 0, 0));
}
/* Keep exactly one ring accepting, on a ring with room: the emptiest open ring if it is at most
 * three quarters full, else a new ring. */
static void accept_balance(void) {
  ring *a = &rings[acc_ring];
  if (a->nopen + ACCEPT_SPARE < a->nslots) {
    if (!a->accept_armed) arm_accept(a);
    return;
  }
  int best = -1;
  for (int i = 0; i < nrings; i++)
    if (i != acc_ring && (best < 0 || rings[i].nopen < rings[best].nopen)) best = i;
  if ((best < 0 || rings[best].nopen > ring_slots / 4 * 3) && nrings < MAX_RINGS) {
    best = nrings++;
    ring_init(&rings[best], best, ring_slots, 0, 0);
    fprintf(stderr, "transport: ring %d opened (%u connections in ring %d)\n", best, a->nopen, acc_ring);
  }
  if (best < 0 || rings[best].nopen + ACCEPT_SPARE >= rings[best].nslots) {
    if (!a->accept_armed) arm_accept(a);   /* everything is full: keep trying where we are */
    return;
  }
  if (a->accept_armed) cancel_accept(a);
  acc_ring = best;
  arm_accept(&rings[best]);
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
  (void)want_conns;
  ring_slots = (u32)per;
  epfd = epoll_create1(EPOLL_CLOEXEC);
  listen_fd = listen_socket(host, port);
  nrings = 1;
  ring_init(&rings[0], 0, ring_slots, host, port);
  arm_accept(&rings[0]);
  fprintf(stderr, "transport: rings of %u direct descriptors, opened on demand (up to %d)\n", (unsigned)per, MAX_RINGS);
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
  /* connections that stopped parsing at a write (or close) continue in the next batch */
  static u32 snap[MAX_JOBS];
  int n = npend;
  memcpy(snap, pend, sizeof(u32) * (size_t)n);
  npend = 0;
  for (int i = 0; i < n; i++) {
    ring *r = &rings[snap[i] >> 16];
    u32 slot = snap[i] & 0xffff;
    conn *c = &r->conns[slot];
    c->pending = 0;
    c->hold = 0;
    if (c->state != C_OPEN || !c->in_len) continue;
    frame_state fs = frame_for(c->in);
    u32 used = parse_requests(r, slot, c->in, c->in_len, &fs);
    conn_keep(c, c->in + used, c->in_len - used, &fs);
  }
}

#ifndef COALESCE_US
#define COALESCE_US 250     /* after a light iteration, let requests accumulate this long */
#endif
#ifndef COALESCE_JOBS
#define COALESCE_JOBS 64    /* an iteration with at least this many requests does not wait */
#endif

/* A ring needs an io_uring_enter if it has submissions, deferred completions waiting to run
 * (IORING_SQ_TASKRUN), CQEs not yet reaped, or its eventfd fired. */
static inline int ring_pending(ring *r) {
  return r->to_submit || r->hot ||
         (__atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) ||
         *r->cq_khead != __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
}

static void transport_run(void) {
  struct epoll_event evs[MAX_RINGS];
  for (;;) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    now_s = ts.tv_sec;
    int got = 0, served = 0;
    STAT(st_iters++);
    for (int i = 0; i < nrings; i++) {
      ring *r = &rings[i];
      if (!ring_pending(r)) continue;
      (void)ring_enter(r);
      r->hot = 0;
      got += reap(r);
    }
    STAT(st_cqes += (u64)got);
    accept_balance();
    STAT(st_jobs += (u64)njobs);
    served += njobs;
    if (njobs) app_batch(jobs, njobs);
    deliver();
    /* connections held at a write continue now, after their batch committed */
    release_holds();
    if (njobs) { STAT(st_jobs += (u64)njobs); served += njobs; app_batch(jobs, njobs); deliver(); }
    for (int i = 0; i < nrearm; i++) {
      ring *r = &rings[rearm[i] >> 16];
      u32 slot = rearm[i] & 0xffff;
      conn *c = &r->conns[slot];
      if (c->state == C_OPEN && !c->recv_armed) arm_recv(r, slot);
    }
    nrearm = 0;
    for (int i = 0; i < nrings; i++) buf_publish(&rings[i]);
#ifdef STATS
    stats_tick();
#endif
    if (got || npend) {
      /* Busy: the queued sends go out with the next io_uring_enter of their ring. After a
       * light iteration, sleeping briefly first lets more requests arrive, so each enter,
       * SQLite transaction and wakeup serves a batch rather than one or two requests. A
       * heavy iteration continues at once: then the work itself is the batching interval. */
#if COALESCE_US > 0
      if (served < COALESCE_JOBS) {
        struct timespec q = {0, COALESCE_US * 1000};
        nanosleep(&q, 0);
        STAT(st_sleeps++);
      }
#endif
      continue;
    }
    for (int i = 0; i < nrings; i++) if (rings[i].to_submit) { ring_flush(&rings[i]); }
    int n;
    do n = epoll_wait(epfd, evs, MAX_RINGS, -1);
    while (n < 0 && errno == EINTR);
    STAT(st_waits++);
    for (int i = 0; i < n; i++) rings[evs[i].data.u32].hot = 1;
  }
}
