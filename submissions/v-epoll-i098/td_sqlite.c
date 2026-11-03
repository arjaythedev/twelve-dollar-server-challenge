/* SQLite amalgamation plus td_feed_read: the feed's read path straight on the B-tree layer.
** It reads the same tables as the SQL it replaces, in the caller's read transaction, and keeps
** no state between calls except root page numbers and key descriptions (schema facts). */
#include "build/sqlite3.c"
#include "td_feed.h"

static struct {
  sqlite3 *db;
  int posts_root, users_root, top_root, likes_root;
  KeyInfo *likes_ki, *top_ki;
  UnpackedRecord *likes_key;
  void *cur[4];
} F;

static int root_of(sqlite3 *db, const char *name) {
  sqlite3_stmt *st;
  int root = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?", -1, &st, 0)) return 0;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW) root = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return root;
}

int td_feed_init(sqlite3 *db) {
  memset(&F, 0, sizeof F);
  F.db = db;
  F.posts_root = root_of(db, "posts");
  F.users_root = root_of(db, "users");
  F.top_root = root_of(db, "posts_created_at_id_idx");
  F.likes_root = root_of(db, "likes_post_id_idx");
  if (!F.posts_root || !F.users_root || !F.top_root || !F.likes_root) return 0;
  F.top_ki = sqlite3KeyInfoAlloc(db, 2, 0);
  F.likes_ki = sqlite3KeyInfoAlloc(db, 2, 0);
  if (!F.top_ki || !F.likes_ki) return 0;
  F.likes_key = sqlite3VdbeAllocUnpackedRecord(F.likes_ki);
  if (!F.likes_key) return 0;
  for (int i = 0; i < 4; i++) {
    F.cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!F.cur[i]) return 0;
  }
  return 1;
}

/* Decoded record: pointers into the cell payload (valid until the cursor moves). */
typedef struct { const unsigned char *col[4]; i64 val[4]; int len[4]; } Rec;

/* Decode up to n columns of a record held entirely in the page. Returns 0 on overflow/corruption. */
static int rec_parse(BtCursor *c, int n, Rec *r) {
  u32 amt, size = sqlite3BtreePayloadSize(c);   /* size first: it parses the cell for Fetch */
  const unsigned char *p = sqlite3BtreePayloadFetch(c, &amt);
  if (amt != size || amt < 2) return 0;
  u32 hdr; int h = getVarint32(p, hdr);
  if (hdr > amt) return 0;
  const unsigned char *data = p + hdr;
  const unsigned char *end = p + amt;
  for (int i = 0; i < n; i++) {
    u32 t;
    if (h >= (int)hdr) return 0;
    h += getVarint32(p + h, t);
    int len; i64 v = 0;
    if (t < 12) {
      static const unsigned char sz[] = {0, 1, 2, 3, 4, 6, 8, 0, 0, 0, 0, 0};
      len = sz[t];
      if (t == 8) v = 0; else if (t == 9) v = 1;
      else if (t >= 10) return 0;
      else if (len) {
        if (data + len > end) return 0;
        v = (signed char)data[0];
        for (int k = 1; k < len; k++) v = (v << 8) | data[k];
      }
    } else {
      len = (t - 12) / 2;
    }
    if (data + len > end) return 0;
    r->col[i] = data; r->val[i] = v; r->len[i] = len;
    data += len;
  }
  return 1;
}

static td_feed OUT;
const td_feed *td_feed_out(void) { return &OUT; }

/* Counts likes per post_id in [lo, hi] into cnt[post_id - lo]. Returns 0 on success. */
static int likes_range(Btree *bt, i64 lo, i64 hi, i64 *cnt) {
  BtCursor *c = F.cur[3];
  UnpackedRecord *key = F.likes_key;
  int res, rc, bad = 0;
  sqlite3BtreeCursorZero(c);
  if (sqlite3BtreeCursor(bt, F.likes_root, 0, F.likes_ki, c)) return 1;
  sqlite3VdbeMemInit(&key->aMem[0], F.db, MEM_Null);
  sqlite3VdbeMemSetInt64(&key->aMem[0], lo);
  key->nField = 1; key->default_rc = 1; key->errCode = 0; key->r1 = 0; key->r2 = 0; key->eqSeen = 0;
  if (sqlite3BtreeIndexMoveto(c, key, &res)) bad = 1;
  else if (res < 0 && (rc = sqlite3BtreeNext(c, 0)) && rc != SQLITE_DONE) bad = 1;
  while (!bad && !sqlite3BtreeEof(c)) {
    Rec r;
    if (!rec_parse(c, 1, &r)) { bad = 1; break; }
    if (r.val[0] > hi) break;
    if (r.val[0] >= lo) cnt[r.val[0] - lo]++;
    rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) break;
    if (rc) bad = 1;
  }
  sqlite3BtreeCloseCursor(c);
  return bad;
}

