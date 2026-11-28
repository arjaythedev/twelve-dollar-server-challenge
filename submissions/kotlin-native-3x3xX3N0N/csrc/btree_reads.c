/* B-tree read paths for GET /feed and GET /posts/:id, through SQLite's own cursor API (allowed: organiser's ruling
 * of 2026-10-31). Included by db.c after sqlite3.c, so SQLite's internal functions are in scope. Read only: it never
 * writes a SQLite structure, it only opens cursors inside the reader connection's read transaction.
 *
 *   post  = posts row by rowid + users row by rowid + likes_post_id_idx entries counted from the first == id
 *   feed  = the first 20 entries of posts_created_at_id_idx (created_at DESC, id DESC: the newest come first), then
 *           the post path for each
 * The JSON is written exactly as SQLite's json_object() writes it (same escaping table, jsonIsOk[], and the same
 * control-character forms), and db_verify() checks every post id and the feed against the SQL path byte for byte.
 * Anything unexpected (a column type, an overflow page larger than the scratch buffer, a missing user) returns -1
 * and the caller answers from the SQL statement instead.
 * Ideas from public MIT entries (no code copied): the cursor-read approach and the schema checks at start follow
 * rust-iouring-kensac (csrc/shim.c) and c-iouring-pavlyhalim (src/btree.h). */

static Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
static KeyInfo *ki_feed, *ki_likes;
static UnpackedRecord *rec_likes;
static BtCursor *cur_posts, *cur_users, *cur_feed, *cur_likes;
static Btree *bt_main;
static uint8_t scratch_post[65536], scratch_user[4096], scratch_idx[4096];

static int bt_query_int(sqlite3 *db, const char *sql, int64_t *v) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) return -1;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) *v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

/* The comma-joined first column of every row, compared with want. */
static int bt_query_eq(sqlite3 *db, const char *sql, const char *want) {
    sqlite3_stmt *st;
    char got[256] = "";
    size_t n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        n += snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", t ? t : "NULL");
        if (n >= sizeof got) break;
    }
    sqlite3_finalize(st);
    return strcmp(got, want) == 0 ? 0 : -1;
}

