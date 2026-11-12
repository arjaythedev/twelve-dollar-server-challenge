/* One-thread io_uring HTTP/1.1 server. Direct descriptors live in each ring's file table, so a
 * ring can hold RLIMIT_NOFILE connections and several rings multiply that. One multishot receive
 * per connection into a provided-buffer ring; one send per connection; one SQLite transaction
 * (group commit) per event-loop pass. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "db.h"
#include "http.h"

#ifndef SQ_ENTRIES
#define SQ_ENTRIES 4096
#endif
#ifndef CQ_ENTRIES
#define CQ_ENTRIES 65536
#endif
#ifndef BUF_SIZE
#define BUF_SIZE 1024
#endif
#ifndef BUF_COUNT
#define BUF_COUNT 4096
#endif
#define LISTENERS 8
#define ACCEPT_DEPTH 512
#define MAX_RINGS 16
#define READS_PER_PASS 16384
#define ANSWERS_PER_PASS 2048
#define WAIT_MIN 256
#define WAIT_NS 1000000L

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_SHUTDOWN, OP_CLOSE, OP_READ };

struct conn {
    uint16_t gen;
    uint32_t sent;
    struct out out;      /* response bytes */
    struct out partial;  /* bytes of an incomplete request, or requests waiting behind a send */
    uint8_t open, closing, sending, recving, queued, eof, close_after, in_batch, held, ready;
};

struct pbuf {
    struct io_uring_buf_ring *ring;
    uint16_t tail;
    uint16_t count;
    uint8_t *mem;
};

struct ring {
    int fd;
    struct io_uring_sqe *sqes;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array, *sq_flags;
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe *cqes;
    struct pbuf pb;
    struct conn *conns;
    uint32_t sq_entries, cq_entries;
    uint32_t slots, open, depth, margin, reads;
    int accepting, full;
    uint32_t accepts[LISTENERS];
};

struct mark {
    uint8_t ri;
    uint32_t slot;
};

struct heldent {
    uint8_t ri;
    uint32_t slot;
    uint16_t gen;
};

struct rqent {
    uint8_t ri;
    uint32_t slot;
    uint16_t gen;
};

struct server {
    struct ring rings[MAX_RINGS];
    int nr;
    uint32_t slots;
    int listen_fd[LISTENERS];
    struct app app;
    struct mark *marks;
    uint32_t nmarks, capmarks;
    struct heldent *held;
    uint32_t nheld, capheld;
    struct rqent *rq;
    uint64_t rq_head, rq_tail, rq_cap;
    /* Connections with buffered requests waiting for an answer budget slot. */
    struct rqent *ready;
    uint64_t ready_head, ready_tail, ready_cap;
    uint32_t answers_left;
    int need_ring;
};

static struct server *S;

static inline uint64_t ud(unsigned op, unsigned ri, unsigned gen, unsigned slot) {
    return (uint64_t)op << 56 | (uint64_t)ri << 48 | (uint64_t)gen << 32 | slot;
}

/* ---- buffer pool (app.c) is used through out_reserve/out_release ------------------------ */

/* ---- provided-buffer ring ------------------------------------------------------------- */

static void pbuf_add(struct pbuf *p, uint16_t bid) {
    struct io_uring_buf *e = &p->ring->bufs[p->tail & (uint16_t)(p->count - 1)];
    e->addr = (uint64_t)(uintptr_t)(p->mem + (size_t)bid * BUF_SIZE);
    e->len = BUF_SIZE;
    e->bid = bid;
    p->tail++;
}

static void pbuf_publish(struct pbuf *p) {
    __atomic_store_n(&p->ring->tail, p->tail, __ATOMIC_RELEASE);
}

