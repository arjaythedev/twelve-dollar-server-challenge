/* Event loop. The primary backend is io_uring (multishot accept, recv, send, one operation in flight per
 * connection). If the kernel refuses io_uring_setup (for example io_uring_disabled=2), the same request code
 * runs on epoll. Both backends end every batch of events with one group commit. */
#define _GNU_SOURCE
#include "app.h"
#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define IN_INITIAL 2048
#define UD_ACCEPT 3ULL
#define OP_RECV 1ULL
#define OP_SEND 2ULL
#define RING_ENTRIES 4096
#define UD_CLOSE 7ULL /* completion of a close or shutdown posted for a registered descriptor: nothing to do */
#define FIXED_SLOTS 65536 /* V_FIXED_FILES: size of the registered file table */
#ifndef V_MS_BUFSZ
#define V_MS_BUFSZ 2048 /* V_MULTISHOT_RECV: bytes per provided buffer (a smaller value tests split requests) */
#endif
#define MS_NBUF 4096 /* V_MULTISHOT_RECV: provided buffers in the ring, a power of two */
#define MS_BGID 7
#define MS_MAX_IN 32768 /* V_MULTISHOT_RECV: most input one connection may buffer before it is closed */

#if defined(V_BUSYPOLL) && !defined(SO_BUSY_POLL)
#define SO_BUSY_POLL 46
#endif

static int lfd = -1;
static conn **dead;
static size_t ndead, dead_cap;
static void (*kick)(conn *);
static unsigned long closed_in_batch;

static void die(const char *msg) {
    fprintf(stderr, "fatal: %s: %s\n", msg, strerror(errno));
    exit(1);
}

/* ---------- shared connection handling ---------- */

static int ms_active;    /* V_MULTISHOT_RECV is running: an idle connection owns no input buffer */
static int fixed_active __attribute__((unused)); /* V_FIXED_FILES is running: c->fd is a slot of the registered file table */

static void fd_close_raw(int fd); /* closes a plain descriptor, or posts the close of a registered slot */
static void fd_shutdown(conn *c) __attribute__((unused));

static conn *conn_new(int fd) {
    conn *c = calloc(1, sizeof *c);
    if (!c) { fd_close_raw(fd); return NULL; }
    c->fd = fd;
#ifdef V_MULTISHOT_RECV
    if (ms_active) return c;
#endif
#ifdef CONN_INLINE_IN
    c->in = c->in0;
    c->in_cap = CONN_INLINE_IN;
    return c;
#else
    char *in = malloc(IN_INITIAL);
    if (!in) { free(c); fd_close_raw(fd); return NULL; }
    c->in = in;
    c->in_cap = IN_INITIAL;
    return c;
#endif
}

/* Releases the descriptor and queues the struct to be freed at the end of the batch. */
static void conn_finish(conn *c) {
    fd_close_raw(c->fd);
    c->fd = -1;
    closed_in_batch++;
    if (ndead == dead_cap) {
        size_t cap = dead_cap ? dead_cap * 2 : 256;
        conn **p = realloc(dead, cap * sizeof *p);
        if (!p) die("out of memory");
        dead = p;
        dead_cap = cap;
    }
    dead[ndead++] = c;
}

/* With a multishot recv or a send in flight the kernel still holds the connection, so the descriptor is released
 * only when the last completion has arrived (conn_settle). A shutdown makes the armed recv end at once. */
static void conn_close(conn *c) {
    if (c->dead) return;
    c->dead = 1;
#ifdef V_MULTISHOT_RECV
    if (ms_active && (c->recv_armed || c->sending)) {
        if (c->recv_armed) fd_shutdown(c);
        return;
    }
#endif
    conn_finish(c);
}

#ifdef V_MULTISHOT_RECV
static void conn_settle(conn *c) {
    if (ms_active && c->dead && c->fd != -1 && !c->recv_armed && !c->sending) conn_finish(c);
}
#endif

static void out_reset(conn *c) {
    free(c->out);
    c->out = NULL;
    c->out_len = c->out_off = c->out_cap = 0;
}

/* Commits the queued writes and releases the held responses. */
static void commit_pending(void) {
    while (app_has_pending()) {
        conn **rel;
        int n = app_commit(&rel);
        for (int i = 0; i < n; i++)
            if (!rel[i]->dead) kick(rel[i]);
    }
}

