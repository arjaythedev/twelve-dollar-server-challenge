// B-tree read path for the two fixed read shapes.  This file is the only
// translation unit that compiles SQLite: including the amalgamation here
// keeps SQLite's internal B-tree/cursor API visible.  A reader context owns
// its cursors, key infos and one packed like-key for the life of a database
// connection.  Each request still starts and ends its own read transaction;
// no row data survives a request.
#define _GNU_SOURCE 1
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sqlite3.h"
#include "sqlite/sqlite3.c"

/* sqlite3.c elides this under SQLITE_AMALGAMATION; cgo links the whole
   object, so sqlite3_libversion() still needs the symbol. */
const char sqlite3_version[] = SQLITE_VERSION;

static int stat_on;
static uint64_t stat_feed_calls, stat_feed_ns, stat_post_calls, stat_post_ns;
static inline uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void btree_stats_enable(int on) { stat_on = on; }
void btree_stats(uint64_t *out) {
  out[0] = __atomic_load_n(&stat_feed_calls, __ATOMIC_RELAXED);
  out[1] = __atomic_load_n(&stat_feed_ns, __ATOMIC_RELAXED);
  out[2] = __atomic_load_n(&stat_post_calls, __ATOMIC_RELAXED);
  out[3] = __atomic_load_n(&stat_post_ns, __ATOMIC_RELAXED);
}

typedef struct {
  char *p;
  int cap;
  int n;
  int err;
} jout;

static void jraw(jout *o, const char *s, int n) {
  if (n <= 0) return;
  if (o->err) return;
  if (n > o->cap - o->n) {
    o->err = 1;
    return;
  }
  memcpy(o->p + o->n, s, (size_t)n);
  o->n += n;
}

static void jch(jout *o, char c) { jraw(o, &c, 1); }

static void jint(jout *o, sqlite3_int64 v) {
  char b[24];
  int n = 0;
  uint64_t u;
  if (v < 0) {
    u = (uint64_t)(-(v + 1)) + 1;
  } else {
    u = (uint64_t)v;
  }
  do {
    b[n++] = (char)('0' + (u % 10));
    u /= 10;
  } while (u);
  if (v < 0) b[n++] = '-';
  char out[24];
  for (int i = 0; i < n; i++) out[i] = b[n - 1 - i];
  jraw(o, out, n);
}

static void jstr(jout *o, const unsigned char *s, int n) {
  static const char hex[] = "0123456789abcdef";
  jch(o, '"');
  if (s && n > 0) {
    int start = 0;
    for (int i = 0; i < n; i++) {
      unsigned char c = s[i];
      const char *esc = NULL;
      char buf[6];
      int elen = 0;
      switch (c) {
        case '"': esc = "\\\""; elen = 2; break;
        case '\\': esc = "\\\\"; elen = 2; break;
        case '\b': esc = "\\b"; elen = 2; break;
        case '\f': esc = "\\f"; elen = 2; break;
        case '\n': esc = "\\n"; elen = 2; break;
        case '\r': esc = "\\r"; elen = 2; break;
        case '\t': esc = "\\t"; elen = 2; break;
        default:
          if (c < 0x20) {
            buf[0] = '\\';
            buf[1] = 'u';
            buf[2] = '0';
            buf[3] = '0';
            buf[4] = hex[c >> 4];
            buf[5] = hex[c & 15];
            esc = buf;
            elen = 6;
          }
      }
      if (esc) {
        if (i > start) jraw(o, (const char *)s + start, i - start);
        jraw(o, esc, elen);
        start = i + 1;
      }
    }
    if (n > start) jraw(o, (const char *)s + start, n - start);
  }
  jch(o, '"');
}

enum { V_NULL = 0, V_INT = 1, V_TEXT = 2 };

typedef struct {
  int kind;
  sqlite3_int64 i;
  const unsigned char *z;
  int n;
} val;

