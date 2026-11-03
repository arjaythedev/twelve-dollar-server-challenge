#define _GNU_SOURCE
#include "uring_server.h"
#include <liburing.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(feed_uring_event)==24,"event ABI must match C#");
_Static_assert(sizeof(feed_uring_response)==24,"response ABI must match C#");

#define MAX_RINGS 32
#define SQ_ENTRIES 1024
/* Under a backlog, deferred completion work consumes a receive buffer for every ready
 * socket at once. A small pool runs dry, ending multishot receives with ENOBUFS so each
 * must be re-armed, which deepens the backlog. 16384 x 2 KiB per ring (256 MiB in total,
 * touched only as used) keeps them armed; the CQ holds a completion per outstanding
 * buffer plus sends. */
#define CQ_ENTRIES 32768
#define BUFFER_COUNT 16384
#define BUFFER_SIZE 2048
#define BUFFER_GROUP 1
#define ACCEPT_WINDOW 32
#define IDLE_SECONDS 120
#define TX_HIGH (64u * 1024u)
#define TX_LOW (32u * 1024u)
#define TX_CONN_LIMIT (256u * 1024u)
#define TX_TOTAL_LIMIT (64u * 1024u * 1024u)
#define GENERATION_MASK 0x0fffffffu

enum { OP_RECV=1, OP_SEND=2, OP_CLOSE=3, OP_CANCEL=4, OP_ACCEPT=5 };
enum { F_LIVE=1, F_CLOSING=2, F_NOTIFIED=4, F_RX=8, F_TX=16,
       F_CANCEL=32, F_CANCEL_SENT=64, F_CLOSE=128, F_AFTER=256,
       F_DIRTY=512, F_WRITABLE=1024, F_EOF_PENDING=2048, F_EOF=4096 };

typedef struct tx_chunk {
    struct tx_chunk *next;
    uint32_t length, offset;
    unsigned char bytes[];
} tx_chunk;
typedef struct connection {
    tx_chunk *head, *tail;
    uint32_t generation, flags, tx_bytes, next_dirty, last_activity;
} connection;
typedef struct ring_state {
    struct io_uring ring;
    struct io_uring_buf_ring *buffer_ring;
    unsigned char *buffers;
    connection *connections;
    uint32_t dirty_head, dirty_tail, dirty_count, active, accept_pending, buffer_adds;
    int64_t accept_retry_ns;
    int initialized;
} ring_state;
typedef struct held_buffer { uint16_t ring, id; } held_buffer;
struct feed_uring {
    int listen_fd, wake_fd, count, slots, fatal_error, next_ring;
    _Atomic int stopping;
    ring_state *rings;
    held_buffer *held; /* buffers handed to the caller by the last poll */
    unsigned held_count;
    uint32_t last_sweep;
    uint64_t queued_tx_bytes;
};

