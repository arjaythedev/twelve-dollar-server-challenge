#ifndef BTREE_H
#define BTREE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEED_N 20
#define ROW_BODY_MAX 2048    /* 500 characters, at most 4 bytes each */
#define ROW_NAME_MAX 255
#define ROW_STAMP_MAX 48
#define LIKES_SPAN 256       /* widest id range counted with a single index scan */
#define FEED_SLOTS 32        /* cursor pairs for the feed, picked by post id */

struct bt_spill {
  uint8_t *buf;
  uint32_t cap;
};

struct btree_state {
  Btree *pBt;
  Pgno root_posts, root_users, root_likes_idx, root_posts_idx;
  KeyInfo *ki_likes, *ki_posts_idx;
  BtCursor *cur_posts, *cur_users, *cur_likes, *cur_posts_idx;
  BtCursor *cur_fpost[FEED_SLOTS], *cur_fuser[FEED_SLOTS];
  uint32_t fopen; /* slots whose cursors are open in this transaction */
  int ready; /* roots and key infos resolved */
  int open;  /* cursors open in the current transaction */
  struct bt_spill spill_post, spill_user;
};

static Pgno bt_rootpage(sqlite3 *db, const char *name) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &st, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  Pgno root = 0;
  if (sqlite3_step(st) == SQLITE_ROW) root = (Pgno)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return root;
}

static int bt_setup(struct btree_state *bs, sqlite3 *db) {
  memset(bs, 0, sizeof *bs);
  bs->pBt = db->aDb[0].pBt;
  bs->root_posts = bt_rootpage(db, "posts");
  bs->root_users = bt_rootpage(db, "users");
  bs->root_likes_idx = bt_rootpage(db, "likes_post_id_idx");
  bs->root_posts_idx = bt_rootpage(db, "posts_created_at_id_idx");
  if (!bs->pBt || !bs->root_posts || !bs->root_users || !bs->root_likes_idx || !bs->root_posts_idx) {
    fprintf(stderr, "btree: schema objects not found, reads will use SQL\n");
    return 0;
  }

  bs->ki_likes = sqlite3KeyInfoAlloc(db, 1, 1);
  bs->ki_posts_idx = sqlite3KeyInfoAlloc(db, 2, 0);
  if (!bs->ki_likes || !bs->ki_posts_idx || !db->pDfltColl) return 0;
  bs->ki_likes->aColl[0] = db->pDfltColl;
  bs->ki_likes->aColl[1] = db->pDfltColl;
  bs->ki_posts_idx->aColl[0] = db->pDfltColl;
  bs->ki_posts_idx->aColl[1] = db->pDfltColl;

  size_t csz = (size_t)sqlite3BtreeCursorSize();
  bs->cur_posts = calloc(1, csz);
  bs->cur_users = calloc(1, csz);
  bs->cur_likes = calloc(1, csz);
  bs->cur_posts_idx = calloc(1, csz);
  if (!bs->cur_posts || !bs->cur_users || !bs->cur_likes || !bs->cur_posts_idx) return 0;
  for (int k = 0; k < FEED_SLOTS; k++) {
    bs->cur_fpost[k] = calloc(1, csz);
    bs->cur_fuser[k] = calloc(1, csz);
    if (!bs->cur_fpost[k] || !bs->cur_fuser[k]) return 0;
  }
  bs->ready = 1;
  return 1;
}

/* Called with a transaction open on the B-tree. */
static int bt_open(struct btree_state *bs) {
  if (!bs->ready) return -1;
  sqlite3BtreeCursorZero(bs->cur_posts);
  sqlite3BtreeCursorZero(bs->cur_users);
  sqlite3BtreeCursorZero(bs->cur_likes);
  sqlite3BtreeCursorZero(bs->cur_posts_idx);
  int rc = sqlite3BtreeCursor(bs->pBt, bs->root_posts, 0, NULL, bs->cur_posts);
  if (rc == SQLITE_OK) rc = sqlite3BtreeCursor(bs->pBt, bs->root_users, 0, NULL, bs->cur_users);
  if (rc == SQLITE_OK) rc = sqlite3BtreeCursor(bs->pBt, bs->root_likes_idx, 0, bs->ki_likes, bs->cur_likes);
  if (rc == SQLITE_OK) rc = sqlite3BtreeCursor(bs->pBt, bs->root_posts_idx, 0, bs->ki_posts_idx, bs->cur_posts_idx);
  bs->open = 1; /* closing a cursor that was only zeroed is harmless */
  bs->fopen = 0;
  return rc == SQLITE_OK ? 0 : -1;
}

