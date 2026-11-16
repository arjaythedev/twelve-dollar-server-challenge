#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sqlite3.h"

typedef long long i64;
typedef uint8_t u8;
typedef uint32_t u32;

int td_open(const char *path, char *err, int n);
void td_close(void);
int td_begin(void);
int td_commit(void);
void *td_cursor(int i);
int td_seek(void *c, i64 id);
int td_first(void *c);
int td_next(void *c);
const u8 *td_payload(void *c, u32 *len);
int td_count_likes(void *c, i64 lo, i64 hi, u32 *n);
int td_insert_post(i64 uid, const char *body, int len, i64 *id, char *created, int *clen);
int td_insert_like(i64 uid, i64 pid);
int td_delete_like(i64 uid, i64 pid);
int td_health(char *err, int n);
int td_feed_sql_step(i64 *id, const u8 **body, int *blen, const u8 **ca, int *clen, const u8 **author, int *alen, i64 *likes);
void td_feed_sql_reset(void);

static int init_scratch_db(const char *path) {
  sqlite3 *db = NULL;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    return -1;
  }
  const char *ddl =
    "PRAGMA journal_mode = WAL;\n"
    "CREATE TABLE users (\n"
    "  id          INTEGER PRIMARY KEY,\n"
    "  username    TEXT NOT NULL UNIQUE,\n"
    "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))\n"
    ");\n"
    "CREATE TABLE posts (\n"
    "  id          INTEGER PRIMARY KEY,\n"
    "  user_id     INTEGER NOT NULL REFERENCES users(id),\n"
    "  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),\n"
    "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))\n"
    ");\n"
    "CREATE TABLE likes (\n"
    "  user_id     INTEGER NOT NULL REFERENCES users(id),\n"
    "  post_id     INTEGER NOT NULL REFERENCES posts(id),\n"
    "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),\n"
    "  PRIMARY KEY (user_id, post_id)\n"
    ");\n"
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);\n"
    "CREATE INDEX posts_user_id_idx       ON posts (user_id);\n"
    "CREATE INDEX likes_post_id_idx       ON likes (post_id);\n";

  if (sqlite3_exec(db, ddl, NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }

  // Populate 50 users and 1,000 posts to exercise multi-page SQLite B-trees
  sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  sqlite3_stmt *st_u = NULL;
  sqlite3_prepare_v2(db, "INSERT INTO users (id, username) VALUES (?, ?);", -1, &st_u, NULL);
  for (int u = 1; u <= 50; u++) {
    char uname[32]; snprintf(uname, sizeof(uname), "user_%d", u);
    sqlite3_bind_int64(st_u, 1, u);
    sqlite3_bind_text(st_u, 2, uname, -1, SQLITE_STATIC);
    sqlite3_step(st_u);
    sqlite3_reset(st_u);
  }
  sqlite3_finalize(st_u);

  sqlite3_stmt *st_p = NULL;
  sqlite3_prepare_v2(db, "INSERT INTO posts (id, user_id, body) VALUES (?, ?, ?);", -1, &st_p, NULL);
  sqlite3_stmt *st_l = NULL;
  sqlite3_prepare_v2(db, "INSERT INTO likes (user_id, post_id) VALUES (?, ?);", -1, &st_l, NULL);

  char body[256];
  for (int p = 1; p <= 1000; p++) {
    int uid = (p % 50) + 1;
    snprintf(body, sizeof(body), "Synthetic PGO training post #%d payload exercising multi-page SQLite B-trees", p);
    sqlite3_bind_int64(st_p, 1, p);
    sqlite3_bind_int64(st_p, 2, uid);
    sqlite3_bind_text(st_p, 3, body, -1, SQLITE_STATIC);
    sqlite3_step(st_p);
    sqlite3_reset(st_p);

    for (int l = 1; l <= (p % 5) + 1; l++) {
      sqlite3_bind_int64(st_l, 1, l);
      sqlite3_bind_int64(st_l, 2, p);
      sqlite3_step(st_l);
      sqlite3_reset(st_l);
    }
  }
  sqlite3_finalize(st_p);
  sqlite3_finalize(st_l);
  sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

  sqlite3_close(db);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) return 1;

  if (init_scratch_db(argv[1]) != 0) {
    fprintf(stderr, "Failed to initialize scratch db: %s\n", argv[1]);
    return 1;
  }

  char err[256];
  if (td_open(argv[1], err, sizeof(err)) != 0) {
    fprintf(stderr, "td_open failed: %s\n", err);
    return 1;
  }

  for (int iter = 0; iter < 1000; iter++) {
    td_health(err, sizeof(err));

    td_feed_sql_reset();
    i64 id, likes;
    const u8 *b, *ca, *author;
    int blen, clen, alen;
    int count = 0;
    while (td_feed_sql_step(&id, &b, &blen, &ca, &clen, &author, &alen, &likes) == 1) {
      count++;
      if (count >= 20) break;
    }
    td_feed_sql_reset();

    if (td_begin() == 0) {
      void *cp = td_cursor(0); // POSTS
      void *cu = td_cursor(1); // USERS
      void *ci = td_cursor(2); // FEED_IDX
      void *cl = td_cursor(3); // LIKES_IDX

      if (ci && td_first(ci) == 1) {
        for (int k = 0; k < 20; k++) {
          if (td_next(ci) != 1) break;
        }
      }

      i64 target_id = (iter * 37) % 1000 + 1;
      if (cp) {
        td_seek(cp, target_id);
        u32 len = 0;
        td_payload(cp, &len);
      }
      if (cu) {
        i64 uid = (target_id % 50) + 1;
        td_seek(cu, uid);
        u32 len = 0;
        td_payload(cu, &len);
      }
      if (cl) {
        u32 c = 0;
        td_count_likes(cl, target_id, target_id, &c);
      }

      i64 new_id = 0;
      char cr[64];
      int cr_len = 0;
      td_insert_post(1, "pgo training body", 17, &new_id, cr, &cr_len);
      if (new_id > 0) {
        td_insert_like(1, new_id);
        td_delete_like(1, new_id);
      }
      td_commit();
    }
  }

  td_close();
  return 0;
}