static sqlite3_int64 read_int(const unsigned char *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 8) | p[i];
  if (n < 8 && (v & (UINT64_C(1) << (n * 8 - 1)))) {
    v |= ~((UINT64_C(1) << (n * 8)) - 1);
  }
  return (sqlite3_int64)v;
}

// Parse the first `want` fields of a SQLite record without constructing Mem
// objects.  This schema only needs null, integer and text fields.
static int parse_vals(const unsigned char *p, u32 n, val *v, int want) {
  u32 hdr = 0;
  int hb = sqlite3GetVarint32(p, &hdr);
  if (hb <= 0 || hdr > n || (u32)hb > hdr) return SQLITE_CORRUPT;
  const unsigned char *h = p + hb;
  const unsigned char *data = p + hdr;
  u32 off = 0;
  for (int i = 0; i < want; i++) {
    if (h >= p + hdr) return SQLITE_CORRUPT;
    u32 st = 0;
    int nb = sqlite3GetVarint32(h, &st);
    if (nb <= 0) return SQLITE_CORRUPT;
    h += nb;
    u32 len = sqlite3VdbeSerialTypeLen(st);
    if (off + len > (u32)(p + n - data)) return SQLITE_CORRUPT;
    val *x = &v[i];
    memset(x, 0, sizeof(*x));
    if (st == 0) {
      x->kind = V_NULL;
    } else if (st <= 6) {
      x->kind = V_INT;
      x->i = read_int(data + off, (int)len);
    } else if (st == 8 || st == 9) {
      x->kind = V_INT;
      x->i = (sqlite3_int64)(st - 8);
    } else if (st >= 12) {
      x->kind = V_TEXT;
      x->z = data + off;
      x->n = (int)len;
    } else {
      return SQLITE_CORRUPT;
    }
    off += len;
  }
  return SQLITE_OK;
}

typedef struct BtreeReader BtreeReader;

struct BtreeReader {
  sqlite3 *db;
  Btree *bt;
  BtCursor *feed;
  BtCursor *post;
  BtCursor *user;
  BtCursor *like;
  KeyInfo *feedKey;
  KeyInfo *likeKey;
  UnpackedRecord *likeFind;
  Pgno feed_root;
  Pgno post_root;
  Pgno user_root;
  Pgno like_root;
};

static BtCursor *open_cursor(Btree *bt, Pgno root, KeyInfo *key, int *rc) {
  int sz = sqlite3BtreeCursorSize();
  BtCursor *c = (BtCursor *)sqlite3_malloc64((sqlite3_uint64)sz);
  if (!c) {
    *rc = SQLITE_NOMEM;
    return NULL;
  }
  memset(c, 0, (size_t)sz);
  *rc = sqlite3BtreeCursor(bt, root, 0, key, c);
  if (*rc != SQLITE_OK) {
    sqlite3_free(c);
    return NULL;
  }
  return c;
}

static void close_cursor(BtCursor *c) {
  if (c) {
    sqlite3BtreeCloseCursor(c);
    sqlite3_free(c);
  }
}

static void reader_free(BtreeReader *r) {
  if (!r) return;
  close_cursor(r->feed);
  close_cursor(r->post);
  close_cursor(r->user);
  close_cursor(r->like);
  if (r->likeFind) sqlite3DbFree(r->db, r->likeFind);
  if (r->feedKey) sqlite3KeyInfoUnref(r->feedKey);
  if (r->likeKey) sqlite3KeyInfoUnref(r->likeKey);
  sqlite3_free(r);
}

