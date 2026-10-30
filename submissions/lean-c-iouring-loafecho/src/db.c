/* db.c — SQLite layer of lean-c-iouring-loafecho. Interface: db.h.
 *
 * One translation unit with the stock SQLite 3.54.1 amalgamation so the
 * B-tree plans can call SQLite's internal cursor API. SQLite is not patched.
 *
 * Compile-time options (build.sh may pass the same list as -D flags; they are
 * #defined here only if not already defined):
 *   SQLITE_THREADSAFE=0 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DQS=0
 *   SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_MAX_EXPR_DEPTH=0
 *   SQLITE_OMIT_DEPRECATED SQLITE_OMIT_PROGRESS_CALLBACK
 *   SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_LOAD_EXTENSION SQLITE_USE_ALLOCA
 *   SQLITE_TEMP_STORE=3 HAVE_USLEEP=1 HAVE_FDATASYNC=1 SQLITE_OMIT_AUTOINIT
 *
 * Build: gcc -O2 -std=gnu11 -Wall -c src/db.c -o obj/db.o
 * Link:  ... obj/db.o -lm   (no -ldl needed: SQLITE_OMIT_LOAD_EXTENSION)
 */
#ifndef SQLITE_THREADSAFE
#define SQLITE_THREADSAFE 0
#endif
#ifndef SQLITE_DEFAULT_MEMSTATUS
#define SQLITE_DEFAULT_MEMSTATUS 0
#endif
#ifndef SQLITE_DQS
#define SQLITE_DQS 0
#endif
#ifndef SQLITE_LIKE_DOESNT_MATCH_BLOBS
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS 1
#endif
#ifndef SQLITE_MAX_EXPR_DEPTH
#define SQLITE_MAX_EXPR_DEPTH 0
#endif
#ifndef SQLITE_OMIT_DEPRECATED
#define SQLITE_OMIT_DEPRECATED 1
#endif
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
#define SQLITE_OMIT_PROGRESS_CALLBACK 1
#endif
#ifndef SQLITE_OMIT_SHARED_CACHE
#define SQLITE_OMIT_SHARED_CACHE 1
#endif
#ifndef SQLITE_OMIT_LOAD_EXTENSION
#define SQLITE_OMIT_LOAD_EXTENSION 1
#endif
#ifndef SQLITE_USE_ALLOCA
#define SQLITE_USE_ALLOCA 1
#endif
#ifndef SQLITE_TEMP_STORE
#define SQLITE_TEMP_STORE 3
#endif
#ifndef HAVE_USLEEP
#define HAVE_USLEEP 1
#endif
#ifndef HAVE_FDATASYNC
#define HAVE_FDATASYNC 1
#endif
#ifndef SQLITE_OMIT_AUTOINIT
#define SQLITE_OMIT_AUTOINIT 1
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include "../deps/sqlite3.c"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"

enum { PLAN_BTREE = 0, PLAN_SQL = 1 };
enum { FORCE_NONE = 0, FORCE_BTREE = 1, FORCE_SQL = 2 };

#define SCRATCH_CAP 65536  /* overflow-payload copy buffer */

struct db {
  sqlite3 *h;
  Btree *bt;
  int inv_ok;            /* checkInv passed at open */
  int plan;              /* PLAN_BTREE / PLAN_SQL; never goes back to btree */
  int force;             /* test-only override */
  char line[160];        /* db_plan_line */
  /* root pages (from sqlite_master) */
  Pgno root_users, root_posts, root_likes_idx;
  KeyInfo *likes_ki;
  UnpackedRecord *likes_key;
  BtCursor *cur_posts, *cur_users, *cur_likes;
  unsigned char *scratch;
  /* insertOk state */
  int64_t max_id;
  char last_ts[64]; int last_ts_len;
  int64_t saved_max_id; char saved_ts[64]; int saved_ts_len;
  /* data_version */
  int64_t data_version;
  /* batch state */
  int in_write, batch_failed;
  /* WAL frames reported by the wal hook */
  int wal_frames;
  /* statements */
  sqlite3_stmt *st_feed, *st_post, *st_begin, *st_commit, *st_rollback,
               *st_ins_post, *st_ins_like, *st_post_exists, *st_user_exists,
               *st_dver, *st_health;
};

/* --------------------------------------------------------------- plan state */

static void set_line(db_t *d, const char *reason) {
  if (d->plan == PLAN_BTREE)
    snprintf(d->line, sizeof d->line, "invariant=ok plan=btree");
  else
    snprintf(d->line, sizeof d->line, "invariant=violated plan=sql reason=%s", reason);
}

static void to_sql(db_t *d, const char *reason) {
  if (d->plan == PLAN_SQL) return;
  d->plan = PLAN_SQL;
  set_line(d, reason);
}

const char *db_plan(const db_t *d) { return d->plan == PLAN_BTREE ? "btree" : "sql"; }
int db_invariant_ok(const db_t *d) { return d->inv_ok; }
const char *db_plan_line(const db_t *d) { return d->line; }

