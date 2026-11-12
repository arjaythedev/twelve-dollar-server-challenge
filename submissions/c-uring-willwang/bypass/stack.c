#include "stack.h"
#include "../challenge.h"

#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A complete chunked request fits under 16 KiB of headers + 128 KiB of
 * representation. Allow one more receive window when its last segment also
 * contains pipelined requests. Idle peers own no HTTP payload buffers. */
enum {
    INPUT_LIMIT = 160 * 1024 + TCP_WND,
    INPUT_BUDGET = 96 * 1024 * 1024,
    OUTPUT_BUDGET = 64 * 1024 * 1024,
    PROTOCOL_BUDGET = 512 * 1024 * 1024,
    PASS_SIZE = 96,
    TX_RETRY_MS = 2,
    KEEPALIVE_IDLE_MS = 75000,
    KEEPALIVE_INTERVAL_MS = 10000,
    KEEPALIVE_PROBES = 6
};

typedef struct Connection Connection;
typedef struct Queue {
    Connection *first, *last;
} Queue;

struct Connection {
    struct tcp_pcb *pcb;
    Connection *all_next, **all_prev;
    Queue *queue;
    Connection *prev, *next;
    Buf input, output;
    size_t sent;
    unsigned eof : 1, closing : 1, retry_output : 1;
};

static struct {
    struct netif interface;
    struct tcp_pcb *listener;
    struct stack_config config;
    stack_emit_fn emit;
    void *opaque;
    Connection *all;
    Queue ready, held, retry;
    uint32_t retry_at;
    size_t input_bytes, output_bytes;
    struct stack_stats stats;
    unsigned initialized : 1, up : 1;
} stack;

u32_t sys_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
}

uint32_t stack_random(void) {
    return arc4random();
}

typedef struct Allocation {
    _Alignas(max_align_t) size_t bytes;
} Allocation;

void *stack_protocol_malloc(size_t size) {
    size_t remaining = PROTOCOL_BUDGET - (size_t)stack.stats.protocol_bytes;
    if (remaining < sizeof(Allocation) || size > remaining - sizeof(Allocation)) return NULL;
    size_t total = size + sizeof(Allocation);
    Allocation *block = malloc(total);
    if (!block) return NULL;
    block->bytes = total;
    stack.stats.protocol_bytes += total;
    if (stack.stats.protocol_bytes > stack.stats.peak_protocol_bytes)
        stack.stats.peak_protocol_bytes = stack.stats.protocol_bytes;
    return block + 1;
}

void *stack_protocol_calloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    void *ptr = stack_protocol_malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void stack_protocol_free(void *ptr) {
    if (!ptr) return;
    Allocation *block = (Allocation *)ptr - 1;
    stack.stats.protocol_bytes -= block->bytes;
    free(block);
}

static void account_buffers(void) {
    stack.stats.buffered_bytes = stack.input_bytes + stack.output_bytes;
    if (stack.stats.buffered_bytes > stack.stats.peak_buffered_bytes)
        stack.stats.peak_buffered_bytes = stack.stats.buffered_bytes;
}

static void drop_buffer(Buf *b, size_t *used) {
    *used -= b->cap;
    free(b->data);
    *b = (Buf){0};
    account_buffers();
}

static void dequeue(Connection *c) {
    Queue *q = c->queue;
    if (!q) return;
    if (c->prev) c->prev->next = c->next;
    else q->first = c->next;
    if (c->next) c->next->prev = c->prev;
    else q->last = c->prev;
    c->queue = NULL;
    c->prev = c->next = NULL;
}

static void enqueue(Queue *q, Connection *c) {
    if (c->queue == q) return;
    dequeue(c);
    c->queue = q;
    c->prev = q->last;
    if (q->last) q->last->next = c;
    else q->first = c;
    q->last = c;
}

static void wake(Connection *c) {
    /* A later arrival on the same stream cannot jump an older held mutation. */
    if (c->queue != &stack.held) enqueue(&stack.ready, c);
}

