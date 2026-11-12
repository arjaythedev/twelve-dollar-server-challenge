#include "lwip/sys.h"

/* Shorten the first delayed ACK on accepted data only. lwIP still decides
 * whether to piggyback, ACK a second segment, advertise credit or retransmit.
 * All slots are owned by the single stack thread and an accepted connection. */
enum { SHORT_ACK_MS = 40, SHORT_ACK_SLOTS = 8192, SHORT_ACK_BURST = 64 };

struct ack_slot {
    Connection *connection;
    uint32_t due;
    uint16_t previous, next;
};
static struct {
    struct ack_slot slot[SHORT_ACK_SLOTS + 1];
    uint16_t first, last, free_slot, allocated;
    uint32_t retry_at;
    unsigned blocked : 1;
} delayed;

static void discard_ack(Connection *c) {
    uint16_t key = c->ack_slot;
    if (!key) return;
    struct ack_slot *s = &delayed.slot[key];
    if (s->previous) delayed.slot[s->previous].next = s->next;
    else delayed.first = s->next;
    if (s->next) delayed.slot[s->next].previous = s->previous;
    else delayed.last = s->previous;
    c->ack_slot = 0;
    s->connection = NULL;
    s->next = delayed.free_slot;
    delayed.free_slot = key;
    if (!delayed.first) delayed.blocked = 0;
}

static void queue_ack(Connection *c, uint32_t now) {
    if (c->ack_slot) return;
    uint16_t key = delayed.free_slot;
    if (key) {
        delayed.free_slot = delayed.slot[key].next;
    } else if (delayed.allocated < SHORT_ACK_SLOTS) {
        key = ++delayed.allocated;
    } else {
        /* The ordinary tcp_input tail sends this ACK or piggybacks it. */
        tcp_ack_now(c->pcb);
        return;
    }
    struct ack_slot *s = &delayed.slot[key];
    *s = (struct ack_slot){.connection = c, .due = now + SHORT_ACK_MS, .previous = delayed.last};
    c->ack_slot = key;
    if (delayed.last) delayed.slot[delayed.last].next = key;
    else delayed.first = key;
    delayed.last = key;
}

static void observe_ack(Connection *c) {
    struct tcp_pcb *p = c->pcb;
    if (p->state == ESTABLISHED &&
        (p->flags & (TF_ACK_DELAY | TF_ACK_NOW)) == TF_ACK_DELAY) {
        if (!c->ack_slot) queue_ack(c, sys_now());
    } else {
        discard_ack(c);
    }
}

static unsigned ack_wait(uint32_t now) {
    if (!delayed.first) return UINT_MAX;
    int32_t remaining = (int32_t)(delayed.slot[delayed.first].due - now);
    if (delayed.blocked) {
        int32_t retry = (int32_t)(delayed.retry_at - now);
        if (retry > remaining) remaining = retry;
    }
    return remaining > 0 ? (unsigned)remaining : 0;
}

static void release_due_acks(uint32_t now) {
    if (!delayed.first || ack_wait(now)) return;
    delayed.blocked = 0;
    for (unsigned sent = 0; sent < SHORT_ACK_BURST && delayed.first; ++sent) {
        struct ack_slot *s = &delayed.slot[delayed.first];
        if ((int32_t)(now - s->due) < 0) return;
        Connection *c = s->connection;
        struct tcp_pcb *p = c->pcb;
        discard_ack(c);
        if (p->state != ESTABLISHED || !(p->flags & TF_ACK_DELAY)) continue;
        tcp_ack_now(p);
        err_t err = tcp_output(p);
        if (err != ERR_OK || (p->flags & TF_ACK_DELAY)) {
            /* Release other protocol work before retrying the congested NIC.
             * Native TCP may also send the ACK or an ordinary response first. */
            queue_ack(c, now);
            delayed.retry_at = now + TX_RETRY_MS;
            delayed.blocked = 1;
            return;
        }
    }
}
