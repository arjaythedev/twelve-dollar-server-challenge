#define _GNU_SOURCE
#include "ring.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(IORING_SETUP_SINGLE_ISSUER) && \
    defined(IORING_SETUP_DEFER_TASKRUN) && \
    defined(IORING_SETUP_SUBMIT_ALL) && \
    defined(IORING_SETUP_TASKRUN_FLAG) && defined(IORING_SQ_TASKRUN)
#define RING_DEFER_TASKRUN_SETUP_FLAGS \
    (IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | \
     IORING_SETUP_SUBMIT_ALL | IORING_SETUP_TASKRUN_FLAG)
#else
#define RING_DEFER_TASKRUN_SETUP_FLAGS 0U
#endif
#ifndef IORING_SQ_TASKRUN
#define IORING_SQ_TASKRUN 0U
#endif

typedef struct ring_state {
    int fd;
    void *sq_ring;
    void *cq_ring;
    void *sqes_map;
    size_t sq_ring_size;
    size_t cq_ring_size;
    size_t sqes_size;
    int single_mmap;

    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_flags;
    unsigned *sq_array;
    unsigned *cq_head;
    unsigned *cq_tail;

    unsigned sq_entries;
    unsigned sq_mask;
    unsigned cq_entries;
    unsigned cq_mask;
    unsigned sqe_tail;
    unsigned published_tail;
    unsigned pending_submit;
    unsigned cq_head_local;
    unsigned defer_taskrun;
#ifdef RING_DIAG
    uint64_t diag_enter_syscalls;
    uint64_t diag_accepted_recv_sqes;
    uint64_t diag_accepted_send_sqes;
    uint64_t diag_accepted_other_sqes;
    ring_diag_send_accept_observed_callback diag_send_accept_observed_callback;
#endif
} ring_state;

