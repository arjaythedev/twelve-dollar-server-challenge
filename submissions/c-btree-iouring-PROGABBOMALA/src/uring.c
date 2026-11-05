#include "rt.h"
/* One thread. On a single CPU the thread owns one io_uring and waits inside
 * io_uring_enter, so DEFER_TASKRUN task work runs when a packet arrives.
 * With more CPUs, several rings share the thread through epoll. Direct descriptors
 * are not process file descriptors, so the connection count is the sparse fixed-file
 * table rather than RLIMIT_NOFILE. Completions are drained in batches and served
 * by one SQLite transaction. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>

#define MAX_RINGS 8
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 16384
#define NBUF 2048
#define BUFSZ 1024
#define CQE_BUDGET 2048
#define MAX_JOBS 65536
#define ONE_CPU_SLOTS 524288u
#define SLOT_BITS 24
#define SLOT_MASK ((1u << SLOT_BITS) - 1u)
#define MAX_TRACK 524288
#define COALESCE_US 250
#define COALESCE_JOBS 64
/* Group-commit coalesce. Never a 1-second poll: deferred task work would sit until it fired. */
#define FLUSH_NS 1000000L
#define EPOLL_WAIT_MS 2

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_SHUTDOWN, OP_SETSOCKOPT, OP_PROVIDE };
enum { C_FREE = 0, C_OPEN, C_CLOSING };

typedef struct conn {
  uint16_t gen;
  uint8_t state, recv_armed, sending, close_after, hold, pending;
  uint32_t in_len, in_cap;
  uint8_t *in;
  obuf *out_head, *out_tail;
} conn;

typedef struct ring {
  int fd, efd, lfd, idx;
  uint32_t *sq_khead, *sq_ktail, *sq_kflags, *sq_array, sq_mask, sq_entries, sq_tail, to_submit;
  struct io_uring_sqe *sqes;
  uint32_t *cq_khead, *cq_ktail, cq_mask;
  struct io_uring_cqe *cqes;
  struct io_uring_buf_ring *br;
  uint16_t br_tail;
  uint8_t *bufs;
  uint32_t nslots;
  conn *conns;
  uint8_t hot;
  uint8_t accept_armed;
} ring;

static ring rings[MAX_RINGS];
static int nrings;
static int epfd = -1;
static int sqpoll_on;
static int pbuf_mode = -1;
static int nodelay_ok = 1;
static unsigned ring_flags;
static unsigned ring_idle;
int64_t now_s;

static const int tcp_nodelay_on = 1;

static job_t jobs[MAX_JOBS];
static int njobs;
static uint32_t rearm[MAX_TRACK];
static int nrearm;
static uint32_t pend[MAX_TRACK];
static int npend;

static int sys_setup(unsigned entries, struct io_uring_params *p) {
  return (int)syscall(__NR_io_uring_setup, entries, p);
}
static int sys_enter(int fd, unsigned sub, unsigned minc, unsigned flags) {
  return (int)syscall(__NR_io_uring_enter, fd, sub, minc, flags, NULL, 0);
}
static int sys_register(int fd, unsigned op, const void *arg, unsigned n) {
  return (int)syscall(__NR_io_uring_register, fd, op, arg, n);
}

static inline uint64_t ud(uint8_t op, uint8_t ri, uint16_t gen, uint32_t slot) {
  return (uint64_t)op << 56 | (uint64_t)ri << 48 | (uint64_t)gen << 32 | slot;
}
static inline uint32_t conn_id(int ri, uint32_t slot) { return ((uint32_t)ri << SLOT_BITS) | slot; }
static inline int id_ring(uint32_t id) { return (int)(id >> SLOT_BITS); }
static inline uint32_t id_slot(uint32_t id) { return id & SLOT_MASK; }

