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
    /* Cursors stay open for the rest of the batch's transaction (td_bt_end closes
    ** them before COMMIT/ROLLBACK); a write in between makes SQLite save and
    ** reseek them, as for any open statement cursor. */
    BtCursor cur[4];
    u8 open[4];
} B;
enum { C_TOP, C_POSTS, C_USERS, C_LIKES };

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

static BtCursor *cur(int i) {
    static const u8 isIndex[4] = {1, 0, 0, 1};
    BtCursor *c = &B.cur[i];
    if (B.open[i]) return c;
    Pgno root = i == C_TOP ? B.top : i == C_POSTS ? B.posts : i == C_USERS ? B.users : B.likes;
    memset(c, 0, sizeof *c);
    sqlite3BtreeCursorZero(c);
    if (sqlite3BtreeCursor(B.db->aDb[0].pBt, root, 0,
                           isIndex[i] ? (i == C_TOP ? B.topKey : B.likesKey) : NULL, c)) return NULL;
    B.open[i] = 1;
    return c;
}

void td_bt_end(void) {
    for (int i = 0; i < 4; i++)
        if (B.open[i]) { sqlite3BtreeCloseCursor(&B.cur[i]); B.open[i] = 0; }
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
    BtCursor *top = cur(C_TOP), *pc = cur(C_POSTS), *uc = cur(C_USERS), *lc = cur(C_LIKES);
    if (!top || !pc || !uc || !lc) return -1;
    i64 ids[20];
    int n = 0, res = 0;
    int rc = sqlite3BtreeFirst(top, &res);     /* the index is (created_at DESC, id DESC): newest first */
    while (rc == SQLITE_OK && !res && n < 20) {
        Col k[2];
        if (!rec(top, 2, k) || !IS_INT(k[1])) return -1;
        ids[n++] = k[1].v;
        rc = sqlite3BtreeNext(top, 0);
        if (rc == SQLITE_DONE) { rc = SQLITE_OK; break; }
    }
    if (rc != SQLITE_OK) return -1;
    if (n == 0) return 0;
    i64 lo = ids[0], hi = ids[0];
    for (int i = 1; i < n; i++) { if (ids[i] < lo) lo = ids[i]; if (ids[i] > hi) hi = ids[i]; }
    i64 cnt[SPAN], likes[20];
    if (hi - lo < SPAN) {
        memset(cnt, 0, sizeof(i64) * (size_t)(hi - lo + 1));
        rc = likes_count(lc, lo, hi, cnt);
        for (int i = 0; i < n; i++) likes[i] = cnt[ids[i] - lo];
    } else {
        rc = 0;
        for (int i = 0; i < n && !rc; i++) { likes[i] = 0; rc = likes_count(lc, ids[i], ids[i], &likes[i]); }
    }
    if (rc) return -1;
    TDPostView v;
    for (int i = 0; i < n; i++) {
        int at = 0;
        if (i == 0 && ids[0] == hi) {
            /* the newest post is usually the table's last row: a rightmost descent, no search */
            at = sqlite3BtreeLast(pc, &res) == SQLITE_OK && !res && sqlite3BtreeIntegerKey(pc) == ids[0];
        } else if (i > 0 && ids[i] == ids[i - 1] - 1) {
            /* consecutive ids: step back one row instead of seeking from the root */
            at = sqlite3BtreePrevious(pc, 0) == SQLITE_OK && sqlite3BtreeIntegerKey(pc) == ids[i];
        }
        if (!at) {
            if (sqlite3BtreeTableMoveto(pc, ids[i], 0, &res)) return -1;
            if (res != 0) continue;     /* not in the table: same as the SQL join */
        }
        if (post_view(pc, uc, &v)) return -1;
        v.id = ids[i];
        v.likes = likes[i];
        sink(ctx, &v);
    }
    return n;
}

int td_bt_post(int64_t id, TDPostSink sink, void *ctx) {
    if (begin_read()) return -1;
    BtCursor *pc = cur(C_POSTS), *uc = cur(C_USERS), *lc = cur(C_LIKES);
    if (!pc || !uc || !lc) return -1;
    int res = 0;
    if (sqlite3BtreeTableMoveto(pc, id, 0, &res)) return -1;
    if (res != 0) return 0;
    TDPostView v;
    if (post_view(pc, uc, &v)) return -1;
    i64 c = 0;
    if (likes_count(lc, id, id, &c)) return -1;
    v.id = id;
    v.likes = c;
    sink(ctx, &v);
    return 1;
}

