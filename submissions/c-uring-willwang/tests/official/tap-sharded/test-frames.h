/* Local datagram ports preserve frame boundaries and can force the nonblocking
 * writes of either switch to return EAGAIN without privileges or a TAP. */
#ifndef TEST_FRAMES_H
#define TEST_FRAMES_H

#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>

#ifdef NDEBUG
#error "Build these fixtures with assertions enabled"
#endif

enum { TEST_FRAME_BYTES = 8192 };
static int peers[MAX_PORTS];

static void open_test_ports(void) {
    count = MAX_PORTS;
    for (unsigned port = 0; port < MAX_PORTS; ++port) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        ports[port] = pair[0];
        peers[port] = pair[1];
    }
}

static void make_frame(unsigned char *bytes, unsigned source, unsigned destination,
                       unsigned sequence, size_t length, int gso) {
    assert(length >= 64 && length <= TEST_FRAME_BYTES);
    memset(bytes, 0, length);
    struct virtio_net_hdr header = {
        .flags = VIRTIO_NET_HDR_F_NEEDS_CSUM, .csum_start = 34, .csum_offset = 16};
    if (gso) {
        header.gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
        header.hdr_len = 54;
        header.gso_size = 1450;
    }
    memcpy(bytes, &header, HEADER);
    unsigned char *ethernet = bytes + HEADER;
    static const unsigned char prefix[5] = {2, 0x12, 0x0d, 0, 0};
    if (destination < MAX_PORTS) {
        memcpy(ethernet, prefix, sizeof(prefix));
        ethernet[5] = (unsigned char)(destination + 1);
    } else {
        memset(ethernet, 0xff, 6);
    }
    memcpy(ethernet + 6, prefix, sizeof(prefix));
    ethernet[11] = (unsigned char)(source + 1);
    ethernet[12] = 8;
    memcpy(ethernet + 16, &sequence, sizeof(sequence));
    for (size_t i = HEADER + 24; i < length; ++i)
        bytes[i] = (unsigned char)(sequence * 31 + i);
}

static void submit(unsigned source, unsigned destination, unsigned sequence, size_t length,
                   int gso) {
    unsigned char bytes[TEST_FRAME_BYTES];
    make_frame(bytes, source, destination, sequence, length, gso);
    assert(write(peers[source], bytes, length) == (ssize_t)length);
}

static void verify(unsigned port, unsigned source, unsigned destination, unsigned sequence,
                   size_t length, int gso) {
    unsigned char wanted[TEST_FRAME_BYTES], received[TEST_FRAME_BYTES];
    make_frame(wanted, source, destination, sequence, length, gso);
    ssize_t got = read(peers[port], received, sizeof(received));
    if (got != (ssize_t)length)
        fprintf(stderr, "port %u frame %u: got %zd bytes, expected %zu\n", port, sequence, got,
                length);
    assert(got == (ssize_t)length);
    assert(memcmp(wanted, received, length) == 0);
}

static void expect_empty(unsigned port) {
    char byte;
    assert(read(peers[port], &byte, 1) < 0 && errno == EAGAIN);
}

static void run_case(void (*body)(void)) {
    /* The production switch uses process-lifetime queues and counters. */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

#endif
