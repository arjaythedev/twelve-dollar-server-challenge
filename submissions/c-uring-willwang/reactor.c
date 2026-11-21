#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "challenge.h"
#include <arpa/inet.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Each ring owns a sparse descriptor table. A persistent epoll watch on the
 * open file description holds no receive buffer; reads are submitted when
 * the socket becomes ready. Multishot receive handles unsupported kernels. */
enum {
    MAX_RINGS = 16, LISTENERS = 8, ACCEPTS_PER_LISTENER = 64,
    SQ_SIZE = 4096, CQ_SIZE = 32768, BUFFER_COUNT = 1024, BUFFER_SIZE = 2048,
    PASS_REPLIES = 512, MAX_OUT = 2048, INPUT_LIMIT = 9 * 16384, CQ_QUANTUM = 2048
};
enum { ACCEPT = 1, MULTIRECV, TRYRECV, SEND, CLOSE, IGNORE, WAKE, INSTALL, PARKWAKE, PARKRECV, CREDITRECV, NORMAL_ACCEPT, IMPORT, IMPORT_PROBE, WATCH_IMPORT, NAMED_IMPORT, NAMED_WATCH };
enum { PARK_WINDOW = 256, PARK_BATCH = 1024, PARK_BATCHES = 8, CREDIT_BYTES = 768 };
/* Each nonblocking read explicitly owns a small, reusable buffer until its
 * completion. The same LIFO pool serves every readiness bank. Only multishot
 * and exceptional reads need the kernel-selected buffer group. */
static _Alignas(64) char read_bytes[PARK_WINDOW][CREDIT_BYTES];
static uint64_t read_keys[PARK_WINDOW];
static unsigned read_free[PARK_WINDOW], read_returned, read_allocated;
enum { IMPORT_WIDTH = 64, ALIAS_LIMIT = LISTENERS * ACCEPTS_PER_LISTENER };
enum {
    OPEN = 1, EOF_READ = 2, CLOSE_AFTER = 4, READY = 8, RETRY = 16,
    STAGED = 32, WRITABLE = 64, HELD_WRITE = 128, PARKED = 256,
    READING = 512, REDO = 1024, HANGUP = 2048, NAMING = 4096, EARLY_EDGE = 8192, FRAGMENT_ACK = 16384
};

typedef struct {
    Buf bytes;
    size_t sent;
} Output;

typedef struct {
    char *input;
    Output *output;
    unsigned length, capacity, generation, flags;
} Connection;

typedef struct {
    uint64_t *keys;
    size_t first, last, capacity;
} Queue;

typedef struct {
    int fd, wake, parked_fd;
    Connection *conns;
    unsigned live, accepting, admission_batch;
    unsigned next_named_slot, named_inflight, kernel_allocates;
    unsigned *sq_head, *sq_tail, *sq_flags, sq_mask, tail;
    unsigned *cq_head, *cq_tail, *cq_flags, cq_mask;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    void *map;
    size_t map_size, sqe_size;
    char *buffers;
    struct io_uring_buf_ring *provided;
    unsigned buf_tail;
    Queue retry;
} Ring;

static Ring rings[MAX_RINGS];
static unsigned bank_disarmed;
static unsigned ring_count, slots, active_accept[LISTENERS], accept_ring;
static int park_fd, park_armed, can_install = 1, secondary_muted;
static int poll_fd, listeners[LISTENERS], epoll_mode;
static Queue ready, held_writes;
/* Only queued sockets have an age. Repeated epoll edges update connection flags
 * without changing a queued entry's original position or observation time. */
typedef struct {
    uint64_t key, since;
} ParkEntry;
static struct {
    ParkEntry *items;
    unsigned head, count, capacity, inflight;
    uint64_t stamp;
} pending;
static Output outputs[MAX_OUT], *available[MAX_OUT];
static unsigned unused, allocated;
static uint64_t staged[PASS_REPLIES];
static unsigned staged_count, budget, in_pass;
static uint64_t next_accept_retry;

/* Ordinary fd lifetime is bounded from issuance of ACCEPT until the temporary
 * normal descriptor has actually been closed. The separate index vector is
 * kernel-writable; the issuing thread always retains the original fd numbers. */
typedef struct {
    int fds[IMPORT_WIDTH], indices[IMPORT_WIDTH];
    struct epoll_event watches[IMPORT_WIDTH];
    unsigned count, ring, next, prefix, pending_watches;
    uint64_t retry_at, rejected_watches;
    unsigned first_slot;
    int update_result;
} AdmissionBatch;
static AdmissionBatch admissions[ALIAS_LIMIT+1];
static unsigned admission_free, admission_used, delayed_imports, ordinary_pending, alias_limit;
static int ordinary_accept, named_imports, ring_watches = 1;
static int closing_alias[ALIAS_LIMIT];
static unsigned closing_count;


static uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void fatal(const char *what) {
    perror(what);
    exit(1);
}

static uint64_t key(unsigned ring, unsigned slot, unsigned generation) {
    return ((uint64_t)generation << 24) | ((uint64_t)ring << 16) | slot;
}
static uint64_t op(unsigned kind, uint64_t k) { return ((uint64_t)kind << 56) | k; }
static unsigned which_ring(uint64_t k) { return (k >> 16) & 255; }
static unsigned which_slot(uint64_t k) { return k & 65535; }

static Connection *find_conn(uint64_t k) {
    Connection *c = &rings[which_ring(k)].conns[which_slot(k)];
    return c->generation == (unsigned)(k >> 24) && (c->flags & OPEN) ? c : NULL;
}

static void push(Queue *q, uint64_t k) {
    if (q->last == q->capacity) {
        if (q->first >= q->capacity / 2 && q->first) {
            memmove(q->keys, q->keys + q->first, (q->last - q->first) * sizeof(*q->keys));
            q->last -= q->first;
            q->first = 0;
        } else {
            q->capacity = q->capacity ? 2 * q->capacity : 128;
            uint64_t *p = realloc(q->keys, q->capacity * sizeof(*q->keys));
            if (!p) fatal("request queue allocation");
            q->keys = p;
        }
    }
    q->keys[q->last++] = k;
}
static int has_items(const Queue *q) { return q->first != q->last; }
static uint64_t pop(Queue *q) {
    uint64_t k = q->keys[q->first++];
    if (q->first == q->last) q->first = q->last = 0;
    return k;
}

static uint64_t park_stamp(void) {
    if (!pending.stamp) pending.stamp = monotonic_ns();
    return pending.stamp;
}
static void enqueue_park(uint64_t k) {
    if (pending.count == pending.capacity) {
        unsigned cap = pending.capacity ? 2 * pending.capacity : 256;
        ParkEntry *items = malloc((size_t)cap * sizeof(*items));
        if (!items) fatal("parked receive queue");
        if (pending.count) {
            unsigned first = pending.capacity - pending.head;
            memcpy(items, pending.items + pending.head, first * sizeof(*items));
            memcpy(items + first, pending.items, pending.head * sizeof(*items));
        }
        free(pending.items);
        pending.items = items;
        pending.capacity = cap;
        pending.head = 0;
    }
    unsigned tail = (pending.head + pending.count++) & (pending.capacity - 1);
    pending.items[tail] = (ParkEntry){.key = k, .since = park_stamp()};
}

