#include "shim.h"
#include "sqlite3.c"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct dbx {
  sqlite3 *db;
  sqlite3 *ckdb;
  Btree *bt;
  BtCursor *cur[4];
  Pgno root[4];
  KeyInfo *ki_feed;
  KeyInfo *ki_likes;
  sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health;
  int in_tx;
  int broken;
  _Atomic int wal_frames;
  unsigned char *rec;
  unsigned rec_cap;
};

static int fail(sqlite3 *db, char *err, int n, const char *what) {
  snprintf(err, (size_t)n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

static int wal_cb(void *p, sqlite3 *db, const char *name, int frames) {
  (void)db;
  (void)name;
  atomic_store_explicit(&((struct dbx *)p)->wal_frames, frames, memory_order_relaxed);
  return SQLITE_OK;
}

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static int read_varint(const unsigned char *p, unsigned len, unsigned *pos, unsigned long long *v) {
  unsigned i = *pos;
  if (i >= len) return -1;
  if (p[i] < 0x80) {
    *v = p[i];
    *pos = i + 1;
    return 0;
  }
  unsigned long long r = 0;
  for (int k = 0; k < 9; k++) {
    if (i >= len) return -1;
    unsigned char b = p[i++];
    if (k == 8) {
      r = (r << 8) | b;
      break;
    }
    r = (r << 7) | (unsigned long long)(b & 0x7f);
    if (!(b & 0x80)) break;
  }
  *pos = i;
  *v = r;
  return 0;
}

static int first_int(const unsigned char *rec, unsigned len, long long *out) {
  unsigned pos = 0;
  unsigned long long hdr = 0;
  if (read_varint(rec, len, &pos, &hdr)) return -1;
  if (hdr < 2 || hdr > len || pos >= hdr) return -1;
  unsigned long long type = 0;
  if (read_varint(rec, (unsigned)hdr, &pos, &type)) return -1;
  unsigned size;
  switch (type) {
    case 8:
      *out = 0;
      return 0;
    case 9:
      *out = 1;
      return 0;
    case 1: size = 1; break;
    case 2: size = 2; break;
    case 3: size = 3; break;
    case 4: size = 4; break;
    case 5: size = 6; break;
    case 6: size = 8; break;
    default: return -1;
  }
  if (hdr + size > len) return -1;
  long long v = (long long)(signed char)rec[hdr];
  for (unsigned i = 1; i < size; i++) v = (v << 8) | rec[hdr + i];
  *out = v;
  return 0;
}

static Pgno rootpage(sqlite3 *db, const char *name) {
  sqlite3_stmt *s = 0;
  Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, 0))
    return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return r;
}

static int columns_match(sqlite3 *db, const char *sql, const char *want) {
  sqlite3_stmt *s = 0;
  char got[512];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
    if (n >= sizeof got) break;
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

static KeyInfo *feed_keyinfo(sqlite3 *db) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, 2, 1);
  if (!k) return 0;
  for (int i = 0; i < 3; i++) k->aColl[i] = db->pDfltColl;
  k->aSortFlags[0] = KEYINFO_ORDER_DESC;
  k->aSortFlags[1] = KEYINFO_ORDER_DESC;
  return k;
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql) {
  sqlite3_stmt *s = 0;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0)) return 0;
  return s;
}

static void close_cursors(struct dbx *h) {
  if (!h->in_tx) return;
  for (int i = 0; i < 4; i++) sqlite3BtreeCloseCursor(h->cur[i]);
  h->in_tx = 0;
}

void dbx_close(dbx *h) {
  if (!h) return;
  if (!h->db) {
    sqlite3_free(h);
    return;
  }
  if (h->in_tx) {
    step_reset(h->st_rollback);
    close_cursors(h);
  }
  sqlite3_finalize(h->st_begin);
  sqlite3_finalize(h->st_commit);
  sqlite3_finalize(h->st_rollback);
  sqlite3_finalize(h->st_post);
  sqlite3_finalize(h->st_like);
  sqlite3_finalize(h->st_health);
  if (h->ki_feed) sqlite3KeyInfoUnref(h->ki_feed);
  if (h->ki_likes) sqlite3KeyInfoUnref(h->ki_likes);
  for (int i = 0; i < 4; i++) sqlite3_free(h->cur[i]);
  if (h->rec) sqlite3_free(h->rec);
  if (h->ckdb) sqlite3_close(h->ckdb);
  sqlite3_close(h->db);
  sqlite3_free(h);
}