static int pbuf_init(struct ring *r) {
    uint16_t count = BUF_COUNT;
    for (;;) {
        struct io_uring_buf_reg reg;
        memset(&reg, 0, sizeof reg);
        size_t rbytes = (size_t)count * sizeof(struct io_uring_buf);
        struct io_uring_buf_ring *br =
            mmap(NULL, rbytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (br == MAP_FAILED) return -1;
        uint8_t *mem = aligned_alloc(4096, (size_t)count * BUF_SIZE);
        if (!mem) {
            munmap(br, rbytes);
            return -1;
        }
        reg.ring_addr = (uint64_t)(uintptr_t)br;
        reg.ring_entries = count;
        reg.bgid = 0;
        long rc = syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, &reg, 1);
        if (rc < 0 && errno == EINVAL) {
            /* Ubuntu 6.8.0-136+ inverts the reserved-word check (Launchpad #2162843). */
            reg.resv[0] = 1;
            rc = syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, &reg, 1);
        }
        if (rc == 0) {
            r->pb.ring = br;
            r->pb.tail = 0;
            r->pb.count = count;
            r->pb.mem = mem;
            for (uint16_t i = 0; i < count; i++) pbuf_add(&r->pb, i);
            pbuf_publish(&r->pb);
            return 0;
        }
        munmap(br, rbytes);
        free(mem);
        if (errno != ENOMEM && errno != EPERM) return -1;
        if (count == 256) return -1;
        count /= 2;
        if (count < 256) count = 256;
    }
}

/* ---- ring setup ----------------------------------------------------------------------- */

static uint32_t descriptor_slots(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    uint64_t s = rl.rlim_cur;
    if (s > (1u << 20)) s = 1u << 20;
#ifdef SLOT_CAP
    if (s > SLOT_CAP) s = SLOT_CAP;
#endif
    return (uint32_t)s;
}

static int ring_init(struct ring *r, uint32_t slots) {
    memset(r, 0, sizeof *r);
    struct io_uring_params p;
    /* The ring's CQ is one contiguous allocation and the file table is charged to
     * RLIMIT_MEMLOCK. On a fragmented or small-limit host either can come back ENOMEM: retry,
     * halve the CQ (it only bounds completions reaped per pass), and shrink the file table if
     * the table itself cannot be registered. */
    r->fd = -1;
    uint32_t chosen = slots;
    for (;;) {
        int ok = 0;
        for (unsigned cq = CQ_ENTRIES; cq >= 8192 && !ok; cq /= 2) {
            for (int attempt = 0; attempt < 20; attempt++) {
                memset(&p, 0, sizeof p);
                p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
                          IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
                p.cq_entries = cq;
                r->fd = (int)syscall(SYS_io_uring_setup, SQ_ENTRIES, &p);
                if (r->fd >= 0) break;
                if (errno != ENOMEM) {
                    perror("io_uring_setup");
                    return -1;
                }
                usleep(1000);
            }
            if (r->fd < 0) continue;
            struct io_uring_rsrc_register rr;
            memset(&rr, 0, sizeof rr);
            rr.nr = chosen;
            rr.flags = IORING_RSRC_REGISTER_SPARSE;
            if (syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_FILES2, &rr, sizeof rr) == 0) {
                ok = 1;
                break;
            }
            if (errno != ENOMEM && errno != EPERM) {
                perror("io_uring register files");
                return -1;
            }
            close(r->fd);
            r->fd = -1;
        }
        if (ok) break;
        uint32_t next = chosen > 65535 ? 65535 : chosen > 16384 ? 16384 : chosen > 4096 ? 4096 : 0;
        if (!next) {
            perror("io_uring register files");
            return -1;
        }
        fprintf(stderr, "file table %u refused; retrying %u\n", chosen, next);
        chosen = next;
    }
    slots = chosen;
    size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    void *sq_ring, *cq_ring;
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        size_t sz = sq_sz > cq_sz ? sq_sz : cq_sz;
        sq_ring = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd,
                       IORING_OFF_SQ_RING);
        if (sq_ring == MAP_FAILED) {
            perror("mmap sq/cq");
            return -1;
        }
        cq_ring = sq_ring;
    } else {
        sq_ring = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd,
                       IORING_OFF_SQ_RING);
        cq_ring = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd,
                       IORING_OFF_CQ_RING);
        if (sq_ring == MAP_FAILED || cq_ring == MAP_FAILED) {
            perror("mmap ring");
            return -1;
        }
    }
    r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        perror("mmap sqes");
        return -1;
    }
    r->sq_head = (unsigned *)((char *)sq_ring + p.sq_off.head);
    r->sq_tail = (unsigned *)((char *)sq_ring + p.sq_off.tail);
    r->sq_mask = (unsigned *)((char *)sq_ring + p.sq_off.ring_mask);
    r->sq_array = (unsigned *)((char *)sq_ring + p.sq_off.array);
    r->sq_flags = (unsigned *)((char *)sq_ring + p.sq_off.flags);
    r->cq_head = (unsigned *)((char *)cq_ring + p.cq_off.head);
    r->cq_tail = (unsigned *)((char *)cq_ring + p.cq_off.tail);
    r->cq_mask = (unsigned *)((char *)cq_ring + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)((char *)cq_ring + p.cq_off.cqes);
    r->sq_entries = p.sq_entries;
    r->cq_entries = p.cq_entries;
    r->slots = slots;
    r->conns = calloc(slots, sizeof(struct conn));
    if (!r->conns) return -1;
    if (pbuf_init(r) != 0) {
        perror("buffer ring");
        return -1;
    }
    r->depth = slots / 16;
    if (r->depth < LISTENERS) r->depth = LISTENERS;
    if (r->depth > ACCEPT_DEPTH) r->depth = ACCEPT_DEPTH;
    r->depth = r->depth / LISTENERS * LISTENERS;
    r->margin = slots / 8;
    if (r->margin < r->depth + 1) r->margin = r->depth + 1;
    if (r->margin > 2 * ACCEPT_DEPTH) r->margin = 2 * ACCEPT_DEPTH;
    if (r->margin > slots / 4) r->margin = slots / 4;
    return 0;
}