/* V_GROUP_COMMIT_N: commit as soon as N writes are queued, instead of once per batch. */
#ifdef V_GROUP_COMMIT_N
#define COMMIT_IF_FULL() do { if (app_pending_count() >= (size_t)(V_GROUP_COMMIT_N + 0)) commit_pending(); } while (0)
#else
#define COMMIT_IF_FULL() ((void)0)
#endif

/* Commits the batch's writes, releases the held responses and frees closed connections. */
static void end_batch(void) {
    commit_pending();
    for (size_t i = 0; i < ndead; i++) {
#ifdef CONN_INLINE_IN
        if (dead[i]->in != dead[i]->in0) free(dead[i]->in);
#else
        free(dead[i]->in);
#endif
        free(dead[i]->out);
        free(dead[i]);
    }
    ndead = 0;
}

static int make_listener(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) die("socket");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* accepted sockets inherit it */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (strcmp(host, "localhost") == 0) host = "127.0.0.1";
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) die("HOST is not an IPv4 address");
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) die("bind");
#ifdef V_BUSYPOLL /* microseconds of busy polling per socket; accepted sockets copy the listener's value */
    {
        int us = (V_BUSYPOLL + 0) > 1 ? (V_BUSYPOLL + 0) : 50;
        if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof us) < 0)
            fprintf(stderr, "V_BUSYPOLL: SO_BUSY_POLL=%d failed: %s\n", us, strerror(errno));
        else
            fprintf(stderr, "V_BUSYPOLL: SO_BUSY_POLL=%d us set on the listener\n", us);
    }
#endif
#ifdef V_DEFER_ACCEPT /* accept a connection only once its first bytes have arrived (value in seconds) */
    {
        int secs = (V_DEFER_ACCEPT + 0) > 1 ? (V_DEFER_ACCEPT + 0) : 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &secs, sizeof secs) < 0)
            fprintf(stderr, "V_DEFER_ACCEPT: TCP_DEFER_ACCEPT=%d failed: %s\n", secs, strerror(errno));
        else
            fprintf(stderr, "V_DEFER_ACCEPT: TCP_DEFER_ACCEPT=%d s set on the listener\n", secs);
    }
#endif
    if (listen(fd, 65535) < 0) die("listen");
    return fd;
}

/* ---------- io_uring backend ---------- */

static struct io_uring ring;
static int accept_armed, multishot = 1;

#ifdef V_MULTISHOT_RECV
static struct io_uring_buf_ring *br; /* provided buffers for the multishot recv */
static char *ms_bufs;
static conn **rearm;
static size_t nrearm, rearm_cap;
#endif

static struct io_uring_sqe *get_sqe(void) {
    struct io_uring_sqe *s;
    while (!(s = io_uring_get_sqe(&ring))) io_uring_submit(&ring);
    return s;
}

/* V_FIXED_FILES: an operation on a registered slot carries IOSQE_FIXED_FILE. */
static void sqe_fixed(struct io_uring_sqe *s) {
#ifdef V_FIXED_FILES
    if (fixed_active) s->flags |= IOSQE_FIXED_FILE;
#endif
    (void)s;
}

static void fd_close_raw(int fd) {
#ifdef V_FIXED_FILES
    if (fixed_active) {
        struct io_uring_sqe *s = get_sqe();
        io_uring_prep_close_direct(s, (unsigned)fd);
        io_uring_sqe_set_data64(s, UD_CLOSE);
        return;
    }
#endif
    close(fd);
}

static void fd_shutdown(conn *c) {
#ifdef V_FIXED_FILES
    if (fixed_active) {
        struct io_uring_sqe *s = get_sqe();
        io_uring_prep_shutdown(s, c->fd, SHUT_RDWR);
        sqe_fixed(s);
        io_uring_sqe_set_data64(s, UD_CLOSE);
        return;
    }
#endif
    shutdown(c->fd, SHUT_RDWR);
}

