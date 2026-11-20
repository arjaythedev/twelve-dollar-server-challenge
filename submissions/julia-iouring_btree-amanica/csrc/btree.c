/* The pinned SQLite amalgamation, unmodified, plus a thin read layer over its own B-tree functions.
 *
 * GET /feed, GET /posts/:id and the post-exists check before a like read SQLite's B-trees through
 * sqlite3Btree* (BeginTrans, Cursor, TableMoveto, IndexMoveto, First, Next, PayloadFetch, Payload,
 * IntegerKey, CloseCursor), skipping the SQL bytecode engine. This file only calls those functions and
 * reads the bytes they return: it writes no field of SQLite's own structures (cursors, pages, the
 * connection). It builds its own inputs for IndexMoveto (a KeyInfo from sqlite3KeyInfoAlloc and an
 * UnpackedRecord), as SQLite's own callers do. Writes, BEGIN/COMMIT and /health stay on SQL.
 *
 * Everything here runs on the event-loop thread, inside the transaction the loop opened with BEGIN (see
 * src/db.jl): jr_bt_ensure() starts the B-tree read transaction (a WAL snapshot) and opens the cursors at
 * the first B-tree read of a transaction; jr_bt_close() closes them before the loop's COMMIT or ROLLBACK.
 * The amalgamation is compiled in this translation unit, so its SQLITE_PRIVATE (static) functions are
 * reachable; the public sqlite3_* API is exported from here as before. */
#include "sqlite3.c"
#include <stdint.h>

static sqlite3 *bdb;
static Btree *bt;
static Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
static KeyInfo *ki_feed, *ki_likes;

/* 0 posts, 1 users, 2 posts_created_at_id_idx, 3 likes_post_id_idx */
#define NCUR 4
static BtCursor *cur[NCUR];
static int cur_open;

/* Feed rows: text copied here (the posts cursor moves on to the next row before Julia writes the JSON).
 * Also holds a record that spills onto overflow pages (not expected with this schema: rows < 2.1 KB). */
#define ARENA (64 << 10)
#define SPAN 256            /* feed ids this close together: like counts from one index walk */
static u8 arena[ARENA];
static u32 arena_len;
static u8 spill[4096 + 2200];

typedef struct {            /* mirrored by JRow in src/db.jl */
    int64_t id, likes;
    const u8 *body, *ca, *au;
    int32_t blen, clen, alen, pad;
} jr_row;

static long bt_reads, bt_errors;

static Pgno rootpage(const char *name) {
    sqlite3_stmt *s;
    Pgno r = 0;
    if (sqlite3_prepare_v2(bdb, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, 0)) return 0;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return r;
}

/* The record decoders below assume this exact layout: refuse to start on anything else. */
static int layout(const char *sql, const char *want) {
    sqlite3_stmt *s;
    char got[512];
    size_t n = 0;
    got[0] = 0;
    if (sqlite3_prepare_v2(bdb, sql, -1, &s, 0)) return 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *c = (const char *)sqlite3_column_text(s, 0);
        n += snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
        if (n >= sizeof got) break;
    }
    sqlite3_finalize(s);
    return strcmp(got, want) == 0;
}

static KeyInfo *keyinfo(int nkey, int nextra, int desc) {
    KeyInfo *k = sqlite3KeyInfoAlloc(bdb, nkey, nextra);
    if (!k) return 0;
    for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = bdb->pDfltColl;
    for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
    return k;
}