static void retry_later(Connection *c) {
    if (!stack.retry.first) stack.retry_at = sys_now() + TX_RETRY_MS;
    enqueue(&stack.retry, c);
}

static void forget(Connection *c) {
    dequeue(c);
    *c->all_prev = c->all_next;
    if (c->all_next) c->all_next->all_prev = c->all_prev;
    drop_buffer(&c->input, &stack.input_bytes);
    drop_buffer(&c->output, &stack.output_bytes);
    --stack.stats.live;
    free(c);
}

static void on_error(void *arg, err_t err) {
    (void)err;
    /* lwIP has already freed the PCB, including retransmission buffers. */
    forget(arg);
}

static void close_connection(Connection *c, int abort_now) {
    struct tcp_pcb *pcb = c->pcb;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    forget(c);
    if (!abort_now) {
        /* tcp_close can enqueue FIN even when the link cannot send it yet.
         * Have the detached PCB retry locally unsent output every second. */
        tcp_poll(pcb, NULL, 2);
        if (tcp_close(pcb) == ERR_OK) return;
    }
    tcp_abort(pcb);
}

static err_t on_ack(void *arg, struct tcp_pcb *pcb, u16_t length) {
    (void)pcb;
    (void)length;
    Connection *c = arg;
    if (c->output.len || c->retry_output || c->closing || c->eof) wake(c);
    return ERR_OK;
}

static err_t on_data(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    Connection *c = arg;
    if (!p) {
        c->eof = 1;
        wake(c);
        return ERR_OK;
    }
    if (err != ERR_OK) return err;
    size_t length = p->tot_len;
    if (!c->closing && length) {
        size_t need = c->input.len + length;
        if (need > INPUT_LIMIT) return ERR_MEM;
        if (need > c->input.cap) {
            size_t cap = c->input.cap ? c->input.cap : 256;
            while (cap < need) cap *= 2;
            if (cap > INPUT_LIMIT) cap = INPUT_LIMIT;
            if (cap - c->input.cap > INPUT_BUDGET - stack.input_bytes) return ERR_MEM;
            char *data = realloc(c->input.data, cap);
            if (!data) return ERR_MEM;
            stack.input_bytes += cap - c->input.cap;
            c->input.data = data;
            c->input.cap = cap;
            account_buffers();
        }
        pbuf_copy_partial(p, c->input.data + c->input.len, (u16_t)length, 0);
        c->input.len = need;
    }
    pbuf_free(p);
    tcp_recved(pcb, (u16_t)length);
    wake(c);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (!pcb || err != ERR_OK) return err;
    Connection *c = NULL;
    if (stack.stats.live < stack.config.max_connections) c = calloc(1, sizeof(*c));
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    c->pcb = pcb;
    c->all_prev = &stack.all;
    c->all_next = stack.all;
    if (c->all_next) c->all_next->all_prev = &c->all_next;
    stack.all = c;
    ++stack.stats.accepted;
    if (++stack.stats.live > stack.stats.peak_live) stack.stats.peak_live = stack.stats.live;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_data);
    tcp_sent(pcb, on_ack);
    tcp_err(pcb, on_error);
    /* An idle peer can keep this connection indefinitely by acknowledging
     * probes. A lost final FIN/RST cannot strand the server's PCB forever. */
    ip_set_option(pcb, SOF_KEEPALIVE);
    pcb->keep_idle = KEEPALIVE_IDLE_MS;
    pcb->keep_intvl = KEEPALIVE_INTERVAL_MS;
    pcb->keep_cnt = KEEPALIVE_PROBES;
    tcp_nagle_disable(pcb);
    tcp_poll(pcb, NULL, 255);
    return ERR_OK;
}

/* COPY transfers response bytes into lwIP's retransmission queue. ACKs release
 * this credit. The application can then discard the original response even
 * if a physical link temporarily refuses the first attempt to transmit it. */
