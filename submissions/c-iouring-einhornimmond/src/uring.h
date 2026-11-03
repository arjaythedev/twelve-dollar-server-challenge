/* io_uring through the system calls and the kernel's own header: the two rings, a ring of
 * provided buffers, and a table of direct descriptors. One thread sets the ring up and is the
 * only one that ever touches it (IORING_SETUP_SINGLE_ISSUER). Needs Linux 6.1. */
#ifndef URING_H
#define URING_H

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <linux/io_uring.h>

typedef struct uring {
    int fd;
    /* submissions: the tail is ours and is published when the kernel is entered */
    unsigned *sq_head, *sq_ktail, sq_mask, sq_entries, sq_tail, sq_submitted;
    struct io_uring_sqe *sqes;
    /* completions */
    unsigned *cq_head, *cq_tail, cq_mask;
    struct io_uring_cqe *cqes;
    /* provided buffers: `buf_count` buffers of `buf_size` bytes, lent to receives. The kernel
     * is offered `buf_spare` bytes less of each, so that many stay free behind whatever it
     * puts there. */
    struct io_uring_buf_ring *br;
    uint8_t *buf_base;
    unsigned buf_count, buf_size, buf_spare, br_tail;
} uring;

static inline int uring_enter_raw(int fd, unsigned submit, unsigned wait, unsigned flags,
                                  const void *arg, size_t arg_size)
{
    return (int)syscall(__NR_io_uring_enter, fd, submit, wait, flags, arg, arg_size);
}

/* 0, or -1 with errno set. */
static inline int uring_init(uring *r, unsigned sq_entries, unsigned cq_entries)
{
    struct io_uring_params p;
    size_t sq_size, cq_size;
    uint8_t *sq, *cq;

    memset(&p, 0, sizeof p);
    /* The loop is the only thread that submits, and completions are handed over when it asks
     * for them and not whenever the kernel has one: no interrupt in the middle of a request. */
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN |
              IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = cq_entries;
    if ((r->fd = (int)syscall(__NR_io_uring_setup, sq_entries, &p)) < 0)
        return -1;

    sq_size = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP)
        sq_size = cq_size = sq_size > cq_size ? sq_size : cq_size;
    sq = mmap(NULL, sq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
    if (sq == MAP_FAILED)
        return -1;
    cq = sq;
    if (!(p.features & IORING_FEAT_SINGLE_MMAP)) {
        cq = mmap(NULL, cq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
        if (cq == MAP_FAILED)
            return -1;
    }
    r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED)
        return -1;

    r->sq_head = (unsigned *)(sq + p.sq_off.head);
    r->sq_ktail = (unsigned *)(sq + p.sq_off.tail);
    r->sq_mask = *(unsigned *)(sq + p.sq_off.ring_mask);
    r->sq_entries = p.sq_entries;
    r->sq_tail = r->sq_submitted = *r->sq_ktail;
    /* Entry i of the ring is entry i of the array, for good. */
    for (unsigned i = 0; i < p.sq_entries; i++)
        ((unsigned *)(sq + p.sq_off.array))[i] = i;

    r->cq_head = (unsigned *)(cq + p.cq_off.head);
    r->cq_tail = (unsigned *)(cq + p.cq_off.tail);
    r->cq_mask = *(unsigned *)(cq + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);
    return 0;
}

/* Hands what is queued to the kernel and waits for @p wait completions, at most @p timeout_ns.
 * A timeout and an interruption are not errors. 0, or -1 with errno set. */
static inline int uring_enter(uring *r, unsigned wait, int64_t timeout_ns)
{
    struct __kernel_timespec ts = {.tv_sec = timeout_ns / 1000000000, .tv_nsec = timeout_ns % 1000000000};
    struct io_uring_getevents_arg arg = {.sigmask_sz = _NSIG / 8, .ts = (uint64_t)(uintptr_t)&ts};

    __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
    for (;;) {
        const unsigned submit = r->sq_tail - r->sq_submitted;
        const int n = uring_enter_raw(r->fd, submit, wait, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                                      &arg, sizeof arg);

        if (n < 0)
            /* EBUSY: completions are backed up and want to be read before more is taken. */
            return errno == ETIME || errno == EINTR || errno == EBUSY ? 0 : -1;
        r->sq_submitted += (unsigned)n;
        if ((unsigned)n == submit)
            return 0;
    }
}

/* The next free submission entry, zeroed. When the ring is full it is handed over first. */
static inline struct io_uring_sqe *uring_sqe(uring *r)
{
    struct io_uring_sqe *sqe;

    while (r->sq_tail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >= r->sq_entries) {
        __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
        const int n = uring_enter_raw(r->fd, r->sq_tail - r->sq_submitted, 0, IORING_ENTER_GETEVENTS, NULL, 0);
        if (n > 0)
            r->sq_submitted += (unsigned)n;
        else if (n < 0 && errno != EINTR && errno != EBUSY && errno != EAGAIN)
            return NULL;
    }
    sqe = &r->sqes[r->sq_tail++ & r->sq_mask];
    memset(sqe, 0, sizeof *sqe);
    return sqe;
}

/* A table of @p slots direct descriptors, all empty: accepted sockets live there and not in the
 * process's descriptor table. 0, or -1 with errno set. */
static inline int uring_register_table(uring *r, unsigned slots)
{
    struct io_uring_rsrc_register reg = {.nr = slots, .flags = IORING_RSRC_REGISTER_SPARSE};

    return (int)syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_FILES2, &reg, sizeof reg);
}

static inline void uring_buffer_return(uring *r, unsigned id)
{
    struct io_uring_buf *buf = &r->br->bufs[r->br_tail & (r->buf_count - 1)];

    buf->addr = (uint64_t)(uintptr_t)(r->buf_base + (size_t)id * r->buf_size);
    buf->len = r->buf_size - r->buf_spare;
    buf->bid = (uint16_t)id;
    r->br_tail++;
    __atomic_store_n(&r->br->tail, (uint16_t)r->br_tail, __ATOMIC_RELEASE);
}

/* Buffer group 0: @p count (a power of two, at most 32768) buffers of @p size bytes, of which
 * the last @p spare are never received into. */
static inline int uring_register_buffers(uring *r, unsigned count, unsigned size, unsigned spare)
{
    struct io_uring_buf_reg reg = {.ring_entries = count, .bgid = 0};

    r->br = mmap(NULL, count * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    r->buf_base = mmap(NULL, (size_t)count * size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->br == MAP_FAILED || r->buf_base == MAP_FAILED)
        return -1;
    reg.ring_addr = (uint64_t)(uintptr_t)r->br;
    if (syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, &reg, 1) != 0)
        return -1;
    r->buf_count = count;
    r->buf_size = size;
    r->buf_spare = spare;
    r->br_tail = 0;
    for (unsigned i = 0; i < count; i++)
        uring_buffer_return(r, i);
    return 0;
}

#endif
