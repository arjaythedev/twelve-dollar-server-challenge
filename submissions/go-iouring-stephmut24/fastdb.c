/* SQLite hot path: amalgamation + B-tree reads (SQL fallback) + JSON render.
 * B-tree plans adapted from c-iouring-pavlyhalim #245 (credited in README).
 * sqlite3.c lives under csrc/ and is #include'd here (unity TU) so cgo does not
 * compile it a second time. */
#include "sqlite3.c"
#include "ob.h"
#include "btree.h"
#include "fastdb.h"

#include <stdlib.h>
#include <string.h>

static sqlite3 *db;
static sqlite3 *ckdb;
static sqlite3_stmt *st_feed, *st_post, *st_insert_post, *st_insert_like, *st_exists, *st_begin,
    *st_commit, *st_rollback, *st_ping, *st_begin_def;
static struct bt g_bt;
static int btree_on = 1; /* BTREE=0 disables */
static int write_tx;     /* group-commit write transaction is open */

#define POST_SELECT                                                                               \
  "SELECT p.id, p.body, p.created_at, u.username,"                                                \
  " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"                                        \
  " FROM posts p JOIN users u ON u.id = p.user_id"

static const char *const PRAGMAS =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous = NORMAL;"
    "PRAGMA busy_timeout = 5000;"
    "PRAGMA mmap_size = 0;"
    "PRAGMA cache_size = -65536;"
    "PRAGMA temp_store = MEMORY;"
    "PRAGMA locking_mode = NORMAL;"
    "PRAGMA wal_autocheckpoint = 0;";

static void set_err(char *err, int errcap, const char *msg) {
  if (errcap <= 0) return;
  int n = (int)strlen(msg);
  if (n >= errcap) n = errcap - 1;
  memcpy(err, msg, (size_t)n);
  err[n] = 0;
}

static int on_wal_commit(void *arg, sqlite3 *conn, const char *name, int frames);

static int prep(const char *sql, sqlite3_stmt **st) {
  return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, NULL);
}

/* Open a short deferred txn + B-tree cursors for one read. No-op if a write txn is open. */
static int read_enter(void) {
  if (write_tx) {
    if (btree_on && g_bt.ok && !g_bt.open && bt_open(&g_bt) != 0) bt_close(&g_bt);
    return 0;
  }
  if (sqlite3_step(st_begin_def) != SQLITE_DONE) {
    sqlite3_reset(st_begin_def);
    return -1;
  }
  sqlite3_reset(st_begin_def);
  if (btree_on && g_bt.ok && bt_open(&g_bt) != 0) bt_close(&g_bt);
  return 0;
}

static void read_leave(void) {
  if (write_tx) return;
  bt_close(&g_bt);
  if (!sqlite3_get_autocommit(db)) {
    sqlite3_step(st_commit);
    sqlite3_reset(st_commit);
  }
}

int fdb_open(const char *path, char *err, int errcap) {
  const char *env = getenv("BTREE");
  if (env && env[0] == '0') btree_on = 0;

  int rc = sqlite3_initialize();
  if (rc == SQLITE_OK) rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_exec(db, PRAGMAS, NULL, NULL, NULL);
  if (rc == SQLITE_OK) rc = prep(POST_SELECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &st_feed);
  if (rc == SQLITE_OK) rc = prep(POST_SELECT " WHERE p.id = ?1", &st_post);
  if (rc == SQLITE_OK)
    rc = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at", &st_insert_post);
  if (rc == SQLITE_OK)
    rc = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2"
              " WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)"
              " ON CONFLICT (user_id, post_id) DO NOTHING",
              &st_insert_like);
  if (rc == SQLITE_OK) rc = prep("SELECT 1 FROM posts WHERE id = ?1", &st_exists);
  if (rc == SQLITE_OK) rc = prep("BEGIN IMMEDIATE", &st_begin);
  if (rc == SQLITE_OK) rc = prep("BEGIN", &st_begin_def);
  if (rc == SQLITE_OK) rc = prep("COMMIT", &st_commit);
  if (rc == SQLITE_OK) rc = prep("ROLLBACK", &st_rollback);
  if (rc == SQLITE_OK) rc = prep("SELECT 1", &st_ping);
  if (rc == SQLITE_OK) sqlite3_wal_hook(db, on_wal_commit, NULL);
  if (rc != SQLITE_OK) {
    set_err(err, errcap, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return FDB_ERR;
  }
  if (btree_on) {
    if (!bt_init(&g_bt, db)) btree_on = 0;
  }
  return 0;
}

int fdb_ping(char *err, int errcap) {
  int rc = sqlite3_step(st_ping);
  sqlite3_reset(st_ping);
  if (rc != SQLITE_ROW) {
    set_err(err, errcap, sqlite3_errmsg(db));
    return FDB_ERR;
  }
  return 0;
}

static char *put_post_row(struct ob *o, sqlite3_stmt *st) {
  const unsigned char *body = sqlite3_column_text(st, 1);
  int blen = sqlite3_column_bytes(st, 1);
  const unsigned char *created = sqlite3_column_text(st, 2);
  int clen = sqlite3_column_bytes(st, 2);
  const unsigned char *author = sqlite3_column_text(st, 3);
  int alen = sqlite3_column_bytes(st, 3);
  ob_post_obj(o, sqlite3_column_int64(st, 0), body, blen, created, clen, author, alen,
              sqlite3_column_int64(st, 4));
  return o->err ? NULL : o->p;
}