static unsigned load_acquire(const unsigned *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_release(unsigned *p, unsigned value) {
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
}

static int ring_bytes(unsigned offset, unsigned entries, size_t entry_size,
                      size_t *out) {
    size_t base = (size_t)offset;
    size_t count = (size_t)entries;
    if (entry_size != 0 && count > (SIZE_MAX - base) / entry_size) {
        return -EOVERFLOW;
    }
    *out = base + count * entry_size;
    return 0;
}

static int is_power_of_two(unsigned n) {
    return n != 0U && (n & (n - 1U)) == 0U;
}

static int setup_ring(unsigned entries, struct io_uring_params *params) {
#if defined(SYS_io_uring_setup)
    for (;;) {
        long fd = syscall(SYS_io_uring_setup, entries, params);
        if (fd >= 0) return (int)fd;
        if (errno == EINTR) continue;
        return -errno;
    }
#else
    (void)entries;
    (void)params;
    return -ENOSYS;
#endif
}

static int enter_ring(ring_state *s, int fd, unsigned to_submit,
                      unsigned min_complete, unsigned flags) {
#if defined(SYS_io_uring_enter)
    for (;;) {
#ifdef RING_DIAG
        ++s->diag_enter_syscalls;
#else
        (void)s;
#endif
        long ret = syscall(SYS_io_uring_enter, fd, to_submit, min_complete,
                           flags, NULL, (size_t)0);
        if (ret >= 0) {
            if ((unsigned long)ret > (unsigned long)INT32_MAX) return -EOVERFLOW;
            return (int)ret;
        }
        if (errno == EINTR) continue;
        return -errno;
    }
#else
    (void)s;
    (void)fd;
    (void)to_submit;
    (void)min_complete;
    (void)flags;
    return -ENOSYS;
#endif
}

static void unmap_state(ring_state *s) {
    if (s->sqes_map != NULL) {
        (void)munmap(s->sqes_map, s->sqes_size);
        s->sqes_map = NULL;
    }
    if (s->single_mmap) {
        if (s->sq_ring != NULL) {
            (void)munmap(s->sq_ring, s->sq_ring_size);
            s->sq_ring = NULL;
            s->cq_ring = NULL;
        }
    } else {
        if (s->cq_ring != NULL) {
            (void)munmap(s->cq_ring, s->cq_ring_size);
            s->cq_ring = NULL;
        }
        if (s->sq_ring != NULL) {
            (void)munmap(s->sq_ring, s->sq_ring_size);
            s->sq_ring = NULL;
        }
    }
}

static void destroy_state(ring_state *s) {
    if (s == NULL) return;
    unmap_state(s);
    if (s->fd >= 0) {
        (void)close(s->fd);
        s->fd = -1;
    }
    free(s);
}

static ring_state *get_state(ring *r) {
    if (r == NULL) return NULL;
    return (ring_state *)r->_state;
}

int ring_init(ring *r, unsigned sq_entries, unsigned cq_entries) {
    if (r == NULL || sq_entries == 0U) return -EINVAL;

    /* Permit a fresh, uninitialized stack object; re-init live rings via close. */
    r->_state = NULL;

    ring_state *s = (ring_state *)calloc(1, sizeof(*s));
    if (s == NULL) return -ENOMEM;
    s->fd = -1;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    unsigned base_flags = 0U;
    if (cq_entries != 0U) {
        base_flags |= IORING_SETUP_CQSIZE;
        params.cq_entries = cq_entries;
    }
    params.flags = base_flags | RING_DEFER_TASKRUN_SETUP_FLAGS;

    int fd = setup_ring(sq_entries, &params);
    unsigned defer_taskrun = fd >= 0 && RING_DEFER_TASKRUN_SETUP_FLAGS != 0U;
    if (fd < 0 && RING_DEFER_TASKRUN_SETUP_FLAGS != 0U) {
        /* Older or restricted kernels can still use the ordinary ring path. */
        memset(&params, 0, sizeof(params));
        params.flags = base_flags;
        params.cq_entries = cq_entries;
        fd = setup_ring(sq_entries, &params);
    }
    if (fd < 0) {
        destroy_state(s);
        return fd;
    }
    s->fd = fd;
    s->defer_taskrun = defer_taskrun;

    if (!is_power_of_two(params.sq_entries) ||
        !is_power_of_two(params.cq_entries)) {
        destroy_state(s);
        return -EPROTO;
    }

    s->sq_entries = params.sq_entries;
    s->sq_mask = params.sq_entries - 1U;
    s->cq_entries = params.cq_entries;
    s->cq_mask = params.cq_entries - 1U;

    int rc = ring_bytes(params.sq_off.array, params.sq_entries,
                        sizeof(unsigned), &s->sq_ring_size);
    if (rc == 0) {
        rc = ring_bytes(params.cq_off.cqes, params.cq_entries,
                        sizeof(struct io_uring_cqe), &s->cq_ring_size);
    }
    if (rc == 0) {
        rc = ring_bytes(0U, params.sq_entries, sizeof(struct io_uring_sqe),
                        &s->sqes_size);
    }
    if (rc != 0) {
        destroy_state(s);
        return rc;
    }

    if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0U) {
        size_t map_size = s->sq_ring_size > s->cq_ring_size
                              ? s->sq_ring_size
                              : s->cq_ring_size;
        s->sq_ring = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          s->fd, (off_t)IORING_OFF_SQ_RING);
        if (s->sq_ring == MAP_FAILED) {
            s->sq_ring = NULL;
            rc = -errno;
            destroy_state(s);
            return rc;
        }
        s->cq_ring = s->sq_ring;
        s->single_mmap = 1;
        s->sq_ring_size = map_size;
        s->cq_ring_size = map_size;
    } else {
        s->sq_ring = mmap(NULL, s->sq_ring_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, s->fd, (off_t)IORING_OFF_SQ_RING);
        if (s->sq_ring == MAP_FAILED) {
            s->sq_ring = NULL;
            rc = -errno;
            destroy_state(s);
            return rc;
        }
        s->cq_ring = mmap(NULL, s->cq_ring_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, s->fd, (off_t)IORING_OFF_CQ_RING);
        if (s->cq_ring == MAP_FAILED) {
            s->cq_ring = NULL;
            rc = -errno;
            destroy_state(s);
            return rc;
        }
    }

    s->sqes_map = mmap(NULL, s->sqes_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, s->fd, (off_t)IORING_OFF_SQES);
    if (s->sqes_map == MAP_FAILED) {
        s->sqes_map = NULL;
        rc = -errno;
        destroy_state(s);
        return rc;
    }

    char *sq_base = (char *)s->sq_ring;
    char *cq_base = (char *)s->cq_ring;
    s->sq_head = (unsigned *)(void *)(sq_base + params.sq_off.head);
    s->sq_tail = (unsigned *)(void *)(sq_base + params.sq_off.tail);
    s->sq_flags = (unsigned *)(void *)(sq_base + params.sq_off.flags);
    s->sq_array = (unsigned *)(void *)(sq_base + params.sq_off.array);
    s->cq_head = (unsigned *)(void *)(cq_base + params.cq_off.head);
    s->cq_tail = (unsigned *)(void *)(cq_base + params.cq_off.tail);
    s->sqes = (struct io_uring_sqe *)s->sqes_map;
    s->cqes = (struct io_uring_cqe *)(void *)(cq_base + params.cq_off.cqes);

    s->sqe_tail = load_acquire(s->sq_tail);
    s->published_tail = s->sqe_tail;
    s->cq_head_local = load_acquire(s->cq_head);
    r->_state = s;
    return 0;
}

