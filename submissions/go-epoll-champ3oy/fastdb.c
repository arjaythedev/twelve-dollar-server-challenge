#include "fastdb.h"
#include "sqlite3.h"

#include <string.h>

// The main connection. Only the event-loop thread touches it (SQLITE_THREADSAFE=2).
static sqlite3 *db;
// The checkpoint connection, owned by the checkpoint thread.
static sqlite3 *ckdb;
static sqlite3_stmt *st_feed, *st_post, *st_insert_post, *st_insert_like, *st_exists,
    *st_begin, *st_commit, *st_rollback, *st_ping;

#define POST_SELECT                                                       \
  "SELECT p.id, p.body, p.created_at, u.username,"                        \
  " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"                \
  " FROM posts p JOIN users u ON u.id = p.user_id"

static const char *const PRAGMAS =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous = NORMAL;"     // rule 6: WAL + NORMAL
    "PRAGMA busy_timeout = 5000;"
    "PRAGMA mmap_size = 1073741824;"   // reads go straight to the OS page cache
    "PRAGMA cache_size = -32768;"      // 32 MiB
    "PRAGMA temp_store = MEMORY;"
    "PRAGMA wal_autocheckpoint = 0;";  // checkpoints run on their own thread

static void set_err(char *err, int errcap, const char *msg) {
  if (errcap <= 0) return;
  int n = (int)strlen(msg);
  if (n >= errcap) n = errcap - 1;
  memcpy(err, msg, n);
  err[n] = 0;
}

static int on_wal_commit(void *arg, sqlite3 *conn, const char *name, int frames);

static int prep(const char *sql, sqlite3_stmt **st) {
  return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, NULL);
}

int fdb_open(const char *path, char *err, int errcap) {
  int rc = sqlite3_initialize();
  if (rc == SQLITE_OK) rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_exec(db, PRAGMAS, NULL, NULL, NULL);
  if (rc == SQLITE_OK) rc = prep(POST_SELECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &st_feed);
  if (rc == SQLITE_OK) rc = prep(POST_SELECT " WHERE p.id = ?1", &st_post);
  if (rc == SQLITE_OK)
    rc = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at", &st_insert_post);
  // Inserts only if the post exists: 0 changes means "already liked" or "no such post".
  if (rc == SQLITE_OK)
    rc = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2"
              " WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)"
              " ON CONFLICT (user_id, post_id) DO NOTHING", &st_insert_like);
  if (rc == SQLITE_OK) rc = prep("SELECT 1 FROM posts WHERE id = ?1", &st_exists);
  if (rc == SQLITE_OK) rc = prep("BEGIN IMMEDIATE", &st_begin);
  if (rc == SQLITE_OK) rc = prep("COMMIT", &st_commit);
  if (rc == SQLITE_OK) rc = prep("ROLLBACK", &st_rollback);
  if (rc == SQLITE_OK) rc = prep("SELECT 1", &st_ping);
  if (rc == SQLITE_OK) sqlite3_wal_hook(db, on_wal_commit, NULL);
  if (rc != SQLITE_OK) {
    set_err(err, errcap, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return FDB_ERR;
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

// --- JSON rendering -------------------------------------------------------------------------

static const char HEX[] = "0123456789abcdef";

// JSON string contents. Needs up to 6*n bytes of room; the callers check that up front.
static char *put_esc(char *o, const unsigned char *s, int n) {
  for (int i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') {
      *o++ = (char)c;
      continue;
    }
    *o++ = '\\';
    switch (c) {
      case '"': *o++ = '"'; break;
      case '\\': *o++ = '\\'; break;
      case '\n': *o++ = 'n'; break;
      case '\r': *o++ = 'r'; break;
      case '\t': *o++ = 't'; break;
      case '\b': *o++ = 'b'; break;
      case '\f': *o++ = 'f'; break;
      default:
        *o++ = 'u'; *o++ = '0'; *o++ = '0';
        *o++ = HEX[c >> 4]; *o++ = HEX[c & 15];
    }
  }
  return o;
}

static char *put_int(char *o, int64_t v) {
  char tmp[24];
  int n = 0;
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0) *o++ = '-';
  while (n) *o++ = tmp[--n];
  return o;
}

#define PUT(o, lit) (memcpy((o), (lit), sizeof(lit) - 1), (o) += sizeof(lit) - 1)

static char *put_post_fields(char *o, int64_t id, const unsigned char *body, int blen,
                             const unsigned char *created, int clen,
                             const unsigned char *author, int alen, int64_t likes) {
  PUT(o, "{\"id\":");
  o = put_int(o, id);
  PUT(o, ",\"body\":\"");
  o = put_esc(o, body, blen);
  PUT(o, "\",\"created_at\":\"");
  o = put_esc(o, created, clen);
  PUT(o, "\",\"author\":\"");
  o = put_esc(o, author, alen);
  PUT(o, "\",\"like_count\":");
  o = put_int(o, likes);
  *o++ = '}';
  return o;
}

// Worst case for one post object: every string byte escaped as \u00XX, plus keys and numbers.
#define POST_ROOM(b, c, a) (6 * ((b) + (c) + (a)) + 128)