/* ---- submission / completion ---------------------------------------------------------- */

static int enter_ring(struct ring *r, unsigned min, int wait) {
    struct __kernel_timespec ts = {0, WAIT_NS};
    struct io_uring_getevents_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.ts = (uint64_t)(uintptr_t)&ts;
    unsigned flags = IORING_ENTER_GETEVENTS;
    void *ap = NULL;
    size_t asz = 0;
    if (wait) {
        flags |= IORING_ENTER_EXT_ARG;
        ap = &arg;
        asz = sizeof arg;
    }
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE);
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    long rc = syscall(SYS_io_uring_enter, r->fd, tail - head, min, flags, ap, asz);
    if (rc >= 0) return 0;
    /* ETIME: the wait timed out after the submissions were consumed. EINTR: same. */
    if (errno == EINTR || errno == EBUSY || errno == EAGAIN || errno == ETIME) return 0;
    return -1;
}

static struct io_uring_sqe *sqe_get(struct ring *r) {
    for (;;) {
        unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
        unsigned tail = *r->sq_tail;
        if (tail - head < r->sq_entries) {
            unsigned idx = tail & *r->sq_mask;
            struct io_uring_sqe *sqe = &r->sqes[idx];
            memset(sqe, 0, sizeof *sqe);
            r->sq_array[idx] = idx;
            __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
            return sqe;
        }
        if (enter_ring(r, 0, 0) != 0) return NULL;
    }
}

static void arm_recv(struct ring *r, uint32_t slot, uint16_t gen) {
    struct io_uring_sqe *sqe = sqe_get(r);
    if (!sqe) return;
    sqe->opcode = IORING_OP_RECV;
    sqe->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->fd = (int)slot;
    sqe->buf_group = 0;
    sqe->user_data = ud(OP_RECV, (unsigned)(r - S->rings), gen, slot);
    r->conns[slot].recving = 1;
}

static void arm_read(struct ring *r, uint32_t slot, uint16_t gen) {
    struct io_uring_sqe *sqe = sqe_get(r);
    if (!sqe) return;
    sqe->opcode = IORING_OP_RECV;
    sqe->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->fd = (int)slot;
    sqe->buf_group = 0;
    sqe->msg_flags = MSG_DONTWAIT;
    sqe->user_data = ud(OP_READ, (unsigned)(r - S->rings), gen, slot);
}

static void arm_send(struct ring *r, uint32_t slot, struct conn *c) {
    struct io_uring_sqe *sqe = sqe_get(r);
    if (!sqe) return;
    sqe->opcode = IORING_OP_SEND;
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->fd = (int)slot;
    sqe->addr = (uint64_t)(uintptr_t)(c->out.p + c->sent);
    sqe->len = (uint32_t)(c->out.len - c->sent);
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ud(OP_SEND, (unsigned)(r - S->rings), c->gen, slot);
}