int ring_register_files_sparse(ring *r, unsigned slots) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
    if (slots == 0U) return -EINVAL;
#if defined(SYS_io_uring_register)
    struct io_uring_rsrc_register reg;
    memset(&reg, 0, sizeof(reg));
    reg.nr = slots;
    reg.flags = IORING_RSRC_REGISTER_SPARSE;
    long ret = syscall(SYS_io_uring_register, s->fd, IORING_REGISTER_FILES2,
                       &reg, sizeof(reg));
    if (ret < 0) return -errno;
    return (int)ret;
#else
    (void)slots;
    return -ENOSYS;
#endif
}

int ring_unregister_files(ring *r) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
#if defined(SYS_io_uring_register)
    long ret = syscall(SYS_io_uring_register, s->fd, IORING_UNREGISTER_FILES,
                       NULL, 0U);
    if (ret < 0) return -errno;
    return (int)ret;
#else
    return -ENOSYS;
#endif
}

int ring_clear_file(ring *r, unsigned slot) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
#if defined(SYS_io_uring_register)
    int empty = -1;
    struct io_uring_files_update update;
    memset(&update, 0, sizeof(update));
    update.offset = slot;
    update.fds = (uint64_t)(uintptr_t)&empty;
    long ret = syscall(SYS_io_uring_register, s->fd, IORING_REGISTER_FILES_UPDATE,
                       &update, 1U);
    if (ret < 0) return -errno;
    return (int)ret;
#else
    (void)slot;
    return -ENOSYS;
#endif
}

int ring_register_eventfd(ring *r, int event_fd) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
#if defined(SYS_io_uring_register)
    long ret = syscall(SYS_io_uring_register, s->fd, IORING_REGISTER_EVENTFD,
                       &event_fd, 1U);
    if (ret < 0) return -errno;
    return (int)ret;
#else
    (void)event_fd;
    return -ENOSYS;
#endif
}

int ring_register_buf_ring(ring *r, struct io_uring_buf_ring *buf_ring,
                           unsigned entries, unsigned group) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
    if (buf_ring == NULL || entries == 0U || group > UINT16_MAX) return -EINVAL;
#if defined(SYS_io_uring_register)
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = (uint64_t)(uintptr_t)buf_ring;
    reg.ring_entries = entries;
    reg.bgid = (uint16_t)group;
    long ret = syscall(SYS_io_uring_register, s->fd, IORING_REGISTER_PBUF_RING,
                       &reg, 1U);
    if (ret < 0 && errno == EINVAL) {
        /* Ubuntu 6.8.0-136 through -147 required a nonzero reserved field. */
        reg.resv[0] = 1U;
        ret = syscall(SYS_io_uring_register, s->fd, IORING_REGISTER_PBUF_RING,
                      &reg, 1U);
    }
    if (ret < 0) return -errno;
    return (int)ret;