static unsigned get_acquire(const unsigned *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void put_release(unsigned *p, unsigned n) { __atomic_store_n(p, n, __ATOMIC_RELEASE); }

static void return_buffer(Ring *r, unsigned bid) {
    struct io_uring_buf *b = &r->provided->bufs[r->buf_tail++ & (BUFFER_COUNT - 1)];
    b->addr = (uintptr_t)(r->buffers + (size_t)bid * BUFFER_SIZE);
    b->len = BUFFER_SIZE;
    b->bid = bid;
}

/* The Ubuntu 6.8 stable series containing LP #2162843 rejects an all-zero
 * reserved field. Other kernels accept the standard registration on the first try. */
static int new_ring(unsigned id) {
    Ring *r = &rings[id];
    struct io_uring_params p = {0};
    r->wake = -1;
    p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_SUBMIT_ALL;
    if (!id) p.flags |= IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    p.cq_entries = CQ_SIZE;
    r->fd = syscall(SYS_io_uring_setup, SQ_SIZE, &p);
    if (r->fd < 0) return -1;
    if (!(p.features & IORING_FEAT_SINGLE_MMAP) || !(p.features & IORING_FEAT_EXT_ARG)) {
        errno = ENOTSUP;
        goto fail;
    }
    size_t sq_bytes = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_bytes = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    r->map_size = sq_bytes > cq_bytes ? sq_bytes : cq_bytes;
    r->map = mmap(NULL, r->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_SQ_RING);
    r->sqe_size = p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqe_size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_SQES);
    if (r->map == MAP_FAILED || r->sqes == MAP_FAILED) goto fail;
    char *base = r->map;
    r->sq_head = (void *)(base + p.sq_off.head);
    r->sq_tail = (void *)(base + p.sq_off.tail);
    r->sq_flags = (void *)(base + p.sq_off.flags);
    r->sq_mask = p.sq_entries - 1;
    r->cq_head = (void *)(base + p.cq_off.head);
    r->cq_tail = (void *)(base + p.cq_off.tail);
    if (p.cq_off.flags) r->cq_flags = (void *)(base + p.cq_off.flags);
    r->cqes = (void *)(base + p.cq_off.cqes);
    r->cq_mask = p.cq_entries - 1;
    unsigned *indexes = (void *)(base + p.sq_off.array);
    for (unsigned i = 0; i < p.sq_entries; ++i) indexes[i] = i;
    struct io_uring_rsrc_register files = {.nr = slots, .flags = IORING_RSRC_REGISTER_SPARSE};
    if (syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_FILES2, &files, sizeof(files)) < 0)
        goto fail;
    r->conns = calloc(slots, sizeof(Connection));
    r->buffers = malloc((size_t)BUFFER_COUNT * BUFFER_SIZE);
    r->provided = mmap(NULL, BUFFER_COUNT * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (!r->conns || !r->buffers || r->provided == MAP_FAILED) goto fail;
    struct io_uring_buf_reg br = {.ring_addr = (uintptr_t)r->provided, .ring_entries = BUFFER_COUNT};
    if (syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, &br, 1) < 0) {
        if (errno != EINVAL) goto fail;
        br.resv[0] = 1;
        if (syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, &br, 1) < 0) goto fail;
    }
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) return_buffer(r, i);
    __atomic_store_n(&r->provided->tail, (uint16_t)r->buf_tail, __ATOMIC_RELEASE);
    if (id) {
        r->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (r->wake < 0 || syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_EVENTFD, &r->wake, 1) < 0) goto fail;
    }
    if (id && r->cq_flags && secondary_muted)
        __atomic_fetch_or(r->cq_flags, IORING_CQ_EVENTFD_DISABLED, __ATOMIC_RELAXED);
    ++ring_count;
    return 0;
fail: {
        int err = errno;
        close(r->fd);
        if (r->wake >= 0) close(r->wake);
        if (r->map && r->map != MAP_FAILED) munmap(r->map, r->map_size);
        if (r->sqes && r->sqes != MAP_FAILED) munmap(r->sqes, r->sqe_size);
        if (r->provided && r->provided != MAP_FAILED) munmap(r->provided, BUFFER_COUNT * sizeof(struct io_uring_buf));
        free(r->buffers);
        free(r->conns);
        memset(r, 0, sizeof(*r));
        errno = err;
        return -1;
    }
}

/* Publish returned buffers before entering so a burst immediately reuses them. */
static void enter_ring(Ring *r, unsigned want, int timeout_ns) {
    struct __kernel_timespec t = {.tv_nsec = timeout_ns};
    struct io_uring_getevents_arg arg = {.ts = (uintptr_t)&t};
    unsigned flags = IORING_ENTER_GETEVENTS;
    if (timeout_ns >= 0) flags |= IORING_ENTER_EXT_ARG;
    __atomic_store_n(&r->provided->tail, (uint16_t)r->buf_tail, __ATOMIC_RELEASE);
    put_release(r->sq_tail, r->tail);
    int rc = syscall(SYS_io_uring_enter, r->fd, r->tail - get_acquire(r->sq_head), want, flags,
                     timeout_ns >= 0 ? &arg : NULL, timeout_ns >= 0 ? sizeof(arg) : 0);
    if (rc < 0 && errno != EINTR && errno != ETIME && errno != EBUSY) fatal("io_uring_enter");
}
static void reserve(Ring *r, unsigned count) {
    while (r->tail - get_acquire(r->sq_head) + count > r->sq_mask + 1)
        enter_ring(r, 0, -1);
}
static struct io_uring_sqe *sqe(Ring *r, unsigned code, uint64_t user, int fd) {
    reserve(r, 1);
    struct io_uring_sqe *s = &r->sqes[r->tail++ & r->sq_mask];
    memset(s, 0, sizeof(*s));
    s->opcode = code;
    s->fd = fd;
    s->user_data = user;
    return s;
}
static void defer_import(unsigned id) {
    AdmissionBatch *b = &admissions[id];
    b->retry_at = monotonic_ns() + 1000000;
    b->next = delayed_imports;
    delayed_imports = id;
}

/* Only virgin slots have predictable ownership without a second free table.
 * An exhausted bank permanently returns allocation to the kernel. */
static void submit_import(Ring *r, unsigned id) {
    AdmissionBatch *b = &admissions[id];
    if (named_imports && ordinary_accept && ring_watches && !r->kernel_allocates &&
        b->count <= slots - r->next_named_slot) {
        reserve(r, 1 + b->count);
        b->first_slot = r->next_named_slot;
        r->next_named_slot += b->count;
        ++r->named_inflight;
        b->pending_watches = b->count;
        b->update_result = INT32_MIN;
        b->rejected_watches = 0;
        struct io_uring_sqe *s = sqe(r, IORING_OP_FILES_UPDATE, op(NAMED_IMPORT, id), -1);
        s->off = b->first_slot;
        s->addr = (uintptr_t)b->fds;
        s->len = b->count;
        for (unsigned i = 0; i < b->count; ++i) {
            unsigned slot = b->first_slot + i;
            Connection *c = &r->conns[slot];
            if (c->flags) { errno = EBUSY; fatal("occupied virgin admission slot"); }
            c->flags = NAMING;
            b->watches[i] = (struct epoll_event){
                .events = EPOLLIN | EPOLLRDHUP | EPOLLET,
                .data.u64 = key(b->ring, slot, ++c->generation)
            };
            s = sqe(r, IORING_OP_EPOLL_CTL, op(NAMED_WATCH, (uint64_t)id * IMPORT_WIDTH + i), r->parked_fd);
            s->addr = (uintptr_t)&b->watches[i];
            s->off = (unsigned)b->fds[i];
            s->len = EPOLL_CTL_ADD;
        }
        return;
    }
    r->kernel_allocates = 1;
    /* INDEX_ALLOC may choose any unused fixed slot, including an explicitly
     * reserved slot whose update has not yet run. Barrier only at handoff. */
    if (r->named_inflight) { defer_import(id); return; }
    memcpy(b->indices, b->fds, b->count * sizeof(int));
    struct io_uring_sqe *s = sqe(r, IORING_OP_FILES_UPDATE, op(IMPORT, id), -1);
    s->off = IORING_FILE_INDEX_ALLOC;
    s->addr = (uintptr_t)b->indices;
    s->len = b->count;
}