static void arm_close(struct ring *r, uint32_t slot, struct conn *c) {
    c->closing = 1;
    unsigned ri = (unsigned)(r - S->rings);
    struct io_uring_sqe *s = sqe_get(r);
    if (s) {
        s->opcode = IORING_OP_SHUTDOWN;
        s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
        s->fd = (int)slot;
        s->len = SHUT_RDWR;
        s->user_data = ud(OP_SHUTDOWN, ri, c->gen, slot);
    }
    struct io_uring_sqe *cl = sqe_get(r);
    if (cl) {
        /* Closing a direct descriptor uses the file_index target (slot + 1), not
         * IOSQE_FIXED_FILE: IORING_OP_CLOSE resolves it to the ring's file table. */
        cl->opcode = IORING_OP_CLOSE;
        cl->flags = 0;
        cl->fd = 0;
        cl->file_index = slot + 1;
        cl->user_data = ud(OP_CLOSE, ri, c->gen, slot);
    }
}

static void fill_accepts(struct ring *r) {
    unsigned ri = (unsigned)(r - S->rings);
    for (int li = 0; li < LISTENERS; li++) {
        while (r->accepts[li] < r->depth / LISTENERS) {
            struct io_uring_sqe *sqe = sqe_get(r);
            if (!sqe) return;
            sqe->opcode = IORING_OP_ACCEPT;
            sqe->fd = S->listen_fd[li];
            sqe->accept_flags = 0;
            sqe->file_index = IORING_FILE_INDEX_ALLOC;
            sqe->user_data = ud(OP_ACCEPT, ri, 0, (unsigned)li);
            r->accepts[li]++;
        }
    }
}

static void arm_accept(struct ring *r) {
    r->accepting = 1;
    fill_accepts(r);
}

/* ---- queues --------------------------------------------------------------------------- */

static void mark(struct ring *r, uint32_t slot, struct conn *c) {
    if (c->in_batch) return;
    if (S->nmarks == S->capmarks) {
        /* Preallocated for every possible connection; never expected. */
        fprintf(stderr, "mark list full\n");
        return;
    }
    S->marks[S->nmarks].ri = (uint8_t)(r - S->rings);
    S->marks[S->nmarks].slot = slot;
    S->nmarks++;
    c->in_batch = 1;
}

static int rq_push(uint8_t ri, uint32_t slot, uint16_t gen) {
    if (S->rq_tail - S->rq_head == S->rq_cap) {
        fprintf(stderr, "read queue full\n");
        return 0;
    }
    S->rq[S->rq_tail & (S->rq_cap - 1)] = (struct rqent){ri, slot, gen};
    S->rq_tail++;
    return 1;
}

static void queue_read(struct ring *r, uint32_t slot, struct conn *c) {
    if (c->queued) return;
    if (rq_push((uint8_t)(r - S->rings), slot, c->gen)) c->queued = 1;
}

static void queue_ready(struct ring *r, uint32_t slot, struct conn *c) {
    if (c->ready) return;
    if (S->ready_tail - S->ready_head == S->ready_cap) {
        fprintf(stderr, "ready queue full\n");
        return;
    }
    S->ready[S->ready_tail & (S->ready_cap - 1)] = (struct rqent){(uint8_t)(r - S->rings), slot, c->gen};
    S->ready_tail++;
    c->ready = 1;
}

/* ---- request handling ----------------------------------------------------------------- */

static void finish(struct ring *r, uint32_t slot, struct conn *c) {
    /* EOF/error: no more bytes will be read. A complete request already deferred on the ready
     * queue is still answered before the close; end_batch/OP_SEND check c->ready. */
    c->eof = 1;
    c->close_after = 1;
    mark(r, slot, c);
}

static void received(struct server *s, struct ring *r, uint32_t slot, struct conn *c,
                     const char *data, size_t n);

static void hold_conn(struct server *s, struct ring *r, uint32_t slot, struct conn *c) {
    if (c->held) return;
    if (s->nheld == s->capheld) {
        fprintf(stderr, "held list full\n");
        return;
    }
    s->held[s->nheld++] = (struct heldent){(uint8_t)(r - s->rings), slot, c->gen};
    c->held = 1;
}

static void release_held(struct server *s) {
    uint32_t n = s->nheld;
    s->nheld = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct heldent e = s->held[i];
        struct ring *r = &s->rings[e.ri];
        struct conn *c = &r->conns[e.slot];
        if (c->gen != e.gen || !c->open || c->closing) continue;
        c->held = 0;
        received(s, r, e.slot, c, "", 0);
    }
}

