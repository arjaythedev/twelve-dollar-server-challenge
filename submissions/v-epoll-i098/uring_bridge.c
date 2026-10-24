#define _GNU_SOURCE
#include "uring_bridge.h"
#include <liburing.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define RINGS 8
#define SLOTS 65535
#define BUFFERS 1024
#define BUFFER_SIZE 2048
#define ACCEPT_DEPTH 64
#define SQ_SIZE 2048
#define CQ_SIZE 8192
#define BATCH_NS 200000 /* Bound completion coalescing before one SQLite transaction. */
#define PENDING_LIMIT (1u << 20) /* Input held while the application is busy; more closes the client. */
#define TD_CANCEL (UINT64_MAX - 1)

enum { FREE, ACCEPT, RECV, SEND, CLOSE, APPLICATION };

/* One multishot receive stays armed per client, so a request costs no receive submission.
 * Input that arrives while the application is busy goes to `pending`, and the receive is
 * cancelled until the application asks for input again, which keeps backpressure in the socket. */
struct td_client {
    unsigned char *output, *pending;
    unsigned output_len, sent, pending_len;
    unsigned generation;
    int next;
    unsigned char operation, queued, busy, close_after, notify_drain;
    unsigned char armed, cancelling, eof;
};

struct td_ring {
    struct io_uring ring;
    struct io_uring_buf_ring *buffers;
    unsigned char *memory;
    struct td_client *clients;
    int *free_slots;
    int free_count, accepting;
    int ready_head, ready_tail, retry_head, retry_tail, deliver_head, deliver_tail;
    uint64_t retry_at, accept_at;
};

struct td_net {
    struct td_ring rings[RINGS];
    struct pollfd polls[RINGS + 1];
    int listener, cursor, checkpoint_waiting;
};

_Static_assert(TD_CAPACITY == RINGS * SLOTS, "capacity mismatch");
_Static_assert(SLOTS <= 65535, "fixed table exceeds the process descriptor limit");

static void td_fail(const char *operation, int result)
{
    fprintf(stderr, "io_uring: %s: %s (%d)\n", operation,
            strerror(result < 0 ? -result : result), result);
    abort();
}

static uint64_t td_now(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) td_fail("clock_gettime", errno);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void td_enqueue(struct td_ring *r, int slot, int operation)
{
    struct td_client *c = &r->clients[slot];
    assert(!c->busy && !c->queued);
    c->operation = operation;
    c->queued = 1;
    c->next = -1;
    if (r->ready_tail < 0) r->ready_head = slot;
    else r->clients[r->ready_tail].next = slot;
    r->ready_tail = slot;
}

static void td_retry(struct td_ring *r, int slot)
{
    struct td_client *c = &r->clients[slot];
    assert(!c->busy && !c->queued);
    c->queued = 1;
    c->next = -1;
    if (r->retry_tail < 0) {
        r->retry_head = slot;
        r->retry_at = td_now() + 10;
    } else r->clients[r->retry_tail].next = slot;
    r->retry_tail = slot;
}

static void td_recycle(struct td_ring *r, int buffer)
{
    assert(buffer >= 0 && buffer < BUFFERS);
    io_uring_buf_ring_add(r->buffers, r->memory + buffer * BUFFER_SIZE,
                         BUFFER_SIZE, buffer, BUFFERS - 1, 0);
    io_uring_buf_ring_advance(r->buffers, 1);
}

/* The application waits for input: hand over held input, close at end of stream, or arm a receive. */
static void td_want_input(struct td_ring *r, int slot)
{
    struct td_client *c = &r->clients[slot];
    assert(!c->busy && !c->queued);
    if (c->pending_len) {
        c->operation = RECV;
        c->queued = 1;
        c->next = -1;
        if (r->deliver_tail < 0) r->deliver_head = slot;
        else r->clients[r->deliver_tail].next = slot;
        r->deliver_tail = slot;
    } else if (c->eof) td_enqueue(r, slot, CLOSE);
    else if (c->armed) c->operation = RECV;
    else td_enqueue(r, slot, RECV);
}

static void td_prep_cancel(struct io_uring_sqe *sqe, int slot)
{
    io_uring_prep_cancel_fd(sqe, slot, IORING_ASYNC_CANCEL_FD_FIXED | IORING_ASYNC_CANCEL_ALL);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    sqe->user_data = TD_CANCEL;
}

