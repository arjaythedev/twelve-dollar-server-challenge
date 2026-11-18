#ifndef BINARYCORE_IO_URING_H
#define BINARYCORE_IO_URING_H

#include <stddef.h>
#include <stdint.h>
#include <linux/io_uring.h>

/* Raw io_uring wrapper using direct syscalls (no liburing).
 * Modeled on c-iouring-pavlyhalim/src/sys.h but organized as a module. */

/* Capability flags detected at init time. */
#define bc_uring_have_pbuf_ring   (1u << 0)
#define bc_uring_have_accept_ms   (1u << 1)
#define bc_uring_have_recv_ms     (1u << 2)
#define bc_uring_have_buf_select  (1u << 3)

struct bc_uring_ring {
    int fd;
    unsigned sq_entries, cq_entries, sq_mask, cq_mask;
    unsigned sq_local_tail;
    unsigned *sq_head, *sq_tail, *sq_flags, *sq_array;
    unsigned *cq_head, *cq_tail;
    struct io_uring_cqe *cqes;
    struct io_uring_sqe *sqes;
    void *sq_ptr, *cq_ptr;
    size_t sq_sz, cq_sz, sqes_sz;
    /* Provided buffers */
    struct io_uring_buf_ring *br;
    unsigned br_entries, br_mask, br_tail;
    unsigned buf_size;
    uint8_t *bufs;
    unsigned bufs_out;
    /* Direct descriptor table */
    unsigned nslots;
    int eventfd;          /* -1 for ring 0; eventfd for rings 1..N */
    unsigned capabilities;
};

struct bc_uring {
    struct bc_uring_ring *rings;
    unsigned n_rings;
    int listen_fds[8];
    unsigned n_listen;
};

typedef void (*bc_uring_cqe_cb)(struct bc_uring_ring *ring,
                                unsigned slot, uint64_t user_data,
                                struct io_uring_cqe *cqe, void *arg);

int bc_uring_init(struct bc_uring *u, unsigned n_rings,
                  unsigned sq_entries, unsigned cq_entries,
                  unsigned nslots, unsigned nbufs, unsigned buf_size,
                  const char **err);

void bc_uring_cleanup(struct bc_uring *u);

/* Socket setup. Returns a listening fd (non-blocking, TCP_NODELAY, SO_REUSEADDR,
 * SO_REUSEPORT) or -1 on error. */
int bc_uring_listen_socket(const char *host, int port, int backlog, const char **err);

/* Register a listening fd for multishot accept on a ring. */
int bc_uring_accept_register(struct bc_uring_ring *r, int listen_fd,
                             unsigned listener_index, void *arg);

/* Arm a multishot poll on a connection slot. `slot` is the direct-descriptor
 * index (used as fd with IOSQE_FIXED_FILE). */
int bc_uring_poll_conn(struct bc_uring_ring *r, unsigned slot, uint64_t user_data);

/* Issue a one-shot recv into a provided buffer. */
int bc_uring_recv_once(struct bc_uring_ring *r, unsigned slot, uint64_t user_data);

/* Issue a send. */
int bc_uring_send(struct bc_uring_ring *r, unsigned slot, const void *p,
                  unsigned n, uint64_t user_data);

/* Cancel a request by user_data. */
int bc_uring_cancel(struct bc_uring_ring *r, uint64_t target, uint64_t user_data);

/* Shutdown + close a connection slot (hard-linked cancel/shutdown/close). */
int bc_uring_shutdown_close(struct bc_uring_ring *r, unsigned slot,
                            uint64_t close_ud, uint64_t poll_ud, uint64_t cancel_ud);

/* Submit pending SQEs on a ring. Returns io_uring_enter result (negative =
 * -errno). */
long bc_uring_submit(struct bc_uring_ring *r, unsigned min_complete,
                     unsigned flags, const void *arg, size_t argsz);

/* Wait for completions on ring 0. Returns io_uring_enter result. */
long bc_uring_wait(struct bc_uring_ring *r, unsigned min, unsigned ms);

/* Reap completions, invoking cb for each. Returns number of CQEs reaped. */
unsigned bc_uring_reap(struct bc_uring_ring *r, bc_uring_cqe_cb cb, void *arg);

/* Return a buffer to the kernel's provided-buffer ring and publish. */
static inline void bc_uring_buf_return(struct bc_uring_ring *r, unsigned bid) {
    struct io_uring_buf *b = &r->br->bufs[r->br_tail & r->br_mask];
    b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)bid * r->buf_size);
    b->len = r->buf_size;
    b->bid = (uint16_t)bid;
    r->br_tail++;
    r->bufs_out--;
}

static inline void bc_uring_buf_publish(struct bc_uring_ring *r) {
    /* release-store the tail so the kernel sees the new buffers */
    __atomic_store_n(&r->br->tail, (uint16_t)r->br_tail, __ATOMIC_RELEASE);
}

static inline uint8_t *bc_uring_buf_ptr(struct bc_uring_ring *r, unsigned bid) {
    return r->bufs + (size_t)bid * r->buf_size;
}

static inline unsigned bc_uring_pending_submit(struct bc_uring_ring *r) {
    return r->sq_local_tail - __atomic_load_n(&r->sq_head, __ATOMIC_ACQUIRE);
}

/* SQE accessor: returns the next SQE, submitting/waiting if the SQ is full. */
struct io_uring_sqe *bc_uring_sqe(struct bc_uring_ring *r);

#endif /* BINARYCORE_IO_URING_H */