/* Probe before accepted descriptors exist. The original direct-accept path
 * remains available on kernels lacking dynamic FILES_UPDATE allocation. */
static int probe_import(Ring *r) {
    int ordinary = eventfd(0, EFD_CLOEXEC);
    if (ordinary < 0) return 0;
    int index = ordinary;
    struct io_uring_sqe *s = sqe(r, IORING_OP_FILES_UPDATE, op(IMPORT_PROBE, 0), -1);
    s->off = IORING_FILE_INDEX_ALLOC;
    s->addr = (uintptr_t)&index;
    s->len = 1;
    while (*r->cq_head == get_acquire(r->cq_tail)) enter_ring(r, 1, -1);
    unsigned head = *r->cq_head;
    struct io_uring_cqe cqe = r->cqes[head & r->cq_mask];
    put_release(r->cq_head, head + 1);
    int ok = cqe.res == 1 && (unsigned)index < slots;
    if (ok) {
        s = sqe(r, IORING_OP_CLOSE, op(IMPORT_PROBE, 0), 0);
        s->file_index = (unsigned)index + 1;
        while (*r->cq_head == get_acquire(r->cq_tail)) enter_ring(r, 1, -1);
        head = *r->cq_head;
        cqe = r->cqes[head & r->cq_mask];
        put_release(r->cq_head, head + 1);
        if (cqe.res != 0) { errno = -cqe.res; fatal("close fixed admission probe"); }
    }
    close(ordinary);
    return ok;
}
static void flush_admission(Ring *r) {
    if (r->admission_batch) {
        submit_import(r, r->admission_batch);
        r->admission_batch = 0;
    }
}
static void append_admission(Ring *r, int fd) {
    unsigned id = r->admission_batch;
    if (!id) {
        if (admission_free) {
            id = admission_free;
            admission_free = admissions[id].next;
        } else {
            if (admission_used == ALIAS_LIMIT) { errno = EOVERFLOW; fatal("admission credits"); }
            id = ++admission_used;
        }
        admissions[id].count = 0;
        admissions[id].ring = (unsigned)(r - rings);
        r->admission_batch = id;
    }
    AdmissionBatch *b = &admissions[id];
    b->fds[b->count++] = fd;
    if (b->count == IMPORT_WIDTH) flush_admission(r);
}
static void retry_admissions(void) {
    if (!delayed_imports) return;
    uint64_t now = monotonic_ns();
    unsigned *next = &delayed_imports;
    while (*next) {
        unsigned id = *next;
        AdmissionBatch *b = &admissions[id];
        if (now >= b->retry_at) {
            *next = b->next;
            submit_import(&rings[b->ring], id);
        } else next = &b->next;
    }
}

static int fd_order(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static void queue_alias_close(int fd) {
    if (closing_count == ALIAS_LIMIT) { errno = EOVERFLOW; fatal("owned admission aliases"); }
    closing_alias[closing_count++] = fd;
}
static void close_aliases(void) {
    static int range_ok = 1;
    if (!closing_count) return;
    unsigned n = closing_count;
    int ordered = 1;
    for (unsigned i = 1; i < n; ++i)
        if (closing_alias[i] < closing_alias[i-1]) { ordered = 0; break; }
    if (!ordered) qsort(closing_alias, n, sizeof(int), fd_order);
    for (unsigned start = 0; start < n;) {
        unsigned end = start + 1;
        while (end < n && closing_alias[end] == closing_alias[end-1] + 1) ++end;
        if (range_ok && end - start > 1) {
            if (syscall(SYS_close_range, (unsigned)closing_alias[start],
                        (unsigned)closing_alias[end-1], 0) == 0) { start = end; continue; }
            /* Both errors reject the operation before closing any descriptor. */
            if (errno != ENOSYS && errno != EPERM) fatal("close_range admission aliases");
            range_ok = 0;
        }
        for (unsigned i = start; i < end; ++i) close(closing_alias[i]);
        start = end;
    }
    ordinary_pending -= n;
    closing_count = 0;
}

/* Non-deferred secondary rings can complete while ring zero is asleep.
 * An edge from their eventfd is enough to end its wait. At high load the
 * scheduler rotates the bounded aggregation window across all busy rings. */
static void watch_ring(unsigned id) {
    struct io_uring_sqe *s = sqe(&rings[0], IORING_OP_POLL_ADD, op(WAKE, id), rings[id].wake);
    s->len = IORING_POLL_ADD_MULTI;
    s->poll32_events = POLLIN;
}
static void receive(uint64_t k, unsigned kind) {
    Ring *r = &rings[which_ring(k)];
    if (kind == PARKRECV && (read_returned || read_allocated < PARK_WINDOW)) {
        unsigned id = read_returned ? read_free[--read_returned] : read_allocated++;
        read_keys[id] = k;
        struct io_uring_sqe *s = sqe(r, IORING_OP_RECV, op(CREDITRECV, id), which_slot(k));
        s->flags = IOSQE_FIXED_FILE;
        s->msg_flags = MSG_DONTWAIT;
        s->addr = (uintptr_t)read_bytes[id];
        s->len = CREDIT_BYTES;
        return;
    }
    struct io_uring_sqe *s = sqe(r, IORING_OP_RECV, op(kind, k), which_slot(k));
    s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    if (kind == MULTIRECV) s->ioprio = IORING_RECV_MULTISHOT;
    else s->msg_flags = MSG_DONTWAIT;
}

/* A one-shot watch wakes an idle reactor once. While it is absent the issuer
 * pulls arrivals directly from epoll each pass and needs no intermediate CQE. */
static void arm_park_before_wait(void) {
    if (park_armed) return;
    Ring *primary = &rings[0];
    struct io_uring_sqe *s = sqe(primary, IORING_OP_POLL_ADD, op(PARKWAKE, 0), park_fd);
    s->poll32_events = POLLIN;
    park_armed = 1;
}
/* epoll_ctl requires a normal descriptor. Install a temporary alias, register
 * it once, then close it. The direct slot keeps the watched file alive. */
static void install_ordinary(uint64_t k) {
    struct io_uring_sqe *s = sqe(&rings[which_ring(k)], IORING_OP_FIXED_FD_INSTALL,
                                 op(INSTALL, k), which_slot(k));
    s->flags = IOSQE_FIXED_FILE;
}
/* Each fixed-file table has its own readiness list. The parent is a one-shot
 * idle doorbell. A busy bank never routes each incoming edge through it. */
static void register_bank(unsigned id) {
    Ring *r = &rings[id];
    r->parked_fd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.u32 = id};
    if (r->parked_fd < 0 || epoll_ctl(park_fd, EPOLL_CTL_ADD, r->parked_fd, &ev) < 0)
        fatal("register readiness bank");
}