static void td_schedule(struct td_ring *r, uint64_t now)
{
    if (r->retry_head >= 0 && now >= r->retry_at) {
        if (r->ready_tail < 0) r->ready_head = r->retry_head;
        else r->clients[r->ready_tail].next = r->retry_head;
        r->ready_tail = r->retry_tail;
        r->retry_head = r->retry_tail = -1;
    }
    while (r->free_count && r->accepting < ACCEPT_DEPTH && now >= r->accept_at) {
        int slot = r->free_slots[--r->free_count];
        struct td_client *c = &r->clients[slot];
        assert(c->operation == FREE && !c->busy && !c->queued && !c->output && !c->pending && !c->armed);
        c->eof = 0; /* a receive cancelled by the last close can end after that close */
        if (++c->generation == 0) ++c->generation;
        td_enqueue(r, slot, ACCEPT);
        ++r->accepting;
    }
}

static void td_submit(struct td_net *net, struct td_ring *r)
{
    if (r == &net->rings[0] && !net->checkpoint_waiting) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
        if (sqe) {
            io_uring_prep_poll_add(sqe, net->polls[RINGS].fd, POLLIN);
            sqe->user_data = UINT64_MAX;
            net->checkpoint_waiting = 1;
        }
    }
    while (r->ready_head >= 0) {
        int slot = r->ready_head;
        struct td_client *c = &r->clients[slot];
        /* A close cancels the armed receive first; the hard link runs the close if nothing matched. */
        unsigned need = c->operation == CLOSE && c->armed ? 2 : 1;
        if (io_uring_sq_space_left(&r->ring) < need) break;
        struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
        if (need == 2) {
            td_prep_cancel(sqe, slot);
            sqe->flags |= IOSQE_IO_HARDLINK;
            sqe = io_uring_get_sqe(&r->ring);
        }
        r->ready_head = c->next;
        if (r->ready_head < 0) r->ready_tail = -1;
        c->queued = 0;
        c->busy = 1;
        switch (c->operation) {
        case ACCEPT:
            /* The slot stays reserved until the matching close CQE is consumed. */
            io_uring_prep_accept_direct(sqe, net->listener, NULL, NULL, 0, slot);
            break;
        case RECV:
            io_uring_prep_recv_multishot(sqe, slot, NULL, 0, 0);
            sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
            sqe->buf_group = 0;
            c->busy = 0;
            c->armed = 1;
            break;
        case SEND:
            assert(c->sent < c->output_len);
            io_uring_prep_send(sqe, slot, c->output + c->sent,
                               c->output_len - c->sent, MSG_NOSIGNAL);
            sqe->flags |= IOSQE_FIXED_FILE;
            break;
        case CLOSE:
            io_uring_prep_close_direct(sqe, slot);
            break;
        default:
            td_fail("invalid queued operation", EINVAL);
        }
        sqe->user_data = ((uint64_t)c->generation << 32) | ((uint64_t)slot << 8) | c->operation;
    }
    int rc = io_uring_submit(&r->ring);
    if (rc < 0 && rc != -EINTR && rc != -EBUSY && rc != -EAGAIN)
        td_fail("submit", rc);
    if (*r->ring.sq.kdropped) td_fail("dropped SQE", EOVERFLOW);
    /* NODROP retains overflow completions. Enter the kernel to move them into the CQ. */
    if (*r->ring.sq.kflags & IORING_SQ_CQ_OVERFLOW) {
        rc = io_uring_get_events(&r->ring);
        if (rc < 0 && rc != -EINTR && rc != -EBUSY) td_fail("flush CQ overflow", rc);
    }
}

