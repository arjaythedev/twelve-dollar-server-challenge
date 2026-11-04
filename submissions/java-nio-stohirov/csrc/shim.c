/* The only C in this submission: what Java cannot reach through SQLite's public API.
 *
 * GET /feed and GET /posts/:id read SQLite's own B-tree cursors, which are private to the
 * amalgamation, so this file #includes it unmodified. Everything else (HTTP, io_uring, JWT, writes,
 * checkpoints) is Java and calls the public sqlite3_* functions through FFM.
 *
 * The read plan and the transaction-per-pass scheme follow rust-iouring-kensac in this repository.
 * Every response is built from rows read while the request is served. Nothing is kept between
 * requests except prepared statements, KeyInfo objects and the four root page numbers. */
#include "sqlite3.c"

typedef long long i64;

static sqlite3 *db, *ckdb;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;
static Pgno root_posts, root_users, root_feed, root_likes;
static KeyInfo *ki_feed, *ki_likes;
static int in_batch, cur_open, wal_frames;

/* Cursors, opened after a pass's read transaction starts and closed before it commits. */
enum { POSTS, USERS, FEED, LIKES, NCUR };
static BtCursor *cur[NCUR];

#define FEED_LEN 20
#define RANGE_SPAN 512
#define TEXT_CAP (256 * 1024)
static u8 *text;  /* copies of the feed's strings, so each cursor can move on */
static u8 *spill; /* records that spill onto overflow pages (not expected with this schema) */
static u32 spill_cap;

enum { ERR = -1, USE_SQL = -2 }; /* USE_SQL: data this plan doesn't expect, so the SQL path answers */

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  return SQLITE_OK;
}

static Pgno rootpage(const char *name) {
  sqlite3_stmt *s;
  Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, 0)) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return r;
}

/* The record decoders assume this exact column order; refuse to start on anything else. */
static int layout(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[256];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

static KeyInfo *keyinfo(int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) return 0;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s = 0;
  sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0);
  return s;
}

static int fail(char *err, int n, const char *what) {
  snprintf(err, n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

/* unix-excl: this process keeps an exclusive lock on the file, and the WAL index lives in heap
 * memory shared by its two connections, so there is no -shm file and no file lock per transaction.
 * No mmap: a page in SQLite's cache keeps its parsed header and needs no WAL lookup. */
int td_open(const char *path, char *err, int n) {
  if (sqlite3_initialize()) return fail(err, n, "init");
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE, "unix-excl"))
    return fail(err, n, "open");
  if (sqlite3_exec(db,
                   "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;"
                   "PRAGMA cache_size = -65536; PRAGMA mmap_size = 0; PRAGMA journal_size_limit = 67108864;"
                   "PRAGMA wal_autocheckpoint = 0; SELECT count(*) FROM sqlite_schema;",
                   0, 0, 0))
    return fail(err, n, "pragmas");
  sqlite3_wal_hook(db, wal_hook, 0);
  /* The checkpoint thread's connection copies frames straight from the WAL and reads no tables. */
  if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") ||
      sqlite3_exec(ckdb, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16", 0, 0, 0))
    return fail(err, n, "checkpoint connection");
  static const char *checks[][2] = {
    {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
    {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
    {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
    {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!layout(checks[i][0], checks[i][1])) {
      snprintf(err, n, "unexpected schema (%s)", checks[i][0]);
      return -1;
    }
  }
  if (ENC(db) != SQLITE_UTF8) {
    snprintf(err, n, "database text encoding is not UTF-8");
    return -1;
  }
  root_posts = rootpage("posts");
  root_users = rootpage("users");
  root_feed = rootpage("posts_created_at_id_idx");
  root_likes = rootpage("likes_post_id_idx");
  ki_feed = keyinfo(2, 1, 1);
  ki_likes = keyinfo(1, 1, 0);
  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  text = sqlite3_malloc(TEXT_CAP);
  if (!root_posts || !root_users || !root_feed || !root_likes || !ki_feed || !ki_likes || !st_begin ||
      !st_commit || !st_rollback || !text)
    return fail(err, n, "setup");
  bt = db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++) {
    cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!cur[i]) return fail(err, n, "cursor alloc");
  }
  return 0;
}

void *td_db(void) { return db; }
void *td_ckdb(void) { return ckdb; }
int td_wal_frames(void) { return wal_frames; }
void td_wal_frames_reset(void) { wal_frames = 0; }

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static void close_cursors(void) {
  if (!cur_open) return;
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}

/* Joins the pass's transaction, starting it if needed. -1 if it can't start, or if SQLite rolled it
 * back by itself earlier in the pass, in which case nothing more may run until td_commit reports it. */
int td_begin(void) {
  if (in_batch) return sqlite3_get_autocommit(db) ? -1 : 0;
  if (step_reset(st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) {
    step_reset(st_rollback);
    return -1;
  }
  Pgno roots[NCUR] = {root_posts, root_users, root_feed, root_likes};
  KeyInfo *kis[NCUR] = {0, 0, ki_feed, ki_likes};
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, roots[i], 0, kis[i], cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      step_reset(st_rollback);
      return -1;
    }
  }
  cur_open = 1;
  in_batch = 1;
  return 0;
}

/* 0 once the pass's transaction is committed, -1 if it was lost or rolled back. */
int td_commit(void) {
  if (!in_batch) return 0;
  in_batch = 0;
  close_cursors();
  if (sqlite3_get_autocommit(db)) return -1;
  if (step_reset(st_commit) != SQLITE_DONE) {
    step_reset(st_rollback);
    return -1;
  }
  return 0;
}

/* The cursor's current record, valid until it moves. */
static const u8 *payload(BtCursor *c, u32 *len) {
  getCellInfo(c);
  u32 total = c->info.nPayload;
  *len = total;
  if (c->info.nLocal == total) return c->info.pPayload;
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    spill = sqlite3_malloc(spill_cap);
    if (!spill) {
      spill_cap = 0;
      return 0;
    }
  }
  return sqlite3BtreePayload(c, 0, total, spill) == SQLITE_OK ? spill : 0;
}

