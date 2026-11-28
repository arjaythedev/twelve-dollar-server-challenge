/* mkdb.c: build-time only. Writes a synthetic training database (the challenge schema, random rows, none of the seed)
 * for the PGO training run in build.sh (from the Graal entry, kotlin-graal-3x3xX3N0N). Linked against build/c/libkn.a (db.o contains SQLite).
 *   mkdb <path> <users> <posts> <likes>
 */
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

static const char *SCHEMA =
  "PRAGMA journal_mode=WAL;"
  "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
  " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
  "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
  " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
  " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
  "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
  " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
  "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
  "CREATE INDEX posts_user_id_idx ON posts (user_id);"
  "CREATE INDEX likes_post_id_idx ON likes (post_id);";

int main(int argc, char **argv) {
  if (argc != 5) { fprintf(stderr, "usage: mkdb <path> <users> <posts> <likes>\n"); return 2; }
  long u = atol(argv[2]), p = atol(argv[3]), l = atol(argv[4]);
  sqlite3 *db;
  if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) return 1;
  char *err = NULL, sql[4096];
  /* Bodies: 1 to ~300 characters with a quote, a backslash and a non-ASCII character now and then (JSON escaping
   * paths). Posts are spread over a year with increasing timestamps; authors skewed to low ids; likes random. */
  snprintf(sql, sizeof sql,
    "%s BEGIN;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<%ld)"
    " INSERT INTO users SELECT i, 'user_'||i, strftime('%%Y-%%m-%%dT%%H:%%M:%%fZ', 1700000000 + i, 'unixepoch') FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<%ld)"
    " INSERT INTO posts SELECT i, 1 + (abs(random()) %% %ld) * (abs(random()) %% 1000) / 1000,"
    " substr(printf('post %%d says \"hi\" \\ caf\xc3\xa9 ', i) || hex(randomblob(150)), 1, 1 + abs(random()) %% 300),"
    " strftime('%%Y-%%m-%%dT%%H:%%M:%%fZ', 1730000000 + i * 600 + (abs(random()) %% 599000) / 1000.0, 'unixepoch') FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<%ld)"
    " INSERT OR IGNORE INTO likes SELECT 1 + abs(random()) %% %ld, %ld - (abs(random()) %% %ld) * (abs(random()) %% 1000) / 1000,"
    " strftime('%%Y-%%m-%%dT%%H:%%M:%%fZ', 1760000000 + i, 'unixepoch') FROM n;"
    "COMMIT; PRAGMA wal_checkpoint(TRUNCATE);",
    SCHEMA, u, p, u, l, u, p, p);
  if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) { fprintf(stderr, "mkdb: %s\n", err); return 1; }
  sqlite3_close(db);
  return 0;
}