/* Copies the post under `posts` and its author into o and the arena. Returns 0 when the row
** does not fit the fast path (overflow pages, odd created_at, arena full). */
static int emit_row(BtCursor *posts, BtCursor *users, td_feed_row *o, unsigned char **ap, unsigned char *aend) {
  Rec r, u;
  int res;
  unsigned char *p = *ap;
  if (!rec_parse(posts, 4, &r) || r.len[3] != 24) return 0;        /* user_id, body, created_at */
  if (sqlite3BtreeTableMoveto(users, r.val[1], 0, &res) || res != 0) return 0;
  if (!rec_parse(users, 2, &u)) return 0;                          /* username */
  if (p + r.len[2] + 24 + u.len[1] > aend) return 0;
  o->body = p; o->body_len = r.len[2]; memcpy(p, r.col[2], r.len[2]); p += r.len[2];
  o->created = p; memcpy(p, r.col[3], 24); p += 24;
  o->user = p; o->user_len = u.len[1]; memcpy(p, u.col[1], u.len[1]); p += u.len[1];
  *ap = p;
  return 1;
}

static int open_post_cursors(Btree *bt) {
  BtCursor *posts = F.cur[1], *users = F.cur[2];
  sqlite3BtreeCursorZero(posts);
  if (sqlite3BtreeCursor(bt, F.posts_root, 0, 0, posts)) return 0;
  sqlite3BtreeCursorZero(users);
  if (sqlite3BtreeCursor(bt, F.users_root, 0, 0, users)) { sqlite3BtreeCloseCursor(posts); return 0; }
  return 1;
}

/* Reads the newest 20 posts (created_at DESC, id DESC) with author and like count.
** Returns the row count, or -1 when the caller must use the SQL path. */
int td_feed_read(int span) {
  Btree *bt = F.db->aDb[0].pBt;
  int n = 0, res, rc, ok = 0;
  i64 ids[20], lo, hi, cnt[256];
  unsigned char *ap = OUT.arena, *aend = OUT.arena + sizeof OUT.arena;
  BtCursor *top = F.cur[0], *posts = F.cur[1], *users = F.cur[2];

  if (span > 256 || sqlite3BtreeBeginTrans(bt, 0, 0)) return -1;
  sqlite3BtreeCursorZero(top);
  if (sqlite3BtreeCursor(bt, F.top_root, 0, F.top_ki, top)) return -1;
  rc = sqlite3BtreeFirst(top, &res);
  while (rc == SQLITE_OK && !res && n < 20) {
    Rec r;
    if (!rec_parse(top, 2, &r)) goto top_done;   /* created_at, id */
    ids[n++] = r.val[1];
    rc = sqlite3BtreeNext(top, 0);
  }
  if (rc != SQLITE_OK && rc != SQLITE_DONE) n = 0;
top_done:
  sqlite3BtreeCloseCursor(top);
  if (n == 0) return -1;
  lo = hi = ids[0];
  for (int k = 1; k < n; k++) { if (ids[k] < lo) lo = ids[k]; if (ids[k] > hi) hi = ids[k]; }
  if (hi - lo >= span) return -1;
  memset(cnt, 0, sizeof(i64) * (hi - lo + 1));
  if (likes_range(bt, lo, hi, cnt)) return -1;
  if (!open_post_cursors(bt)) return -1;
  for (int k = 0; k < n; k++) {
    if (!(k > 0 && ids[k] == ids[k - 1] - 1 && sqlite3BtreePrevious(posts, 0) == SQLITE_OK
          && sqlite3BtreeIntegerKey(posts) == ids[k])) {
      if (sqlite3BtreeTableMoveto(posts, ids[k], 0, &res) || res != 0) goto done;
    }
    if (!emit_row(posts, users, &OUT.row[k], &ap, aend)) goto done;
    OUT.row[k].id = ids[k]; OUT.row[k].likes = cnt[ids[k] - lo];
  }
  ok = 1;
done:
  sqlite3BtreeCloseCursor(users);
  sqlite3BtreeCloseCursor(posts);
  return ok ? n : -1;
}

/* Reads one post by id into OUT.row[0]. Returns 1, or 0 when the SQL path must answer
** (unknown id, or a row the fast path does not cover). */
int td_post_read(long long id) {
  Btree *bt = F.db->aDb[0].pBt;
  int res, ok = 0;
  i64 cnt[1] = {0};
  unsigned char *ap = OUT.arena;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) || !open_post_cursors(bt)) return 0;
  if (!sqlite3BtreeTableMoveto(F.cur[1], id, 0, &res) && res == 0
      && emit_row(F.cur[1], F.cur[2], &OUT.row[0], &ap, OUT.arena + sizeof OUT.arena)) ok = 1;
  sqlite3BtreeCloseCursor(F.cur[2]);
  sqlite3BtreeCloseCursor(F.cur[1]);
  if (!ok || likes_range(bt, id, id, cnt)) return 0;
  OUT.row[0].id = id; OUT.row[0].likes = cnt[0];
  return 1;
}
