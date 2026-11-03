// Helpers declared in cuuring.h that need more than one liburing call.
#include "cuuring.h"
#include <string.h>

int tu_setup(struct io_uring *ring, unsigned entries, unsigned cq_entries) {
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN
        | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = cq_entries;
    return io_uring_queue_init_params(entries, ring, &p);
}

int tu_wait(struct io_uring *ring, unsigned min, long timeout_us) {
    struct io_uring_cqe *cqe;
    if (timeout_us <= 0) return io_uring_submit_and_wait(ring, min);
    struct __kernel_timespec ts = {.tv_sec = timeout_us / 1000000, .tv_nsec = (timeout_us % 1000000) * 1000};
    return io_uring_submit_and_wait_timeout(ring, &cqe, min, &ts, 0);
}

int tu_probe(void) {
    struct io_uring ring;
    if (tu_setup(&ring, 8, 16) != 0) return 0;
    io_uring_queue_exit(&ring);
    return 1;
}

struct io_uring_buf_ring *tu_buffers(struct io_uring *ring, unsigned count, unsigned short group) {
    int err = 0;
    struct io_uring_buf_ring *br = io_uring_setup_buf_ring(ring, count, group, 0, &err);
    return err ? 0 : br;
}

static pthread_mutex_t tu_mutex = PTHREAD_MUTEX_INITIALIZER;
void tu_lock(void) { pthread_mutex_lock(&tu_mutex); }
void tu_unlock(void) { pthread_mutex_unlock(&tu_mutex); }

int tu_spawn(void *(*body)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1 << 20);
    int rc = pthread_create(&t, &a, body, arg);
    pthread_attr_destroy(&a);
    return rc;
}
