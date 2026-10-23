#pragma once
// SQLite layer: one NOMUTEX connection, persistent prepared statements,
// WAL + group-commit-friendly pragmas, WAL-hook checkpointing.
#include <sqlite3.h>
#include <stddef.h>

typedef struct {
  sqlite3 *db;
  sqlite3_stmt *st_health, *st_feed, *st_post, *st_create, *st_sel_ts,
               *st_like, *st_exists,
               *st_begin, *st_commit, *st_rollback;
  int wal_frames;   // frames since last checkpoint (via WAL hook)
  unsigned long long last_ckpt_ms; // last checkpoint attempt (pacing)
  int in_txn;       // group-commit transaction currently open
  int batch_failed; // a mid-batch rollback poisoned this batch (owner clears per iteration)
} db_t;

// Open path with all pragmas. 0 ok, -1 fail (reason on stderr).
int db_open(db_t *d, const char *path);
// Group-commit boundaries. 0 ok, -1 fail.
int db_begin(db_t *d);
int db_commit(db_t *d);
int db_rollback(db_t *d);
// Run a PASSIVE checkpoint if frames >= threshold. Safe to call any time.
void db_checkpoint_maybe(db_t *d);
void db_close(db_t *d);
