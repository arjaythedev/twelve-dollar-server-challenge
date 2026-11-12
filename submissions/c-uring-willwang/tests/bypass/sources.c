/* Give each stock client UID a preferred IPv4 source on the one private veth. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/ethtool.h>
#include <linux/fib_rules.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_SOURCES = 64, FIRST_UID = 41000, FIRST_ADDRESS = 32, FIRST_TABLE = 100 };

static void fail(const char *what) {
    perror(what);
    exit(1);
}

static unsigned long number(const char *text) {
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || *text < '0' || *text > '9') {
        fprintf(stderr, "invalid number: %s\n", text);
        exit(2);
    }
    return value;
}

static unsigned client_device(unsigned long host_inode) {
    struct stat ns;
    if (stat("/proc/self/ns/net", &ns))
        fail("network namespace");
    if (!host_inode || host_inode == ns.st_ino) {
        fprintf(stderr, "the owned private client network namespace is required\n");
        exit(2);
    }
    struct ifaddrs *all;
    if (getifaddrs(&all))
        fail("client interfaces");
    unsigned index = 0;
    for (struct ifaddrs *a = all; a; a = a->ifa_next) {
        if (a->ifa_flags & IFF_LOOPBACK)
            continue;
        if (strcmp(a->ifa_name, "bench0")) {
            fprintf(stderr, "the client must have exactly one private veth\n");
            exit(2);
        }
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET &&
            ((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr == inet_addr("10.241.1.254"))
            index = if_nametoindex(a->ifa_name);
    }
    freeifaddrs(all);
    if (!index) {
        fprintf(stderr, "missing private client address 10.241.1.254 on bench0\n");
        exit(2);
    }
    return index;
}

static int ethtool(int fd, void *command) {
    struct ifreq request = {0};
    strcpy(request.ifr_name, "bench0");
    request.ifr_data = command;
    int ret = ioctl(fd, SIOCETHTOOL, &request);
    if (ret < 0)
        fail("ethtool on private client bench0");
    return ret;
}

static void report_features(int fd, unsigned words, const char *phase) {
    struct ethtool_gfeatures *features = calloc(1, sizeof(*features) + words * sizeof(features->features[0]));
    if (!features)
        fail("allocate feature snapshot");
    features->cmd = ETHTOOL_GFEATURES;
    features->size = words;
    ethtool(fd, features);
    if (features->size != words) {
        fprintf(stderr, "private veth feature count changed\n");
        exit(1);
    }
    for (unsigned i = 0; i < words; i++) {
        struct ethtool_get_features_block *b = &features->features[i];
        printf("%s %u %08x %08x %08x %08x\n", phase, i, b->available, b->requested, b->active, b->never_changed);
    }
    free(features);
}

/* A real NIC normally finishes CHECKSUM_PARTIAL before sending a frame;
 * an XSK receiving bytes over a Linux veth has no such checksum metadata. */
static void materialize_checksums(void) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        fail("private ethtool socket");
    struct ethtool_drvinfo driver = {.cmd = ETHTOOL_GDRVINFO};
    ethtool(fd, &driver);
    if (strcmp(driver.driver, "veth")) {
        fprintf(stderr, "the selected private client device must use veth\n");
        exit(2);
    }
    uint64_t mask = UINT64_C(1) << ETH_SS_FEATURES;
    struct ethtool_sset_info *counts = calloc(1, sizeof(*counts) + sizeof(uint32_t));
    if (!counts)
        fail("allocate feature count");
    counts->cmd = ETHTOOL_GSSET_INFO;
    counts->sset_mask = mask;
    ethtool(fd, counts);
    unsigned n = counts->data[0];
    if (counts->sset_mask != mask || !n || n > 4096) {
        fprintf(stderr, "private veth does not expose a usable feature list\n");
        exit(1);
    }
    free(counts);
    struct ethtool_gstrings *names = calloc(1, sizeof(*names) + n * ETH_GSTRING_LEN);
    if (!names)
        fail("allocate feature names");
    names->cmd = ETHTOOL_GSTRINGS;
    names->string_set = ETH_SS_FEATURES;
    names->len = n;
    ethtool(fd, names);
    unsigned target = n, words = (n + 31) / 32;
    for (unsigned i = 0; i < n; i++) {
        const char *name = (const char *)names->data + i * ETH_GSTRING_LEN;
        if (*name)
            printf("feature %u %.*s\n", i, ETH_GSTRING_LEN, name);
        if (!strncmp(name, "tx-checksum-ip-generic", ETH_GSTRING_LEN))
            target = i;
    }
    free(names);
    if (target == n) {
        fprintf(stderr, "private client veth has no generic IPv4 TX checksum feature\n");
        exit(1);
    }
    report_features(fd, words, "before");
    struct ethtool_sfeatures *set = calloc(1, sizeof(*set) + words * sizeof(set->features[0]));
    if (!set)
        fail("allocate checksum request");
    set->cmd = ETHTOOL_SFEATURES;
    set->size = words;
    set->features[target / 32].valid = UINT32_C(1) << (target % 32);
    /* All requested bits are zero. Only the one 'valid' bit can be changed. */
    int flags = ethtool(fd, set);
    free(set);
    printf("set_status %d\n", flags);
    report_features(fd, words, "after");
    close(fd);
}