#else
    (void)buf_ring;
    (void)entries;
    (void)group;
    return -ENOSYS;
#endif
}

int ring_unregister_buf_ring(ring *r, unsigned group) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
    if (group > UINT16_MAX) return -EINVAL;
#if defined(SYS_io_uring_register)
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.bgid = (uint16_t)group;
    long ret = syscall(SYS_io_uring_register, s->fd,
                       IORING_UNREGISTER_PBUF_RING, &reg, 1U);
    if (ret < 0) return -errno;
    return (int)ret;
#else
    (void)group;
    return -ENOSYS;
#endif
}

struct io_uring_sqe *ring_get_sqe(ring *r) {
    ring_state *s = get_state(r);
    if (s == NULL) return NULL;

    unsigned head = load_acquire(s->sq_head);
    if ((unsigned)(s->sqe_tail - head) >= s->sq_entries) return NULL;

    unsigned index = s->sqe_tail & s->sq_mask;
    struct io_uring_sqe *sqe = &s->sqes[index];
    memset(sqe, 0, sizeof(*sqe));
    s->sqe_tail++;
    return sqe;
}

int ring_submit(ring *r) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;

    unsigned new_sqes = (unsigned)(s->sqe_tail - s->published_tail);
    if (new_sqes > s->sq_entries) return -EOVERFLOW;
    if (new_sqes != 0U) {
        for (unsigned i = 0; i < new_sqes; ++i) {
            unsigned tail = s->published_tail + i;
            s->sq_array[tail & s->sq_mask] = tail & s->sq_mask;
        }
        s->published_tail = s->sqe_tail;
        store_release(s->sq_tail, s->published_tail);
        s->pending_submit += new_sqes;
    }
    unsigned head_before = load_acquire(s->sq_head);
    unsigned queued_before = (unsigned)(s->published_tail - head_before);
    s->pending_submit = queued_before;
    if (queued_before == 0U) {
        if (!s->defer_taskrun ||
            (load_acquire(s->sq_flags) & IORING_SQ_TASKRUN) == 0U)
            return 0;
        int taskrun_rc = enter_ring(s, s->fd, 0U, 0U,
                                    IORING_ENTER_GETEVENTS);
        return taskrun_rc < 0 ? taskrun_rc : 0;
    }

    unsigned enter_flags = s->defer_taskrun ? IORING_ENTER_GETEVENTS : 0U;
    int ret = enter_ring(s, s->fd, queued_before, 0U, enter_flags);
    unsigned head_after = load_acquire(s->sq_head);
    unsigned accepted = (unsigned)(head_after - head_before);
    unsigned queued_after = (unsigned)(s->published_tail - head_after);
    if (queued_after > s->sq_entries || accepted > queued_before) return -EPROTO;
#ifdef RING_DIAG
    for (unsigned i = 0; i < accepted; ++i) {
        unsigned sq_index = (head_before + i) & s->sq_mask;
        unsigned sqe_index = s->sq_array[sq_index] & s->sq_mask;
        unsigned opcode = s->sqes[sqe_index].opcode;
        if (opcode == IORING_OP_RECV) ++s->diag_accepted_recv_sqes;
        else if (opcode == IORING_OP_SEND) {
            ++s->diag_accepted_send_sqes;
            if (s->diag_send_accept_observed_callback)
                s->diag_send_accept_observed_callback(s->sqes[sqe_index].user_data);
        }
        else ++s->diag_accepted_other_sqes;
    }
#endif
    s->pending_submit = queued_after;

    /* The shared SQ head is authoritative even if enter was interrupted. */
    if (accepted != 0U) {
        if (accepted > (unsigned)INT32_MAX) return -EOVERFLOW;
        return (int)accepted;
    }
    if (ret < 0) return ret;
    if (ret == 0) return -EAGAIN;
    return -EPROTO;
}

