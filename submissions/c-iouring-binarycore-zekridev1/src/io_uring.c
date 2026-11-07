#define _GNU_SOURCE
#include "io_uring.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <linux/time_types.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

/* Constants the Ubuntu 24.04 headers have; defined here anyway so an older
 * header still builds. */
#ifndef IORING_SETUP_CQSIZE
#define IORING_SETUP_CQSIZE (1U << 3)
#endif
#ifndef IORING_SETUP_CLAMP
#define IORING_SETUP_CLAMP (1U << 4)
#endif
#ifndef IORING_SETUP_SUBMIT_ALL
#define IORING_SETUP_SUBMIT_ALL (1U << 7)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS (1U << 0)
#endif
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1U << 3)
#endif
#ifndef IORING_FEAT_SINGLE_MMAP
#define IORING_FEAT_SINGLE_MMAP (1U << 0)
#endif
#ifndef IORING_FEAT_EXT_ARG
#define IORING_FEAT_EXT_ARG (1U << 8)
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
#ifndef IORING_REGISTER_FILES2
#define IORING_REGISTER_FILES2 13
#endif
#ifndef IORING_REGISTER_PBUF_RING
#define IORING_REGISTER_PBUF_RING 22
#endif
#ifndef IORING_REGISTER_EVENTFD
#define IORING_REGISTER_EVENTFD 9
#endif
#ifndef IORING_RSRC_REGISTER_SPARSE
#define IORING_RSRC_REGISTER_SPARSE (1U << 0)
#endif
#ifndef IOSQE_FIXED_FILE
#define IOSQE_FIXED_FILE (1U << 0)
#endif
#ifndef IOSQE_IO_HARDLINK
#define IOSQE_IO_HARDLINK (1U << 3)
#endif
#ifndef IOSQE_BUFFER_SELECT
#define IOSQE_BUFFER_SELECT (1U << 4)
#endif
#ifndef IOSQE_CQE_SKIP_SUCCESS
#define IOSQE_CQE_SKIP_SUCCESS (1U << 6)
#endif
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif
#ifndef IORING_POLL_ADD_MULTI
#define IORING_POLL_ADD_MULTI (1U << 0)
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
#ifndef IORING_OFF_SQ_RING
#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES 0x10000000ULL
#endif

static inline long sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    long r = syscall(__NR_io_uring_setup, entries, p);
    return r < 0 ? -errno : r;
}
static inline long sys_io_uring_enter(int fd, unsigned to_submit,
                                      unsigned min_complete, unsigned flags,
                                      const void *arg, size_t argsz) {
    long r = syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                     flags, arg, argsz);
    return r < 0 ? -errno : r;
}
static inline long sys_io_uring_register(int fd, unsigned op,
                                         const void *arg, unsigned nr) {
    long r = syscall(__NR_io_uring_register, fd, op, arg, nr);
    return r < 0 ? -errno : r;
}

