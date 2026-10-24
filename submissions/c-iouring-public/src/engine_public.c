// engine_public.c — components/engine.h implemented on SQLite's public API only, for SQLITE_LINK=separate (and so
// for CURSED=boring): SQLite is its own translation unit, this file sees nothing but sqlite3.h, and every request is
// one of the spec's prepared statements. build.sh checks that the object references only public sqlite3_* symbols.
//
// Same contract as components/engine.c (which #includes sqlite3.c and reads B-trees directly): one transaction per
// batch, ops in arrival order, exact JSON bodies, replies held until eng_commit_batch returns. What it doesn't do:
// direct reads, the coalescing VFS, the checkpoint thread / write gate, WAL page dropping, feed coalescing.
// Checkpoints: SQLite's own autocheckpoint (ckpt_frames 0) or a PASSIVE checkpoint after the replies once the WAL
// has ckpt_frames frames (ckpt_frames > 0).
#include "engine.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef sqlite3_int64 i64;

#define POSTQ                                                                                                \
  "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS " \
  "like_count FROM posts p JOIN users u ON u.id = p.user_id "

struct eng {
  sqlite3 *db;
  eng_opts o;
  sqlite3_stmt *s_begin, *s_commit, *s_rollback, *s_health, *s_feed, *s_post, *s_exists, *s_ins, *s_like;
  int in_batch, in_txn, failed, wal_frames;
  char *fbuf;
  eng_stats st;
};

// ---------------------------------------------------------------- JSON writers (as engine.c, scalar)
static const char HEX[] = "0123456789abcdef";

size_t eng_json_escape_scalar(char *out, const char *s, size_t n) {
  char *o = out;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x20 && c != '"' && c != '\\') {
      *o++ = (char)c;
      continue;
    }
    *o++ = '\\';
    switch (c) {
      case '"': *o++ = '"'; break;
      case '\\': *o++ = '\\'; break;
      case '\b': *o++ = 'b'; break;
      case '\f': *o++ = 'f'; break;
      case '\n': *o++ = 'n'; break;
      case '\r': *o++ = 'r'; break;
      case '\t': *o++ = 't'; break;
      default:
        memcpy(o, "u00", 3);
        o[3] = HEX[c >> 4];
        o[4] = HEX[c & 15];
        o += 5;
    }
  }
  return (size_t)(o - out);
}

size_t eng_json_escape(char *out, const char *s, size_t n) {
  return eng_json_escape_scalar(out, s, n);
}

const char *eng_simd_name(void) {
  return "scalar";
}

static char *w_lit(char *p, const char *s) {
  size_t n = strlen(s);
  memcpy(p, s, n);
  return p + n;
}

