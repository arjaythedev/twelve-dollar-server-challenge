/* Reuse the same moving traffic fixture. Schedule its configured live peers
 * just as the optional port does after the real TCP handshake. */
#include "lwip/priv/tcp_priv.h"
#if LWIP_TCP_SLOW_WORK
static void calendar_register(struct tcp_pcb *pcb) {
    TCP_REG_ACTIVE(pcb);
    if (pcb->pollinterval == 255 && pcb->poll == NULL)
        tcp_slow_work_enroll(pcb);
}
#undef TCP_REG_ACTIVE
#define TCP_REG_ACTIVE(pcb) calendar_register(pcb)
#endif
#include "test-fast-work.c"