struct message {
    struct nlmsghdr h;
    unsigned char body[256];
};

static struct message create(unsigned type, size_t payload, unsigned flags) {
    return (struct message){.h = {.nlmsg_type = type, .nlmsg_len = NLMSG_LENGTH(payload),
        .nlmsg_flags = NLM_F_REQUEST | flags, .nlmsg_seq = 1}};
}

static void attr(struct message *m, int type, const void *data, size_t n) {
    size_t start = NLMSG_ALIGN(m->h.nlmsg_len);
    size_t end = start + RTA_SPACE(n);
    if (end > sizeof(*m)) {
        fprintf(stderr, "netlink message overflow\n");
        exit(1);
    }
    struct rtattr *a = (void *)((char *)m + start);
    a->rta_type = type;
    a->rta_len = RTA_LENGTH(n);
    memcpy(RTA_DATA(a), data, n);
    m->h.nlmsg_len = end;
}

static void exchange(int fd, struct message *m, struct message *reply) {
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    if (sendto(fd, m, m->h.nlmsg_len, 0, (void *)&kernel, sizeof(kernel)) != (ssize_t)m->h.nlmsg_len)
        fail("send private route operation");
    ssize_t size = recv(fd, reply, sizeof(*reply), MSG_TRUNC);
    if (size < 0)
        fail("receive private route operation");
    if (size > (ssize_t)sizeof(*reply) || !NLMSG_OK(&reply->h, size) || reply->h.nlmsg_seq != 1) {
        fprintf(stderr, "incomplete private route reply\n");
        exit(1);
    }
    if (reply->h.nlmsg_type == NLMSG_ERROR) {
        if (reply->h.nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
            fprintf(stderr, "incomplete private route acknowledgement\n");
            exit(1);
        }
        int error = ((struct nlmsgerr *)NLMSG_DATA(&reply->h))->error;
        if (error) {
            errno = -error;
            fail("private route operation");
        }
    }
}