/* A page cache through SQLite's public SQLITE_CONFIG_PCACHE2 plugin API. The stock
** one keys its pages in a hash table, keeps an LRU list, and scans the whole table
** at the next commit whenever a B-tree split parked a page under the pending-byte
** page number (~30 times a second under load, ~350 us each with 25,000 pages).
** Here a key is an index into an array, unpinning writes one byte (eviction is a
** clock over all pages), and a truncate looks only at keys added above the last
** limit. Pages hold what SQLite puts in them, and SQLite decides what they mean. */
typedef struct TDPage TDPage;
struct TDPage {
    sqlite3_pcache_page pg;       /* first: SQLite gets this address back */
    TDPage *nextFree;
    u32 key;
    u8 pinned, ref, live;
};
typedef struct {
    int szPage, szExtra, szAlloc, purgeable;
    u32 nMax, nPage;
    TDPage **tab;                 /* by key */
    u32 tabCap, hi, maxKey;       /* every live key above hi is in above[] */
    u32 *above;
    u32 nAbove, capAbove;
    u8 aboveLost;
    TDPage **all;                 /* every page ever carved: the clock's dial */
    u32 nAll, capAll, hand;
    TDPage *freeList;
    char *carve;
    u32 carveLeft;
    void **chunks;
    u32 nChunks, capChunks;
} TDCache;

static int tdp_fit(TDCache *c, u32 key) {
    if (key < c->tabCap) return 1;
    u32 cap = c->tabCap * 2;
    if (cap <= key) cap = key + 1;
    TDPage **t = calloc(cap, sizeof *t);
    if (!t) return 0;
    memcpy(t, c->tab, c->tabCap * sizeof *t);
    free(c->tab);
    c->tab = t;
    c->tabCap = cap;
    return 1;
}

static void tdp_note(TDCache *c, u32 key) {
    if (key > c->maxKey) c->maxKey = key;
    if (key <= c->hi) return;
    if (c->nAbove == c->capAbove) {
        u32 cap = c->capAbove ? c->capAbove * 2 : 64;
        u32 *a = realloc(c->above, cap * sizeof *a);
        if (!a) { c->aboveLost = 1; return; }
        c->above = a;
        c->capAbove = cap;
    }
    c->above[c->nAbove++] = key;
}

static void tdp_drop(TDCache *c, TDPage *p) {
    c->tab[p->key] = 0;
    p->live = 0;
    p->pinned = 0;
    p->nextFree = c->freeList;
    c->freeList = p;
    c->nPage--;
}

static TDPage *tdp_alloc(TDCache *c) {
    TDPage *p = c->freeList;
    if (p) { c->freeList = p->nextFree; return p; }
    if (c->nAll == c->capAll) {
        u32 cap = c->capAll ? c->capAll * 2 : 64;
        TDPage **a = realloc(c->all, cap * sizeof *a);
        if (!a) return 0;
        c->all = a;
        c->capAll = cap;
    }
    if (!c->carveLeft) {
        u32 n = c->purgeable ? 64 : 4;
        if (c->nChunks == c->capChunks) {
            u32 cap = c->capChunks ? c->capChunks * 2 : 16;
            void **a = realloc(c->chunks, cap * sizeof *a);
            if (!a) return 0;
            c->chunks = a;
            c->capChunks = cap;
        }
        char *m = malloc((size_t)c->szAlloc * n);
        if (!m) return 0;
        c->chunks[c->nChunks++] = m;
        c->carve = m;
        c->carveLeft = n;
    }
    char *m = c->carve;
    c->carve += c->szAlloc;
    c->carveLeft--;
    p = (TDPage *)(m + c->szPage + c->szExtra);
    p->pg.pBuf = m;
    p->pg.pExtra = m + c->szPage;
    p->live = 0;
    c->all[c->nAll++] = p;
    return p;
}

/* The clock: a page unpinned since the hand last passed gets another round. */
static TDPage *tdp_evict(TDCache *c) {
    for (u32 n = c->nAll * 2; n && c->nAll; n--) {
        TDPage *p = c->all[c->hand];
        if (++c->hand == c->nAll) c->hand = 0;
        if (!p->live || p->pinned) continue;
        if (p->ref) { p->ref = 0; continue; }
        c->tab[p->key] = 0;
        p->live = 0;
        c->nPage--;
        return p;
    }
    return 0;
}

