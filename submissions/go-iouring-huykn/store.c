#include "store.h"
#include "read_plan.h"
#include "checkpoint.h"
#include "sqlite/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum query {
  RECENT, DETAIL, CREATE, CREATED_ROW, VOTE, VOTE_STATUS, PING, BEGIN_WRITE, COMMIT_WRITE,
  ABORT_WRITE, QUERY_COUNT
};

// Reference queries handle unsupported read layouts. Writes and transaction
// control use public SQL; the independent cursor plan handles supported reads.
static const char *const queries[QUERY_COUNT] = {
  [RECENT] =
    "SELECT p.id,p.body,p.created_at,u.username,"
    "(SELECT count(*) FROM likes WHERE post_id=p.id) "
    "FROM posts AS p JOIN users AS u ON u.id=p.user_id "
    "ORDER BY p.created_at DESC,p.id DESC LIMIT 20",
  [DETAIL] =
    "SELECT p.id,p.body,p.created_at,u.username,"
    "(SELECT count(*) FROM likes WHERE post_id=p.id) "
    "FROM posts AS p JOIN users AS u ON u.id=p.user_id WHERE p.id=?1",
  [CREATE] = "INSERT INTO posts(user_id,body) VALUES(?1,?2)",
  [CREATED_ROW] = "SELECT id,created_at FROM posts WHERE id=last_insert_rowid()",
  [VOTE] = "INSERT INTO likes(user_id,post_id) VALUES(?1,?2) "
           "ON CONFLICT(user_id,post_id) DO NOTHING",
  [VOTE_STATUS] =
    "SELECT CASE WHEN NOT EXISTS(SELECT 1 FROM posts WHERE id=?2) THEN 404 "
    "WHEN EXISTS(SELECT 1 FROM likes WHERE user_id=?1 AND post_id=?2) THEN 200 "
    "ELSE 201 END",
  [PING] = "SELECT 1",
  [BEGIN_WRITE] = "BEGIN DEFERRED",
  [COMMIT_WRITE] = "COMMIT",
  [ABORT_WRITE] = "ROLLBACK"
};

struct hkn_store {
  sqlite3 *connection;
  sqlite3_stmt *statement[QUERY_COUNT];
  hkn_result result;
  hkn_read_plan *reader;
  hkn_checkpoint *checkpoint;
  int wal_frames, checkpoint_sent;
  char error[256];
  int transaction, poisoned;
};

static int failed(hkn_store *s, int code) {
  if (code == SQLITE_OK) return 0;
  snprintf(s->error, sizeof s->error, "%s", sqlite3_errmsg(s->connection));
  // FULL/IOERR and some other errors may roll back the entire transaction.
  // Do not acknowledge earlier staged writes if SQLite did that implicitly.
  if (s->transaction && sqlite3_get_autocommit(s->connection)) s->poisoned = 1;
  return -1;
}

static int execute(hkn_store *s, enum query q) {
  sqlite3_stmt *st = s->statement[q];
  int code = sqlite3_step(st);
  int result = code == SQLITE_DONE ? 0 : failed(s, code);
  sqlite3_reset(st);
  return result;
}

void hkn_disconnect(hkn_store *s) {
  if (!s) return;
  hkn_plan_release(s->reader);
  hkn_checkpoint_free(s->checkpoint);
  for (int i = 0; i < QUERY_COUNT; ++i) sqlite3_finalize(s->statement[i]);
  sqlite3_close(s->connection);
  free(s);
}

static int record_wal(void *context, sqlite3 *db, const char *name, int frames) {
  (void)db; (void)name;
  ((hkn_store *)context)->wal_frames = frames;
  return SQLITE_OK;
}