static void arm_accept(void) {
    struct io_uring_sqe *s = get_sqe();
#ifdef V_FIXED_FILES /* accepted sockets go straight into a free slot of the registered table */
    if (fixed_active) {
        /* no SOCK_CLOEXEC here: the kernel rejects it for a direct descriptor, which has no close-on-exec bit */
        if (multishot) io_uring_prep_multishot_accept_direct(s, lfd, NULL, NULL, 0);
        else io_uring_prep_accept_direct(s, lfd, NULL, NULL, 0, IORING_FILE_INDEX_ALLOC);
    } else
#endif
    if (multishot) io_uring_prep_multishot_accept(s, lfd, NULL, NULL, SOCK_CLOEXEC);
    else io_uring_prep_accept(s, lfd, NULL, NULL, SOCK_CLOEXEC);
    io_uring_sqe_set_data64(s, UD_ACCEPT);
    accept_armed = 1;
}

static void post_recv(conn *c) {
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_recv(s, c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
    sqe_fixed(s);
    io_uring_sqe_set_data64(s, (uint64_t)(uintptr_t)c | OP_RECV);
}

#ifdef V_MULTISHOT_RECV
/* One recv stays armed for the life of the connection. Each completion names a buffer of the provided ring. */
static void arm_recv_ms(conn *c) {
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_recv_multishot(s, c->fd, NULL, 0, 0);
    s->flags |= IOSQE_BUFFER_SELECT;
    s->buf_group = MS_BGID;
    sqe_fixed(s);
    io_uring_sqe_set_data64(s, (uint64_t)(uintptr_t)c | OP_RECV);
    c->recv_armed = 1;
}

static void ms_return(unsigned bid) {
    io_uring_buf_ring_add(br, ms_bufs + (size_t)bid * V_MS_BUFSZ, V_MS_BUFSZ, (unsigned short)bid,
                          io_uring_buf_ring_mask(MS_NBUF), 0);
    io_uring_buf_ring_advance(br, 1);
}

/* After app_process: bytes still unparsed are copied out of the provided buffer (which goes back to the ring),
 * and a connection with nothing buffered gives its input buffer back. */
static void ms_detach(conn *c) {
    if (c->in_ext) {
        c->in_ext = 0;
        if (c->in_len == 0) {
            c->in = NULL;
            c->in_cap = 0;
            return;
        }
        size_t cap = c->in_len < 512 ? 512 : c->in_len;
        char *p = malloc(cap);
        if (!p) die("out of memory");
        memcpy(p, c->in, c->in_len);
        c->in = p;
        c->in_cap = (uint32_t)cap;
    } else if (c->in && c->in_len == 0) {
        free(c->in);
        c->in = NULL;
        c->in_cap = 0;
    }
}

static void uring_advance(conn *c);

static void ms_feed(conn *c, char *data, unsigned len) {
    if (c->in == NULL) { /* idle: parse in place in the provided buffer */
        c->in = data;
        c->in_cap = c->in_len = len;
        c->in_ext = 1;
    } else {
        if ((size_t)c->in_len + len > MS_MAX_IN) { conn_close(c); return; }
        if (c->in_len + len > c->in_cap) {
            size_t cap = (size_t)c->in_cap * 2;
            if (cap < (size_t)c->in_len + len) cap = (size_t)c->in_len + len;
            char *p = realloc(c->in, cap);
            if (!p) die("out of memory");
            c->in = p;
            c->in_cap = (uint32_t)cap;
        }
        memcpy(c->in + c->in_len, data, len);
        c->in_len += len;
    }
    int idle = !c->hold && !c->sending; /* a held or sending connection keeps the bytes until its turn */
    if (idle) app_process(c);
    ms_detach(c);
    if (idle) uring_advance(c);
}
#endif

#ifdef V_ZC_SEND
/* io_uring zero-copy send posts two completions: the result (IORING_CQE_F_MORE set) and later a notification
 * (IORING_CQE_F_NOTIF) once the kernel no longer reads the buffer. c->out is kept until the notification. */
static int zc_ok = 1;
#endif

static void post_send_mode(conn *c, int zc) {
    struct io_uring_sqe *s = get_sqe();
#ifdef V_ZC_SEND
    if (zc && zc_ok)
        io_uring_prep_send_zc(s, c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL, 0);
    else
#endif
        io_uring_prep_send(s, c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
    (void)zc;
    sqe_fixed(s);
    c->sending = 1;
    io_uring_sqe_set_data64(s, (uint64_t)(uintptr_t)c | OP_SEND);
}

static void post_send(conn *c) { post_send_mode(c, 1); }

/* Decides the next operation for a connection with nothing in flight (with V_MULTISHOT_RECV the recv stays armed,
 * so only a send is ever posted here). */
static void uring_advance(conn *c) {
    if (c->dead || c->hold) return;
#ifdef V_MULTISHOT_RECV
    if (ms_active) {
        if (c->sending) return;
        if (c->out_len > c->out_off) { post_send(c); return; }
        if (c->closing) { conn_close(c); return; }
        if (!c->recv_armed) arm_recv_ms(c);
        return;
    }
#endif
    if (c->out_len > c->out_off) { post_send(c); return; }
    if (c->closing) { conn_close(c); return; }
    post_recv(c);
}

#ifdef V_MULTISHOT_RECV
static void ms_recv_done(conn *c, int res, unsigned flags) {
    char *data = NULL;
    unsigned bid = 0;
    if (flags & IORING_CQE_F_BUFFER) {
        bid = flags >> IORING_CQE_BUFFER_SHIFT;
        data = ms_bufs + (size_t)bid * V_MS_BUFSZ;
    }
    if (!(flags & IORING_CQE_F_MORE)) c->recv_armed = 0;
    if (res > 0 && data && !c->dead) ms_feed(c, data, (unsigned)res);
    if (data) ms_return(bid);
    if (res <= 0 && res != -ENOBUFS) conn_close(c); /* end of stream or an error */
    else if (!c->recv_armed && !c->dead) { /* the multishot ended (no buffer was free): arm it again after the batch */
        if (nrearm == rearm_cap) {
            rearm_cap = rearm_cap ? rearm_cap * 2 : 256;
            rearm = realloc(rearm, rearm_cap * sizeof *rearm);
            if (!rearm) die("out of memory");
        }
        rearm[nrearm++] = c;
    }
    conn_settle(c);
}

/* Arms the connections whose recv ended for lack of a buffer, once the batch has given its buffers back. Arming
 * earlier would find the ring empty again and complete with ENOBUFS at once. */
static void ms_rearm_flush(void) {
    for (size_t i = 0; i < nrearm; i++)
        if (!rearm[i]->dead && !rearm[i]->recv_armed) arm_recv_ms(rearm[i]);
    nrearm = 0;
}
#endif

static void uring_handle(uint64_t ud, int res, unsigned flags) {
    if (ud == UD_CLOSE) return;
    if (ud == UD_ACCEPT) {
        if (res >= 0) {
#ifdef V_BUSYPOLL
            static int checked;
            if (!checked) { /* log once what an accepted socket inherited */
                int v = -1;
                socklen_t l = sizeof v;
                getsockopt(res, SOL_SOCKET, SO_BUSY_POLL, &v, &l);
                fprintf(stderr, "V_BUSYPOLL: first accepted socket has SO_BUSY_POLL=%d\n", v);
                checked = 1;
            }
#endif
            conn *c = conn_new(res);
#ifdef V_MULTISHOT_RECV
            if (c && ms_active) arm_recv_ms(c);
            else
#endif
            if (c) post_recv(c);
        } else if (res == -EINVAL && multishot) {
            multishot = 0;
        } else if (res != -EAGAIN && res != -EINTR && res != -ECONNABORTED) {
            fprintf(stderr, "accept: %s\n", strerror(-res));
        }
        if (!(flags & IORING_CQE_F_MORE)) {
            accept_armed = 0;
            if (res >= 0 || !(res == -EMFILE || res == -ENFILE || res == -ENOMEM)) arm_accept();
        }
        return;
    }
    conn *c = (conn *)(uintptr_t)(ud & ~3ULL);
    uint64_t op = ud & 3ULL;
    if (op == OP_RECV) {
#ifdef V_MULTISHOT_RECV
        if (ms_active) { ms_recv_done(c, res, flags); return; }
#endif
        if (res <= 0) { conn_close(c); return; }
        c->in_len += (uint32_t)res;
        app_process(c);
        uring_advance(c);
    } else {
#ifdef V_ZC_SEND
        if (flags & IORING_CQE_F_NOTIF) {
            res = c->zc_res; /* buffer released, now finish the send the first completion reported */
        } else if (flags & IORING_CQE_F_MORE) {
            c->zc_res = res; /* a notification follows; keep c->out until it arrives */
            return;
        } else if (zc_ok && (res == -EINVAL || res == -EOPNOTSUPP)) {
            fprintf(stderr, "V_ZC_SEND: kernel refused zero-copy send (%s), using plain send\n", strerror(-res));
            zc_ok = 0;
            post_send_mode(c, 0);
            return;
        } else if (zc_ok && res == -ENOMEM) {
            post_send_mode(c, 0); /* pinned-page limit: this send copies */
            return;
        }
#endif
        c->sending = 0;
#ifdef V_MULTISHOT_RECV
        if (ms_active && c->dead) { conn_settle(c); return; } /* the peer went away while this send was in flight */
#endif
        if (res < 0) { conn_close(c); return; }
        c->out_off += (uint32_t)res;
        if (c->out_off < c->out_len) { post_send(c); return; }
        out_reset(c);
        if (!c->closing && c->in_len > 0) app_process(c);
#ifdef V_MULTISHOT_RECV
        if (ms_active) ms_detach(c);
#endif
        uring_advance(c);
    }
}

static int run_uring(void) {
#ifdef V_DEFER_TASKRUN
    /* Task work (the completions of recv and send) runs when the loop enters the kernel in submit_and_wait, not at
     * any syscall boundary or interrupt. The loop only ever waits inside io_uring_submit_and_wait, so it always
     * enters. Falls back to cooperative task work, then to the default setup, when the kernel refuses a flag. */
    static const unsigned setup_tries[] = {IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
                                           IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG, 0};
    int rc = -EINVAL;
    for (unsigned i = 0; i < sizeof setup_tries / sizeof *setup_tries && rc == -EINVAL; i++) {
        struct io_uring_params prm;
        memset(&prm, 0, sizeof prm);
        prm.flags = setup_tries[i];
        rc = io_uring_queue_init_params(RING_ENTRIES, &ring, &prm);
        if (rc == 0) fprintf(stderr, "V_DEFER_TASKRUN: ring flags 0x%x (%s)\n", prm.flags,
                             i == 0 ? "single issuer, deferred task run" : i == 1 ? "cooperative task run" : "default");
    }
#else
    int rc = io_uring_queue_init(RING_ENTRIES, &ring, 0);
#endif
    if (rc < 0) {
        fprintf(stderr, "io_uring unavailable (%s), using epoll\n", strerror(-rc));
        return -1;
    }
    fprintf(stderr, "backend: io_uring\n");
#ifdef V_FIXED_FILES
    rc = io_uring_register_files_sparse(&ring, FIXED_SLOTS);
    if (rc < 0) {
        fprintf(stderr, "V_FIXED_FILES: register_files_sparse failed (%s), using plain descriptors\n", strerror(-rc));
    } else {
        fixed_active = 1;
        fprintf(stderr, "V_FIXED_FILES: %d registered slots, accept direct\n", FIXED_SLOTS);
    }
#endif
#ifdef V_MULTISHOT_RECV
    {
        int ber = 0;
        br = io_uring_setup_buf_ring(&ring, MS_NBUF, MS_BGID, 0, &ber);
        ms_bufs = br ? malloc((size_t)MS_NBUF * V_MS_BUFSZ) : NULL;
        if (br && ms_bufs) {
            for (unsigned i = 0; i < MS_NBUF; i++)
                io_uring_buf_ring_add(br, ms_bufs + (size_t)i * V_MS_BUFSZ, V_MS_BUFSZ, (unsigned short)i,
                                      io_uring_buf_ring_mask(MS_NBUF), (int)i);
            io_uring_buf_ring_advance(br, MS_NBUF);
            ms_active = 1;
            fprintf(stderr, "V_MULTISHOT_RECV: %d provided buffers of %d bytes\n", MS_NBUF, V_MS_BUFSZ);
        } else {
            fprintf(stderr, "V_MULTISHOT_RECV: provided buffer ring failed (%s), one recv per request\n", strerror(-ber));
        }
    }
#endif
    kick = uring_advance;
    arm_accept();
    for (;;) {
        if (!accept_armed && closed_in_batch) arm_accept();
        closed_in_batch = 0;
        rc = io_uring_submit_and_wait(&ring, 1);
        if (rc < 0 && rc != -EINTR && rc != -EBUSY) { errno = -rc; die("io_uring_submit_and_wait"); }
        struct io_uring_cqe *cqe;
        unsigned head, n = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            uring_handle(cqe->user_data, cqe->res, cqe->flags);
            n++;
            COMMIT_IF_FULL();
        }
        io_uring_cq_advance(&ring, n);
#ifdef V_MULTISHOT_RECV
        ms_rearm_flush();
#endif
#ifdef V_READS_FIRST /* put the sends of this batch's read responses on the wire before the commit's disk write */
        if (app_has_pending()) io_uring_submit(&ring);
#endif
        end_batch();
    }
}

/* ---------- epoll backend ---------- */

static int ep;

static void ep_mod(conn *c, uint32_t events) {
    struct epoll_event e = {.events = events, .data.ptr = c};
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e);
}