struct td_net *td_net_new(int listener, int checkpoint)
{
    struct utsname kernel;
    int major = 0;
    if (uname(&kernel) || sscanf(kernel.release, "%d", &major) != 1 || major < 6)
        td_fail("Linux 6.0 or newer is required", ENOTSUP);
    struct td_net *net = calloc(1, sizeof(*net));
    if (!net) td_fail("allocate network state", ENOMEM);
    net->listener = listener;
    int one = 1;
    /* Accepted sockets inherit TCP_NODELAY without entering the process file table. */
    if (setsockopt(listener, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)))
        td_fail("listener TCP_NODELAY", errno);
    for (int i = 0; i < RINGS; ++i) {
        struct td_ring *r = &net->rings[i];
        r->ready_head = r->ready_tail = r->retry_head = r->retry_tail = -1;
        r->deliver_head = r->deliver_tail = -1;
        struct io_uring_params params = {
            .flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN,
            .cq_entries = CQ_SIZE
        };
        int rc = io_uring_queue_init_params(SQ_SIZE, &r->ring, &params);
        if (rc < 0) td_fail("create ring", rc);
        if (!(params.features & IORING_FEAT_NODROP)) td_fail("NODROP is required", ENOTSUP);
        rc = io_uring_register_files_sparse(&r->ring, SLOTS);
        if (rc < 0) td_fail("register 65535 sparse files", rc);
        r->buffers = io_uring_setup_buf_ring(&r->ring, BUFFERS, 0, 0, &rc);
        if (!r->buffers) td_fail("register buffer ring", rc);
        r->memory = malloc(BUFFERS * BUFFER_SIZE);
        r->clients = calloc(SLOTS, sizeof(*r->clients));
        r->free_slots = malloc(SLOTS * sizeof(*r->free_slots));
        if (!r->memory || !r->clients || !r->free_slots) td_fail("allocate ring state", ENOMEM);
        for (int slot = 0; slot < SLOTS; ++slot) r->free_slots[r->free_count++] = slot;
        for (int b = 0; b < BUFFERS; ++b)
            io_uring_buf_ring_add(r->buffers, r->memory + b * BUFFER_SIZE,
                                 BUFFER_SIZE, b, BUFFERS - 1, b);
        io_uring_buf_ring_advance(r->buffers, BUFFERS);
        net->polls[i] = (struct pollfd){ .fd = r->ring.ring_fd, .events = POLLIN };
    }
    net->polls[RINGS] = (struct pollfd){ .fd = checkpoint, .events = POLLIN };
    return net;
}

static int td_received(struct td_ring *r, int ring_index, int slot, const struct io_uring_cqe *cqe,
                       int buffer, struct td_event *event)
{
    struct td_client *c = &r->clients[slot];
    int result = cqe->res;
    if (!(cqe->flags & IORING_CQE_F_MORE)) c->armed = c->cancelling = 0;
    int waiting = c->operation == RECV && !c->queued;
    if (result > 0) {
        if (buffer < 0 || buffer >= BUFFERS || result > BUFFER_SIZE)
            td_fail("invalid receive buffer", EOVERFLOW);
        if (waiting) {
            c->operation = APPLICATION;
            *event = (struct td_event){ .kind = 2, .id = ring_index * SLOTS + slot,
                                        .data = r->memory + buffer * BUFFER_SIZE,
                                        .len = result, .buffer = buffer };
            return 1;
        }
        if (c->operation != CLOSE && c->operation != FREE && !c->eof) {
            if (c->pending_len + result > PENDING_LIMIT) c->eof = 1;
            else {
                unsigned char *pending = realloc(c->pending, c->pending_len + result);
                if (!pending) td_fail("allocate held input", ENOMEM);
                memcpy(pending + c->pending_len, r->memory + buffer * BUFFER_SIZE, result);
                c->pending = pending;
                c->pending_len += result;
            }
            if (c->armed && !c->cancelling) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
                if (sqe) {
                    td_prep_cancel(sqe, slot);
                    c->cancelling = 1;
                }
            }
        }
        td_recycle(r, buffer);
        return 0;
    }
    if (buffer >= 0) td_recycle(r, buffer);
    int transient = result == -ENOBUFS || result == -EAGAIN || result == -EINTR || result == -ENOMEM;
    if (result != -ECANCELED && !transient) c->eof = 1; /* end of stream or a failed socket */
    if (!waiting || c->armed) return 0;
    if (c->eof) td_enqueue(r, slot, CLOSE);
    else if (transient) td_retry(r, slot);
    else td_enqueue(r, slot, RECV);
    return 0;
}

