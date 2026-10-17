#include "db.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdint.h>

#define CKPT_FRAMES 4000
/* Checkpoint at most once per second: under write-heavy load the frame count
   is always above threshold, and attempting a checkpoint after every commit
   burns CPU + fsync storms for no gain (measured: 70%+ CPU at 150k users).
   The 64MB journal_size_limit still bounds WAL size between checkpoints. */

static int exec_all(db_t *d, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(d->db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "db pragma failed: %s: %s\n", err ? err : "?", sql);
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

static int prep(db_t *d, sqlite3_stmt **dst, const char *sql) {
  int rc = sqlite3_prepare_v3(d->db, sql, -1, SQLITE_PREPARE_PERSISTENT, dst, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "db prepare failed (%d): %s\n%s\n", rc, sqlite3_errmsg(d->db), sql);
    return -1;
  }
  return 0;
}

static int wal_hook(void *arg, sqlite3 *db, const char *name, int frames) {
  (void)db; (void)name;
  ((db_t *)arg)->wal_frames = frames;
  return SQLITE_OK;
}

int db_open(db_t *d, const char *path) {
  memset(d, 0, sizeof *d);
  int rc = sqlite3_open_v2(path, &d->db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "db open failed: %s\n", path);
    if (d->db) sqlite3_close(d->db);
    d->db = NULL;
    return -1;
  }
  sqlite3_busy_timeout(d->db, 5000);
  if (exec_all(d,
      "PRAGMA locking_mode=EXCLUSIVE;"
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=NORMAL;"
      "PRAGMA mmap_size=1073741824;"
      "PRAGMA cache_size=-8192;"
      "PRAGMA temp_store=MEMORY;"
      "PRAGMA foreign_keys=ON;"
      "PRAGMA journal_size_limit=67108864;"
      "PRAGMA wal_autocheckpoint=0;") != 0) return -1;
  sqlite3_wal_hook(d->db, wal_hook, d);

  if (prep(d, &d->st_health, "SELECT 1") != 0) return -1;
  if (prep(d, &d->st_feed,
      "SELECT p.id, p.body, p.created_at, u.username,"
      " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
      " FROM posts p JOIN users u ON u.id = p.user_id"
      " ORDER BY p.created_at DESC, p.id DESC LIMIT 20") != 0) return -1;
  /* Single post via LEFT JOIN + GROUP BY: one probe per table instead of a
     correlated count(*) setup per execution (~2x). Same bytes: bare columns
     are functionally dependent on p.id, COUNT form matches count(*). */
  if (prep(d, &d->st_post,
      "SELECT p.id, p.body, p.created_at, u.username, COUNT(l.post_id)"
      " FROM posts p JOIN users u ON u.id = p.user_id"
      " LEFT JOIN likes l ON l.post_id = p.id WHERE p.id = ?1 GROUP BY p.id") != 0) return -1;
  if (prep(d, &d->st_create, "INSERT INTO posts (user_id, body) VALUES (?1, ?2)") != 0) return -1;
  if (prep(d, &d->st_sel_ts, "SELECT created_at FROM posts WHERE id = ?1") != 0) return -1;
  if (prep(d, &d->st_like,
      "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2)"
      " ON CONFLICT (user_id, post_id) DO NOTHING") != 0) return -1;
  if (prep(d, &d->st_exists, "SELECT 1 FROM posts WHERE id = ?1") != 0) return -1;
  if (prep(d, &d->st_begin, "BEGIN IMMEDIATE") != 0) return -1;
  if (prep(d, &d->st_commit, "COMMIT") != 0) return -1;
  if (prep(d, &d->st_rollback, "ROLLBACK") != 0) return -1;
  return 0;
}

static int step_done(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  sqlite3_clear_bindings(s);
  return rc;
}

int db_begin(db_t *d) {
  if (d->in_txn) return 0;
  if (step_done(d->st_begin) != SQLITE_DONE) return -1;
  d->in_txn = 1;
  return 0;
}

int db_commit(db_t *d) {
  if (!d->in_txn) return 0;
  d->in_txn = 0;
  if (step_done(d->st_commit) != SQLITE_DONE) return -1;
  db_checkpoint_maybe(d);
  return 0;
}

int db_rollback(db_t *d) {
  d->in_txn = 0;
  d->batch_failed = 1; // responses built before this rollback must not go out
  step_done(d->st_rollback);
  return 0;
}

void db_checkpoint_maybe(db_t *d) {
  if (d->wal_frames < CKPT_FRAMES) return;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
  if (now_ms - d->last_ckpt_ms < 1000) return;
  d->last_ckpt_ms = now_ms;
  d->wal_frames = 0;
  // PASSIVE: never blocks the loop; restarts the WAL when possible.
  sqlite3_wal_checkpoint_v2(d->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
}

void db_close(db_t *d) {
  if (!d->db) return;
  sqlite3_stmt *all[] = { d->st_health, d->st_feed, d->st_post, d->st_create,
    d->st_sel_ts, d->st_like, d->st_exists,
    d->st_begin, d->st_commit, d->st_rollback };
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) sqlite3_finalize(all[i]);
  sqlite3_close(d->db);
  d->db = NULL;
}
