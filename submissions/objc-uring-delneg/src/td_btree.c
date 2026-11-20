/* The stock SQLite 3.54.1 amalgamation (unmodified), plus the read paths of
** GET /feed and GET /posts/:id on SQLite's own B-tree layer: the pager, WAL,
** locking and page cache are SQLite's, only the bytecode engine is skipped.
** Reads run inside the batch's transaction (BEGIN ... COMMIT around each
** completion batch). Nothing is kept between calls but schema facts (root
** pages, key descriptions), checked against the schema cookie on every call.
** Any row the fast path does not cover (overflow pages, unexpected types)
** sends the caller back to its SQL statements. */
#include "sqlite3.c"
#include "td_btree.h"

_Static_assert(SQLITE_VERSION_NUMBER == 3054001, "check td_btree.c against this SQLite version");

static struct {
    sqlite3 *db;
    Pgno posts, users, top, likes;   /* root pages: posts, users, posts_created_at_id_idx, likes_post_id_idx */
    u32 cookie;
    KeyInfo *topKey, *likesKey;
} B;

int td_bt_init(sqlite3 *db) {
    memset(&B, 0, sizeof B);
    if (ENC(db) != SQLITE_UTF8) return 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name, rootpage FROM sqlite_schema WHERE name IN "
                               "('posts','users','posts_created_at_id_idx','likes_post_id_idx')", -1, &s, NULL)) return 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(s, 0);
        Pgno r = (Pgno)sqlite3_column_int64(s, 1);
        if (!strcmp(n, "posts")) B.posts = r;
        else if (!strcmp(n, "users")) B.users = r;
        else if (!strcmp(n, "posts_created_at_id_idx")) B.top = r;
        else B.likes = r;
    }
    sqlite3_finalize(s);
    if (!B.posts || !B.users || !B.top || !B.likes) return 0;
    B.cookie = (u32)db->aDb[0].pSchema->schema_cookie;
    B.topKey = sqlite3KeyInfoAlloc(db, 2, 1);
    B.likesKey = sqlite3KeyInfoAlloc(db, 1, 1);
    if (!B.topKey || !B.likesKey) return 0;
    B.db = db;
    return 1;
}

/* ---- records (https://sqlite.org/fileformat2.html#record_format) */
typedef struct { const u8 *p; u32 len; u32 type; i64 v; } Col;

/* Decodes the first n columns of the cursor's record, which must lie wholly on
** its page. Returns 0 if it does not (overflow) or is malformed. */
static int rec(BtCursor *c, int n, Col *col) {
    u32 size = sqlite3BtreePayloadSize(c), amt = 0;
    const u8 *p = sqlite3BtreePayloadFetch(c, &amt);
    if (amt < size || size < 1) return 0;
    u32 hdr;
    u32 h = getVarint32(p, hdr);
    if (hdr > size || hdr < h) return 0;
    const u8 *d = p + hdr, *end = p + size;
    for (int i = 0; i < n; i++) {
        if (h >= hdr) return 0;
        u32 t;
        h += getVarint32(p + h, t);
        u32 len;
        i64 v = 0;
        if (t >= 12) len = (t - 12) / 2;
        else {
            static const u8 sz[10] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0};
            if (t == 10 || t == 11) return 0;
            len = sz[t];
            if (t == 9) v = 1;
            else if (t >= 1 && t <= 6) {
                if (d + len > end) return 0;
                v = (signed char)d[0];
                for (u32 k = 1; k < len; k++) v = (v << 8) | d[k];
            }
        }
        if (d + len > end) return 0;
        col[i] = (Col){d, len, t, v};
        d += len;
    }
    return 1;
}
#define IS_INT(c) (((c).type >= 1 && (c).type <= 6) || (c).type == 8 || (c).type == 9)
#define IS_TEXT(c) ((c).type >= 13 && ((c).type & 1))

static int open_cur(BtCursor *c, Pgno root, KeyInfo *k) {
    memset(c, 0, sizeof *c);
    sqlite3BtreeCursorZero(c);
    return sqlite3BtreeCursor(B.db->aDb[0].pBt, root, 0, k, c);
}

/* First likes_post_id_idx entry with post_id >= id. 0: on an entry, 1: none, -1: error. */
static int likes_seek(BtCursor *c, i64 id) {
    Mem m;
    memset(&m, 0, sizeof m);
    m.flags = MEM_Int;
    m.u.i = id;
    UnpackedRecord key;
    memset(&key, 0, sizeof key);
    key.pKeyInfo = B.likesKey;
    key.aMem = &m;
    key.nField = 1;
    key.default_rc = 1;     /* entries equal on post_id sort after the key */
    int res = 0, rc = sqlite3BtreeIndexMoveto(c, &key, &res);
    if (rc) return -1;
    if (sqlite3BtreeEof(c)) return 1;
    if (res < 0) {
        rc = sqlite3BtreeNext(c, 0);
        if (rc == SQLITE_DONE) return 1;
        if (rc) return -1;
    }
    return 0;
}

/* Adds the likes of post ids lo..hi to cnt[id - lo]. 0 ok, -1 error. */
static int likes_count(BtCursor *c, i64 lo, i64 hi, i64 *cnt) {
    int r = likes_seek(c, lo);
    if (r) return r < 0 ? -1 : 0;
    for (;;) {
        Col k;
        if (!rec(c, 1, &k) || !IS_INT(k)) return -1;
        if (k.v > hi) return 0;
        if (k.v >= lo) cnt[k.v - lo]++;
        int rc = sqlite3BtreeNext(c, 0);
        if (rc == SQLITE_DONE) return 0;
        if (rc) return -1;
    }
}