static int bt_slot_open(struct btree_state *bs, int k) {
  sqlite3BtreeCursorZero(bs->cur_fpost[k]);
  sqlite3BtreeCursorZero(bs->cur_fuser[k]);
  bs->fopen |= 1u << k;
  if (sqlite3BtreeCursor(bs->pBt, bs->root_posts, 0, NULL, bs->cur_fpost[k]) != SQLITE_OK) return -1;
  if (sqlite3BtreeCursor(bs->pBt, bs->root_users, 0, NULL, bs->cur_fuser[k]) != SQLITE_OK) return -1;
  return 0;
}

/* closed before the writes of a pass */
static void bt_feed_close(struct btree_state *bs) {
  for (uint32_t m = bs->fopen; m; m &= m - 1) {
    int k = __builtin_ctz(m);
    sqlite3BtreeCloseCursor(bs->cur_fpost[k]);
    sqlite3BtreeCloseCursor(bs->cur_fuser[k]);
  }
  bs->fopen = 0;
}

static void bt_close(struct btree_state *bs) {
  if (!bs->open) return;
  bt_feed_close(bs);
  sqlite3BtreeCloseCursor(bs->cur_posts);
  sqlite3BtreeCloseCursor(bs->cur_users);
  sqlite3BtreeCloseCursor(bs->cur_likes);
  sqlite3BtreeCloseCursor(bs->cur_posts_idx);
  bs->open = 0;
}

struct bt_col {
  uint64_t t;       /* serial type */
  const uint8_t *p; /* value bytes */
  uint32_t n;
};

static inline const uint8_t *bt_varint(const uint8_t *p, const uint8_t *e, uint64_t *v) {
  if (p < e && !(*p & 0x80)) {
    *v = *p;
    return p + 1;
  }
  uint64_t x = 0;
  for (int i = 0; i < 8; i++) {
    if (p >= e) return NULL;
    uint8_t b = *p++;
    x = (x << 7) | (b & 0x7f);
    if (!(b & 0x80)) {
      *v = x;
      return p;
    }
  }
  if (p >= e) return NULL;
  *v = (x << 8) | *p++;
  return p;
}

