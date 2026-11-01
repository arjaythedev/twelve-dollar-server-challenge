/* Build-time SQLite trainer for GCC profile-guided optimization. Never linked into the server.
 * Usage: train <new-database-file>. Builds a synthetic database, then runs the server's statements
 * in a typical read-heavy feed mix: reads of the feed and of single posts, some likes, few new
 * posts. It runs ANALYZE, as seed/make-seed.sh does, so the query plans
 * match production. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

#define USERS 3000
#define POSTS 20000
#define CYCLES 30000
#define BATCH 32
#define SPAN 256

static sqlite3 *db;
static int wal_frames;

static void die(const char *what) {
  fprintf(stderr, "train: %s failed: %s\n", what, db ? sqlite3_errmsg(db) : "no connection");
  exit(1);
}

static void exec(const char *sql) {
  if (sqlite3_exec(db, sql, 0, 0, 0) != SQLITE_OK) die(sql);
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) != SQLITE_OK) die(sql);
  return s;
}

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  return SQLITE_OK;
}

static uint64_t rng = 88172645463325252ULL;
static uint32_t rnd(uint32_t n) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (uint32_t)((rng >> 11) % n);
}

/* Reads every column of the current row, as the server does. */
static int64_t touch(sqlite3_stmt *s) {
  int64_t sum = 0;
  for (int i = 0; i < sqlite3_column_count(s); i++) {
    if (sqlite3_column_type(s, i) == SQLITE_TEXT) {
      sum += sqlite3_column_bytes(s, i) + (sqlite3_column_text(s, i) != NULL);
    } else {
      sum += sqlite3_column_int64(s, i);
    }
  }
  return sum;
}

