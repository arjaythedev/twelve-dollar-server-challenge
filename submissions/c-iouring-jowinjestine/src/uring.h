/* uring.h: the io_uring ABI used by the server, on raw system calls (no liburing).
 *
 * One ring = one submission queue, one completion queue, a sparse table of direct descriptors
 * and one provided-buffer ring for receives. Everything here is plain data movement on the
 * shared memory the kernel maps for us. */
#ifndef URING_H
#define URING_H

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Constants that older kernel headers may lack (the kernel itself decides what works). */
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif
#ifndef IORING_FILE_INDEX_ALLOC
#define IORING_FILE_INDEX_ALLOC (~0U)
#endif
#ifndef IORING_REGISTER_PBUF_RING
#define IORING_REGISTER_PBUF_RING 22
#endif
#ifndef IORING_REGISTER_FILES2
#define IORING_REGISTER_FILES2 13
#endif
#ifndef IORING_RSRC_REGISTER_SPARSE
#define IORING_RSRC_REGISTER_SPARSE (1U << 0)
#endif
#ifndef IORING_ASYNC_CANCEL_USERDATA
#define IORING_ASYNC_CANCEL_USERDATA (1U << 4)
#endif
#ifndef IORING_CQE_BUFFER_SHIFT
#define IORING_CQE_BUFFER_SHIFT 16
#endif

#define LOAD_ACQ(p) atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)
#define LOAD_RLX(p) atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_relaxed)
#define STORE_REL(p, v) atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release)

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct ring {
  int fd;
  unsigned sq_entries, cq_entries, sq_mask, cq_mask;
  unsigned *sq_head, *sq_tail, *sq_flags, *sq_array;
  unsigned *cq_head, *cq_tail, *cq_overflow;
  struct io_uring_sqe *sqes;
  struct io_uring_cqe *cqes;
  unsigned sq_local;   /* our tail: entries written but not yet published to the kernel */
  unsigned sq_pushed;  /* the tail last published */
  int no_sqarray;
  /* direct descriptors */
  unsigned nslots;
  /* provided buffers (group 0) */
  struct io_uring_buf_ring *br;
  unsigned br_entries, br_mask;
  uint16_t br_tail;
  unsigned br_pending; /* buffers added since the last publish */
  uint8_t *bufs;
  unsigned buf_size;
};

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return (int)syscall(__NR_io_uring_setup, entries, p);
}
static inline long sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags, const void *arg,
                                      size_t argsz) {
  return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, argsz);
}
static inline int sys_io_uring_register(int fd, unsigned op, const void *arg, unsigned nr) {
  return (int)syscall(__NR_io_uring_register, fd, op, arg, nr);
}

/* Registers the provided-buffer ring. Ubuntu's 6.8.0-136 and later kernels invert the check on
 * the reserved words (Launchpad #2162843): they refuse all zeroes and accept a nonzero resv[0],
 * so a refused registration is tried once more that way. A correct kernel never gets there. */
static int ring_register_bufring(struct ring *r, unsigned entries) {
  size_t bytes = (size_t)entries * sizeof(struct io_uring_buf);
  void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return -1;
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (uint64_t)(uintptr_t)m;
  reg.ring_entries = entries;
  reg.bgid = 0;
  int rc = sys_io_uring_register(r->fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  if (rc < 0 && errno == EINVAL) {
    reg.resv[0] = 1;
    rc = sys_io_uring_register(r->fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  }
  if (rc < 0) {
    munmap(m, bytes);
    return -1;
  }
  r->br = m;
  r->br_entries = entries;
  r->br_mask = entries - 1;
  r->br_tail = 0;
  return 0;
}

static inline void ring_buf_add(struct ring *r, unsigned bid) {
  struct io_uring_buf *b = &r->br->bufs[r->br_tail & r->br_mask];
  b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)bid * r->buf_size);
  b->len = r->buf_size;
  b->bid = (uint16_t)bid;
  r->br_tail++;
  r->br_pending++;
}
static inline void ring_buf_publish(struct ring *r) {
  if (!r->br_pending) return;
  STORE_REL(&r->br->tail, r->br_tail);
  r->br_pending = 0;
}
static inline uint8_t *ring_buf_ptr(struct ring *r, unsigned bid) { return r->bufs + (size_t)bid * r->buf_size; }

/* Touches every page of a mapping now, while memory is plentiful. */
static void prefault(void *p, size_t n) {
  volatile uint8_t *q = p;
  for (size_t i = 0; i < n; i += 4096) q[i] = 0;
}

