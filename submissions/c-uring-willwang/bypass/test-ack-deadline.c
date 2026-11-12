/* Check the bounded queue independently of upstream's TCP timers. */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lwip/priv/tcp_priv.h"
#include "lwip/sys.h"

typedef struct {
    struct tcp_pcb *pcb;
    uint16_t ack_slot;
} Connection;

enum { TX_RETRY_MS = 2 };
static uint32_t now;
static unsigned outputs, refusals;
static struct tcp_pcb *last;

u32_t sys_now(void) { return now; }

static err_t mock_output(struct tcp_pcb *pcb) {
    assert(pcb->flags & TF_ACK_NOW);
    ++outputs;
    last = pcb;
    if (refusals) {
        --refusals;
        return ERR_MEM;
    }
    pcb->flags &= ~(TF_ACK_DELAY | TF_ACK_NOW);
    return ERR_OK;
}
#define tcp_output mock_output
#include "ack-deadline.h"

static Connection connections[SHORT_ACK_SLOTS + 1];
static struct tcp_pcb pcbs[SHORT_ACK_SLOTS + 1];

static void reset(void) {
    memset(&delayed, 0, sizeof(delayed));
    memset(connections, 0, sizeof(connections));
    memset(pcbs, 0, sizeof(pcbs));
    outputs = refusals = now = 0;
    last = NULL;
    for (unsigned i = 0; i <= SHORT_ACK_SLOTS; ++i) {
        connections[i].pcb = &pcbs[i];
        pcbs[i].state = ESTABLISHED;
        pcbs[i].flags = TF_ACK_DELAY;
    }
}

static void check_wrap_and_cancellation(void) {
    Connection *c = connections;
    reset();
    now = UINT32_MAX - 19;
    observe_ack(&c[0]);
    assert(ack_wait(now) == 40);
    now += 21;
    observe_ack(&c[0]);
    assert(ack_wait(now) == 19);
    observe_ack(&c[1]);
    observe_ack(&c[2]);
    discard_ack(&c[1]);
    pcbs[1].flags = 0;
    observe_ack(&c[1]);
    assert(!c[1].ack_slot);
    now += 18;
    release_due_acks(now);
    assert(!outputs);
    release_due_acks(++now);
    assert(outputs == 1 && last == &pcbs[0] && ack_wait(now) == 21);
    pcbs[2].flags = 0; /* Native output piggybacked first. */
    now += 21;
    release_due_acks(now);
    assert(outputs == 1 && ack_wait(now) == UINT_MAX);

    reset();
    observe_ack(&c[0]);
    pcbs[0].flags |= TF_ACK_NOW;
    observe_ack(&c[0]);
    assert(!c[0].ack_slot);
    pcbs[0].flags = TF_ACK_DELAY;
    pcbs[0].state = CLOSE_WAIT;
    observe_ack(&c[0]);
    assert(!c[0].ack_slot);
    observe_ack(&c[1]);
    pcbs[1].state = FIN_WAIT_1;
    observe_ack(&c[2]);
    c[2].pcb = NULL; /* Error callback after PCB destruction. */
    discard_ack(&c[2]);
    now = 40;
    release_due_acks(now);
    assert(!outputs && !delayed.first);
}

static void check_limit_and_reuse(void) {
    Connection *c = connections;
    reset();
    for (unsigned i = 0; i <= SHORT_ACK_SLOTS; ++i) observe_ack(&c[i]);
    assert(delayed.allocated == SHORT_ACK_SLOTS && !c[SHORT_ACK_SLOTS].ack_slot);
    assert(pcbs[SHORT_ACK_SLOTS].flags & TF_ACK_NOW);
    now = 40;
    release_due_acks(now);
    assert(outputs == SHORT_ACK_BURST && !ack_wait(now));
    assert(!c[0].ack_slot && c[SHORT_ACK_BURST].ack_slot);
    while (delayed.first) release_due_acks(now);
    assert(outputs == SHORT_ACK_SLOTS && ack_wait(now) == UINT_MAX);

    pcbs[0].flags = TF_ACK_DELAY;
    observe_ack(&c[0]);
    uint16_t slot = c[0].ack_slot;
    discard_ack(&c[0]);
    pcbs[1].flags = TF_ACK_DELAY;
    observe_ack(&c[1]);
    assert(c[1].ack_slot == slot);
    now += 40;
    release_due_acks(now);
    assert(last == &pcbs[1]);
}

static void check_backpressure(void) {
    reset();
    observe_ack(&connections[0]);
    observe_ack(&connections[1]);
    now = 40;
    refusals = 1;
    release_due_acks(now);
    assert(outputs == 1 && ack_wait(now) == TX_RETRY_MS);
    release_due_acks(now);
    assert(outputs == 1);
    now += TX_RETRY_MS;
    release_due_acks(now);
    assert(outputs == 2 && last == &pcbs[1] && ack_wait(now) == 40 - TX_RETRY_MS);
    now += 40 - TX_RETRY_MS;
    release_due_acks(now);
    assert(outputs == 3 && last == &pcbs[0] && !delayed.first);
}

int main(void) {
    check_wrap_and_cancellation();
    check_limit_and_reuse();
    check_backpressure();
    printf("40 ms ACK queue: wraparound, piggyback, FIN/RST, reuse, %u slots,"
           " %u per dispatch, output refusal and retry passed (%zu bytes)\n",
           SHORT_ACK_SLOTS, SHORT_ACK_BURST, sizeof(delayed));
}
