// System headers and small wrappers for macros and errno.
#ifndef SV_SHIM_H
#define SV_SHIM_H
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif

static inline int sv_errno(void) { return errno; }

static inline int64_t sv_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static inline int64_t sv_monotonic_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec;
}

static inline int64_t sv_monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static inline const char *sv_getenv(const char *name) { return getenv(name); }

#ifdef __linux__
int sv_listen(const char *host, int port, int backlog);
int sv_accept(int fd);
int sv_epoll_create(void);
int sv_epoll_add(int ep, int fd, int writable);
int sv_epoll_mod(int ep, int fd, int writable);
int sv_epoll_wait(int ep, int *fds, unsigned *flags, int max, int timeout_ms);
int sv_raise_nofile(void);
void sv_ignore_sigpipe(void);
#endif
#endif