static int64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static uint64_t connection_id(unsigned ring, unsigned slot, unsigned gen) {
    return ((uint64_t)gen << 32) | ((uint64_t)ring << 24) | slot;
}
static uint64_t operation_id(uint64_t id, unsigned op) { return (id << 4) | op; }
static void dirty(ring_state *r, unsigned slot) {
    connection *c=&r->connections[slot];
    if (c->flags & F_DIRTY) return;
    c->flags |= F_DIRTY;
    c->next_dirty=0;
    if (r->dirty_tail) r->connections[r->dirty_tail-1].next_dirty=slot+1;
    else r->dirty_head=slot+1;
    r->dirty_tail=slot+1;
    r->dirty_count++;
}
static connection *lookup(feed_uring *s, uint64_t id, ring_state **ring, unsigned *slot) {
    unsigned ri=(unsigned)((id >> 24) & 255), ix=(unsigned)(id & 0xffffff);
    if (ri >= (unsigned)s->count || ix >= (unsigned)s->slots || (id >> 60)) return NULL;
    ring_state *r=&s->rings[ri];
    connection *c=&r->connections[ix];
    if (!(c->flags & F_LIVE) || c->generation != (unsigned)(id >> 32)) return NULL;
    if (ring) *ring=r;
    if (slot) *slot=ix;
    return c;
}
static int flush(feed_uring *s, ring_state *r) {
    int rc=io_uring_submit(&r->ring);
    if (rc < 0 && rc != -EINTR && rc != -EAGAIN) s->fatal_error=-rc;
    return rc;
}
static struct io_uring_sqe *get_sqe(feed_uring *s, ring_state *r) {
    struct io_uring_sqe *sqe=io_uring_get_sqe(&r->ring);
    if (!sqe) {
        flush(s,r);
        sqe=io_uring_get_sqe(&r->ring);
    }
    return sqe;
}
static void free_chunk(feed_uring *s, connection *c, tx_chunk *p) {
    c->tx_bytes-=p->length;
    s->queued_tx_bytes-=p->length;
    free(p);
}
/* A submitted send may still reference the head; retain it until its CQE. */
static void discard_transmits(feed_uring *s, connection *c) {
    tx_chunk *p;
    if (c->flags & F_TX) {
        p=c->head->next;
        c->head->next=NULL;
        c->tail=c->head;
    } else {
        p=c->head;
        c->head=c->tail=NULL;
    }
    while (p) { tx_chunk *next=p->next; free_chunk(s,c,p); p=next; }
}
static void begin_close(feed_uring *s, ring_state *r, unsigned slot) {
    connection *c=&r->connections[slot];
    c->flags |= F_CLOSING;
    discard_transmits(s,c);
    dirty(r,slot);
}
static void recycle_buffer(ring_state *r, unsigned id) {
    io_uring_buf_ring_add(r->buffer_ring, r->buffers+(size_t)id*BUFFER_SIZE, BUFFER_SIZE, id,
        io_uring_buf_ring_mask(BUFFER_COUNT), r->buffer_adds++);
}
static void publish_buffers(ring_state *r) {
    if (r->buffer_adds) {
        io_uring_buf_ring_advance(r->buffer_ring,r->buffer_adds);
        r->buffer_adds=0;
    }
}
static void arm_accepts(feed_uring *s, ring_state *r) {
    /* One-shot direct accepts reserve capacity before submission. Unlike
     * multishot accept, they cannot consume/drop a socket after the table fills. */
    if (r->accept_retry_ns && monotonic_ns()<r->accept_retry_ns) return;
    r->accept_retry_ns=0;
    while (r->accept_pending < ACCEPT_WINDOW && r->active+r->accept_pending < (unsigned)s->slots) {
        struct io_uring_sqe *sqe=get_sqe(s,r);
        if (!sqe) break;
        io_uring_prep_accept_direct(sqe,s->listen_fd,NULL,NULL,0,IORING_FILE_INDEX_ALLOC);
        io_uring_sqe_set_data64(sqe,OP_ACCEPT);
        r->accept_pending++;
    }
}
static void push_event(feed_uring_event *events, int *count, uint64_t id, int kind) {
    events[(*count)++]=(feed_uring_event){id,NULL,0,kind};
}
static void drive_connection(feed_uring *s, ring_state *r, unsigned ri,
    unsigned slot, feed_uring_event *events, int max, int *count) {
    connection *c=&r->connections[slot];
    if (!(c->flags & F_LIVE)) return;
    uint64_t id=connection_id(ri,slot,c->generation);
    if ((c->flags & F_AFTER) && !c->head) c->flags |= F_CLOSING;
    int pause=(c->flags & (F_CLOSING|F_AFTER)) || c->tx_bytes >= TX_HIGH;
    if (pause && (c->flags & F_RX) && !(c->flags & F_CANCEL_SENT)) {
        struct io_uring_sqe *sqe=get_sqe(s,r);
        if (!sqe) { dirty(r,slot); return; }
        io_uring_prep_cancel64(sqe,operation_id(id,OP_RECV),0);
        io_uring_sqe_set_data64(sqe,operation_id(id,OP_CANCEL));
        c->flags |= F_CANCEL|F_CANCEL_SENT;
    }
    if (c->flags & F_CLOSING) {
        if (!(c->flags & F_NOTIFIED)) {
            if (*count == max) { dirty(r,slot); return; }
            push_event(events,count,id,FEED_EVENT_CLOSED);
            c->flags |= F_NOTIFIED;
        }
        if (!(c->flags & (F_RX|F_TX|F_CANCEL|F_CLOSE))) {
            struct io_uring_sqe *sqe=get_sqe(s,r);
            if (!sqe) { dirty(r,slot); return; }
            io_uring_prep_close_direct(sqe,slot);
            io_uring_sqe_set_data64(sqe,operation_id(id,OP_CLOSE));
            c->flags |= F_CLOSE;
        }
        return;
    }
    /* The data events preceding EOF have already been appended to this batch.
     * The application acknowledges EOF only after its pipeline is processed. */
    if (c->flags & F_EOF_PENDING) {
        if (*count<max) {
            push_event(events,count,id,FEED_EVENT_EOF);
            c->flags &= ~F_EOF_PENDING;
        } else dirty(r,slot);
    }
    if (c->flags & F_WRITABLE) {
        if (*count<max) {
            push_event(events,count,id,FEED_EVENT_WRITABLE);
            c->flags &= ~F_WRITABLE;
        } else dirty(r,slot);
    }
    if (c->head && !(c->flags & F_TX)) {
        struct io_uring_sqe *sqe=get_sqe(s,r);
        if (!sqe) { dirty(r,slot); return; }
        tx_chunk *p=c->head;
        io_uring_prep_send(sqe,slot,p->bytes+p->offset,p->length-p->offset,MSG_NOSIGNAL);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe,operation_id(id,OP_SEND));
        c->flags |= F_TX;
    }
    if (!(c->flags & (F_RX|F_CANCEL|F_AFTER|F_EOF)) && c->tx_bytes <= TX_LOW) {
        struct io_uring_sqe *sqe=get_sqe(s,r);
        if (!sqe) { dirty(r,slot); return; }
        io_uring_prep_recv_multishot(sqe,slot,NULL,0,0);
        sqe->flags |= IOSQE_FIXED_FILE|IOSQE_BUFFER_SELECT;
        sqe->buf_group=BUFFER_GROUP;
        io_uring_sqe_set_data64(sqe,operation_id(id,OP_RECV));
        c->flags |= F_RX;
        c->flags &= ~F_CANCEL_SENT;
    }
}
static void drive_ring(feed_uring *s, unsigned ri, feed_uring_event *events, int max, int *count) {
    ring_state *r=&s->rings[ri];
    /* A bounded pass prevents a full event array or SQ from spinning. */
    unsigned budget=r->dirty_count<SQ_ENTRIES ? r->dirty_count : SQ_ENTRIES;
    for (unsigned n=0; n<budget && r->dirty_head; n++) {
        unsigned slot=r->dirty_head-1;
        connection *c=&r->connections[slot];
        r->dirty_head=c->next_dirty;
        r->dirty_count--;
        if (!r->dirty_head) r->dirty_tail=0;
        c->flags &= ~F_DIRTY;
        c->next_dirty=0;
        drive_connection(s,r,ri,slot,events,max,count);
    }
    publish_buffers(r);
    arm_accepts(s,r);
}

