#ifndef BINARYCORE_CONNECTION_H
#define BINARYCORE_CONNECTION_H

#include "http.h"
#include "io_uring.h"
#include "memory.h"

#include <stdatomic.h>
#include <stdint.h>

/* Connection flags. Modeled on c-iouring-pavlyhalim/src/server.c (CF_*). */
enum {
    bc_cf_open      = 1u << 0,
    bc_cf_sending   = 1u << 1,
    bc_cf_closing   = 1u << 2,
    bc_cf_close_after = 1u << 3,
    bc_cf_replied   = 1u << 4,   /* a reply is waiting for this batch's commit */
    bc_cf_ready     = 1u << 5,   /* on the ready list (has pipelined/pending input) */
    bc_cf_held      = 1u << 6,   /* a write waiting for the WAL gate */
    bc_cf_rearm     = 1u << 7,   /* the multishot poll ended; re-arm after batch */
    bc_cf_rq        = 1u << 8,   /* on the read queue */
    bc_cf_recving   = 1u << 9,   /* a read is in flight */
};

struct bc_conn {
    uint32_t gen;              /* generation, incremented on reuse */
    uint16_t flags;
    uint16_t ring;             /* ring index */
    uint32_t slot;             /* direct-descriptor slot within the ring */
    uint32_t last_active;      /* last activity time (seconds) */
    uint8_t *pend;             /* partial / pipelined input (rare path) */
    uint32_t pend_len, pend_cap;
    struct bc_arena *send_arena;   /* arena owning an in-flight send */
};

struct bc_conn_pool {
    struct bc_conn *conns;
    unsigned nslots;
    unsigned open;
};

static inline struct bc_conn *bc_conn_at(struct bc_conn_pool *pool, unsigned slot) {
    return &pool->conns[slot];
}

static inline int bc_conn_is_open(const struct bc_conn *c) {
    return (c->flags & bc_cf_open) != 0;
}

static inline void bc_conn_mark_open(struct bc_conn *c, unsigned ring,
                                     unsigned slot) {
    c->flags = (uint16_t)bc_cf_open;
    c->ring = (uint16_t)ring;
    c->slot = slot;
    c->pend_len = 0;
    c->send_arena = NULL;
}

static inline void bc_conn_close_begin(struct bc_conn *c) {
    c->flags |= (uint16_t)bc_cf_closing;
}

static inline void bc_conn_free(struct bc_conn *c, struct bc_conn_pool *pool) {
    c->gen++;
    c->flags = 0;
    if (c->pend) { free(c->pend); c->pend = NULL; }
    c->pend_len = c->pend_cap = 0;
    if (c->send_arena) {
        /* a send that never completed: drop the arena reference */
        c->send_arena = NULL;
    }
}

#endif /* BINARYCORE_CONNECTION_H */