static void ring_enter(ring *r) {
  unsigned sub = r->to_submit;
  unsigned flags = IORING_ENTER_GETEVENTS;
  unsigned enter_sub = sub;
  if (sqpoll_on) {
    unsigned sf = __atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE);
    if (sf & IORING_SQ_NEED_WAKEUP) flags |= IORING_ENTER_SQ_WAKEUP;
    enter_sub = 0;
    int cq_empty = *r->cq_khead == __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
    if (sub == 0 && !(flags & IORING_ENTER_SQ_WAKEUP) && cq_empty && !(sf & IORING_SQ_TASKRUN))
      return;
  }
  int rc;
  do rc = sys_enter(r->fd, enter_sub, 0, flags);
  while (rc < 0 && errno == EINTR);
  if (rc < 0) { perror("io_uring_enter"); exit(1); }
  if (sqpoll_on) r->to_submit = 0;
  else if ((unsigned)rc >= sub) r->to_submit = 0;
  else r->to_submit = sub - (unsigned)rc;
}

/* io_uring_submit_and_wait(ring, 1). A timespec bounds the group-commit coalesce;
 * NULL waits for the next completion. The enter itself runs DEFER_TASKRUN task work. */
static void ring_wait(ring *r, const struct __kernel_timespec *ts) {
  unsigned flags = IORING_ENTER_GETEVENTS;
  unsigned sub = 0;
  if (sqpoll_on) {
    unsigned sf = __atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE);
    if (sf & IORING_SQ_NEED_WAKEUP) flags |= IORING_ENTER_SQ_WAKEUP;
  } else if (!ts) {
    sub = r->to_submit;
  } else if (r->to_submit) {
    ring_enter(r);
  }
  int rc;
  do {
    if (ts) {
      struct io_uring_getevents_arg arg;
      memset(&arg, 0, sizeof arg);
      arg.ts = (uint64_t)(uintptr_t)ts;
      rc = (int)syscall(__NR_io_uring_enter, r->fd, 0, 1,
                        flags | IORING_ENTER_EXT_ARG, &arg, sizeof arg);
    } else {
      rc = sys_enter(r->fd, sub, 1, flags);
    }
  } while (rc < 0 && errno == EINTR);
  if (rc < 0 && errno != ETIME) { perror("io_uring_enter"); exit(1); }
  if (!ts && !sqpoll_on && rc >= 0) {
    if ((unsigned)rc >= sub) r->to_submit = 0;
    else r->to_submit = sub - (unsigned)rc;
  }
  r->hot = 1;
}

static struct io_uring_sqe *sqe_get(ring *r) {
  uint32_t head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  if (r->sq_tail - head >= r->sq_entries) {
    for (int i = 0; i < 1000 && r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE) >= r->sq_entries; i++) {
      ring_enter(r);
      if (sqpoll_on) {
        struct timespec q = {0, 100000};
        nanosleep(&q, 0);
      }
    }
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

static void arm_accept(ring *r) {
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_ACCEPT;
  s->fd = r->lfd;
  s->ioprio = IORING_ACCEPT_MULTISHOT;
  s->file_index = IORING_FILE_INDEX_ALLOC;
  s->user_data = ud(OP_ACCEPT, (uint8_t)r->idx, 0, 0);
  r->accept_armed = 1;
}
static void arm_recv(ring *r, uint32_t slot) {
  conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_RECV;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->ioprio = IORING_RECV_MULTISHOT;
  s->buf_group = 0;
  s->user_data = ud(OP_RECV, (uint8_t)r->idx, c->gen, slot);
  c->recv_armed = 1;
}
static void arm_nodelay(ring *r, uint32_t slot) {
  if (!nodelay_ok) return;
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_URING_CMD;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS;
  s->cmd_op = SOCKET_URING_OP_SETSOCKOPT;
  s->level = IPPROTO_TCP;
  s->optname = TCP_NODELAY;
  s->optval = (uint64_t)(uintptr_t)&tcp_nodelay_on;
  s->optlen = sizeof tcp_nodelay_on;
  s->user_data = ud(OP_SETSOCKOPT, (uint8_t)r->idx, 0, slot);
}
static void submit_send(ring *r, uint32_t slot) {
  conn *c = &r->conns[slot];
  obuf *b = c->out_head;
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_SEND;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE;
  s->addr = (uint64_t)(uintptr_t)b->data;
  s->len = b->len;
  s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
  s->user_data = ud(OP_SEND, (uint8_t)r->idx, c->gen, slot);
  c->sending = 1;
}

/* Fixed-file close is 1-based. Shutdown first so a multishot recv cannot hold the socket. */
static void conn_close(ring *r, uint32_t slot) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) return;
  if (c->sending) { c->close_after = 1; return; }
  c->state = C_CLOSING;
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_SHUTDOWN;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
  s->len = SHUT_RDWR;
  s->user_data = ud(OP_SHUTDOWN, (uint8_t)r->idx, c->gen, slot);
  s = sqe_get(r);
  s->opcode = IORING_OP_CLOSE;
  s->file_index = slot + 1;
  s->user_data = ud(OP_CLOSE, (uint8_t)r->idx, c->gen, slot);
}
static void conn_reset(conn *c) {
  free(c->in);
  c->in = 0;
  c->in_len = c->in_cap = 0;
  for (obuf *b = c->out_head; b;) { obuf *n = b->next; obuf_release(b); b = n; }
  c->out_head = c->out_tail = 0;
  c->recv_armed = c->sending = c->close_after = c->hold = 0;
}
static void conn_send(ring *r, uint32_t slot, obuf *b) {
  conn *c = &r->conns[slot];
  if (c->state != C_OPEN) { obuf_release(b); return; }
  b->next = 0;
  if (c->out_tail) c->out_tail->next = b; else c->out_head = b;
  c->out_tail = b;
  if (!c->sending) submit_send(r, slot);
}