static void arm_banks(void) {
    struct epoll_event events[MAX_RINGS];
    int n = epoll_wait(park_fd, events, MAX_RINGS, 0);
    if (n < 0 && errno != EINTR) fatal("idle bank notifications");
    for (int i = 0; i < n; ++i) bank_disarmed |= 1u << events[i].data.u32;
    while (bank_disarmed) {
        unsigned id = __builtin_ctz(bank_disarmed);
        bank_disarmed &= bank_disarmed - 1;
        struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.u32 = id};
        if (epoll_ctl(park_fd, EPOLL_CTL_MOD, rings[id].parked_fd, &ev) < 0)
            fatal("rearm idle bank");
    }
}

static void replenish_accepts(void) {
    unsigned depth = slots / (LISTENERS * 4);
    if (depth > ACCEPTS_PER_LISTENER) depth = ACCEPTS_PER_LISTENER;
    if (ordinary_accept && depth > alias_limit / LISTENERS) depth = alias_limit / LISTENERS;
    for (unsigned l = 0; l < LISTENERS; ++l) {
        while (active_accept[l] < depth) {
            if (ordinary_accept && ordinary_pending >= alias_limit) return;
            Ring *r = &rings[accept_ring];
            if (r->live + r->accepting >= slots) {
                unsigned i = 0;
                for (; i < ring_count; ++i)
                    if (rings[i].live + rings[i].accepting + depth <= slots) break;
                if (i == ring_count) {
                    if (i == MAX_RINGS || (next_accept_retry && monotonic_ns() < next_accept_retry)) return;
                    if (new_ring(i) < 0) {
                        perror("additional io_uring");
                        next_accept_retry = monotonic_ns() + 1000000000;
                        return;
                    }
                    register_bank(i);
                    watch_ring(i);
                    next_accept_retry = 0;
                }
                accept_ring = i;
                r = &rings[i];
            }
            if (!ordinary_accept) {
                r->kernel_allocates = 1;
                if (r->named_inflight) return;
            }
            unsigned kind = ordinary_accept ? NORMAL_ACCEPT : ACCEPT;
            struct io_uring_sqe *s = sqe(r, IORING_OP_ACCEPT, op(kind, key(accept_ring, l, 0)), listeners[l]);
            if (ordinary_accept) { s->accept_flags = SOCK_CLOEXEC; ++ordinary_pending; }
            else s->file_index = IORING_FILE_INDEX_ALLOC;
            ++r->accepting;
            ++active_accept[l];
        }
    }
}

static int output_available(void) { return unused || allocated < MAX_OUT; }

static Output *get_output(void) {
    Output *o = unused ? available[--unused] : &outputs[allocated++];
    o->sent = o->bytes.len = 0;
    return o;
}
static void put_output(Output *o) {
    /* Rare expanded responses must not make every recycled buffer that size. */
    if (o->bytes.cap > 32768) {
        free(o->bytes.data);
        o->bytes = (Buf){0};
    }
    available[unused++] = o;
}

static void close_conn(uint64_t k, Connection *c) {
    /* A send still owns its buffer until its CQE arrives. Normal peer EOF
     * (write-half close) must also allow the response to reach the reader. */
    if (c->output) {
        c->flags |= CLOSE_AFTER;
        return;
    }
    Ring *r = &rings[which_ring(k)];
    unsigned slot = which_slot(k);
    free(c->input);
    c->input = NULL;
    c->length = c->capacity = c->flags = 0;
    if (epoll_mode) {
        close((int)slot); /* last reference also removes the epoll watch */
        --r->live;
    } else {
        reserve(r, 2);
        struct io_uring_sqe *s = sqe(r, IORING_OP_SHUTDOWN, op(IGNORE, k), slot);
        s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
        s->len = SHUT_RDWR;
        s = sqe(r, IORING_OP_CLOSE, op(CLOSE, k), 0);
        s->file_index = slot + 1;
    }
}

static void schedule(uint64_t k, Connection *c) {
    if (!c->output && c->length && !(c->flags & (READY | HELD_WRITE))) {
        c->flags |= READY;
        push(&ready, k);
    }
}
static int append_input(Connection *c, const char *p, size_t n) {
    if (n > INPUT_LIMIT - c->length) return -1;
    unsigned total = c->length + (unsigned)n;
    if (total > c->capacity) {
        unsigned cap = c->capacity ? c->capacity * 2 : 256;
        if (cap < total) cap = total;
        char *b = realloc(c->input, cap);
        if (!b) return -1;
        c->input = b;
        c->capacity = cap;
    }
    memcpy(c->input + c->length, p, n);
    c->length = total;
    return 0;
}

enum DispatchSource { READY_INPUT, OLDEST_HELD_WRITE };

/* When the request is incomplete there is no response to carry the ACK.
 * A peer using Nagle may be waiting for it before sending the next bytes.
 * The fixed socket can receive the same option without installing an alias. */
static void acknowledge_fragment(uint64_t k) {
    static const int quick = 1;
    if (epoll_mode) {
        (void)setsockopt((int)which_slot(k), IPPROTO_TCP, TCP_QUICKACK, &quick, sizeof(quick));
        return;
    }
    struct io_uring_sqe *s = sqe(&rings[which_ring(k)], IORING_OP_URING_CMD,
                                 op(IGNORE, k), which_slot(k));
    s->flags = IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS;
    s->cmd_op = SOCKET_URING_OP_SETSOCKOPT;
    s->level = IPPROTO_TCP;
    s->optname = TCP_QUICKACK;
    s->optval = (uintptr_t)&quick;
    s->optlen = sizeof(quick);
}


/* The common case calls the HTTP parser directly on the kernel-selected buffer.
 * Only fragments, pipelined bytes and requests deferred by a budget get copied. */