int ring_defer_taskrun_enabled(ring *r) {
    ring_state *s = get_state(r);
    return s != NULL && s->defer_taskrun != 0U;
}

int ring_cq_overflow_pending(ring *r) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
    return (load_acquire(s->sq_flags) & IORING_SQ_CQ_OVERFLOW) != 0U ? 1 : 0;
}

int ring_flush_cq_overflow(ring *r) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
    if ((load_acquire(s->sq_flags) & IORING_SQ_CQ_OVERFLOW) == 0U) return 0;
    int rc = enter_ring(s, s->fd, 0U, 0U, IORING_ENTER_GETEVENTS);
    return rc < 0 ? rc : 0;
}

unsigned ring_sq_pending(ring *r) {
    ring_state *s = get_state(r);
    if (s == NULL) return 0U;
    unsigned head = load_acquire(s->sq_head);
    return (unsigned)(s->published_tail - head);
}

static unsigned cq_ready_count(ring_state *s) {
    unsigned tail = load_acquire(s->cq_tail);
    return (unsigned)(tail - s->cq_head_local);
}

int ring_wait(ring *r, unsigned min_complete) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;
    if (min_complete > s->cq_entries) return -EINVAL;

    unsigned ready = cq_ready_count(s);
    if (ready >= min_complete) {
        if (ready > (unsigned)INT32_MAX) return -EOVERFLOW;
        return (int)ready;
    }
    if (min_complete == 0U) return (int)ready;

    for (;;) {
        int ret = enter_ring(s, s->fd, 0U, min_complete,
                             IORING_ENTER_GETEVENTS);
        if (ret < 0) {
            if (ret == -EINTR) {
                ready = cq_ready_count(s);
                if (ready >= min_complete) {
                    if (ready > (unsigned)INT32_MAX) return -EOVERFLOW;
                    return (int)ready;
                }
                continue;
            }
            return ret;
        }
        ready = cq_ready_count(s);
        if (ready >= min_complete) {
            if (ready > (unsigned)INT32_MAX) return -EOVERFLOW;
            return (int)ready;
        }
    }
}

unsigned ring_cq_ready(ring *r) {
    ring_state *s = get_state(r);
    if (s == NULL) return 0U;
    unsigned ready = cq_ready_count(s);
    return ready <= s->cq_entries ? ready : 0U;
}

struct io_uring_cqe *ring_peek_cqe_at(ring *r, unsigned offset) {
    ring_state *s = get_state(r);
    if (s == NULL) return NULL;
    unsigned ready = cq_ready_count(s);
    if (ready > s->cq_entries || offset >= ready) return NULL;
    return &s->cqes[(s->cq_head_local + offset) & s->cq_mask];
}

struct io_uring_cqe *ring_peek_cqe(ring *r) {
    return ring_peek_cqe_at(r, 0U);
}

int ring_cq_advance(ring *r, unsigned n) {
    ring_state *s = get_state(r);
    if (s == NULL) return -EBADF;

    unsigned available = cq_ready_count(s);
    if (n > available) return -EINVAL;
    s->cq_head_local += n;
    store_release(s->cq_head, s->cq_head_local);
    return 0;
}

void ring_close(ring *r) {
    if (r == NULL) return;
    ring_state *s = get_state(r);
    r->_state = NULL;
    destroy_state(s);
}

#ifdef RING_DIAG
void ring_diag_snapshot(ring *r, ring_diag *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    ring_state *s = get_state(r);
    if (s == NULL) return;
    out->enter_syscalls = s->diag_enter_syscalls;
    out->accepted_recv_sqes = s->diag_accepted_recv_sqes;
    out->accepted_send_sqes = s->diag_accepted_send_sqes;
    out->accepted_other_sqes = s->diag_accepted_other_sqes;
}

void ring_diag_set_send_accept_observed_callback(
    ring *r, ring_diag_send_accept_observed_callback callback) {
    ring_state *s = get_state(r);
    if (s != NULL) s->diag_send_accept_observed_callback = callback;
}
#endif