/* 0 ok, else a message for the log. db: the event loop's connection (its schema is loaded). */
const char *jr_bt_init(sqlite3 *db) {
    bdb = db;
    static const char *checks[][2] = {
        {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
        {"SELECT pk FROM pragma_table_info('posts') WHERE name = 'id'", "1"},
        {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
        {"SELECT pk FROM pragma_table_info('users') WHERE name = 'id'", "1"},
        {"SELECT name FROM pragma_index_xinfo('posts_created_at_id_idx')", "created_at,id,"},
        {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
        {"SELECT name FROM pragma_index_xinfo('likes_post_id_idx')", "post_id,"},
        {"SELECT desc FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "0"},
        {"PRAGMA encoding", "UTF-8"},
    };
    for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++)
        if (!layout(checks[i][0], checks[i][1])) return checks[i][0];
    root_posts = rootpage("posts");
    root_users = rootpage("users");
    root_feed_idx = rootpage("posts_created_at_id_idx");
    root_likes_idx = rootpage("likes_post_id_idx");
    if (!root_posts || !root_users || !root_feed_idx || !root_likes_idx) return "root pages";
    ki_feed = keyinfo(2, 1, 1);
    ki_likes = keyinfo(1, 1, 0);
    if (!ki_feed || !ki_likes) return "keyinfo";
    bt = db->aDb[0].pBt;
    for (int i = 0; i < NCUR; i++) {
        cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
        if (!cur[i]) return "cursor alloc";
    }
    return 0;
}

/* Closes the cursors: before the loop's COMMIT/ROLLBACK, and after a read error. */
void jr_bt_close(void) {
    if (!cur_open) return;
    for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
    cur_open = 0;
}

/* In a transaction the loop has begun (BEGIN): the B-tree read transaction and the cursors, once per
 * transaction. If the transaction already writes, BeginTrans(read) leaves it as it is. 0 ok, -1 error. */
static int ensure(void) {
    if (cur_open) return 0;
    if (sqlite3_get_autocommit(bdb)) return -1;        /* no BEGIN: COMMIT would end it under the cursors */
    if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return -1;
    static const Pgno *roots[NCUR] = {&root_posts, &root_users, &root_feed_idx, &root_likes_idx};
    KeyInfo *kis[NCUR] = {0, 0, ki_feed, ki_likes};
    for (int i = 0; i < NCUR; i++) {
        sqlite3BtreeCursorZero(cur[i]);
        if (sqlite3BtreeCursor(bt, *roots[i], 0, kis[i], cur[i]) != SQLITE_OK) {
            for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
            return -1;
        }
    }
    cur_open = 1;
    return 0;
}

static int fail(void) {
    bt_errors++;
    jr_bt_close();          /* the next read reopens them */
    return -1;
}

/* ---- record decoding (SQLite's record format: header of varint serial types, then the values) ---- */

static inline u32 st_size(u64 t) {
    static const u8 sz[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
    return t < 12 ? sz[t] : (u32)((t - 12) >> 1);
}

/* Serial types and offsets of the first n columns of record p[0:len). 1 ok, 0 malformed. */
static int decode(const u8 *p, u32 len, int n, u64 *types, u32 *offs) {
    u64 h;
    u32 i = sqlite3GetVarint(p, &h);
    if (h > len || h < (u64)i) return 0;
    u32 off = (u32)h;
    for (int k = 0; k < n; k++) {
        if (i >= h) return 0;
        i += sqlite3GetVarint(p + i, &types[k]);
        offs[k] = off;
        off += st_size(types[k]);
        if (off > len || i > h) return 0;
    }
    return 1;
}

/* Integer value of serial type t at q. 1 ok, 0 not an integer. */
static inline int int_at(u64 t, const u8 *q, i64 *v) {
    if (t >= 1 && t <= 6) {
        u32 k = st_size(t);
        i64 x = (i8)q[0];
        for (u32 j = 1; j < k; j++) x = (x << 8) | q[j];
        *v = x;
        return 1;
    }
    if (t == 8 || t == 9) { *v = (i64)(t - 8); return 1; }
    return 0;
}

static inline int is_text(u64 t) { return t >= 13 && (t & 1); }

/* The current entry's record: all of it, from the page or (overflow) copied into spill. */
static const u8 *record(BtCursor *c, u32 *len) {
    u32 amt = 0;
    u32 total = sqlite3BtreePayloadSize(c);     /* first: it parses the cell that PayloadFetch then returns */
    const u8 *p = sqlite3BtreePayloadFetch(c, &amt);
    if (amt >= total) { *len = total; return p; }
    if (total > sizeof spill || sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
    *len = total;
    return spill;
}

/* Table cursor to rowid id: 1 found, 0 absent, -1 error. */
static int seek_row(BtCursor *c, i64 id) {
    int res = 0;
    if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
    return res == 0 && !sqlite3BtreeEof(c);
}

/* Index cursor to the first entry whose leading integer column is >= k: 1 on an entry, 0 none, -1 error. */
static int seek_ge(BtCursor *c, i64 k) {
    Mem m;
    UnpackedRecord r;
    int res = 0;
    memset(&m, 0, sizeof m);
    m.flags = MEM_Int;
    m.u.i = k;
    memset(&r, 0, sizeof r);
    r.pKeyInfo = ki_likes;
    r.aMem = &m;
    r.nField = 1;
    r.default_rc = +1;      /* entries equal on the key sort after it: land next to the first of them */
    if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
    if (sqlite3BtreeEof(c)) return 0;
    if (res < 0) {          /* on the entry just before: step to the first one >= k */
        int rc = sqlite3BtreeNext(c, 0);
        if (rc == SQLITE_DONE) return 0;
        if (rc != SQLITE_OK) return -1;
    }
    return 1;
}

/* Likes of lo <= post_id <= hi, one walk of likes_post_id_idx (post_id, rowid): n[post_id - lo]
 * counts the entries (n has hi - lo + 1 zeroed slots). 0 ok, -1 error. */
static int count_likes(i64 lo, i64 hi, int64_t *n) {
    BtCursor *c = cur[3];
    int r = seek_ge(c, lo);
    if (r <= 0) return r;
    for (;;) {
        u32 amt = 0;
        sqlite3BtreePayloadSize(c);                 /* parses the cell for PayloadFetch */
        const u8 *p = sqlite3BtreePayloadFetch(c, &amt);
        i64 v;
        /* Usually a 3-byte header (both serial types one byte), all on the page. */
        if (amt >= 3 && p[0] == 3 && p[1] < 0x80 && p[2] < 0x80 && 3 + st_size(p[1]) <= amt) {
            if (!int_at(p[1], p + 3, &v)) return -1;
        } else {
            u32 len;
            u64 t[1];
            u32 o[1];
            const u8 *q = record(c, &len);
            if (!q || !decode(q, len, 1, t, o) || !int_at(t[0], q + o[0], &v)) return -1;
        }
        if (v > hi) return 0;
        if (v >= lo) n[v - lo]++;
        int rc = sqlite3BtreeNext(c, 0);
        if (rc == SQLITE_DONE) return 0;
        if (rc != SQLITE_OK) return -1;
    }
}

static const u8 *keep(const u8 *p, u32 n) {
    if (arena_len + n > ARENA) return 0;
    u8 *d = arena + arena_len;
    memcpy(d, p, n);
    arena_len += n;
    return d;
}

/* Post row id (posts: id = rowid, user_id, body, created_at) and its author into *row; copy: text into the
 * arena (else it points into the cursors' pages, valid until they move). 1 found, 0 absent, 2 no such
 * user (the SQL join drops the row), -1 error. */
static int row_at(i64 id, jr_row *row, int copy);
static int row_of(i64 id, jr_row *row, int copy) {
    int r = seek_row(cur[0], id);
    if (r <= 0) return r;
    return row_at(id, row, copy);
}

/* The same, for the row the posts cursor is on. */
static int row_at(i64 id, jr_row *row, int copy) {
    int r;
    u32 len;
    const u8 *p = record(cur[0], &len);
    u64 t[4];
    u32 o[4];
    i64 uid;
    if (!p || !decode(p, len, 4, t, o) || !int_at(t[1], p + o[1], &uid) || !is_text(t[2]) || !is_text(t[3]))
        return -1;
    row->id = id;
    row->blen = (int32_t)st_size(t[2]);
    row->clen = (int32_t)st_size(t[3]);
    row->body = p + o[2];
    row->ca = p + o[3];
    if (copy) {
        row->body = keep(row->body, row->blen);
        row->ca = keep(row->ca, row->clen);
        if (!row->body || !row->ca) return -1;
    }
    r = seek_row(cur[1], uid);
    if (r < 0) return -1;
    if (r == 0) return 2;
    const u8 *q = record(cur[1], &len);
    if (!q || !decode(q, len, 2, t, o) || !is_text(t[1])) return -1;
    row->alen = (int32_t)st_size(t[1]);
    row->au = q + o[1];
    if (copy && !(row->au = keep(row->au, row->alen))) return -1;
    return 1;
}

/* GET /posts/:id: 1 found, 0 not found, -1 error. Pointers stay valid until the next jr_bt_* call. */
int jr_bt_post(int64_t id, jr_row *row) {
    if (ensure()) return fail();
    bt_reads++;
    int r = row_of(id, row, 0);
    if (r < 0) return fail();
    if (r != 1) return 0;   /* absent, or no such user: the SQL join finds no row either */
    row->likes = 0;
    return count_likes(id, id, &row->likes) < 0 ? fail() : 1;
}

/* Before a like: 1 the post exists, 0 not, -1 error. */
int jr_bt_exists(int64_t id) {
    if (ensure()) return fail();
    bt_reads++;
    int r = seek_row(cur[0], id);
    return r < 0 ? fail() : r;
}

/* GET /feed: the 20 newest posts (posts_created_at_id_idx: created_at DESC, id DESC, rowid) into rows[20],
 * text copied into the arena. Returns the number of rows (fewer than 20 only when there are fewer posts),
 * -1 error, -2 a post without a user (the caller runs the SQL query, which skips it). */
int jr_bt_feed(jr_row *rows) {
    if (ensure()) return fail();
    bt_reads++;
    arena_len = 0;
    BtCursor *c = cur[2];
    int empty = 1, n = 0;
    if (sqlite3BtreeFirst(c, &empty) != SQLITE_OK) return fail();
    while (!empty && n < 20) {
        u32 len;
        u64 t[2];
        u32 o[2];
        i64 id;
        const u8 *p = record(c, &len);
        if (!p || !decode(p, len, 2, t, o) || !int_at(t[1], p + o[1], &id)) return fail();
        rows[n++].id = id;
        int rc = sqlite3BtreeNext(c, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return fail();
    }
    /* Like counts: one walk of likes_post_id_idx over [min id, max id] when the ids are close together
     * (the usual case: hi, hi-1, ..., since ids and created_at both grow with each insert), else one seek
     * per post. */
    i64 mn = INT64_MAX, mx = INT64_MIN;
    int dense = n > 0;
    for (int k = 0; k < n; k++) {
        rows[k].likes = 0;
        if (rows[k].id < mn) mn = rows[k].id;
        if (rows[k].id > mx) mx = rows[k].id;
        if (rows[k].id != rows[0].id - k) dense = 0;
    }
    if (n > 0 && mx - mn < SPAN) {
        static int64_t cnt[SPAN];
        memset(cnt, 0, (size_t)(mx - mn + 1) * sizeof cnt[0]);
        if (count_likes(mn, mx, cnt) < 0) return fail();
        for (int k = 0; k < n; k++) rows[k].likes = cnt[rows[k].id - mn];
    } else {
        for (int k = 0; k < n; k++)
            if (count_likes(rows[k].id, rows[k].id, &rows[k].likes) < 0) return fail();
    }
    /* Rows: for hi, hi-1, ..., lo one seek to lo and then the next rows in the table, else a seek each. */
    int walk = 0;
    if (dense) {
        int r = seek_row(cur[0], mn);
        if (r < 0) return fail();
        walk = r;
    }
    int up = walk;                          /* the order is fixed once: walking up from lo, newest last */
    for (int i = 0; i < n; i++) {
        int k = up ? n - 1 - i : i;
        int64_t likes = rows[k].likes;
        int r;
        if (walk && i > 0) {
            int rc = sqlite3BtreeNext(cur[0], 0);
            if (rc != SQLITE_OK && rc != SQLITE_DONE) return fail();
            /* the next row is not the next id (none deleted between: the feed index said so) */
            if (rc == SQLITE_DONE || sqlite3BtreeIntegerKey(cur[0]) != rows[k].id) walk = 0;
        }
        r = walk ? row_at(rows[k].id, &rows[k], 1) : row_of(rows[k].id, &rows[k], 1);
        if (r <= 0) return fail();          /* an index entry without its row: corrupt */
        if (r == 2) return -2;
        rows[k].likes = likes;
    }
    return n;
}

long jr_bt_reads(void) { return bt_reads; }
long jr_bt_errors(void) { return bt_errors; }