BtreeReader *btree_reader_new(sqlite3 *db, unsigned posts_root,
                              unsigned users_root, unsigned posts_idx_root,
                              unsigned likes_idx_root) {
  BtreeReader *r = (BtreeReader *)sqlite3_malloc64(sizeof(*r));
  if (!r) return NULL;
  memset(r, 0, sizeof(*r));
  r->db = db;
  r->bt = db->aDb[0].pBt;
  if (!r->bt) goto fail;

  r->feedKey = sqlite3KeyInfoAlloc(db, 2, 1);
  r->likeKey = sqlite3KeyInfoAlloc(db, 2, 0);
  if (!r->feedKey || !r->likeKey) goto fail;
  // posts_created_at_id_idx is (created_at DESC, id DESC).
  r->feedKey->aSortFlags[0] = KEYINFO_ORDER_DESC;
  r->feedKey->aSortFlags[1] = KEYINFO_ORDER_DESC;

  int rc = SQLITE_OK;
  r->feed_root = (Pgno)posts_idx_root;
  r->post_root = (Pgno)posts_root;
  r->user_root = (Pgno)users_root;
  r->like_root = (Pgno)likes_idx_root;
  r->feed = open_cursor(r->bt, r->feed_root, r->feedKey, &rc);
  if (!r->feed) goto fail;
  r->post = open_cursor(r->bt, r->post_root, NULL, &rc);
  if (!r->post) goto fail;
  r->user = open_cursor(r->bt, r->user_root, NULL, &rc);
  if (!r->user) goto fail;
  r->like = open_cursor(r->bt, r->like_root, r->likeKey, &rc);
  if (!r->like) goto fail;

  r->likeFind = sqlite3VdbeAllocUnpackedRecord(r->likeKey);
  if (!r->likeFind) goto fail;
  r->likeFind->nField = 2;
  r->likeFind->r1 = -1;
  r->likeFind->r2 = 1;
  r->likeFind->default_rc = 0;
  return r;

fail:
  reader_free(r);
  return NULL;
}

static int manage_tx = 1;

static int reader_begin(BtreeReader *r) {
  if (!manage_tx) return SQLITE_OK;
  return sqlite3BtreeBeginTrans(r->bt, 0, NULL);
}

static int reader_end(BtreeReader *r, int ok) {
  if (!manage_tx) return SQLITE_OK;
  if (ok) return sqlite3BtreeCommit(r->bt);
  return sqlite3BtreeRollback(r->bt, SQLITE_OK, 0);
}

static const unsigned char *payload(BtCursor *c, u32 *n) {
  *n = sqlite3BtreePayloadSize(c);
  return (const unsigned char *)sqlite3BtreePayloadFetch(c, n);
}

static int get_varint(const unsigned char *p, u32 n, uint64_t *out, int *used) {
  uint64_t v = 0;
  for (int i = 0; i < n && i < 9; i++) {
    unsigned char c = p[i];
    if (i == 8) {
      v = (v << 8) | c;
      *out = v;
      *used = 9;
      return SQLITE_OK;
    }
    v = (v << 7) | (c & 0x7f);
    if (!(c & 0x80)) {
      *out = v;
      *used = i + 1;
      return SQLITE_OK;
    }
  }
  return SQLITE_CORRUPT;
}

/* Decode field 0 of a record when it is an integer. */
static int first_int(const unsigned char *p, u32 n, sqlite3_int64 *out) {
  uint64_t hdr;
  int hl;
  if (get_varint(p, n, &hdr, &hl) != SQLITE_OK || hdr > n || hdr <= (uint64_t)hl) {
    return SQLITE_CORRUPT;
  }
  uint64_t st;
  int sl;
  if (get_varint(p + hl, n - hl, &st, &sl) != SQLITE_OK || (uint64_t)hl + (uint64_t)sl > hdr) {
    return SQLITE_CORRUPT;
  }
  if (st == 8) {
    *out = 0;
    return SQLITE_OK;
  }
  if (st == 9) {
    *out = 1;
    return SQLITE_OK;
  }
  if (st == 0) return SQLITE_NOTFOUND;
  if (st < 1 || st > 6) return SQLITE_NOTFOUND;
  uint64_t off = (uint64_t)hl + (uint64_t)sl;
  if (off + st > n) return SQLITE_CORRUPT;
  *out = read_int(p + off, (int)st);
  return SQLITE_OK;
}