static int feed_sql(struct ob *o) {
  ob_lit(o, "{\"posts\":[");
  int rc, first = 1;
  while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
    if (!first) ob_char(o, ',');
    first = 0;
    put_post_row(o, st_feed);
    if (o->err) {
      sqlite3_reset(st_feed);
      return FDB_NOSPACE;
    }
  }
  sqlite3_reset(st_feed);
  if (rc != SQLITE_DONE) return FDB_ERR;
  ob_lit(o, "]}");
  return o->err ? FDB_NOSPACE : o->n;
}

int fdb_feed(char *out, int cap) {
  if (cap < 16) return FDB_NOSPACE;
  if (read_enter() != 0) return FDB_ERR;
  struct ob o;
  ob_init(&o, out, cap);
  int n;
  if (btree_on && g_bt.open) {
    int mark = o.n;
    if (bt_feed(&g_bt, &o) == 0 && !o.err) {
      n = o.n;
      read_leave();
      return n;
    }
    o.n = mark;
    o.err = 0;
  }
  n = feed_sql(&o);
  read_leave();
  return n;
}

int fdb_post(int64_t id, char *out, int cap) {
  if (cap < 16) return FDB_NOSPACE;
  if (read_enter() != 0) return FDB_ERR;
  struct ob o;
  ob_init(&o, out, cap);
  int n = FDB_ERR;
  if (btree_on && g_bt.open) {
    int mark = o.n;
    int rc = bt_post(&g_bt, &o, id);
    if (rc >= 0 && !o.err) {
      n = rc == 0 ? 0 : o.n;
      read_leave();
      return n;
    }
    o.n = mark;
    o.err = 0;
  }
  sqlite3_bind_int64(st_post, 1, id);
  int rc = sqlite3_step(st_post);
  if (rc == SQLITE_ROW) {
    ob_lit(&o, "{\"post\":");
    put_post_row(&o, st_post);
    ob_char(&o, '}');
    n = o.err ? FDB_NOSPACE : o.n;
  } else if (rc == SQLITE_DONE) {
    n = 0;
  }
  sqlite3_reset(st_post);
  read_leave();
  return n;
}

static int run(sqlite3_stmt *st) {
  int rc = sqlite3_step(st);
  sqlite3_reset(st);
  return rc == SQLITE_DONE ? 0 : FDB_ERR;
}

int fdb_begin(void) {
  if (write_tx) return 0;
  if (!sqlite3_get_autocommit(db)) {
    bt_close(&g_bt);
    run(st_commit);
  }
  if (run(st_begin) != 0) return FDB_ERR;
  if (btree_on && g_bt.ok && bt_open(&g_bt) != 0) bt_close(&g_bt);
  write_tx = 1;
  return 0;
}

int fdb_commit(void) {
  if (!write_tx) return 0;
  bt_close(&g_bt);
  write_tx = 0;
  return run(st_commit);
}

int fdb_rollback(void) {
  bt_close(&g_bt);
  write_tx = 0;
  if (sqlite3_get_autocommit(db)) return 0;
  return run(st_rollback);
}

int fdb_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(st_insert_like, 1, user_id);
  sqlite3_bind_int64(st_insert_like, 2, post_id);
  if (run(st_insert_like)) return FDB_ERR;
  if (sqlite3_changes(db) == 1) return 1;
  sqlite3_bind_int64(st_exists, 1, post_id);
  int rc = sqlite3_step(st_exists);
  sqlite3_reset(st_exists);
  if (rc == SQLITE_ROW) return 2;
  return rc == SQLITE_DONE ? 0 : FDB_ERR;
}

int fdb_create(int64_t user_id, const char *body, int blen, const char *author, int alen, char *out,
               int cap) {
  struct ob o;
  ob_init(&o, out, cap);
  sqlite3_bind_int64(st_insert_post, 1, user_id);
  sqlite3_bind_text(st_insert_post, 2, body, blen, SQLITE_STATIC);
  int rc = sqlite3_step(st_insert_post);
  int n = FDB_ERR;
  if (rc == SQLITE_ROW) {
    const unsigned char *created = sqlite3_column_text(st_insert_post, 1);
    int clen = sqlite3_column_bytes(st_insert_post, 1);
    ob_lit(&o, "{\"post\":");
    ob_post_obj(&o, sqlite3_column_int64(st_insert_post, 0), (const unsigned char *)body, blen, created,
                clen, (const unsigned char *)author, alen, 0);
    ob_char(&o, '}');
    n = o.err ? FDB_NOSPACE : o.n;
    rc = sqlite3_step(st_insert_post);
  }
  sqlite3_reset(st_insert_post);
  sqlite3_clear_bindings(st_insert_post);
  if (rc != SQLITE_DONE) return FDB_ERR;
  return n;
}

static int wal_frames;

static int on_wal_commit(void *arg, sqlite3 *conn, const char *name, int frames) {
  (void)arg;
  (void)conn;
  (void)name;
  wal_frames = frames;
  return SQLITE_OK;
}

int fdb_wal_frames(void) { return wal_frames; }

int fdb_ckpt_open(const char *path) {
  int rc = sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_exec(ckdb, "PRAGMA busy_timeout = 5000; PRAGMA wal_autocheckpoint = 0; SELECT count(*) FROM sqlite_schema;",
                      NULL, NULL, NULL);
  return rc == SQLITE_OK ? 0 : FDB_ERR;
}

int fdb_checkpoint(void) {
  int rc = sqlite3_wal_checkpoint_v2(ckdb, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
  return rc == SQLITE_OK ? 0 : FDB_ERR;
}
