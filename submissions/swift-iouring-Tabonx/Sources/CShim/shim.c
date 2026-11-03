// Socket and epoll setup. Kept in C so Swift never has to know the layout of
// struct epoll_event (packed on x86_64) or the sockaddr dance.
#define _GNU_SOURCE
#include "shim.h"

#ifdef __linux__
#include <netdb.h>
#include <stdio.h>

int sv_listen(const char *host, int port, int backlog) {
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = {0}, *res = 0;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    // Accepted sockets inherit TCP_NODELAY from the listener on Linux.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, backlog) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

int sv_accept(int fd) { return accept4(fd, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC); }

int sv_epoll_create(void) { return epoll_create1(EPOLL_CLOEXEC); }

int sv_epoll_add(int ep, int fd, int writable) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLRDHUP | (writable ? EPOLLOUT : 0);
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

int sv_epoll_mod(int ep, int fd, int writable) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLRDHUP | (writable ? EPOLLOUT : 0);
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

// Waits for events and unpacks them into parallel arrays: fds[i] and flags[i]
// (bit 0 readable, bit 1 writable, bit 2 hang-up or error).
int sv_epoll_wait(int ep, int *fds, unsigned *flags, int max, int timeout_ms) {
    struct epoll_event evs[1024];
    if (max > 1024) max = 1024;
    int n = epoll_wait(ep, evs, max, timeout_ms);
    for (int i = 0; i < n; i++) {
        unsigned e = evs[i].events, f = 0;
        if (e & EPOLLIN) f |= 1;
        if (e & EPOLLOUT) f |= 2;
        if (e & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) f |= 4;
        fds[i] = evs[i].data.fd;
        flags[i] = f;
    }
    return n;
}

int sv_raise_nofile(void) {
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) != 0) return 1024;
    r.rlim_cur = r.rlim_max;
    setrlimit(RLIMIT_NOFILE, &r);
    getrlimit(RLIMIT_NOFILE, &r);
    return r.rlim_cur > 1048576 ? 1048576 : (int)r.rlim_cur;
}

void sv_ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }
#endif