static void buf_recycle(ring *r, uint16_t bid) {
  if (pbuf_mode) {
    struct io_uring_buf *b = &r->br->bufs[r->br_tail & (NBUF - 1)];
    b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)bid * BUFSZ);
    b->len = BUFSZ;
    b->bid = bid;
    r->br_tail++;
    return;
  }
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_PROVIDE_BUFFERS;
  s->fd = 1;
  s->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)bid * BUFSZ);
  s->len = BUFSZ;
  s->off = bid;
  s->buf_group = 0;
  s->user_data = ud(OP_PROVIDE, (uint8_t)r->idx, 0, bid);
}
static void buf_publish(ring *r) {
  if (pbuf_mode) __atomic_store_n(&r->br->tail, r->br_tail, __ATOMIC_RELEASE);
}

static const char RESP_400[] =
  "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"
  "{\"error\":\"malformed request\"}";

static void conn_hold(ring *r, uint32_t slot) {
  conn *c = &r->conns[slot];
  c->hold = 1;
  if (!c->pending && npend < MAX_TRACK) {
    c->pending = 1;
    pend[npend++] = conn_id(r->idx, slot);
  }
}
static uint32_t parse_requests(ring *r, uint32_t slot, const uint8_t *p, uint32_t n) {
  conn *c = &r->conns[slot];
  uint32_t used = 0;
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
      j->resp = obuf_static((const uint8_t *)RESP_400, (uint32_t)(sizeof RESP_400 - 1));
      j->close_after = 1;
      used = n;
      conn_hold(r, slot);
      break;
    }
    j->close_after = rq.close;
    app_decode(&rq, j);
    used += (uint32_t)k;
    if (j->is_write || j->close_after) conn_hold(r, slot);
  }
  return used;
}
static void conn_keep(conn *c, const uint8_t *p, uint32_t n) {
  if (!n) { c->in_len = 0; return; }
  if (c->in == p) { c->in_len = n; return; }
  if (n > c->in_cap) {
    uint32_t cap = n < 4096 ? 4096 : n * 2;
    uint8_t *m = malloc(cap);
    if (!m) abort();
    memcpy(m, p, n);
    free(c->in);
    c->in = m;
    c->in_cap = cap;
  } else memmove(c->in, p, n);
  c->in_len = n;
}
static void on_data(ring *r, uint32_t slot, const uint8_t *p, uint32_t n) {
  conn *c = &r->conns[slot];
  if (c->in_len || c->hold) {
    if (c->in_len + n > 65536) { conn_close(r, slot); return; }
    if (c->in_len + n > c->in_cap) {
      uint32_t cap = (c->in_len + n) * 2;
      uint8_t *m = realloc(c->in, cap);
      if (!m) abort();
      c->in = m;
      c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, p, n);
    c->in_len += n;
    if (c->hold) return;
    uint32_t used = parse_requests(r, slot, c->in, c->in_len);
    conn_keep(c, c->in + used, c->in_len - used);
  } else {
    uint32_t used = parse_requests(r, slot, p, n);
    if (used < n) conn_keep(c, p + used, n - used);
  }
}