static int td_complete(struct td_ring *r, int ring_index, const struct io_uring_cqe *cqe,
                       struct td_event *event)
{
    int slot = (cqe->user_data >> 8) & 0xffff;
    int operation = cqe->user_data & 0xff;
    unsigned generation = cqe->user_data >> 32;
    int buffer = cqe->flags & IORING_CQE_F_BUFFER ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : -1;
    if (slot >= SLOTS || operation < ACCEPT || operation > CLOSE)
        td_fail("invalid completion tag", EINVAL);
    struct td_client *c = &r->clients[slot];
    if (c->generation != generation) {
        if (buffer >= 0) td_recycle(r, buffer);
        return 0;
    }
    if (operation == RECV) return td_received(r, ring_index, slot, cqe, buffer, event);
    assert(c->busy && c->operation == operation);
    c->busy = 0;
    int result = cqe->res;
    *event = (struct td_event){ .id = ring_index * SLOTS + slot, .buffer = -1 };
    if (operation == ACCEPT) {
        --r->accepting;
        if (result < 0) {
            c->operation = FREE;
            r->free_slots[r->free_count++] = slot;
            if (result == -ENFILE || result == -EMFILE || result == -ENOBUFS || result == -ENOMEM ||
                result == -EAGAIN || result == -ENETDOWN || result == -ENONET ||
                result == -EHOSTDOWN || result == -EHOSTUNREACH || result == -ENETUNREACH ||
                result == -ENOPROTOOPT || result == -EOPNOTSUPP)
                r->accept_at = td_now() + 50;
            else if (result != -EINTR && result != -ECONNABORTED && result != -EPROTO)
                td_fail("direct accept", result);
            return 0;
        }
        if (result != 0) td_fail("direct accept did not return zero", EINVAL);
        c->operation = APPLICATION;
        event->kind = 1;
        return 1;
    }
    if (operation == CLOSE) {
        if (result < 0) {
            if (result == -EINTR || result == -EAGAIN || result == -ENOMEM) {
                td_retry(r, slot);
                return 0;
            }
            td_fail("direct close", result);
        }
        free(c->output);
        free(c->pending);
        c->output = c->pending = NULL;
        c->output_len = c->sent = c->pending_len = c->close_after = 0;
        c->armed = c->cancelling = c->eof = 0;
        c->operation = FREE;
        r->free_slots[r->free_count++] = slot;
        event->kind = 3;
        return 1;
    }
    if (result < 0) {
        if (buffer >= 0) td_recycle(r, buffer);
        if (result == -ENOBUFS || result == -EAGAIN || result == -EINTR || result == -ENOMEM) {
            td_retry(r, slot);
        } else td_enqueue(r, slot, CLOSE);
        return 0;
    }
    if (result == 0 || (unsigned)result > c->output_len - c->sent) {
        td_enqueue(r, slot, CLOSE);
        return 0;
    }
    c->sent += result;
    if (c->sent < c->output_len) td_enqueue(r, slot, SEND);
    else {
        free(c->output);
        c->output = NULL;
        c->output_len = c->sent = 0;
        if (c->close_after) td_enqueue(r, slot, CLOSE);
        else if (c->notify_drain) {
            c->operation = APPLICATION;
            event->kind = 4;
            return 1;
        } else td_want_input(r, slot);
    }
    return 0;
}

