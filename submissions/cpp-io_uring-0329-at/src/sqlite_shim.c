#include "sqlite3.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite_shim.h"

static struct {
  Btree *bt;
  Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
  BtCursor *posts, *users, *feed_idx, *likes_idx;
  KeyInfo *ki_feed, *ki_likes;
  int ok, open;
} g;

static unsigned char g_a[8192], g_b[1024], g_c[256];

static Pgno root_page(sqlite3 *db, const char *name) {
  sqlite3_stmt *s = 0;
  Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name=?1", -1, &s, 0) == SQLITE_OK) {
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
  }
  return r;
}

static int table_ok(sqlite3 *db, const char *t, const char *expect) {
  char sql[96], got[512];
  sqlite3_stmt *s = 0;
  size_t n = 0;
  got[0] = 0;
  snprintf(sql, sizeof sql, "PRAGMA table_info(\"%s\")", t);
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0) != SQLITE_OK) return 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got) {
    const unsigned char *a = sqlite3_column_text(s, 1);
    const unsigned char *b = sqlite3_column_text(s, 2);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s:%s:%d", n ? "," : "",
                          a ? (const char *)a : "?", b ? (const char *)b : "?",
                          sqlite3_column_int(s, 5));
  }
  sqlite3_finalize(s);
  if (strcmp(got, expect)) {
    fprintf(stderr, "shim: %s layout %s (want %s)\n", t, got, expect);
    return 0;
  }
  return 1;
}

static KeyInfo *keyinfo(sqlite3 *db, int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) return NULL;
  for (int i = 0; i < nkey + nextra; i++) {
    k->aColl[i] = db->pDfltColl;
    if (desc && i < nkey) k->aSortFlags[i] = KEYINFO_ORDER_DESC;
  }
  return k;
}

int shim_init(sqlite3 *db) {
  memset(&g, 0, sizeof g);
  g.bt = db->aDb[0].pBt;
  if (!g.bt) return 0;
  if (!table_ok(db, "users", "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0") ||
      !table_ok(db, "posts", "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0")) {
    return 0;
  }
  g.root_posts = root_page(db, "posts");
  g.root_users = root_page(db, "users");
  g.root_feed_idx = root_page(db, "posts_created_at_id_idx");
  g.root_likes_idx = root_page(db, "likes_post_id_idx");
  if (!g.root_posts || !g.root_users || !g.root_feed_idx || !g.root_likes_idx) {
    fprintf(stderr, "shim: missing table or index\n");
    return 0;
  }
  g.ki_feed = keyinfo(db, 2, 1, 1);
  g.ki_likes = keyinfo(db, 1, 1, 0);
  size_t sz = (size_t)sqlite3BtreeCursorSize();
  g.posts = (BtCursor *)malloc(sz);
  g.users = (BtCursor *)malloc(sz);
  g.feed_idx = (BtCursor *)malloc(sz);
  g.likes_idx = (BtCursor *)malloc(sz);
  if (!g.ki_feed || !g.ki_likes || !g.posts || !g.users || !g.feed_idx || !g.likes_idx) return 0;
  g.ok = 1;
  return 1;
}

void shim_txn_begin(sqlite3 *db) {
  (void)db;
  if (!g.ok || g.open) return;
  if (sqlite3BtreeBeginTrans(g.bt, 0, 0) != SQLITE_OK) return;
  sqlite3BtreeCursorZero(g.posts);
  sqlite3BtreeCursorZero(g.users);
  sqlite3BtreeCursorZero(g.feed_idx);
  sqlite3BtreeCursorZero(g.likes_idx);
  if (sqlite3BtreeCursor(g.bt, g.root_posts, 0, NULL, g.posts) != SQLITE_OK) return;
  if (sqlite3BtreeCursor(g.bt, g.root_users, 0, NULL, g.users) != SQLITE_OK) return;
  if (sqlite3BtreeCursor(g.bt, g.root_feed_idx, 0, g.ki_feed, g.feed_idx) != SQLITE_OK) return;
  if (sqlite3BtreeCursor(g.bt, g.root_likes_idx, 0, g.ki_likes, g.likes_idx) != SQLITE_OK) return;
  g.open = 1;
}

void shim_txn_end(void) {
  if (!g.open) return;
  sqlite3BtreeCloseCursor(g.posts);
  sqlite3BtreeCloseCursor(g.users);
  sqlite3BtreeCloseCursor(g.feed_idx);
  sqlite3BtreeCloseCursor(g.likes_idx);
  g.open = 0;
}