static inline uint32_t bt_serial_len(uint64_t t) {
  static const uint8_t fixed[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? fixed[t] : (uint32_t)((t - 12) / 2);
}

/* Splits the first nc columns of a record. */
static int bt_cols(const uint8_t *rec, uint32_t n, struct bt_col *c, int nc) {
  const uint8_t *end = rec + n;
  uint64_t hsz;
  const uint8_t *h = bt_varint(rec, end, &hsz);
  if (!h || hsz > n) return -1;
  const uint8_t *hend = rec + hsz;
  const uint8_t *val = hend;
  for (int i = 0; i < nc; i++) {
    if (h >= hend) return -1;
    h = bt_varint(h, hend, &c[i].t);
    if (!h) return -1;
    uint32_t len = bt_serial_len(c[i].t);
    if ((size_t)(end - val) < len) return -1;
    c[i].p = val;
    c[i].n = len;
    val += len;
  }
  return 0;
}

static inline int bt_is_int(const struct bt_col *c) { return (c->t >= 1 && c->t <= 6) || c->t == 8 || c->t == 9; }
static inline int bt_is_text(const struct bt_col *c) { return c->t >= 13 && (c->t & 1); }

static inline int64_t bt_int(const struct bt_col *c) {
  if (c->t == 8) return 0;
  if (c->t == 9) return 1;
  uint64_t v = (c->p[0] & 0x80) ? ~(uint64_t)0 : 0; /* sign extension */
  for (uint32_t i = 0; i < c->n; i++) v = (v << 8) | c->p[i];
  return (int64_t)v;
}

static const uint8_t *bt_payload(BtCursor *cur, uint32_t *n, struct bt_spill *sp) {
  uint32_t total = sqlite3BtreePayloadSize(cur);
  u32 local = 0;
  const uint8_t *p = (const uint8_t *)sqlite3BtreePayloadFetch(cur, &local);
  if (p && local >= total) {
    *n = total;
    return p;
  }
  if (!sp) return NULL;
  if (total > sp->cap) {
    uint8_t *m = realloc(sp->buf, (size_t)total + 256);
    if (!m) return NULL;
    sp->buf = m;
    sp->cap = total + 256;
  }
  if (sqlite3BtreePayload(cur, 0, total, sp->buf) != SQLITE_OK) return NULL;
  *n = total;
  return sp->buf;
}

static int bt_likes_seek(struct btree_state *bs, int64_t key) {
  Mem m;
  UnpackedRecord r;
  memset(&m, 0, sizeof m);
  memset(&r, 0, sizeof r);
  m.flags = MEM_Int;
  m.u.i = key;
  r.pKeyInfo = bs->ki_likes;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = 1;
  int res = 0;
  if (sqlite3BtreeIndexMoveto(bs->cur_likes, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(bs->cur_likes)) return 1;
  if (res < 0) {
    int rc = sqlite3BtreeNext(bs->cur_likes, 0);
    if (rc == SQLITE_DONE) return 1;
    if (rc != SQLITE_OK) return -1;
  }
  return 0;
}

static int bt_likes_range(struct btree_state *bs, int64_t lo, int64_t hi, uint32_t *cnt) {
  memset(cnt, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
  int st = bt_likes_seek(bs, lo);
  if (st < 0) return -1;
  while (st == 0) {
    uint32_t n;
    const uint8_t *p = bt_payload(bs->cur_likes, &n, NULL);
    struct bt_col c[1];
    if (!p || bt_cols(p, n, c, 1) || !bt_is_int(&c[0])) return -1;
    int64_t pid = bt_int(&c[0]);
    if (pid > hi) break;
    if (pid >= lo) cnt[pid - lo]++;
    int rc = sqlite3BtreeNext(bs->cur_likes, 0);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_OK) return -1;
  }
  return 0;
}

struct bt_post {
  int64_t id, uid;
  const uint8_t *body, *stamp, *name;
  uint32_t body_len, stamp_len, name_len;
  uint32_t likes;
};

static int bt_post_at(BtCursor *cur, struct bt_spill *sp, int64_t id, struct bt_post *o) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur, id, 0, &res) != SQLITE_OK) return -1;
  if (res != 0) return 0;
  uint32_t n;
  const uint8_t *p = bt_payload(cur, &n, sp);
  struct bt_col c[4]; /* id (NULL: it is the rowid), user_id, body, created_at */
  if (!p || bt_cols(p, n, c, 4) || !bt_is_int(&c[1]) || !bt_is_text(&c[2]) || !bt_is_text(&c[3])) return -1;
  o->id = id;
  o->uid = bt_int(&c[1]);
  o->body = c[2].p;
  o->body_len = c[2].n;
  o->stamp = c[3].p;
  o->stamp_len = c[3].n;
  return 1;
}

static int bt_user_at(BtCursor *cur, struct bt_spill *sp, struct bt_post *o) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur, o->uid, 0, &res) != SQLITE_OK || res != 0) return -1;
  uint32_t n;
  const uint8_t *p = bt_payload(cur, &n, sp);
  struct bt_col c[2]; /* id (NULL), username */
  if (!p || bt_cols(p, n, c, 2) || !bt_is_text(&c[1])) return -1;
  o->name = c[1].p;
  o->name_len = c[1].n;
  return 0;
}