static char *w_i64(char *p, i64 v) {
  char t[24];
  int k = 0;
  sqlite3_uint64 u = v < 0 ? (sqlite3_uint64)0 - (sqlite3_uint64)v : (sqlite3_uint64)v;
  if (v < 0) *p++ = '-';
  do {
    t[k++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  while (k)
    *p++ = t[--k];
  return p;
}

static char *w_str(char *p, const void *s, size_t n) {
  *p++ = '"';
  p += eng_json_escape(p, (const char *)s, n);
  *p++ = '"';
  return p;
}

static int err500(char *out, size_t *n) {
  static const char m[] = "{\"error\":\"internal server error\"}";
  memcpy(out, m, sizeof m - 1);
  *n = sizeof m - 1;
  return 500;
}

static int err404(char *out, size_t *n) {
  static const char m[] = "{\"error\":\"post not found\"}";
  memcpy(out, m, sizeof m - 1);
  *n = sizeof m - 1;
  return 404;
}

// room needed for one post object from a POSTQ row (6 bytes per escaped byte, worst case)
static size_t post_room(sqlite3_stmt *s) {
  return 120 + 6 * ((size_t)sqlite3_column_bytes(s, 1) + sqlite3_column_bytes(s, 2) + sqlite3_column_bytes(s, 3));
}

static char *w_post(char *p, sqlite3_stmt *s) {
  p = w_lit(p, "{\"id\":");
  p = w_i64(p, sqlite3_column_int64(s, 0));
  p = w_lit(p, ",\"body\":");
  p = w_str(p, sqlite3_column_text(s, 1), (size_t)sqlite3_column_bytes(s, 1));
  p = w_lit(p, ",\"created_at\":");
  p = w_str(p, sqlite3_column_text(s, 2), (size_t)sqlite3_column_bytes(s, 2));
  p = w_lit(p, ",\"author\":");
  p = w_str(p, sqlite3_column_text(s, 3), (size_t)sqlite3_column_bytes(s, 3));
  p = w_lit(p, ",\"like_count\":");
  p = w_i64(p, sqlite3_column_int64(s, 4));
  *p++ = '}';
  return p;
}

// ---------------------------------------------------------------- timestamps (exported for tests, as engine.c)
void eng_format_ts(char o[24], int64_t ms) {
  time_t s = (time_t)(ms / 1000);
  struct tm t;
  gmtime_r(&s, &t);
  char b[32];
  snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
           t.tm_min, t.tm_sec, (int)(ms % 1000));
  memcpy(o, b, 24);
}

// ---------------------------------------------------------------- open / close
void eng_default_opts(eng_opts *o) {
  memset(o, 0, sizeof *o);
  o->exclusive = 1;
  o->cache_kib = 8192;
  o->ckpt_mode = SQLITE_CHECKPOINT_PASSIVE;
}

static int wal_hook(void *arg, sqlite3 *db, const char *name, int frames) {
  (void)db;
  (void)name;
  ((eng *)arg)->wal_frames = frames;
  return SQLITE_OK;
}

static int prep(eng *e, const char *sql, sqlite3_stmt **s) {
  return sqlite3_prepare_v3(e->db, sql, -1, SQLITE_PREPARE_PERSISTENT, s, 0) != SQLITE_OK;
}

eng *eng_open(const char *path, const eng_opts *opts, char *err, size_t errn) {
  eng *e = calloc(1, sizeof *e);
  const char *why = "out of memory";
  char sql[512];
  if (!e) goto fail;
  e->o = *opts;
  why = "this engine supports only SQL reads, SQLite's VFS and no checkpoint thread (SQLITE_LINK=separate)";
  if (e->o.direct_reads || e->o.direct_writes || e->o.wal_coalesce || e->o.ckpt_thread || e->o.wal_fadvise) goto fail;
  if (sqlite3_open_v2(path, &e->db, SQLITE_OPEN_READWRITE, 0) != SQLITE_OK) {
    why = e->db ? sqlite3_errmsg(e->db) : "open failed";
    goto fail;
  }
  snprintf(sql, sizeof sql,
           "PRAGMA locking_mode=%s; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-%d;"
           "PRAGMA mmap_size=%lld; PRAGMA temp_store=MEMORY; PRAGMA wal_autocheckpoint=%d;"
           "SELECT count(*) FROM sqlite_schema;",
           e->o.exclusive ? "EXCLUSIVE" : "NORMAL", e->o.cache_kib, (long long)e->o.mmap_size,
           e->o.ckpt_frames > 0 ? 0 : 1000);
  if (sqlite3_exec(e->db, sql, 0, 0, 0) != SQLITE_OK) {
    why = sqlite3_errmsg(e->db);
    goto fail;
  }
  if (e->o.ckpt_frames > 0) sqlite3_wal_hook(e->db, wal_hook, e);
  // 3.54.0 re-prepares "WHERE p.id = ?" on every rebind; 3.54.1 fixed it. QPSG keeps the
  // plan fixed either way, as engine.c does.
  sqlite3_db_config(e->db, SQLITE_DBCONFIG_ENABLE_QPSG, 1, 0);
  why = "prepare";
  if (prep(e, "BEGIN", &e->s_begin) || prep(e, "COMMIT", &e->s_commit) || prep(e, "ROLLBACK", &e->s_rollback) ||
      prep(e, "SELECT 1", &e->s_health) ||
      prep(e, POSTQ "ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &e->s_feed) ||
      prep(e, POSTQ "WHERE p.id = ?1", &e->s_post) || prep(e, "SELECT 1 FROM posts WHERE id = ?1", &e->s_exists) ||
      prep(e, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at", &e->s_ins) ||
      prep(e, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
           &e->s_like)) {
    why = sqlite3_errmsg(e->db);
    goto fail;
  }
  return e;
fail:
  if (err) snprintf(err, errn, "%s", why);
  eng_close(e);
  return 0;
}

void eng_close(eng *e) {
  if (!e) return;
  sqlite3_stmt *all[] = {e->s_begin, e->s_commit, e->s_rollback, e->s_health, e->s_feed,
                         e->s_post,  e->s_exists, e->s_ins,      e->s_like};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
    sqlite3_finalize(all[i]);
  sqlite3_close(e->db);
  free(e->fbuf);
  free(e);
}

// ---------------------------------------------------------------- batches
static int step_done(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc == SQLITE_DONE ? 0 : -1;
}

// the batch's transaction starts with its first op (BEGIN DEFERRED: a read txn that the first write upgrades)
static int txn(eng *e) {
  if (e->in_txn) return 0;
  if (step_done(e->s_begin)) return -1;
  e->in_txn = 1;
  return 0;
}

int eng_begin_batch(eng *e) {
  if (e->in_batch) return -1;
  e->in_batch = 1;
  e->failed = 0;
  e->st.batches++;
  return 0;
}

int eng_commit_batch(eng *e) {
  if (!e->in_batch) return -1;
  int ok = !e->failed;
  if (e->in_txn) {
    if (!ok || step_done(e->s_commit)) {
      ok = 0;
      step_done(e->s_rollback);
    }
    if (!sqlite3_get_autocommit(e->db)) {  // still inside a transaction: force it closed
      ok = 0;
      sqlite3_exec(e->db, "ROLLBACK", 0, 0, 0);
    }
  }
  if (!ok) e->st.commits_failed++;
  e->in_batch = e->in_txn = e->failed = 0;
  return ok ? 0 : -1;
}

void eng_after_batch(eng *e) {
  if (e->in_batch || e->o.ckpt_frames <= 0 || e->wal_frames < e->o.ckpt_frames) return;
  int nlog = 0, nck = 0;
  if (sqlite3_wal_checkpoint_v2(e->db, "main", e->o.ckpt_mode, &nlog, &nck) == SQLITE_OK) {
    e->st.checkpoints++;
    e->st.ckpt_frames += (uint64_t)nck;
    e->wal_frames = 0;
  }
}

void eng_idle(eng *e) {
  (void)e;
}

int eng_writes_paused(eng *e) {
  (void)e;
  return 0;
}

// ---------------------------------------------------------------- reads
int eng_feed(eng *e, char *out, size_t cap, size_t *n) {
  if (!e->in_batch || cap < 64 || txn(e)) return err500(out, n);
  sqlite3_stmt *s = e->s_feed;
  char *p = w_lit(out, "{\"posts\":["), *end = out + cap;
  int k = 0, rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    if ((size_t)(end - p) < post_room(s) + 8) {
      rc = SQLITE_NOMEM;
      break;
    }
    if (k++) *p++ = ',';
    p = w_post(p, s);
  }
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) return err500(out, n);
  *p++ = ']';
  *p++ = '}';
  *n = (size_t)(p - out);
  e->st.feeds_computed++;
  return 200;
}

int eng_get_post(eng *e, int64_t id, char *out, size_t cap, size_t *n) {
  if (!e->in_batch || cap < 64 || txn(e)) return err500(out, n);
  sqlite3_stmt *s = e->s_post;
  sqlite3_bind_int64(s, 1, id);
  int rc = sqlite3_step(s), st;
  if (rc == SQLITE_ROW && cap >= post_room(s) + 16) {
    char *p = w_lit(out, "{\"post\":");
    p = w_post(p, s);
    *p++ = '}';
    *n = (size_t)(p - out);
    st = 200;
  } else {
    st = rc == SQLITE_DONE ? err404(out, n) : err500(out, n);
  }
  sqlite3_reset(s);
  return st;
}

int eng_health(eng *e, int64_t up, char *out, size_t cap, size_t *n) {
  int rc = SQLITE_ERROR, v = 0;
  if (!(e->in_batch && txn(e))) {
    rc = sqlite3_step(e->s_health);
    v = rc == SQLITE_ROW ? sqlite3_column_int(e->s_health, 0) : 0;
    sqlite3_reset(e->s_health);
  }
  if (rc == SQLITE_ROW && v == 1 && cap >= 64) {
    char *p = w_lit(out, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    p = w_i64(p, up);
    *p++ = '}';
    *n = (size_t)(p - out);
    return 200;
  }
  const char *m = sqlite3_errmsg(e->db);
  size_t ml = strlen(m) > 200 ? 200 : strlen(m);
  if (cap < 80 + 6 * ml) return err500(out, n);
  char *p = w_lit(out, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
  p = w_str(p, m, ml);
  *p++ = '}';
  *n = (size_t)(p - out);
  return 503;
}

// ---------------------------------------------------------------- writes
// a non-constraint error leaves the write transaction in an unknown state: fail the batch (as engine.c)
static void write_failed(eng *e, int rc) {
  if ((rc & 0xff) != SQLITE_CONSTRAINT && rc != SQLITE_TOOBIG) e->failed = 1;
}

int eng_like(eng *e, int64_t uid, int64_t pid, char *out, size_t cap, size_t *n) {
  if (!e->in_batch || cap < 96 || e->failed || txn(e)) return err500(out, n);
  sqlite3_bind_int64(e->s_exists, 1, pid);
  int rc = sqlite3_step(e->s_exists);
  sqlite3_reset(e->s_exists);
  if (rc == SQLITE_DONE) return err404(out, n);
  if (rc != SQLITE_ROW) return err500(out, n);
  sqlite3_bind_int64(e->s_like, 1, uid);
  sqlite3_bind_int64(e->s_like, 2, pid);
  rc = sqlite3_step(e->s_like);
  sqlite3_reset(e->s_like);
  if (rc != SQLITE_DONE) {
    write_failed(e, rc);
    return err500(out, n);
  }
  int already = sqlite3_changes(e->db) == 0;
  char *p = w_lit(out, "{\"liked\":true,\"already_liked\":");
  p = w_lit(p, already ? "true" : "false");
  p = w_lit(p, ",\"post_id\":");
  p = w_i64(p, pid);
  *p++ = '}';
  *n = (size_t)(p - out);
  return already ? 200 : 201;
}

int eng_create(eng *e, int64_t uid, const char *un, size_t unl, const char *body, size_t bn, char *out, size_t cap,
               size_t *n, int64_t *idp, char *created_at) {
  if (!e->in_batch || e->failed || cap < 160 + 6 * (bn + unl) || txn(e)) return err500(out, n);
  sqlite3_stmt *s = e->s_ins;
  sqlite3_bind_int64(s, 1, uid);
  sqlite3_bind_text(s, 2, body, (int)bn, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  if (rc != SQLITE_ROW || sqlite3_column_bytes(s, 1) != 24) {
    sqlite3_reset(s);
    write_failed(e, rc == SQLITE_ROW ? SQLITE_ERROR : sqlite3_errcode(e->db));
    return err500(out, n);
  }
  i64 id = sqlite3_column_int64(s, 0);
  char ts[24];
  memcpy(ts, sqlite3_column_text(s, 1), 24);
  rc = sqlite3_step(s);  // RETURNING: run the statement to completion
  sqlite3_reset(s);
  sqlite3_clear_bindings(s);
  if (rc != SQLITE_DONE) {
    write_failed(e, rc);
    return err500(out, n);
  }
  if (idp) *idp = id;
  if (created_at) memcpy(created_at, ts, 24);
  char *p = w_lit(out, "{\"post\":{\"id\":");
  p = w_i64(p, id);
  p = w_lit(p, ",\"body\":");
  p = w_str(p, body, bn);
  p = w_lit(p, ",\"created_at\":\"");
  memcpy(p, ts, 24);
  p += 24;
  *p++ = '"';
  p = w_lit(p, ",\"author\":");
  p = w_str(p, un, unl);
  p = w_lit(p, ",\"like_count\":0}}");
  *n = (size_t)(p - out);
  return 201;
}

// ---------------------------------------------------------------- the rest of engine.h
void eng_set_coalesce(eng *e, int on) {
  (void)e;
  (void)on;  // feed coalescing is an engine.c feature
}

int eng_feed_body(eng *e, eng_body **body) {
  if (!e->fbuf && !(e->fbuf = malloc(ENG_FEED_CAP))) {
    *body = 0;
    return 500;
  }
  size_t n;
  int st = eng_feed(e, e->fbuf, ENG_FEED_CAP, &n);
  if (!(*body = malloc(sizeof **body + n))) return 500;
  (*body)->refs = 1;
  (*body)->len = (uint32_t)n;
  memcpy((*body)->data, e->fbuf, n);
  return st;
}

void eng_body_unref(eng_body *b) {
  if (b && --b->refs == 0) free(b);
}

void eng_get_stats(eng *e, eng_stats *s) {
  *s = e->st;
}

void eng_set_clock(eng *e, int64_t (*now_ms)(void)) {
  (void)e;
  (void)now_ms;  // created_at comes from the column default here
}

struct sqlite3 *eng_db(eng *e) {
  return e->db;
}