static void dispatch(uint64_t k, Connection *c, const char *input, size_t n, enum DispatchSource source) {
    if (c->flags & CLOSE_AFTER) return;
    if (c->length || !input || c->output || !budget || !output_available()) {
        if (n && append_input(c, input, n) < 0) {
            close_conn(k, c);
            return;
        }
        input = c->input;
        n = c->length;
        if (c->flags & HELD_WRITE) return;
        if (c->output || !budget || !output_available()) {
            schedule(k, c);
            return;
        }
    }
    if (!n) return;
    if (!in_pass) {
        db_begin_pass();
        in_pass = 1;
    }
    /* The write gate may open after the pass has already checked its FIFO.
     * New arrivals must still wait behind the older requests in that FIFO. */
    if (n >= 5 && !memcmp(input, "POST ", 5) &&
        ((source != OLDEST_HELD_WRITE && has_items(&held_writes)) || db_writes_deferred())) {
        if (input != c->input && append_input(c, input, n) < 0) {
            close_conn(k, c);
            return;
        }
        c->flags |= HELD_WRITE;
        push(&held_writes, k);
        return;
    }
    size_t consumed = 0;
    int close_after = 0;
    Output *o = get_output();
    int result = app_request(input, n, &consumed, &o->bytes, &close_after);
    if (result <= 0) {
        if (!result && !(c->flags & (FRAGMENT_ACK | EOF_READ))) {
            c->flags |= FRAGMENT_ACK;
            acknowledge_fragment(k);
        }
        put_output(o);
        if (result < 0 || (c->flags & EOF_READ) ||
            (input != c->input && append_input(c, input, n) < 0)) close_conn(k, c);
        return;
    }
    c->flags &= ~FRAGMENT_ACK;
    if (!consumed || consumed > n || !o->bytes.len) {
        put_output(o);
        close_conn(k, c);
        return;
    }
    c->output = o;
    if (close_after) c->flags |= CLOSE_AFTER;
    c->flags |= STAGED;
    staged[staged_count++] = k;
    --budget;
    size_t rest = close_after ? 0 : n - consumed;
    if (input == c->input) {
        if (rest) memmove(c->input, c->input + consumed, rest);
        else {
            free(c->input);
            c->input = NULL;
            c->capacity = 0;
        }
        c->length = rest;
    } else if (rest && append_input(c, input + consumed, rest) < 0) {
        c->flags |= CLOSE_AFTER;
    }
}

static void peer_eof(uint64_t k, Connection *c) {
    c->flags |= EOF_READ;
    if (c->output) return;
    if (c->length) schedule(k, c);
    else close_conn(k, c);
}
static int runnable(void) {
    return output_available() && (has_items(&ready) ||
           (has_items(&held_writes) && !db_writes_deferred()));
}

static void drain_buffered(void) {
    while (budget && output_available() && has_items(&ready)) {
        uint64_t k = pop(&ready);
        Connection *c = find_conn(k);
        if (!c || !(c->flags & READY)) continue;
        c->flags &= ~READY;
        dispatch(k, c, NULL, 0, READY_INPUT);
    }
}

static void drain_ready(void) {
    /* Give the oldest held writes the first slots of an open commit window.
     * Returning them to the general queue lets new arrivals close the window
     * again before those writes run. Reserve half each pass for other work. */
    if (has_items(&held_writes) && output_available()) {
        if (!in_pass) {
            db_begin_pass();
            in_pass = 1;
        }
        if (!db_writes_deferred()) {
            unsigned remaining = PASS_REPLIES / 2;
            while (remaining && budget && output_available() && has_items(&held_writes)) {
                uint64_t k = pop(&held_writes);
                Connection *c = find_conn(k);
                if (!c || !(c->flags & HELD_WRITE)) continue;
                c->flags &= ~HELD_WRITE;
                dispatch(k, c, NULL, 0, OLDEST_HELD_WRITE);
                --remaining;
            }
        }
    }
    drain_buffered();
}

static void epoll_interest(uint64_t k, Connection *c, int write) {
    unsigned events = write ? EPOLLOUT : 0;
    if (!(c->flags & (EOF_READ | CLOSE_AFTER))) events |= EPOLLIN | EPOLLRDHUP;
    struct epoll_event ev = {.events = events, .data.u64 = k};
    if (epoll_ctl(poll_fd, EPOLL_CTL_MOD, (int)which_slot(k), &ev) < 0 && errno != ENOENT)
        fatal("epoll socket interest");
    if (write) c->flags |= WRITABLE;
    else c->flags &= ~WRITABLE;
}

static void send_result(uint64_t k, Connection *c, int rc) {
    Output *o = c->output;
    if (rc > 0) o->sent += (unsigned)rc;
    if (rc <= 0 || o->sent >= o->bytes.len) {
        put_output(o);
        c->output = NULL;
        if (rc <= 0 || (c->flags & CLOSE_AFTER) || ((c->flags & EOF_READ) && !c->length)) {
            close_conn(k, c);
        } else {
            if (epoll_mode && (c->flags & WRITABLE)) epoll_interest(k, c, 0);
            schedule(k, c);
        }
    }
}
static void issue_send(uint64_t k, Connection *c) {
    Output *o = c->output;
    if (epoll_mode) {
        for (;;) {
            ssize_t n = send((int)which_slot(k), o->bytes.data + o->sent, o->bytes.len - o->sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!(c->flags & WRITABLE)) epoll_interest(k, c, 1);
                return;
            }
            send_result(k, c, (int)n);
            if (!c->output) return;
        }
    } else {
        struct io_uring_sqe *s = sqe(&rings[which_ring(k)], IORING_OP_SEND, op(SEND, k), which_slot(k));
        s->flags = IOSQE_FIXED_FILE;
        s->addr = (uintptr_t)(o->bytes.data + o->sent);
        s->len = (unsigned)(o->bytes.len - o->sent);
        s->msg_flags = MSG_NOSIGNAL;
    }
}

static void flush_responses(int failed) {
    for (unsigned i = 0; i < staged_count; ++i) {
        uint64_t k = staged[i];
        Connection *c = find_conn(k);
        if (!c || !(c->flags & STAGED)) continue;
        c->flags &= ~STAGED;
        if (failed) {
            app_error_500(&c->output->bytes);
            c->flags |= CLOSE_AFTER;
        }
        issue_send(k, c);
    }
    staged_count = 0;
}

static void finish_pass(void) {
    int failed = in_pass && db_commit_pass() != 0;
    flush_responses(failed);
    in_pass = 0;
}

/* A response from an unmodified snapshot contains only previously committed
 * data. Send those replies while their TCP sockets are still hot, keeping the
 * read transaction open for the next bank. A transaction that contains a
 * write must complete in SQLite before any of these responses are handed off. */
static void flush_bank(void) {
    if (!staged_count) return;
    if (db_pass_read_only()) flush_responses(0);
    else finish_pass();
}

static uint64_t accept_conn(unsigned ri, unsigned slot) {
    Ring *r = &rings[ri];
    Connection *c = &r->conns[slot];
    ++c->generation;
    c->flags = OPEN;
    ++r->live;
    return key(ri, slot, c->generation);
}
static void retry_receive(Ring *r, uint64_t k, Connection *c) {
    if (!(c->flags & (RETRY | EOF_READ | CLOSE_AFTER))) {
        c->flags |= RETRY;
        if (c->flags & PARKED) enqueue_park(k);
        else push(&r->retry, k);
    }
}
static void service_retries(Ring *r) {
    /* After a burst, drain at most half the buffer group per ring with
     * nonblocking receives. Reserve the rest for still-armed sockets. */
    for (unsigned n = 0; n < BUFFER_COUNT / 2 && has_items(&r->retry);) {
        uint64_t k = pop(&r->retry);
        Connection *c = find_conn(k);
        if (!c || !(c->flags & RETRY)) continue;
        c->flags &= ~RETRY;
        if (c->flags & (EOF_READ | CLOSE_AFTER)) continue;
        receive(k, TRYRECV);
        ++n;
    }
}
/* The kernel keeps unread readiness FIFO within each bank. Advance every
 * bank in turn. The userspace queue handles only synthetic retries (a full
 * buffer, EOF or an edge that raced an outstanding read). */
