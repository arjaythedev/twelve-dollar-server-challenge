/* Included only when compiling the calendar port's copy of stack.c.
 * The raw application configures keepalive before its live poll interval.
 * Detached poll and FIN processing continue through tcp_poll itself. */
#ifndef CHALLENGE_SLOW_PORT_H
#define CHALLENGE_SLOW_PORT_H
#include "lwip/tcp.h"
static inline void challenge_slow_poll(struct tcp_pcb *pcb, tcp_poll_fn poll, u8_t interval) {
    tcp_poll(pcb, poll, interval);
    if (poll == NULL && interval == 255)
        tcp_slow_work_enroll(pcb);
}
#define tcp_poll(pcb, poll, interval) challenge_slow_poll((pcb), (poll), (interval))
#endif
