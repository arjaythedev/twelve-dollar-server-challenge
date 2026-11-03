// Helpers declared in cuuring.h that need more than one liburing call.
#include "cuuring.h"
#include <string.h>
#include <sys/mman.h>

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
    if (br || err != -EINVAL) return br;
    // Ubuntu's 6.8.0-136 and later invert the check on the reserved words (Launchpad #2162843):
    // they refuse all zeroes and accept a nonzero resv[0]. A correct kernel never gets here.
    size_t size = count * sizeof(struct io_uring_buf);
    void *memory = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (memory == MAP_FAILED) return 0;
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr = (unsigned long)memory;
    reg.ring_entries = count;
    reg.bgid = group;
    reg.resv[0] = 1;
    if (io_uring_register_buf_ring(ring, &reg, 0) != 0) {
        munmap(memory, size);
        return 0;
    }
    br = memory;
    io_uring_buf_ring_init(br);
    return br;
}