static unsigned collect_bank(Ring *r, unsigned max) {
    struct epoll_event events[64];
    if (max > sizeof(events) / sizeof(events[0])) max = sizeof(events) / sizeof(events[0]);
    if (!max) return 0;
    int n = epoll_wait(r->parked_fd, events, (int)max, 0);
    if (n < 0) {
        if (errno == EINTR) return 1;
        fatal("bank epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
        if (i + 6 < n) __builtin_prefetch(&r->conns[which_slot(events[i+6].data.u64)], 1, 1);
        uint64_t k = events[i].data.u64;
        Connection *c = find_conn(k);
        if (!c) {
            /* A watch can post an edge before its explicit fixed-table update
             * even executes. Preserve it without operating on that slot. */
            Connection *provisional = &r->conns[which_slot(k)];
            if ((provisional->flags & NAMING) && provisional->generation == (unsigned)(k >> 24)) {
                provisional->flags |= EARLY_EDGE;
                if (events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) provisional->flags |= HANGUP;
            }
            continue;
        }
        if (!(c->flags & PARKED) || (c->flags & (CLOSE_AFTER | EOF_READ))) continue;
        if (events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) c->flags |= HANGUP;
        if (c->flags & READING) c->flags |= REDO;
        else if (!(c->flags & RETRY)) {
            c->flags |= READING;
            ++pending.inflight;
            receive(k, PARKRECV);
        }
    }
    return (unsigned)n;
}

/* One end always advances the global oldest observation. When there is a
 * choice, spend the other slots on arrivals recent enough to benefit from
 * a short reply time. Older heads get a progressively larger guaranteed
 * share. Never reverse work that fits in this pass or is all already old. */
static unsigned prefer_recent(uint64_t now, unsigned tail, unsigned issued) {
    uint64_t head_age = now - pending.items[pending.head].since;
    unsigned phase = issued & 3;
    if (now - pending.items[tail].since >= 16000000) return 0;
    if (head_age >= 96000000) return phase == 3;
    if (head_age >= 48000000) return phase & 1;
    return phase != 0;
}
static unsigned issue_park(void) {
    unsigned room = unused + MAX_OUT - allocated;
    if (!pending.count || !room) return 0;
    unsigned limit = PARK_WINDOW - pending.inflight;
    if (has_items(&ready) && limit > PASS_REPLIES / 4) limit = PASS_REPLIES / 4;
    if (limit > room) limit = room;
    unsigned issued = 0, inspected = 0;
    int congested = pending.count > limit;
    uint64_t now = congested && limit ? monotonic_ns() : 0;
    while (issued < limit && pending.count && inspected++ < 2 * limit + 32) {
        unsigned mask = pending.capacity - 1;
        unsigned tail = (pending.head + pending.count - 1) & mask;
        unsigned index = pending.head;
        if (congested && prefer_recent(now, tail, issued)) index = tail;
        else pending.head = (pending.head + 1) & mask;
        --pending.count;
        uint64_t k = pending.items[index].key;
        Connection *c = find_conn(k);
        if (!c || !(c->flags & RETRY)) continue;
        c->flags &= ~RETRY;
        if (c->flags & (EOF_READ | CLOSE_AFTER)) continue;
        c->flags |= READING;
        receive(k, PARKRECV);
        ++issued;
    }
    pending.inflight += issued;
    return issued;
}
/* Return ordinary aliases only after every asynchronous watch for the prefix
 * has completed. close_aliases() will close each contiguous owned run at once.
 * The unreleased suffix still owns its original descriptors and ring credits. */
static void retire_import(Ring *r, unsigned id, unsigned done, int res) {
    AdmissionBatch *b = &admissions[id];
    for (unsigned i = 0; i < done; ++i) queue_alias_close(b->fds[i]);
    if (done < b->count) {
        unsigned left = b->count - done;
        memmove(b->fds, b->fds + done, left * sizeof(int));
        b->count = left;
        if (res >= 0 || res == -ENOMEM || res == -ENFILE || res == -EMFILE ||
            res == -EBUSY || res == -EAGAIN || res == -EINTR) {
            /* A positive short result installed precisely its prefix. Retain
             * ownership and bank reservation for each untouched suffix fd. */
            defer_import(id);
            return;
        }
        /* Preflight excludes unsupported kernels before the first real accept.
         * If policy changes while running, later accepts use the original path. */
        ordinary_accept = 0;
        fprintf(stderr, "fixed admission registration: %s; resuming direct accepts\n", strerror(-res));
        for (unsigned i = 0; i < left; ++i) queue_alias_close(b->fds[i]);
        r->accepting -= left;
    }
    b->next = admission_free;
    admission_free = id;
}

/* A duplicate can retain an old epoll data key. Refresh that existing
 * registration using the still-owned fd and the current generation. */
static int add_import_watch(Ring *r, int fd, struct epoll_event *event) {
    if (epoll_ctl(r->parked_fd, EPOLL_CTL_ADD, fd, event) == 0) return 1;
    return errno == EEXIST && epoll_ctl(r->parked_fd, EPOLL_CTL_MOD, fd, event) == 0;
}

static void finish_import(Ring *r, unsigned id, int res) {
    AdmissionBatch *b = &admissions[id];
    unsigned done = res > 0 ? (unsigned)res : 0;
    if (done > b->count) { errno = EOVERFLOW; fatal("fixed admission prefix"); }
    /* Small prefixes cannot amortize a registration enter and its result CQEs. */
    int batch_watches = ring_watches && done >= 4;
    if (batch_watches) {
        reserve(r, done);
        b->pending_watches = b->prefix = done;
    }
    for (unsigned i = 0; i < done; ++i) {
        unsigned index = (unsigned)b->indices[i];
        if (index >= slots) { errno = EOVERFLOW; fatal("fixed admission slot"); }
        --r->accepting;
        uint64_t k = accept_conn(b->ring, index);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP | EPOLLET, .data.u64 = k};
        if (batch_watches) {
            b->watches[i] = ev;
            uint64_t tag = (uint64_t)id * IMPORT_WIDTH + i;
            struct io_uring_sqe *s = sqe(r, IORING_OP_EPOLL_CTL, op(WATCH_IMPORT, tag), r->parked_fd);
            s->addr = (uintptr_t)&b->watches[i];
            s->off = (unsigned)b->fds[i];
            s->len = EPOLL_CTL_ADD;
            /* An edge can precede the registration CQE. Only a successful ADD
             * can expose this socket to the bank; authorize its edge now. */
            r->conns[index].flags |= PARKED;
        } else if (add_import_watch(r, b->fds[i], &ev)) {
            r->conns[index].flags |= PARKED;
        } else receive(k, MULTIRECV);
    }
    if (!batch_watches) retire_import(r, id, done, res);
}

static void finish_watch(Ring *r, uint64_t tag, int res) {
    unsigned id = (unsigned)(tag / IMPORT_WIDTH), index = (unsigned)(tag % IMPORT_WIDTH);
    AdmissionBatch *b = &admissions[id];
    if (res < 0) {
        if (res == -EINVAL || res == -EOPNOTSUPP || res == -EPERM) ring_watches = 0;
        struct epoll_event *ev = &b->watches[index];
        uint64_t key = ev->data.u64;
        Connection *c = find_conn(key);
        if (c && !add_import_watch(r, b->fds[index], ev)) {
            c->flags &= ~PARKED;
            receive(key, MULTIRECV);
        }
    }
    if (!--b->pending_watches) retire_import(r, id, b->prefix, (int)b->prefix);
}