static int harvest(feed_uring *s, unsigned ri, feed_uring_event *events, int max, int *count) {
    ring_state *r=&s->rings[ri];
    int processed=0;
    uint32_t now=(uint32_t)(monotonic_ns()/1000000000LL); /* idle tracking needs seconds only */
    for (unsigned budget=0; budget<256; budget++) {
        struct io_uring_cqe *cqe;
        int peek=io_uring_peek_cqe(&r->ring,&cqe);
        if (peek < 0) {
            if (peek != -EAGAIN && peek != -EINTR) s->fatal_error=-peek;
            break;
        }
        uint64_t ud=io_uring_cqe_get_data64(cqe), id=ud >> 4;
        unsigned op=(unsigned)(ud & 15), flags=cqe->flags;
        int result=cqe->res;
        unsigned slot=(unsigned)(id & 0xffffff);
        connection *c=NULL;
        if (op != OP_ACCEPT && slot<(unsigned)s->slots) {
            c=&r->connections[slot];
            if (!(c->flags & F_LIVE) || c->generation!=(unsigned)(id >> 32)) c=NULL;
        }
        if (op == OP_RECV && c && result>0 && !(c->flags & F_CLOSING) && *count == max) break;
        io_uring_cqe_seen(&r->ring,cqe);
        processed++;
        if (op == OP_ACCEPT) {
            if (r->accept_pending) r->accept_pending--;
            if (result>=0) {
                if ((unsigned)result >= (unsigned)s->slots || (r->connections[result].flags & F_LIVE)) {
                    s->fatal_error=EPROTO;
                    break;
                }
                c=&r->connections[result];
                c->generation=(c->generation+1)&GENERATION_MASK;
                if (!c->generation) c->generation=1;
                /* An explicit close racing the terminal close CQE may have
                 * left this slot on the dirty list. Preserve that list link. */
                c->flags=(c->flags & F_DIRTY)|F_LIVE;
                c->last_activity=now;
                r->active++;
                dirty(r,(unsigned)result);
            } else if (result == -ENOMEM || result == -ENOBUFS || result == -ENFILE) {
                r->accept_retry_ns=monotonic_ns()+50000000LL;
            } else if (result != -EAGAIN && result != -EINTR &&
                       result != -ECONNABORTED && result != -EPROTO &&
                       result != -ECANCELED && result != -ENETDOWN &&
                       result != -ENONET && result != -ENETUNREACH &&
                       result != -EHOSTDOWN && result != -EHOSTUNREACH &&
                       result != -EOPNOTSUPP && result != -ENOPROTOOPT) {
                s->fatal_error=-result;
            }
            continue;
        }
        if (op == OP_RECV) {
            if (c && !(flags & IORING_CQE_F_MORE)) {
                c->flags &= ~(F_RX|F_CANCEL_SENT);
                dirty(r,slot);
            }
            if (flags & IORING_CQE_F_BUFFER) {
                unsigned bid=flags >> IORING_CQE_BUFFER_SHIFT;
                if (bid>=BUFFER_COUNT) { s->fatal_error=EPROTO; break; }
                if (c && result>0 && !(c->flags & F_CLOSING)) {
                    if (result>BUFFER_SIZE) { s->fatal_error=EPROTO; break; }
                    events[(*count)++]=(feed_uring_event){id,r->buffers+(size_t)bid*BUFFER_SIZE,
                        result,FEED_EVENT_DATA};
                    s->held[s->held_count++]=(held_buffer){(uint16_t)ri,(uint16_t)bid};
                    c->last_activity=now;
                } else recycle_buffer(r,bid);
            } else if (result>0) {
                s->fatal_error=EPROTO;
            }
            if (c && result==0 && !(c->flags & F_CLOSING)) {
                c->flags |= F_EOF_PENDING|F_EOF;
                dirty(r,slot);
            } else if (c && result<0 && result != -ECANCELED && result != -ENOBUFS && result != -EAGAIN) {
                begin_close(s,r,slot);
            }
        } else if (c && op == OP_SEND) {
            c->flags &= ~F_TX;
            tx_chunk *p=c->head;
            if (!p) { s->fatal_error=EPROTO; break; }
            if (result>0 && (unsigned)result<=p->length-p->offset) {
                unsigned before=c->tx_bytes;
                p->offset+=(unsigned)result;
                c->last_activity=now;
                if (p->offset==p->length || (c->flags & F_CLOSING)) {
                    c->head=p->next;
                    if (!c->head) c->tail=NULL;
                    free_chunk(s,c,p);
                }
                if ((before>=TX_HIGH && c->tx_bytes<TX_HIGH) || !c->head) c->flags |= F_WRITABLE;
            } else if (result != -EAGAIN && result != -EINTR) {
                begin_close(s,r,slot);
            }
            dirty(r,slot);
        } else if (c && op == OP_CANCEL) {
            c->flags &= ~F_CANCEL;
            /* F_CANCEL_SENT survives until the terminal receive CQE. */
            if (result<0 && result != -ENOENT && result != -EALREADY && result != -ECANCELED)
                s->fatal_error=-result;
            dirty(r,slot);
        } else if (c && op == OP_CLOSE) {
            if (result<0 && result != -EBADF) { s->fatal_error=-result; break; }
            discard_transmits(s,c);
            /* A previously queued dirty entry can still link this slot. Do
             * not clear its link until drive_ring removes that entry. */
            c->flags &= F_DIRTY;
            r->active--;
        }
    }
    return processed;
}

