//go:build train

// Build time only (see build.sh and train.go): the scratch database the training run works
// on, and the call that writes the profile. Not part of the server.
#include "sqlite/sqlite3.h"

void __gcov_dump(void); // from gcc's profiling runtime, linked into the training binary only

// The schema is the challenge's schema.sql. The rows have the seed's shape (50,000 users,
// 500,000 posts in time order, about 2 million likes, each post liked by different users);
// the content is made up here.
static const char *const make_sql =
    "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA cache_size=-65536;"
    "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
    " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
    "CREATE INDEX posts_user_id_idx ON posts (user_id);"
    "CREATE INDEX likes_post_id_idx ON likes (post_id);"
    "BEGIN;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 50000)"
    " INSERT INTO users SELECT i, 'quiet_' || substr('otterheronfinchbadgermarten', 1 + i % 22, 6) || '_' || i,"
    " strftime('%Y-%m-%dT%H:%M:%fZ', 1700000000 + i * 600.007, 'unixepoch') FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 500000)"
    " INSERT INTO posts SELECT i, 1 + (i * 7919) % 50000,"
    " substr('Went for a walk by the river this morning and the light was something else. Has anyone tried the new place"
    " on the corner? Still thinking about that talk from last week, so many good ideas in it. Rain again, so it is tea"
    " and a book for me today. Finally fixed the bug that has been following me around since Monday, it was a missing"
    " comma all along!', 1 + i % 40, 30 + (i * 31) % 270),"
    " strftime('%Y-%m-%dT%H:%M:%fZ', 1731000000 + i * 63.011, 'unixepoch') FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 500000),"
    " k(j) AS (VALUES (0),(1),(2),(3),(4),(5),(6),(7))"
    " INSERT INTO likes SELECT 1 + (i * 7919 + j * 4729) % 50000, i,"
    " strftime('%Y-%m-%dT%H:%M:%fZ', 1731000000 + i * 63.011 + j * 3600, 'unixepoch') FROM n, k WHERE j < i % 9;"
    "COMMIT; ANALYZE; PRAGMA journal_mode=WAL;";

int train_make_db(const char *path) {
  sqlite3 *d;
  if (sqlite3_open(path, &d) != SQLITE_OK) return -1;
  int rc = sqlite3_exec(d, make_sql, 0, 0, 0);
  sqlite3_close(d);
  return rc == SQLITE_OK ? 0 : -1;
}

void train_write_profile(void) { __gcov_dump(); }