static void flush(Connection *c) {
    int copied = 0;
    if (c->sent < c->output.len) {
        size_t left = c->output.len - c->sent;
        u16_t room = tcp_sndbuf(c->pcb);
        if (room) {
            u16_t count = left < room ? (u16_t)left : room;
            u8_t flags = TCP_WRITE_FLAG_COPY;
            if (count < left) flags |= TCP_WRITE_FLAG_MORE;
            err_t rc = tcp_write(c->pcb, c->output.data + c->sent, count, flags);
            if (rc == ERR_OK) {
                c->sent += count;
                copied = 1;
            } else if (rc == ERR_MEM) {
                retry_later(c);
            } else {
                close_connection(c, 1);
                return;
            }
        }
    }
    if (copied || c->retry_output) {
        c->retry_output = tcp_output(c->pcb) != ERR_OK;
        if (c->retry_output) retry_later(c);
    }
    if (c->output.len && c->sent == c->output.len) {
        drop_buffer(&c->output, &stack.output_bytes);
        c->sent = 0;
    }
    if (c->output.len || c->retry_output) return;
    if (!c->closing && c->input.len) {
        wake(c);
        return;
    }
    /* The peer receives all committed data before a FIN. Refused data will
     * be offered again by lwIP; consume it first to avoid its unread-data RST. */
    if ((c->closing || c->eof) && tcp_sndbuf(c->pcb) == TCP_SND_BUF && !c->pcb->refused_data)
        close_connection(c, 0);
}

static void discard_input(Connection *c, size_t consumed) {
    if (consumed >= c->input.len || c->closing) {
        drop_buffer(&c->input, &stack.input_bytes);
        return;
    }
    c->input.len -= consumed;
    memmove(c->input.data, c->input.data + consumed, c->input.len);
}

static void server_error(Connection *c) {
    size_t old_cap = c->output.cap;
    c->output.len = 0;
    app_error_500(&c->output);
    stack.output_bytes += c->output.cap - old_cap;
    account_buffers();
    c->closing = 1;
    drop_buffer(&c->input, &stack.input_bytes);
}

static int is_write(const Buf *input) {
    return input->len >= 5 && !memcmp(input->data, "POST ", 5);
}

static void release_retries(uint32_t now) {
    if ((int32_t)(now - stack.retry_at) < 0) return;
    for (unsigned i = 0; i < PASS_SIZE && stack.retry.first; ++i)
        enqueue(&stack.ready, stack.retry.first);
    stack.retry_at = now + TX_RETRY_MS;
}

int stack_dispatch(void) {
    if (!stack.up) return -1;
    sys_check_timeouts();
    release_retries(sys_now());
    if (!stack.ready.first && (!stack.held.first || db_writes_deferred())) return 0;

    /* A connection contributes at most one uncommitted response per pass.
     * No network callback runs while SQLite is evaluating this batch. */
    Connection *done[PASS_SIZE];
    unsigned count = 0;
    db_begin_pass();
    for (unsigned checked = 0; checked < PASS_SIZE; ++checked) {
        int oldest = stack.held.first && !db_writes_deferred();
        Connection *c = oldest ? stack.held.first : stack.ready.first;
        if (!c) break;
        dequeue(c);
        if (c->output.len || c->retry_output || c->closing || !c->input.len) {
            flush(c);
            continue;
        }
        if (!oldest && is_write(&c->input) && (stack.held.first || db_writes_deferred())) {
            enqueue(&stack.held, c);
            continue;
        }
        if (stack.output_bytes >= OUTPUT_BUDGET) {
            retry_later(c);
            continue;
        }

        size_t consumed = 0;
        int close_after = 0;
        int rc = app_request(c->input.data, c->input.len, &consumed, &c->output, &close_after);
        stack.output_bytes += c->output.cap;
        account_buffers();
        if (rc == 0) {
            if (c->eof) {
                c->closing = 1;
                drop_buffer(&c->input, &stack.input_bytes);
                flush(c);
            }
            continue;
        }
        if (rc < 0 || !consumed || consumed > c->input.len) {
            server_error(c);
        } else {
            c->closing = close_after != 0;
            discard_input(c, consumed);
        }
        ++stack.stats.requests;
        done[count++] = c;
    }
    int failed = db_commit_pass();
    if (failed) ++stack.stats.commit_failures;
    for (unsigned i = 0; i < count; ++i) {
        Connection *c = done[i];
        if (failed) server_error(c);
        if (!c->output.len) close_connection(c, 1);
        else flush(c);
    }
    return (int)count;
}

