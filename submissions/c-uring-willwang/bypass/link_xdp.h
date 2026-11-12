#ifndef TWELVE_BYPASS_XDP_H
#define TWELVE_BYPASS_XDP_H
#include "link.h"
typedef struct XdpLink XdpLink;
XdpLink *xdp_link_open(const LinkConfig *, unsigned ifindex, char *error, size_t size);
int xdp_link_receive(XdpLink *, LinkFrame *, unsigned capacity);
void xdp_link_release(XdpLink *);
int xdp_link_send(XdpLink *, const void *, unsigned);
int xdp_link_flush(XdpLink *);
int xdp_link_wait(XdpLink *, int timeout_ms, int pending_packet_tx_fd);
const char *xdp_link_mode(const XdpLink *);
int xdp_link_local_listener(const XdpLink *);
void xdp_link_set_local_epoll(XdpLink *, int epfd);
void xdp_link_report(const XdpLink *);
void xdp_link_close(XdpLink *);
#endif