/* Test-only (not in db.h): force a plan for reads. 0 = none, 1 = btree, 2 = sql. */
void db_force_plan(db_t *d, int which) { d->force = which; }

/* Test-only (not in db.h): current PRAGMA data_version of this connection. */
static int read_data_version(db_t *d, int64_t *v);
int64_t db_data_version(db_t *d) { int64_t v = -1; read_data_version(d, &v); return v; }

static int use_btree(const db_t *d) {
  if (d->force == FORCE_BTREE) return 1;
  if (d->force == FORCE_SQL) return 0;
  return d->plan == PLAN_BTREE;
}

/* ------------------------------------------------------------- JSON output */

typedef struct { char *p; size_t n, cap; int bad; } out_t;

static inline void put(out_t *o, const void *s, size_t n) {
  if (o->bad || o->cap - o->n < n) { o->bad = 1; return; }
  memcpy(o->p + o->n, s, n);
  o->n += n;
}
#define PUTS(o, lit) put((o), (lit), sizeof(lit) - 1)

static void put_i64(out_t *o, int64_t v) {
  char tmp[24]; int i = 24; uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { tmp[--i] = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0) tmp[--i] = '-';
  put(o, tmp + i, (size_t)(24 - i));
}

static void put_str(out_t *o, const unsigned char *s, size_t n) {
  static const char hex[] = "0123456789abcdef";
  PUTS(o, "\"");
  size_t run = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    put(o, s + run, i - run);
    run = i + 1;
    switch (c) {
      case '"':  PUTS(o, "\\\""); break;
      case '\\': PUTS(o, "\\\\"); break;
      case '\n': PUTS(o, "\\n"); break;
      case '\r': PUTS(o, "\\r"); break;
      case '\t': PUTS(o, "\\t"); break;
      case '\b': PUTS(o, "\\b"); break;
      case '\f': PUTS(o, "\\f"); break;
      default: { char u[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]}; put(o, u, 6); }
    }
  }
  put(o, s + run, n - run);
  PUTS(o, "\"");
}

/* {"id":..,"body":..,"created_at":..,"author":..,"like_count":..} pieces */
static void put_post_head(out_t *o, int64_t id, const unsigned char *body, size_t bl,
                          const unsigned char *ts, size_t tl) {
  PUTS(o, "{\"id\":"); put_i64(o, id);
  PUTS(o, ",\"body\":"); put_str(o, body, bl);
  PUTS(o, ",\"created_at\":"); put_str(o, ts, tl);
}
static void put_post_author(out_t *o, const unsigned char *a, size_t al) {
  PUTS(o, ",\"author\":"); put_str(o, a, al);
}
static void put_post_tail(out_t *o, int64_t likes) {
  PUTS(o, ",\"like_count\":"); put_i64(o, likes); PUTS(o, "}");
}

/* ------------------------------------------------------- record decoding */

static inline int rd_varint(const unsigned char *p, const unsigned char *end, uint64_t *v) {
  uint64_t x = 0;
  for (int i = 0; i < 9; i++) {
    if (p + i >= end) return 0;
    if (i == 8) { x = (x << 8) | p[i]; *v = x; return 9; }
    x = (x << 7) | (p[i] & 0x7f);
    if (!(p[i] & 0x80)) { *v = x; return i + 1; }
  }
  return 0;
}

/* One decoded column. kind: 0 null, 1 int, 2 text, 3 other (real/blob/reserved). */
typedef struct { int kind; int64_t i; const unsigned char *z; uint32_t n; } col_t;

static uint32_t serial_len(uint64_t t) {
  static const uint8_t L[10] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0};
  if (t < 10) return L[t];
  if (t < 12) return 0xffffffffu;           /* reserved: invalid */
  if (t > 0xffffffffull) return 0xffffffffu;
  return (uint32_t)((t - 12) / 2);
}

static int64_t rd_int(const unsigned char *p, uint64_t t) {
  switch (t) {
    case 8: return 0;
    case 9: return 1;
    case 1: return (int8_t)p[0];
    case 2: return (int16_t)((p[0] << 8) | p[1]);
    case 3: return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)) >> 8;
    case 4: return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
    case 5: {
      uint64_t x = ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) | ((uint64_t)p[2] << 24) |
                   ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 8) | p[5];
      return (int64_t)(x << 16) >> 16;
    }
    default: {  /* 6 */
      uint64_t x = 0;
      for (int i = 0; i < 8; i++) x = (x << 8) | p[i];
      return (int64_t)x;
    }
  }
}

/* Decode the first ncol columns of a record. Returns 0 on success, -1 on any
 * malformation or if the record has fewer than ncol columns. */