hkn_store *hkn_connect(const char *filename, char *error, int capacity) {
  hkn_store *s = calloc(1, sizeof *s);
  if (!s) {
    snprintf(error, capacity, "database handle allocation failed");
    return NULL;
  }
  const char *vfs = getenv("HKN_VFS");
  if (!vfs) vfs = "unix-excl";
  int code = sqlite3_initialize();
  if (code == SQLITE_OK) code = sqlite3_open_v2(filename, &s->connection,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, vfs);
  if (code == SQLITE_OK) code = sqlite3_busy_timeout(s->connection, 1000);
  if (code == SQLITE_OK) code = sqlite3_db_config(s->connection,
                                                SQLITE_DBCONFIG_ENABLE_QPSG, 1, NULL);
  if (code == SQLITE_OK) code = sqlite3_exec(s->connection,
    "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
    "PRAGMA temp_store=MEMORY; PRAGMA cache_size=-262144; PRAGMA mmap_size=0;", NULL, NULL, NULL);
  // Automatic checkpoints are the safe fallback if the worker cannot start.
  if (code == SQLITE_OK) code = sqlite3_wal_autocheckpoint(s->connection, 8192);
  for (int i = 0; code == SQLITE_OK && i < QUERY_COUNT; ++i) {
    code = sqlite3_prepare_v3(s->connection, queries[i], -1,
                            SQLITE_PREPARE_PERSISTENT, &s->statement[i], NULL);
  }
  if (code == SQLITE_OK) {
    const char *sql_reads = getenv("HKN_SQL_READS");
    if (!sql_reads || strcmp(sql_reads, "1")) s->reader = hkn_plan_new(s->connection);
    const char *inline_checkpoint = getenv("HKN_CHECKPOINT");
    if (!inline_checkpoint || strcmp(inline_checkpoint, "inline")) {
      s->checkpoint = hkn_checkpoint_new(filename, vfs);
      if (s->checkpoint) sqlite3_wal_hook(s->connection, record_wal, s);
    }
    return s;
  }
  snprintf(error, capacity, "%s", s->connection ? sqlite3_errmsg(s->connection) : sqlite3_errstr(code));
  hkn_disconnect(s);
  return NULL;
}

// Copy column bytes before stepping/resetting the statement. A request never
// borrows a SQLite column pointer, and no result is reused across requests.
static int save_column(hkn_store *s, sqlite3_stmt *st, int column, hkn_span *span) {
  const unsigned char *value = sqlite3_column_text(st, column);
  int size = sqlite3_column_bytes(st, column);
  if (!value && sqlite3_column_type(st, column) != SQLITE_NULL) {
    return failed(s, SQLITE_NOMEM);
  }
  if ((unsigned)size > sizeof s->result.bytes - s->result.used) {
    snprintf(s->error, sizeof s->error, "database result exceeds 65536 bytes");
    return -1;
  }
  span->at = s->result.used;
  span->size = (unsigned)size;
  if (size) memcpy(s->result.bytes + span->at, value, size);
  s->result.used += size;
  return 0;
}

static int begin_batch(hkn_store *s);

const hkn_result *hkn_read(hkn_store *s, int single, int64_t key) {
  if (s->poisoned) return NULL;
  if (s->reader) {
    if (begin_batch(s)) return NULL;
    int code = hkn_plan_read(s->reader, single, key, &s->result);
    if (code == SQLITE_OK) return &s->result;
    hkn_plan_end(s->reader);
    if (code != SQLITE_NOTFOUND) { failed(s, code); return NULL; }
  }
  sqlite3_stmt *st = s->statement[single ? DETAIL : RECENT];
  s->result.used = 0;
  s->result.length = 0;
  int code = single ? sqlite3_bind_int64(st, 1, key) : SQLITE_OK;
  int bad = failed(s, code);
  while (!bad && (code = sqlite3_step(st)) == SQLITE_ROW) {
    hkn_record *r = &s->result.items[s->result.length];
    r->key = sqlite3_column_int64(st, 0);
    r->count = sqlite3_column_int64(st, 4);
    bad = save_column(s, st, 1, &r->content) ||
          save_column(s, st, 2, &r->timestamp) ||
          save_column(s, st, 3, &r->username);
    if (!bad) ++s->result.length;
  }
  if (!bad && code != SQLITE_DONE) bad = failed(s, code);
  sqlite3_reset(st);
  return bad ? NULL : &s->result;
}

static int begin_batch(hkn_store *s) {
  if (s->poisoned) return -1;
  if (s->transaction) return 0;
  if (execute(s, BEGIN_WRITE)) return -1;
  s->transaction = 1;
  return 0;
}