static int ring_init(struct bc_uring_ring *r, unsigned sq_entries,
                     unsigned cq_entries, unsigned nslots,
                     unsigned nbufs, unsigned buf_size, int defer,
                     unsigned *caps, const char **err) {
    memset(r, 0, sizeof *r);
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP |
              IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER |
              (defer ? IORING_SETUP_DEFER_TASKRUN : 0);
    p.cq_entries = cq_entries;
    long fd = sys_io_uring_setup(sq_entries, &p);
    if (fd == -EINVAL && defer) {
        /* older kernel: fall back to cooperative task running */
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP |
                  IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER |
                  IORING_SETUP_COOP_TASKRUN;
        p.cq_entries = cq_entries;
        fd = sys_io_uring_setup(sq_entries, &p);
    }
    if (fd < 0) { *err = "io_uring_setup"; errno = (int)-fd; return -1; }
    if (!(p.features & IORING_FEAT_SINGLE_MMAP) ||
        !(p.features & IORING_FEAT_EXT_ARG)) {
        *err = "io_uring: kernel too old (needs SINGLE_MMAP and EXT_ARG)";
        errno = ENOSYS;
        close(fd);
        return -1;
    }
    r->fd = (int)fd;
    r->sq_entries = p.sq_entries;
    r->cq_entries = p.cq_entries;
    r->sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    r->cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (r->cq_sz > r->sq_sz) r->sq_sz = r->cq_sz;
    r->sq_ptr = mmap(NULL, r->sq_sz, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
    if (r->sq_ptr == MAP_FAILED) { *err = "mmap sq"; close(fd); return -1; }
    r->cq_ptr = r->sq_ptr;
    r->sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) { *err = "mmap sqes"; close(fd); return -1; }
    uint8_t *sq = r->sq_ptr, *cq = r->cq_ptr;
    r->sq_head = (unsigned *)(sq + p.sq_off.head);
    r->sq_tail = (unsigned *)(sq + p.sq_off.tail);
    r->sq_mask = *(unsigned *)(sq + p.sq_off.ring_mask);
    r->sq_flags = (unsigned *)(sq + p.sq_off.flags);
    r->sq_array = (unsigned *)(sq + p.sq_off.array);
    r->cq_head = (unsigned *)(cq + p.cq_off.head);
    r->cq_tail = (unsigned *)(cq + p.cq_off.tail);
    r->cq_mask = *(unsigned *)(cq + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);
    for (unsigned i = 0; i < r->sq_entries; i++) r->sq_array[i] = i;
    r->sq_local_tail = *r->sq_tail;

    *caps = 0;
    /* Detect provided-buffer-ring support. */
    {
        struct io_uring_rsrc_register reg;
        memset(&reg, 0, sizeof reg);
        reg.nr = nslots;
        reg.flags = IORING_RSRC_REGISTER_SPARSE;
        long rc = sys_io_uring_register(r->fd, IORING_REGISTER_FILES2, &reg, sizeof reg);
        if (rc < 0) { *err = "register files"; errno = (int)-rc; close(fd); return -1; }
        r->nslots = nslots;
    }
    /* Provided buffer ring. */
    r->br_entries = nbufs;
    r->br_mask = nbufs - 1;
    r->buf_size = buf_size;
    size_t ring_bytes = (size_t)nbufs * sizeof(struct io_uring_buf);
    r->br = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (r->br == MAP_FAILED) { *err = "mmap buf ring"; close(fd); return -1; }
    r->bufs = mmap(NULL, (size_t)nbufs * buf_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->bufs == MAP_FAILED) { *err = "mmap bufs"; close(fd); return -1; }
    struct io_uring_buf_reg breg;
    memset(&breg, 0, sizeof breg);
    breg.ring_addr = (uint64_t)(uintptr_t)r->br;
    breg.ring_entries = nbufs;
    breg.bgid = 0;
    long rc = sys_io_uring_register(r->fd, IORING_REGISTER_PBUF_RING, &breg, 1);
    if (rc == -EINVAL) {
        /* Ubuntu 6.8.0-136..147 (Launchpad #2162843) inverted the reserved-field
         * check; those kernels accept a nonzero reserved byte. */
        breg.resv[0] = 1;
        rc = sys_io_uring_register(r->fd, IORING_REGISTER_PBUF_RING, &breg, 1);
    }
    if (rc < 0) { *err = "register buf ring"; errno = (int)-rc; close(fd); return -1; }
    *caps |= bc_uring_have_pbuf_ring;
    for (unsigned i = 0; i < nbufs; i++) {
        r->br->bufs[i].addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)i * buf_size);
        r->br->bufs[i].len = buf_size;
        r->br->bufs[i].bid = (uint16_t)i;
    }
    r->br_tail = nbufs;
    __atomic_store_n(&r->br->tail, (uint16_t)r->br_tail, __ATOMIC_RELEASE);
    r->bufs_out = 0;

    /* Accept multishot and recv multishot are detected by trying to submit;
     * assume available on kernel 5.10+. We mark them optimistically and let
     * submissions fail gracefully if not supported. */
    *caps |= bc_uring_have_accept_ms | bc_uring_have_recv_ms | bc_uring_have_buf_select;
    return 0;
}