// Renders the current row of a POST_SELECT statement, or returns NULL if it doesn't fit.
static char *put_post_row(char *o, char *end, sqlite3_stmt *st) {
  const unsigned char *body = sqlite3_column_text(st, 1);
  int blen = sqlite3_column_bytes(st, 1);
  const unsigned char *created = sqlite3_column_text(st, 2);
  int clen = sqlite3_column_bytes(st, 2);
  const unsigned char *author = sqlite3_column_text(st, 3);
  int alen = sqlite3_column_bytes(st, 3);
  if (end - o < POST_ROOM(blen, clen, alen)) return NULL;
  return put_post_fields(o, sqlite3_column_int64(st, 0), body, blen, created, clen, author, alen,
                         sqlite3_column_int64(st, 4));
}

int fdb_feed(char *out, int cap) {
  char *o = out, *end = out + cap;
  if (cap < 16) return FDB_NOSPACE;
  PUT(o, "{\"posts\":[");
  int rc, first = 1;
  while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
    if (!first) *o++ = ',';
    first = 0;
    o = put_post_row(o, end - 2, st_feed);
    if (!o) { sqlite3_reset(st_feed); return FDB_NOSPACE; }
  }
  sqlite3_reset(st_feed);
  if (rc != SQLITE_DONE) return FDB_ERR;
  *o++ = ']';
  *o++ = '}';
  return (int)(o - out);
}

int fdb_post(int64_t id, char *out, int cap) {
  char *o = out, *end = out + cap;
  if (cap < 16) return FDB_NOSPACE;
  sqlite3_bind_int64(st_post, 1, id);
  int rc = sqlite3_step(st_post);
  if (rc == SQLITE_ROW) {
    PUT(o, "{\"post\":");
    o = put_post_row(o, end - 1, st_post);
    if (!o) { sqlite3_reset(st_post); return FDB_NOSPACE; }
    *o++ = '}';
  }
  sqlite3_reset(st_post);
  if (rc == SQLITE_DONE) return 0;
  if (rc != SQLITE_ROW) return FDB_ERR;
  return (int)(o - out);
}

// --- writes ---------------------------------------------------------------------------------

static int run(sqlite3_stmt *st) {
  int rc = sqlite3_step(st);
  sqlite3_reset(st);
  return rc == SQLITE_DONE ? 0 : FDB_ERR;
}

int fdb_begin(void) { return run(st_begin); }
int fdb_commit(void) { return run(st_commit); }
int fdb_rollback(void) {
  if (sqlite3_get_autocommit(db)) return 0;  // SQLite already rolled the transaction back
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

int fdb_create(int64_t user_id, const char *body, int blen, const char *author, int alen,
               char *out, int cap) {
  char *o = out;
  sqlite3_bind_int64(st_insert_post, 1, user_id);
  sqlite3_bind_text(st_insert_post, 2, body, blen, SQLITE_STATIC);
  int rc = sqlite3_step(st_insert_post);  // RETURNING: the insert happens on this first step
  int n = FDB_ERR;
  if (rc == SQLITE_ROW) {
    const unsigned char *created = sqlite3_column_text(st_insert_post, 1);
    int clen = sqlite3_column_bytes(st_insert_post, 1);
    if (cap < POST_ROOM(blen, clen, alen) + 16) {
      n = FDB_NOSPACE;
    } else {
      PUT(o, "{\"post\":");
      o = put_post_fields(o, sqlite3_column_int64(st_insert_post, 0), (const unsigned char *)body,
                          blen, created, clen, (const unsigned char *)author, alen, 0);
      *o++ = '}';
      n = (int)(o - out);
    }
    rc = sqlite3_step(st_insert_post);
  }
  sqlite3_reset(st_insert_post);
  sqlite3_clear_bindings(st_insert_post);  // body points at Go memory (SQLITE_STATIC)
  if (rc != SQLITE_DONE) return FDB_ERR;
  return n;
}

// --- checkpoints ----------------------------------------------------------------------------

// SQLite's autocheckpoint, moved off the event loop: its fsyncs would stall every request.
// The WAL only rewinds once a checkpoint has caught up with every frame, which never happens
// while the writer keeps appending, so the event loop holds back writes (not reads) while the
// checkpoint thread runs. See server.go.
static int wal_frames;

static int on_wal_commit(void *arg, sqlite3 *conn, const char *name, int frames) {
  (void)arg; (void)conn; (void)name;
  wal_frames = frames;
  return SQLITE_OK;
}

int fdb_wal_frames(void) { return wal_frames; }

int fdb_ckpt_open(const char *path) {
  int rc = sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE, NULL);
  // SQLite opens the file lazily: read the schema once so this connection is attached to the
  // WAL, otherwise its checkpoints silently do nothing.
  if (rc == SQLITE_OK)
    rc = sqlite3_exec(ckdb, "PRAGMA busy_timeout = 5000; SELECT count(*) FROM sqlite_schema;",
                      NULL, NULL, NULL);
  return rc == SQLITE_OK ? 0 : FDB_ERR;
}

int fdb_checkpoint(void) {
  int rc = sqlite3_wal_checkpoint_v2(ckdb, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
  return rc == SQLITE_OK ? 0 : FDB_ERR;
}