static int decode_record(const unsigned char *rec, uint32_t len, col_t *cols, int ncol) {
  const unsigned char *end = rec + len;
  uint64_t hdr;
  int k = rd_varint(rec, end, &hdr);
  if (!k || hdr > len || hdr < (uint64_t)k) return -1;
  const unsigned char *hp = rec + k, *hend = rec + hdr, *dp = hend;
  for (int c = 0; c < ncol; c++) {
    uint64_t t;
    if (hp >= hend) return -1;
    int m = rd_varint(hp, hend, &t);
    if (!m) return -1;
    hp += m;
    uint32_t sl = serial_len(t);
    if (sl == 0xffffffffu || (uint64_t)(end - dp) < sl) return -1;
    if (t == 0) cols[c].kind = 0;
    else if (t <= 6 || t == 8 || t == 9) { cols[c].kind = 1; cols[c].i = rd_int(dp, t); }
    else if (t >= 13 && (t & 1)) { cols[c].kind = 2; cols[c].z = dp; cols[c].n = sl; }
    else cols[c].kind = 3;
    dp += sl;
  }
  return 0;
}

/* Full payload of the cursor's current entry: zero-copy when local, else
 * copied into scratch (only one overflow payload may be live at a time). */
static int cursor_payload(db_t *d, BtCursor *c, const unsigned char **p, uint32_t *n) {
  uint32_t total = sqlite3BtreePayloadSize(c), amt = 0;
  const void *loc = sqlite3BtreePayloadFetch(c, &amt);
  if (loc && amt >= total) { *p = loc; *n = total; return 0; }
  if (total > SCRATCH_CAP) return -1;
  if (sqlite3BtreePayload(c, 0, total, d->scratch) != SQLITE_OK) return -1;
  *p = d->scratch; *n = total;
  return 0;
}

/* ------------------------------------------------------------ B-tree plan */

static int bt_open(db_t *d, BtCursor *c, Pgno root, KeyInfo *ki) {
  sqlite3BtreeCursorZero(c);
  return sqlite3BtreeCursor(d->bt, root, 0, ki, c);
}

/* Render the post the posts cursor points at. Uses cur_users and cur_likes,
 * which must be open. 0 ok, -1 unexpected. */
static int bt_render_current(db_t *d, out_t *o) {
  BtCursor *pc = d->cur_posts;
  int64_t id = sqlite3BtreeIntegerKey(pc);
  const unsigned char *rec; uint32_t rl;
  if (cursor_payload(d, pc, &rec, &rl)) return -1;
  col_t pcol[4];
  if (decode_record(rec, rl, pcol, 4)) return -1;
  /* [NULL rowid alias, INT user_id, TEXT body, TEXT created_at] */
  if (pcol[0].kind != 0 || pcol[1].kind != 1 || pcol[2].kind != 2 || pcol[3].kind != 2) return -1;
  int64_t uid = pcol[1].i;
  put_post_head(o, id, pcol[2].z, pcol[2].n, pcol[3].z, pcol[3].n);

  /* author: rowid seek in users */
  int res = 0;
  if (sqlite3BtreeTableMoveto(d->cur_users, uid, 0, &res) != SQLITE_OK || res != 0) return -1;
  if (sqlite3BtreeIntegerKey(d->cur_users) != uid) return -1;
  if (cursor_payload(d, d->cur_users, &rec, &rl)) return -1;
  col_t ucol[2];
  if (decode_record(rec, rl, ucol, 2)) return -1;
  if (ucol[0].kind != 0 || ucol[1].kind != 2) return -1;
  put_post_author(o, ucol[1].z, ucol[1].n);

  /* like_count: entries (post_id = id, rowid) in likes_post_id_idx, SeekGE */
  UnpackedRecord *r = d->likes_key;
  sqlite3VdbeMemSetInt64(&r->aMem[0], id);
  r->nField = 1;
  r->default_rc = 1;      /* as OP_SeekGE: equal prefix sorts after the key */
  r->eqSeen = 0;
  r->errCode = 0;
  int64_t cnt = 0;
  int rc = sqlite3BtreeIndexMoveto(d->cur_likes, r, &res);
  if (rc != SQLITE_OK || r->errCode) return -1;
  if (res < 0) {
    rc = sqlite3BtreeNext(d->cur_likes, 0);
    if (rc == SQLITE_DONE) goto counted;
    if (rc != SQLITE_OK) return -1;
  } else if (sqlite3BtreeEof(d->cur_likes)) {
    goto counted;
  }
  for (;;) {
    if (cursor_payload(d, d->cur_likes, &rec, &rl)) return -1;
    col_t k[1];
    if (decode_record(rec, rl, k, 1)) return -1;
    if (k[0].kind != 1) {
      /* NULL sorts first (cannot be ≥ key); text/blob sort after all ints;
       * a real could equal an int — refuse to guess. */
      if (k[0].kind == 2) break;
      return -1;
    }
    if (k[0].i != id) { if (k[0].i < id) return -1; break; }
    cnt++;
    rc = sqlite3BtreeNext(d->cur_likes, 0);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_OK) return -1;
  }
counted:
  put_post_tail(o, cnt);
  return o->bad ? -1 : 0;
}