unsigned stack_timeout_ms(void) {
    if (!stack.up) return UINT_MAX;
    if (stack.ready.first) return 0;
    unsigned wait = sys_timeouts_sleeptime();
    if (stack.held.first) {
        if (!db_writes_deferred()) return 0;
        if (wait > 1) wait = 1;
    }
    if (stack.retry.first) {
        int32_t delay = (int32_t)(stack.retry_at - sys_now());
        if (delay <= 0) return 0;
        if ((unsigned)delay < wait) wait = (unsigned)delay;
    }
    return wait;
}

static err_t output_frame(struct netif *netif, struct pbuf *p) {
    (void)netif;
    const unsigned char *frame = p->payload;
    static unsigned char contiguous[UINT16_MAX];
    if (p->len != p->tot_len) {
        pbuf_copy_partial(p, contiguous, p->tot_len, 0);
        frame = contiguous;
    }
    if (stack.emit(stack.opaque, frame, p->tot_len)) {
        ++stack.stats.output_busy;
        return ERR_MEM;
    }
    ++stack.stats.output_frames;
    return ERR_OK;
}

static err_t configure_netif(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 't';
    netif->mtu = stack.config.mtu;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, stack.config.mac, 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = output_frame;
    return ERR_OK;
}

int stack_input(const void *frame, size_t length) {
    if (!stack.up || !frame) return -1;
    ++stack.stats.input_frames;
    const unsigned char *bytes = frame;
    if (length < 14 || length > UINT16_MAX || length > (size_t)stack.config.mtu + 18 ||
        (!(bytes[0] & 1) && memcmp(bytes, stack.config.mac, 6))) {
        ++stack.stats.input_dropped;
        return 0;
    }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_RAM);
    if (!p) {
        ++stack.stats.input_dropped;
        return 0;
    }
    if (pbuf_take(p, frame, (u16_t)length) != ERR_OK ||
        stack.interface.input(p, &stack.interface) != ERR_OK) {
        pbuf_free(p);
        ++stack.stats.input_dropped;
    }
    return 0;
}

int stack_init(const struct stack_config *config, stack_emit_fn emit, void *opaque) {
    if (stack.up || !config || !emit || !config->port || !config->max_connections ||
        !config->address_be || config->mtu < 576 || config->mtu > UINT16_MAX - 18) return -1;
    uint64_t retained = stack.stats.protocol_bytes;
    stack.stats = (struct stack_stats){.protocol_bytes = retained, .peak_protocol_bytes = retained};
    stack.config = *config;
    stack.emit = emit;
    stack.opaque = opaque;
    if (!stack.initialized) {
        lwip_init();
        stack.initialized = 1;
    }
    ip4_addr_t address = {.addr = config->address_be};
    ip4_addr_t mask = {.addr = config->netmask_be};
    ip4_addr_t gateway = {.addr = config->gateway_be};
    if (!netif_add(&stack.interface, &address, &mask, &gateway, NULL, configure_netif, ethernet_input))
        return -1;
    netif_set_default(&stack.interface);
    netif_set_up(&stack.interface);
    netif_set_link_up(&stack.interface);
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb && tcp_bind(pcb, &address, config->port) == ERR_OK)
        stack.listener = tcp_listen(pcb);
    if (!stack.listener) {
        if (pcb) tcp_close(pcb);
        netif_remove(&stack.interface);
        return -1;
    }
    tcp_accept(stack.listener, on_accept);
    stack.up = 1;
    return 0;
}

