#ifndef CHALLENGE_RING_H
#define CHALLENGE_RING_H

#include <linux/io_uring.h>

#include <stdbool.h>
#include <stddef.h>
#ifdef RING_DIAG
#include <stdint.h>
#endif

/*
 * Small raw io_uring wrapper. The state is intentionally opaque to callers;
 * initialize a ring before using it and call ring_close() before reinitializing
 * one that is already live. A ring supports one producer and one CQ consumer;
 * callers must not use its operations concurrently from multiple threads.
 */
typedef struct ring {
    void *_state;
} ring;

#define RING_INITIALIZER { NULL }

/*
 * All functions returning int return a nonnegative result on success or a
 * negative errno value on failure (including EPERM/ENOMEM from setup or enter).
 * cq_entries == 0 asks the kernel for its default CQ size. ring_get_sqe()
 * returns NULL when the SQ is full; submit queued SQEs and retry. ring_wait()
 * returns the number of CQEs currently available once at least min_complete
 * are ready. ring_cq_advance() returns -EINVAL if asked to advance past the
 * available CQEs.
 */
int ring_init(ring *r, unsigned sq_entries, unsigned cq_entries);
int ring_init_ex(ring *r, unsigned sq_entries, unsigned cq_entries,
                 bool defer_taskrun);
int ring_register_files_sparse(ring *r, unsigned slots);
int ring_unregister_files(ring *r);
int ring_clear_file(ring *r, unsigned slot);
int ring_register_eventfd(ring *r, int event_fd);
int ring_register_buf_ring(ring *r, struct io_uring_buf_ring *buf_ring,
                           unsigned entries, unsigned group);
int ring_unregister_buf_ring(ring *r, unsigned group);
struct io_uring_sqe *ring_get_sqe(ring *r);
int ring_submit(ring *r);
/* Whether this ring was created with deferred task running enabled. */
int ring_defer_taskrun_enabled(ring *r);
/* Whether the kernel has completion entries waiting outside the shared CQ. */
int ring_cq_overflow_pending(ring *r);
/* Ask the kernel to move pending overflow CQEs into the shared CQ ring. */
int ring_flush_cq_overflow(ring *r);
/* SQEs visible to userspace but not yet consumed by the kernel. */
unsigned ring_sq_pending(ring *r);
int ring_wait(ring *r, unsigned min_complete);
/* Wait for min_complete CQEs, or return -ETIME after timeout_ms. */
int ring_wait_timeout(ring *r, unsigned min_complete, unsigned timeout_ms);
unsigned ring_cq_ready(ring *r);
struct io_uring_cqe *ring_peek_cqe(ring *r);
struct io_uring_cqe *ring_peek_cqe_at(ring *r, unsigned offset);
int ring_cq_advance(ring *r, unsigned n);
void ring_close(ring *r);

#ifdef RING_DIAG
typedef struct {
    uint64_t enter_syscalls;
    uint64_t accepted_recv_sqes;
    uint64_t accepted_send_sqes;
    uint64_t accepted_other_sqes;
} ring_diag;

typedef void (*ring_diag_send_accept_observed_callback)(uint64_t user_data);

void ring_diag_snapshot(ring *r, ring_diag *out);
void ring_diag_set_send_accept_observed_callback(
    ring *r, ring_diag_send_accept_observed_callback callback);
#endif

#endif