typedef struct {
  sqlite3_int64 id;
  int idx;
} LikePair;

static void like_find_set(BtreeReader *r, sqlite3_int64 post_id) {
  Mem *m = &r->likeFind->aMem[0];
  m->flags = MEM_Int;
  m->u.i = post_id;
  m->n = 0;
  m->z = 0;
  m->szMalloc = 0;
  Mem *rid = &r->likeFind->aMem[1];
  rid->flags = MEM_Int;
  rid->u.i = 0;
  rid->n = 0;
  rid->z = 0;
  rid->szMalloc = 0;
  r->likeFind->nField = 2;
  r->likeFind->default_rc = 0;
  r->likeFind->eqSeen = 0;
}

/* Count likes for every requested post id with one forward walk over the
   (post_id, rowid) index.  Pairs are sorted so the cursor only moves
   forward; no per-post re-seek. */
static int like_counts(BtreeReader *r, const sqlite3_int64 *ids, int n,
                       sqlite3_int64 *likes) {
  for (int i = 0; i < n; i++) likes[i] = 0;
  if (n <= 0) return SQLITE_OK;

  LikePair local[64];
  LikePair *pairs = local;
  if (n > (int)(sizeof(local) / sizeof(local[0]))) {
    pairs = (LikePair *)sqlite3_malloc64((sqlite3_uint64)n * sizeof(*pairs));
    if (!pairs) return SQLITE_NOMEM;
  }
  for (int i = 0; i < n; i++) {
    pairs[i].id = ids[i];
    pairs[i].idx = i;
  }
  for (int i = 1; i < n; i++) {
    LikePair key = pairs[i];
    int j = i - 1;
    while (j >= 0 && pairs[j].id > key.id) {
      pairs[j + 1] = pairs[j];
      j--;
    }
    pairs[j + 1] = key;
  }

  like_find_set(r, pairs[0].id);
  int res = 0;
  int rc = sqlite3BtreeIndexMoveto(r->like, r->likeFind, &res);
  int pi = 0;
  sqlite3_int64 prev = 0;
  while (rc == SQLITE_OK && pi < n && !sqlite3BtreeEof(r->like)) {
    sqlite3_int64 target = pairs[pi].id;
    /* Random post ids can be far apart.  Walking every like between
       them would scan the whole index, so reseek across large gaps.
       Dense ids (the 20-post feed) still use one forward walk. */
    if (pi > 0 && (target < prev || target - prev > 4096)) {
      like_find_set(r, target);
      rc = sqlite3BtreeIndexMoveto(r->like, r->likeFind, &res);
      if (rc != SQLITE_OK) break;
      if (sqlite3BtreeEof(r->like)) break;
    }
    sqlite3_int64 key = 0;
    int ir = SQLITE_OK;

    while (!sqlite3BtreeEof(r->like)) {
      u32 pn;
      const unsigned char *p = payload(r->like, &pn);
      if (!p) { rc = SQLITE_CORRUPT; break; }
      ir = first_int(p, pn, &key);
      if (ir != SQLITE_OK) break;
      if (key >= target) break;
      rc = sqlite3BtreeNext(r->like, 0);
      if (rc != SQLITE_OK) break;
    }
    if (rc != SQLITE_OK || ir != SQLITE_OK) {
      if (rc == SQLITE_OK && ir != SQLITE_OK) rc = SQLITE_CORRUPT;
      break;
    }
    if (sqlite3BtreeEof(r->like)) break;
    if (key > target) {
      pi++;
      continue;
    }

    sqlite3_int64 count = 0;
    while (!sqlite3BtreeEof(r->like)) {
      u32 pn;
      const unsigned char *p = payload(r->like, &pn);
      if (!p) { rc = SQLITE_CORRUPT; break; }
      ir = first_int(p, pn, &key);
      if (ir != SQLITE_OK) break;
      if (key != target) break;
      count++;
      rc = sqlite3BtreeNext(r->like, 0);
      if (rc != SQLITE_OK) break;
    }
    if (rc != SQLITE_OK) break;
    if (ir != SQLITE_OK) { rc = SQLITE_CORRUPT; break; }
    likes[pairs[pi].idx] = count;
    prev = target;
    pi++;
  }

  if (rc == SQLITE_DONE) rc = SQLITE_OK;
  if (pairs != local) sqlite3_free(pairs);
  return rc;
}