/* Steps a statement to the end, reading every column. Returns the checksum. */
static int64_t drain(sqlite3_stmt *s) {
  int64_t sum = 0;
  int rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) sum += touch(s);
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) die("step");
  return sum;
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: train <database>\n"); return 2; }
  /* SQLITE_OMIT_AUTOINIT is set, so initialize by hand. */
  if (sqlite3_initialize() != SQLITE_OK) { fprintf(stderr, "train: initialize failed\n"); return 1; }
  if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) die("open");

  exec("PRAGMA locking_mode=EXCLUSIVE");
  exec("PRAGMA journal_mode=WAL");
  exec("PRAGMA synchronous=NORMAL");
  exec("PRAGMA wal_autocheckpoint=0");
  exec("PRAGMA mmap_size=1073741824");
  exec("PRAGMA cache_size=-16384");
  exec("PRAGMA temp_store=MEMORY");
  sqlite3_wal_hook(db, wal_hook, 0);

  /* schema.sql */
  exec("CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
       " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
       "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
       " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
       " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
       "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
       " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
       "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
       "CREATE INDEX posts_user_id_idx ON posts (user_id);"
       "CREATE INDEX likes_post_id_idx ON likes (post_id);");

  /* Synthetic rows. Post ids grow with created_at, as in the seed. Repeated like pairs are ignored. */
  exec("BEGIN");
  exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<3000)"
       " INSERT INTO users(id,username) SELECT i,'user'||i FROM n");
  exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<20000)"
       " INSERT INTO posts(id,user_id,body,created_at) SELECT i,(i*7919)%3000+1,"
       "'post '||i||' lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor',"
       "strftime('%Y-%m-%dT%H:%M:%fZ',1700000000+i,'unixepoch') FROM n");
  exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<100000)"
       " INSERT OR IGNORE INTO likes(user_id,post_id,created_at) SELECT i%3000+1,(i*31)%20000+1,"
       "strftime('%Y-%m-%dT%H:%M:%fZ',1700000000+i%20000,'unixepoch') FROM n");
  exec("COMMIT");
  exec("ANALYZE");

  /* The server's statements, copied from Db.cs. */
  const char *sel = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id";
  char buf[512];
  snprintf(buf, sizeof buf, "%s ORDER BY p.created_at DESC,p.id DESC LIMIT 20", sel);
  sqlite3_stmt *feed = prep(buf);
  sqlite3_stmt *feed_ids = prep("SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20");
  sqlite3_stmt *feed_likes = prep("SELECT post_id,count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id");
  sqlite3_stmt *feed_rows = prep("SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2");
  snprintf(buf, sizeof buf, "%s WHERE p.id=?1", sel);
  sqlite3_stmt *post = prep(buf);
  sqlite3_stmt *ins_post = prep("INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at");
  sqlite3_stmt *ins_like = prep("INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING");
  sqlite3_stmt *exists = prep("SELECT 1 FROM posts WHERE id=?1");
  sqlite3_stmt *one = prep("SELECT 1");
  sqlite3_stmt *begin = prep("BEGIN");
  sqlite3_stmt *commit = prep("COMMIT");

  int64_t ids[20] = {0}, sum = 0;
  int n = 0, ops = 0;
  for (int c = 0; c < CYCLES; c++) {
    if (ops == 0 && sqlite3_step(begin) != SQLITE_DONE) die("begin");
    sqlite3_reset(begin);

    /* GET /feed: range path, or the join query now and then (and when the span is wide). */
    int slow = rnd(16) == 0;
    n = 0;
    if (!slow) {
      int rc;
      while ((rc = sqlite3_step(feed_ids)) == SQLITE_ROW && n < 20) ids[n++] = sqlite3_column_int64(feed_ids, 0);
      sqlite3_reset(feed_ids);
      if (rc != SQLITE_ROW && rc != SQLITE_DONE) die("feed ids");
      int64_t lo = ids[0], hi = ids[0];
      for (int i = 1; i < n; i++) { if (ids[i] < lo) lo = ids[i]; if (ids[i] > hi) hi = ids[i]; }
      if ((uint64_t)hi - (uint64_t)lo >= SPAN) slow = 1;
      else {
        sqlite3_bind_int64(feed_likes, 1, lo); sqlite3_bind_int64(feed_likes, 2, hi);
        sum += drain(feed_likes);
        sqlite3_bind_int64(feed_rows, 1, lo); sqlite3_bind_int64(feed_rows, 2, hi);
        sum += drain(feed_rows);
      }
    }
    if (slow) {
      n = 0;
      int rc;
      while ((rc = sqlite3_step(feed)) == SQLITE_ROW) { sum += touch(feed); if (n < 20) ids[n++] = sqlite3_column_int64(feed, 0); }
      sqlite3_reset(feed);
      if (rc != SQLITE_DONE) die("feed");
    }

    /* GET /posts/:id for a post from the feed. */
    int64_t pid = ids[rnd(n ? n : 1)];
    sqlite3_bind_int64(post, 1, pid);
    sum += drain(post);

    if (rnd(100) < 15) { /* like */
      sqlite3_bind_int64(ins_like, 1, rnd(USERS) + 1);
      sqlite3_bind_int64(ins_like, 2, pid);
      int rc = sqlite3_step(ins_like);
      sqlite3_reset(ins_like);
      if (rc != SQLITE_DONE) die("like");
      if (sqlite3_changes(db) != 1) {
        sqlite3_bind_int64(exists, 1, pid);
        sum += drain(exists);
      }
    }
    if (rnd(100) < 2) { /* create */
      char body[96];
      int len = snprintf(body, sizeof body, "user says hi at cycle %d", c);
      sqlite3_bind_int64(ins_post, 1, rnd(USERS) + 1);
      sqlite3_bind_text(ins_post, 2, body, len, SQLITE_STATIC);
      if (sqlite3_step(ins_post) != SQLITE_ROW) die("insert post");
      sum += touch(ins_post);
      if (sqlite3_step(ins_post) != SQLITE_DONE) die("insert post done");
      sqlite3_reset(ins_post);
    }
    if (c % 500 == 0) sum += drain(one);

    /* End the batch: commit, then checkpoint once enough frames have built up. */
    if (++ops >= BATCH || c == CYCLES - 1) {
      if (sqlite3_step(commit) != SQLITE_DONE) die("commit");
      sqlite3_reset(commit);
      ops = 0;
      if (wal_frames >= 4096) {
        wal_frames = 0;
        sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, 0, 0);
      }
    }
  }
  fprintf(stderr, "train: done, checksum %lld\n", (long long)sum);
  sqlite3_close_v2(db); /* the _v2 close allows unfinalized statements; the process exits next */
  return 0;
}
