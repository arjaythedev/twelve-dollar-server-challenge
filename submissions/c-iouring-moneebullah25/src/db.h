/* SQLite access: one connection on the event loop thread for all requests,
 * one more on a SCHED_IDLE thread that only runs passive checkpoints.
 *
 * Rule 2: in-process SQLite only. Rule 5: every call reads from SQLite; the
 * only reuse is of prepared statements. Rule 6: WAL + synchronous=NORMAL, and
 * db_write_commit() returns before any 201 leaves the process. */
#ifndef DB_H
#define DB_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

struct db;

struct db_stats {
    uint64_t checkpoints;
    uint64_t checkpoint_ns_total;
    uint64_t checkpoint_ns_max;
    uint64_t holds;
    uint64_t hold_ns_total;
    uint64_t hold_ns_max;
    uint64_t hold_timeouts;
    uint64_t feed_fallbacks;
    uint32_t wal_frames;
};

struct db *db_open(const struct config *cfg, char *err, size_t err_cap);
void db_close(struct db *db);

/* Starts the checkpoint thread. Returns 0 on success. */
int db_checkpointer_start(struct db *db);

/* SELECT 1; returns 0 or fills err with the SQLite message. */
int db_health(struct db *db, char *err, size_t err_cap);

/* One deferred read transaction shared by the reads of a batch. Both are
 * no-ops when the shared read transaction is disabled. */
void db_read_begin(struct db *db);
void db_read_end(struct db *db);

/* Writes the feed body {"posts":[...]}; returns its length, or 0 on error. */
size_t db_feed_json(struct db *db, char *out, size_t cap);
/* Writes {"post":{...}}; returns the length, 0 when the post does not exist,
 * or (size_t)-1 on error. */
size_t db_post_json(struct db *db, int64_t id, char *out, size_t cap);

/* Write transactions: begin, any number of operations, then commit. A failed
 * operation leaves the transaction state undefined; callers check
 * db_txn_active() and treat every write of the batch as failed if it is gone. */
int db_write_begin(struct db *db);
int db_write_commit(struct db *db);
void db_write_rollback(struct db *db);
int db_txn_active(struct db *db);

/* Writes are held while the WAL is being restarted. Returns 1 if a write
 * should wait; a hold that lasts longer than the timeout is released. */
int db_writes_held(struct db *db);

/* Inserts a post; fills id and created_at (NUL-terminated). Returns 0 or -1. */
int db_create_post(struct db *db, int64_t user_id, const char *body, size_t body_len, int64_t *id_out,
                   char created_at[32]);
/* Returns 2 = liked now, 1 = already liked, 0 = post not found, -1 = error. */
int db_like(struct db *db, int64_t user_id, int64_t post_id);

/* The post object as the SPEC lays it out, with escaping. Returns the length or 0 if it does not fit. */
size_t db_write_post_json(char *out, size_t cap, int64_t id, const char *body, size_t body_len, const char *created_at,
                          size_t created_len, const char *author, size_t author_len, int64_t like_count);

/* Warms the page cache with the queries the load will run. */
void db_warm(struct db *db);
void db_get_stats(struct db *db, struct db_stats *out);

#endif