static void handle_cqe(ring *r, struct io_uring_cqe *e) {
  uint64_t u = e->user_data;
  uint8_t op = (uint8_t)(u >> 56);
  uint16_t gen = (uint16_t)(u >> 32);
  uint32_t slot = (uint32_t)u;
  int res = e->res;
  switch (op) {
  case OP_ACCEPT:
    if (res >= 0 && (uint32_t)res < r->nslots) {
      conn *c = &r->conns[res];
      if (c->state != C_FREE) conn_reset(c);
      c->gen++;
      c->state = C_OPEN;
      arm_nodelay(r, (uint32_t)res);
      arm_recv(r, (uint32_t)res);
    } else if (res < 0 && res != -ECANCELED && res != -EINTR) {
      static time_t last;
      if (time(0) != last) { last = time(0); fprintf(stderr, "accept: %s\n", strerror(-res)); }
    }
    if (!(e->flags & IORING_CQE_F_MORE)) r->accept_armed = 0;
    return;
  case OP_RECV: {
    conn *c = &r->conns[slot];
    int has_buf = (e->flags & IORING_CQE_F_BUFFER) != 0;
    uint16_t bid = (uint16_t)(e->flags >> IORING_CQE_BUFFER_SHIFT);
    if (c->gen != gen || c->state != C_OPEN) { if (has_buf) buf_recycle(r, bid); return; }
    if (!(e->flags & IORING_CQE_F_MORE)) c->recv_armed = 0;
    if (res > 0 && has_buf) {
      on_data(r, slot, r->bufs + (size_t)bid * BUFSZ, (uint32_t)res);
      buf_recycle(r, bid);
      if (!c->recv_armed && c->state == C_OPEN && nrearm < MAX_TRACK) rearm[nrearm++] = conn_id(r->idx, slot);
    } else {
      if (has_buf) buf_recycle(r, bid);
      if (res == -ENOBUFS && c->state == C_OPEN && nrearm < MAX_TRACK) rearm[nrearm++] = conn_id(r->idx, slot);
      else if (res <= 0 && res != -ENOBUFS) conn_close(r, slot);
    }
    return;
  }
  case OP_SEND: {
    conn *c = &r->conns[slot];
    if (c->gen != gen) return;
    obuf *b = c->out_head;
    c->sending = 0;
    if (b && res > 0 && (uint32_t)res < b->len) {
      b->data += res;
      b->len -= (uint32_t)res;
      submit_send(r, slot);
      return;
    }
    if (b) {
      c->out_head = b->next;
      if (!c->out_head) c->out_tail = 0;
      obuf_release(b);
    }
    if (res < 0) {
      c->close_after = 0;
      for (obuf *x = c->out_head; x;) { obuf *nx = x->next; obuf_release(x); x = nx; }
      c->out_head = c->out_tail = 0;
      conn_close(r, slot);
      return;
    }
    if (c->out_head) submit_send(r, slot);
    else if (c->close_after) { c->close_after = 0; conn_close(r, slot); }
    return;
  }
  case OP_CLOSE: {
    conn *c = &r->conns[slot];
    if (c->gen == gen && c->state == C_CLOSING) { conn_reset(c); c->state = C_FREE; }
    if (res < 0) fprintf(stderr, "close: %s\n", strerror(-res));
    return;
  }
  case OP_SETSOCKOPT:
    if (res < 0 && nodelay_ok) {
      fprintf(stderr, "setsockopt TCP_NODELAY: %s (kept on the listener only)\n", strerror(-res));
      nodelay_ok = 0;
    }
    return;
  case OP_SHUTDOWN:
  case OP_PROVIDE:
    if (res < 0 && op == OP_PROVIDE) fprintf(stderr, "provide buffer: %s\n", strerror(-res));
    return;
  }
}

