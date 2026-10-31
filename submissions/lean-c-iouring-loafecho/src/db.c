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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
  int cur_open;          /* the three cursors are open (inside a batch txn) */
  unsigned char *scratch;
  /* insertOk state */
  int64_t max_id;
  char last_ts[64]; int last_ts_len;
  int64_t saved_max_id; char saved_ts[64]; int saved_ts_len;
  /* data_version */
  int64_t data_version;
  /* batch state: in_batch = db_batch_begin opened the batch txn (BEGIN +
   * read snapshot); in_write = a write ran in it */
  int in_batch, in_write, batch_failed;
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

static void bt_close_cursors(db_t *d);

static void to_sql(db_t *d, const char *reason) {
  bt_close_cursors(d);   /* no B-tree cursor outlives the btree plan */
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

/* Test-only (not in db.h): B-tree txn state (SQLITE_TXN_*) and autocommit. */
int db_txn_state(db_t *d) { return sqlite3BtreeTxnState(d->bt); }
int db_autocommit(db_t *d) { return sqlite3_get_autocommit(d->h); }

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

static const char digits2[201] =
  "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
  "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
  "8081828384858687888990919293949596979899";

/* Decimal int64, two digits per division step. */
static inline void put_i64(out_t *o, int64_t v) {
  if ((uint64_t)v < 10) { char ch = (char)('0' + v); put(o, &ch, 1); return; }
  char tmp[24]; int i = 24; uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  while (u >= 100) {
    unsigned r = (unsigned)(u % 100); u /= 100;
    i -= 2; memcpy(tmp + i, digits2 + 2 * r, 2);
  }
  if (u >= 10) { i -= 2; memcpy(tmp + i, digits2 + 2 * u, 2); }
  else tmp[--i] = (char)('0' + u);
  if (v < 0) tmp[--i] = '-';
  put(o, tmp + i, (size_t)(24 - i));
}

/* 1 for bytes JSON strings must escape: controls, '"', '\\'. */
static const unsigned char needs_escape[256] = {
  [0x00 ... 0x1f] = 1, ['"'] = 1, ['\\'] = 1,
};

/* Write the escape for one byte c (needs_escape[c] == 1); returns new end. */
static inline char *esc_byte(char *w, unsigned char c) {
  static const char hex[] = "0123456789abcdef";
  switch (c) {
    case '"':  w[0] = '\\'; w[1] = '"';  return w + 2;
    case '\\': w[0] = '\\'; w[1] = '\\'; return w + 2;
    case '\n': w[0] = '\\'; w[1] = 'n';  return w + 2;
    case '\r': w[0] = '\\'; w[1] = 'r';  return w + 2;
    case '\t': w[0] = '\\'; w[1] = 't';  return w + 2;
    case '\b': w[0] = '\\'; w[1] = 'b';  return w + 2;
    case '\f': w[0] = '\\'; w[1] = 'f';  return w + 2;
    default:
      w[0] = '\\'; w[1] = 'u'; w[2] = '0'; w[3] = '0'; w[4] = hex[c >> 4]; w[5] = hex[c & 15];
      return w + 6;
  }
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PUT_STR_SWAR 1
/* Exact per-byte mask: 0x80 in each byte of x that is < 0x20, '"' or '\\'. */
static inline uint64_t esc_mask8(uint64_t x) {
  const uint64_t L7 = 0x7f7f7f7f7f7f7f7fULL;
#define ZMASK(v) (~((((v) & L7) + L7) | (v) | L7))
  uint64_t a = x & 0xe0e0e0e0e0e0e0e0ULL;   /* byte zero iff < 0x20 */
  uint64_t b = x ^ 0x2222222222222222ULL;   /* '"'  */
  uint64_t c = x ^ 0x5c5c5c5c5c5c5c5cULL;   /* '\\' */
  return ZMASK(a) | ZMASK(b) | ZMASK(c);
#undef ZMASK
}
#endif

/* Bounds-checked path: used only when the worst case (6 bytes per input
 * byte + 2 quotes) might not fit, so o->bad ends up exactly as if every
 * piece had gone through put(). */
static void put_str_slow(out_t *o, const unsigned char *s, size_t n) {
  PUTS(o, "\"");
  size_t run = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (!needs_escape[c]) continue;
    put(o, s + run, i - run);
    run = i + 1;
    char e[6];
    put(o, e, (size_t)(esc_byte(e, c) - e));
  }
  put(o, s + run, n - run);
  PUTS(o, "\"");
}

static inline void put_str(out_t *o, const unsigned char *s, size_t n) {
  if (__builtin_expect(o->bad || n > (SIZE_MAX - 2) / 6 || o->cap - o->n < 6 * n + 2, 0)) {
    put_str_slow(o, s, n);
    return;
  }
  char *w = o->p + o->n;
  *w++ = '"';
  size_t i = 0, run = 0;
  for (;;) {
#ifdef PUT_STR_SWAR
    while (i + 8 <= n) {
      uint64_t x; memcpy(&x, s + i, 8);
      uint64_t m = esc_mask8(x);
      if (m) { i += (size_t)__builtin_ctzll(m) >> 3; goto esc; }
      i += 8;
    }
#endif
    while (i < n && !needs_escape[s[i]]) i++;
    if (i >= n) break;
#ifdef PUT_STR_SWAR
  esc:
#endif
    memcpy(w, s + run, i - run); w += i - run;
    w = esc_byte(w, s[i]);
    run = ++i;
  }
  memcpy(w, s + run, n - run); w += n - run;
  *w++ = '"';
  o->n = (size_t)(w - o->p);
}

/* {"id":..,"body":..,"created_at":..,"author":..,"like_count":..} pieces */
static inline void put_post_head(out_t *o, int64_t id, const unsigned char *body, size_t bl,
                          const unsigned char *ts, size_t tl) {
  PUTS(o, "{\"id\":"); put_i64(o, id);
  PUTS(o, ",\"body\":"); put_str(o, body, bl);
  PUTS(o, ",\"created_at\":"); put_str(o, ts, tl);
}
static inline void put_post_author(out_t *o, const unsigned char *a, size_t al) {
  PUTS(o, ",\"author\":"); put_str(o, a, al);
}
static inline void put_post_tail(out_t *o, int64_t likes) {
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

static inline uint32_t serial_len(uint64_t t) {
  static const uint8_t L[10] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0};
  if (t < 10) return L[t];
  if (t < 12) return 0xffffffffu;           /* reserved: invalid */
  if (t > 0xffffffffull) return 0xffffffffu;
  return (uint32_t)((t - 12) / 2);
}

static inline int64_t rd_int(const unsigned char *p, uint64_t t) {
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
static inline int decode_record(const unsigned char *rec, uint32_t len, col_t *cols, int ncol) {
  const unsigned char *end = rec + len;
  uint64_t hdr;
  int k;
  /* one-byte varint fast path: same result rd_varint gives for p[0] < 0x80 */
  if (__builtin_expect(len > 0 && rec[0] < 0x80, 1)) { hdr = rec[0]; k = 1; }
  else k = rd_varint(rec, end, &hdr);
  if (__builtin_expect(!k || hdr > len || hdr < (uint64_t)k, 0)) return -1;
  const unsigned char *hp = rec + k, *hend = rec + hdr, *dp = hend;
  for (int c = 0; c < ncol; c++) {
    uint64_t t;
    if (__builtin_expect(hp >= hend, 0)) return -1;
    if (__builtin_expect(hp[0] < 0x80, 1)) { t = hp[0]; hp++; }
    else {
      int m = rd_varint(hp, hend, &t);
      if (!m) return -1;
      hp += m;
    }
    uint32_t sl = serial_len(t);
    if (__builtin_expect(sl == 0xffffffffu || (uint64_t)(end - dp) < sl, 0)) return -1;
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
static inline int cursor_payload(db_t *d, BtCursor *c, const unsigned char **p, uint32_t *n) {
  uint32_t total = sqlite3BtreePayloadSize(c), amt = 0;
  const void *loc = sqlite3BtreePayloadFetch(c, &amt);
  if (__builtin_expect(loc && amt >= total, 1)) { *p = loc; *n = total; return 0; }
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

/* Open the three read cursors (all or none). 0 ok, else an SQLite code. */
static int bt_open_cursors(db_t *d) {
  int rc;
  if ((rc = bt_open(d, d->cur_posts, d->root_posts, 0)) != SQLITE_OK) return rc;
  if ((rc = bt_open(d, d->cur_users, d->root_users, 0)) != SQLITE_OK) goto close1;
  if ((rc = bt_open(d, d->cur_likes, d->root_likes_idx, d->likes_ki)) != SQLITE_OK) goto close2;
  d->cur_open = 1;
  return SQLITE_OK;
close2:
  sqlite3BtreeCloseCursor(d->cur_users);
close1:
  sqlite3BtreeCloseCursor(d->cur_posts);
  return rc;
}

static void bt_close_cursors(db_t *d) {
  if (!d->cur_open) return;
  sqlite3BtreeCloseCursor(d->cur_likes);
  sqlite3BtreeCloseCursor(d->cur_users);
  sqlite3BtreeCloseCursor(d->cur_posts);
  d->cur_open = 0;
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
 * under us (caller re-serves with SQL; the plan has been switched).
 *
 * Inside a batch (db_batch_begin opened the batch txn and it is still live)
 * the batch's snapshot is used and the cursors are opened on the first call
 * and kept open until db_batch_commit; the data_version was checked against
 * this same snapshot by db_batch_begin. fn re-positions every cursor it uses
 * (Last / TableMoveto / IndexMoveto), so no request depends on where the
 * previous one left a cursor; a write in between saves the cursors on the
 * written tree (CURSOR_REQUIRESEEK) and the next moveto re-seeks.
 * Outside a batch (selfcheck, dbtest, a failed db_batch_begin) the call runs
 * its own read txn (unless one is open) with its own cursors. */
typedef int (*bt_fn)(db_t *d, void *arg);

static int bt_run(db_t *d, bt_fn fn, void *arg) {
  int own_txn = 0, out = -1;
  if (d->in_batch) {
    if (!sqlite3_get_autocommit(d->h) && sqlite3BtreeTxnState(d->bt) != SQLITE_TXN_NONE) {
      if (!d->cur_open && bt_open_cursors(d) != SQLITE_OK) return -1;
      out = fn(d, arg);
      if (out != 0) bt_close_cursors(d);
      return out;
    }
    /* the batch txn was rolled back under us: drop the cursor set and serve
     * this request in its own read txn */
    bt_close_cursors(d);
  }
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
  if (bt_open_cursors(d) != SQLITE_OK) goto done;
  out = fn(d, arg);
  bt_close_cursors(d);
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

static int step_once(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

/* One transaction per batch: a deferred BEGIN turns autocommit off (so no
 * statement halt ends the txn), then the read snapshot is taken here, once,
 * and its data_version (the same value PRAGMA data_version reports) is
 * compared with the last one seen. The first write in the batch upgrades
 * the txn to a write txn; db_batch_commit always ends it. */
int db_batch_commit(db_t *d);

int db_batch_begin(db_t *d) {
  if (d->in_batch || d->in_write) db_batch_commit(d);   /* unpaired begin */
  d->batch_failed = 0;
  if (step_once(d->st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(d->bt, 0, 0) != SQLITE_OK) {
    if (!sqlite3_get_autocommit(d->h)) step_once(d->st_rollback);
    return -1;
  }
  u32 v = 0;
  sqlite3BtreeGetMeta(d->bt, BTREE_DATA_VERSION, &v);
  d->in_batch = 1;
  if ((int64_t)v != d->data_version) { d->data_version = (int64_t)v; to_sql(d, "data_version"); }
  return 0;
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
  /* The batch txn is already open (deferred BEGIN in db_batch_begin); the
   * first write statement upgrades it to a write txn. Only if no batch txn
   * is open (db_batch_begin failed, or it was rolled back under us) is a
   * BEGIN run here. */
  if (sqlite3_get_autocommit(d->h) && step_once(d->st_begin) != SQLITE_DONE) return -1;
  d->in_write = 1;
  d->saved_max_id = d->max_id;
  memcpy(d->saved_ts, d->last_ts, sizeof d->saved_ts);
  d->saved_ts_len = d->last_ts_len;
  return 0;
}

int db_batch_commit(db_t *d) {
  bt_close_cursors(d);   /* COMMIT/ROLLBACK with no B-tree cursor open → TXN_NONE */
  d->in_batch = 0;
  int ok = 1;
  if (d->in_write) {
    ok = !d->batch_failed && !sqlite3_get_autocommit(d->h) &&
         step_once(d->st_commit) == SQLITE_DONE;
    if (!ok) restore_ins_state(d);
  } else if (!sqlite3_get_autocommit(d->h)) {
    /* read-only batch: end the snapshot (nothing to fail for callers) */
    if (step_once(d->st_commit) == SQLITE_DONE) goto done;
  }
  if (!sqlite3_get_autocommit(d->h)) step_once(d->st_rollback);
done:
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

/* ------------------------------------------------------- page-cache wrapper */
/* Installed with the public sqlite3_config(SQLITE_CONFIG_PCACHE2) before the
 * first sqlite3_initialize(); wraps SQLite's built-in page cache (pcache1,
 * fetched with SQLITE_CONFIG_GETPCACHE2) and forwards every method, except
 * that xTruncate(iLimit) is skipped when no cached page can have a key >=
 * iLimit (the call is then a no-op by its contract).
 * Why: balance_nonroot swaps sibling page numbers through the lock-page key
 * (PENDING_BYTE/pageSize)+1 = 262145 with sqlite3PagerRekey; pcache1Rekey
 * raises pcache1's iMaxKey to it and only a truncate lowers it again, so the
 * next COMMIT's pcache1Truncate(dbSize+1) walks the whole hash table (every
 * cached page header) to drop nothing.
 * Each cache keeps a bitmap of keys that MAY be in the inner cache: a
 * superset, set on every successful xFetch and on xRekey's new key, cleared on
 * xRekey's old key and above iLimit after a forwarded xTruncate; eviction
 * inside the inner cache only shrinks the real set. If the bitmap cannot grow,
 * that cache forwards everything. Single thread (SQLITE_THREADSAFE=0). */
typedef struct pcw {
  sqlite3_pcache *inner;
  uint64_t *bits;     /* bit k set: key k may be in the inner cache */
  uint64_t nbits;     /* bits allocated (multiple of 64); 0 in passthrough */
  uint64_t top;       /* every set bit is < top; top <= nbits */
  int passthrough;    /* bitmap growth failed: forward every xTruncate */
} pcw_t;

static sqlite3_pcache_methods2 pcw_inner;   /* the built-in cache's methods */
static struct { unsigned long skipped, forwarded; } pcw_stats;

void db_pcache_stats(unsigned long *skipped, unsigned long *forwarded) {
  if (skipped) *skipped = pcw_stats.skipped;
  if (forwarded) *forwarded = pcw_stats.forwarded;
}

__attribute__((noinline, cold)) static int pcw_grow(pcw_t *w, unsigned key) {
  uint64_t n = w->nbits ? w->nbits : 4096;
  while (n <= key) n *= 2;
  uint64_t *b = realloc(w->bits, (size_t)(n / 8));
  if (!b) {
    free(w->bits);
    w->bits = NULL; w->nbits = 0; w->top = 0; w->passthrough = 1;
    return 0;
  }
  memset(b + w->nbits / 64, 0, (size_t)((n - w->nbits) / 8));
  w->bits = b; w->nbits = n;
  return 1;
}

static inline void pcw_set(pcw_t *w, unsigned key) {
  if (__builtin_expect(key >= w->nbits, 0) && (w->passthrough || !pcw_grow(w, key))) return;
  w->bits[key >> 6] |= 1ull << (key & 63);
  if (key >= w->top) w->top = (uint64_t)key + 1;
}

/* 1 if any bit >= from is set. Bits >= top are all clear. */
static int pcw_any_from(const pcw_t *w, unsigned from) {
  if (from >= w->top) return 0;
  uint64_t i = from >> 6, end = (w->top + 63) >> 6;
  if (w->bits[i] & (~0ull << (from & 63))) return 1;
  for (i++; i < end; i++) if (w->bits[i]) return 1;
  return 0;
}

static sqlite3_pcache *pcw_create(int szPage, int szExtra, int bPurgeable) {
  sqlite3_pcache *in = pcw_inner.xCreate(szPage, szExtra, bPurgeable);
  if (!in) return NULL;
  pcw_t *w = calloc(1, sizeof *w);
  if (!w) { pcw_inner.xDestroy(in); return NULL; }
  w->inner = in;
  return (sqlite3_pcache *)w;
}

static sqlite3_pcache_page *pcw_fetch(sqlite3_pcache *p, unsigned key, int create) {
  pcw_t *w = (pcw_t *)p;
  sqlite3_pcache_page *r = pcw_inner.xFetch(w->inner, key, create);
  if (r) pcw_set(w, key);
  return r;
}

static void pcw_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned oldk, unsigned newk) {
  pcw_t *w = (pcw_t *)p;
  pcw_inner.xRekey(w->inner, pg, oldk, newk);
  if (oldk < w->nbits) w->bits[oldk >> 6] &= ~(1ull << (oldk & 63));
  pcw_set(w, newk);
}

static void pcw_truncate(sqlite3_pcache *p, unsigned iLimit) {
  pcw_t *w = (pcw_t *)p;
  if (!w->passthrough && !pcw_any_from(w, iLimit)) {
    pcw_stats.skipped++;
    if (w->top > iLimit) w->top = iLimit;
    return;
  }
  pcw_stats.forwarded++;
  pcw_inner.xTruncate(w->inner, iLimit);
  if (!w->passthrough && w->top > iLimit) {
    uint64_t i = iLimit >> 6, end = (w->top + 63) >> 6;
    w->bits[i] &= (1ull << (iLimit & 63)) - 1;
    memset(w->bits + i + 1, 0, (size_t)(end - i - 1) * 8);
    w->top = iLimit;
  }
}

static void pcw_destroy(sqlite3_pcache *p) {
  pcw_t *w = (pcw_t *)p;
  pcw_inner.xDestroy(w->inner);
  free(w->bits);
  free(w);
}

static void pcw_cachesize(sqlite3_pcache *p, int n) { pcw_inner.xCachesize(((pcw_t *)p)->inner, n); }
static int pcw_pagecount(sqlite3_pcache *p) { return pcw_inner.xPagecount(((pcw_t *)p)->inner); }
static void pcw_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) {
  pcw_inner.xUnpin(((pcw_t *)p)->inner, pg, discard);
}
static void pcw_shrink(sqlite3_pcache *p) {
  if (pcw_inner.xShrink) pcw_inner.xShrink(((pcw_t *)p)->inner);
}
static int pcw_init(void *arg) {
  (void)arg;
  return pcw_inner.xInit ? pcw_inner.xInit(pcw_inner.pArg) : SQLITE_OK;
}
static void pcw_shutdown(void *arg) {
  (void)arg;
  if (pcw_inner.xShutdown) pcw_inner.xShutdown(pcw_inner.pArg);
}

/* Call right before every sqlite3_initialize() in this file. On failure the
 * default cache stays in place (one line on stderr). */
static void pcache_install_once(void) {
  static int done;
  if (done) return;
  done = 1;
  if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pcw_inner) != SQLITE_OK || !pcw_inner.xCreate) {
    fprintf(stderr, "db: SQLITE_CONFIG_GETPCACHE2 failed; using the default page cache\n");
    return;
  }
  sqlite3_pcache_methods2 m = {   /* sqlite3_config copies it */
    pcw_inner.iVersion, NULL, pcw_init, pcw_shutdown, pcw_create, pcw_cachesize,
    pcw_pagecount, pcw_fetch, pcw_unpin, pcw_rekey, pcw_truncate, pcw_destroy, pcw_shrink,
  };
  if (sqlite3_config(SQLITE_CONFIG_PCACHE2, &m) != SQLITE_OK)
    fprintf(stderr, "db: SQLITE_CONFIG_PCACHE2 failed; using the default page cache\n");
}

void db_close(db_t *d) {
  if (!d) return;
  if (d->h) {
    bt_close_cursors(d);
    if (!sqlite3_get_autocommit(d->h)) exec(d->h, "ROLLBACK");
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
  pcache_install_once();
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
    "PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL", "PRAGMA mmap_size=0",
    "PRAGMA cache_size=-131072", "PRAGMA temp_store=memory", "PRAGMA wal_autocheckpoint=0",
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
      prep(d, &d->st_begin, "BEGIN", err, errlen) ||
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

/* ------------------------------------------------------------ PGO training */
/* Used only by `bin/server --pgo-train` (build.sh's PGO step). Never called
 * while serving. The DDL below is the challenge's schema.sql, embedded so the
 * training never reads a file. */

static const char pgo_schema[] =
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

static uint32_t pgo_rand(uint64_t *s) {
  *s = *s * 6364136223846793005ull + 1442695040888963407ull;
  return (uint32_t)(*s >> 33);
}
static double pgo_unit(uint64_t *s) { return pgo_rand(s) / 2147483648.0; }   /* [0, 1) */

/* YYYY-MM-DDTHH:MM:SS.mmmZ */
static void pgo_ts(char *o, size_t cap, int64_t ms) {
  time_t t = (time_t)(ms / 1000);
  struct tm tm;
  gmtime_r(&t, &tm);
  snprintf(o, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ms % 1000));
}

static const char *const pgo_adj[] = {"quiet", "brave", "clever", "sunny", "rusty", "swift", "lazy", "mellow",
  "sharp", "wild", "cosmic", "tiny", "golden", "silver", "happy", "grumpy", "curious", "lucky", "salty", "zesty"};
static const char *const pgo_noun[] = {"otter", "falcon", "badger", "comet", "river", "maple", "pixel", "walrus",
  "ember", "glacier", "lantern", "meadow", "nebula", "orchid", "pebble", "quartz", "raven", "sparrow", "thistle", "willow"};

/* A post body shaped like seed/generate.mjs: an optional opener, 1-3
 * sentences of 4-14 words, at most 500 characters. */
static size_t pgo_body(uint64_t *s, char *o) {
  static const char *const words[] = {"the", "a", "and", "but", "so", "because", "just", "really", "maybe",
    "server", "database", "request", "latency", "cache", "index", "query", "benchmark", "droplet", "cpu",
    "memory", "disk", "network", "throughput", "users", "coffee", "deploy", "release", "bug", "feature",
    "rewrite", "commit", "thinking", "shipping", "debugging", "fast", "slow", "cheap", "simple", "boring",
    "today", "tonight", "finally", "never", "always", "nginx", "postgres", "linux", "kernel", "socket", "pool"};
  static const char *const openers[] = {"Hot take:", "TIL", "Unpopular opinion:", "Note to self:", "PSA:",
    "Okay so", "Update:", "Honestly,", "Reminder:", "Question:"};
  static const char *const ends[] = {".", ".", ".", "!", "?", "..."};
  char *p = o;
  if (pgo_rand(s) % 4 == 0) {
    const char *w = openers[pgo_rand(s) % 10];
    size_t l = strlen(w); memcpy(p, w, l); p += l; *p++ = ' ';
  }
  int ns = 1 + (int)(pgo_rand(s) % 3);
  for (int i = 0; i < ns; i++) {
    if (i) *p++ = ' ';
    int nw = 4 + (int)(pgo_rand(s) % 11);
    char *first = p;
    for (int j = 0; j < nw; j++) {
      if (j) *p++ = ' ';
      const char *w = words[pgo_rand(s) % (sizeof words / sizeof words[0])];
      size_t l = strlen(w); memcpy(p, w, l); p += l;
    }
    if (*first >= 'a' && *first <= 'z') *first = (char)(*first - 32);
    const char *e = ends[pgo_rand(s) % 6];
    size_t l = strlen(e); memcpy(p, e, l); p += l;
  }
  size_t n = (size_t)(p - o);
  if (n > 500) { memcpy(o + 497, "...", 3); n = 500; }
  return n;
}

static int pgo_gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

int db_pgo_make(const char *path, int nusers, int nposts, int nlikes, char *err, size_t errlen) {
  sqlite3 *h = NULL;
  sqlite3_stmt *su = NULL, *sp = NULL, *sl = NULL;
  uint64_t rng = 0x5eed5eed12345678ull;
  char ts[40], name[64], body[700];
  int rc = -1;
  if (nusers < 1 || nposts < 1 || nlikes < 0) { snprintf(err, errlen, "bad sizes"); return -1; }
  pcache_install_once();
  if (sqlite3_initialize() != SQLITE_OK) { snprintf(err, errlen, "sqlite3_initialize failed"); return -1; }
  if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) goto out;
  /* Like seed/make-seed.sh: no journal while loading, WAL at the end. */
  if (exec(h, "PRAGMA journal_mode=OFF") || exec(h, "PRAGMA synchronous=OFF") ||
      exec(h, pgo_schema) || exec(h, "BEGIN")) goto out;
  if (sqlite3_prepare_v2(h, "INSERT INTO users (id, username, created_at) VALUES (?, ?, ?)", -1, &su, 0) ||
      sqlite3_prepare_v2(h, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?, ?, ?, ?)", -1, &sp, 0) ||
      sqlite3_prepare_v2(h, "INSERT INTO likes (user_id, post_id, created_at) VALUES (?, ?, ?)", -1, &sl, 0))
    goto out;

  const int64_t DAY = 86400000;
  const int64_t end_ms = 1767225600000LL;              /* 2026-01-01T00:00:00Z */
  for (int i = 1; i <= nusers; i++) {
    int nl = snprintf(name, sizeof name, "%s_%s_%d", pgo_adj[pgo_rand(&rng) % 20], pgo_noun[pgo_rand(&rng) % 20], i);
    pgo_ts(ts, sizeof ts, end_ms - 730 * DAY + (int64_t)(pgo_unit(&rng) * 365 * DAY));
    sqlite3_bind_int64(su, 1, i);
    sqlite3_bind_text(su, 2, name, nl, SQLITE_STATIC);
    sqlite3_bind_text(su, 3, ts, -1, SQLITE_STATIC);
    if (sqlite3_step(su) != SQLITE_DONE) goto out;
    sqlite3_reset(su);
  }

  /* Inv: dense ids 1..nposts, created_at strictly increasing (jitter < step),
   * authors skewed toward low user ids and always existing. */
  const int64_t start_ms = end_ms - 365 * DAY, step = 365 * DAY / nposts;
  int64_t *post_ms = malloc(sizeof(int64_t) * (size_t)nposts);
  if (!post_ms) { snprintf(err, errlen, "out of memory"); goto out_noerr; }
  for (int i = 0; i < nposts; i++) {
    int64_t ms = start_ms + i * step + (int64_t)(pgo_unit(&rng) * (double)step * 0.9);
    post_ms[i] = ms;
    pgo_ts(ts, sizeof ts, ms);
    size_t bl = pgo_body(&rng, body);
    sqlite3_bind_int64(sp, 1, i + 1);
    sqlite3_bind_int64(sp, 2, 1 + (int64_t)(nusers * pgo_unit(&rng) * pgo_unit(&rng)));
    sqlite3_bind_text(sp, 3, body, (int)bl, SQLITE_STATIC);
    sqlite3_bind_text(sp, 4, ts, -1, SQLITE_STATIC);
    if (sqlite3_step(sp) != SQLITE_DONE) { free(post_ms); goto out; }
    sqlite3_reset(sp);
  }

  /* Likes: about nlikes in all, exponential per post with 1% popular posts at
   * ~20x the mean; distinct users per post (a stride coprime to nusers). */
  int stride = 7919 % nusers;
  if (stride == 0) stride = 1;
  while (pgo_gcd(stride, nusers) != 1) stride++;
  double mean = (double)nlikes / nposts;
  int64_t left = nlikes;
  for (int i = 0; i < nposts && left > 0; i++) {
    double m = pgo_rand(&rng) % 100 == 0 ? mean * 20 : mean * 0.81;
    int64_t k = (int64_t)(-m * log(1.0 - pgo_unit(&rng)) + 0.5);
    if (k > nusers) k = nusers;
    if (k > left) k = left;
    int u0 = (int)(pgo_rand(&rng) % (uint32_t)nusers);
    for (int64_t j = 0; j < k; j++) {
      pgo_ts(ts, sizeof ts, post_ms[i] + (int64_t)(pgo_unit(&rng) * 7 * DAY));
      sqlite3_bind_int64(sl, 1, 1 + (u0 + j * stride) % nusers);
      sqlite3_bind_int64(sl, 2, i + 1);
      sqlite3_bind_text(sl, 3, ts, -1, SQLITE_STATIC);
      if (sqlite3_step(sl) != SQLITE_DONE) { free(post_ms); goto out; }
      sqlite3_reset(sl);
    }
    left -= k;
  }
  free(post_ms);
  if (exec(h, "COMMIT") || exec(h, "ANALYZE") || exec(h, "PRAGMA journal_mode=WAL")) goto out;
  rc = 0;
out:
  if (rc) snprintf(err, errlen, "%s", h ? sqlite3_errmsg(h) : "open failed");
out_noerr:
  sqlite3_finalize(su); sqlite3_finalize(sp); sqlite3_finalize(sl);
  if (h) sqlite3_close(h);
  return rc;
}