dbx *dbx_open(const char *path, char *err, int n) {
  if (sqlite3_initialize()) {
    snprintf(err, (size_t)n, "sqlite init failed");
    return 0;
  }
  struct dbx *h = (struct dbx *)sqlite3_malloc((int)sizeof *h);
  if (!h) {
    snprintf(err, (size_t)n, "out of memory");
    return 0;
  }
  memset(h, 0, sizeof *h);

  if (sqlite3_open_v2(path, &h->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl")) {
    fail(h->db, err, n, "open");
    dbx_close(h);
    return 0;
  }

  static const char *pragmas[] = {
      "PRAGMA journal_mode = WAL", "PRAGMA synchronous = NORMAL",
      "PRAGMA temp_store = MEMORY", "PRAGMA cache_size = -65536",
      "PRAGMA mmap_size = 0", "PRAGMA wal_autocheckpoint = 0",
      "SELECT count(*) FROM sqlite_schema",
  };
  for (size_t i = 0; i < sizeof pragmas / sizeof pragmas[0]; i++) {
    if (sqlite3_exec(h->db, pragmas[i], 0, 0, 0)) {
      fail(h->db, err, n, "pragmas");
      dbx_close(h);
      return 0;
    }
  }
  sqlite3_wal_hook(h->db, wal_cb, h);

  static const char *checks[][2] = {
      {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
      {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
      {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
      {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
      {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!columns_match(h->db, checks[i][0], checks[i][1])) {
      snprintf(err, (size_t)n, "unexpected schema");
      dbx_close(h);
      return 0;
    }
  }
  if (ENC(h->db) != SQLITE_UTF8) {
    snprintf(err, (size_t)n, "database is not UTF-8");
    dbx_close(h);
    return 0;
  }

  h->root[DBX_POSTS] = rootpage(h->db, "posts");
  h->root[DBX_USERS] = rootpage(h->db, "users");
  h->root[DBX_FEED_IDX] = rootpage(h->db, "posts_created_at_id_idx");
  h->root[DBX_LIKES_IDX] = rootpage(h->db, "likes_post_id_idx");
  if (!h->root[0] || !h->root[1] || !h->root[2] || !h->root[3]) {
    fail(h->db, err, n, "root pages");
    dbx_close(h);
    return 0;
  }
  h->ki_feed = feed_keyinfo(h->db);
  h->ki_likes = sqlite3KeyInfoAlloc(h->db, 1, 1);
  if (h->ki_likes) {
    for (int i = 0; i < 2; i++) h->ki_likes->aColl[i] = h->db->pDfltColl;
  }
  if (!h->ki_feed || !h->ki_likes) {
    fail(h->db, err, n, "keyinfo");
    dbx_close(h);
    return 0;
  }

  h->st_begin = prep(h->db, "BEGIN");
  h->st_commit = prep(h->db, "COMMIT");
  h->st_rollback = prep(h->db, "ROLLBACK");
  h->st_post = prep(h->db,
                    "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  h->st_like = prep(h->db,
                    "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT DO NOTHING");
  h->st_health = prep(h->db, "SELECT 1");
  if (!h->st_begin || !h->st_commit || !h->st_rollback || !h->st_post || !h->st_like ||
      !h->st_health) {
    fail(h->db, err, n, "prepare");
    dbx_close(h);
    return 0;
  }

  if (sqlite3_open_v2(path, &h->ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl")) {
    fail(h->db, err, n, "checkpoint connection");
    dbx_close(h);
    return 0;
  }
  h->bt = h->db->aDb[0].pBt;
  for (int i = 0; i < 4; i++) {
    h->cur[i] = (BtCursor *)sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!h->cur[i]) {
      fail(h->db, err, n, "cursor");
      dbx_close(h);
      return 0;
    }
  }
  return h;
}

int dbx_begin(dbx *h) {
  if (h->broken) return -1;
  if (h->in_tx) {
    if (!sqlite3_get_autocommit(h->db)) return 0;
    h->in_tx = 0;
    h->broken = 1;
    return -1;
  }
  if (step_reset(h->st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(h->bt, 0, 0) != SQLITE_OK) {
    step_reset(h->st_rollback);
    return -1;
  }
  for (int i = 0; i < 4; i++) {
    sqlite3BtreeCursorZero(h->cur[i]);
    KeyInfo *ki = (i == DBX_FEED_IDX) ? h->ki_feed : (i == DBX_LIKES_IDX ? h->ki_likes : 0);
    if (sqlite3BtreeCursor(h->bt, h->root[i], 0, ki, h->cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(h->cur[j]);
      step_reset(h->st_rollback);
      return -1;
    }
  }
  h->in_tx = 1;
  return 0;
}

int dbx_commit(dbx *h) {
  if (h->broken) {
    h->broken = 0;
    if (h->in_tx) {
      close_cursors(h);
      if (!sqlite3_get_autocommit(h->db)) step_reset(h->st_rollback);
    }
    return -1;
  }
  if (!h->in_tx) return 0;
  close_cursors(h);
  if (step_reset(h->st_commit) != SQLITE_DONE) {
    step_reset(h->st_rollback);
    return -1;
  }
  return 0;
}

int dbx_checkpoint(dbx *h) {
  int nlog = 0, ndone = 0;
  int rc = sqlite3_wal_checkpoint_v2(h->ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
  if (rc != SQLITE_OK) return -1;
  return (nlog == 0 || ndone >= nlog) ? 0 : 1;
}

int dbx_wal_frames(dbx *h) {
  return atomic_load_explicit(&h->wal_frames, memory_order_relaxed);
}

static i64 bt_cell_key(MemPage *pg, int i) {
  CellInfo info;
  pg->xParseCell(pg, findCell(pg, i), &info);
  return info.nKey;
}

static int bt_lower_bound(MemPage *pg, i64 k) {
  int n = pg->nCell;
  if (n == 0) return 0;
  i64 k0 = bt_cell_key(pg, 0);
  i64 kn = bt_cell_key(pg, n - 1);
  if (k <= k0) return 0;
  if (k > kn) return n;
  int lo = 1, hi = n - 1;
  int g = lo + (int)((double)(k - k0) / (double)(kn - k0) * (double)(n - 1) - 0.5);
  if (g < lo) g = lo;
  if (g > hi) g = hi;
  for (int probe = 0; lo < hi; probe++) {
    if (bt_cell_key(pg, g) >= k) hi = g;
    else lo = g + 1;
    if (probe < 2 && lo < hi) g = hi == g ? g - 1 : g + 1;
    else g = lo + ((hi - lo) >> 1);
    if (g < lo) g = lo;
    if (g > hi) g = hi;
  }
  return lo;
}

int dbx_users_seek_many(dbx *h, const long long *ids, int n, unsigned char *buf, unsigned *lens) {
  if (n <= 0 || n > 32) return -1;
  Btree *bt = h->bt;
  BtShared *bs = bt->pBt;
  MemPage *pg[32];
  Pgno next[32];
  for (int i = 0; i < n; i++) next[i] = h->root[DBX_USERS];
  for (int depth = 0; depth < BTCURSOR_MAX_DEPTH; depth++) {
    for (int i = 0; i < n; i++) {
      int j = i - 1;
      while (j >= 0 && next[j] != next[i]) j--;
      if (j >= 0) {
        pg[i] = pg[j];
        sqlite3PagerRef(pg[i]->pDbPage);
      } else if (getAndInitPage(bs, next[i], &pg[i], PAGER_GET_READONLY) != SQLITE_OK) {
        pg[i] = 0;
        for (int k = 0; k < i; k++)
          if (pg[k]) releasePageNotNull(pg[k]);
        return -1;
      }
      if (!pg[i] || !pg[i]->intKey || pg[i]->nCell == 0 || pg[i]->nOverflow ||
          (i > 0 && pg[i]->leaf != pg[0]->leaf)) {
        for (int k = 0; k <= i; k++)
          if (pg[k]) releasePageNotNull(pg[k]);
        return -1;
      }
      __builtin_prefetch(pg[i]->aCellIdx);
    }
    if (pg[0]->leaf) {
      for (int i = 0; i < n; i++) {
        lens[i] = 0;
        int ix = bt_lower_bound(pg[i], ids[i]);
        if (ix >= pg[i]->nCell || bt_cell_key(pg[i], ix) != ids[i]) continue;
        CellInfo info;
        pg[i]->xParseCell(pg[i], findCell(pg[i], ix), &info);
        if (info.nLocal != info.nPayload) continue;
        u32 copy = info.nPayload > 256 ? 256 : info.nPayload;
        memcpy(buf + (size_t)i * 256, info.pPayload, copy);
        lens[i] = info.nPayload;
      }
      for (int i = 0; i < n; i++)
        if (pg[i]) releasePageNotNull(pg[i]);
      return 0;
    }
    for (int i = 0; i < n; i++) {
      int ix = bt_lower_bound(pg[i], ids[i]);
      next[i] = ix >= pg[i]->nCell ? get4byte(&pg[i]->aData[pg[i]->hdrOffset + 8])
                                   : get4byte(findCell(pg[i], ix));
    }
    for (int i = 0; i < n; i++)
      if (pg[i]) releasePageNotNull(pg[i]);
  }
  return -1;
}

int dbx_health(dbx *h, char *err, int n) {
  int rc = sqlite3_step(h->st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(h->st_health, 0) == 1;
  sqlite3_reset(h->st_health);
  if (!ok) {
    snprintf(err, (size_t)n, "%s", sqlite3_errmsg(h->db));
    return -1;
  }
  return 0;
}

int dbx_first(dbx *h, int c) {
  int empty = 1;
  if (sqlite3BtreeFirst(h->cur[c], &empty) != SQLITE_OK) return -1;
  return empty ? 0 : 1;
}

int dbx_next(dbx *h, int c) {
  int rc = sqlite3BtreeNext(h->cur[c], 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

int dbx_seek(dbx *h, int c, long long rowid) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(h->cur[c], (i64)rowid, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && h->cur[c]->eState == CURSOR_VALID;
}

int dbx_seek_ge(dbx *h, int c, long long key) {
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  memset(&r, 0, sizeof r);
  m.flags = MEM_Int;
  m.u.i = key;
  r.pKeyInfo = h->cur[c]->pKeyInfo;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = 1;
  if (sqlite3BtreeIndexMoveto(h->cur[c], &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(h->cur[c])) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(h->cur[c], 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

const unsigned char *dbx_record(dbx *h, int c, unsigned *len) {
  u32 total = sqlite3BtreePayloadSize(h->cur[c]);
  if (total == 0) {
    *len = 0;
    return (const unsigned char *)"";
  }
  u32 amt = 0;
  const void *direct = sqlite3BtreePayloadFetch(h->cur[c], &amt);
  if (direct && amt == total) {
    *len = total;
    return (const unsigned char *)direct;
  }
  if (total > h->rec_cap) {
    sqlite3_free(h->rec);
    h->rec_cap = total + 4096;
    h->rec = (unsigned char *)sqlite3_malloc((int)h->rec_cap);
    if (!h->rec) {
      h->rec_cap = 0;
      return 0;
    }
  }
  if (sqlite3BtreePayload(h->cur[c], 0, total, h->rec) != SQLITE_OK) return 0;
  *len = total;
  return h->rec;
}

long long dbx_likes(dbx *h, long long post_id) {
  int r = dbx_seek_ge(h, DBX_LIKES_IDX, post_id);
  if (r <= 0) return r;
  long long count = 0;
  while (r == 1) {
    unsigned len = 0;
    const unsigned char *p = dbx_record(h, DBX_LIKES_IDX, &len);
    if (!p) return -1;
    long long pid = 0;
    if (first_int(p, len, &pid)) return -1;
    if (pid > post_id) break;
    if (pid == post_id) count++;
    r = dbx_next(h, DBX_LIKES_IDX);
  }
  return r < 0 ? -1 : count;
}

int dbx_likes_many(dbx *h, const long long *ids, int n, unsigned *counts) {
  if (n <= 0) return 0;
  memset(counts, 0, sizeof(unsigned) * (size_t)n);
  int r = dbx_seek_ge(h, DBX_LIKES_IDX, ids[0]);
  if (r < 0) return -1;
  int j = 0;
  while (r == 1) {
    unsigned len = 0;
    const unsigned char *p = dbx_record(h, DBX_LIKES_IDX, &len);
    if (!p) return -1;
    long long pid = 0;
    if (first_int(p, len, &pid)) return -1;
    while (j < n && ids[j] < pid) j++;
    if (j == n || pid > ids[n - 1]) break;
    if (ids[j] == pid) counts[j]++;
    r = dbx_next(h, DBX_LIKES_IDX);
  }
  return r < 0 ? -1 : 0;
}

static void check_tx(struct dbx *h) {
  if (h->in_tx && sqlite3_get_autocommit(h->db)) {
    h->in_tx = 0;
    h->broken = 1;
  }
}

int dbx_insert_post(dbx *h, long long uid, const char *body, int len, long long *id, char *created,
                    int *clen) {
  sqlite3_bind_int64(h->st_post, 1, uid);
  sqlite3_bind_text(h->st_post, 2, body, len, SQLITE_STATIC);
  int rc = sqlite3_step(h->st_post);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(h->st_post);
    check_tx(h);
    return -1;
  }
  *id = sqlite3_column_int64(h->st_post, 0);
  int n = sqlite3_column_bytes(h->st_post, 1);
  const unsigned char *t = sqlite3_column_text(h->st_post, 1);
  if (!t || n > 63) {
    sqlite3_reset(h->st_post);
    return -1;
  }
  memcpy(created, t, (size_t)n);
  *clen = n;
  rc = sqlite3_step(h->st_post);
  sqlite3_reset(h->st_post);
  sqlite3_clear_bindings(h->st_post);
  return rc == SQLITE_DONE ? 0 : -1;
}

int dbx_insert_like(dbx *h, long long uid, long long pid) {
  sqlite3_bind_int64(h->st_like, 1, uid);
  sqlite3_bind_int64(h->st_like, 2, pid);
  int rc = step_reset(h->st_like);
  if (rc != SQLITE_DONE) {
    check_tx(h);
    return -1;
  }
  return sqlite3_changes(h->db) > 0;
}