static int reap(ring *r) {
  uint32_t head = *r->cq_khead, tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
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

static int listen_socket(const char *host, int port) {
  struct sockaddr_storage ss;
  memset(&ss, 0, sizeof ss);
  struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
  struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
  socklen_t sl;
  int fam;
  if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) {
    fam = AF_INET; a4->sin_family = AF_INET; a4->sin_port = htons((uint16_t)port); sl = sizeof *a4;
  } else if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) {
    fam = AF_INET6; a6->sin6_family = AF_INET6; a6->sin6_port = htons((uint16_t)port); sl = sizeof *a6;
  } else {
    fprintf(stderr, "fatal: HOST %s is not an IP address\n", host);
    exit(1);
  }
  int fd = socket(fam, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) { perror("socket"); exit(1); }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  /* Set before listen() so accepted sockets inherit the buffer caps. TCP_NODELAY is
   * repeated per connection: Linux does not copy nonagle from the listener. */
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  int sz = 8192;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
  if (bind(fd, (struct sockaddr *)&ss, sl) < 0) { perror("bind"); exit(1); }
  if (listen(fd, 65535) < 0) { perror("listen"); exit(1); }
  return fd;
}

static int setup_try(unsigned flags, unsigned idle) {
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = flags;
  p.sq_thread_idle = idle;
  if (flags & IORING_SETUP_CQSIZE) p.cq_entries = CQ_ENTRIES;
  return sys_setup(SQ_ENTRIES, &p);
}

static int online_cpus(void) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) != 0) return 1;
  int n = CPU_COUNT(&set);
  return n > 0 ? n : 1;
}

static void choose_flags(void) {
  unsigned defer = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN |
                   IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_TASKRUN_FLAG;
  /* Kernel 5.13+ allows SQPOLL without CAP_SYS_NICE. Eight poll threads on the
   * benchmark's single vCPU would steal the core from the server. One deferred
   * issuer is the path that fits that machine. SQPOLL stays available when the
   * process can actually run on more than one CPU, and still falls back on EPERM. */
  if (online_cpus() == 1) {
    int fd = setup_try(defer, 0);
    if (fd >= 0) {
      close(fd);
      ring_flags = defer;
      ring_idle = 0;
      sqpoll_on = 0;
      return;
    }
  }
  unsigned sqpoll = IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL;
  int fd = setup_try(sqpoll, 2000);
  if (fd >= 0) {
    close(fd);
    ring_flags = sqpoll;
    ring_idle = 2000;
    sqpoll_on = 1;
    return;
  }
  if (errno != EPERM && errno != ENOMEM && errno != EINVAL)
    fprintf(stderr, "sqpoll unavailable: %s\n", strerror(errno));
  fd = setup_try(defer, 0);
  if (fd >= 0) { close(fd); ring_flags = defer; ring_idle = 0; sqpoll_on = 0; return; }
  fd = setup_try(0, 0);
  if (fd < 0) { perror("io_uring_setup"); exit(1); }
  close(fd);
  ring_flags = 0;
  ring_idle = 0;
  sqpoll_on = 0;
}