int32_t feed_uring_available(int32_t slots) {
    struct rlimit limit;
    if (slots<1 || getrlimit(RLIMIT_NOFILE,&limit)<0 || (rlim_t)slots>limit.rlim_cur) return 0;
    struct io_uring ring;
    struct io_uring_params params={0};
    params.flags=IORING_SETUP_SINGLE_ISSUER|IORING_SETUP_DEFER_TASKRUN|IORING_SETUP_TASKRUN_FLAG;
    if (io_uring_queue_init_params(8,&ring,&params)<0) return 0;
    io_uring_queue_exit(&ring);
    return 1;
}

feed_uring *feed_uring_open(const char *host, int32_t port, int32_t rings, int32_t slots,
    int32_t *out_error) {
    int error=EINVAL, rc;
    if (out_error) *out_error=0;
    if (!host || port<1 || port>65535 || rings<1 || rings>MAX_RINGS || slots<1 || slots>0xffffff)
        goto fail_early;
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE,&limit)<0) { error=errno; goto fail_early; }
    if ((rlim_t)slots>limit.rlim_cur) { error=EMFILE; goto fail_early; }
    feed_uring *s=calloc(1,sizeof(*s));
    if (!s) { error=ENOMEM; goto fail_early; }
    atomic_init(&s->stopping,0);
    s->listen_fd=s->wake_fd=-1;
    s->count=rings; s->slots=slots;
    s->rings=calloc((size_t)rings,sizeof(*s->rings));
    s->held=calloc((size_t)rings*BUFFER_COUNT,sizeof(*s->held));
    if (!s->rings || !s->held) { error=ENOMEM; goto fail; }
    s->wake_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    if (s->wake_fd<0) { error=errno; goto fail; }
    s->listen_fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if (s->listen_fd<0) { error=errno; goto fail; }
    /* Small kernel socket buffers keep hundreds of thousands of idle connections cheap;
     * responses are a few KiB and are queued in user space when a socket is full. */
    int one=1, receive_size=4096, send_size=4096;
    if (setsockopt(s->listen_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))<0 ||
        setsockopt(s->listen_fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))<0 ||
        setsockopt(s->listen_fd,SOL_SOCKET,SO_RCVBUF,&receive_size,sizeof(receive_size))<0 ||
        setsockopt(s->listen_fd,SOL_SOCKET,SO_SNDBUF,&send_size,sizeof(send_size))<0) {
        error=errno; goto fail;
    }
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if (inet_pton(AF_INET,host,&address.sin_addr)!=1) { error=EINVAL; goto fail; }
    if (bind(s->listen_fd,(struct sockaddr *)&address,sizeof(address))<0 ||
        listen(s->listen_fd,4096)<0) { error=errno; goto fail; }
    for (int ri=0; ri<rings; ri++) {
        ring_state *r=&s->rings[ri];
        struct io_uring_params params={0};
        /* Completion work runs in batches when this thread enters the kernel instead of
         * interrupting it. TASKRUN_FLAG lets liburing's CQ peek enter only when work is pending. */
        params.flags=IORING_SETUP_CQSIZE|IORING_SETUP_SINGLE_ISSUER|
                     IORING_SETUP_DEFER_TASKRUN|IORING_SETUP_TASKRUN_FLAG;
        params.cq_entries=CQ_ENTRIES;
        rc=io_uring_queue_init_params(SQ_ENTRIES,&r->ring,&params);
        if (rc<0) { error=-rc; goto fail; }
        r->initialized=1;
        /* Deferred task work does not wake a poll() sleeping on the ring fd; the eventfd does. */
        rc=io_uring_register_eventfd(&r->ring,s->wake_fd);
        if (rc<0) { error=-rc; goto fail; }
        rc=io_uring_register_ring_fd(&r->ring);
        if (rc<0) { error=-rc; goto fail; }
        /* Use the syscall directly: liburing's convenience sparse registration
         * may attempt setrlimit on failure. This transport never raises it. */
        struct io_uring_rsrc_register reg={.nr=(unsigned)slots,.flags=IORING_RSRC_REGISTER_SPARSE};
        rc=(int)syscall(__NR_io_uring_register,r->ring.ring_fd,IORING_REGISTER_FILES2,&reg,sizeof(reg));
        if (rc<0) { error=errno; goto fail; }
        r->connections=calloc((size_t)slots,sizeof(*r->connections));
        r->buffers=mmap(NULL,(size_t)BUFFER_COUNT*BUFFER_SIZE,PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (!r->connections || r->buffers==MAP_FAILED) { error=ENOMEM; goto fail; }
        r->buffer_ring=io_uring_setup_buf_ring(&r->ring,BUFFER_COUNT,BUFFER_GROUP,0,&rc);
        if (!r->buffer_ring) { error=-rc; goto fail; }
        for (unsigned j=0; j<BUFFER_COUNT; j++) recycle_buffer(r,j);
        publish_buffers(r);
        arm_accepts(s,r);
        rc=flush(s,r);
        if (rc<0) { error=-rc; goto fail; }
    }
    return s;
fail:
    feed_uring_free(s);
fail_early:
    if (out_error) *out_error=error;
    return NULL;
}