static void configure(int fd, unsigned dev, unsigned i) {
    const unsigned change = NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    uint32_t addr = htonl(0x0af10100u + FIRST_ADDRESS + i);
    uint32_t subnet = htonl(0x0af10100u), table = FIRST_TABLE + i, priority = 1000 + i;
    struct fib_rule_uid_range uid = {.start = FIRST_UID + i, .end = FIRST_UID + i};
    struct message reply, m = create(RTM_NEWADDR, sizeof(struct ifaddrmsg), change);
    struct ifaddrmsg *a = NLMSG_DATA(&m.h);
    *a = (struct ifaddrmsg){.ifa_family = AF_INET, .ifa_prefixlen = 32, .ifa_index = dev};
    attr(&m, IFA_LOCAL, &addr, sizeof(addr));
    attr(&m, IFA_ADDRESS, &addr, sizeof(addr));
    exchange(fd, &m, &reply);

    m = create(RTM_NEWROUTE, sizeof(struct rtmsg), change);
    struct rtmsg *r = NLMSG_DATA(&m.h);
    *r = (struct rtmsg){.rtm_family = AF_INET, .rtm_dst_len = 24, .rtm_table = table,
        .rtm_protocol = RTPROT_STATIC, .rtm_scope = RT_SCOPE_LINK, .rtm_type = RTN_UNICAST};
    attr(&m, RTA_DST, &subnet, sizeof(subnet));
    attr(&m, RTA_OIF, &dev, sizeof(dev));
    attr(&m, RTA_PREFSRC, &addr, sizeof(addr));
    exchange(fd, &m, &reply);

    m = create(RTM_NEWRULE, sizeof(struct fib_rule_hdr), change);
    struct fib_rule_hdr *rule = NLMSG_DATA(&m.h);
    *rule = (struct fib_rule_hdr){.family = AF_INET, .dst_len = 24, .table = table, .action = FR_ACT_TO_TBL};
    attr(&m, FRA_DST, &subnet, sizeof(subnet));
    attr(&m, FRA_UID_RANGE, &uid, sizeof(uid));
    attr(&m, FRA_PRIORITY, &priority, sizeof(priority));
    exchange(fd, &m, &reply);

    /* Resolve both services exactly as a socket with this UID will. */
    const char *targets[] = {"10.241.1.1", "10.241.1.253"};
    for (size_t n = 0; n < sizeof(targets) / sizeof(targets[0]); n++) {
        m = create(RTM_GETROUTE, sizeof(struct rtmsg), 0);
        r = NLMSG_DATA(&m.h);
        *r = (struct rtmsg){.rtm_family = AF_INET, .rtm_dst_len = 32};
        uint32_t destination = inet_addr(targets[n]), source = 0, output = 0;
        attr(&m, RTA_DST, &destination, sizeof(destination));
        attr(&m, RTA_UID, &uid.start, sizeof(uid.start));
        exchange(fd, &m, &reply);
        if (reply.h.nlmsg_type != RTM_NEWROUTE || reply.h.nlmsg_len < NLMSG_LENGTH(sizeof(*r))) {
            fprintf(stderr, "invalid route lookup for UID %u\n", uid.start);
            exit(1);
        }
        r = NLMSG_DATA(&reply.h);
        int bytes = RTM_PAYLOAD(&reply.h);
        for (struct rtattr *p = RTM_RTA(r); RTA_OK(p, bytes); p = RTA_NEXT(p, bytes)) {
            if (RTA_PAYLOAD(p) != sizeof(uint32_t))
                continue;
            if (p->rta_type == RTA_PREFSRC) memcpy(&source, RTA_DATA(p), sizeof(source));
            if (p->rta_type == RTA_OIF) memcpy(&output, RTA_DATA(p), sizeof(output));
        }
        if (source != addr || output != dev) {
            fprintf(stderr, "UID %u route to %s selected an unexpected source or device\n", uid.start, targets[n]);
            exit(1);
        }
    }
    printf("source %u 10.241.1.%u\n", uid.start, FIRST_ADDRESS + i);
}

static void ports(const char *name, const char *path) {
    char line[65536];
    FILE *f = fopen(path, "r");
    if (!f || !fgets(line, sizeof(line), f))
        fail(path);
    line[strcspn(line, "\n")] = 0;
    printf("%s %s\n", name, line);
    fclose(f);
}

/* A real unbound TCP socket for the optional two-sided source attribution test. */
static void identify(unsigned long port) {
    if (!port || port > 65535 || getuid() < FIRST_UID || getuid() >= FIRST_UID + MAX_SOURCES)
        exit(2);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("10.241.1.1"),
                               .sin_port = htons(port)};
    if (fd < 0 || connect(fd, (void *)&peer, sizeof(peer)))
        fail("connect private TCP source probe");
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    char address[INET_ADDRSTRLEN], reply;
    if (getsockname(fd, (void *)&local, &size) || !inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address)) ||
        send(fd, "?", 1, MSG_NOSIGNAL) != 1 || recv(fd, &reply, 1, 0) != 1 || reply != '!')
        fail("identify private TCP endpoints");
    printf("%s %u\n", address, ntohs(local.sin_port));
    close(fd);
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4)
        return 2;
    unsigned dev = client_device(number(argv[1]));
    if (argc == 3 && !strcmp(argv[2], "wire-checksums")) {
        materialize_checksums();
        return 0;
    }
    if (argc == 4) {
        if (strcmp(argv[2], "identify"))
            return 2;
        identify(number(argv[3]));
        return 0;
    }
    unsigned long count = number(argv[2]);
    if (!count || count > MAX_SOURCES)
        return 2;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        fail("private route socket");
    for (unsigned i = 0; i < count; ++i)
        configure(fd, dev, i);
    close(fd);
    ports("port_range", "/proc/sys/net/ipv4/ip_local_port_range");
    ports("reserved", "/proc/sys/net/ipv4/ip_local_reserved_ports");
    return 0;
}