static void ring_init(ring *r, int idx, uint32_t slots, const char *host, int port) {
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = ring_flags;
  p.sq_thread_idle = ring_idle;
  if (ring_flags & IORING_SETUP_CQSIZE) p.cq_entries = CQ_ENTRIES;
  int fd = sys_setup(SQ_ENTRIES, &p);
  if (fd < 0) { perror("io_uring_setup"); exit(1); }
  r->fd = fd;
  r->idx = idx;
  size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
  size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  if (cq_sz > sq_sz) sq_sz = cq_sz;
  uint8_t *m = mmap(0, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  if (m == MAP_FAILED) { perror("mmap sq"); exit(1); }
  r->sq_khead = (uint32_t *)(m + p.sq_off.head);
  r->sq_ktail = (uint32_t *)(m + p.sq_off.tail);
  r->sq_kflags = (uint32_t *)(m + p.sq_off.flags);
  r->sq_mask = *(uint32_t *)(m + p.sq_off.ring_mask);
  r->sq_entries = *(uint32_t *)(m + p.sq_off.ring_entries);
  r->sq_array = (uint32_t *)(m + p.sq_off.array);
  for (uint32_t i = 0; i < r->sq_entries; i++) r->sq_array[i] = i;
  r->sq_tail = *r->sq_ktail;
  r->cq_khead = (uint32_t *)(m + p.cq_off.head);
  r->cq_ktail = (uint32_t *)(m + p.cq_off.tail);
  r->cq_mask = *(uint32_t *)(m + p.cq_off.ring_mask);
  r->cqes = (struct io_uring_cqe *)(m + p.cq_off.cqes);
  r->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  if (r->sqes == MAP_FAILED) { perror("mmap sqes"); exit(1); }

  struct io_uring_rsrc_register rr;
  memset(&rr, 0, sizeof rr);
  rr.nr = slots;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  if (sys_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr) < 0) { perror("register files"); exit(1); }
  r->nslots = slots;
  r->conns = calloc(slots, sizeof(conn));
  if (!r->conns) abort();

  r->bufs = mmap(0, (size_t)NBUF * BUFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->bufs == MAP_FAILED) { perror("mmap bufs"); exit(1); }
  /* Separate calls. MADV_HUGEPAGE | MADV_WILLNEED equals MADV_NOHUGEPAGE. */
  (void)madvise(r->bufs, (size_t)NBUF * BUFSZ, MADV_HUGEPAGE);
  (void)madvise(r->bufs, (size_t)NBUF * BUFSZ, MADV_WILLNEED);

  if (pbuf_mode != 0) {
    r->br = mmap(0, NBUF * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->br == MAP_FAILED) { perror("mmap buf ring"); exit(1); }
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr = (uint64_t)(uintptr_t)r->br;
    reg.ring_entries = NBUF;
    reg.bgid = 0;
    if (sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) {
      if (pbuf_mode == 1) { perror("register buffer ring"); exit(1); }
      fprintf(stderr, "provided buffer ring unavailable (%s); using provide-buffers\n", strerror(errno));
      pbuf_mode = 0;
      munmap(r->br, NBUF * sizeof(struct io_uring_buf));
      r->br = 0;
    } else pbuf_mode = 1;
  }
  if (pbuf_mode) {
    r->br_tail = 0;
    for (uint16_t i = 0; i < NBUF; i++) buf_recycle(r, i);
    buf_publish(r);
  } else {
    struct io_uring_sqe *s = sqe_get(r);
    s->opcode = IORING_OP_PROVIDE_BUFFERS;
    s->fd = NBUF;
    s->addr = (uint64_t)(uintptr_t)r->bufs;
    s->len = BUFSZ;
    s->off = 0;
    s->buf_group = 0;
    s->user_data = ud(OP_PROVIDE, (uint8_t)idx, 0, 0);
  }

  r->efd = -1;
  if (epfd >= 0) {
    r->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (r->efd < 0) { perror("eventfd"); exit(1); }
    if (sys_register(fd, IORING_REGISTER_EVENTFD, &r->efd, 1) < 0) { perror("register eventfd"); exit(1); }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u32 = (uint32_t)idx;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, r->efd, &ev) < 0) { perror("epoll_ctl"); exit(1); }
  }
  r->lfd = listen_socket(host, port);
  arm_accept(r);
  r->hot = 1;
}

void transport_init(const char *host, int port) {
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0 && errno != EPERM && errno != ENOMEM)
    perror("mlockall");
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0 && errno != EPERM) perror("setrlimit");
    getrlimit(RLIMIT_NOFILE, &rl);
  }
  /* Sparse fixed files are not process fds. One CPU gets a single table of
   * 524288 slots and waits in io_uring_enter. More CPUs keep eight rings. */
  choose_flags();
  uint32_t per;
  if (online_cpus() == 1) {
    nrings = 1;
    per = ONE_CPU_SLOTS;
    epfd = -1;
  } else {
    uint64_t cap = 65535;
    if (rl.rlim_cur < cap + 64) cap = rl.rlim_cur > 128 ? rl.rlim_cur - 64 : 1024;
    if (cap > 65535) cap = 65535;
    nrings = MAX_RINGS;
    per = (uint32_t)cap;
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create"); exit(1); }
  }
  for (int i = 0; i < nrings; i++) ring_init(&rings[i], i, per, host, port);
  fprintf(stderr, "transport: %d rings x %u direct descriptors (sqpoll=%d pbuf=%d wait=%s)\n",
          nrings, per, sqpoll_on, pbuf_mode, nrings == 1 ? "enter" : "epoll");
}

