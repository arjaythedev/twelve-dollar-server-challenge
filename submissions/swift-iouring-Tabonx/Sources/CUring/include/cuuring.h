// liburing plus small helpers for what Swift can't express: flag macros, SQE preparation with
// fixed files and buffer selection, and waiting with a timeout.
#ifndef CUURING_H
#define CUURING_H
#include "liburing.h"
#include <pthread.h>

enum {
    TU_CQE_F_MORE = IORING_CQE_F_MORE,
    TU_CQE_F_BUFFER = IORING_CQE_F_BUFFER,
    TU_CQE_BUFFER_SHIFT = IORING_CQE_BUFFER_SHIFT,
};

/// Creates a ring for one issuing thread that only runs completion work when it waits.
int tu_setup(struct io_uring *ring, unsigned entries, unsigned cq_entries);

/// Returns a free SQE, submitting the queue first if it is full.
static inline struct io_uring_sqe *tu_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    while (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

/// Multishot accept into the ring's registered file table (the kernel picks the slot).
static inline void tu_accept(struct io_uring *ring, int listener, unsigned long long tag) {
    struct io_uring_sqe *sqe = tu_sqe(ring);
    io_uring_prep_multishot_accept_direct(sqe, listener, 0, 0, 0);
    io_uring_sqe_set_data64(sqe, tag);
}

/// Multishot receive on a registered slot, taking buffers from buffer group `group`.
static inline void tu_recv(struct io_uring *ring, unsigned slot, unsigned short group, unsigned long long tag) {
    struct io_uring_sqe *sqe = tu_sqe(ring);
    io_uring_prep_recv_multishot(sqe, (int)slot, 0, 0, 0);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->buf_group = group;
    io_uring_sqe_set_data64(sqe, tag);
}

static inline void tu_send(struct io_uring *ring, unsigned slot, const void *p, unsigned len, unsigned long long tag) {
    struct io_uring_sqe *sqe = tu_sqe(ring);
    io_uring_prep_send(sqe, (int)slot, p, len, MSG_NOSIGNAL);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(sqe, tag);
}

static inline void tu_close(struct io_uring *ring, unsigned slot, unsigned long long tag) {
    struct io_uring_sqe *sqe = tu_sqe(ring);
    io_uring_prep_close_direct(sqe, slot);
    io_uring_sqe_set_data64(sqe, tag);
}

/// Cancels the request whose user_data is `target` (used to stop a multishot accept).
static inline void tu_cancel(struct io_uring *ring, unsigned long long target, unsigned long long tag) {
    struct io_uring_sqe *sqe = tu_sqe(ring);
    io_uring_prep_cancel64(sqe, target, 0);
    io_uring_sqe_set_data64(sqe, tag);
}

/// Submits, then waits until `min` completions are ready or `timeout_us` passes (0: no timeout).
int tu_wait(struct io_uring *ring, unsigned min, long timeout_us);

/// Provided-buffer ring: `count` buffers of `size` bytes starting at `base`.
struct io_uring_buf_ring *tu_buffers(struct io_uring *ring, unsigned count, unsigned short group);
static inline void tu_buffer_put(struct io_uring_buf_ring *br, void *addr, unsigned len, unsigned short bid,
                                 int mask, int offset) {
    io_uring_buf_ring_add(br, addr, len, bid, mask, offset);
}
static inline void tu_buffer_commit(struct io_uring_buf_ring *br, int count) { io_uring_buf_ring_advance(br, count); }
static inline int tu_buffer_mask(unsigned count) { return io_uring_buf_ring_mask(count); }

/// 1 if this process can create a ring with the flags tu_setup uses.
int tu_probe(void);

/// Threads and one mutex, for running several rings that share the SQLite connection.
int tu_spawn(void *(*body)(void *), void *arg);
void tu_lock(void);
void tu_unlock(void);
#endif