const hkn_result *hkn_create(hkn_store *s, int64_t user, const char *body, int bytes) {
  if (begin_batch(s)) return NULL;
  sqlite3_stmt *st = s->statement[CREATE];
  s->result.used = 0;
  s->result.length = 0;
  int code = sqlite3_bind_int64(st, 1, user);
  // Go pins body for this call. The statement is stepped, reset and unbound
  // before returning on every path, so SQLite never retains a Go pointer.
  if (code == SQLITE_OK) code = sqlite3_bind_text(st, 2, body, bytes, SQLITE_STATIC);
  if (code == SQLITE_OK) code = sqlite3_step(st);
  int bad = code != SQLITE_DONE;
  if (bad) failed(s, code);
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  if (!bad) {
    // Reading the inserted row inside this transaction avoids RETURNING's
    // ephemeral result table while still returning the stored default timestamp.
    st = s->statement[CREATED_ROW];
    code = sqlite3_step(st);
    if (code != SQLITE_ROW) { failed(s, code); bad = 1; }
    else {
      s->result.items[0].key = sqlite3_column_int64(st, 0);
      bad = save_column(s, st, 1, &s->result.items[0].timestamp);
    }
    sqlite3_reset(st);
  }
  s->result.length = bad ? 0 : 1;
  return bad ? NULL : &s->result;
}

int hkn_vote(hkn_store *s, int64_t user, int64_t post) {
  if (s->poisoned) return -1;
  sqlite3_stmt *check = s->statement[VOTE_STATUS];
  int code = sqlite3_bind_int64(check, 1, user);
  if (code == SQLITE_OK) code = sqlite3_bind_int64(check, 2, post);
  if (code == SQLITE_OK) code = sqlite3_step(check);
  int outcome = code == SQLITE_ROW ? sqlite3_column_int(check, 0) : -1;
  int bad = code != SQLITE_ROW;
  if (bad) failed(s, code);
  sqlite3_reset(check);
  if (bad) return -1;
  // A repeat still reads both tables for this request, but need not attempt a
  // write or start an otherwise empty transaction.
  if (outcome != 201) return outcome;
  if (begin_batch(s)) return -1;
  sqlite3_stmt *st = s->statement[VOTE];
  code = sqlite3_bind_int64(st, 1, user);
  if (code == SQLITE_OK) code = sqlite3_bind_int64(st, 2, post);
  if (code == SQLITE_OK) code = sqlite3_step(st);
  bad = code != SQLITE_DONE;
  if (bad) failed(s, code);
  int changed = sqlite3_changes(s->connection);
  sqlite3_reset(st);
  return bad ? -1 : changed ? 201 : 200;
}

int hkn_finish(hkn_store *s) {
  hkn_plan_end(s->reader);
  if (!s->transaction) return 0;
  int bad = s->poisoned || execute(s, COMMIT_WRITE);
  if (bad && !sqlite3_get_autocommit(s->connection)) execute(s, ABORT_WRITE);
  if (!sqlite3_get_autocommit(s->connection)) {
    s->poisoned = 1;
    return -1;
  }
  s->transaction = 0;
  s->poisoned = 0;
  if (bad || !s->checkpoint) return bad ? -1 : 0;
  // WAL growth is observed only after durable commit, with all read cursors
  // closed. Signal at coarse frame intervals rather than on every request.
  if (s->wal_frames < s->checkpoint_sent) s->checkpoint_sent = 0;
  if (s->wal_frames >= 8192 &&
      (!s->checkpoint_sent || s->wal_frames - s->checkpoint_sent >= 2048)) {
    s->checkpoint_sent = s->wal_frames;
    int code = hkn_checkpoint_request(s->checkpoint);
    if (code != SQLITE_OK) return failed(s, code);
  }
  if (s->wal_frames >= 65536) {
    // A bounded-growth fallback waits outside any transaction, then lets SQLite
    // restart the WAL. This also excludes racing checkpoint calls on the worker.
    int code = hkn_checkpoint_wait(s->checkpoint);
    if (code == SQLITE_OK) code = sqlite3_wal_checkpoint_v2(s->connection, "main", SQLITE_CHECKPOINT_RESTART, NULL, NULL);
    if (code != SQLITE_OK && code != SQLITE_BUSY) return failed(s, code);
    if (code == SQLITE_OK) { s->wal_frames = 0; s->checkpoint_sent = 0; }
  }
  return 0;
}

int hkn_probe(hkn_store *s) {
  if (s->poisoned) return -1;
  sqlite3_stmt *st = s->statement[PING];
  int code = sqlite3_step(st);
  int bad = code == SQLITE_ROW ? 0 : failed(s, code);
  sqlite3_reset(st);
  return bad;
}

const char *hkn_error(hkn_store *s) { return s->error; }