static void epoll_advance(conn *c) {
    for (;;) {
        if (c->dead || c->hold) return;
        if (c->out_len > c->out_off) {
            ssize_t w = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!c->sending) { c->sending = 1; ep_mod(c, EPOLLIN | EPOLLOUT); }
                    return;
                }
                if (errno == EINTR) continue;
                conn_close(c);
                return;
            }
            c->out_off += (uint32_t)w;
            if (c->out_off < c->out_len) continue;
            out_reset(c);
            if (c->sending) { c->sending = 0; ep_mod(c, EPOLLIN); }
        }
        if (c->closing) { conn_close(c); return; }
        if (c->in_len == 0) return;
        uint32_t before = c->in_len;
        app_process(c);
        if (c->out_len == 0 && !c->hold && c->in_len == before) return;
    }
}

static int run_epoll(void) {
    fprintf(stderr, "backend: epoll\n");
    kick = epoll_advance;
    ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) die("epoll_create1");
    struct epoll_event le = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &le) < 0) die("epoll_ctl");
    static struct epoll_event evs[512];
    for (;;) {
        int n = epoll_wait(ep, evs, 512, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait");
        }
        for (int i = 0; i < n; i++) {
            conn *c = evs[i].data.ptr;
            if (!c) {
                for (;;) {
                    int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (fd < 0) {
                        if (errno == EMFILE || errno == ENFILE) usleep(1000);
                        break;
                    }
                    conn *nc = conn_new(fd);
                    if (!nc) continue;
                    struct epoll_event e = {.events = EPOLLIN, .data.ptr = nc};
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e) < 0) conn_close(nc);
                }
                continue;
            }
            if (c->dead) continue;
            uint32_t ev = evs[i].events;
            if (c->sending) {
                if (ev & (EPOLLOUT | EPOLLERR | EPOLLHUP)) epoll_advance(c);
                continue;
            }
            if (c->hold) continue;
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                ssize_t r = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
                if (r == 0) { conn_close(c); continue; }
                if (r < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) conn_close(c);
                    continue;
                }
                c->in_len += (uint32_t)r;
                app_process(c);
                epoll_advance(c);
                COMMIT_IF_FULL();
            }
        }
        end_batch();
    }
    return 0; /* not reached */
}

/* ---------- main ---------- */

#ifdef V_PGO_GEN
extern void __gcov_dump(void);
static void pgo_term(int sig) { (void)sig; __gcov_dump(); _exit(0); }
#endif
/* Kept out of line and present in every build, so main() has the same control flow with and without V_PGO_GEN and
 * its profile still matches. */
__attribute__((noinline)) static void pgo_install(void) {
#ifdef V_PGO_GEN
    signal(SIGTERM, pgo_term); /* a profile run ends with SIGTERM, which writes the .gcda files */
#endif
}

int main(void) {
    const char *db = getenv("SQLITE_PATH");
    const char *secret = getenv("JWT_SECRET");
    const char *host = getenv("HOST");
    const char *port = getenv("PORT");
    if (!db || !secret) {
        fprintf(stderr, "SQLITE_PATH and JWT_SECRET must be set\n");
        return 1;
    }
    if (!host || !*host) host = "0.0.0.0";
    int p = port && *port ? atoi(port) : 80;

    signal(SIGPIPE, SIG_IGN);
    pgo_install();
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    app_init(db, secret);
    lfd = make_listener(host, p);
    fprintf(stderr, "listening on %s:%d\n", host, p);
    if (run_uring() < 0) run_epoll();
    return 1;
}
