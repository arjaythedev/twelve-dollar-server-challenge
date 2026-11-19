// The SQLite shim. The server's loop is Go; SQLite is C. Crossing between the two costs
// about 50-100 ns per call, so each request crosses ONCE: one function per endpoint, which
// does its reads or its write to completion.
//
// This file and db.c are shared, byte for byte, with the Rust entry.
//
// db_feed and db_post write finished JSON into the buffer they are given, which is the
// response buffer itself (see "Writing the JSON" in db.c).
#ifndef DB_H
#define DB_H

#include <stdint.h>

// Opens the database and prepares every statement. Returns 0, or -1 with db_error() set.
int db_open(const char *path);
const char *db_error(void);

// Group commit. The loop calls db_begin() before the first database access of a pass and
// db_commit() once at the end, so every write that arrived in that pass shares ONE commit.
// Rule 6 is kept by the caller: no response from the pass is sent until db_commit() returns 0.
// On failure db_commit() rolls back and returns -1.
int db_begin(void);
int db_commit(void);

// Checkpointing runs on a background thread started by db_open (see db.c). These numbers
// are for logging: WAL size in pages now, the largest it has been, and checkpoint runs so far.
void db_stats(int *wal_pages, int *wal_max, int *checkpoints);

// The write hold. A checkpoint can only finish, and the WAL only start over, if no commit
// lands while it runs. When the WAL is large the loop calls db_urgent(1), stops committing
// writes (reads continue), and waits until db_backfills() has gone up: that counts the
// checkpoints that copied the WHOLE WAL. Then it calls db_urgent(0) and resumes writes.
void db_urgent(int on);
int db_hold_wanted(void); // the early copying has caught up (or the WAL is very large): hold now
int db_backfills(void);

// 1 if reads go straight to the B-trees, 0 if they use the SQL statements (see db.c).
int db_reads_direct(void);

// The block of memory given to SQLite's page cache: its size and how much of it is on 2 MB
// pages, both in kB (0 and 0 if the block could not be set up). For the startup log.
void db_page_memory(long *block_kb, long *huge_kb);

// The program text that was moved onto anonymous memory at startup: its size and how much of
// it is on 2 MB pages, both in kB. Returns "" if it was moved, otherwise why it was not.
const char *db_text_memory(long *moved_kb, long *huge_kb);

// 1 if `SELECT 1` works.
int db_health(void);

// The 20 newest posts as {"posts":[...]}. Returns bytes written to out, or -1 (also if it
// does not fit).
int db_feed(uint8_t *out, int cap);

// One post as {"post":{...}}. Returns bytes written, 0 if there is no such post, or -1.
int db_post(int64_t id, uint8_t *out, int cap);

// s as a JSON string with its quotes. Returns bytes written, or -1 if it does not fit; it
// asks for up to 26 bytes more room than the result needs.
// (Used by db_feed and db_post; exported so a test can hold it to the Go version.)
int db_json_string(const uint8_t *s, int n, uint8_t *out, int cap);
int db_json_int(int64_t v, uint8_t *out, int cap);

// 1 if the post exists, 0 if not, -1 on error.
int db_post_exists(int64_t id);

// Inserts a like. 1 = newly liked, 0 = was already liked, -1 = error.
int db_like(int64_t user_id, int64_t post_id);

// Inserts a post. Writes the new id and its created_at (24 bytes, no terminator) and returns 0,
// or -1 on error. The row is durable only after the pass's db_commit().
int db_create(int64_t user_id, const char *body, int body_len, int64_t *id, char *created_at);

#endif
