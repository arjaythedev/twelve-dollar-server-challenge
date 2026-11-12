#ifndef CHALLENGE_NETWORK_STACK_H
#define CHALLENGE_NETWORK_STACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* IPv4 addresses are in network byte order; port and interface MTU are host order. */
struct stack_config {
    uint32_t address_be, netmask_be, gateway_be;
    uint8_t mac[6];
    uint16_t port, mtu;
    unsigned max_connections;
};

struct stack_stats {
    uint64_t accepted, live, peak_live;
    uint64_t input_frames, input_dropped, output_frames, output_busy;
    uint64_t requests, commit_failures;
    uint64_t buffered_bytes, peak_buffered_bytes;
    uint64_t protocol_bytes, peak_protocol_bytes;
};

/* The callback must copy or synchronously accept the frame and return zero.
 * A negative return means that the frame could not be queued. */
typedef int (*stack_emit_fn)(void *opaque, const unsigned char *frame, size_t length);

/* Call on one thread after initializing the HTTP handler and opening SQLite. */
int stack_init(const struct stack_config *config, stack_emit_fn emit, void *opaque);
int stack_input(const void *ether_frame, size_t length);
int stack_dispatch(void);
unsigned stack_timeout_ms(void);
void stack_get_stats(struct stack_stats *out);
/* Bounded on-demand diagnostics after a large population has begun closing. */
void stack_tail_report(FILE *out);
void stack_shutdown(void);

#endif
