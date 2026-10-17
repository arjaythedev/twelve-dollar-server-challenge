#include "uring.h"

#include <errno.h>
#include <liburing.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_RINGS 4

// user_data values with this top byte are completions the shim handles itself.
#define UD_INTERNAL (0xffull << 56)
#define UD_IGNORE   (UD_INTERNAL | 1)
#define UD_WAKE     (UD_INTERNAL | 2)  // | ring << 8

static struct io_uring rings[MAX_RINGS];
static struct io_uring_buf_ring *bufring[MAX_RINGS];
static char *bufmem[MAX_RINGS];
static int wake_fd[MAX_RINGS];
static int nr, nbuf, bsize;

static struct io_uring_sqe *sqe_for(int r) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&rings[r]);
  if (!sqe) {
    io_uring_submit(&rings[r]);
    sqe = io_uring_get_sqe(&rings[r]);
  }
  return sqe;
}

// Rings other than ring 0 signal an eventfd on every completion; ring 0 polls those eventfds,
// so the loop only ever blocks on ring 0.
static int arm_wake(int r) {
  struct io_uring_sqe *sqe = sqe_for(0);
  if (!sqe) return -EBUSY;
  io_uring_prep_poll_multishot(sqe, wake_fd[r], POLLIN);
  io_uring_sqe_set_data64(sqe, UD_WAKE | ((uint64_t)r << 8));
  return 0;
}

static int init_ring(struct io_uring *ring) {
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
  p.cq_entries = 65536;
  int ret = io_uring_queue_init_params(4096, ring, &p);
  if (ret == -EINVAL) {  // kernels before 6.0 lack SINGLE_ISSUER
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_CQSIZE;
    p.cq_entries = 65536;
    ret = io_uring_queue_init_params(4096, ring, &p);
  }
  return ret;
}

int ur_setup(int nrings, int slots, int nbufs, int bufsize) {
  if (nrings < 1 || nrings > MAX_RINGS) return -EINVAL;
  nbuf = nbufs;
  bsize = bufsize;
  for (int r = 0; r < nrings; r++) {
    int ret = init_ring(&rings[r]);
    if (ret < 0) return ret;
    nr = r + 1;
    ret = io_uring_register_files_sparse(&rings[r], slots);
    if (ret < 0) return ret;
    if (posix_memalign((void **)&bufmem[r], 4096, (size_t)nbufs * bufsize)) return -ENOMEM;
    bufring[r] = io_uring_setup_buf_ring(&rings[r], nbufs, 0, 0, &ret);
    if (!bufring[r]) return ret;
    for (int i = 0; i < nbufs; i++)
      io_uring_buf_ring_add(bufring[r], bufmem[r] + (size_t)i * bufsize, bufsize, i,
                            io_uring_buf_ring_mask(nbufs), i);
    io_uring_buf_ring_advance(bufring[r], nbufs);
    if (r > 0) {
      wake_fd[r] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (wake_fd[r] < 0) return -errno;
      ret = io_uring_register_eventfd(&rings[r], wake_fd[r]);
      if (ret < 0) return ret;
      ret = arm_wake(r);
      if (ret < 0) return ret;
    }
  }
  return 0;
}

char *ur_bufs(int ring) { return bufmem[ring]; }

void ur_buf_return(int ring, int bid) {
  io_uring_buf_ring_add(bufring[ring], bufmem[ring] + (size_t)bid * bsize, bsize, bid,
                        io_uring_buf_ring_mask(nbuf), 0);
  io_uring_buf_ring_advance(bufring[ring], 1);
}

int ur_accept(int ring, int lfd, uint64_t ud) {
  struct io_uring_sqe *sqe = sqe_for(ring);
  if (!sqe) return -EBUSY;
  io_uring_prep_multishot_accept_direct(sqe, lfd, NULL, NULL, 0);
  io_uring_sqe_set_data64(sqe, ud);
  return 0;
}

