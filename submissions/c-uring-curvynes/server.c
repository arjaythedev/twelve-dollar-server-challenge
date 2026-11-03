#define _GNU_SOURCE
#include "api.h"
#include <liburing.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { RINGS = 8, MAX_SLOTS = 65500, SQ_SIZE = 4096, BUFFER_COUNT = 4096,
       BUFFER_SIZE = 2048, CQ_BATCH = 512, MAX_REQUEST = 65536, MAX_BODY = 16384 };
enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_SHUTDOWN, OP_PROVIDE };
typedef struct {
    char *rx, *tx;
    size_t rx_len, rx_cap, tx_offset, tx_len, tx_sent;
    uint32_t generation;
    unsigned retry_previous, retry_next;
    bool alive, close_after_send, receiving, sending, closing, shutdown_queued, pending_commit, retry_queued, peer_eof;
} Connection;
typedef struct {
    struct io_uring ring;
    struct io_uring_buf_ring *br;
    char *buffers;
    Connection *connections;
    int listener;
    unsigned slots, active, retry_head, retry_tail, retry_count;
    unsigned legacy_retry_budget;
    bool ready, accepting, legacy_buffers;
} Worker;
typedef struct {
    Slice method, path, auth, body;
    size_t consumed;
    bool close;
} Request;
static Worker workers[RINGS];
static volatile sig_atomic_t running = 1;
static uint64_t requests, errors;
static uint64_t cq_operations[7], accept_full, recv_retries, cq_negative;
enum { MAX_BATCH = 1024 };
typedef struct { Worker *worker; unsigned index; uint32_t generation; } Pending;
static Pending pending[MAX_BATCH];
static unsigned pending_count;
static void signal_stop(int sig) { (void)sig; running = 0; }
static uint64_t monotonic_ns(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
}
static void record_stage(uint64_t peaks[5], unsigned stage, uint64_t *previous) {
    uint64_t now = monotonic_ns(), elapsed = now - *previous;
    if (elapsed > peaks[stage]) peaks[stage] = elapsed;
    *previous = now;
}

