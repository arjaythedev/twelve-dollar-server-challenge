#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <linux/tcp.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { EXCHANGES = 24, REQUEST = 60, RESPONSE = 5102, MAXCPU = 512 };

static void fail(const char *msg) {
    perror(msg);
    exit(1);
}

static void show_file(const char *name) {
    FILE *fp = fopen(name, "r");
    if (!fp) {
        printf("%s unavailable errno=%d\n", name, errno);
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp))
        printf("%s %s", name, line);
    fclose(fp);
}

static void environment(void) {
    struct io_uring_params params = {0};
    int ring = syscall(SYS_io_uring_setup, 4, &params);
    if (ring < 0)
        printf("io_uring_setup failed errno=%d %s\n", errno, strerror(errno));
    else {
        printf("io_uring_setup OK fd=%d\n", ring);
        close(ring);
    }
    printf("uid=%u cpu=%d\n", (unsigned)getuid(), sched_getcpu());
    const char *files[] = {"/proc/net/route",
                           "/sys/class/net/eth0/mtu",
                           "/sys/class/net/eth0/queues/rx-0/rps_cpus",
                           "/sys/class/net/eth0/queues/tx-0/xps_cpus",
                           "/proc/sys/net/netfilter/nf_conntrack_count",
                           "/proc/sys/net/netfilter/nf_conntrack_max",
                           "/proc/sys/net/ipv4/ip_local_port_range",
                           "/proc/self/attr/current"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        show_file(files[i]);
    FILE *fp = fopen("/proc/self/status", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (!strncmp(line, "CapEff:", 7) || !strncmp(line, "Seccomp:", 8) ||
                !strncmp(line, "NoNewPrivs:", 11))
                fputs(line, stdout);
        }
        fclose(fp);
    }
}

static void transfer(int fd, void *buf, size_t length, int send_data) {
    for (size_t off = 0; off < length;) {
        ssize_t n = send_data ? send(fd, (char *)buf + off, length - off, MSG_NOSIGNAL)
                              : recv(fd, (char *)buf + off, length - off, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = ECONNRESET;
            fail(send_data ? "send" : "recv");
        }
        off += (size_t)n;
    }
}

static void note_cpu(int fd, unsigned counts[MAXCPU]) {
    int n;
    socklen_t len = sizeof(n);
    if (getsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &n, &len))
        fail("SO_INCOMING_CPU");
    if (n >= 0 && n < MAXCPU)
        ++counts[n];
}

static long long ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        fail("clock");
    return (long long)t.tv_sec * 1000000000 + t.tv_nsec;
}

static int compare_time(const void *a, const void *b) {
    long long x = *(long long *)a, y = *(long long *)b;
    return (x > y) - (x < y);
}

static void report(int fd, const char *side, unsigned counts[MAXCPU], long long *times) {
    int mtu;
    socklen_t mlen = sizeof(mtu);
    struct tcp_info tcp = {0};
    socklen_t size = sizeof(tcp);
    if (getsockopt(fd, IPPROTO_IP, IP_MTU, &mtu, &mlen) ||
        getsockopt(fd, IPPROTO_TCP, TCP_INFO, &tcp, &size))
        fail("TCP_INFO");
    printf("side=%s process_cpu=%d mtu=%d snd_mss=%u rcv_mss=%u rtt_us=%u segs_in=%u segs_out=%u "
           "incoming_cpu=",
           side, sched_getcpu(), mtu, tcp.tcpi_snd_mss, tcp.tcpi_rcv_mss, tcp.tcpi_rtt, tcp.tcpi_segs_in,
           tcp.tcpi_segs_out);
    for (int i = 0; i < MAXCPU; ++i)
        if (counts[i])
            printf("%d:%u,", i, counts[i]);
    if (times) {
        qsort(times, EXCHANGES, sizeof(*times), compare_time);
        printf(" echo_median_us=%.3f", times[EXCHANGES / 2] / 1000.);
    }
    struct sockaddr_in original = {0};
    socklen_t original_size = sizeof(original);
    if (!getsockopt(fd, IPPROTO_IP, 80, &original, &original_size)) {
        char address[INET_ADDRSTRLEN];
        printf(" conntrack_original=%s:%u",
               inet_ntop(AF_INET, &original.sin_addr, address, sizeof(address)),
               ntohs(original.sin_port));
    } else
        printf(" conntrack_lookup_errno=%d", errno);
    putchar('\n');
    show_file("/proc/sys/net/netfilter/nf_conntrack_count");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "info")) {
        environment();
        return 0;
    }
    if (argc != 4 || (strcmp(argv[1], "server") && strcmp(argv[1], "client")))
        return 2;
    int server = !strcmp(argv[1], "server");
    const char *activated = server ? getenv("TWELVE_TEST_LISTENER_FD") : NULL;
    int sock = activated ? atoi(activated) : socket(AF_INET, SOCK_STREAM, 0), fd = sock;
    if (sock < 0)
        fail("socket");
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)atoi(argv[3]))};
    if (inet_pton(AF_INET, argv[2], &addr.sin_addr) != 1)
        return 2;
    if (server) {
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) || listen(sock, 1))
            fail("listen");
        printf("listening %s:%s\n", argv[2], argv[3]);
        fd = accept(sock, NULL, NULL);
        if (fd < 0)
            fail("accept");
    } else if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)))
        fail("connect");
    struct timeval deadline = {.tv_sec = 5};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline)))
        fail("timeout");
    unsigned counts[MAXCPU] = {0};
    long long times[EXCHANGES];
    char request[REQUEST] = {0}, reply[RESPONSE] = {0};
    for (int i = 0; i < EXCHANGES; ++i) {
        if (server) {
            transfer(fd, request, sizeof(request), 0);
            note_cpu(fd, counts);
            transfer(fd, reply, sizeof(reply), 1);
        } else {
            long long start = ns();
            transfer(fd, request, sizeof(request), 1);
            transfer(fd, reply, sizeof(reply), 0);
            times[i] = ns() - start;
            note_cpu(fd, counts);
        }
    }
    if (server) {
        transfer(fd, request, 1, 0);
        report(fd, "server", counts, NULL);
        transfer(fd, reply, 1, 1);
    } else {
        report(fd, "client", counts, times);
        transfer(fd, request, 1, 1);
        transfer(fd, reply, 1, 0);
    }
    if (server)
        close(sock);
    close(fd);
    return 0;
}