/* Completion order is immaterial. Ordinary aliases and provisional keys stay
 * exclusively owned until the update and every individual ADD have returned. */
static void finish_named(Ring *r, unsigned id) {
    AdmissionBatch *b = &admissions[id];
    int res = b->update_result;
    if (res == INT32_MIN || b->pending_watches) return;
    unsigned done = res > 0 ? (unsigned)res : 0;
    if (done > b->count) { errno = EOVERFLOW; fatal("named admission prefix"); }
    for (unsigned i = 0; i < b->count; ++i) {
        Connection *c = &r->conns[b->first_slot + i];
        unsigned initial = c->flags;
        c->flags = 0;
        if (i >= done) continue;
        struct epoll_event *ev = &b->watches[i];
        uint64_t k = ev->data.u64;
        ++r->live;
        --r->accepting;
        c->flags = OPEN | (initial & HANGUP);
        if (!(b->rejected_watches & (UINT64_C(1) << i)) || add_import_watch(r, b->fds[i], ev)) {
            c->flags |= PARKED;
            if (initial & EARLY_EDGE) retry_receive(r, k, c);
        } else receive(k, MULTIRECV);
    }
    --r->named_inflight;
    if (done != b->count) {
        r->kernel_allocates = 1;
        if (res == -EINVAL || res == -EPERM || res == -EOPNOTSUPP || res == -EACCES)
            named_imports = 0;
        /* A watch on a still-owned suffix alias can have consumed its edge.
         * The original importer refreshes an existing registration with MOD. */
    }
    retire_import(r, id, done, (int)done);
}

static void on_cqe(Ring *r, const struct io_uring_cqe *event) {
    unsigned kind = event->user_data >> 56;
    uint64_t k = event->user_data & UINT64_C(0x00ffffffffffffff);
    int own_buffer = kind == CREDITRECV;
    unsigned read_id = 0;
    if (own_buffer) {
        read_id = (unsigned)k;
        k = read_keys[read_id];
        kind = PARKRECV;
    }
    unsigned slot = which_slot(k);
    int res = event->res;
    if (kind == NAMED_IMPORT) {
        admissions[k].update_result = res;
        finish_named(r, (unsigned)k);
        return;
    }
    if (kind == NAMED_WATCH) {
        unsigned id = (unsigned)(k / IMPORT_WIDTH), i = (unsigned)(k % IMPORT_WIDTH);
        AdmissionBatch *b = &admissions[id];
        if (res < 0) {
            b->rejected_watches |= UINT64_C(1) << i;
            if (res == -EINVAL || res == -EOPNOTSUPP || res == -EPERM) ring_watches = 0;
        }
        --b->pending_watches;
        finish_named(r, id);
        return;
    }
    if (kind == WATCH_IMPORT) { finish_watch(r, k, res); return; }
    if (kind == IMPORT) { finish_import(r, (unsigned)k, res); return; }
    if (kind == NORMAL_ACCEPT) {
        --active_accept[slot];
        if (res >= 0) append_admission(r, res);
        else {
            --r->accepting;
            --ordinary_pending;
            if (res == -EMFILE || res == -ENFILE) ordinary_accept = 0;
        }
        return;
    }
    if (kind == ACCEPT) {
        --active_accept[slot];
        --r->accepting;
        if (res >= 0) {
            uint64_t peer = accept_conn(which_ring(k), (unsigned)res);
            if (can_install) install_ordinary(peer);
            else receive(peer, MULTIRECV);
        }
        return;
    }
    if (kind == CLOSE) {
        --r->live;
        return;
    }
    if (kind == IGNORE) return;
    if (kind == WAKE) {
        if (!(event->flags & IORING_CQE_F_MORE)) watch_ring(slot);
        return;
    }
    if (kind == PARKWAKE) {
        park_armed = 0;
        return;
    }
    if (kind == PARKRECV) --pending.inflight;  /* A closed socket still returns its credit. */
    Connection *c = find_conn(k);
    if (kind == INSTALL) {
        if (!c) { if (res >= 0) close(res); return; }
        if (res == -EINVAL || res == -EOPNOTSUPP) can_install = 0;
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP | EPOLLET, .data.u64 = k};
        if (res >= 0 && epoll_ctl(r->parked_fd, EPOLL_CTL_ADD, res, &ev) == 0) c->flags |= PARKED;
        else receive(k, MULTIRECV);
        if (res >= 0) close(res);
        return;
    }
    if (kind == SEND) {
        if (c && c->output) {
            send_result(k, c, res);
            if (c->output) issue_send(k, c);
        }
        return;
    }
    int has_buffer = (event->flags & IORING_CQE_F_BUFFER) != 0;
    unsigned bid = event->flags >> IORING_CQE_BUFFER_SHIFT;
    if (c) {
        unsigned redo = c->flags & REDO;
        c->flags &= ~(REDO | READING);
        if (res > 0 && (own_buffer || has_buffer)) {
            const char *input = own_buffer ? read_bytes[read_id] : r->buffers + (size_t)bid * BUFFER_SIZE;
            dispatch(k, c, input, (unsigned)res, READY_INPUT);
            if ((c->flags & OPEN) && !(c->flags & (EOF_READ | CLOSE_AFTER)) &&
                !(event->flags & IORING_CQE_F_MORE)) {
                if (c->flags & PARKED) {
                    if (redo || (c->flags & HANGUP) || (event->flags & IORING_CQE_F_SOCK_NONEMPTY) ||
                        res == (own_buffer ? CREDIT_BYTES : BUFFER_SIZE))
                        retry_receive(r, k, c);
                } else if (kind == TRYRECV && !(event->flags & IORING_CQE_F_SOCK_NONEMPTY))
                    receive(k, MULTIRECV);
                else retry_receive(r, k, c);
            }
        } else if (res == -ENOBUFS) {
            retry_receive(r, k, c);
        } else if (res == -EAGAIN || res == -EINTR) {
            if (c->flags & PARKED) {
                if (redo || res == -EINTR) retry_receive(r, k, c);
            } else if (!(c->flags & (CLOSE_AFTER | EOF_READ))) receive(k, MULTIRECV);
        } else {
            peer_eof(k, c);
        }
    }
    if (has_buffer) return_buffer(r, bid);
    if (own_buffer) read_free[read_returned++] = read_id;
}

static unsigned reap(Ring *r) {
    unsigned head = *r->cq_head, end = get_acquire(r->cq_tail);
    unsigned count = end - head;
    if (count > CQ_QUANTUM) count = CQ_QUANTUM;
    for (unsigned i = 0; i < count; ++i) on_cqe(r, &r->cqes[(head + i) & r->cq_mask]);
    if (count) put_release(r->cq_head, head + count);
    flush_admission(r);
    return count;
}

static int needs_enter(Ring *r) {
    return r->tail != get_acquire(r->sq_head) ||
           (get_acquire(r->sq_flags) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW));
}
static int start_listener(const char *host, const char *port) {
    char *end = NULL;
    long number = strtol(port, &end, 10);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)number)};
    if (!*port || *end || number <= 0 || number > 65535 || inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | (epoll_mode ? SOCK_NONBLOCK : 0), 0);
    if (fd < 0) return -1;
    int one = 1;
    /* Short request/reply exchanges normally have one response in flight.
     * Retain the kernel's chosen algorithm if this socket cannot select Reno. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, "reno", sizeof("reno") - 1);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, SOMAXCONN)) {
        close(fd);
        return -1;
    }
    /* listen() initializes the listener's delayed-ACK state. Setting the
     * interactive hint afterwards lets children inherit it during the TCP
     * handshake, even when their first request arrives before accept().
     * Linux can then acknowledge that request in the first response. Its
     * usual delayed-ACK timer still handles slow applications and loss. */
    int quick_ack = 0;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quick_ack, sizeof(quick_ack));
    return fd;
}

