#ifndef NET_H
#define NET_H

#include <arpa/inet.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef IORING_SETUP_CLAMP
#define IORING_SETUP_CLAMP (1U << 4)
#endif
#ifndef IORING_SETUP_SUBMIT_ALL
#define IORING_SETUP_SUBMIT_ALL (1U << 7)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SQ_CQ_OVERFLOW
#define IORING_SQ_CQ_OVERFLOW (1U << 1)
#endif
#ifndef IORING_SQ_TASKRUN
#define IORING_SQ_TASKRUN (1U << 2)
#endif
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1U << 3)
#endif
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif
#ifndef IORING_FILE_INDEX_ALLOC
#define IORING_FILE_INDEX_ALLOC (~0U)
#endif
#ifndef IORING_RSRC_REGISTER_SPARSE
#define IORING_RSRC_REGISTER_SPARSE (1U << 0)
#endif
#ifndef IORING_CQE_F_BUFFER
#define IORING_CQE_F_BUFFER (1U << 0)
#endif
#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE (1U << 1)
#endif
#ifndef IORING_CQE_BUFFER_SHIFT
#define IORING_CQE_BUFFER_SHIFT 16
#endif
#ifndef IOSQE_CQE_SKIP_SUCCESS
#define IOSQE_CQE_SKIP_SUCCESS (1U << 6)
#endif

#define MAX_RINGS 8        /* 8 x 65,535 direct descriptors */
#define NLIS 8             /* SO_REUSEPORT listeners: 8 accept queues for the ramp */
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 65536
#define PBUF_COUNT 4096    /* receive buffers per ring; a power of two */
#define PBUF_SIZE 2048
#define ACCEPT_DEPTH 512   /* accepts in flight on the accepting ring, 64 per listener */

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_MISC };

/* user_data: op | ring | generation | aux | slot */
static inline uint64_t ud_make(unsigned op, unsigned ring, unsigned gen, unsigned aux, unsigned slot) {
  return ((uint64_t)op << 56) | ((uint64_t)ring << 48) | ((uint64_t)(gen & 0xffff) << 32) |
         ((uint64_t)(aux & 0xffff) << 16) | (uint64_t)(slot & 0xffff);
}
#define UD_OP(u) ((unsigned)((u) >> 56))
#define UD_GEN(u) ((uint16_t)((u) >> 32))
#define UD_AUX(u) ((uint16_t)((u) >> 16))
#define UD_SLOT(u) ((uint32_t)((u) & 0xffff))

enum { C_FREE = 0, C_OPEN, C_CLOSING };

#define CF_RECV 0x01  /* multishot recv armed */
#define CF_BUSY 0x02  /* a request of this connection is being answered */
#define CF_HELD 0x04  /* a POST is parked until writes resume */
#define CF_READY 0x08 /* on the ready list: buffered input to parse */
#define CF_REARM 0x10 /* on the re-arm list */
#define CF_CLOSE 0x20 /* close once the response is out */

struct conn {
  uint16_t gen;
  uint8_t state;
  uint8_t flags;
  uint32_t send_len;
  char *in;
  uint32_t in_len;
  uint32_t in_cap;
};

struct ring_ctx {
  int fd;
  int idx;
  uint32_t sq_entries, sq_mask, sq_tail;
  uint32_t *sq_khead, *sq_ktail, *sq_kflags;
  struct io_uring_sqe *sqes;
  uint32_t cq_mask;
  uint32_t *cq_khead, *cq_ktail;
  struct io_uring_cqe *cqes;

  struct io_uring_buf_ring *br;
  uint16_t br_tail;
  uint8_t *bufs;

  struct conn *conns;
  uint32_t nslots;
  uint32_t open;              /* slots in use */
  uint32_t acc_inflight;      /* accepts queued here; each one has a slot reserved */
  uint16_t acc_lis[NLIS];     /* ... per listener */
};

struct tds_getevents_arg {
  uint64_t sigmask;
  uint32_t sigmask_sz;
  uint32_t pad;
  uint64_t ts;
};

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return (int)syscall(__NR_io_uring_setup, entries, p);
}
static inline int sys_io_uring_register(int fd, unsigned op, const void *arg, unsigned n) {
  return (int)syscall(__NR_io_uring_register, fd, op, arg, n);
}

static void ring_die(const char *what) {
  fprintf(stderr, "fatal: %s: %s\n", what, strerror(errno));
  exit(1);
}

static inline uint32_t ring_unsubmitted(struct ring_ctx *r) {
  return r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
}
static inline int ring_has_cqe(struct ring_ctx *r) {
  return *r->cq_khead != __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
}

static inline int ring_has_work(struct ring_ctx *r) {
  return (__atomic_load_n(r->sq_kflags, __ATOMIC_RELAXED) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) != 0;
}

static void ring_flush(struct ring_ctx *r) {
  uint32_t n = ring_unsubmitted(r);
  __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
  for (;;) {
    int rc = (int)syscall(__NR_io_uring_enter, r->fd, n, 0, IORING_ENTER_GETEVENTS, NULL, 0);
    if (rc >= 0 || errno == EBUSY || errno == EAGAIN) return;
    if (errno != EINTR) ring_die("io_uring_enter");
  }
}

