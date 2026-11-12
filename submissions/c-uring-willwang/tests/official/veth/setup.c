/* Create one veth entirely between the two explicitly cooperating private
 * network namespaces. This short-lived process exits before the benchmark. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static void die(const char *name) {
    perror(name);
    exit(1);
}
struct request {
    struct nlmsghdr h;
    unsigned char data[2048];
};
static struct rtattr *attr(struct request *r, unsigned type, const void *val, size_t len) {
    size_t off = NLMSG_ALIGN(r->h.nlmsg_len), need = RTA_LENGTH(len);
    if (off + RTA_ALIGN(need) > sizeof(*r)) {
        errno = EMSGSIZE;
        die("netlink attribute");
    }
    struct rtattr *a = (struct rtattr *)((char *)r + off);
    a->rta_type = type;
    a->rta_len = need;
    if (len)
        memcpy(RTA_DATA(a), val, len);
    r->h.nlmsg_len = off + RTA_ALIGN(need);
    return a;
}
static void end(struct request *r, struct rtattr *a) {
    a->rta_len = (char *)r + r->h.nlmsg_len - (char *)a;
}
static void send_request(struct request *r) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        die("private rtnetlink");
    struct sockaddr_nl addr = {.nl_family = AF_NETLINK};
    r->h.nlmsg_seq = 1;
    if (sendto(fd, r, r->h.nlmsg_len, 0, (struct sockaddr *)&addr, sizeof(addr)) !=
        (ssize_t)r->h.nlmsg_len)
        die("private rtnetlink send");
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)))
        die("private rtnetlink receive");
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    if (h->nlmsg_type != NLMSG_ERROR) {
        errno = EPROTO;
        die("private rtnetlink reply");
    }
    int error = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
    if (error) {
        errno = -error;
        die("private rtnetlink operation");
    }
    close(fd);
}
static unsigned long ns_inode(int fd) {
    struct stat st;
    struct statfs fs;
    if (fstat(fd, &st) || fstatfs(fd, &fs) || fs.f_type != NSFS_MAGIC ||
        ioctl(fd, NS_GET_NSTYPE) != CLONE_NEWNET) {
        errno = EINVAL;
        die("private network namespace descriptor");
    }
    return st.st_ino;
}
static void make_veth(int peer_fd, const char *local_name) {
    struct request r = {.h = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                              .nlmsg_type = RTM_NEWLINK,
                              .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL}};
    struct ifinfomsg *m = NLMSG_DATA(&r.h);
    m->ifi_family = AF_UNSPEC;
    const char name[] = "bench0";
    attr(&r, IFLA_IFNAME, local_name, strlen(local_name) + 1);
    struct rtattr *info = attr(&r, IFLA_LINKINFO, NULL, 0);
    attr(&r, IFLA_INFO_KIND, "veth", 5);
    struct rtattr *data = attr(&r, IFLA_INFO_DATA, NULL, 0);
    struct ifinfomsg empty = {.ifi_family = AF_UNSPEC};
    struct rtattr *peer = attr(&r, VETH_INFO_PEER, &empty, sizeof(empty));
    attr(&r, IFLA_IFNAME, name, sizeof(name));
    attr(&r, IFLA_NET_NS_FD, &peer_fd, sizeof(peer_fd));
    end(&r, peer);
    end(&r, data);
    end(&r, info);
    send_request(&r);
}
static void configure(const char *local_name, int subnet, int server, int alias_count) {
    unsigned idx = if_nametoindex(local_name);
    if (!idx)
        die("private bench0");
    for (int i = 0; i < (server ? alias_count : 1); ++i) {
        struct request r = {.h = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg)),
                                  .nlmsg_type = RTM_NEWADDR,
                                  .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL}};
        struct ifaddrmsg *a = NLMSG_DATA(&r.h);
        a->ifa_family = AF_INET;
        a->ifa_index = idx;
        a->ifa_prefixlen = 24;
        unsigned char ip[4] = {10, 241, (unsigned char)subnet, (unsigned char)(server ? i + 1 : 254)};
        attr(&r, IFA_ADDRESS, ip, 4);
        attr(&r, IFA_LOCAL, ip, 4);
        send_request(&r);
    }
    struct request r = {.h = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                              .nlmsg_type = RTM_NEWLINK,
                              .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK}};
    struct ifinfomsg *m = NLMSG_DATA(&r.h);
    m->ifi_family = AF_UNSPEC;
    m->ifi_index = idx;
    m->ifi_flags = IFF_UP;
    m->ifi_change = IFF_UP;
    send_request(&r);
}
static int send_ns(int socket, int fd) {
    char val = 'N';
    struct iovec io = {.iov_base = &val, .iov_len = 1};
    union {
        char bytes[CMSG_SPACE(sizeof(fd))];
        struct cmsghdr align;
    } buf = {0};
    struct msghdr msg = {
        .msg_iov = &io, .msg_iovlen = 1, .msg_control = buf.bytes, .msg_controllen = sizeof(buf.bytes)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    return sendmsg(socket, &msg, MSG_NOSIGNAL) == 1 ? 0 : -1;
}
static int receive_ns(int socket) {
    char val = 0;
    struct iovec io = {.iov_base = &val, .iov_len = 1};
    union {
        char bytes[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } buf = {0};
    struct msghdr msg = {
        .msg_iov = &io, .msg_iovlen = 1, .msg_control = buf.bytes, .msg_controllen = sizeof(buf.bytes)};
    if (recvmsg(socket, &msg, MSG_CMSG_CLOEXEC) != 1 || val != 'N' || msg.msg_flags & MSG_CTRUNC) {
        errno = EPROTO;
        die("peer namespace");
    }
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
        c->cmsg_len != CMSG_LEN(sizeof(int))) {
        errno = EPROTO;
        die("peer namespace FD");
    }
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    return fd;
}
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6)
        return 2;
    int server = !strcmp(argv[1], "server"), subnet = atoi(argv[2]), aliases = atoi(argv[3]);
    if ((!server && strcmp(argv[1], "client")) || subnet < 1 || subnet > 200 || aliases < 1 ||
        aliases > 64)
        return 2;
    int local_index = (argc == 6 && !server) ? atoi(argv[5]) : 0;
    if (local_index < 0 || local_index > 7)
        return 2;
    char local_name[16];
    snprintf(local_name, sizeof(local_name), "bench%d", local_index);
    unsigned long host = strtoul(argv[4], NULL, 10);
    int ours = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    if (ours < 0)
        die("own namespace");
    unsigned long own = ns_inode(ours);
    if (own == host || !host || if_nametoindex(local_name)) {
        errno = EPERM;
        die("unique disposable namespace required");
    }
    int ctl = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (ctl < 0)
        die("private rendezvous");
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, "/pair/setup.sock");
    if (server) {
        if (bind(ctl, (struct sockaddr *)&a, sizeof(a)) || chmod(a.sun_path, 0666) || listen(ctl, 1))
            die("private rendezvous listen");
        int pair = accept(ctl, NULL, NULL);
        if (pair < 0 || send_ns(pair, ours))
            die("offer own private network handle");
        char ready;
        if (recv(pair, &ready, 1, 0) != 1 || ready != 'C') {
            errno = EPROTO;
            die("client endpoint setup");
        }
        configure(local_name, subnet, 1, aliases);
        if (send(pair, "R", 1, MSG_NOSIGNAL) != 1)
            die("private endpoint ready");
        close(pair);
    } else {
        if (connect(ctl, (struct sockaddr *)&a, sizeof(a)))
            die("private rendezvous connect");
        int peer = receive_ns(ctl);
        unsigned long other = ns_inode(peer);
        if (other == host || other == own) {
            errno = EPERM;
            die("target must be the other disposable namespace");
        }
        make_veth(peer, local_name);
        close(peer);
        configure(local_name, subnet, 0, 1);
        if (send(ctl, "C", 1, MSG_NOSIGNAL) != 1)
            die("notify target");
        char ready;
        if (recv(ctl, &ready, 1, 0) != 1 || ready != 'R') {
            errno = EPROTO;
            die("target ready");
        }
    }
    close(ours);
    close(ctl);
    struct __user_cap_header_struct cap = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct zero[2] = {{0}};
    if (syscall(SYS_capset, &cap, zero) || syscall(SYS_capget, &cap, zero))
        die("clear private setup capability");
    printf("configured own namespace %lu, private bench0, final effective=%u permitted=%u\n", own,
           zero[0].effective | zero[1].effective, zero[0].permitted | zero[1].permitted);
    return 0;
}
