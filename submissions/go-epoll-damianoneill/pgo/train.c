// Training run for the profile guided SQLite build. build.sh links this against an instrumented SQLite
// and runs it on a throwaway database of synthetic rows, using the server's statements in roughly the
// bench/load.js mix. No challenge data is used. Keep the statements in step with db.go.
//
//   train <database path> <schema SQL>
#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>

#define POSTS 50000
#define POST_SELECT \
  "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) " \
  "FROM posts p JOIN users u ON u.id = p.user_id"

static sqlite3 *db;

static void check(int rc, const char *what) {
  if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
    fprintf(stderr, "train: %s: %s\n", what, sqlite3_errmsg(db));
    exit(1);
  }
}

static sqlite3_stmt *prepare(const char *sql) {
  sqlite3_stmt *s;
  check(sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0), sql);
  return s;
}

// Steps every row and reads every column as text, as the server does.
static void run(sqlite3_stmt *s) {
  int rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    for (int i = 0; i < sqlite3_column_count(s); i++) {
      sqlite3_column_text(s, i);
      sqlite3_column_bytes(s, i);
    }
  }
  check(rc, sqlite3_sql(s));
  sqlite3_reset(s);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: train <database path> <schema SQL>\n");
    return 2;
  }
  check(sqlite3_open(argv[1], &db), "open");
  check(sqlite3_exec(db, "PRAGMA locking_mode = EXCLUSIVE; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;",
                     0, 0, 0), "pragmas");
  check(sqlite3_exec(db, argv[2], 0, 0, 0), "schema");
  check(sqlite3_exec(db,
    "BEGIN;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 5000)"
    "  INSERT INTO users (id, username) SELECT i, 'user_' || i FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 50000)"
    "  INSERT INTO posts (id, user_id, body, created_at)"
    "  SELECT i, 1 + abs(random()) % 5000, 'Synthetic post ' || i || ' ' || substr(hex(randomblob(64)), 1, 20 + abs(random()) % 100),"
    "         strftime('%Y-%m-%dT%H:%M:%fZ', 1735689600 + i * 60, 'unixepoch') FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000)"
    "  INSERT OR IGNORE INTO likes (user_id, post_id) SELECT 1 + abs(random()) % 5000, 1 + abs(random()) % 50000 FROM n;"
    "COMMIT;", 0, 0, 0), "synthetic rows");

  sqlite3_stmt *feed = prepare(POST_SELECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  sqlite3_stmt *post = prepare(POST_SELECT " WHERE p.id = ?");
  sqlite3_stmt *insert = prepare("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
  sqlite3_stmt *like = prepare("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS "
                               "(SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  sqlite3_stmt *exists = prepare("SELECT 1 FROM posts WHERE id = ?");
  sqlite3_stmt *begin = prepare("BEGIN IMMEDIATE");
  sqlite3_stmt *commit = prepare("COMMIT");

  // Each iteration is one user's visit: the feed, a post from it, a 15% like and a 2% new post. A batch
  // of 32 visits shares one transaction, opened at its first write as the server does.
  srand(1);
  int newest = POSTS, in_tx = 0;
  for (int i = 1; i <= 20000; i++) {
    run(feed);
    int id = newest - rand() % 20;
    sqlite3_bind_int64(post, 1, id);
    run(post);
    if (rand() % 100 < 15) {
      if (!in_tx) run(begin), in_tx = 1;
      sqlite3_bind_int64(like, 1, 1 + rand() % 5000);
      sqlite3_bind_int64(like, 2, id);
      run(like);
      if (sqlite3_changes(db) == 0) {
        sqlite3_bind_int64(exists, 1, id);
        run(exists);
      }
    }
    if (rand() % 100 < 2) {
      if (!in_tx) run(begin), in_tx = 1;
      sqlite3_bind_int64(insert, 1, 1 + rand() % 5000);
      sqlite3_bind_text(insert, 2, "A training post with a few words in it.", -1, SQLITE_STATIC);
      run(insert);
      newest++;
    }
    if (i % 32 == 0 && in_tx) run(commit), in_tx = 0;
  }
  if (in_tx) run(commit);
  check(sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, 0, 0), "checkpoint");
  check(sqlite3_close_v2(db), "close");
  return 0;
}