struct col { int type; long long i; const unsigned char *p; int n; };

static inline void rd_varint(const unsigned char **pp, const unsigned char *end,
                             unsigned long long *out) {
  const unsigned char *p = *pp;
  if (p < end && *p < 0x80) {
    *out = *p;
    *pp = p + 1;
    return;
  }
  unsigned long long v = 0;
  for (int i = 0; i < 9; i++) {
    if (p >= end) break;
    if (i == 8) { v = (v << 8) | *p++; break; }
    v = (v << 7) | (unsigned long long)(*p & 0x7f);
    if (!(*p++ & 0x80)) break;
  }
  *pp = p;
  *out = v;
}

static inline int int_of(int t, const unsigned char *b, long long *out, int *nbytes) {
  static const int kSz[7] = {0, 1, 2, 3, 4, 6, 8};
  if (t == 8) { *out = 0; *nbytes = 0; return 0; }
  if (t == 9) { *out = 1; *nbytes = 0; return 0; }
  if (t < 1 || t > 6) return -1;
  int n = kSz[t];
  long long v = (long long)(signed char)b[0];
  for (int i = 1; i < n; i++) v = (v << 8) | b[i];
  *out = v;
  *nbytes = n;
  return 0;
}

static int decode(const unsigned char *rec, int n, struct col *f, int nf) {
  const unsigned char *end = rec + n, *p = rec;
  unsigned long long hdr = 0;
  rd_varint(&p, end, &hdr);
  if (hdr < (unsigned long long)(p - rec) || hdr > (unsigned long long)n) return -1;
  const unsigned char *he = rec + hdr, *d = he;
  for (int i = 0; i < nf; i++) {
    unsigned long long t = 0;
    if (p >= he) return -1;
    rd_varint(&p, he, &t);
    f[i].type = (int)t;
    f[i].i = 0;
    f[i].p = 0;
    f[i].n = 0;
    if (t == 0) continue;
    if (t <= 6 || t == 8 || t == 9) {
      long long v = 0;
      int nb = 0;
      if (int_of((int)t, d, &v, &nb) != 0) return -1;
      if (d + nb > end) return -1;
      f[i].i = v;
      d += nb;
      continue;
    }
    if (t == 7) {
      if (d + 8 > end) return -1;
      d += 8;
      continue;
    }
    if (t >= 12) {
      int sz = (int)((t & 1) ? (t - 13) / 2 : (t - 12) / 2);
      if (d + sz > end) return -1;
      f[i].p = d;
      f[i].n = sz;
      d += sz;
      continue;
    }
    return -1;
  }
  return 0;
}

static int table_row(BtCursor *cur, long long rowid, unsigned char *buf, size_t cap,
                     struct col *f, int nf) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur, (i64)rowid, 0, &res) != SQLITE_OK) return -1;
  if (res != 0) return 1;
  u32 n = sqlite3BtreePayloadSize(cur);
  if (n == 0 || n > cap) return -1;
  if (sqlite3BtreePayload(cur, 0, n, buf) != SQLITE_OK) return -1;
  return decode(buf, (int)n, f, nf) == 0 ? 0 : -1;
}

int shim_get_post(sqlite3 *db, long long id, struct shim_post *out) {
  (void)db;
  if (!g.ok || !g.open) return -1;

  struct col p[3];
  int rc = table_row(g.posts, id, g_a, sizeof g_a, p, 3);
  if (rc != 0) return rc;
  if (p[0].type < 1 || p[0].type > 9) return -1;
  if (p[1].type < 13 || (p[1].type & 1) == 0) return -1;
  if (p[2].type < 13 || (p[2].type & 1) == 0) return -1;

  struct col u[2];
  if (table_row(g.users, p[0].i, g_b, sizeof g_b, u, 2) != 0) return -1;
  if (u[0].type < 13 || (u[0].type & 1) == 0) return -1;

  out->body = (const char *)p[1].p;
  out->body_len = p[1].n;
  out->created_at = (const char *)p[2].p;
  out->created_len = p[2].n;
  out->author = (const char *)u[0].p;
  out->author_len = u[0].n;
  return 0;
}