static int bt_post_seek(struct btree_state *bs, int64_t id, struct bt_post *o) {
  return bt_post_at(bs->cur_posts, &bs->spill_post, id, o);
}

static int bt_user_seek(struct btree_state *bs, struct bt_post *o) {
  return bt_user_at(bs->cur_users, &bs->spill_user, o);
}

/* 1 = exists, 0 = missing, -1 = error. */
static int bt_post_exists(struct btree_state *bs, int64_t id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(bs->cur_posts, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0;
}

static int bt_get_post(struct btree_state *bs, int64_t id, struct bt_post *o) {
  if (!bs->open) return -1;
  int rc = bt_post_seek(bs, id, o);
  if (rc <= 0) return rc;
  if (bt_user_seek(bs, o)) return -1;
  uint32_t cnt;
  if (bt_likes_range(bs, id, id, &cnt)) return -1;
  o->likes = cnt;
  return 1;
}

struct bt_feed {
  int n;
  struct bt_post row[FEED_N];
  uint8_t text[FEED_N * (ROW_BODY_MAX + ROW_NAME_MAX + ROW_STAMP_MAX)];
};

static int bt_feed(struct btree_state *bs, struct bt_feed *f) {
  if (!bs->open) return -1;
  f->n = 0;
  int empty = 0;
  if (sqlite3BtreeFirst(bs->cur_posts_idx, &empty) != SQLITE_OK) return -1;
  if (empty) return 0;

  int64_t ids[FEED_N];
  int n = 0;
  for (;;) {
    uint32_t len;
    const uint8_t *p = bt_payload(bs->cur_posts_idx, &len, NULL);
    struct bt_col c[2];
    if (!p || bt_cols(p, len, c, 2) || !bt_is_text(&c[0]) || !bt_is_int(&c[1])) return -1;
    ids[n++] = bt_int(&c[1]);
    if (n == FEED_N) break;
    int rc = sqlite3BtreeNext(bs->cur_posts_idx, 0);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_OK) return -1;
  }

  int64_t lo = ids[0], hi = ids[0];
  for (int i = 1; i < n; i++) {
    if (ids[i] < lo) lo = ids[i];
    if (ids[i] > hi) hi = ids[i];
  }

  uint32_t cnt[LIKES_SPAN];
  int ranged = hi - lo < LIKES_SPAN;
  if (ranged && bt_likes_range(bs, lo, hi, cnt)) return -1;

  /* a cursor pair per post, kept for the transaction */
  uint8_t *w = f->text;
  uint32_t used = 0;
  for (int i = 0; i < n; i++) {
    struct bt_post *o = &f->row[i];
    int k = (int)(ids[i] & (FEED_SLOTS - 1));
    uint32_t bit = 1u << k;
    if (!(used & bit)) {
      used |= bit;
      if (!(bs->fopen & bit) && bt_slot_open(bs, k)) return -1;
      if (bt_post_at(bs->cur_fpost[k], NULL, ids[i], o) == 1 && bt_user_at(bs->cur_fuser[k], NULL, o) == 0) {
        o->likes = 0;
        goto likes;
      }
    }
    /* slot taken by another id: shared cursors, text copied */
    if (bt_post_seek(bs, ids[i], o) != 1) return -1;
    if (o->body_len > ROW_BODY_MAX || o->stamp_len > ROW_STAMP_MAX) return -1;
    memcpy(w, o->body, o->body_len);
    o->body = w;
    w += o->body_len;
    memcpy(w, o->stamp, o->stamp_len);
    o->stamp = w;
    w += o->stamp_len;
    if (bt_user_seek(bs, o) || o->name_len > ROW_NAME_MAX) return -1;
    memcpy(w, o->name, o->name_len);
    o->name = w;
    w += o->name_len;
  likes:
    if (ranged) {
      o->likes = cnt[o->id - lo];
    } else {
      uint32_t one;
      if (bt_likes_range(bs, o->id, o->id, &one)) return -1;
      o->likes = one;
    }
  }
  f->n = n;
  return 0;
}

#endif