/* Under continuous load, the issuer scans all completion queues on every pass.
 * Each secondary eventfd signal would schedule one more multishot poll on ring
 * zero. Keep the poll installed so idle wakeups need no registration syscall. */
static void mute_secondaries(void) {
    for (unsigned i = 1; i < ring_count; ++i) {
        unsigned *flags = rings[i].cq_flags;
        if (flags) __atomic_fetch_or(flags, IORING_CQ_EVENTFD_DISABLED, __ATOMIC_RELAXED);
    }
    secondary_muted = 1;
}

static int enable_and_recheck(void) {
    for (unsigned i = 1; i < ring_count; ++i) {
        unsigned *flags = rings[i].cq_flags;
        if (flags) __atomic_fetch_and(flags, ~IORING_CQ_EVENTFD_DISABLED, __ATOMIC_RELAXED);
    }
    secondary_muted = 0;
    /* Publish all enables before re-reading any tails. Work posted with a
     * notification disabled is visible here. Later work again wakes ring zero. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < ring_count; ++i) {
        Ring *r = &rings[i];
        if (*r->cq_head != get_acquire(r->cq_tail) ||
            (get_acquire(r->sq_flags) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW))) return 1;
    }
    return 0;
}

static void ring_loop(void) {
    unsigned rotation = 0, busy_passes = 0;
    park_fd = epoll_create1(EPOLL_CLOEXEC);
    if (park_fd < 0) fatal("park epoll_create");
    register_bank(0);
    alias_limit = slots / 4;
    if (alias_limit > ALIAS_LIMIT) alias_limit = ALIAS_LIMIT;
    ordinary_accept = probe_import(&rings[0]);
    named_imports = ordinary_accept;
    fprintf(stderr, "socket admission: %s (up to %u temporary descriptors)\n",
            ordinary_accept ? "bulk fixed-file updates" : "direct accepts", alias_limit);
    for (;;) {
        pending.stamp = 0;
        retry_admissions();
        replenish_accepts();
        unsigned first = rotation++ % ring_count;
        unsigned issued = issue_park();
        int work = issued || runnable();
        budget = PASS_REPLIES - issued;
        drain_ready();
        flush_bank();
        budget += issued;
        for (unsigned j = 0; j < ring_count; ++j) {
            Ring *r = &rings[(first + j) % ring_count];
            unsigned room = unused + MAX_OUT - allocated;
            unsigned max = PARK_WINDOW - pending.inflight;
            if (max > budget) max = budget;
            if (max > room) max = room;
            if (collect_bank(r, max)) work = 1;
            if (has_items(&r->retry)) service_retries(r);
            if (needs_enter(r)) { enter_ring(r, 0, -1); work = 1; }
            if (reap(r)) work = 1;
            flush_bank();
            if (needs_enter(r)) { enter_ring(r, 0, -1); work = 1; }
        }
        close_aliases();
        drain_buffered();
        finish_pass();
        if (work || runnable() || (pending.count && output_available())) {
            if (!secondary_muted && ring_count > 1 && ++busy_passes >= 2) mute_secondaries();
            continue;
        }
        busy_passes = 0;
        if (secondary_muted && enable_and_recheck()) continue;
        arm_banks();
        arm_park_before_wait();
        /* The parent poll handles new socket readiness and the primary ring's
         * eventfd watches receive completions from every secondary. */
        enter_ring(&rings[0], 1, has_items(&held_writes) || delayed_imports ? 1000000 : -1);
    }
}

static void epoll_accept(unsigned listener) {
    for (unsigned i = 0; i < ACCEPTS_PER_LISTENER; ++i) {
        int fd = accept4(listeners[listener], NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if ((unsigned)fd >= slots) { close(fd); continue; }
        uint64_t k = accept_conn(0, (unsigned)fd);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u64 = k};
        if (epoll_ctl(poll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
            close_conn(k, find_conn(k));
    }
}
static void epoll_loop(void) {
    struct epoll_event events[512];
    char input[BUFFER_SIZE];
    for (;;) {
        int n = epoll_wait(poll_fd, events, 512, runnable() ? 0 : has_items(&held_writes) ? 1 : -1);
        if (n < 0 && errno != EINTR) fatal("epoll_wait sockets");
        budget = PASS_REPLIES;
        drain_ready();
        for (int i = 0; i < n; ++i) {
            uint64_t k = events[i].data.u64;
            unsigned mask = events[i].events;
            if (k >> 56 == ACCEPT) {
                epoll_accept(which_slot(k));
                continue;
            }
            Connection *c = find_conn(k);
            if (!c) continue;
            if ((mask & EPOLLOUT) && c->output && !(c->flags & STAGED)) {
                issue_send(k, c);
                if (!(c->flags & OPEN)) continue;
            }
            if ((mask & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) && !(c->flags & (EOF_READ | CLOSE_AFTER))) {
                ssize_t len = recv((int)which_slot(k), input, sizeof(input), 0);
                if (len > 0) dispatch(k, c, input, (size_t)len, READY_INPUT);
                else if (!len || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    peer_eof(k, c);
                    if ((c->flags & OPEN) && (c->flags & WRITABLE)) epoll_interest(k, c, 1);
                }
            }
        }
        finish_pass();
    }
}

int reactor_run(const char *host, const char *port) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) { perror("getrlimit"); return 1; }
    if (limit.rlim_cur < limit.rlim_max) {
        struct rlimit raised = {limit.rlim_max, limit.rlim_max};
        if (!setrlimit(RLIMIT_NOFILE, &raised)) limit = raised;
    }
    slots = limit.rlim_cur > 65535 ? 65535 : (unsigned)limit.rlim_cur;
    if (slots < 64) { fprintf(stderr, "at least 64 file slots required\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    poll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (poll_fd < 0) { perror("epoll_create1"); return 1; }
    if (new_ring(0) < 0) {
        fprintf(stderr, "io_uring unavailable (%s), using epoll\n", strerror(errno));
        epoll_mode = 1;
        rings[0].conns = calloc(slots, sizeof(Connection));
        if (!rings[0].conns) return 1;
        ring_count = 1;
    }
    for (unsigned l = 0; l < LISTENERS; ++l) {
        listeners[l] = start_listener(host, port);
        if (listeners[l] < 0) { perror("listen"); return 1; }
        if (epoll_mode) {
            struct epoll_event ev = {.events = EPOLLIN, .data.u64 = op(ACCEPT, l)};
            if (epoll_ctl(poll_fd, EPOLL_CTL_ADD, listeners[l], &ev)) { perror("listen epoll"); return 1; }
        }
    }
    fprintf(stderr, "listening on %s:%s (%s)\n", host, port, epoll_mode ? "epoll" : "io_uring");
    if (epoll_mode) epoll_loop();
    else ring_loop();
    return 0;
}