static void deliver(void) {
  for (int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    ring *r = &rings[id_ring(j->conn)];
    uint32_t slot = id_slot(j->conn);
    conn *c = &r->conns[slot];
    if (c->gen != (uint16_t)j->gen || c->state != C_OPEN) { if (j->resp) obuf_release(j->resp); continue; }
    if (j->resp) conn_send(r, slot, j->resp);
    if (j->close_after) conn_close(r, slot);
  }
  njobs = 0;
}
static void release_holds(void) {
  static uint32_t snap[MAX_TRACK];
  int n = npend;
  if (n > MAX_TRACK) n = MAX_TRACK;
  memcpy(snap, pend, sizeof(uint32_t) * (size_t)n);
  npend = 0;
  for (int i = 0; i < n; i++) {
    ring *r = &rings[id_ring(snap[i])];
    uint32_t slot = id_slot(snap[i]);
    conn *c = &r->conns[slot];
    c->pending = 0;
    c->hold = 0;
    if (c->state != C_OPEN || !c->in_len) continue;
    uint32_t used = parse_requests(r, slot, c->in, c->in_len);
    conn_keep(c, c->in + used, c->in_len - used);
  }
}

static inline int ring_pending(ring *r) {
  if (r->to_submit || r->hot) return 1;
  if (*r->cq_khead != __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE)) return 1;
  unsigned sf = __atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE);
  return (sf & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) != 0;
}

static void drain_efd(int efd) {
  uint64_t v;
  while (read(efd, &v, 8) == 8) {}
}

void transport_run(void) {
  struct epoll_event evs[MAX_RINGS];
  for (;;) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    now_s = ts.tv_sec;
    int got = 0, served = 0;
    for (int i = 0; i < nrings; i++) {
      ring *r = &rings[i];
      if (!ring_pending(r)) continue;
      ring_enter(r);
      r->hot = 0;
      got += reap(r);
    }
    for (int i = 0; i < nrings; i++) if (!rings[i].accept_armed) arm_accept(&rings[i]);
    served += njobs;
    if (njobs) app_batch(jobs, njobs);
    deliver();
    release_holds();
    if (njobs) { served += njobs; app_batch(jobs, njobs); deliver(); }
    for (int i = 0; i < nrearm; i++) {
      ring *r = &rings[id_ring(rearm[i])];
      uint32_t slot = id_slot(rearm[i]);
      conn *c = &r->conns[slot];
      if (c->state == C_OPEN && !c->recv_armed) arm_recv(r, slot);
    }
    nrearm = 0;
    for (int i = 0; i < nrings; i++) buf_publish(&rings[i]);
    if (got || npend) {
      if (served < COALESCE_JOBS) {
        if (nrings == 1) {
          struct __kernel_timespec ts = {0, FLUSH_NS};
          ring_wait(&rings[0], &ts);
        } else {
          struct timespec q = {0, COALESCE_US * 1000};
          nanosleep(&q, 0);
        }
      }
      continue;
    }
    if (nrings == 1) {
      ring_wait(&rings[0], 0);
      continue;
    }
    for (int i = 0; i < nrings; i++) if (rings[i].to_submit) ring_enter(&rings[i]);
    int n;
    do n = epoll_wait(epfd, evs, MAX_RINGS, EPOLL_WAIT_MS);
    while (n < 0 && errno == EINTR);
    for (int i = 0; i < n; i++) {
      ring *r = &rings[evs[i].data.u32];
      drain_efd(r->efd);
      r->hot = 1;
    }
  }
}