/* Fills v from the posts row under pc and its author. 0 ok, -1: not covered. */
static int post_view(BtCursor *pc, BtCursor *uc, TDPostView *v) {
    Col p[4], u[2];
    if (!rec(pc, 4, p) || !IS_INT(p[1]) || !IS_TEXT(p[2]) || !IS_TEXT(p[3])) return -1;
    int res = 0;
    if (sqlite3BtreeTableMoveto(uc, p[1].v, 0, &res) || res != 0) return -1;
    if (!rec(uc, 2, u) || !IS_TEXT(u[1])) return -1;
    v->body = (const char *)p[2].p; v->bodyLen = p[2].len;
    v->ca = (const char *)p[3].p; v->caLen = p[3].len;
    v->au = (const char *)u[1].p; v->auLen = u[1].len;
    return 0;
}

static int begin_read(void) {
    sqlite3 *db = B.db;
    if (!db || sqlite3_get_autocommit(db)) return -1;   /* only inside the batch's BEGIN */
    Btree *bt = db->aDb[0].pBt;
    if (sqlite3BtreeBeginTrans(bt, 0, 0)) return -1;
    u32 cookie = 0;
    sqlite3BtreeGetMeta(bt, BTREE_SCHEMA_VERSION, &cookie);
    return cookie == B.cookie ? 0 : -1;
}

#define SPAN 256

int td_bt_feed(TDPostSink sink, void *ctx) {
    if (begin_read()) return -1;
    BtCursor top, pc, uc, lc;
    i64 ids[20];
    int n = 0, res = 0, ok = -1;
    if (open_cur(&top, B.top, B.topKey)) return -1;
    int rc = sqlite3BtreeFirst(&top, &res);     /* the index is (created_at DESC, id DESC): newest first */
    while (rc == SQLITE_OK && !res && n < 20) {
        Col k[2];
        if (!rec(&top, 2, k) || !IS_INT(k[1])) { rc = SQLITE_CORRUPT; break; }
        ids[n++] = k[1].v;
        rc = sqlite3BtreeNext(&top, 0);
        if (rc == SQLITE_DONE) { rc = SQLITE_OK; break; }
    }
    sqlite3BtreeCloseCursor(&top);
    if (rc != SQLITE_OK) return -1;
    if (n == 0) return 0;
    i64 lo = ids[0], hi = ids[0];
    for (int i = 1; i < n; i++) { if (ids[i] < lo) lo = ids[i]; if (ids[i] > hi) hi = ids[i]; }
    i64 cnt[SPAN], likes[20];
    if (open_cur(&lc, B.likes, B.likesKey)) return -1;
    if (hi - lo < SPAN) {
        memset(cnt, 0, sizeof(i64) * (size_t)(hi - lo + 1));
        rc = likes_count(&lc, lo, hi, cnt);
        for (int i = 0; i < n; i++) likes[i] = cnt[ids[i] - lo];
    } else {
        rc = 0;
        for (int i = 0; i < n && !rc; i++) { likes[i] = 0; rc = likes_count(&lc, ids[i], ids[i], &likes[i]); }
    }
    sqlite3BtreeCloseCursor(&lc);
    if (rc) return -1;
    if (open_cur(&pc, B.posts, NULL)) return -1;
    if (open_cur(&uc, B.users, NULL)) { sqlite3BtreeCloseCursor(&pc); return -1; }
    TDPostView v[20];
    int i = 0;
    for (; i < n; i++) {
        /* consecutive ids: step back one row instead of seeking from the root */
        if (!(i > 0 && ids[i] == ids[i - 1] - 1 && sqlite3BtreePrevious(&pc, 0) == SQLITE_OK
              && sqlite3BtreeIntegerKey(&pc) == ids[i])) {
            if (sqlite3BtreeTableMoveto(&pc, ids[i], 0, &res)) break;
            if (res != 0) continue;     /* not in the table: same as the SQL join */
        }
        if (post_view(&pc, &uc, &v[0])) break;
        v[0].id = ids[i];
        v[0].likes = likes[i];
        sink(ctx, &v[0]);
    }
    if (i == n) ok = n;
    sqlite3BtreeCloseCursor(&uc);
    sqlite3BtreeCloseCursor(&pc);
    return ok;
}

int td_bt_post(int64_t id, TDPostSink sink, void *ctx) {
    if (begin_read()) return -1;
    BtCursor pc, uc, lc;
    int res = 0, ok = -1;
    if (open_cur(&pc, B.posts, NULL)) return -1;
    if (open_cur(&uc, B.users, NULL)) { sqlite3BtreeCloseCursor(&pc); return -1; }
    TDPostView v;
    if (sqlite3BtreeTableMoveto(&pc, id, 0, &res) == SQLITE_OK) {
        if (res != 0) ok = 0;
        else if (post_view(&pc, &uc, &v) == 0) {
            i64 c = 0;
            if (open_cur(&lc, B.likes, B.likesKey) == SQLITE_OK) {
                if (likes_count(&lc, id, id, &c) == 0) {
                    v.id = id;
                    v.likes = c;
                    sink(ctx, &v);
                    ok = 1;
                }
                sqlite3BtreeCloseCursor(&lc);
            }
        }
    }
    sqlite3BtreeCloseCursor(&uc);
    sqlite3BtreeCloseCursor(&pc);
    return ok;
}