static int like_count(BtreeReader *r, sqlite3_int64 post_id, jout *o) {
  sqlite3_int64 ids[1] = {post_id};
  sqlite3_int64 likes[1] = {0};
  int rc = like_counts(r, ids, 1, likes);
  if (rc == SQLITE_OK) jint(o, likes[0]);
  return rc;
}

static int emit_post(BtreeReader *r, sqlite3_int64 id, sqlite3_int64 likes, jout *o) {
  int res = 0;
  int rc = sqlite3BtreeTableMoveto(r->post, id, 0, &res);
  if (rc != SQLITE_OK) return rc;
  if (res != 0) return SQLITE_NOTFOUND;

  u32 pn;
  const unsigned char *p = payload(r->post, &pn);
  if (!p) return SQLITE_CORRUPT;
  val pf[4];
  if (parse_vals(p, pn, pf, 4) != SQLITE_OK) return SQLITE_CORRUPT;
  if (pf[1].kind != V_INT || pf[2].kind != V_TEXT || pf[3].kind != V_TEXT) {
    return SQLITE_CORRUPT;
  }

  rc = sqlite3BtreeTableMoveto(r->user, pf[1].i, 0, &res);
  if (rc != SQLITE_OK) return rc;
  if (res != 0) return SQLITE_NOTFOUND;
  u32 un;
  const unsigned char *up = payload(r->user, &un);
  if (!up) return SQLITE_CORRUPT;
  val uf[2];
  if (parse_vals(up, un, uf, 2) != SQLITE_OK || uf[1].kind != V_TEXT) {
    return SQLITE_CORRUPT;
  }

  jraw(o, "{\"id\":", 6);
  jint(o, id);
  jraw(o, ",\"body\":", 8);
  jstr(o, pf[2].z, pf[2].n);
  jraw(o, ",\"created_at\":", 14);
  jstr(o, pf[3].z, pf[3].n);
  jraw(o, ",\"author\":", 10);
  jstr(o, uf[1].z, uf[1].n);
  jraw(o, ",\"like_count\":", 14);
  jint(o, likes);
  jch(o, '}');
  return SQLITE_OK;
}

int feed_btree(BtreeReader *r, char *out, int cap, int *outlen) {
  uint64_t t0 = stat_on ? mono_ns() : 0;
  jout o = {out, cap, 0, 0};
  *outlen = 0;
  int rc = reader_begin(r);
  if (rc != SQLITE_OK) {
    sqlite3BtreeRollback(r->bt, SQLITE_OK, 0);
    return rc;
  }

  int res = 0;
  sqlite3_int64 feed_ids[20];
  sqlite3_int64 feed_likes[20];
  int nfeed = 0;
  rc = sqlite3BtreeFirst(r->feed, &res);
  while (rc == SQLITE_OK && !sqlite3BtreeEof(r->feed) && nfeed < 20) {
    u32 n;
    const unsigned char *p = payload(r->feed, &n);
    if (!p) {
      rc = SQLITE_CORRUPT;
      break;
    }
    val f[2];
    if (parse_vals(p, n, f, 2) != SQLITE_OK || f[1].kind != V_INT) {
      rc = SQLITE_CORRUPT;
      break;
    }
    feed_ids[nfeed++] = f[1].i;
    rc = sqlite3BtreeNext(r->feed, 0);
  }
  if (rc == SQLITE_DONE) rc = SQLITE_OK;
  if (rc == SQLITE_OK && nfeed > 0) {
    rc = like_counts(r, feed_ids, nfeed, feed_likes);
  }
  if (rc == SQLITE_OK) {
    jch(&o, '[');
    for (int i = 0; i < nfeed; i++) {
      if (i) jch(&o, ',');
      rc = emit_post(r, feed_ids[i], feed_likes[i], &o);
      if (rc != SQLITE_OK) break;
    }
    if (rc == SQLITE_OK) jch(&o, ']');
  }
  int endrc = reader_end(r, rc == SQLITE_OK);
  if (rc == SQLITE_OK) rc = endrc;
  if (o.err && rc == SQLITE_OK) rc = SQLITE_TOOBIG;
  *outlen = o.n;
  if (stat_on) {
    __atomic_add_fetch(&stat_feed_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_feed_ns, mono_ns() - t0, __ATOMIC_RELAXED);
  }
  return rc;
}