static int ring_register_eventfd(struct bc_uring_ring *r) {
    int efd = (int)syscall(__NR_eventfd2, 0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) return -1;
    long rc = sys_io_uring_register(r->fd, IORING_REGISTER_EVENTFD, &efd, 1);
    if (rc < 0) { close(efd); errno = (int)-rc; return -1; }
    return efd;
}

int bc_uring_init(struct bc_uring *u, unsigned n_rings,
                  unsigned sq_entries, unsigned cq_entries,
                  unsigned nslots, unsigned nbufs, unsigned buf_size,
                  const char **err) {
    const char *local_err = NULL;
    const char *errp = err ? err : &local_err;
    memset(u, 0, sizeof *u);
    u->n_rings = n_rings;
    u->rings = calloc(n_rings, sizeof *u->rings);
    if (!u->rings) { *errp = "calloc rings"; return -1; }
    for (unsigned i = 0; i < n_rings; i++) {
        int defer = (i == 0);
        if (ring_init(&u->rings[i], sq_entries, cq_entries, nslots,
                      nbufs, buf_size, defer, &u->rings[i].capabilities, errp) < 0) {
            return -1;
        }
        if (i > 0) {
            u->rings[i].eventfd = ring_register_eventfd(&u->rings[i]);
            if (u->rings[i].eventfd < 0) { *errp = "register eventfd"; return -1; }
        } else {
            u->rings[i].eventfd = -1;
        }
    }
    return 0;
}

void bc_uring_cleanup(struct bc_uring *u) {
    if (!u->rings) return;
    for (unsigned i = 0; i < u->n_rings; i++) {
        struct bc_uring_ring *r = &u->rings[i];
        if (r->eventfd >= 0) close(r->eventfd);
        if (r->fd >= 0) close(r->fd);
        if (r->sqes != MAP_FAILED) munmap(r->sqes, r->sqes_sz);
        if (r->sq_ptr != MAP_FAILED) munmap(r->sq_ptr, r->sq_sz);
        if (r->br != MAP_FAILED) munmap(r->br, (size_t)r->br_entries * sizeof(struct io_uring_buf));
        if (r->bufs != MAP_FAILED) munmap(r->bufs, (size_t)r->br_entries * r->buf_size);
    }
    for (unsigned i = 0; i < u->n_listen; i++)
        if (u->listen_fds[i] >= 0) close(u->listen_fds[i]);
    free(u->rings);
    u->rings = NULL;
    u->n_rings = 0;
}

static inline struct io_uring_sqe *ring_sqe(struct bc_uring_ring *r) {
    if (r->sq_local_tail - __atomic_load_n(&r->sq_head, __ATOMIC_ACQUIRE) >= r->sq_entries) {
        /* SQ full: push what we have without waiting */
        sys_io_uring_enter(r->fd, r->sq_local_tail - __atomic_load_n(&r->sq_tail, __ATOMIC_ACQUIRE),
                           0, 0, NULL, 0);
        while (r->sq_local_tail - __atomic_load_n(&r->sq_head, __ATOMIC_ACQUIRE) >= r->sq_entries) {
            sys_io_uring_enter(r->fd, r->sq_local_tail - __atomic_load_n(&r->sq_tail, __ATOMIC_ACQUIRE),
                               0, IORING_ENTER_GETEVENTS, NULL, 0);
        }
    }
    struct io_uring_sqe *s = &r->sqes[r->sq_local_tail & r->sq_mask];
    r->sq_local_tail++;
    memset(s, 0, sizeof *s);
    return s;
}

int bc_uring_accept_register(struct bc_uring_ring *r, int listen_fd,
                             unsigned listener_index, void *arg) {
    struct io_uring_sqe *s = ring_sqe(r);
    s->opcode = IORING_OP_ACCEPT;
    s->fd = listen_fd;
    s->ioprio = IORING_ACCEPT_MULTISHOT;   /* multishot accept */
    s->file_index = IORING_FILE_INDEX_ALLOC; /* allocate a direct descriptor */
    s->user_data = (uint64_t)(uintptr_t)arg;
    return 0;
}

