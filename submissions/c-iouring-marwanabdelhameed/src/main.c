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

static conn *conn_new(int fd) {
    conn *c = calloc(1, sizeof *c);
    char *in = malloc(IN_INITIAL);
    if (!c || !in) { free(c); free(in); close(fd); return NULL; }
    c->fd = fd;
    c->in = in;
    c->in_cap = IN_INITIAL;
    return c;
}

static void conn_close(conn *c) {
    if (c->dead) return;
    c->dead = 1;
    close(c->fd);
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

static void out_reset(conn *c) {
    free(c->out);
    c->out = NULL;
    c->out_len = c->out_off = c->out_cap = 0;
}

/* Commits the batch's writes, releases the held responses and frees closed connections. */
static void end_batch(void) {
    while (app_has_pending()) {
        conn **rel;
        int n = app_commit(&rel);
        for (int i = 0; i < n; i++)
            if (!rel[i]->dead) kick(rel[i]);
    }
    for (size_t i = 0; i < ndead; i++) {
        free(dead[i]->in);
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
    if (listen(fd, 65535) < 0) die("listen");
    return fd;
}

/* ---------- io_uring backend ---------- */

static struct io_uring ring;
static int accept_armed, multishot = 1;

static struct io_uring_sqe *get_sqe(void) {
    struct io_uring_sqe *s;
    while (!(s = io_uring_get_sqe(&ring))) io_uring_submit(&ring);
    return s;
}

static void arm_accept(void) {
    struct io_uring_sqe *s = get_sqe();
    if (multishot) io_uring_prep_multishot_accept(s, lfd, NULL, NULL, SOCK_CLOEXEC);
    else io_uring_prep_accept(s, lfd, NULL, NULL, SOCK_CLOEXEC);
    io_uring_sqe_set_data64(s, UD_ACCEPT);
    accept_armed = 1;
}

static void post_recv(conn *c) {
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_recv(s, c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
    io_uring_sqe_set_data64(s, (uint64_t)(uintptr_t)c | OP_RECV);
}

static void post_send(conn *c) {
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_send(s, c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(s, (uint64_t)(uintptr_t)c | OP_SEND);
}

/* Decides the next operation for a connection with nothing in flight. */
static void uring_advance(conn *c) {
    if (c->dead || c->hold) return;
    if (c->out_len > c->out_off) { post_send(c); return; }
    if (c->closing) { conn_close(c); return; }
    post_recv(c);
}

static void uring_handle(uint64_t ud, int res, unsigned flags) {
    if (ud == UD_ACCEPT) {
        if (res >= 0) {
            conn *c = conn_new(res);
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
        if (res <= 0) { conn_close(c); return; }
        c->in_len += (uint32_t)res;
        app_process(c);
        uring_advance(c);
    } else {
        if (res < 0) { conn_close(c); return; }
        c->out_off += (uint32_t)res;
        if (c->out_off < c->out_len) { post_send(c); return; }
        out_reset(c);
        if (!c->closing && c->in_len > 0) app_process(c);
        uring_advance(c);
    }
}

static int run_uring(void) {
    int rc = io_uring_queue_init(RING_ENTRIES, &ring, 0);
    if (rc < 0) {
        fprintf(stderr, "io_uring unavailable (%s), using epoll\n", strerror(-rc));
        return -1;
    }
    fprintf(stderr, "backend: io_uring\n");
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
        }
        io_uring_cq_advance(&ring, n);
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
            }
        }
        end_batch();
    }
    return 0; /* not reached */
}

/* ---------- main ---------- */

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