/* Deferred request bytes. A complete request parsed while the pass's answer budget is
 * exhausted, or recv bytes arriving while a send is in flight, go here. Unlike a response
 * buffer this starts at 512 bytes, so 310k idle/deferred connections do not pin 2.5 GB. */
static int stash(struct conn *c, const char *data, size_t n) {
    if (n == 0) return 0;
    if (out_reserve_partial(&c->partial, n) != 0) return -1;
    memcpy(c->partial.p + c->partial.len, data, n);
    c->partial.len += n;
    return 0;
}

static void received(struct server *s, struct ring *r, uint32_t slot, struct conn *c,
                     const char *data, size_t n) {
    if (c->closing) return;
    if (n > 0 && c->close_after) return; /* EOF was seen: no new request bytes */
    /* A send in flight owns c->out; a deferred request or an exhausted answer budget means the
     * bytes wait in the connection's pending buffer and it is queued for a later pass. */
    if (c->sending || c->ready || s->answers_left == 0) {
        if (stash(c, data, n) != 0) {
            c->close_after = 1;
            mark(r, slot, c);
            return;
        }
        if (!c->sending) queue_ready(r, slot, c);
        return;
    }
    const char *buf;
    size_t len;
    if (c->partial.len) {
        if (stash(c, data, n) != 0) {
            c->close_after = 1;
            mark(r, slot, c);
            return;
        }
        buf = c->partial.p;
        len = c->partial.len;
    } else {
        buf = data;
        len = n;
    }
    size_t off = 0;
    int budget_hit = 0;
    while (off < len) {
        struct http_request req;
        size_t used = 0;
        enum http_result hr = http_parse(buf + off, len - off, &req, &used);
        if (hr == HTTP_PARTIAL) break;
        if (hr == HTTP_BAD) {
            c->close_after = 1;
            mark(r, slot, c);
            off = len;
            break;
        }
        if (db_writes_held() && req.method_len == 4 && memcmp(req.method, "POST", 4) == 0) {
            /* The checkpoint thread is copying the log's tail; this write and anything behind
             * it stay unread on the connection until the hold ends. Reads keep going. */
            hold_conn(s, r, slot, c);
            break;
        }
        if (s->answers_left == 0) {
            budget_hit = 1;
            break;
        }
        s->answers_left--;
        app_handle(&s->app, &req, &c->out);
        off += used;
    }
    if (off < len) {
        size_t rem = len - off;
        if (buf == c->partial.p) {
            memmove(c->partial.p, buf + off, rem);
            c->partial.len = rem;
        } else if (stash(c, buf + off, rem) != 0) {
            c->close_after = 1;
            mark(r, slot, c);
            return;
        }
    } else {
        c->partial.len = 0;
        if (c->partial.p) out_release(&c->partial);
    }
    if (budget_hit && c->partial.len) queue_ready(r, slot, c);
    if (c->out.len) mark(r, slot, c);
    else if (c->close_after && !c->sending && !c->held && !c->ready)
        mark(r, slot, c); /* EOF while this conn had a deferred request that turned out to be
                            incomplete: end_batch must still arm the close. */
}

/* Serve deferred connections while the pass still has answers left. A connection interrupted
 * mid-parse keeps its partial; a connection stopped between requests has the whole request in
 * the partial and is served here. */
static void run_ready(struct server *s) {
    while (s->answers_left > 0 && s->ready_head != s->ready_tail) {
        struct rqent e = s->ready[s->ready_head & (s->ready_cap - 1)];
        s->ready_head++;
        struct ring *r = &s->rings[e.ri];
        struct conn *c = &r->conns[e.slot];
        if (c->gen != e.gen || !c->open || c->closing || !c->ready) continue;
        if (c->held) {
            /* The held list owns this connection until the checkpoint releases it; its
             * received(..., "", 0) will process the buffered request and re-queue it if the
             * answer budget is gone. Leaving c->ready set here would make that call take the
             * "already queued" early exit with no queue entry left behind: a stuck connection. */
            c->ready = 0;
            continue;
        }
        c->ready = 0;
        received(s, r, e.slot, c, "", 0);
    }
}

/* ---- pass end: commit then send ------------------------------------------------------- */