static inline u32 serial_len(u64 t) {
  static const u8 fixed[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? fixed[t] : (u32)((t - 12) / 2);
}

/* The first n columns of a record. 0, or -1 if it isn't well formed. */
static inline int columns(const u8 *rec, u32 len, int n, u64 *type, const u8 **val) {
  const u8 *p = rec, *end = rec + len;
  u64 hsize;
  if (len == 0) return -1;
  p += sqlite3GetVarint(p, &hsize);
  if (hsize > len) return -1;
  const u8 *hend = rec + hsize, *body = hend;
  for (int i = 0; i < n; i++) {
    if (p >= hend) return -1;
    if (*p < 0x80) type[i] = *p++;
    else p += sqlite3GetVarint(p, &type[i]);
    u32 k = serial_len(type[i]);
    if ((u64)(end - body) < k) return -1;
    val[i] = body;
    body += k;
  }
  return 0;
}

static inline int int_of(u64 t, const u8 *b, i64 *out) {
  if (t == 8 || t == 9) {
    *out = (i64)t - 8;
    return 0;
  }
  if (t < 1 || t > 6) return -1;
  u32 n = serial_len(t);
  i64 v = (signed char)b[0];
  for (u32 i = 1; i < n; i++) v = v << 8 | b[i];
  *out = v;
  return 0;
}

static inline int is_text(u64 t) { return t >= 13 && (t & 1); }

/* Table cursor to rowid: 1 found, 0 absent, -1 error. */
static inline int seek(BtCursor *c, i64 id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID;
}

/* Index cursor to the first entry whose first integer column is >= k: 1 on an entry, 0 none, -1 error. */
static int seek_ge(BtCursor *c, i64 k) {
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = c->pKeyInfo;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1; /* entries equal on the key sort after it: land on the first of them */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

/* Likes per post for lo <= post_id <= hi, one walk over likes_post_id_idx, decoding cells a leaf at a
 * time: n[post_id - lo] counts them. 0 ok, -1 error, USE_SQL for a cell not in the expected form. */
static int count_likes(BtCursor *c, i64 lo, i64 hi, u32 *n) {
  int r = seek_ge(c, lo);
  if (r <= 0) return r;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1; /* an interior entry: just this one */
    for (; i < last; i++) {
      u8 *cell = findCell(pg, i) + pg->childPtrSize;
      u32 len = cell[0];
      const u8 *p = cell + 1;
      if (len >= 0x80 || len < 3 || p + len > pg->aDataEnd) return USE_SQL;
      u32 h = p[0], t = p[1];
      i64 v;
      if (h >= len || h < 3 || t >= 0x80 || int_of(t, p + h, &v) || h + serial_len(t) > len) return USE_SQL;
      if (v > hi) return 0;
      if (v >= lo) n[v - lo]++;
    }
    c->ix = (u16)(last - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

#define LIT(o, s) (memcpy((o), (s), sizeof(s) - 1), (o) + sizeof(s) - 1)

static inline u8 *put_int(u8 *o, i64 v) {
  char tmp[24];
  int k = 0;
  if (v < 0) {
    *o++ = '-';
    v = -v;
  }
  do tmp[k++] = (char)('0' + v % 10); while ((v /= 10) > 0);
  while (k) *o++ = (u8)tmp[--k];
  return o;
}

/* JSON string contents. Eight bytes at a time until one needs escaping; the caller guarantees 6n + 8
 * bytes of room. Little-endian only, as is everything this server runs on. */
static inline u8 *escape(u8 *o, const u8 *s, u32 n) {
  static const char hex[] = "0123456789abcdef";
  const u64 ones = 0x0101010101010101ULL, highs = 0x8080808080808080ULL;
  const u8 *end = s + n;
  while (s < end) {
    if (end - s >= 8) {
      u64 x;
      memcpy(&x, s, 8);
      u64 q = x ^ (ones * '"'), b = x ^ (ones * '\\');
      u64 hit = ((q - ones) & ~q) | ((b - ones) & ~b) | ((x - ones * 0x20) & ~x);
      hit &= highs;
      memcpy(o, &x, 8);
      if (!hit) {
        s += 8;
        o += 8;
        continue;
      }
      int k = __builtin_ctzll(hit) >> 3;
      s += k;
      o += k;
    }
    u8 c = *s++;
    if (c >= 0x20 && c != '"' && c != '\\') {
      *o++ = c;
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
        o = LIT(o, "u00");
        *o++ = (u8)hex[c >> 4];
        *o++ = (u8)hex[c & 15];
    }
  }
  return o;
}

/* Worst case for one row's fixed text and integers. */
#define ROW_BOUND 128

static inline u8 *put_post(u8 *o, i64 id, const u8 *body, u32 blen, const u8 *created, u32 clen,
                           const u8 *author, u32 alen, i64 likes) {
  o = LIT(o, "{\"id\":");
  o = put_int(o, id);
  o = LIT(o, ",\"body\":\"");
  o = escape(o, body, blen);
  o = LIT(o, "\",\"created_at\":\"");
  o = escape(o, created, clen);
  o = LIT(o, "\",\"author\":\"");
  o = escape(o, author, alen);
  o = LIT(o, "\",\"like_count\":");
  o = put_int(o, likes);
  *o++ = '}';
  return o;
}

typedef struct {
  i64 id, uid, likes;
  u32 body, blen, created, clen, author, alen;
} Row;

static inline int keep(u32 *used, const u8 *s, u32 n, u32 *at) {
  if (*used + n > TEXT_CAP) return -1;
  memcpy(text + *used, s, n);
  *at = *used;
  *used += n;
  return 0;
}

/* GET /feed: the 20 newest by (created_at DESC, id DESC), read off that index. Writes
 * {"posts":[...]} to out and returns its length, or ERR or USE_SQL. */
int td_feed(u8 *out, int cap) {
  static Row rows[FEED_LEN];
  static u32 counts[RANGE_SPAN + 1];
  int order[FEED_LEN], nrows = 0;
  u32 used = 0, len;
  u64 t[3];
  const u8 *v[3];

  /* 1. The index: (created_at, id), newest first. */
  int empty = 1;
  if (sqlite3BtreeFirst(cur[FEED], &empty) != SQLITE_OK) return ERR;
  if (!empty) {
    for (;;) {
      const u8 *rec = payload(cur[FEED], &len);
      if (!rec) return ERR;
      Row *r = &rows[nrows];
      if (columns(rec, len, 2, t, v) || !is_text(t[0]) || int_of(t[1], v[1], &r->id)) return USE_SQL;
      r->clen = serial_len(t[0]);
      if (keep(&used, v[0], r->clen, &r->created)) return USE_SQL;
      if (++nrows == FEED_LEN) break;
      int rc = sqlite3BtreeNext(cur[FEED], 0);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_OK) return ERR;
    }
  }

  /* 2. The posts in ascending id order: consecutive ids are one step along the same leaf.
   * Newest-first is normally descending id, so ascending is usually just the reverse. */
  for (int i = 0; i < nrows; i++) {
    int k = i;
    while (k > 0 && rows[order[k - 1]].id < rows[i].id) {
      order[k] = order[k - 1];
      k--;
    }
    order[k] = i;
  }
  for (int k = nrows - 1; k >= 0; k--) {
    Row *r = &rows[order[k]];
    int found = seek(cur[POSTS], r->id);
    if (found < 0) return ERR;
    if (!found) return USE_SQL;
    const u8 *rec = payload(cur[POSTS], &len);
    if (!rec) return ERR;
    if (columns(rec, len, 3, t, v) || !is_text(t[2]) || int_of(t[1], v[1], &r->uid)) return USE_SQL;
    r->blen = serial_len(t[2]);
    if (keep(&used, v[2], r->blen, &r->body)) return USE_SQL;
  }

  /* 3. The authors. A post without a user: the JOIN would drop it, so SQL answers. */
  for (int i = 0; i < nrows; i++) {
    Row *r = &rows[i];
    int found = seek(cur[USERS], r->uid);
    if (found < 0) return ERR;
    if (!found) return USE_SQL;
    const u8 *rec = payload(cur[USERS], &len);
    if (!rec) return ERR;
    if (columns(rec, len, 2, t, v) || !is_text(t[1])) return USE_SQL;
    r->alen = serial_len(t[1]);
    if (keep(&used, v[1], r->alen, &r->author)) return USE_SQL;
  }

  /* 4. Like counts: one walk over the index when the ids are close together. */
  if (nrows > 0) {
    i64 lo = rows[order[nrows - 1]].id, hi = rows[order[0]].id;
    if (hi - lo <= RANGE_SPAN) {
      memset(counts, 0, (size_t)(hi - lo + 1) * sizeof counts[0]);
      int rc = count_likes(cur[LIKES], lo, hi, counts);
      if (rc) return rc;
      for (int i = 0; i < nrows; i++) rows[i].likes = counts[rows[i].id - lo];
    } else {
      for (int i = 0; i < nrows; i++) {
        u32 one = 0;
        int rc = count_likes(cur[LIKES], rows[i].id, rows[i].id, &one);
        if (rc) return rc;
        rows[i].likes = one;
      }
    }
  }

  if ((u64)cap < 6ULL * used + (u64)(nrows + 1) * ROW_BOUND) return USE_SQL;
  u8 *o = LIT(out, "{\"posts\":[");
  for (int i = 0; i < nrows; i++) {
    Row *r = &rows[i];
    if (i) *o++ = ',';
    o = put_post(o, r->id, text + r->body, r->blen, text + r->created, r->clen, text + r->author, r->alen, r->likes);
  }
  o = LIT(o, "]}");
  return (int)(o - out);
}

/* GET /posts/:id. Writes {"post":{...}} to out: its length, 0 if there is no such row, or ERR or USE_SQL. */
int td_post(i64 id, u8 *out, int cap) {
  u32 len, alen;
  u64 t[4], a[2];
  const u8 *v[4], *av[2];
  i64 uid;
  u32 likes = 0;
  int found = seek(cur[POSTS], id);
  if (found <= 0) return found;
  const u8 *rec = payload(cur[POSTS], &len);
  if (!rec) return ERR;
  if (columns(rec, len, 4, t, v) || !is_text(t[2]) || !is_text(t[3]) || int_of(t[1], v[1], &uid)) return USE_SQL;
  /* The post's cell stays valid: only the other cursors move from here on. */
  found = seek(cur[USERS], uid);
  if (found <= 0) return found; /* no such user: the JOIN finds no row */
  const u8 *urec = payload(cur[USERS], &alen);
  if (!urec) return ERR;
  if (columns(urec, alen, 2, a, av) || !is_text(a[1])) return USE_SQL;
  int rc = count_likes(cur[LIKES], id, id, &likes);
  if (rc) return rc;
  u32 blen = serial_len(t[2]), clen = serial_len(t[3]), nlen = serial_len(a[1]);
  if ((u64)cap < 6ULL * ((u64)blen + clen + nlen) + ROW_BOUND) return USE_SQL;
  u8 *o = LIT(out, "{\"post\":");
  o = put_post(o, id, v[2], blen, v[3], clen, av[1], nlen, likes);
  *o++ = '}';
  return (int)(o - out);
}

/* Whether post id exists (for likes): 1, 0, or -1 on error. */
int td_post_exists(i64 id) { return seek(cur[POSTS], id); }