void stack_get_stats(struct stack_stats *out) {
    if (out) *out = stack.stats;
}

/* Reporting runs only on the issuer, on request. The protocol lists are
 * observed without altering their order, flags or timers. Values are bounded
 * samples; live application peers and detached CLOSE/LAST_ACK PCBs differ. */
void stack_tail_report(FILE *out) {
    if (!out || !stack.up || stack.stats.peak_live < 256 ||
        stack.stats.live > stack.stats.peak_live / 4) return;
    enum { APP_SAMPLE = 128, PROTOCOL_SAMPLE = 512 };
    unsigned apps = 0, established = 0, close_wait = 0, eof = 0, closing = 0;
    unsigned no_credit = 0, unacked = 0, unsent = 0, refused = 0, ooseq = 0;
    unsigned ready = 0, held = 0, retry = 0;
    Connection *first = stack.all;
    for (Connection *c = first; c && apps < APP_SAMPLE; c = c->all_next) {
        struct tcp_pcb *p = c->pcb;
        ++apps;
        established += p->state == ESTABLISHED;
        close_wait += p->state == CLOSE_WAIT;
        eof += c->eof;
        closing += c->closing;
        no_credit += tcp_sndbuf(p) != TCP_SND_BUF;
        unacked += p->unacked != NULL;
        unsent += p->unsent != NULL;
        refused += p->refused_data != NULL;
        ooseq += p->ooseq != NULL;
        ready += c->queue == &stack.ready;
        held += c->queue == &stack.held;
        retry += c->queue == &stack.retry;
    }
    unsigned active = 0, detached = 0, last_ack = 0, fin_wait = 0;
    unsigned fin_unsent = 0, fin_unacked = 0, time_wait = 0;
    for (struct tcp_pcb *p = tcp_active_pcbs; p && active < PROTOCOL_SAMPLE; p = p->next) {
        ++active;
        if (p->callback_arg) continue;
        ++detached;
        last_ack += p->state == LAST_ACK;
        fin_wait += p->state == FIN_WAIT_1 || p->state == FIN_WAIT_2;
        fin_unsent += p->unsent != NULL && p->unacked == NULL;
        fin_unacked += p->unacked != NULL;
    }
    for (struct tcp_pcb *p = tcp_tw_pcbs; p && time_wait < PROTOCOL_SAMPLE; p = p->next)
        ++time_wait;
    char remote[IP4ADDR_STRLEN_MAX] = "-";
    unsigned port = 0, state = 0;
    if (first) {
        ip4addr_ntoa_r(ip_2_ip4(&first->pcb->remote_ip), remote, sizeof(remote));
        port = first->pcb->remote_port;
        state = (unsigned)first->pcb->state;
    }
    fprintf(out, "bypass tail app_sample=%u established=%u close_wait=%u eof=%u closing=%u"
            " credit_used=%u unacked=%u unsent=%u refused=%u ooseq=%u"
            " ready=%u held=%u retry=%u active_sample=%u detached=%u last_ack=%u"
            " fin_wait=%u detached_unsent=%u detached_unacked=%u time_wait_sample=%u"
            " peer=%s:%u peer_state=%u\n",
            apps, established, close_wait, eof, closing, no_credit, unacked, unsent, refused,
            ooseq, ready, held, retry, active, detached, last_ack, fin_wait, fin_unsent,
            fin_unacked, time_wait, remote, port, state);
}

void stack_shutdown(void) {
    if (!stack.up) return;
    tcp_close(stack.listener);
    stack.listener = NULL;
    while (stack.all) close_connection(stack.all, 1);
    netif_set_link_down(&stack.interface);
    netif_remove(&stack.interface);
    stack.up = 0;
}
