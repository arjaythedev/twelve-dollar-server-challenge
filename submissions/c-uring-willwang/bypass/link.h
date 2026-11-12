#ifndef TWELVE_BYPASS_LINK_H
#define TWELVE_BYPASS_LINK_H

#include <stddef.h>
#include <stdint.h>

typedef struct BypassLink BypassLink;

typedef struct {
    const unsigned char *data;
    unsigned len;
} LinkFrame;

typedef enum {
    LINK_AUTO = 0,
    LINK_PACKET,
    LINK_XDP,
    LINK_XDP_ZEROCOPY,
    LINK_XDP_GENERIC       /* Force generic/SKB XDP with XDP_COPY. */
} LinkPreference;

static inline int link_is_xdp(LinkPreference mode) {
    return mode == LINK_XDP || mode == LINK_XDP_ZEROCOPY || mode == LINK_XDP_GENERIC;
}

typedef struct {
    const char *ifname;
    unsigned char mac[6];  /* Must match the dedicated interface's real MAC. */
    uint32_t ipv4_be;      /* Network order (inet_pton). */
    uint16_t port;         /* TCP port in host byte order. */
    unsigned queue_id;     /* AF_XDP hardware receive queue. */
    LinkPreference mode;
    int packet_fd;         /* -1, or a delegated ETH_P_ALL fd bound to ifname. */
    unsigned mtu;          /* 0 discovers the actual interface MTU. */
    unsigned packet_rx_frames; /* AF_PACKET only. Zero selects 512 frame slots. */
    unsigned packet_tx_frames; /* AF_PACKET sender, including hybrid GSO. Zero selects 512 slots. */
    unsigned xdp_tx_frames; /* AF_XDP TX/COMPLETION slots. Zero selects 512. */
    unsigned xdp_rx_frames; /* AF_XDP RX/FILL slots. Zero selects 512. */
    unsigned probe_rx_cpus; /* Enable per-CPU XDP_REDIRECT counters for diagnostics. */
    int local_listener;    /* Shared XDP may serve host-local TCP on its reserved port. */
    uint32_t local_bind_be; /* HOST, in network order. Zero selects the wildcard. */
    int shared_ip;         /* XDP only. Linux owns this interface address and ARP. */
    int packet_gso;        /* Use queued AF_PACKET TCP segmentation for adjacent compatible frames. */
} LinkConfig;

BypassLink *link_open(const LinkConfig *cfg, char *error, size_t error_size);
/* One receive batch may be outstanding. Frames are valid until link_release. */
int link_receive(BypassLink *link, LinkFrame *frames, unsigned capacity);
void link_release(BypassLink *link);
/* Send returns 1 after copying one frame, 0 on backpressure, or -1 on error. */
int link_send(BypassLink *link, const void *frame, unsigned length);
int link_flush(BypassLink *link);
int link_wait(BypassLink *link, int timeout_ms);
const char *link_mode(const BypassLink *link);
/* Borrowed descriptors. The link owns its listener, the caller owns epoll. */
int link_local_listener(const BypassLink *link);
void link_set_local_epoll(BypassLink *link, int epfd);
/* Emit cumulative kernel and userspace ring counters. */
void link_report(BypassLink *link);
void link_close(BypassLink *link);

#endif