static int count_likes_one(long long post_id) {
  UnpackedRecord *k = sqlite3VdbeAllocUnpackedRecord(g.ki_likes);
  if (!k) return -1;
  k->nField = 1;
  k->aMem[0].flags = MEM_Int;
  k->aMem[0].u.i = post_id;
  int res = 0;
  if (sqlite3BtreeIndexMoveto(g.likes_idx, k, &res) != SQLITE_OK) return -1;
  if (res != 0) return 0;
  int n = 0;
  for (;;) {
    u32 sz = sqlite3BtreePayloadSize(g.likes_idx);
    if (sz == 0 || sz > sizeof g_c) break;
    if (sqlite3BtreePayload(g.likes_idx, 0, sz, g_c) != SQLITE_OK) break;
    struct col f[2];
    if (decode(g_c, (int)sz, f, 2) != 0) break;
    if (f[0].type < 1 || f[0].type > 9 || f[0].i != post_id) break;
    n++;
    if (sqlite3BtreeNext(g.likes_idx, 0) != SQLITE_OK) break;
  }
  return n;
}

static int count_likes_all(const long long *ids, int n, long long *counts) {
  for (int i = 0; i < n; i++) counts[i] = 0;
  if (n == 0) return 0;

  long long lo = ids[n - 1], hi = ids[0];
  if (hi - lo > 512) {
    for (int i = 0; i < n; i++) {
      int c = count_likes_one(ids[i]);
      if (c < 0) return -1;
      counts[i] = c;
    }
    return 0;
  }

  UnpackedRecord *k = sqlite3VdbeAllocUnpackedRecord(g.ki_likes);
  if (!k) return -1;
  k->nField = 1;
  k->aMem[0].flags = MEM_Int;
  k->aMem[0].u.i = lo;

  int res = 0;
  if (sqlite3BtreeIndexMoveto(g.likes_idx, k, &res) != SQLITE_OK) return -1;
  if (res != 0) return 0;

  int j = n - 1;
  for (;;) {
    u32 sz = sqlite3BtreePayloadSize(g.likes_idx);
    if (sz == 0 || sz > sizeof g_c) break;
    if (sqlite3BtreePayload(g.likes_idx, 0, sz, g_c) != SQLITE_OK) break;
    struct col f[2];
    if (decode(g_c, (int)sz, f, 2) != 0) break;
    if (f[0].type < 1 || f[0].type > 9) break;
    long long pid = f[0].i;
    if (pid > hi) break;
    while (j >= 0 && ids[j] < pid) j--;
    if (j >= 0 && ids[j] == pid) counts[j]++;
    if (sqlite3BtreeNext(g.likes_idx, 0) != SQLITE_OK) break;
  }
  return 0;
}

int shim_feed(sqlite3 *db, struct shim_feed_entry *out, int max) {
  (void)db;
  if (!g.ok || !g.open) return -1;

  long long ids[32];
  int n = 0;
  int res = 0;
  if (sqlite3BtreeFirst(g.feed_idx, &res) != SQLITE_OK) return -1;

  while (res == 0 && n < max) {
    u32 sz = sqlite3BtreePayloadSize(g.feed_idx);
    if (sz == 0 || sz > sizeof g_a) return -1;
    if (sqlite3BtreePayload(g.feed_idx, 0, sz, g_a) != SQLITE_OK) return -1;
    struct col f[2];
    if (decode(g_a, (int)sz, f, 2) != 0) return -1;
    if (f[1].type < 1 || f[1].type > 9) return -1;
    ids[n++] = f[1].i;
    if (n >= max) break;
    if (sqlite3BtreeNext(g.feed_idx, 0) != SQLITE_OK) res = 1;
  }

  long long likes[32];
  if (count_likes_all(ids, n, likes) != 0) return -1;

  for (int i = 0; i < n; i++) {
    struct col p[3];
    if (table_row(g.posts, ids[i], g_a, sizeof g_a, p, 3) != 0) return -1;
    if (p[1].type < 13 || (p[1].type & 1) == 0) return -1;
    if (p[2].type < 13 || (p[2].type & 1) == 0) return -1;
    struct col u[2];
    if (table_row(g.users, p[0].i, g_b, sizeof g_b, u, 2) != 0) return -1;
    if (u[0].type < 13 || (u[0].type & 1) == 0) return -1;
    out[i].id = ids[i];
    out[i].body = (const char *)p[1].p;
    out[i].body_len = p[1].n;
    out[i].created_at = (const char *)p[2].p;
    out[i].created_len = p[2].n;
    out[i].author = (const char *)u[0].p;
    out[i].author_len = u[0].n;
    out[i].like_count = likes[i];
  }
  return n;
}