static void end_batch(struct server *s) {
    int committed = db_commit() == 0;
    db_tick();
    for (uint32_t k = 0; k < s->nmarks; k++) {
        struct ring *r = &s->rings[s->marks[k].ri];
        uint32_t slot = s->marks[k].slot;
        struct conn *c = &r->conns[slot];
        c->in_batch = 0;
        if (!c->open || c->closing) {
            if (c->out.len) out_release(&c->out);
            continue;
        }
        if (!committed) {
            c->close_after = 1;
            if (c->sending) {
                /* The send in flight predates this batch; its bytes are committed data. The
                 * buffered request behind it belongs to the failed transaction: drop it and
                 * close once the send completes. Do not release c->out here. Clear the ready
                 * flag too: its queue entry has nothing left to parse, and the send completion
                 * can only arm the close when c->ready is clear. */
                out_release(&c->partial);
                c->ready = 0;
                continue;
            }
            if (c->out.len) {
                /* Only a request answered in this batch gets the 500. A connection marked only
                 * for EOF (or whose request is still deferred) must not receive a fabricated
                 * response for a request it never made in this transaction. */
                out_release(&c->out);
                app_500(&c->out);
            }
        }
        if (c->sending) continue;
        if (c->out.len) {
            c->sending = 1;
            c->sent = 0;
            arm_send(r, slot, c);
        } else if (c->close_after && !c->ready && !c->held) {
            arm_close(r, slot, c);
        }
    }
    s->nmarks = 0;
}

/* ---- completion handling -------------------------------------------------------------- */

static void handle_cqe(struct server *s, struct ring *r, struct io_uring_cqe *cqe) {
    uint64_t u = cqe->user_data;
    unsigned op = (unsigned)(u >> 56);
    uint16_t gen = (uint16_t)((u >> 32) & 0xffff);
    uint32_t slot = (uint32_t)u;
    int res = cqe->res;
    uint32_t flags = cqe->flags;
    switch (op) {
    case OP_ACCEPT: {
        if (slot < LISTENERS && r->accepts[slot]) r->accepts[slot]--;
        if (res >= 0) {
            if ((uint32_t)res >= r->slots) {
                /* Should not happen: the table is sized to the ring. */
                break;
            }
            struct conn *c = &r->conns[res];
            uint32_t g = c->gen + 1;
            memset(c, 0, sizeof *c);
            c->gen = g;
            c->open = 1;
            r->open++;
            arm_recv(r, (uint32_t)res, (uint16_t)g);
            if (r->open + r->margin >= r->slots) {
                r->full = 1;
                r->accepting = 0;
                s->need_ring = 1;
            }
        } else if (res == -ENFILE) {
            r->full = 1;
            r->accepting = 0;
            s->need_ring = 1;
        }
        break;
    }
    case OP_RECV: {
        struct conn *c = &r->conns[slot];
        if (c->gen != gen || !c->open || c->closing) {
            if (flags & IORING_CQE_F_BUFFER) pbuf_add(&r->pb, (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT));
            break;
        }
        if (res > 0 && (flags & IORING_CQE_F_BUFFER)) {
            uint16_t bid = (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT);
            const char *data = (const char *)(r->pb.mem + (size_t)bid * BUF_SIZE);
            received(s, r, slot, c, data, (size_t)res);
            pbuf_add(&r->pb, bid);
            if (!(flags & IORING_CQE_F_MORE)) {
                c->recving = 0;
                queue_read(r, slot, c);
            }
        } else if (res == -ENOBUFS) {
            c->recving = 0;
            queue_read(r, slot, c);
        } else {
            c->recving = 0;
            finish(r, slot, c);
        }
        break;
    }
    case OP_READ: {
        struct conn *c = &r->conns[slot];
        if (r->reads) r->reads--;
        if (c->gen != gen || !c->open || c->closing) {
            if (flags & IORING_CQE_F_BUFFER) pbuf_add(&r->pb, (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT));
            break;
        }
        c->recving = 0;
        if (res > 0 && (flags & IORING_CQE_F_BUFFER)) {
            uint16_t bid = (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT);
            const char *data = (const char *)(r->pb.mem + (size_t)bid * BUF_SIZE);
            received(s, r, slot, c, data, (size_t)res);
            pbuf_add(&r->pb, bid);
            if ((size_t)res == BUF_SIZE)
                queue_read(r, slot, c);
            else {
                arm_recv(r, slot, c->gen);
            }
        } else if (res == -EAGAIN) {
            arm_recv(r, slot, c->gen);
        } else if (res == -ENOBUFS) {
            queue_read(r, slot, c);
        } else {
            finish(r, slot, c);
        }
        break;
    }
    case OP_SEND: {
        struct conn *c = &r->conns[slot];
        if (c->gen != gen || !c->sending) break;
        if (res < 0) {
            c->sending = 0;
            out_release(&c->out);
            c->close_after = 1;
            mark(r, slot, c);
            break;
        }
        c->sent += (uint32_t)res;
        if (c->sent < c->out.len) {
            arm_send(r, slot, c);
        } else {
            c->sending = 0;
            out_release(&c->out);
            if (c->partial.len) received(s, r, slot, c, "", 0);
            if (c->out.len) {
                mark(r, slot, c);
            } else if (c->close_after && !c->ready && !c->held) {
                arm_close(r, slot, c);
            }
        }
        break;
    }
    case OP_SHUTDOWN:
        break;
    case OP_CLOSE: {
        struct conn *c = &r->conns[slot];
        if (c->gen == gen && c->open) {
            out_release(&c->out);
            out_release(&c->partial);
            uint32_t g = c->gen;
            memset(c, 0, sizeof *c);
            c->gen = g;
            if (r->open) r->open--;
            if (r->full && r->open + 2 * r->margin < r->slots) {
                r->full = 0;
                r->accepting = 1;
            }
        }
        break;
    }
    default:
        break;
    }
}