/* Run fn inside the right snapshot with the three cursors open.
 * Returns fn's result, or -1 on error, or -2 when the data_version moved
 * under us (caller re-serves with SQL; the plan has been switched). */
typedef int (*bt_fn)(db_t *d, void *arg);

static int bt_run(db_t *d, bt_fn fn, void *arg) {
  int own_txn = 0, rc, out = -1;
  int st = sqlite3BtreeTxnState(d->bt);
  if (st == SQLITE_TXN_NONE) {
    if (sqlite3BtreeBeginTrans(d->bt, 0, 0) != SQLITE_OK) return -1;
    own_txn = 1;
  }
  if (d->force != FORCE_BTREE) {
    u32 v = 0;
    sqlite3BtreeGetMeta(d->bt, BTREE_DATA_VERSION, &v);
    if ((int64_t)v != d->data_version) {
      to_sql(d, "data_version");
      out = -2;
      goto done;
    }
  }
  if ((rc = bt_open(d, d->cur_posts, d->root_posts, 0)) != SQLITE_OK) goto done;
  if ((rc = bt_open(d, d->cur_users, d->root_users, 0)) != SQLITE_OK) goto close1;
  if ((rc = bt_open(d, d->cur_likes, d->root_likes_idx, d->likes_ki)) != SQLITE_OK) goto close2;
  out = fn(d, arg);
  sqlite3BtreeCloseCursor(d->cur_likes);
close2:
  sqlite3BtreeCloseCursor(d->cur_users);
close1:
  sqlite3BtreeCloseCursor(d->cur_posts);
done:
  if (own_txn && sqlite3BtreeCommit(d->bt) != SQLITE_OK) out = -1;
  return out;
}

static int bt_feed(db_t *d, void *arg) {
  out_t *o = arg;
  int res = 0, rc;
  PUTS(o, "{\"posts\":[");
  if (sqlite3BtreeLast(d->cur_posts, &res) != SQLITE_OK) return -1;
  if (!res) {
    for (int i = 0; i < 20; i++) {
      if (i) PUTS(o, ",");
      if (bt_render_current(d, o)) return -1;
      if (i == 19) break;
      rc = sqlite3BtreePrevious(d->cur_posts, 0);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_OK) return -1;
    }
  }
  PUTS(o, "]}");
  return o->bad ? -1 : 0;
}

typedef struct { out_t *o; int64_t id; int found; } post_arg;

static int bt_post(db_t *d, void *arg) {
  post_arg *a = arg;
  int res = 0;
  if (sqlite3BtreeTableMoveto(d->cur_posts, a->id, 0, &res) != SQLITE_OK) return -1;
  if (res != 0) { a->found = 0; return 0; }
  if (sqlite3BtreeIntegerKey(d->cur_posts) != a->id) return -1;
  a->found = 1;
  PUTS(a->o, "{\"post\":");
  if (bt_render_current(d, a->o)) return -1;
  PUTS(a->o, "}");
  return a->o->bad ? -1 : 0;
}

/* --------------------------------------------------------------- SQL plan */

static void sql_render_row(sqlite3_stmt *s, out_t *o) {
  put_post_head(o, sqlite3_column_int64(s, 0),
                sqlite3_column_text(s, 1), (size_t)sqlite3_column_bytes(s, 1),
                sqlite3_column_text(s, 2), (size_t)sqlite3_column_bytes(s, 2));
  put_post_author(o, sqlite3_column_text(s, 3), (size_t)sqlite3_column_bytes(s, 3));
  put_post_tail(o, sqlite3_column_int64(s, 4));
}

static int sql_feed(db_t *d, out_t *o) {
  sqlite3_stmt *s = d->st_feed;
  int rc, i = 0;
  PUTS(o, "{\"posts\":[");
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    if (i++) PUTS(o, ",");
    sql_render_row(s, o);
  }
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) return -1;
  PUTS(o, "]}");
  return o->bad ? -1 : 0;
}

/* 1 found, 0 not found, -1 error */
static int sql_post(db_t *d, int64_t id, out_t *o) {
  sqlite3_stmt *s = d->st_post;
  sqlite3_bind_int64(s, 1, id);
  int rc = sqlite3_step(s), out;
  if (rc == SQLITE_ROW) {
    PUTS(o, "{\"post\":");
    sql_render_row(s, o);
    PUTS(o, "}");
    out = o->bad ? -1 : 1;
  } else out = rc == SQLITE_DONE ? 0 : -1;
  sqlite3_reset(s);
  return out;
}

/* ------------------------------------------------------------------ reads */

int db_feed_json(db_t *d, char *buf, size_t cap) {
  out_t o = {buf, 0, cap, 0};
  if (use_btree(d)) {
    int r = bt_run(d, bt_feed, &o);
    if (r == 0) return (int)o.n;
    if (r == -1) {
      if (!o.bad && d->force != FORCE_BTREE) to_sql(d, "btree-error");
      return -1;
    }
    o.n = 0; o.bad = 0;   /* r == -2: data_version moved; serve with SQL */
  }
  return sql_feed(d, &o) ? -1 : (int)o.n;
}