/* Creates a ring: sq_entries SQEs, cq_entries CQEs, a sparse direct-descriptor table of nslots,
 * nbufs receive buffers of bufsz bytes. Returns 0, or -1 with *err naming the failed step. */
static int ring_init(struct ring *r, unsigned sq_entries, unsigned cq_entries, unsigned nslots, unsigned nbufs,
                     unsigned bufsz, const char **err) {
  memset(r, 0, sizeof *r);
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE |
            IORING_SETUP_NO_SQARRAY;
  p.cq_entries = cq_entries;
  int fd = sys_io_uring_setup(sq_entries, &p);
  if (fd < 0 && errno == EINVAL) { /* a kernel before 6.6: no NO_SQARRAY */
    p.flags &= ~IORING_SETUP_NO_SQARRAY;
    memset(&p.sq_off, 0, sizeof p.sq_off);
    memset(&p.cq_off, 0, sizeof p.cq_off);
    fd = sys_io_uring_setup(sq_entries, &p);
  }
  if (fd < 0) { *err = "io_uring_setup"; return -1; }
  r->fd = fd;
  r->no_sqarray = (p.flags & IORING_SETUP_NO_SQARRAY) != 0;
  if (!(p.features & IORING_FEAT_SINGLE_MMAP)) { *err = "no single mmap"; goto fail; }
  size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  size_t sz = sq_sz > cq_sz ? sq_sz : cq_sz;
  uint8_t *sq = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  if (sq == MAP_FAILED) { *err = "mmap sq"; goto fail; }
  struct io_uring_sqe *sqes = mmap(NULL, (size_t)p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  if (sqes == MAP_FAILED) { *err = "mmap sqes"; goto fail; }
  r->sq_entries = p.sq_entries;
  r->cq_entries = p.cq_entries;
  r->sq_head = (unsigned *)(sq + p.sq_off.head);
  r->sq_tail = (unsigned *)(sq + p.sq_off.tail);
  r->sq_mask = *(unsigned *)(sq + p.sq_off.ring_mask);
  r->sq_flags = (unsigned *)(sq + p.sq_off.flags);
  r->sq_array = (unsigned *)(sq + p.sq_off.array);
  r->cq_head = (unsigned *)(sq + p.cq_off.head);
  r->cq_tail = (unsigned *)(sq + p.cq_off.tail);
  r->cq_mask = *(unsigned *)(sq + p.cq_off.ring_mask);
  r->cq_overflow = (unsigned *)(sq + p.cq_off.overflow);
  r->cqes = (struct io_uring_cqe *)(sq + p.cq_off.cqes);
  r->sqes = sqes;
  r->sq_local = r->sq_pushed = *r->sq_tail;
  if (!r->no_sqarray)
    for (unsigned i = 0; i < r->sq_entries; i++) r->sq_array[i] = i;
  /* the direct-descriptor table, sparse: nothing is allocated for unused slots */
  struct io_uring_rsrc_register rr;
  memset(&rr, 0, sizeof rr);
  rr.nr = nslots;
  rr.flags = IORING_RSRC_REGISTER_SPARSE;
  if (sys_io_uring_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr) < 0) { *err = "register files"; goto fail; }
  r->nslots = nslots;
  /* receive buffers */
  r->buf_size = bufsz;
  r->bufs = mmap(NULL, (size_t)nbufs * bufsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r->bufs == MAP_FAILED) { *err = "mmap bufs"; goto fail; }
  if (ring_register_bufring(r, nbufs) < 0) { *err = "register pbuf ring"; goto fail; }
  for (unsigned i = 0; i < nbufs; i++) ring_buf_add(r, i);
  ring_buf_publish(r);
  return 0;
fail:
  close(fd);
  return -1;
}

static inline unsigned ring_pending_submit(const struct ring *r) { return r->sq_local - r->sq_pushed; }

/* Publishes the SQEs written so far and enters the kernel: submits them, runs the ring's
 * deferred task work (GETEVENTS), and waits for min completions or timeout_ns (0: no wait). */