int bc_uring_poll_conn(struct bc_uring_ring *r, unsigned slot, uint64_t user_data) {
    struct io_uring_sqe *s = ring_sqe(r);
    s->opcode = IORING_OP_POLL_ADD;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE;
    s->len = IORING_POLL_ADD_MULTI;   /* multishot poll */
    s->poll32_events = POLLIN | POLLRDHUP;
    s->user_data = user_data;
    return 0;
}

int bc_uring_recv_once(struct bc_uring_ring *r, unsigned slot, uint64_t user_data) {
    struct io_uring_sqe *s = ring_sqe(r);
    s->opcode = IORING_OP_RECV;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    s->buf_group = 0;
    s->msg_flags = MSG_DONTWAIT;
    s->user_data = user_data;
    return 0;
}

int bc_uring_send(struct bc_uring_ring *r, unsigned slot, const void *p,
                  unsigned n, uint64_t user_data) {
    struct io_uring_sqe *s = ring_sqe(r);
    s->opcode = IORING_OP_SEND;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE;
    s->addr = (uint64_t)(uintptr_t)p;
    s->len = n;
    s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
    s->user_data = user_data;
    return 0;
}

int bc_uring_cancel(struct bc_uring_ring *r, uint64_t target, uint64_t user_data) {
    struct io_uring_sqe *s = ring_sqe(r);
    s->opcode = IORING_OP_ASYNC_CANCEL;
    s->fd = -1;
    s->addr = target;
    s->user_data = user_data;
    return 0;
}

int bc_uring_shutdown_close(struct bc_uring_ring *r, unsigned slot,
                            uint64_t close_ud, uint64_t poll_ud,
                            uint64_t cancel_ud) {
    struct io_uring_sqe *s = ring_sqe(r);
    s->opcode = IORING_OP_ASYNC_CANCEL;
    s->fd = -1;
    s->addr = poll_ud;
    s->flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
    s->user_data = cancel_ud;
    s = ring_sqe(r);
    s->opcode = IORING_OP_SHUTDOWN;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
    s->len = SHUT_RDWR;
    s->user_data = close_ud;
    s = ring_sqe(r);
    s->opcode = IORING_OP_CLOSE;
    s->file_index = slot;
    s->user_data = close_ud;
    return 0;
}

long bc_uring_submit(struct bc_uring_ring *r, unsigned min_complete,
                     unsigned flags, const void *arg, size_t argsz) {
    unsigned to_submit = r->sq_local_tail - __atomic_load_n(&r->sq_tail, __ATOMIC_ACQUIRE);
    __atomic_store_n(&r->sq_tail, r->sq_local_tail, __ATOMIC_RELEASE);
    return sys_io_uring_enter(r->fd, to_submit, min_complete, flags, arg, argsz);
}

long bc_uring_wait(struct bc_uring_ring *r, unsigned min, unsigned ms) {
    struct __kernel_timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    struct io_uring_getevents_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.ts = (uint64_t)(uintptr_t)&ts;
    return bc_uring_submit(r, min, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof arg);
}

unsigned bc_uring_reap(struct bc_uring_ring *r, bc_uring_cqe_cb cb, void *arg) {
    unsigned head = __atomic_load_n(&r->cq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(&r->cq_tail, __ATOMIC_ACQUIRE);
    unsigned n = 0;
    while (head != tail) {
        struct io_uring_cqe *cqe = &r->cqes[head & r->cq_mask];
        if (cb) cb(r, (unsigned)(cqe->user_data & 0xffff), cqe->user_data, cqe, arg);
        head++;
        n++;
    }
    __atomic_store_n(&r->cq_head, head, __ATOMIC_RELEASE);
    return n;
}

int bc_uring_listen_socket(const char *host, int port, int backlog, const char **err) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { *err = "socket"; return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        *err = "HOST must be an IPv4 address";
        close(fd);
        return -1;
    }
    for (int i = 0; ; i++) {
        if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0) break;
        if (errno != EADDRINUSE || i >= 300) {
            *err = "bind";
            close(fd);
            return -1;
        }
        usleep(100000);
    }
    if (listen(fd, backlog) < 0) { *err = "listen"; close(fd); return -1; }
    return fd;
}

struct io_uring_sqe *bc_uring_sqe(struct bc_uring_ring *r) {
    return ring_sqe(r);
}