int db_post_json(db_t *d, int64_t id, char *buf, size_t cap) {
  out_t o = {buf, 0, cap, 0};
  if (use_btree(d)) {
    post_arg a = {&o, id, 0};
    int r = bt_run(d, bt_post, &a);
    if (r == 0) return a.found ? (int)o.n : 0;
    if (r == -1) {
      if (!o.bad && d->force != FORCE_BTREE) to_sql(d, "btree-error");
      return -1;
    }
    o.n = 0; o.bad = 0;
  }
  int r = sql_post(d, id, &o);
  return r < 0 ? -1 : r == 0 ? 0 : (int)o.n;
}

int db_health(db_t *d) {
  int rc = sqlite3_step(d->st_health);
  sqlite3_reset(d->st_health);
  return rc == SQLITE_ROW ? 0 : -1;
}

/* ---------------------------------------------------------------- batches */

static int read_data_version(db_t *d, int64_t *v) {
  int rc = sqlite3_step(d->st_dver);
  if (rc == SQLITE_ROW) *v = sqlite3_column_int64(d->st_dver, 0);
  sqlite3_reset(d->st_dver);
  return rc == SQLITE_ROW ? 0 : -1;
}

int db_batch_begin(db_t *d) {
  int64_t v;
  d->batch_failed = 0;
  if (read_data_version(d, &v)) return -1;
  if (v != d->data_version) { d->data_version = v; to_sql(d, "data_version"); }
  return 0;
}

