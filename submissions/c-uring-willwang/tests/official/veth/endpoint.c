/* Namespace-local socket activation. The native server owns and configures
 * every new TCP file after the handoff; this helper handles no IP packets. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void fail(const char *s) {
    perror(s);
    exit(1);
}

static int tcp_metrics(char *out, size_t size, const char *path, const char *prefix) {
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;
    char names[12000], values[12000];
    int used = 0;
    while (fgets(names, sizeof(names), file) && fgets(values, sizeof(values), file)) {
        if (strncmp(names, prefix, strlen(prefix)))
            continue;
        char *a = NULL, *b = NULL, *name = strtok_r(names, " \n", &a),
             *value = strtok_r(values, " \n", &b);
        while (name && value) {
            if (!strcmp(name, "CurrEstab") || !strcmp(name, "InSegs") || !strcmp(name, "OutSegs") ||
                !strcmp(name, "RetransSegs") || !strcmp(name, "AttemptFails") ||
                !strcmp(name, "EstabResets") || !strcmp(name, "ListenDrops") ||
                !strcmp(name, "ListenOverflows") || !strcmp(name, "TCPKeepAlive") ||
                !strcmp(name, "TCPSynRetrans") || !strcmp(name, "TCPTimeouts") ||
                !strcmp(name, "TCPBacklogDrop") || !strcmp(name, "SyncookiesSent")) {
                int n = snprintf(out + used, size - used, "%s=%s ", name, value);
                if (n < 0 || (size_t)n >= size - used) {
                    fclose(file);
                    return used;
                }
                used += n;
            }
            name = strtok_r(NULL, " \n", &a);
            value = strtok_r(NULL, " \n", &b);
        }
        break;
    }
    fclose(file);
    return used;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int sd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sd < 0)
        fail("unix socket");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, "/control/new.sock");
    if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) || chmod(addr.sun_path, 0666) || listen(sd, 16))
        fail("listen");
    puts("ENDPOINT_READY");
    for (;;) {
        int peer = accept(sd, NULL, NULL);
        if (peer < 0 && errno == EINTR)
            continue;
        if (peer < 0)
            fail("accept");
        unsigned char flags;
        if (recv(peer, &flags, 1, 0) != 1) {
            close(peer);
            continue;
        }
        if (flags == 'T') {
            char result[768];
            int n = tcp_metrics(result, sizeof(result), "/proc/net/snmp", "Tcp:");
            n += tcp_metrics(result + n, sizeof(result) - n, "/proc/net/netstat", "TcpExt:");
            (void)send(peer, result, n, MSG_NOSIGNAL);
            close(peer);
            continue;
        }
        if (flags == 'I' || (flags >= '0' && flags <= '7')) {
            unsigned ordinal = flags == 'I' ? 0 : (unsigned)(flags - '0');
            char output[768] = {0};
            int used = 0;
            const char *keys[] = {"rx_bytes",         "rx_packets",     "rx_dropped",    "rx_errors",
                                  "rx_missed_errors", "rx_fifo_errors", "tx_bytes",      "tx_packets",
                                  "tx_dropped",       "tx_errors",      "tx_fifo_errors"};
            for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
                char path[128];
                snprintf(path, sizeof(path), "/sys/class/net/bench%u/statistics/%s", ordinal, keys[i]);
                FILE *f = fopen(path, "r");
                unsigned long long n;
                if (f) {
                    if (fscanf(f, "%llu", &n) == 1)
                        used += snprintf(output + used, sizeof(output) - used, "%s=%llu ", keys[i], n);
                    fclose(f);
                }
            }
            (void)send(peer, output, used, MSG_NOSIGNAL);
            close(peer);
            continue;
        }
        if (flags == 'S') {
            char line[256];
            FILE *fp = fopen("/proc/net/sockstat", "r");
            if (fp) {
                while (fgets(line, sizeof(line), fp))
                    if (!strncmp(line, "TCP:", 4)) {
                        (void)send(peer, line, strlen(line), MSG_NOSIGNAL);
                        break;
                    }
                fclose(fp);
            }
            close(peer);
            continue;
        }
        if (flags == '?') {
            char buf[128] = {0};
            FILE *fp = fopen("/proc/sys/net/netfilter/nf_conntrack_count", "r");
            if (fp) {
                if (!fgets(buf, sizeof(buf), fp))
                    strcpy(buf, "unavailable\n");
                fclose(fp);
            } else
                strcpy(buf, "unavailable\n");
            (void)send(peer, buf, strlen(buf), MSG_NOSIGNAL);
            close(peer);
            continue;
        }
        if (flags == 'L') {
            FILE *fp = fopen("/proc/net/tcp", "r");
            char line[1024];
            const char *ready = "0";
            if (fp) {
                while (fgets(line, sizeof(line), fp)) {
                    unsigned ip, port, state;
                    if (sscanf(line, " %*u: %x:%x %*s %x", &ip, &port, &state) == 3 && port == 19432 &&
                        state == 10) {
                        ready = "1";
                        break;
                    }
                }
                fclose(fp);
            }
            (void)send(peer, ready, 1, MSG_NOSIGNAL);
            close(peer);
            continue;
        }
        if (flags > 1 && flags != 'Q') {
            close(peer);
            continue;
        }
        /* Send a namespace-bound read-only counter fd for passive manager sampling. */
        int fd = flags == 'Q'
                     ? open("/proc/net/snmp", O_RDONLY | O_CLOEXEC)
                     : socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | (flags ? SOCK_NONBLOCK : 0), 0);
        if (fd < 0)
            fail("empty TCP socket");
        unsigned char ack = 0;
        struct iovec iov = {.iov_base = &ack, .iov_len = 1};
        union {
            char bytes[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } ancillary = {0};
        struct msghdr msg = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = ancillary.bytes,
                             .msg_controllen = sizeof(ancillary.bytes)};
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(fd));
        memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
        (void)sendmsg(peer, &msg, MSG_NOSIGNAL);
        close(fd);
        close(peer);
    }
}