int32_t feed_uring_poll(feed_uring *s, feed_uring_event *events, int32_t max, int32_t timeout_ms) {
    if (!s || !events || max<1 || timeout_ms < -1) return -EINVAL;
    int count=0, did_work=0;
    /* Recycle exactly once, after the caller has finished the prior batch. */
    for (unsigned i=0; i<s->held_count; i++) recycle_buffer(&s->rings[s->held[i].ring],s->held[i].id);
    s->held_count=0;
    int waited=0;
again:
    if (atomic_load_explicit(&s->stopping,memory_order_relaxed) || s->fatal_error)
        return -(s->fatal_error ? s->fatal_error : ECANCELED);
    uint32_t now=(uint32_t)(monotonic_ns()/1000000000LL);
    if (now!=s->last_sweep) {
        s->last_sweep=now;
        for (int ri=0; ri<s->count; ri++) {
            ring_state *r=&s->rings[ri];
            for (int j=0; j<s->slots; j++) {
                connection *c=&r->connections[j];
                if ((c->flags & F_LIVE) && !(c->flags & F_CLOSING) &&
                    (uint32_t)(now-c->last_activity)>=IDLE_SECONDS)
                    begin_close(s,r,(unsigned)j);
            }
        }
    }
    for (int n=0; n<s->count; n++) {
        unsigned ri=(unsigned)((s->next_ring+n)%s->count);
        drive_ring(s,ri,events,max,&count);
        flush(s,&s->rings[ri]);
        did_work+=harvest(s,ri,events,max,&count);
        drive_ring(s,ri,events,max,&count);
        flush(s,&s->rings[ri]);
    }
    s->next_ring=(s->next_ring+1)%s->count;
    if (s->fatal_error) return -s->fatal_error;
    if (count || waited || timeout_ms==0 || did_work) return count;
    struct pollfd fds[MAX_RINGS+1];
    fds[0]=(struct pollfd){s->wake_fd,POLLIN,0};
    for (int i=0; i<s->count; i++) fds[i+1]=(struct pollfd){s->rings[i].ring.ring_fd,POLLIN,0};
    int wait_ms=(timeout_ms<0 || timeout_ms>1000) ? 1000 : timeout_ms;
    int64_t wait_now=monotonic_ns();
    for (int i=0; i<s->count; i++) if (s->rings[i].accept_retry_ns) {
        int64_t remaining=s->rings[i].accept_retry_ns-wait_now;
        int retry_ms=remaining>0 ? (int)((remaining+999999)/1000000) : 0;
        if (retry_ms<wait_ms) wait_ms=retry_ms;
    }
    if (poll(fds,(nfds_t)s->count+1,wait_ms)<0 && errno != EINTR) return -errno;
    uint64_t wake_count;
    while (read(s->wake_fd,&wake_count,sizeof(wake_count))>0) {}
    waited=1;
    goto again;
}