static int step_once(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static void restore_ins_state(db_t *d) {
  d->max_id = d->saved_max_id;
  memcpy(d->last_ts, d->saved_ts, sizeof d->last_ts);
  d->last_ts_len = d->saved_ts_len;
}

static int ensure_write(db_t *d) {
  if (d->batch_failed) return -1;
  if (d->in_write) {
    if (sqlite3_get_autocommit(d->h)) {   /* transaction was rolled back under us */
      d->batch_failed = 1;
      return -1;
    }
    return 0;
  }
  if (step_once(d->st_begin) != SQLITE_DONE) return -1;
  d->in_write = 1;
  d->saved_max_id = d->max_id;
  memcpy(d->saved_ts, d->last_ts, sizeof d->saved_ts);
  d->saved_ts_len = d->last_ts_len;
  return 0;
}

int db_batch_commit(db_t *d) {
  if (!d->in_write) { d->batch_failed = 0; return 0; }
  int ok = !d->batch_failed && !sqlite3_get_autocommit(d->h) &&
           step_once(d->st_commit) == SQLITE_DONE;
  if (!ok) {
    if (!sqlite3_get_autocommit(d->h)) step_once(d->st_rollback);
    restore_ins_state(d);
  }
  d->in_write = 0;
  d->batch_failed = 0;
  return ok ? 0 : -1;
}

/* ----------------------------------------------------------------- writes */

static int ts_cmp(const char *a, int al, const char *b, int bl) {
  int m = al < bl ? al : bl;
  int c = memcmp(a, b, (size_t)m);
  return c ? c : al - bl;
}

static int user_exists(db_t *d, int64_t uid) {
  sqlite3_bind_int64(d->st_user_exists, 1, uid);
  int rc = sqlite3_step(d->st_user_exists);
  sqlite3_reset(d->st_user_exists);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int db_insert_post(db_t *d, int64_t user_id, const char *username, size_t ulen,
                   const char *body, size_t blen, char *buf, size_t cap) {
  if (ensure_write(d)) return -1;
  sqlite3_stmt *s = d->st_ins_post;
  sqlite3_bind_int64(s, 1, user_id);
  sqlite3_bind_text(s, 2, body, (int)blen, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  if (rc != SQLITE_ROW || sqlite3_column_type(s, 1) != SQLITE_TEXT) {
    sqlite3_reset(s); sqlite3_clear_bindings(s);
    return -1;
  }
  int64_t id = sqlite3_column_int64(s, 0);
  char ts[64];
  int tl = sqlite3_column_bytes(s, 1);
  if (tl <= 0 || tl >= (int)sizeof ts) { sqlite3_reset(s); sqlite3_clear_bindings(s); return -1; }
  memcpy(ts, sqlite3_column_text(s, 1), (size_t)tl);
  ts[tl] = 0;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {}
  int rrc = sqlite3_reset(s);
  sqlite3_clear_bindings(s);
  if (rc != SQLITE_DONE || rrc != SQLITE_OK) return -1;

  /* insertOk */
  if (d->plan == PLAN_BTREE) {
    int ue = user_exists(d, user_id);
    if (ue < 0) return -1;
    if (id == d->max_id + 1 && ts_cmp(ts, tl, d->last_ts, d->last_ts_len) >= 0 && ue == 1) {
      d->max_id = id;
      memcpy(d->last_ts, ts, (size_t)tl + 1);
      d->last_ts_len = tl;
    } else {
      to_sql(d, "insertOk");
    }
  }

  out_t o = {buf, 0, cap, 0};
  PUTS(&o, "{\"post\":");
  put_post_head(&o, id, (const unsigned char *)body, blen, (const unsigned char *)ts, (size_t)tl);
  put_post_author(&o, (const unsigned char *)username, ulen);
  put_post_tail(&o, 0);
  PUTS(&o, "}");
  return o.bad ? -1 : (int)o.n;
}

int db_like(db_t *d, int64_t user_id, int64_t post_id, int *already_liked) {
  if (ensure_write(d)) return -1;
  sqlite3_stmt *s = d->st_post_exists;
  sqlite3_bind_int64(s, 1, post_id);
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  if (rc == SQLITE_DONE) return 0;
  if (rc != SQLITE_ROW) return -1;
  s = d->st_ins_like;
  sqlite3_bind_int64(s, 1, user_id);
  sqlite3_bind_int64(s, 2, post_id);
  rc = sqlite3_step(s);
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) return -1;
  if (already_liked) *already_liked = sqlite3_changes(d->h) == 0;
  return 1;
}

/* ------------------------------------------------------------ maintenance */

static int wal_hook(void *arg, sqlite3 *h, const char *name, int nframes) {
  (void)h; (void)name;
  ((db_t *)arg)->wal_frames = nframes;
  return SQLITE_OK;
}

int db_maybe_checkpoint(db_t *d, int min_frames) {
  if (d->in_write || !sqlite3_get_autocommit(d->h)) return 0;
  if (min_frames > 0 && d->wal_frames <= min_frames) return 0;
  int nlog = 0, nckpt = 0;
  sqlite3_wal_checkpoint_v2(d->h, NULL, SQLITE_CHECKPOINT_PASSIVE, &nlog, &nckpt);
  d->wal_frames = 0;
  return 0;
}

/* -------------------------------------------------------------- open/close */

static int exec(sqlite3 *h, const char *sql) {
  return sqlite3_exec(h, sql, 0, 0, 0);
}

static int prep(db_t *d, sqlite3_stmt **s, const char *sql, char *err, size_t errlen) {
  if (sqlite3_prepare_v3(d->h, sql, -1, SQLITE_PREPARE_PERSISTENT, s, 0) != SQLITE_OK) {
    snprintf(err, errlen, "prepare failed: %s: %s", sqlite3_errmsg(d->h), sql);
    return -1;
  }
  return 0;
}

static int64_t q_int(sqlite3 *h, const char *sql, int64_t dflt) {
  sqlite3_stmt *s; int64_t v = dflt;
  if (sqlite3_prepare_v2(h, sql, -1, &s, 0) != SQLITE_OK) return dflt;
  if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
    v = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return v;
}

/* Column names of a table/index, comma-joined, into out. */
static void q_cols(sqlite3 *h, const char *pragma, int col, char *out, size_t cap) {
  sqlite3_stmt *s; size_t n = 0; out[0] = 0;
  if (sqlite3_prepare_v2(h, pragma, -1, &s, 0) != SQLITE_OK) return;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *nm = (const char *)sqlite3_column_text(s, col);
    if (!nm) nm = "";
    n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s%s", n ? "," : "", nm);
    if (n >= cap) break;
  }
  sqlite3_finalize(s);
}

/* checkInv + layout checks. Returns NULL if the B-tree plan may be used,
 * else a short reason. Fills max_id / last_ts. -1 in *fatal on SQL error. */
static const char *check_inv(db_t *d, int *fatal) {
  sqlite3 *h = d->h;
  *fatal = 0;
  /* layout: the plan decodes records positionally */
  char cols[256];
  q_cols(h, "PRAGMA table_info(posts)", 1, cols, sizeof cols);
  if (strcmp(cols, "id,user_id,body,created_at")) return "schema";
  q_cols(h, "PRAGMA table_info(users)", 1, cols, sizeof cols);
  if (strcmp(cols, "id,username,created_at")) return "schema";
  q_cols(h, "PRAGMA index_info(likes_post_id_idx)", 2, cols, sizeof cols);
  if (strcmp(cols, "post_id")) return "schema";
  if (q_int(h, "SELECT count(*) FROM pragma_index_list('likes') WHERE name='likes_post_id_idx' AND partial=0", 0) != 1)
    return "schema";
  {
    sqlite3_stmt *s; int okenc = 0;
    if (sqlite3_prepare_v2(h, "PRAGMA encoding", -1, &s, 0) == SQLITE_OK) {
      if (sqlite3_step(s) == SQLITE_ROW) okenc = !strcmp((const char *)sqlite3_column_text(s, 0), "UTF-8");
      sqlite3_finalize(s);
    }
    if (!okenc) return "encoding";
  }

  /* dense + mono in one scan over posts in rowid order */
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(h, "SELECT id, created_at FROM posts ORDER BY id", -1, &s, 0) != SQLITE_OK) {
    *fatal = 1; return "sql-error";
  }
  int64_t expect = 1;
  char prev[64]; int pl = 0; prev[0] = 0;
  const char *why = NULL;
  int rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(s, 0);
    if (id != expect) { why = "not-dense"; break; }
    if (sqlite3_column_type(s, 1) != SQLITE_TEXT) { why = "created_at-type"; break; }
    const char *ts = (const char *)sqlite3_column_text(s, 1);
    int tl = sqlite3_column_bytes(s, 1);
    if (tl >= (int)sizeof prev) { why = "created_at-type"; break; }
    if (expect > 1 && ts_cmp(ts, tl, prev, pl) < 0) { why = "created_at-order"; break; }
    memcpy(prev, ts, (size_t)tl); prev[tl] = 0; pl = tl;
    expect++;
  }
  if (!why && rc != SQLITE_DONE) { *fatal = 1; why = "sql-error"; }
  sqlite3_finalize(s);
  if (why) return why;
  d->max_id = expect - 1;
  memcpy(d->last_ts, prev, sizeof prev);
  d->last_ts_len = pl;

  /* authors: every posts.user_id has a users row */
  int64_t missing = q_int(h,
      "SELECT EXISTS (SELECT 1 FROM posts p LEFT JOIN users u ON u.id = p.user_id WHERE u.id IS NULL)", -1);
  if (missing < 0) { *fatal = 1; return "sql-error"; }
  if (missing) return "missing-author";
  return NULL;
}

