/* Only startup stream-socket creation is redirected into the task's private
 * Linux network namespaces. The application performs all TCP/HTTP operations. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int socket(int family, int type, int protocol) {
    static int (*original)(int, int, int);
    static _Atomic unsigned next_endpoint;
    if (!original)
        original = dlsym(RTLD_NEXT, "socket");
    if (!original) {
        errno = ENOSYS;
        return -1;
    }
    const char *root = getenv("TWELVE_TEST_SOCKET_ROOT"), *n = getenv("TWELVE_TEST_SOCKET_COUNT");
    if (!root || !n || family != AF_INET || (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != SOCK_STREAM ||
        (protocol && protocol != IPPROTO_TCP))
        return original(family, type, protocol);
    char *end;
    long count = strtol(n, &end, 10);
    if (*end || count < 1 || count > 8) {
        errno = EINVAL;
        return -1;
    }
    unsigned which =
        atomic_fetch_add_explicit(&next_endpoint, 1, memory_order_relaxed) % (unsigned)count;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/ep%u/new.sock", root, which) >=
        (int)sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int peer = original(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (peer < 0)
        return -1;
    unsigned char flags = !!(type & SOCK_NONBLOCK);
    if (connect(peer, (struct sockaddr *)&addr, sizeof(addr)) ||
        send(peer, &flags, 1, MSG_NOSIGNAL) != 1) {
        int err = errno;
        close(peer);
        errno = err;
        return -1;
    }
    unsigned char ack;
    struct iovec iov = {.iov_base = &ack, .iov_len = 1};
    union {
        char bytes[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } buf = {0};
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf.bytes, .msg_controllen = sizeof(buf.bytes)};
    ssize_t rc = recvmsg(peer, &msg, type & SOCK_CLOEXEC ? MSG_CMSG_CLOEXEC : 0);
    int err = errno;
    close(peer);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (rc != 1 || msg.msg_flags & MSG_CTRUNC || !cm || cm->cmsg_level != SOL_SOCKET ||
        cm->cmsg_type != SCM_RIGHTS || cm->cmsg_len != CMSG_LEN(sizeof(int))) {
        errno = rc < 0 ? err : EPROTO;
        return -1;
    }
    int fd;
    memcpy(&fd, CMSG_DATA(cm), sizeof(fd));
    return fd;
}