static int32_t enqueue_response(feed_uring *s, uint64_t id, const void *data, int32_t length,
    int32_t close_after) {
    ring_state *r; unsigned slot;
    connection *c=lookup(s,id,&r,&slot);
    if (!c || (c->flags & (F_CLOSING|F_AFTER)) || atomic_load_explicit(&s->stopping,memory_order_relaxed))
        return -ENOTCONN;
    if (c->tx_bytes+(unsigned)length>TX_CONN_LIMIT || s->queued_tx_bytes+(unsigned)length>TX_TOTAL_LIMIT) {
        begin_close(s,r,slot);
        return -ENOBUFS;
    }
    tx_chunk *p=malloc(sizeof(*p)+(size_t)length);
    if (!p) { begin_close(s,r,slot); return -ENOMEM; }
    p->next=NULL; p->length=(unsigned)length; p->offset=0;
    memcpy(p->bytes,data,(size_t)length);
    if (c->tail) c->tail->next=p; else c->head=p;
    c->tail=p;
    c->tx_bytes+=(unsigned)length;
    s->queued_tx_bytes+=(unsigned)length;
    if (close_after) c->flags |= F_AFTER;
    dirty(r,slot);
    return 0;
}
int32_t feed_uring_send(feed_uring *s, uint64_t id, const void *data, int32_t length, int32_t close_after) {
    if (!s || !data || length<1 || (unsigned)length>TX_CONN_LIMIT) return -EINVAL;
    return enqueue_response(s,id,data,length,close_after);
}
int32_t feed_uring_send_batch(feed_uring *s, const void *data, int32_t length,
    feed_uring_response *entries, int32_t count) {
    if (!s || !data || length<1 || !entries || count<1 || count>256) return -EINVAL;
    for (int i=0;i<count;i++) {
        feed_uring_response *entry=&entries[i];
        if (entry->offset<0 || entry->length<1 || (unsigned)entry->length>TX_CONN_LIMIT ||
            entry->length>length || entry->offset>length-entry->length) entry->result=-EINVAL;
        else entry->result=enqueue_response(s,entry->connection,(const char *)data+entry->offset,
            entry->length,entry->close_after);
    }
    return 0;
}
int32_t feed_uring_close(feed_uring *s, uint64_t id) {
    if (!s) return -EINVAL;
    ring_state *r; unsigned slot;
    connection *c=lookup(s,id,&r,&slot);
    if (!c) return -ENOTCONN;
    begin_close(s,r,slot);
    return 0;
}
int32_t feed_uring_finish(feed_uring *s, uint64_t id) {
    if (!s) return -EINVAL;
    ring_state *r; unsigned slot;
    connection *c=lookup(s,id,&r,&slot);
    if (!c || (c->flags & F_CLOSING)) return -ENOTCONN;
    c->flags |= F_AFTER;
    dirty(r,slot);
    return 0;
}
int64_t feed_uring_pending_tx(feed_uring *s, uint64_t id) {
    if (!s) return -EINVAL;
    connection *c=lookup(s,id,NULL,NULL);
    return (!c || (c->flags & F_CLOSING)) ? -ENOTCONN : (int64_t)c->tx_bytes;
}
void feed_uring_stop(feed_uring *s) {
    if (!s) return;
    atomic_store_explicit(&s->stopping,1,memory_order_relaxed);
    uint64_t one=1;
    if (write(s->wake_fd,&one,sizeof(one))<0) { /* the poll timeout still observes stopping */ }
}
void feed_uring_free(feed_uring *s) {
    if (!s) return;
    /* No concurrent callers. Synchronous cancellation protects user buffers
     * from deferred io-wq access before releasing their backing allocations. */
    if (s->rings) for (int i=0; i<s->count; i++) {
        ring_state *r=&s->rings[i];
        int safe=1;
        if (r->initialized) {
            io_uring_submit(&r->ring);
            struct io_uring_sync_cancel_reg cancel={0};
            cancel.fd=-1;
            cancel.flags=IORING_ASYNC_CANCEL_ANY|IORING_ASYNC_CANCEL_ALL;
            cancel.timeout.tv_sec=5;
            int rc;
            do { rc=io_uring_register_sync_cancel(&r->ring,&cancel); } while (rc == -EINTR);
            safe=(rc>=0 || rc == -ENOENT);
        }
        if (safe && r->buffer_ring) io_uring_free_buf_ring(&r->ring,r->buffer_ring,BUFFER_COUNT,BUFFER_GROUP);
        if (r->initialized) io_uring_queue_exit(&r->ring);
        /* On an exceptional failed/timed-out kernel cancellation, preserve
         * referenced buffers until process exit rather than risk a use-after-free. */
        if (safe && r->connections) for (int j=0; j<s->slots; j++) {
            tx_chunk *p=r->connections[j].head;
            while (p) { tx_chunk *next=p->next; free(p); p=next; }
        }
        if (safe && r->buffers && r->buffers!=MAP_FAILED)
            munmap(r->buffers,(size_t)BUFFER_COUNT*BUFFER_SIZE);
        free(r->connections);
    }
    if (s->listen_fd>=0) close(s->listen_fd);
    if (s->wake_fd>=0) close(s->wake_fd);
    free(s->rings); free(s->held);
    free(s);
}