static unsigned drain(struct server *s, struct ring *r) {
    unsigned head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
    unsigned n = 0;
    while (head != tail) {
        struct io_uring_cqe *cqe = &r->cqes[head & *r->cq_mask];
        handle_cqe(s, r, cqe);
        head++;
        n++;
    }
    __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
    pbuf_publish(&r->pb);
    return n;
}

/* ---- recovery reads ------------------------------------------------------------------- */

static unsigned issue_reads(struct server *s, unsigned budget) {
    unsigned issued = 0;
    uint64_t scanned = 0, total = s->rq_tail - s->rq_head;
    while (issued < budget && s->rq_head != s->rq_tail && scanned < total) {
        struct rqent e = s->rq[s->rq_head & (s->rq_cap - 1)];
        struct ring *r = &s->rings[e.ri];
        struct conn *c = &r->conns[e.slot];
        scanned++;
        s->rq_head++;
        if (c->gen != e.gen || !c->open || c->closing) continue;
        if (c->recving) continue; /* already being read; a later completion re-queues it */
        if (r->reads >= r->pb.count) {
            /* No free receive buffer on this ring: try another, in order. */
            rq_push(e.ri, e.slot, e.gen);
            continue;
        }
        c->queued = 0;
        c->recving = 1;
        r->reads++;
        arm_read(r, e.slot, e.gen);
        issued++;
    }
    return issued;
}

static void read_rounds(struct server *s) {
    unsigned budget = READS_PER_PASS;
    while (budget && s->rq_head != s->rq_tail) {
        unsigned k = issue_reads(s, budget);
        if (!k) break;
        budget -= k;
        for (int i = 0; i < s->nr; i++) {
            enter_ring(&s->rings[i], 0, 0);
        }
        for (int i = 0; i < s->nr; i++) drain(s, &s->rings[i]);
    }
}

/* ---- ring spilling -------------------------------------------------------------------- */

static int ring_add(struct server *s) {
    if (s->nr >= MAX_RINGS) return -1;
    struct ring *r = &s->rings[s->nr];
    if (ring_init(r, s->slots) != 0) return -1;
    s->nr++;
    fprintf(stderr, "ring %d added (%u slots)\n", s->nr, r->slots);
    arm_accept(r);
    return 0;
}

static void spill(struct server *s) {
    for (int i = 0; i < s->nr; i++)
        if (s->rings[i].accepting) return;
    for (int i = 0; i < s->nr; i++) {
        struct ring *r = &s->rings[i];
        if (!r->full && r->open + r->margin < r->slots) {
            arm_accept(r);
            return;
        }
    }
    if (ring_add(s) != 0) fprintf(stderr, "connection limit reached (%d rings)\n", s->nr);
}

/* ---- listeners ------------------------------------------------------------------------ */

static int make_listener(uint32_t addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, "reno", 5);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = addr;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    if (listen(fd, 65535) < 0) return -1;
    return fd;
}

