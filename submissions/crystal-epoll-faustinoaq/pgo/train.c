/* PGO training for the SQLite build. It runs the server's statements against a small
 * synthetic database it creates itself (same schema, generated rows: none of the
 * challenge seed). Only the profile is kept; this program is not part of the server. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

static sqlite3 *db;

static void ex(const char *sql) {
  char *err = 0;
  if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) { fprintf(stderr, "%s: %s\n", sql, err); exit(1); }
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *st;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, 0) != SQLITE_OK) {
    fprintf(stderr, "%s: %s\n", sql, sqlite3_errmsg(db)); exit(1);
  }
  return st;
}

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)rs; }

static long long drain(sqlite3_stmt *st) {
  long long acc = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    int n = sqlite3_column_count(st);
    for (int i = 0; i < n; i++) {
      if (sqlite3_column_type(st, i) == SQLITE_INTEGER) acc += sqlite3_column_int64(st, i);
      else acc += sqlite3_column_bytes(st, i) + (sqlite3_column_text(st, i) ? 1 : 0);
    }
  }
  sqlite3_reset(st);
  return acc;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "train.db";
  remove(path);
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0) != SQLITE_OK) return 1;
  ex("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON;");
  ex("CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
     " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
     "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
     " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
     " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
     "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
     " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
     "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
     "CREATE INDEX posts_user_id_idx ON posts (user_id);"
     "CREATE INDEX likes_post_id_idx ON likes (post_id);");
  /* synthetic rows: 5k users, 40k posts with increasing timestamps, ~160k likes */
  ex("BEGIN");
  ex("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 5000)"
     " INSERT INTO users (id, username, created_at) SELECT i, 'user_' || i, '2025-01-01T00:00:00.000Z' FROM n");
  ex("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 40000)"
     " INSERT INTO posts (id, user_id, body, created_at) SELECT i, 1 + (i * 7919) % 5000,"
     " 'synthetic body ' || i || ' with \"quotes\" and words to escape and copy around',"
     " strftime('%Y-%m-%dT%H:%M:%fZ', '2025-01-01', '+' || (i * 13) || ' seconds') FROM n");
  ex("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 160000)"
     " INSERT OR IGNORE INTO likes (user_id, post_id, created_at) SELECT 1 + (i * 104729) % 5000,"
     " 1 + (i * 15485863) % 40000, '2025-06-01T00:00:00.000Z' FROM n");
  ex("COMMIT; ANALYZE;");
  ex("PRAGMA locking_mode = EXCLUSIVE; PRAGMA mmap_size = 268435456; PRAGMA wal_autocheckpoint = 4000;");

  const char *post = "SELECT p.id, p.body, p.created_at, u.username, "
                     "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
                     "FROM posts p JOIN users u ON u.id = p.user_id ";
  char buf[512];
  snprintf(buf, sizeof buf, "%sORDER BY p.created_at DESC, p.id DESC LIMIT 20", post);
  sqlite3_stmt *feed = prep(buf);
  snprintf(buf, sizeof buf, "%sWHERE p.id = ?", post);
  sqlite3_stmt *one = prep(buf);
  sqlite3_stmt *ids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
  sqlite3_stmt *rows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id "
                            "WHERE p.id BETWEEN ?1 AND ?2");
  sqlite3_stmt *lk = prep("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id");
  sqlite3_stmt *ins_post = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
  sqlite3_stmt *ins_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING");
  sqlite3_stmt *b = prep("BEGIN"), *c = prep("COMMIT");

  long long acc = 0;
  for (int batch = 0; batch < 6000; batch++) {
    sqlite3_step(b); sqlite3_reset(b);
    for (int r = 0; r < 24; r++) {
      unsigned x = rnd() % 100;
      if (x < 46) {
        long long lo = 1LL << 62, hi = 0, v;
        while (sqlite3_step(ids) == SQLITE_ROW) { v = sqlite3_column_int64(ids, 0); if (v < lo) lo = v; if (v > hi) hi = v; }
        sqlite3_reset(ids);
        sqlite3_bind_int64(lk, 1, lo); sqlite3_bind_int64(lk, 2, hi); acc += drain(lk);
        sqlite3_bind_int64(rows, 1, lo); sqlite3_bind_int64(rows, 2, hi); acc += drain(rows);
        if ((x & 7) == 0) acc += drain(feed);
      } else if (x < 92) {
        sqlite3_bind_int64(one, 1, 1 + rnd() % 41000); acc += drain(one);
      } else if (x < 99) {
        sqlite3_bind_int64(ins_like, 1, 1 + rnd() % 5000);
        sqlite3_bind_int64(ins_like, 2, 1 + rnd() % 42000); /* some miss: FK failure path */
        sqlite3_step(ins_like); acc += sqlite3_changes(db); sqlite3_reset(ins_like);
      } else {
        snprintf(buf, sizeof buf, "training post %u says hi", rnd());
        sqlite3_bind_int64(ins_post, 1, 1 + rnd() % 5000);
        sqlite3_bind_text(ins_post, 2, buf, -1, SQLITE_STATIC);
        acc += drain(ins_post);
      }
    }
    sqlite3_step(c); sqlite3_reset(c);
  }
  printf("pgo training done (%lld)\n", acc);
  sqlite3_close(db);
  char side[1024];
  remove(path);
  snprintf(side, sizeof side, "%s-wal", path); remove(side);
  snprintf(side, sizeof side, "%s-shm", path); remove(side);
  return 0;
}
