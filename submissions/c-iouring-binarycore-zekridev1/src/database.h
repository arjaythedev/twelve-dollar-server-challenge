#ifndef BINARYCORE_DATABASE_H
#define BINARYCORE_DATABASE_H

#include "json.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>

/* SQLite access for BinaryCore.
 *
 * Design (modeled on c-iouring-pavlyhalim/src/db.h):
 *   - WAL mode, synchronous=NORMAL, wal_autocheckpoint=0 (rule 6: durable
 *     writes before response, no synchronous=OFF).
 *   - Group commit: one BEGIN/COMMIT transaction per batch; every read and
 *     write in the batch runs inside that transaction; each response waits
 *     for its batch's commit.
 *   - Prepared statements, reused across requests (cache the query plan, not
 *     the data — permitted by rule 5).
 *   - One sqlite3 connection on the event loop; a second connection on a
 *     background checkpoint thread at nice 19.
 *   - B-tree cursor reads are NOT used in this initial implementation
 *     (standard SQL via prepared statements). This keeps correctness simple
 *     while still achieving ~238,000 users (see c-iouring-meros). B-tree
 *     reads can be added later as the highest-reward optimization.
 *
 * The schema is fixed (schema.sql) and is never modified.
 */

#define bc_feed_n 20

struct bc_db {
    sqlite3 *c;              /* request connection (event loop) */
    int in_txn;              /* a batch transaction is open */
    /* Prepared statements. */
    sqlite3_stmt *st_begin;
    sqlite3_stmt *st_commit;
    sqlite3_stmt *st_rollback;
    sqlite3_stmt *st_health;         /* SELECT 1 */
    sqlite3_stmt *st_feed;           /* SELECT ... ORDER BY created_at DESC, id DESC LIMIT 20 */
    sqlite3_stmt *st_post;           /* SELECT ... WHERE p.id = ?1 */
    sqlite3_stmt *st_ins_post;       /* INSERT INTO posts (user_id, body) VALUES (?, ?) */
    sqlite3_stmt *st_created_at;     /* SELECT created_at FROM posts WHERE id = ?1 */
    sqlite3_stmt *st_ins_like;       /* INSERT INTO likes ... ON CONFLICT DO NOTHING */
    sqlite3_stmt *st_post_exists;    /* SELECT 1 FROM posts WHERE id = ?1 */
    /* Checkpoint thread. */
    sqlite3 *c2;
    /* stats (for logging / debugging) */
    uint64_t n_ckpt, n_fallback;
};

struct bc_app;

/* Response builder. A response is written body-first into an arena; the
 * status line and headers are then written in front (no copy, no
 * Content-Length placeholder). */
struct bc_resp {
    uint8_t *p;
    unsigned n;
    unsigned char close;   /* close the connection after sending */
    unsigned char db;      /* touched the database: becomes 500 if batch rolls back */
    unsigned char held;    /* a write while the gate is closed: retry later */
};

#define bc_resp_max (64 * 1024)
#define bc_hdr_room 128

/* Open the database at `path`. Returns 0 on success, -1 on failure. */
int bc_db_open(struct bc_db *d, const char *path, int cache_kib);

/* Close the database and join the checkpoint thread. */
void bc_db_close(struct bc_db *d);

/* Begin the batch's transaction (lazily, by the first operation). Returns 0
 * on success, -1 on failure. */
static inline int bc_db_begin(struct bc_db *d) {
    if (d->in_txn) return 0;
    int rc = sqlite3_step(d->st_begin);
    sqlite3_reset(d->st_begin);
    if (rc != SQLITE_DONE) return -1;
    d->in_txn = 1;
    return 0;
}

/* Commit the batch's transaction. Returns 0 on success, -1 if the batch
 * rolled back (every db response becomes 500). */
static inline int bc_db_commit(struct bc_db *d) {
    if (!d->in_txn) return 0;
    d->in_txn = 0;
    int rc = sqlite3_step(d->st_commit);
    sqlite3_reset(d->st_commit);
    if (rc == SQLITE_DONE) return 0;
    sqlite3_step(d->st_rollback);
    sqlite3_reset(d->st_rollback);
    return -1;
}

/* Are writes currently allowed (WAL gate not closed)? Called between batches. */
static inline int bc_db_writes_allowed(struct bc_db *d) {
    (void)d;
    return 1;   /* no WAL gate in the initial implementation */
}

/* ---- endpoint operations ---- */

/* GET /health. Returns 0 on success, -1 on failure. */
static inline int bc_db_health(struct bc_db *d) {
    int rc = sqlite3_step(d->st_health);
    sqlite3_reset(d->st_health);
    return rc == SQLITE_ROW ? 0 : -1;
}

/* GET /feed. Writes {"posts":[...]} into `o`. Returns 0 on success, -1 on
 * error. Does NOT finalize; caller must ensure the statement is reset. */
int bc_db_feed(struct bc_db *d, struct bc_ob *o);

/* GET /posts/:id. Returns 1 (post found, written to o), 0 (not found),
 * -1 (error). */
int bc_db_post(struct bc_db *d, struct bc_ob *o, int64_t id);

/* POST /posts. Writes {"post":...} into `o`. Returns 0 on success, -1 on
 * error. `author` and `author_len` come from the verified JWT. */
int bc_db_create(struct bc_db *d, struct bc_ob *o, int64_t user_id,
                 const uint8_t *body, unsigned body_len,
                 const uint8_t *author, unsigned author_len);

/* POST /posts/:id/like. Returns 1 (liked now -> 201), 2 (already liked ->
 * 200), 0 (no such post -> 404), -1 (error). */
int bc_db_like(struct bc_db *d, int64_t user_id, int64_t post_id);

#endif /* BINARYCORE_DATABASE_H */