static int bt_init(sqlite3 *db, char *why, int cap) {
    static const struct { const char *sql, *want; } shape[] = {
        {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
        {"SELECT pk FROM pragma_table_info('posts') WHERE name='id'", "1"},
        {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
        {"SELECT pk FROM pragma_table_info('users') WHERE name='id'", "1"},
        {"SELECT name FROM pragma_index_xinfo('posts_created_at_id_idx')", "created_at,id,NULL"},
        {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx')", "1,1,0"},
        {"SELECT coll FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "BINARY,BINARY"},
        {"SELECT name FROM pragma_index_xinfo('likes_post_id_idx')", "post_id,NULL"},
        {"SELECT encoding FROM pragma_encoding", "UTF-8"},
    };
    for (size_t i = 0; i < sizeof shape / sizeof shape[0]; i++)
        if (bt_query_eq(db, shape[i].sql, shape[i].want)) { snprintf(why, cap, "schema check failed: %s", shape[i].sql); return -1; }
    int64_t r[4];
    if (bt_query_int(db, "SELECT rootpage FROM sqlite_schema WHERE type='table' AND name='posts'", &r[0]) ||
        bt_query_int(db, "SELECT rootpage FROM sqlite_schema WHERE type='table' AND name='users'", &r[1]) ||
        bt_query_int(db, "SELECT rootpage FROM sqlite_schema WHERE type='index' AND name='posts_created_at_id_idx'", &r[2]) ||
        bt_query_int(db, "SELECT rootpage FROM sqlite_schema WHERE type='index' AND name='likes_post_id_idx'", &r[3])) {
        snprintf(why, cap, "root pages not found");
        return -1;
    }
    root_posts = (Pgno)r[0]; root_users = (Pgno)r[1]; root_feed_idx = (Pgno)r[2]; root_likes_idx = (Pgno)r[3];
    bt_main = db->aDb[0].pBt;
    ki_feed = sqlite3KeyInfoAlloc(db, 2, 1);        /* (created_at DESC, id DESC) + rowid */
    ki_likes = sqlite3KeyInfoAlloc(db, 1, 1);       /* (post_id) + rowid */
    if (!ki_feed || !ki_likes) { snprintf(why, cap, "out of memory"); return -1; }
    ki_feed->aSortFlags[0] = ki_feed->aSortFlags[1] = KEYINFO_ORDER_DESC;
    rec_likes = sqlite3VdbeAllocUnpackedRecord(ki_likes);
    int sz = sqlite3BtreeCursorSize();
    cur_posts = sqlite3_malloc(sz); cur_users = sqlite3_malloc(sz);
    cur_feed = sqlite3_malloc(sz); cur_likes = sqlite3_malloc(sz);
    if (!rec_likes || !cur_posts || !cur_users || !cur_feed || !cur_likes) { snprintf(why, cap, "out of memory"); return -1; }
    return 0;
}

static int bt_open(Pgno root, KeyInfo *ki, BtCursor *c) {
    sqlite3BtreeCursorZero(c);
    return sqlite3BtreeCursor(bt_main, root, 0, ki, c);
}

/* Called inside the reader's BEGIN: start the B-tree read transaction and open the four cursors. */
static int bt_begin(sqlite3 *db) {
    (void)db;
    if (sqlite3BtreeBeginTrans(bt_main, 0, 0) != SQLITE_OK) return -1;
    if (bt_open(root_posts, 0, cur_posts) || bt_open(root_users, 0, cur_users) ||
        bt_open(root_feed_idx, ki_feed, cur_feed) || bt_open(root_likes_idx, ki_likes, cur_likes)) return -1;
    return 0;
}

static void bt_end(void) {
    sqlite3BtreeCloseCursor(cur_posts);
    sqlite3BtreeCloseCursor(cur_users);
    sqlite3BtreeCloseCursor(cur_feed);
    sqlite3BtreeCloseCursor(cur_likes);
}

/* ------------------------------------------------------------------ record decoding */
static int rd_varint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
    uint64_t x = 0;
    for (int i = 0; i < 9; i++) {
        if (p + i >= end) return 0;
        if (i == 8) { *v = (x << 8) | p[i]; return 9; }
        x = (x << 7) | (p[i] & 0x7f);
        if (!(p[i] & 0x80)) { *v = x; return i + 1; }
    }
    return 0;
}

static uint32_t serial_len(uint64_t t) {
    static const uint8_t fixed[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
    return t < 12 ? fixed[t] : (uint32_t)((t - 12) / 2);
}

/* Column types and body offsets of the first ncol columns of a record. Returns 0, or -1 if malformed/short. */
static int rec_cols(const uint8_t *p, uint32_t n, int ncol, uint64_t *type, uint32_t *off) {
    const uint8_t *end = p + n;
    uint64_t hdr;
    int k = rd_varint(p, end, &hdr);
    if (!k || hdr > n) return -1;
    const uint8_t *h = p + k, *hend = p + hdr;
    uint32_t body = (uint32_t)hdr;
    for (int i = 0; i < ncol; i++) {
        if (h >= hend) return -1;
        k = rd_varint(h, hend, &type[i]);
        if (!k) return -1;
        h += k;
        off[i] = body;
        body += serial_len(type[i]);
        if (body > n) return -1;
    }
    return 0;
}

static int rec_int(const uint8_t *p, uint64_t t, int64_t *v) {
    switch (t) {
    case 1: *v = (int8_t)p[0]; return 0;
    case 2: *v = (int16_t)((p[0] << 8) | p[1]); return 0;
    case 3: *v = (int32_t)(((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8) ) >> 8; return 0;
    case 4: *v = (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]); return 0;
    case 5: case 6: {
        int nb = t == 5 ? 6 : 8;
        uint64_t x = (p[0] & 0x80) ? ~0ULL : 0;
        for (int i = 0; i < nb; i++) x = (x << 8) | p[i];
        *v = (int64_t)x;
        return 0;
    }
    case 8: *v = 0; return 0;
    case 9: *v = 1; return 0;
    default: return -1;
    }
}

/* Record payload of the cursor's current entry (from the page, or copied into scratch when it spills). */
static const uint8_t *payload(BtCursor *c, uint8_t *scratch, uint32_t cap, uint32_t *n) {
    uint32_t avail = 0;
    uint32_t sz = sqlite3BtreePayloadSize(c);                /* computes the cell info PayloadFetch reads */
    const uint8_t *p = sqlite3BtreePayloadFetch(c, &avail);
    if (avail >= sz) { *n = sz; return p; }
    if (sz > cap || sqlite3BtreePayload(c, 0, sz, scratch) != SQLITE_OK) return 0;
    *n = sz;
    return scratch;
}

/* ------------------------------------------------------------------ JSON output (as json_object() writes it) */
typedef struct { uint8_t *p; int n, cap; } out_t;

static int put(out_t *o, const char *s, int n) {
    if (o->n + n > o->cap) return -1;
    memcpy(o->p + o->n, s, n);
    o->n += n;
    return 0;
}

static int put_int(out_t *o, int64_t v) {
    char t[24];
    int k = 24;
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do { t[--k] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) t[--k] = '-';
    return put(o, t + k, 24 - k);
}

/* jsonAppendString() of SQLite's json.c, writing into o: same jsonIsOk[] table, `"` and `\` escaped, `'` kept,
 * other control bytes as \b \t \n \f \r or \u00XX. */
static int put_str(out_t *o, const uint8_t *z, uint32_t n) {
    static const char special[32] = {0, 0, 0, 0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f', 'r', 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 0, 0,   0,   0,   0, 0,   0,   0, 0};
    if (o->n + (int)n * 6 + 2 > o->cap) return -1;
    uint8_t *w = o->p + o->n;
    *w++ = '"';
    uint32_t i = 0;
    for (;;) {
        /* Skip 8 bytes at a time while none needs escaping (no byte < 0x20, '"', '\'' or '\\'; same set as
         * jsonIsOk[]), then copy the clean run in one memcpy. */
        uint32_t j = i;
        while (j + 8 <= n) {
            uint64_t x;
            memcpy(&x, z + j, 8);
            const uint64_t ones = 0x0101010101010101ULL, high = 0x8080808080808080ULL;
            uint64_t lt20 = (x - ones * 0x20) & ~x;
            uint64_t q = x ^ (ones * 0x22), a = x ^ (ones * 0x27), b = x ^ (ones * 0x5c);
            uint64_t hit = (lt20 | ((q - ones) & ~q) | ((a - ones) & ~a) | ((b - ones) & ~b)) & high;
            if (hit) break;
            j += 8;
        }
        while (j < n && jsonIsOk[z[j]]) j++;
        memcpy(w, z + i, j - i);
        w += j - i;
        if (j >= n) break;
        uint8_t c = z[j];
        if (c == '"' || c == '\\') { *w++ = '\\'; *w++ = c; }
        else if (c == '\'') *w++ = c;
        else if (special[c]) { *w++ = '\\'; *w++ = (uint8_t)special[c]; }
        else {
            *w++ = '\\'; *w++ = 'u'; *w++ = '0'; *w++ = '0';
            *w++ = (uint8_t)"0123456789abcdef"[c >> 4]; *w++ = (uint8_t)"0123456789abcdef"[c & 0xf];
        }
        i = j + 1;
    }
    *w++ = '"';
    o->n = (int)(w - o->p);
    return 0;
}

#define LIT(o, s) put(o, s, (int)sizeof(s) - 1)

/* Number of likes_post_id_idx entries with post_id == id, or -1. */
static int64_t bt_like_count(int64_t id) {
    int res = 0;
    UnpackedRecord *r = rec_likes;
    r->nField = 1;
    r->default_rc = +1;     /* as OP_SeekGE: land on the first entry >= (id) */
    r->eqSeen = 0;
    r->errCode = 0;
    r->aMem[0].flags = MEM_Int;
    r->aMem[0].u.i = id;
    if (sqlite3BtreeIndexMoveto(cur_likes, r, &res) != SQLITE_OK || r->errCode) return -1;
    if (sqlite3BtreeEof(cur_likes)) return 0;
    if (res < 0) {
        int rc = sqlite3BtreeNext(cur_likes, 0);
        if (rc == SQLITE_DONE) return 0;
        if (rc != SQLITE_OK) return -1;
    }
    int64_t n = 0;
    for (;;) {
        uint32_t len;
        const uint8_t *p = payload(cur_likes, scratch_idx, sizeof scratch_idx, &len);
        uint64_t t[1];
        uint32_t off[1];
        int64_t v;
        if (!p || rec_cols(p, len, 1, t, off) || rec_int(p + off[0], t[0], &v)) return -1;
        if (v != id) break;
        n++;
        int rc = sqlite3BtreeNext(cur_likes, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return -1;
    }
    return n;
}

/* The post object for id into o without its like count and closing brace: {"id":..,"body":..,"created_at":..,
 * "author":.. . Returns 1, 0 = no such post, -1 = use SQL. */
static int bt_post_head(int64_t id, out_t *o) {
    int res = 0;
    if (sqlite3BtreeTableMoveto(cur_posts, id, 0, &res) != SQLITE_OK) return -1;
    if (res != 0 || sqlite3BtreeEof(cur_posts)) return 0;
    uint32_t n;
    const uint8_t *p = payload(cur_posts, scratch_post, sizeof scratch_post, &n);
    uint64_t t[4];
    uint32_t off[4];
    int64_t uid;
    if (!p || rec_cols(p, n, 4, t, off)) return -1;
    if (t[0] != 0 || rec_int(p + off[1], t[1], &uid)) return -1;          /* id is the rowid; user_id an integer */
    if (t[2] < 13 || !(t[2] & 1) || t[3] < 13 || !(t[3] & 1)) return -1;  /* body, created_at: text */
    if (LIT(o, "{\"id\":") || put_int(o, id) ||
        LIT(o, ",\"body\":") || put_str(o, p + off[2], serial_len(t[2])) ||
        LIT(o, ",\"created_at\":") || put_str(o, p + off[3], serial_len(t[3]))) return -1;

    if (sqlite3BtreeTableMoveto(cur_users, uid, 0, &res) != SQLITE_OK) return -1;
    if (res != 0 || sqlite3BtreeEof(cur_users)) return -1;               /* JOIN finds nothing: SQL decides */
    uint32_t un;
    const uint8_t *u = payload(cur_users, scratch_user, sizeof scratch_user, &un);
    uint64_t ut[2];
    uint32_t uoff[2];
    if (!u || rec_cols(u, un, 2, ut, uoff) || ut[0] != 0 || ut[1] < 13 || !(ut[1] & 1)) return -1;
    if (LIT(o, ",\"author\":") || put_str(o, u + uoff[1], serial_len(ut[1]))) return -1;
    return 1;
}

static int put_likes(out_t *o, int64_t likes) {
    return LIT(o, ",\"like_count\":") || put_int(o, likes) || LIT(o, "}") ? -1 : 0;
}

/* {"post":{...}}: length, 0 = not found, -1 = use SQL. */
static int bt_post(int64_t id, uint8_t *buf, int cap) {
    out_t o = {buf, 0, cap};
    if (LIT(&o, "{\"post\":")) return -1;
    int r = bt_post_head(id, &o);
    if (r <= 0) return r;
    int64_t likes = bt_like_count(id);
    if (likes < 0 || put_likes(&o, likes) || LIT(&o, "}")) return -1;
    return o.n;
}

/* Like counts of every post id in [lo, hi] in one walk of likes_post_id_idx (cnt[id - lo]). 0, or -1. */
static int bt_like_range(int64_t lo, int64_t hi, int32_t *cnt) {
    int res = 0;
    UnpackedRecord *r = rec_likes;
    r->nField = 1; r->default_rc = +1; r->eqSeen = 0; r->errCode = 0;
    r->aMem[0].flags = MEM_Int; r->aMem[0].u.i = lo;
    if (sqlite3BtreeIndexMoveto(cur_likes, r, &res) != SQLITE_OK || r->errCode) return -1;
    if (sqlite3BtreeEof(cur_likes)) return 0;
    if (res < 0) {
        int rc = sqlite3BtreeNext(cur_likes, 0);
        if (rc == SQLITE_DONE) return 0;
        if (rc != SQLITE_OK) return -1;
    }
    for (;;) {
        uint32_t len;
        const uint8_t *p = payload(cur_likes, scratch_idx, sizeof scratch_idx, &len);
        uint64_t t[1];
        uint32_t off[1];
        int64_t v;
        if (!p || rec_cols(p, len, 1, t, off) || rec_int(p + off[0], t[0], &v)) return -1;
        if (v > hi) break;
        if (v >= lo) cnt[v - lo]++;
        int rc = sqlite3BtreeNext(cur_likes, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return -1;
    }
    return 0;
}

#define FEED_SPAN 512

/* {"posts":[...]}: length, or -1 = use SQL.
 * The 20 ids come from posts_created_at_id_idx (newest first). Their rows are then read in ascending id order, so
 * consecutive ids (the usual case: new posts get the next ids) step to the next cell instead of descending from the
 * root (SQLite's own TableMoveto fast path); each object (without its like count) goes to a scratch area. When the
 * ids span fewer than FEED_SPAN, one walk of likes_post_id_idx counts all their likes; otherwise one seek per post.
 * Idea credit: rust-iouring-kensac (ascending-id lookups), zig-iouring-unorig-username (one range walk of likes). */
static int bt_feed(uint8_t *buf, int cap) {
    static uint8_t heads[20 * 4096];
    static int32_t cnt[FEED_SPAN];
    int64_t ids[20];
    int hoff[20], hlen[20], order[20];
    int n = 0, empty = 0;
    if (sqlite3BtreeFirst(cur_feed, &empty) != SQLITE_OK) return -1;
    while (!empty && n < 20) {
        uint32_t len;
        const uint8_t *p = payload(cur_feed, scratch_idx, sizeof scratch_idx, &len);
        uint64_t t[3];
        uint32_t off[3];
        if (!p || rec_cols(p, len, 3, t, off) || rec_int(p + off[2], t[2], &ids[n])) return -1;  /* the rowid */
        n++;
        int rc = sqlite3BtreeNext(cur_feed, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return -1;
    }
    /* ascending id order (insertion sort of <= 20) */
    for (int i = 0; i < n; i++) {
        int j = i;
        while (j > 0 && ids[order[j - 1]] > ids[i]) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    out_t h = {heads, 0, (int)sizeof heads};
    for (int k = 0; k < n; k++) {
        int i = order[k];
        hoff[i] = h.n;
        if (bt_post_head(ids[i], &h) != 1) return -1;   /* a post without its row/user: SQL decides */
        hlen[i] = h.n - hoff[i];
    }
    int64_t lo = n ? ids[order[0]] : 0, hi = n ? ids[order[n - 1]] : 0;
    int ranged = n > 0 && hi - lo < FEED_SPAN;
    if (ranged) {
        memset(cnt, 0, sizeof(int32_t) * (size_t)(hi - lo + 1));
        if (bt_like_range(lo, hi, cnt)) return -1;
    }
    out_t o = {buf, 0, cap};
    if (LIT(&o, "{\"posts\":[")) return -1;
    for (int i = 0; i < n; i++) {
        if (i && LIT(&o, ",")) return -1;
        if (put(&o, (const char *)heads + hoff[i], hlen[i])) return -1;
        int64_t likes = ranged ? cnt[ids[i] - lo] : bt_like_count(ids[i]);
        if (likes < 0 || put_likes(&o, likes)) return -1;
    }
    if (LIT(&o, "]}")) return -1;
    return o.n;
}