int ur_cancel_accept(int ring, int lfd) {
  struct io_uring_sqe *sqe = sqe_for(ring);
  if (!sqe) return -EBUSY;
  io_uring_prep_cancel_fd(sqe, lfd, IORING_ASYNC_CANCEL_ALL);
  io_uring_sqe_set_data64(sqe, UD_IGNORE);
  return 0;
}

int ur_recv(int ring, int slot, uint64_t ud) {
  struct io_uring_sqe *sqe = sqe_for(ring);
  if (!sqe) return -EBUSY;
  io_uring_prep_recv_multishot(sqe, slot, NULL, 0, 0);
  sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  sqe->buf_group = 0;
  io_uring_sqe_set_data64(sqe, ud);
  return 0;
}

int ur_send(int ring, int slot, const void *p, int len, uint64_t ud) {
  struct io_uring_sqe *sqe = sqe_for(ring);
  if (!sqe) return -EBUSY;
  io_uring_prep_send(sqe, slot, p, len, MSG_NOSIGNAL | MSG_WAITALL);
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(sqe, ud);
  return 0;
}

int ur_close(int ring, int slot, uint64_t ud) {
  struct io_uring *r = &rings[ring];
  // The cancel and the close must go in the same submission to stay linked.
  if (io_uring_sq_space_left(r) < 2) io_uring_submit(r);
  struct io_uring_sqe *cancel = io_uring_get_sqe(r);
  struct io_uring_sqe *close = io_uring_get_sqe(r);
  if (!cancel || !close) return -EBUSY;
  // A pending multishot recv holds its own reference to the socket, so closing the slot alone
  // would leave the connection open. HARDLINK runs the close even when nothing was cancelled.
  io_uring_prep_cancel_fd(cancel, slot, IORING_ASYNC_CANCEL_FD_FIXED | IORING_ASYNC_CANCEL_ALL);
  cancel->flags |= IOSQE_IO_HARDLINK;
  io_uring_sqe_set_data64(cancel, UD_IGNORE);
  io_uring_prep_close_direct(close, slot);
  io_uring_sqe_set_data64(close, ud);
  return 0;
}

int ur_poll(int ring, int fd, uint64_t ud) {
  struct io_uring_sqe *sqe = sqe_for(ring);
  if (!sqe) return -EBUSY;
  io_uring_prep_poll_multishot(sqe, fd, POLLIN);
  io_uring_sqe_set_data64(sqe, ud);
  return 0;
}

static void internal(struct io_uring_cqe *cqe) {
  if ((cqe->user_data & ~0xff00ull) == UD_WAKE) {
    int r = (int)((cqe->user_data >> 8) & 0xff);
    uint64_t v;
    if (read(wake_fd[r], &v, sizeof v) < 0) { /* already drained */ }
    if (!(cqe->flags & IORING_CQE_F_MORE)) arm_wake(r);
  }
}

static int harvest(ur_cqe *out, int max) {
  int n = 0;
  for (int r = 0; r < nr && n < max; r++) {
    struct io_uring *ring = &rings[r];
    if (io_uring_cq_has_overflow(ring)) io_uring_get_events(ring);
    unsigned head, seen = 0;
    struct io_uring_cqe *cqe;
    io_uring_for_each_cqe(ring, head, cqe) {
      if (n == max) break;
      seen++;
      if ((cqe->user_data & UD_INTERNAL) == UD_INTERNAL) {
        internal(cqe);
        continue;
      }
      out[n].ud = cqe->user_data;
      out[n].res = cqe->res;
      out[n].flags = cqe->flags;
      n++;
    }
    io_uring_cq_advance(ring, seen);
  }
  return n;
}

int ur_wait(ur_cqe *out, int max) {
  for (int r = 1; r < nr; r++)
    if (io_uring_sq_ready(&rings[r])) io_uring_submit(&rings[r]);
  for (;;) {
    int n = harvest(out, max);
    if (n > 0) {
      if (io_uring_sq_ready(&rings[0])) io_uring_submit(&rings[0]);
      return n;
    }
    int ret = io_uring_submit_and_wait(&rings[0], 1);
    if (ret < 0 && ret != -EINTR && ret != -ETIME && ret != -EBUSY) return ret;
  }
}
