/* db.h — the SQLite layer of lean-c-iouring-loafecho.
 *
 * One connection, one thread (the request thread). db.c #includes the stock
 * SQLite amalgamation (sqlite3.c) so the B-tree read plans can use SQLite's
 * internal cursor API in the same translation unit. SQLite itself is never
 * patched.
 *
 * Plans. Two ways to serve GET /feed and GET /posts/:id:
 *   btree — walk the posts table B-tree from its last row (feed: 20 rows
 *           backwards; post: rowid seek), look up the author by rowid in
 *           users, count likes in likes_post_id_idx (feed: one range walk
 *           over the 20 ids' entries; post: one seek). Correct only under the
 *           invariant Inv (dense ids 1..n, created_at non-decreasing in id,
 *           every author exists) — proved in spec/ (Lean 4).
 *   sql   — the reference SQL from SPEC.md.
 * checkInv() runs once at open. insertOk() runs after every post insert.
 * The data_version is checked at the start of every batch. Any failure
 * switches the connection to plan=sql for the rest of its life (never back).
 *
 * Page cache. Before the first sqlite3_initialize(), db.c installs a thin
 * SQLITE_CONFIG_PCACHE2 wrapper that forwards every call to SQLite's built-in
 * page cache but skips xTruncate when a per-cache bitmap of possibly-cached
 * keys shows no page at or above the limit. That avoids the full hash-table
 * walk pcache1 does on every COMMIT after a B-tree balance rekeys a page
 * through the lock-page number (which leaves pcache1's iMaxKey at 262145).
 *
 * Responses are written as compact JSON bodies into caller buffers.
 * Every function returns 0 on success unless documented otherwise; -1 is an
 * internal error (the server answers 500).
 */
#ifndef TWELVE_DB_H
#define TWELVE_DB_H
#include <stddef.h>
#include <stdint.h>

typedef struct db db_t;

/* Open $SQLITE_PATH. Sets journal_mode=WAL, synchronous=NORMAL,
 * mmap_size=0 with a 256 MiB page cache (cache_size=-262144),
 * wal_autocheckpoint=0 (db_maybe_checkpoint() does it), prepares every
 * statement, runs checkInv. On failure returns NULL and writes a
 * message into err. */
db_t *db_open(const char *path, char *err, size_t errlen);
void  db_close(db_t *db);

/* "btree" or "sql" — the plan currently in force. */
const char *db_plan(const db_t *db);
/* 1 if checkInv passed at open, else 0. */
int db_invariant_ok(const db_t *db);
/* Printable one-line reason for the current plan, e.g.
 * "invariant=ok plan=btree" or "invariant=violated plan=sql reason=gap-at-id-123457".
 * The server prints this line to stdout once after open, and again whenever
 * the plan changes. */
const char *db_plan_line(const db_t *db);

/* --- batching -------------------------------------------------------------
 * The server wraps every completion batch:
 *   db_batch_begin(db);  ...reads/writes...  rc = db_batch_commit(db);
 * db_batch_begin runs a deferred BEGIN and takes ONE read snapshot for the
 * whole batch, checking its data_version (a change → plan=sql). B-tree reads
 * in the batch share that snapshot and one set of cursors (opened on the
 * first read, closed by db_batch_commit). The first write inside the batch
 * upgrades the transaction to a write transaction. db_batch_commit always
 * ends the transaction: COMMIT (0 = committed or nothing to commit), or on a
 * failed write ROLLBACK (-1: every write response of this batch must become
 * a 500). Write responses may only be sent after db_batch_commit returned 0.
 * Read responses may be sent at once. */
int db_batch_begin(db_t *db);
int db_batch_commit(db_t *db);

/* --- reads -------------------------------------------------------------------
 * Write the whole JSON body into buf (no trailing newline) and return its
 * length. Return -1 on internal error or if cap is too small.
 * db_post_json returns 0 when the post does not exist (buf untouched). */
int db_feed_json(db_t *db, char *buf, size_t cap);                 /* {"posts":[...]} */
int db_post_json(db_t *db, int64_t id, char *buf, size_t cap);     /* {"post":{...}} */
/* SELECT 1. 0 ok, -1 failed. */
int db_health(db_t *db);

/* --- writes (inside a batch) ---------------------------------------------------
 * db_insert_post: INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id,
 * created_at; then insertOk (new id == previous max id + 1, created_at >= the
 * previous last created_at, author exists). Writes {"post":{...}} with
 * like_count 0 and the token username as author. body is already trimmed and
 * validated (1..500 characters). Returns length, -1 on error.
 * db_like: INSERT INTO likes ... ON CONFLICT DO NOTHING. Returns 1 if the post
 * exists (already_liked set from changes()==0), 0 if the post does not exist,
 * -1 on error. */
int db_insert_post(db_t *db, int64_t user_id, const char *username, size_t ulen,
                   const char *body, size_t blen, char *buf, size_t cap);
int db_like(db_t *db, int64_t user_id, int64_t post_id, int *already_liked);

/* --- maintenance ---------------------------------------------------------------
 * Call between batches when idle-ish. Runs a PASSIVE checkpoint when the WAL
 * has more than `min_frames` frames (0 = always). Returns 0. */
int db_maybe_checkpoint(db_t *db, int min_frames);

/* --- selfcheck -----------------------------------------------------------------
 * Opens path, serves the feed, posts 1, 250000, 500000 and 10,000 random ids
 * (deterministic from seed) with BOTH plans, compares the JSON bytes, prints
 * one line per mismatch and finally "selfcheck mismatches=N". Returns N
 * (or -1 if the db cannot be opened or the invariant does not hold, after
 * printing why). */
int db_selfcheck(const char *path, unsigned seed);

/* --- PGO training (build.sh only; never called while serving) -------------------
 * Creates a fresh database at path (in a new, empty directory) from an
 * embedded copy of the challenge schema.sql and fills it in Inv order:
 * nusers users, nposts posts (dense ids, created_at strictly increasing
 * through 2025, existing authors), about nlikes likes, then ANALYZE.
 * Used by `bin/server --pgo-train`. 0 ok, -1 error (message in err). */
int db_pgo_make(const char *path, int nusers, int nposts, int nlikes, char *err, size_t errlen);

/* --- page-cache wrapper counters (tests) -----------------------------------------
 * xTruncate calls skipped as no-ops / forwarded to SQLite's built-in cache,
 * process-wide since start. */
void db_pcache_stats(unsigned long *skipped, unsigned long *forwarded);

#endif
