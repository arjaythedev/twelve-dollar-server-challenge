/* Give a nonprivileged process one packet socket already bound to our private veth. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/capability.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static void fail(const char *message) {
    perror(message);
    exit(1);
}

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "packet") && strcmp(argv[1], "baseline")))
        return 2;
    int packet = !strcmp(argv[1], "packet");
    struct stat ns;
    if (stat("/proc/self/ns/net", &ns))
        fail("network namespace");
    unsigned long host = strtoul(argv[2], NULL, 10);
    require(host && host != (unsigned long)ns.st_ino, "this helper needs a disposable nonhost namespace");
    struct if_nameindex *links = if_nameindex();
    if (!links)
        fail("private interface list");
    unsigned count = 0;
    for (struct if_nameindex *p = links; p->if_index; ++p) {
        require(!strcmp(p->if_name, "lo") || !strcmp(p->if_name, "bench0"), "unexpected network interface");
        ++count;
    }
    if_freenameindex(links);
    unsigned idx = if_nametoindex("bench0");
    require(count == 2 && idx, "expected only loopback and the dedicated bench0 link");

    int query = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (query < 0)
        fail("private interface information");
    struct ifreq req = {0};
    strcpy(req.ifr_name, "bench0");
    struct ethtool_drvinfo driver = {.cmd = ETHTOOL_GDRVINFO};
    req.ifr_data = (void *)&driver;
    if (ioctl(query, SIOCETHTOOL, &req))
        fail("veth driver type");
    require(!strcmp(driver.driver, "veth"), "bench0 must be a Linux veth");
    if (ioctl(query, SIOCGIFMTU, &req))
        fail("private MTU");
    int mtu = req.ifr_mtu;
    if (ioctl(query, SIOCGIFHWADDR, &req))
        fail("private MAC");
    unsigned char mac[6];
    memcpy(mac, req.ifr_hwaddr.sa_data, sizeof(mac));
    close(query);
    struct ifaddrs *addresses;
    if (getifaddrs(&addresses))
        fail("private addresses");
    int kernel_ip = 0;
    for (struct ifaddrs *p = addresses; p; p = p->ifa_next) {
        if (!p->ifa_addr || strcmp(p->ifa_name, "bench0") || p->ifa_addr->sa_family != AF_INET)
            continue;
        uint32_t ip = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
        require(ip != inet_addr("10.241.1.253"), "stack-owned address is already a Linux address");
        kernel_ip |= ip == inet_addr("10.241.1.1");
    }
    freeifaddrs(addresses);
    require(kernel_ip, "private baseline address missing");
    FILE *forward = fopen("/proc/sys/net/ipv4/ip_forward", "r");
    require(forward && fgetc(forward) == '0', "private endpoint must not route stack-owned traffic");
    fclose(forward);
    int fd;
    if (packet) {
        fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_ALL));
        if (fd < 0)
            fail("private packet socket");
        struct sockaddr_ll addr = {
            .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = (int)idx};
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)))
            fail("bind to private bench0");
    } else {
        fd = open("/proc/net/snmp", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            fail("private counter handle");
    }
    struct __user_cap_header_struct cap = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct bits[2] = {{0}};
    if (syscall(SYS_capset, &cap, bits) || syscall(SYS_capget, &cap, bits))
        fail("drop capabilities");
    require(!(bits[0].effective | bits[0].permitted | bits[1].effective | bits[1].permitted),
            "descriptor provider retained capabilities");
    int sd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, "/control/packet.sock");
    if (sd < 0 || bind(sd, (struct sockaddr *)&addr, sizeof(addr)) || chmod(addr.sun_path, 0666) ||
        listen(sd, 1))
        fail("private descriptor rendezvous");
    int peer = accept4(sd, NULL, NULL, SOCK_CLOEXEC);
    if (peer < 0)
        fail("descriptor recipient");
    close(sd);
    char metadata[256];
    int len = snprintf(metadata, sizeof(metadata),
                       "{\"ifindex\":%u,\"mtu\":%d,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                       "\"namespace\":%lu,\"effective_caps\":0,\"driver\":\"veth\"}",
                       idx, mtu, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned long)ns.st_ino);
    char ancillary[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec buf = {.iov_base = metadata, .iov_len = (size_t)len};
    struct msghdr msg = {
        .msg_iov = &buf, .msg_iovlen = 1, .msg_control = ancillary, .msg_controllen = sizeof(ancillary)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    if (sendmsg(peer, &msg, MSG_NOSIGNAL) != len)
        fail("transfer private descriptor");
    close(fd);
    /* Retain the creator's measurable cgroup without holding any network socket. */
    char end;
    while (read(peer, &end, 1) < 0 && errno == EINTR) {
    }
    close(peer);
    return 0;
}