static void ring_wait(struct ring_ctx *r, unsigned min_complete, long timeout_ns) {
  uint32_t n = ring_unsubmitted(r);
  __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
  struct __kernel_timespec ts = {.tv_sec = timeout_ns / 1000000000L, .tv_nsec = timeout_ns % 1000000000L};
  struct tds_getevents_arg arg = {.sigmask = 0, .sigmask_sz = 0, .pad = 0, .ts = (uint64_t)(uintptr_t)&ts};
  int rc = (int)syscall(__NR_io_uring_enter, r->fd, n, min_complete, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                        &arg, sizeof arg);
  if (rc < 0 && errno != ETIME && errno != EINTR && errno != EBUSY && errno != EAGAIN) ring_die("io_uring_enter");
}

static inline struct io_uring_sqe *sqe_get(struct ring_ctx *r) {
  if (__builtin_expect(ring_unsubmitted(r) >= r->sq_entries, 0)) {
    ring_flush(r);
    if (ring_unsubmitted(r) >= r->sq_entries) {
      errno = EBUSY;
      ring_die("submission queue stuck");
    }
  }
  struct io_uring_sqe *s = &r->sqes[r->sq_tail & r->sq_mask];
  memset(s, 0, sizeof *s);
  r->sq_tail++;
  return s;
}

static inline void pbuf_put(struct ring_ctx *r, uint16_t bid) {
  struct io_uring_buf *b = &r->br->bufs[r->br_tail & (PBUF_COUNT - 1)];
  b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)bid * PBUF_SIZE);
  b->len = PBUF_SIZE;
  b->bid = bid;
  r->br_tail++;
}
static inline void pbuf_publish(struct ring_ctx *r) {
  __atomic_store_n(&r->br->tail, r->br_tail, __ATOMIC_RELEASE);
}

static uint32_t ring_slot_limit(void) {
  struct rlimit rl;
  uint64_t lim = 1024;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) lim = rl.rlim_cur;
  }
  if (lim > 65535) lim = 65535;
  const char *e = getenv("RING_SLOTS"); /* smaller tables, to exercise the spill to further rings */
  long want = e ? atol(e) : 0;
  if (want >= 16 && (uint64_t)want < lim) lim = (uint64_t)want;
  return (uint32_t)lim;
}

static int ring_init(struct ring_ctx *r, int idx, uint32_t nslots) {
  memset(r, 0, sizeof *r);
  r->idx = idx;
  r->fd = -1;

  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
            IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;
  p.cq_entries = CQ_ENTRIES;
  int fd = sys_io_uring_setup(SQ_ENTRIES, &p);
  if (fd < 0 && errno == EINVAL) { /* a kernel without DEFER_TASKRUN (before 6.1) */
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_SUBMIT_ALL |
              IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;
    p.cq_entries = CQ_ENTRIES;
    fd = sys_io_uring_setup(SQ_ENTRIES, &p);
  }
  if (fd < 0) return 0;
  r->fd = fd;

  size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
  uint8_t *sq = mmap(0, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  uint8_t *cq = mmap(0, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
  r->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                 fd, IORING_OFF_SQES);
  if (sq == MAP_FAILED || cq == MAP_FAILED || r->sqes == MAP_FAILED) goto fail;
  r->sq_entries = p.sq_entries;
  r->sq_mask = *(uint32_t *)(sq + p.sq_off.ring_mask);
  r->sq_khead = (uint32_t *)(sq + p.sq_off.head);
  r->sq_ktail = (uint32_t *)(sq + p.sq_off.tail);
  r->sq_kflags = (uint32_t *)(sq + p.sq_off.flags);
  uint32_t *sq_array = (uint32_t *)(sq + p.sq_off.array);
  for (uint32_t i = 0; i < p.sq_entries; i++) sq_array[i] = i;
  r->sq_tail = *r->sq_ktail;
  r->cq_mask = *(uint32_t *)(cq + p.cq_off.ring_mask);
  r->cq_khead = (uint32_t *)(cq + p.cq_off.head);
  r->cq_ktail = (uint32_t *)(cq + p.cq_off.tail);
  r->cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);

  struct io_uring_rsrc_register rr;
  memset(&rr, 0, sizeof rr);
  rr.nr = nslots;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  if (sys_io_uring_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr) < 0) goto fail;
  r->nslots = nslots;
  r->conns = calloc(nslots, sizeof(struct conn));
  if (!r->conns) goto fail;

  r->bufs = mmap(0, (size_t)PBUF_COUNT * PBUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  r->br = mmap(0, PBUF_COUNT * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->bufs == MAP_FAILED || (void *)r->br == MAP_FAILED) goto fail;
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (uint64_t)(uintptr_t)r->br;
  reg.ring_entries = PBUF_COUNT;
  reg.bgid = 0;
  if (sys_io_uring_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) {
    if (errno != EINVAL) goto fail;
    reg.resv[0] = 1;
    if (sys_io_uring_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) goto fail;
  }
  r->br_tail = 0;
  for (unsigned i = 0; i < PBUF_COUNT; i++) pbuf_put(r, (uint16_t)i);
  pbuf_publish(r);
  return 1;

fail:;
  int saved = errno;
  close(fd);
  r->fd = -1;
  errno = saved;
  return 0;
}

/* Accepted sockets inherit TCP_NODELAY from the listener. */
static int open_listener(const char *host, int port) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    fprintf(stderr, "fatal: HOST %s is not an IPv4 address\n", host);
    return -1;
  }
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 65535) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

#endif
