/* Training run for SQLite's profile-guided build (build.sh, PLAN 8.9). Idea and structure from
 * fsharp-kestrel-delneg's sqlite-train.c. It builds a synthetic database with the challenge schema
 * (schema.sql) in a scratch file, then runs this server's statements with its pragmas (src/db.jl): feeds,
 * single posts, likes and new posts in the bench/load.js shares, one transaction per 16 requests (the
 * loop's pass), with PASSIVE checkpoints on a second connection (the checkpoint thread in csrc/shim.c).
 * gcc records which branches run; the second compile of sqlite3.c uses that profile. No benchmark data
 * is read: the database is made here and deleted at the end. */
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

static sqlite3 *db;

static void ok(int rc, const char *what) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    fprintf(stderr, "train: %s: %s\n", what, sqlite3_errmsg(db));
    exit(1);
  }
}
static void run(const char *sql) { ok(sqlite3_exec(db, sql, 0, 0, 0), sql); }
static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s;
  ok(sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0), sql);
  return s;
}
static unsigned rnd(void) { /* xorshift: the same run every build */
  static unsigned long long x = 88172645463325252ULL;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  return (unsigned)(x >> 11);
}
/* created_at in the wire format, secs seconds after the start of 2025 (28-day months are enough here) */
static void stamp(char *b, int secs) {
  int d = secs / 86400, s = secs % 86400;
  snprintf(b, 32, "2025-%02d-%02dT%02d:%02d:%02d.%03dZ", 1 + (d / 28) % 12, 1 + d % 28, s / 3600, (s / 60) % 60,
           s % 60, secs % 1000);
}
/* reads every column of a post row, as the reply writer does */
static long long cols(sqlite3_stmt *s, int likes) {
  long long v = sqlite3_column_int64(s, 0);
  for (int c = 1; c <= 3; c++) v += sqlite3_column_bytes(s, c) + (long long)(size_t)sqlite3_column_text(s, c);
  if (likes) v += sqlite3_column_int64(s, 4);
  return v;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "train.db";
  enum { USERS = 20000, POSTS = 60000, LIKES = 240000, ROUNDS = 40000 };
  char b[600], ts[32], wal[256], shm[256];
  snprintf(wal, sizeof wal, "%s-wal", path);
  snprintf(shm, sizeof shm, "%s-shm", path);
  remove(path); remove(wal); remove(shm);
  ok(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0), "open");
  run("PRAGMA journal_mode = WAL");
  run("CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
      " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
      " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
      " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
      " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
      "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
      "CREATE INDEX posts_user_id_idx ON posts (user_id);"
      "CREATE INDEX likes_post_id_idx ON likes (post_id);");
  run("BEGIN");
  sqlite3_stmt *u = prep("INSERT INTO users (id, username) VALUES (?1, ?2)");
  for (int i = 1; i <= USERS; i++) {
    snprintf(b, sizeof b, "user_%d", i);
    sqlite3_bind_int64(u, 1, i); sqlite3_bind_text(u, 2, b, -1, SQLITE_TRANSIENT);
    ok(sqlite3_step(u), "user"); sqlite3_reset(u);
  }
  sqlite3_stmt *p = prep("INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)");
  for (int i = 1; i <= POSTS; i++) {
    int n = 20 + rnd() % 200;
    for (int k = 0; k < n; k++) b[k] = "abcdefghij klmnopqrstuvwxyz.,!?"[rnd() % 31];
    b[0] = 'X';
    stamp(ts, i * 30);
    sqlite3_bind_int64(p, 1, i); sqlite3_bind_int64(p, 2, 1 + rnd() % USERS);
    sqlite3_bind_text(p, 3, b, n, SQLITE_TRANSIENT); sqlite3_bind_text(p, 4, ts, -1, SQLITE_TRANSIENT);
    ok(sqlite3_step(p), "post"); sqlite3_reset(p);
  }
  sqlite3_stmt *l = prep("INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)");
  for (int i = 0; i < LIKES; i++) {
    sqlite3_bind_int64(l, 1, 1 + rnd() % USERS); sqlite3_bind_int64(l, 2, 1 + rnd() % POSTS);
    ok(sqlite3_step(l), "like"); sqlite3_reset(l);
  }
  run("COMMIT");
  sqlite3_finalize(u); sqlite3_finalize(p); sqlite3_finalize(l);
  sqlite3_close(db);

  /* The server's connection: flags, pragmas and statements as in src/db.jl. */
  ok(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0), "reopen");
  run("PRAGMA journal_mode=WAL"); run("PRAGMA synchronous=NORMAL"); run("PRAGMA busy_timeout=5000");
  run("PRAGMA cache_size=-65536"); run("PRAGMA mmap_size=0"); run("PRAGMA temp_store=MEMORY");
  run("PRAGMA wal_autocheckpoint=0");
  sqlite3 *ck; /* the checkpoint thread's connection */
  ok(sqlite3_open_v2(path, &ck, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0), "checkpoint open");
  sqlite3_exec(ck, "PRAGMA journal_mode=WAL", 0, 0, 0);
  const char *sel = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
                    " FROM posts p JOIN users u ON u.id = p.user_id";
  snprintf(b, sizeof b, "%s ORDER BY p.created_at DESC, p.id DESC LIMIT 20", sel);
  sqlite3_stmt *feed = prep(b);
  snprintf(b, sizeof b, "%s WHERE p.id = ?1", sel);
  sqlite3_stmt *post = prep(b);
  sqlite3_stmt *ids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
  sqlite3_stmt *range = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id"
                             " WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC");
  sqlite3_stmt *likes = prep("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id");
  sqlite3_stmt *exists = prep("SELECT 1 FROM posts WHERE id = ?1");
  sqlite3_stmt *ins = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  sqlite3_stmt *like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  sqlite3_stmt *ping = prep("SELECT 1");
  sqlite3_stmt *begin = prep("BEGIN"), *commit = prep("COMMIT");
  long long sum = 0;
  for (int r = 0; r < ROUNDS; r++) {
    if (r % 16 == 0) { ok(sqlite3_step(begin), "begin"); sqlite3_reset(begin); } /* one transaction per pass */
    unsigned x = rnd() % 217; /* bench/load.js shares: feed and post ~46% each, like 15/217, new post 2/217 */
    if (x < 100) {
      if (x % 8) { /* the range path (the 20 newest ids are contiguous), the plain query now and then */
        long long hi = 0, lo = 0; int n = 0;
        while (sqlite3_step(ids) == SQLITE_ROW) { lo = sqlite3_column_int64(ids, 0); if (!n++) hi = lo; }
        sqlite3_reset(ids);
        sqlite3_bind_int64(likes, 1, lo); sqlite3_bind_int64(likes, 2, hi);
        while (sqlite3_step(likes) == SQLITE_ROW) sum += sqlite3_column_int64(likes, 0) + sqlite3_column_int64(likes, 1);
        sqlite3_reset(likes);
        sqlite3_bind_int64(range, 1, lo); sqlite3_bind_int64(range, 2, hi);
        while (sqlite3_step(range) == SQLITE_ROW) sum += cols(range, 0);
        sqlite3_reset(range);
      } else {
        while (sqlite3_step(feed) == SQLITE_ROW) sum += cols(feed, 1);
        sqlite3_reset(feed);
      }
    } else if (x < 200) {
      sqlite3_bind_int64(post, 1, 1 + rnd() % (POSTS + 1000)); /* a few ids that do not exist: 404 */
      while (sqlite3_step(post) == SQLITE_ROW) sum += cols(post, 1);
      sqlite3_reset(post);
    } else if (x < 215) { /* h_like!: the post must exist, then the insert */
      long long pid = 1 + rnd() % (POSTS + 100);
      sqlite3_bind_int64(exists, 1, pid);
      int found = sqlite3_step(exists) == SQLITE_ROW;
      sqlite3_reset(exists);
      if (found) {
        sqlite3_bind_int64(like, 1, 1 + rnd() % USERS); sqlite3_bind_int64(like, 2, pid);
        ok(sqlite3_step(like), "like2"); sqlite3_reset(like);
        sum += sqlite3_changes(db);
      }
    } else {
      int n = 10 + rnd() % 60;
      for (int k = 0; k < n; k++) b[k] = 'a' + rnd() % 26;
      sqlite3_bind_int64(ins, 1, 1 + rnd() % USERS); sqlite3_bind_text(ins, 2, b, n, SQLITE_TRANSIENT);
      ok(sqlite3_step(ins), "insert"); sum += sqlite3_column_int64(ins, 0) + sqlite3_column_bytes(ins, 1);
      ok(sqlite3_step(ins), "insert done"); sqlite3_reset(ins);
    }
    if (r % 1000 == 0) { sum += sqlite3_step(ping); sqlite3_reset(ping); }
    if (r % 16 == 15) { ok(sqlite3_step(commit), "commit"); sqlite3_reset(commit); }
    if (r % 400 == 399) { int lf = 0, cp = 0; sqlite3_wal_checkpoint_v2(ck, "main", SQLITE_CHECKPOINT_PASSIVE, &lf, &cp); }
  }
  if (ROUNDS % 16) { ok(sqlite3_step(commit), "commit"); sqlite3_reset(commit); }
  sqlite3_close_v2(ck);
  sqlite3_close_v2(db);
  remove(path); remove(wal); remove(shm);
  printf("train: %d rounds (%lld)\n", (int)ROUNDS, sum & 0xff);
  return 0;
}