int td_net_next(struct td_net *net, struct td_event *events, int capacity, int timeout_ms)
{
    assert(capacity > 0);
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : td_now() + timeout_ms;
    int count = 0;
    uint64_t flush_at = 0;
    for (;;) {
        uint64_t now = td_now();
        int internal = 0, pending = 0;
        uint64_t wake = deadline;
        for (int j = 0; j < RINGS; ++j) {
            int i = (net->cursor + j) % RINGS;
            struct td_ring *r = &net->rings[i];
            td_schedule(r, now);
            td_submit(net, r);
            while (r->deliver_head >= 0 && count < capacity) {
                int slot = r->deliver_head;
                struct td_client *c = &r->clients[slot];
                r->deliver_head = c->next;
                if (r->deliver_head < 0) r->deliver_tail = -1;
                c->queued = 0;
                c->operation = APPLICATION;
                /* buffer -2: td_net_release frees the held input */
                events[count++] = (struct td_event){ .kind = 2, .id = i * SLOTS + slot, .data = c->pending,
                                                     .len = (int)c->pending_len, .buffer = -2 };
                c->pending = NULL;
                c->pending_len = 0;
            }
            struct io_uring_cqe *cqe;
            unsigned head, consumed = 0;
            io_uring_for_each_cqe(&r->ring, head, cqe) {
                if (count == capacity || consumed == CQ_SIZE) break;
                if (cqe->user_data == TD_CANCEL) {
                    /* -ENOENT or -EALREADY: the receive already ended or is ending. */
                } else if (cqe->user_data == UINT64_MAX) {
                    if (cqe->res < 0 || !(cqe->res & POLLIN))
                        td_fail("checkpoint completion", cqe->res < 0 ? cqe->res : EIO);
                    events[count++] = (struct td_event){ .kind = 5, .id = -1, .buffer = -1 };
                } else count += td_complete(r, i, cqe, &events[count]);
                ++consumed;
            }
            io_uring_cq_advance(&r->ring, consumed);
            internal |= consumed != 0;
            pending |= r->ready_head >= 0 || io_uring_sq_ready(&r->ring) != 0;
            if (r->retry_head >= 0 && r->retry_at < wake) wake = r->retry_at;
            if (r->free_count && r->accepting < ACCEPT_DEPTH && r->accept_at > now && r->accept_at < wake)
                wake = r->accept_at;
        }
        net->cursor = (net->cursor + 1) % RINGS;
        if (count) {
            struct timespec clock;
            if (clock_gettime(CLOCK_MONOTONIC, &clock)) td_fail("batch clock", errno);
            uint64_t nanos = (uint64_t)clock.tv_sec * 1000000000 + clock.tv_nsec;
            if (!flush_at) flush_at = nanos + BATCH_NS;
            if (count == capacity || nanos >= flush_at || td_now() >= deadline) return count;
            struct timespec pause = { .tv_nsec = (long)(flush_at - nanos) };
            int rc = ppoll(net->polls, RINGS, &pause, NULL);
            if (rc < 0 && errno != EINTR) td_fail("batch poll", errno);
            continue;
        }
        now = td_now();
        if (now >= deadline) return 0;
        if (internal) continue;
        /* A transient submit error must not strand unsent SQEs in poll forever. */
        if (pending && wake > now + 1) wake = now + 1;
        int wait = wake == UINT64_MAX ? -1 : (wake <= now ? 0 : (int)(wake - now > INT_MAX ? INT_MAX : wake - now));
        int rc = poll(net->polls, RINGS, wait);
        if (rc < 0 && errno != EINTR) td_fail("poll rings", errno);
        for (int i = 0; i < RINGS; ++i)
            if (net->polls[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                td_fail("ring or checkpoint fd failed", EIO);
    }
}

void td_net_release(struct td_net *net, const struct td_event *event)
{
    if (event->kind == 5) net->checkpoint_waiting = 0;
    if (event->buffer >= 0) td_recycle(&net->rings[event->id / SLOTS], event->buffer);
    else if (event->buffer == -2) free(event->data);
}

void td_net_send(struct td_net *net, int id, const unsigned char *data, int len)
{
    assert(id >= 0 && id < TD_CAPACITY && len > 0);
    struct td_client *c = &net->rings[id / SLOTS].clients[id % SLOTS];
    assert(c->operation == APPLICATION && !c->busy && !c->queued);
    if ((unsigned)len > UINT_MAX - c->output_len) td_fail("output size", EOVERFLOW);
    unsigned size = c->output_len + len;
    unsigned char *output = realloc(c->output, size);
    if (!output) td_fail("allocate committed output", ENOMEM);
    memcpy(output + c->output_len, data, len);
    c->output = output;
    c->output_len = size;
}

void td_net_resume(struct td_net *net, int id, int close_after, int notify_drain)
{
    assert(id >= 0 && id < TD_CAPACITY);
    struct td_ring *r = &net->rings[id / SLOTS];
    struct td_client *c = &r->clients[id % SLOTS];
    assert(c->operation == APPLICATION && !c->busy && !c->queued);
    c->close_after = close_after != 0;
    c->notify_drain = notify_drain != 0;
    if (c->output_len || c->close_after) td_enqueue(r, id % SLOTS, c->output_len ? SEND : CLOSE);
    else td_want_input(r, id % SLOTS);
}

void td_net_abort(struct td_net *net, int id)
{
    assert(id >= 0 && id < TD_CAPACITY);
    struct td_ring *r = &net->rings[id / SLOTS];
    struct td_client *c = &r->clients[id % SLOTS];
    assert(c->operation == APPLICATION && !c->busy && !c->queued);
    td_enqueue(r, id % SLOTS, CLOSE);
}