static sqlite3_pcache *tdp_create(int szPage, int szExtra, int purgeable) {
    TDCache *c = calloc(1, sizeof *c);
    if (!c) return 0;
    c->szPage = szPage;
    c->szExtra = ROUND8(szExtra);
    c->szAlloc = szPage + c->szExtra + ROUND8(sizeof(TDPage));
    c->purgeable = purgeable;
    c->nMax = 100;
    c->tabCap = purgeable ? 1u << 19 : 1u << 8;   /* above the pending-byte page of 4 KB pages; calloc's zero pages stay untouched */
    c->tab = calloc(c->tabCap, sizeof *c->tab);
    if (!c->tab) { free(c); return 0; }
    return (sqlite3_pcache *)c;
}

static void tdp_cachesize(sqlite3_pcache *pc, int n) { ((TDCache *)pc)->nMax = (u32)n; }
static int tdp_pagecount(sqlite3_pcache *pc) { return (int)((TDCache *)pc)->nPage; }

static sqlite3_pcache_page *tdp_fetch(sqlite3_pcache *pc, unsigned key, int create) {
    TDCache *c = (TDCache *)pc;
    TDPage *p = key < c->tabCap ? c->tab[key] : 0;
    if (p) { p->pinned = 1; return &p->pg; }
    if (!create) return 0;
    if (!tdp_fit(c, key)) return 0;
    p = 0;
    if (c->purgeable && c->nPage >= c->nMax) {
        p = tdp_evict(c);
        if (!p && create == 1) return 0;     /* SQLite spills a dirty page, or asks again with 2 */
    }
    if (!p && !(p = tdp_alloc(c))) return 0;
    p->key = key;
    p->pinned = 1;
    p->ref = 0;
    p->live = 1;
    c->tab[key] = p;
    c->nPage++;
    *(void **)p->pg.pExtra = 0;
    tdp_note(c, key);
    return &p->pg;
}

static void tdp_unpin(sqlite3_pcache *pc, sqlite3_pcache_page *pg, int discard) {
    TDPage *p = (TDPage *)pg;
    p->pinned = 0;
    p->ref = 1;
    if (discard) tdp_drop((TDCache *)pc, p);
}

static void tdp_rekey(sqlite3_pcache *pc, sqlite3_pcache_page *pg, unsigned oldKey, unsigned newKey) {
    TDCache *c = (TDCache *)pc;
    TDPage *p = (TDPage *)pg;
    if (!tdp_fit(c, newKey)) { fprintf(stderr, "page cache: out of memory in rekey\n"); abort(); }   /* xRekey can't fail */
    if (c->tab[newKey] && c->tab[newKey] != p) tdp_drop(c, c->tab[newKey]);
    c->tab[oldKey] = 0;
    c->tab[newKey] = p;
    p->key = newKey;
    tdp_note(c, newKey);
}

static void tdp_truncate(sqlite3_pcache *pc, unsigned limit) {
    TDCache *c = (TDCache *)pc;
    if (limit <= c->hi || c->aboveLost) {
        for (u32 k = limit; k <= c->maxKey && k < c->tabCap; k++)
            if (c->tab[k]) tdp_drop(c, c->tab[k]);
        c->aboveLost = 0;
    } else {
        for (u32 i = 0; i < c->nAbove; i++) {
            u32 k = c->above[i];
            if (k >= limit && c->tab[k]) tdp_drop(c, c->tab[k]);
        }
    }
    c->nAbove = 0;
    c->hi = limit ? limit - 1 : 0;
    if (c->maxKey > c->hi) c->maxKey = c->hi;
}

static void tdp_destroy(sqlite3_pcache *pc) {
    TDCache *c = (TDCache *)pc;
    for (u32 i = 0; i < c->nChunks; i++) free(c->chunks[i]);
    free(c->chunks);
    free(c->all);
    free(c->above);
    free(c->tab);
    free(c);
}

static void tdp_shrink(sqlite3_pcache *pc) { (void)pc; }
static int tdp_init(void *a) { (void)a; return SQLITE_OK; }
static void tdp_shutdown(void *a) { (void)a; }

int td_pcache_install(void) {
    static const sqlite3_pcache_methods2 m = {
        1, 0, tdp_init, tdp_shutdown, tdp_create, tdp_cachesize, tdp_pagecount, tdp_fetch,
        tdp_unpin, tdp_rekey, tdp_truncate, tdp_destroy, tdp_shrink
    };
    return sqlite3_config(SQLITE_CONFIG_PCACHE2, &m);
}