static Pgno root_of(sqlite3 *h, const char *name) {
  sqlite3_stmt *s; int64_t v = 0;
  if (sqlite3_prepare_v2(h, "SELECT rootpage FROM sqlite_schema WHERE name=?", -1, &s, 0) != SQLITE_OK) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return v > 0 && v < 0x7fffffff ? (Pgno)v : 0;
}

void db_close(db_t *d) {
  if (!d) return;
  if (d->h) {
    if (d->in_write && !sqlite3_get_autocommit(d->h)) exec(d->h, "ROLLBACK");
    sqlite3_stmt **all[] = {&d->st_feed, &d->st_post, &d->st_begin, &d->st_commit, &d->st_rollback,
                            &d->st_ins_post, &d->st_ins_like, &d->st_post_exists, &d->st_user_exists,
                            &d->st_dver, &d->st_health};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) sqlite3_finalize(*all[i]);
    if (d->likes_key) sqlite3DbFree(d->h, d->likes_key);
    if (d->likes_ki) sqlite3KeyInfoUnref(d->likes_ki);
    sqlite3_close(d->h);
  }
  free(d->cur_posts); free(d->cur_users); free(d->cur_likes);
  free(d->scratch);
  free(d);
}

db_t *db_open(const char *path, char *err, size_t errlen) {
  if (err && errlen) err[0] = 0;
  char dummy[8];
  if (!err) { err = dummy; errlen = sizeof dummy; }
  if (sqlite3_initialize() != SQLITE_OK) { snprintf(err, errlen, "sqlite3_initialize failed"); return NULL; }
  db_t *d = calloc(1, sizeof *d);
  if (!d) { snprintf(err, errlen, "out of memory"); return NULL; }
  if (sqlite3_open_v2(path, &d->h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    snprintf(err, errlen, "open %s: %s", path, d->h ? sqlite3_errmsg(d->h) : "failed");
    goto fail;
  }
  sqlite3_extended_result_codes(d->h, 1);
  sqlite3_busy_timeout(d->h, 5000);
  static const char *pragmas[] = {
    "PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL", "PRAGMA mmap_size=1073741824",
    "PRAGMA cache_size=-65536", "PRAGMA temp_store=memory", "PRAGMA wal_autocheckpoint=0",
    "PRAGMA foreign_keys=OFF",
  };
  for (size_t i = 0; i < sizeof pragmas / sizeof pragmas[0]; i++) {
    if (exec(d->h, pragmas[i]) != SQLITE_OK) {
      snprintf(err, errlen, "%s: %s", pragmas[i], sqlite3_errmsg(d->h));
      goto fail;
    }
  }
  sqlite3_wal_hook(d->h, wal_hook, d);

#define POST_SQL "SELECT p.id, p.body, p.created_at, u.username, " \
                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count " \
                 "FROM posts p JOIN users u ON u.id = p.user_id "
  if (prep(d, &d->st_feed, POST_SQL "ORDER BY p.created_at DESC, p.id DESC LIMIT 20", err, errlen) ||
      prep(d, &d->st_post, POST_SQL "WHERE p.id = ?", err, errlen) ||
      prep(d, &d->st_begin, "BEGIN IMMEDIATE", err, errlen) ||
      prep(d, &d->st_commit, "COMMIT", err, errlen) ||
      prep(d, &d->st_rollback, "ROLLBACK", err, errlen) ||
      prep(d, &d->st_ins_post, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at", err, errlen) ||
      prep(d, &d->st_ins_like, "INSERT INTO likes (user_id, post_id) VALUES (?, ?) "
                               "ON CONFLICT (user_id, post_id) DO NOTHING", err, errlen) ||
      prep(d, &d->st_post_exists, "SELECT 1 FROM posts WHERE id = ?", err, errlen) ||
      prep(d, &d->st_user_exists, "SELECT 1 FROM users WHERE id = ?", err, errlen) ||
      prep(d, &d->st_dver, "PRAGMA data_version", err, errlen) ||
      prep(d, &d->st_health, "SELECT 1", err, errlen))
    goto fail;
#undef POST_SQL

  if (read_data_version(d, &d->data_version)) {
    snprintf(err, errlen, "data_version: %s", sqlite3_errmsg(d->h));
    goto fail;
  }

  /* B-tree plan resources (schema is loaded by the prepares above) */
  d->bt = d->h->aDb[0].pBt;
  d->root_users = root_of(d->h, "users");
  d->root_posts = root_of(d->h, "posts");
  d->root_likes_idx = root_of(d->h, "likes_post_id_idx");
  int csz = sqlite3BtreeCursorSize();
  d->cur_posts = calloc(1, (size_t)csz);
  d->cur_users = calloc(1, (size_t)csz);
  d->cur_likes = calloc(1, (size_t)csz);
  d->scratch = malloc(SCRATCH_CAP);
  d->likes_ki = sqlite3KeyInfoAlloc(d->h, 1, 1);  /* (post_id) + rowid, BINARY, ASC */
  if (d->likes_ki) {
    d->likes_key = sqlite3VdbeAllocUnpackedRecord(d->likes_ki);
    if (d->likes_key) {
      memset(d->likes_key->aMem, 0, sizeof(Mem) * 2);
      d->likes_key->aMem[0].flags = MEM_Null;
      d->likes_key->aMem[0].db = d->h;
    }
  }
  if (!d->cur_posts || !d->cur_users || !d->cur_likes || !d->scratch || !d->likes_ki || !d->likes_key) {
    snprintf(err, errlen, "out of memory");
    goto fail;
  }

  int fatal = 0;
  const char *why;
  if (!d->bt || !d->root_users || !d->root_posts || !d->root_likes_idx) why = "schema";
  else why = check_inv(d, &fatal);
  if (fatal) { snprintf(err, errlen, "checkInv: %s", sqlite3_errmsg(d->h)); goto fail; }
  if (why) { d->inv_ok = 0; d->plan = PLAN_SQL; }
  else     { d->inv_ok = 1; d->plan = PLAN_BTREE; }
  set_line(d, why);
  return d;

fail:
  db_close(d);
  return NULL;
}

/* -------------------------------------------------------------- selfcheck */

int db_selfcheck(const char *path, unsigned seed) {
  char err[512];
  db_t *d = db_open(path, err, sizeof err);
  if (!d) { printf("selfcheck: cannot open: %s\n", err); fflush(stdout); return -1; }
  if (!d->inv_ok) { printf("%s\n", db_plan_line(d)); fflush(stdout); db_close(d); return -1; }
  size_t cap = 1 << 20;
  char *a = malloc(cap), *b = malloc(cap);
  int mism = 0;
  if (!a || !b) { free(a); free(b); db_close(d); return -1; }

  d->force = FORCE_BTREE; int la = db_feed_json(d, a, cap);
  d->force = FORCE_SQL;   int lb = db_feed_json(d, b, cap);
  if (la < 0 || la != lb || memcmp(a, b, (size_t)la)) {
    printf("selfcheck mismatch feed btree_len=%d sql_len=%d\n", la, lb); mism++;
  }
  uint64_t x = seed;
  int64_t maxid = d->max_id > 0 ? d->max_id : 1;
  for (int i = 0; i < 10003; i++) {
    int64_t id;
    if (i == 0) id = 1; else if (i == 1) id = 250000; else if (i == 2) id = 500000;
    else {
      x = x * 6364136223846793005ull + 1442695040888963407ull;
      id = 1 + (int64_t)((x >> 33) % (uint64_t)maxid);
    }
    d->force = FORCE_BTREE; la = db_post_json(d, id, a, cap);
    d->force = FORCE_SQL;   lb = db_post_json(d, id, b, cap);
    if (la < 0 || la != lb || memcmp(a, b, (size_t)la)) {
      printf("selfcheck mismatch post id=%lld btree_len=%d sql_len=%d\n", (long long)id, la, lb);
      mism++;
    }
  }
  printf("selfcheck compared btree plan vs reference SQL: feed, posts 1/250000/500000, "
         "10000 random ids (seed %u, max id %lld), 10004 comparisons\n", seed, (long long)maxid);
  printf("selfcheck mismatches=%d\n", mism);
  fflush(stdout);
  free(a); free(b);
  db_close(d);
  return mism;
}