static long ring_enter(struct ring *r, unsigned min, uint64_t timeout_ns) {
  unsigned n = r->sq_local - r->sq_pushed;
  if (n) {
    STORE_REL(r->sq_tail, r->sq_local);
    r->sq_pushed = r->sq_local;
  }
  long rc;
  if (timeout_ns) {
    struct __kernel_timespec ts = {.tv_sec = (long long)(timeout_ns / 1000000000ULL), .tv_nsec = (long long)(timeout_ns % 1000000000ULL)};
    struct io_uring_getevents_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.ts = (uint64_t)(uintptr_t)&ts;
    rc = sys_io_uring_enter(r->fd, n, min, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof arg);
  } else {
    rc = sys_io_uring_enter(r->fd, n, min, IORING_ENTER_GETEVENTS, NULL, 0);
  }
  return rc < 0 ? -errno : rc;
}

/* The next free SQE, zeroed. If the queue is full, what is queued is submitted first. */
static inline struct io_uring_sqe *ring_sqe(struct ring *r) {
  while (r->sq_local - LOAD_ACQ(r->sq_head) >= r->sq_entries) {
    long rc = ring_enter(r, 0, 0);
    if (rc < 0 && rc != -EINTR && rc != -EBUSY && rc != -EAGAIN) {
      fprintf(stderr, "io_uring_enter (queue full): %s\n", strerror((int)-rc));
      exit(1);
    }
  }
  unsigned i = r->sq_local & r->sq_mask;
  if (!r->no_sqarray) r->sq_array[i] = i;
  r->sq_local++;
  struct io_uring_sqe *s = &r->sqes[i];
  memset(s, 0, sizeof *s);
  return s;
}

/* ---- SQE builders ---- */

static inline void sqe_accept_multishot(struct io_uring_sqe *s, int lfd, uint64_t ud) {
  s->opcode = IORING_OP_ACCEPT;
  s->fd = lfd;
  s->ioprio = IORING_ACCEPT_MULTISHOT;
  s->file_index = IORING_FILE_INDEX_ALLOC;
  s->user_data = ud;
}
/* One multishot receive per connection into the buffer ring: a request costs one completion. */
static inline void sqe_recv_multishot(struct io_uring_sqe *s, unsigned slot, uint64_t ud) {
  s->opcode = IORING_OP_RECV;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->fd = (int)slot;
  s->ioprio = IORING_RECV_MULTISHOT;
  s->buf_group = 0;
  s->user_data = ud;
}
/* One read, MSG_DONTWAIT: if nothing is left it ends with EAGAIN instead of waiting. */
static inline void sqe_recv_once(struct io_uring_sqe *s, unsigned slot, uint64_t ud) {
  s->opcode = IORING_OP_RECV;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->fd = (int)slot;
  s->buf_group = 0;
  s->msg_flags = MSG_DONTWAIT;
  s->user_data = ud;
}
/* MSG_WAITALL: io_uring retries a short send itself, so the completion means all or nothing. */
static inline void sqe_send(struct io_uring_sqe *s, unsigned slot, const void *p, unsigned n, uint64_t ud) {
  s->opcode = IORING_OP_SEND;
  s->flags = IOSQE_FIXED_FILE;
  s->fd = (int)slot;
  s->addr = (uint64_t)(uintptr_t)p;
  s->len = n;
  s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
  s->user_data = ud;
}
/* shutdown(SHUT_RDWR) ends the receive in flight (it holds the socket open past the slot's
 * close), hard-linked to the close of the slot. */
static inline void sqe_shutdown_close(struct ring *r, unsigned slot, uint64_t ud_shutdown, uint64_t ud_close) {
  struct io_uring_sqe *s = ring_sqe(r);
  s->opcode = IORING_OP_SHUTDOWN;
  s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  s->fd = (int)slot;
  s->len = SHUT_RDWR;
  s->user_data = ud_shutdown;
  s = ring_sqe(r);
  s->opcode = IORING_OP_CLOSE;
  s->fd = 0;
  s->file_index = slot + 1;
  s->user_data = ud_close;
}
static inline void sqe_cancel_ud(struct io_uring_sqe *s, uint64_t target, uint64_t ud) {
  s->opcode = IORING_OP_ASYNC_CANCEL;
  s->addr = target;
  s->user_data = ud;
}
/* A listening socket on host:port: SO_REUSEPORT (several share the port), TCP_NODELAY (inherited
 * by accepted sockets). */
static int listen_socket(const char *host, int port, int backlog, const char **err) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) { *err = "socket"; return -1; }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) { *err = "SO_REUSEPORT"; close(fd); return -1; }
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { *err = "HOST must be an IPv4 address"; close(fd); return -1; }
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { *err = "bind"; close(fd); return -1; }
  if (listen(fd, backlog) < 0) { *err = "listen"; close(fd); return -1; }
  return fd;
}

#endif