static uint64_t tag(unsigned operation, unsigned slot, uint32_t generation) {
    return ((uint64_t)generation << 32) | ((uint64_t)slot << 8) | operation;
}
static struct io_uring_sqe *sqe(Worker *w) {
    struct io_uring_sqe *entry = io_uring_get_sqe(&w->ring);
    if (!entry) {
        int rc = io_uring_submit(&w->ring);
        if (rc < 0) { fprintf(stderr, "io_uring_submit: %s\n", strerror(-rc)); running = 0; return NULL; }
        entry = io_uring_get_sqe(&w->ring);
    }
    if (!entry) { fprintf(stderr, "submission queue remained full\n"); running = 0; }
    return entry;
}
static void accept_connections(Worker *w) {
    if (w->accepting || w->active >= w->slots) return;
    struct io_uring_sqe *entry = sqe(w); if (!entry) return;
    io_uring_prep_multishot_accept_direct(entry, w->listener, NULL, NULL, 0);
    entry->user_data = OP_ACCEPT; w->accepting = true;
}
static void remove_retry(Worker *w, unsigned index) {
    Connection *c = &w->connections[index];
    if (!c->retry_queued) return;
    if (c->retry_previous == UINT_MAX) w->retry_head = c->retry_next;
    else w->connections[c->retry_previous].retry_next = c->retry_next;
    if (c->retry_next == UINT_MAX) w->retry_tail = c->retry_previous;
    else w->connections[c->retry_next].retry_previous = c->retry_previous;
    c->retry_queued = false; c->retry_previous = c->retry_next = UINT_MAX;
    w->retry_count--;
}
static void queue_retry(Worker *w, unsigned index) {
    Connection *c = &w->connections[index];
    if (c->retry_queued || c->receiving || c->closing || c->peer_eof || !c->alive) return;
    c->retry_queued = true; c->retry_previous = w->retry_tail; c->retry_next = UINT_MAX;
    if (w->retry_tail == UINT_MAX) w->retry_head = index;
    else w->connections[w->retry_tail].retry_next = index;
    w->retry_tail = index; w->retry_count++;
}
static void receive(Worker *w, unsigned index) {
    Connection *c = &w->connections[index];
    if (c->receiving || c->closing || c->retry_queued || c->peer_eof) return;
    struct io_uring_sqe *entry = sqe(w); if (!entry) return;
    io_uring_prep_recv_multishot(entry, (int)index, NULL, 0, 0);
    entry->ioprio |= IORING_RECVSEND_POLL_FIRST;
    entry->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    entry->buf_group = 0;
    entry->user_data = tag(OP_RECV, index, w->connections[index].generation);
    c->receiving = true;
}
static void retry_receives(Worker *w) {
    if (!w->retry_count) return;
    int available = w->legacy_buffers ? (int)w->legacy_retry_budget : io_uring_buf_ring_available(&w->ring, w->br, 0);
    if (available < 0) { fprintf(stderr, "provided-buffer status: %s\n", strerror(-available)); running = 0; return; }
    unsigned budget = available > 512 ? 512U : (unsigned)available;
    /* A failed multishot receive is rearmed only after buffers return. One
       queue node per connection bounds retries and preserves FIFO fairness. */
    while (budget-- && w->retry_head != UINT_MAX) {
        unsigned index = w->retry_head; remove_retry(w, index);
        Connection *c = &w->connections[index];
        if (w->legacy_buffers) w->legacy_retry_budget--;
        if (c->alive && !c->closing) receive(w, index);
    }
}
static void send_reply(Worker *w, unsigned index) {
    Connection *c = &w->connections[index];
    struct io_uring_sqe *entry = sqe(w); if (!entry) return;
    io_uring_prep_send(entry, (int)index, c->tx + c->tx_offset + c->tx_sent,
                       c->tx_len - c->tx_sent, MSG_NOSIGNAL);
    entry->flags = IOSQE_FIXED_FILE;
    entry->user_data = tag(OP_SEND, index, c->generation);
    c->sending = true;
}
static void close_connection(Worker *w, unsigned index) {
    Connection *c = &w->connections[index];
    if (!c->alive) return;
    c->closing = true;
    remove_retry(w, index);
    if (c->receiving && !c->shutdown_queued) {
        struct io_uring_sqe *stop = sqe(w); if (!stop) return;
        io_uring_prep_shutdown(stop, (int)index, SHUT_RDWR);
        stop->flags = IOSQE_FIXED_FILE;
        stop->user_data = tag(OP_SHUTDOWN, index, c->generation);
        c->shutdown_queued = true;
    }
    if (c->receiving || c->sending) return;
    c->alive = false; w->active--;
    c->pending_commit = false;
    free(c->rx); free(c->tx); c->rx = c->tx = NULL;
    c->rx_cap = c->rx_len = c->tx_len = c->tx_sent = 0;
    struct io_uring_sqe *entry = sqe(w); if (!entry) return;
    io_uring_prep_close_direct(entry, index);
    entry->user_data = tag(OP_CLOSE, index, c->generation);
}
static bool provide_buffers(Worker *w, unsigned id, unsigned count) {
    struct io_uring_sqe *entry = sqe(w); if (!entry) return false;
    io_uring_prep_provide_buffers(entry, w->buffers + (size_t)id * BUFFER_SIZE,
                                  BUFFER_SIZE, (int)count, 0, (int)id);
    entry->flags = IOSQE_CQE_SKIP_SUCCESS;
    entry->user_data = tag(OP_PROVIDE, count, 0);
    /* A returned buffer grants at most one receive retry. It is not an
       estimate of kernel availability: completions may still be pending. */
    w->legacy_retry_budget += count;
    if (w->legacy_retry_budget > BUFFER_COUNT) w->legacy_retry_budget = BUFFER_COUNT;
    return true;
}
static void recycle(Worker *w, unsigned id) {
    if (w->legacy_buffers) { provide_buffers(w, id, 1); return; }
    io_uring_buf_ring_add(w->br, w->buffers + (size_t)id * BUFFER_SIZE,
                         BUFFER_SIZE, (unsigned short)id, BUFFER_COUNT - 1, 0);
    io_uring_buf_ring_advance(w->br, 1);
}
static bool append_rx(Connection *c, const char *source, size_t n) {
    if (n > MAX_REQUEST - c->rx_len) return false;
    if (c->rx_len + n > c->rx_cap) {
        size_t capacity = c->rx_cap ? c->rx_cap : 1024;
        while (capacity < c->rx_len + n) capacity *= 2;
        char *p = realloc(c->rx, capacity);
        if (!p) return false;
        c->rx = p; c->rx_cap = capacity;
    }
    memcpy(c->rx + c->rx_len, source, n); c->rx_len += n; return true;
}
static bool header_equal(Slice s, const char *name) {
    return s.n == strlen(name) && strncasecmp(s.p, name, s.n) == 0;
}
static bool token_present(Slice s, const char *token) {
    size_t i = 0;
    while (i < s.n) {
        while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t' || s.p[i] == ',')) i++;
        size_t start = i;
        while (i < s.n && s.p[i] != ',') i++;
        size_t end = i;
        while (end > start && (s.p[end-1] == ' ' || s.p[end-1] == '\t')) end--;
        if (header_equal((Slice){s.p + start, end - start}, token)) return true;
    }
    return false;
}
static const char *crlf(const char *p, size_t n) {
    return n < 2 ? NULL : memmem(p, n, "\r\n", 2);
}
/* Determine framing before changing the input. A second pass compacts chunks. */
static int chunks(char *p, size_t n, size_t *consumed, size_t *decoded) {
    size_t at = 0, total = 0;
    for (;;) {
        const char *end = crlf(p + at, n - at);
        if (!end) return 0;
        size_t line_end = (size_t)(end - p), i = at, size = 0, digits = 0;
        for (; i < line_end && p[i] != ';'; i++) {
            unsigned char c = (unsigned char)p[i]; unsigned digit;
            if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
            else return -1;
            if (++digits > 8 || size > MAX_BODY / 16) return -1;
            size = size * 16 + digit;
        }
        if (!digits || size > MAX_BODY - total) return -1;
        at = line_end + 2;
        if (!size) {
            for (;;) {
                end = crlf(p + at, n - at);
                if (!end) return 0;
                size_t next = (size_t)(end - p) + 2;
                if (next == at + 2) { *consumed = next; *decoded = total; goto complete; }
                if (!memchr(p + at, ':', next - at - 2)) return -1;
                at = next;
            }
        }
        if (n - at < size + 2) return 0;
        if (memcmp(p + at + size, "\r\n", 2)) return -1;
        total += size; at += size + 2;
    }
complete:
    at = 0; size_t output = 0;
    while (output < total) {
        const char *end = crlf(p + at, n - at); size_t size = 0;
        for (const char *q = p + at; q < end && *q != ';'; q++) {
            unsigned char c = (unsigned char)*q;
            size = size * 16 + (c <= '9' ? (unsigned)(c - '0') : c <= 'F' ? (unsigned)(c - 'A' + 10) : (unsigned)(c - 'a' + 10));
        }
        at = (size_t)(end - p) + 2;
        memmove(p + output, p + at, size); output += size; at += size + 2;
    }
    return 1;
}
static int parse_request(Connection *c, Request *r) {
    char *p = c->rx; size_t n = c->rx_len;
    const char *boundary = n >= 4 ? memmem(p, n, "\r\n\r\n", 4) : NULL;
    if (!boundary) return n >= 32768 ? -1 : 0;
    size_t header_length = (size_t)(boundary - p) + 4;
    if (header_length > 32768) return -1;
    const char *line_end = crlf(p, header_length);
    const char *space = memchr(p, ' ', (size_t)(line_end - p));
    if (!space || space == p) return -1;
    r->method = (Slice){p, (size_t)(space - p)};
    const char *second = memchr(space + 1, ' ', (size_t)(line_end - space - 1));
    if (!second || second == space + 1 || space[1] != '/') return -1;
    r->path = (Slice){space + 1, (size_t)(second - space - 1)};
    Slice version = {second + 1, (size_t)(line_end - second - 1)};
    if (version.n != 8 || memcmp(version.p, "HTTP/1.", 7) || (version.p[7] != '1' && version.p[7] != '0')) return -1;
    r->close = version.p[7] == '0';
    bool length_seen = false, transfer_seen = false, auth_seen = false;
    size_t body_length = 0; const char *at = line_end + 2;
    while (at < boundary) {
        line_end = crlf(at, (size_t)(boundary + 2 - at));
        if (!line_end) return -1;
        const char *colon = memchr(at, ':', (size_t)(line_end - at));
        if (!colon || colon == at) return -1;
        for (const char *q = at; q < colon; q++) {
            unsigned char ch = (unsigned char)*q;
            if (!ch || !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("!#$%&'*+-.^_`|~", ch))) return -1;
        }
        Slice key = {at, (size_t)(colon - at)};
        const char *v = colon + 1, *last = line_end;
        while (v < last && (*v == ' ' || *v == '\t')) v++;
        while (last > v && (last[-1] == ' ' || last[-1] == '\t')) last--;
        for (const char *q = v; q < last; q++) if (((unsigned char)*q < 32 && *q != '\t') || *q == 127) return -1;
        Slice value = {v, (size_t)(last - v)};
        if (header_equal(key, "Content-Length")) {
            if (length_seen || !value.n) return -1;
            length_seen = true;
            for (size_t i = 0; i < value.n; i++) {
                if (value.p[i] < '0' || value.p[i] > '9' || body_length > MAX_BODY / 10) return -1;
                body_length = body_length * 10 + (unsigned)(value.p[i] - '0');
                if (body_length > MAX_BODY) return -1;
            }
        } else if (header_equal(key, "Transfer-Encoding")) {
            if (transfer_seen || !header_equal(value, "chunked")) return -1;
            transfer_seen = true;
        } else if (header_equal(key, "Authorization")) {
            if (auth_seen) return -1;
            auth_seen = true; r->auth = value;
        } else if (header_equal(key, "Connection")) {
            if (token_present(value, "close")) r->close = true;
            else if (version.p[7] == '0' && token_present(value, "keep-alive")) r->close = false;
        }
        at = line_end + 2;
    }
    if (transfer_seen && length_seen) return -1;
    if (transfer_seen) {
        size_t wire_length;
        int rc = chunks(p + header_length, n - header_length, &wire_length, &body_length);
        if (rc != 1) return rc;
        r->consumed = header_length + wire_length;
    } else {
        if (n - header_length < body_length) return 0;
        r->consumed = header_length + body_length;
    }
    r->body = (Slice){p + header_length, body_length};
    return 1;
}
static void flush_batch(void) {
    int failed = api_commit_batch();
    for (unsigned i = 0; i < pending_count; i++) {
        Pending *p = &pending[i]; Connection *c = &p->worker->connections[p->index];
        if (!c->alive || c->generation != p->generation || !c->pending_commit) continue;
        c->pending_commit = false;
        if (c->closing) { close_connection(p->worker, p->index); continue; }
        if (failed) {
            Reply error = {.b = {.p = c->tx, .cap = c->tx_len + c->tx_offset}};
            api_error(&error, 500, "internal server error");
            c->tx = error.b.p; errors++;
            if (api_finish(&error, c->close_after_send, &c->tx_offset, &c->tx_len)) {
                close_connection(p->worker, p->index); continue;
            }
        }
        send_reply(p->worker, p->index);
    }
    pending_count = 0;
}
static void dispatch(Worker *w, unsigned index) {
    if (pending_count == MAX_BATCH) flush_batch();
    Connection *c = &w->connections[index]; Request request = {0};
    int parsed = parse_request(c, &request);
    if (!parsed) {
        if (!c->peer_eof) { receive(w, index); return; }
        parsed = -1;
    }
    Reply reply = {0};
    if (parsed < 0) { api_error(&reply, 400, "malformed HTTP request"); request.close = true; }
    else api_handle(request.method, request.path, request.auth, request.body, &reply);
    if (c->peer_eof && parsed > 0 && c->rx_len == request.consumed) request.close = true;
    requests++; if (reply.status >= 500) errors++;
    if (api_finish(&reply, request.close, &c->tx_offset, &c->tx_len)) {
        free(reply.b.p); close_connection(w, index); return;
    }
    c->tx = reply.b.p; c->tx_sent = 0; c->close_after_send = request.close;
    if (parsed > 0 && c->rx_len > request.consumed) {
        memmove(c->rx, c->rx + request.consumed, c->rx_len - request.consumed);
        c->rx_len -= request.consumed;
    } else { free(c->rx); c->rx = NULL; c->rx_len = c->rx_cap = 0; }
    if (reply.pending_commit) {
        c->pending_commit = true;
        pending[pending_count++] = (Pending){w, index, c->generation};
    } else send_reply(w, index);
}
static void completion(Worker *w, struct io_uring_cqe *event) {
    uint64_t data = event->user_data;
    unsigned op = (unsigned)(data & 255), index = (unsigned)((data >> 8) & 0xffffff);
    uint32_t generation = (uint32_t)(data >> 32);
    int result = event->res;
    if (op < 7) cq_operations[op]++;
    if (result < 0) cq_negative++;
    if (op == OP_PROVIDE) {
        if (result < 0) { fprintf(stderr, "provide buffers: %s\n", strerror(-result)); running = 0; }
        return;
    }
    if (op == OP_ACCEPT && result == -ENFILE) accept_full++;
    if (op == OP_RECV && (result == -ENOBUFS || result == -EAGAIN || result == -EINTR)) recv_retries++;
    if (op == OP_ACCEPT) {
        if (!(event->flags & IORING_CQE_F_MORE)) w->accepting = false;
        if (result >= 0 && (unsigned)result < w->slots) {
            Connection *c = &w->connections[result];
            /* A direct slot can be reused after its close is issued. */
            c->generation++; c->alive = true; c->close_after_send = false;
            c->receiving = c->sending = c->closing = c->shutdown_queued = false;
            c->pending_commit = false;
            c->peer_eof = false;
            c->retry_queued = false; c->retry_previous = c->retry_next = UINT_MAX;
            w->active++; receive(w, (unsigned)result);
        } else if (result < 0 && result != -ENFILE && result != -ECANCELED && result != -EINTR && result != -ECONNABORTED) {
            fprintf(stderr, "direct accept: %s\n", strerror(-result)); running = 0;
        }
        return;
    }
    bool selected = (event->flags & IORING_CQE_F_BUFFER) != 0;
    unsigned id = event->flags >> IORING_CQE_BUFFER_SHIFT;
    if (index >= w->slots || w->connections[index].generation != generation || !w->connections[index].alive) {
        if (op == OP_RECV && selected) recycle(w, id);
        return;
    }
    Connection *c = &w->connections[index];
    if (op == OP_SHUTDOWN) {
        if (result == -EINTR || result == -EAGAIN) c->shutdown_queued = false;
        else c->receiving = false;
        /* Receives use shared buffers, not connection-owned memory. Once read
           shutdown completes, stale receive CQEs can safely retire by tag. */
        close_connection(w, index);
    } else if (op == OP_RECV) {
        if (!(event->flags & IORING_CQE_F_MORE)) c->receiving = false;
        if (c->closing) { if (selected) recycle(w, id); close_connection(w, index); return; }
        if (result == -ENOBUFS) {
            if (selected) recycle(w, id);
            queue_retry(w, index); return;
        }
        if (result == -EAGAIN || result == -EINTR) {
            if (selected) recycle(w, id);
            receive(w, index); return;
        }
        if (result <= 0 || !selected) {
            if (selected) recycle(w, id);
            if (result == 0) {
                c->peer_eof = true; remove_retry(w, index);
                if (c->tx) { if (!c->rx_len) c->close_after_send = true; }
                else if (c->rx_len) dispatch(w, index);
                else close_connection(w, index);
            } else close_connection(w, index);
            return;
        }
        bool appended = append_rx(c, w->buffers + (size_t)id * BUFFER_SIZE, (size_t)result);
        recycle(w, id);
        if (!appended) { close_connection(w, index); return; }
        if (!c->tx) dispatch(w, index);
        receive(w, index);
    } else if (op == OP_SEND) {
        c->sending = false;
        if (c->closing) { close_connection(w, index); return; }
        if (result == -EAGAIN || result == -EINTR) { send_reply(w, index); return; }
        if (result <= 0) { close_connection(w, index); return; }
        c->tx_sent += (size_t)result;
        if (c->tx_sent < c->tx_len) { send_reply(w, index); return; }
        free(c->tx); c->tx = NULL; c->tx_len = c->tx_sent = 0;
        if (c->close_after_send) close_connection(w, index);
        else if (c->rx_len) dispatch(w, index);
        else receive(w, index);
    }
}
static int worker_init(Worker *w, unsigned slots) {
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_SUBMIT_ALL |
                   IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN |
                   IORING_SETUP_TASKRUN_FLAG;
    params.cq_entries = SQ_SIZE * 4;
    int rc = io_uring_queue_init_params(SQ_SIZE, &w->ring, &params);
    if (rc < 0) { fprintf(stderr, "ring queues: %s\n", strerror(-rc)); return rc; }
    w->ready = true; w->slots = slots; w->retry_head = w->retry_tail = UINT_MAX;
    rc = io_uring_register_files_sparse(&w->ring, slots);
    if (rc < 0) { fprintf(stderr, "direct-file table: %s\n", strerror(-rc)); return rc; }
    w->connections = calloc(slots, sizeof(Connection));
    w->buffers = mmap(NULL, (size_t)BUFFER_COUNT * BUFFER_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!w->connections || w->buffers == MAP_FAILED) { fputs("connection/buffer allocation failed\n", stderr); return -ENOMEM; }
    w->br = io_uring_setup_buf_ring(&w->ring, BUFFER_COUNT, 0, 0, &rc);
    if (!w->br) {
        if (rc != -EINVAL && rc != -EOPNOTSUPP) { fprintf(stderr, "provided-buffer ring: %s\n", strerror(-rc)); return rc; }
        /* Some Ubuntu 6.8 updates reject valid PBUF_RING registrations. The
           older provided-buffer SQE interface still supports multishot recv. */
        w->legacy_buffers = true;
        fputs("provided-buffer ring unavailable; using legacy provided buffers\n", stderr);
        if (!provide_buffers(w, 0, BUFFER_COUNT)) return -ENOMEM;
    } else {
        for (unsigned i = 0; i < BUFFER_COUNT; i++)
            io_uring_buf_ring_add(w->br, w->buffers + (size_t)i * BUFFER_SIZE,
                                BUFFER_SIZE, (unsigned short)i, BUFFER_COUNT - 1, (int)i);
        io_uring_buf_ring_advance(w->br, BUFFER_COUNT);
        rc = io_uring_buf_ring_available(&w->ring, w->br, 0);
        if (rc < 0) return rc;
    }
    accept_connections(w);
    return 0;
}
static void worker_destroy(Worker *w) {
    bool quiesced = true;
    if (w->ready) {
        /* Stop outstanding reads/sends before releasing the memory they use.
           Closing a ring alone may defer kernel-side cancellation. */
        int rc = io_uring_submit(&w->ring);
        struct io_uring_sync_cancel_reg cancel = {
            .flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL,
            .timeout = {.tv_sec = 10, .tv_nsec = 0}
        };
        if (rc >= 0 || rc == -EINTR) {
            do { rc = io_uring_register_sync_cancel(&w->ring, &cancel); } while (rc == -EINTR);
        }
        quiesced = rc >= 0 || rc == -ENOENT;
        if (!quiesced) fprintf(stderr, "shutdown cancellation: %s\n", strerror(-rc));
        if (quiesced) io_uring_unregister_files(&w->ring);
    }
    if (w->br) io_uring_free_buf_ring(&w->ring, w->br, BUFFER_COUNT, 0);
    if (w->ready) io_uring_queue_exit(&w->ring);
    if (w->listener >= 0) close(w->listener);
    /* On a failed cancellation retain I/O memory until process exit, when the
       kernel tears down the address space along with outstanding requests. */
    if (quiesced && w->connections) {
        for (unsigned i = 0; i < w->slots; i++) { free(w->connections[i].rx); free(w->connections[i].tx); }
        free(w->connections);
    }
    if (quiesced && w->buffers && w->buffers != MAP_FAILED) munmap(w->buffers, (size_t)BUFFER_COUNT * BUFFER_SIZE);
}
static int create_listener(const struct sockaddr_in *address) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int yes = 1, receive_size = 8192, send_size = 16384;
    /* Each ring owns a reuse-port listener. Sharing one accept queue lets a
       full direct-file table accept and then reject an otherwise valid peer. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_size, sizeof(receive_size)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size))) {
        perror("listener socket options"); close(fd); return -1;
    }
    if (bind(fd, (const struct sockaddr *)address, sizeof(*address)) || listen(fd, SOMAXCONN)) {
        perror("bind/listen"); close(fd); return -1;
    }
    return fd;
}
int main(void) {
    for (unsigned i = 0; i < RINGS; i++) workers[i].listener = -1;
    signal(SIGPIPE, SIG_IGN); signal(SIGINT, signal_stop); signal(SIGTERM, signal_stop);
    const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
    const char *host = getenv("HOST"), *port = getenv("PORT");
    if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
    if (!host) host = "127.0.0.1";
    if (!port) port = "3000";
    char *end; long port_number = strtol(port, &end, 10);
    if (!*port || *end || port_number < 1 || port_number > 65535) { fprintf(stderr, "invalid PORT\n"); return 1; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port_number)};
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) { fprintf(stderr, "HOST must be an IPv4 address\n"); return 1; }
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) || limit.rlim_cur < 128) { fprintf(stderr, "insufficient file limit\n"); return 1; }
    unsigned slots = limit.rlim_cur > MAX_SLOTS ? MAX_SLOTS : (unsigned)limit.rlim_cur - 32;
    int epoll = -1, status = 1;
    if (api_init(path, secret)) goto cleanup;
    epoll = epoll_create1(EPOLL_CLOEXEC);
    if (epoll < 0) { perror("epoll_create1"); goto cleanup; }
    for (unsigned i = 0; i < RINGS; i++) {
        workers[i].listener = create_listener(&address);
        if (workers[i].listener < 0) goto cleanup;
        int rc = worker_init(&workers[i], slots);
        if (rc < 0) { fprintf(stderr, "io_uring ring %u initialization: %s (requires Linux 6.8+ with io_uring enabled)\n", i, strerror(-rc)); goto cleanup; }
        struct epoll_event event = {.events = EPOLLIN, .data.u32 = i};
        if (epoll_ctl(epoll, EPOLL_CTL_ADD, workers[i].ring.ring_fd, &event)) { perror("epoll_ctl"); goto cleanup; }
    }
    fprintf(stderr, "curvynes C server: %s:%ld, SQLite SQL, %u rings x %u direct slots\n", host, port_number, RINGS, slots);
    time_t last_report = time(NULL);
    uint64_t peaks[5] = {0}, previous_loop = monotonic_ns(), peak_loop = 0, report_duration = 0;
    unsigned peak_ready = 0;
    while (running) {
        uint64_t phase = monotonic_ns();
        uint64_t interval = phase - previous_loop; previous_loop = phase;
        if (interval > peak_loop) peak_loop = interval;
        bool progress = false;
        unsigned completed = 0;
        for (unsigned i = 0; i < RINGS; i++) {
            Worker *w = &workers[i];
            struct io_uring_cqe *events[CQ_BATCH];
            unsigned count = io_uring_peek_batch_cqe(&w->ring, events, CQ_BATCH);
            completed += count;
            if (count) progress = true;
            for (unsigned j = 0; j < count; j++) completion(w, events[j]);
            io_uring_cq_advance(&w->ring, count);
            accept_connections(w);
        }
        record_stage(peaks, 0, &phase);
        flush_batch();
        record_stage(peaks, 1, &phase);
        unsigned ready = 0;
        for (unsigned i = 0; i < RINGS; i++) {
            Worker *w = &workers[i];
            retry_receives(w);
            int rc = io_uring_submit(&w->ring);
            if (rc < 0 && rc != -EINTR) { fprintf(stderr, "submit: %s\n", strerror(-rc)); running = 0; }
            ready += io_uring_cq_ready(&w->ring);
        }
        if (ready) progress = true;
        if (ready > peak_ready) peak_ready = ready;
        record_stage(peaks, 2, &phase);
        api_checkpoint_maintenance();
        record_stage(peaks, 3, &phase);
        if (!progress && running) {
            struct epoll_event events[RINGS];
            if (epoll_wait(epoll, events, RINGS, 5) < 0 && errno != EINTR) { perror("epoll_wait"); running = 0; }
        } else if (running && completed >= 8 && ready < 64) {
            /* Coalesce small busy batches for at most one millisecond. Large
               ready batches and lightly loaded clients proceed immediately. */
            struct timespec coalesce = {0, 1000000}; nanosleep(&coalesce, NULL);
        }
        record_stage(peaks, 4, &phase);
        time_t now = time(NULL);
        if (now - last_report >= 10) {
            uint64_t report_start = monotonic_ns();
            fprintf(stderr, "requests=%llu errors=%llu connections=", (unsigned long long)requests, (unsigned long long)errors);
            for (unsigned i = 0; i < RINGS; i++) fprintf(stderr, "%s%u", i ? "," : "", workers[i].active);
            fputs(" retries_waiting=", stderr);
            for (unsigned i = 0; i < RINGS; i++) fprintf(stderr, "%s%u", i ? "," : "", workers[i].retry_count);
            fputc('\n', stderr); last_report = now;
            api_log_diagnostics();
            fprintf(stderr, "loop_peak_ms: completions=%.3f commit=%.3f submit=%.3f checkpoint=%.3f wait=%.3f ready=%u total=%.3f previous_report=%.3f\n",
                    (double)peaks[0] / 1e6, (double)peaks[1] / 1e6, (double)peaks[2] / 1e6,
                    (double)peaks[3] / 1e6, (double)peaks[4] / 1e6, peak_ready,
                    (double)peak_loop / 1e6, (double)report_duration / 1e6);
            memset(peaks, 0, sizeof(peaks)); peak_ready = 0;
            fprintf(stderr, "cq_ops: accept=%llu recv=%llu send=%llu close=%llu shutdown=%llu provide=%llu negative=%llu full=%llu recv_retry=%llu\n",
                    (unsigned long long)cq_operations[OP_ACCEPT], (unsigned long long)cq_operations[OP_RECV],
                    (unsigned long long)cq_operations[OP_SEND], (unsigned long long)cq_operations[OP_CLOSE],
                    (unsigned long long)cq_operations[OP_SHUTDOWN], (unsigned long long)cq_operations[OP_PROVIDE], (unsigned long long)cq_negative,
                    (unsigned long long)accept_full, (unsigned long long)recv_retries);
            memset(cq_operations, 0, sizeof(cq_operations)); accept_full = recv_retries = cq_negative = 0;
            peak_loop = 0; report_duration = monotonic_ns() - report_start;
        }
    }
    status = 0;
    fprintf(stderr, "requests=%llu server_errors=%llu\n", (unsigned long long)requests, (unsigned long long)errors);
cleanup:
    for (unsigned i = 0; i < RINGS; i++) worker_destroy(&workers[i]);
    api_shutdown(); if (epoll >= 0) close(epoll); return status;
}
