/*
 * feedbt_train.c: the profile-guided build's training run. It builds a synthetic database in the schema's shape (nothing from
 * the seed is read or shipped), then drives the feed and post plans in the benchmark's mix: 47% GET /feed, 47% GET /posts/:id,
 * 5% likes, 1% new posts. pgo.sh compiles feedbt.c instrumented, runs this, and compiles feedbt.c again with the profile.
 * No profile file is kept in the repository; the profile only exists inside the build directory.
 */
#include "sqlite3.h"
#include "feedbt.h"
#include <stdio.h>
#include <stdlib.h>

static void run(sqlite3 *db, const char *sql) {
  char *err = 0;
  if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) { fprintf(stderr, "train: %s: %s\n", sql, err); exit(1); }
}

int main(int argc, char **argv) {
  sqlite3 *db;
  static char buf[1 << 16];
  if (argc < 2 || sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0)) return 1;
  run(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA locking_mode=EXCLUSIVE; PRAGMA cache_size=-16384");
  run(db,
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
      "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
      "CREATE INDEX posts_user_id_idx ON posts (user_id);"
      "CREATE INDEX likes_post_id_idx ON likes (post_id);"
      "BEGIN;"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000) INSERT INTO users (id, username) SELECT i, 'user_' || i FROM n;"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000) INSERT INTO posts (id, user_id, body, created_at) "
      "SELECT i, 1 + (i * 7919) % 20000, 'Fast tomorrow small bug big droplet number ' || i || ' rarely everything?', strftime('%Y-%m-%dT%H:%M:%fZ', 1735689600 + i * 0.5, 'unixepoch') FROM n;"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 400000) INSERT OR IGNORE INTO likes (user_id, post_id) "
      "SELECT 1 + (i * 104729) % 20000, 1 + (i * 15485863) % 200000 FROM n;"
      "COMMIT; ANALYZE;");
  if (!feedbt_init(db)) { fprintf(stderr, "train: feedbt_init declined the synthetic schema\n"); return 2; }
  srand(1);
  for (long i = 0; i < 400000; i++) {
    int r = rand() % 100;
    long long id = 200000 - rand() % 20;
    if (r < 47) feedbt_feed(buf, sizeof buf);
    else if (r < 94) feedbt_post(id, buf, sizeof buf);
    else if (r < 99) { feedbt_release(); run(db, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (abs(random()) % 20000 + 1, 199990 + abs(random()) % 10)"); }
    else { feedbt_release(); run(db, "INSERT INTO posts (user_id, body) VALUES (7, 'hi from the load test')"); }
    if (i % 20 == 19) feedbt_release();
  }
  feedbt_release();
  sqlite3_close(db);
  return 0;
}
