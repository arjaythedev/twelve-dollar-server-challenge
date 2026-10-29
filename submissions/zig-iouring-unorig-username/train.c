// Profile training for SQLite (build.sh): runs the server's statements in the benchmark mix against a
// synthetic database it creates (no challenge data), so gcc optimizes SQLite for the real hot paths.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sqlite3.h"
static sqlite3 *d;
static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3(d, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) exit(1);
  return s;
}
static void run(sqlite3_stmt *s) {  // steps to the end, reading every column like the server does
  while (sqlite3_step(s) == SQLITE_ROW)
    for (int i = 0; i < sqlite3_column_count(s); i++) i ? (void)sqlite3_column_text(s, i) : (void)sqlite3_column_int64(s, 0);
  sqlite3_reset(s);
}
int main(int argc, char **argv) {
  sqlite3_initialize(); unlink(argv[1]);
  if (sqlite3_open(argv[1], &d) != SQLITE_OK || sqlite3_exec(d,
      "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000;"
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
      "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC); CREATE INDEX posts_user_id_idx ON posts (user_id); CREATE INDEX likes_post_id_idx ON likes (post_id);"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000) INSERT INTO users (id, username) SELECT i, 'user' || i FROM n;"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000) INSERT INTO posts (user_id, body, created_at)"
      "  SELECT 1 + i % 20000, 'synthetic post ' || i || ' \"quoted\" text', strftime('%Y-%m-%dT%H:%M:%fZ', 1700000000 + i * 60, 'unixepoch') FROM n;"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 600000) INSERT OR IGNORE INTO likes (user_id, post_id)"
      "  SELECT 1 + abs(random()) % 20000, 1 + abs(random()) % 200000 FROM n;", NULL, NULL, NULL) != SQLITE_OK) return 1;
  sqlite3_stmt *fids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20"),
    *flikes = prep("SELECT post_id FROM likes WHERE post_id BETWEEN ? AND ?"),
    *frows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ?"),
    *post = prep("SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?"),
    *ins = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"),
    *like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING"),
    *begin = prep("BEGIN"), *commit = prep("COMMIT");
  for (int i = 1; i <= 300000; i++) {
    int k = i % 217, id = 1 + (i * 7919) % 200000;
    if (i % 64 == 1) run(begin);
    if (k < 100) {
      sqlite3_int64 lo = -1, hi = 0, v;
      while (sqlite3_step(fids) == SQLITE_ROW) { v = sqlite3_column_int64(fids, 0); if (lo < 0 || v < lo) lo = v; if (v > hi) hi = v; }
      sqlite3_reset(fids);
      sqlite3_bind_int64(flikes, 1, lo); sqlite3_bind_int64(flikes, 2, hi); run(flikes);
      sqlite3_bind_int64(frows, 1, lo); sqlite3_bind_int64(frows, 2, hi); run(frows);
    } else if (k < 200) { sqlite3_bind_int64(post, 1, id); run(post); }
    else if (k < 215) { sqlite3_bind_int64(like, 1, 1 + i % 20000); sqlite3_bind_int64(like, 2, id); run(like); }
    else { char b[64]; int n = snprintf(b, sizeof b, "training post %d", i);
      sqlite3_bind_int64(ins, 1, 1 + i % 20000); sqlite3_bind_text(ins, 2, b, n, SQLITE_TRANSIENT); run(ins); }
    if (i % 64 == 0) run(commit);
  }
  return 0;
}