static int listen_all(const char *host, uint16_t port) {
    uint32_t addr = INADDR_ANY;
    if (host && *host && strcmp(host, "0.0.0.0") != 0) {
        struct in_addr in;
        if (inet_pton(AF_INET, host, &in) != 1) return -1;
        addr = in.s_addr;
    }
    for (int i = 0; i < LISTENERS; i++) {
        S->listen_fd[i] = make_listener(addr, port);
        if (S->listen_fd[i] < 0) {
            perror("listen");
            return -1;
        }
    }
    return 0;
}

/* ---- main loop ------------------------------------------------------------------------ */

static void run(struct server *s) {
    for (;;) {
        int busy = s->rq_head != s->rq_tail || s->ready_head != s->ready_tail;
        s->answers_left = ANSWERS_PER_PASS;
        for (int i = s->nr - 1; i >= 1; i--) enter_ring(&s->rings[i], 0, 0);
        enter_ring(&s->rings[0], busy ? 0u : WAIT_MIN, !busy);
        for (int i = 0; i < s->nr; i++) drain(s, &s->rings[i]);
        run_ready(s);
        read_rounds(s);
        run_ready(s);
        for (int i = 0; i < s->nr; i++)
            if (s->rings[i].accepting && !s->rings[i].full) fill_accepts(&s->rings[i]);
        if (s->nheld && !db_writes_held()) release_held(s);
        end_batch(s);
        if (s->need_ring) {
            s->need_ring = 0;
            spill(s);
        }
    }
}

/* Locks the executable's and libc's read-only code mappings in RAM (up to 2 MiB) once io_uring
 * has taken its locked memory. With no swap, reclaim near the memory wall takes file-backed pages
 * first; a major fault on the event loop would then read its own code back from disk. */
static void lock_code(void) {
    char exe[512];
    ssize_t en = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (en <= 0) return;
    exe[en] = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[1024];
    size_t locked = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char perms[8], path[768];
        path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %767s", &a, &b, perms, path) < 3) continue;
        if (strcmp(perms, "r--p") && strcmp(perms, "r-xp")) continue;
        if (!path[0] || (strcmp(path, exe) && !strstr(path, "libc.so") && !strstr(path, "libgcc_s")))
            continue;
        if (b <= a || locked + (b - a) > (2u << 20)) continue;
        if (mlock((const void *)a, b - a) == 0) locked += b - a;
    }
    fclose(f);
    if (locked) fprintf(stderr, "mlocked %zu bytes of read-only code\n", locked);
}

int main(void) {
    const char *path = getenv("SQLITE_PATH");
    if (!path) path = "feed.db";
    const char *secret = getenv("JWT_SECRET");
    if (!secret) secret = "";
    const char *host = getenv("HOST");
    if (!host) host = "127.0.0.1";
    const char *port_s = getenv("PORT");
    uint16_t port = port_s ? (uint16_t)atoi(port_s) : 3000;
    signal(SIGPIPE, SIG_IGN);

    S = calloc(1, sizeof *S);
    if (!S) return 1;
    /* Every connection can sit in the ready, read, held, and mark lists. Allocate the maxima
     * once: growing a queue in the middle of a 310k-connection burst is not an option. */
    S->rq_cap = (uint64_t)1 << 21;
    S->rq = calloc((size_t)S->rq_cap, sizeof *S->rq);
    S->ready_cap = (uint64_t)1 << 21;
    S->ready = calloc((size_t)S->ready_cap, sizeof *S->ready);
    S->capmarks = 1u << 20;
    S->marks = calloc(S->capmarks, sizeof *S->marks);
    S->capheld = 1u << 20;
    S->held = calloc(S->capheld, sizeof *S->held);
    if (!S->rq || !S->ready || !S->marks || !S->held) return 1;
    S->slots = descriptor_slots();
    char err[256];
    if (db_open(path, err, sizeof err) != 0) {
        fprintf(stderr, "database: %s\n", err);
        return 1;
    }
    app_init(&S->app, secret, strlen(secret));
    if (listen_all(host, port) != 0) return 1;
    if (ring_init(&S->rings[0], S->slots) != 0) return 1;
    S->slots = S->rings[0].slots;
    S->nr = 1;
    arm_accept(&S->rings[0]);
    lock_code();
    fprintf(stderr, "listening on %s:%u; io_uring, %u descriptors per ring\n", host, port, S->slots);
    run(S);
    return 0;
}