int posts_btree(BtreeReader *r, const sqlite3_int64 *ids, int n, char *out,
                int cap, int *outlen, int *lens) {
  *outlen = 0;
  if (n <= 0) return SQLITE_OK;
  for (int i = 0; i < n; i++) lens[i] = 0;

  int rc = reader_begin(r);
  if (rc != SQLITE_OK) return rc;

  sqlite3_int64 local_likes[64];
  sqlite3_int64 *likes = local_likes;
  if (n > (int)(sizeof(local_likes) / sizeof(local_likes[0]))) {
    likes = (sqlite3_int64 *)sqlite3_malloc64((sqlite3_uint64)n * sizeof(*likes));
    if (!likes) {
      reader_end(r, 0);
      return SQLITE_NOMEM;
    }
  }
  rc = like_counts(r, ids, n, likes);
  if (rc != SQLITE_OK) {
    if (likes != local_likes) sqlite3_free(likes);
    reader_end(r, 0);
    return rc;
  }

  int pos = 0;
  for (int i = 0; i < n; i++) {
    jout o = {out + pos, cap - pos, 0, 0};
    rc = emit_post(r, ids[i], likes[i], &o);
    if (rc == SQLITE_NOTFOUND) {
      rc = SQLITE_OK;
      continue;
    }
    if (rc != SQLITE_OK) break;
    if (o.err) {
      rc = SQLITE_TOOBIG;
      break;
    }
    lens[i] = o.n;
    pos += o.n;
  }
  if (likes != local_likes) sqlite3_free(likes);
  int endrc = reader_end(r, rc == SQLITE_OK);
  if (rc == SQLITE_OK) rc = endrc;
  *outlen = pos;
  return rc;
}

int feed_btree_tx(BtreeReader *r, char *out, int cap, int *outlen) {
  manage_tx = 0;
  int rc = feed_btree(r, out, cap, outlen);
  manage_tx = 1;
  return rc;
}

int posts_btree_tx(BtreeReader *r, const sqlite3_int64 *ids, int n, char *out,
                   int cap, int *outlen, int *lens) {
  manage_tx = 0;
  int rc = posts_btree(r, ids, n, out, cap, outlen, lens);
  manage_tx = 1;
  return rc;
}

void btree_reader_refresh_feed(BtreeReader *r) {
  int rc = SQLITE_OK;
  BtCursor *c = open_cursor(r->bt, r->feed_root, r->feedKey, &rc);
  if (c) { close_cursor(r->feed); r->feed = c; }
}

void btree_reader_refresh_post(BtreeReader *r) {
  int rc = SQLITE_OK;
  BtCursor *c = open_cursor(r->bt, r->post_root, NULL, &rc);
  if (c) { close_cursor(r->post); r->post = c; }
}

void btree_reader_refresh_like(BtreeReader *r) {
  int rc = SQLITE_OK;
  BtCursor *c = open_cursor(r->bt, r->like_root, r->likeKey, &rc);
  if (c) { close_cursor(r->like); r->like = c; }
}

void btree_reader_free(BtreeReader *r) {
  reader_free(r);
}
