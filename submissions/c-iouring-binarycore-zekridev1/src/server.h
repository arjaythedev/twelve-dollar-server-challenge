#ifndef BINARYCORE_SERVER_H
#define BINARYCORE_SERVER_H

#include "app.h"
#include "connection.h"
#include "database.h"
#include "io_uring.h"
#include "memory.h"

/* Server configuration read from the environment. */
struct bc_config {
    const char *host;
    const char *port;
    const char *db_path;
    const char *secret;
    int cache_kib;
    int backlog;
    unsigned n_rings;
    unsigned sq_entries;
    unsigned cq_entries;
    unsigned nslots;
    unsigned nbufs;
    unsigned buf_size;
    unsigned max_conns;
};

/* Read configuration from environment variables (SQLITE_PATH, JWT_SECRET,
 * HOST, PORT, and tuning knobs). Returns 0 on success, -1 if required vars
 * are missing. */
int bc_config_read(struct bc_config *cfg);

/* The server: one event-loop thread, io_uring, group commit per batch. */
struct bc_server {
    struct bc_config cfg;
    struct bc_uring uring;
    struct bc_conn_pool *conn_pools;   /* one per ring */
    struct bc_app app;
    struct bc_arena_pool arena_pool;
    struct bc_arena *cur_arena;
    /* per-batch lists */
    struct { struct bc_conn *c; uint32_t gen; } *replies;
    unsigned nrep, rep_cap;
    struct { struct bc_conn *c; uint32_t gen; } *ready;
    unsigned nready, ready_cap;
    struct { struct bc_conn *c; uint32_t gen; } *rq;
    unsigned nrq, rq_cap, rq_head;
    unsigned answers_left;
    /* stats */
    uint64_t batches, cqes, reqs_sent, closes, accepts;
    uint64_t max_turn_ns, max_commit_ns;
    /* time */
    uint32_t now_s;
    int writes_ok;
    int running;
};

int bc_server_init(struct bc_server *s, const struct bc_config *cfg);
void bc_server_run(struct bc_server *s);
void bc_server_cleanup(struct bc_server *s);

#endif /* BINARYCORE_SERVER_H */
