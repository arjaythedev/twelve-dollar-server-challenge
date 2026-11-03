/* Direct B-tree reads (see db_btree.h). The amalgamation is included so
 * that the SQLITE_PRIVATE cursor API is visible; its warnings are its own. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#include "sqlite3.c"
#pragma GCC diagnostic pop

#include <string.h>

#include "db.h"
#include "db_btree.h"

#define FEED_N      20
#define SPAN_MAX    1024 /* widest id span the one-scan like count accepts */
#define BODY_MAX    2000
#define CREATED_MAX 31
#define NAME_MAX    512

enum { C_POSTS, C_USERS, C_LIKES_IDX, C_CREATED_IDX, C_N };

struct bt {
    sqlite3 *conn;
    Btree *btree;
    Pgno root[C_N];
    KeyInfo *ki_likes, *ki_created;
    BtCursor *cur[C_N];
    unsigned open; /* bit i: cur[i] is open */
    int txn_mine;  /* this layer began the read transaction (autocommit mode) */
    int schema_cookie;
    uint32_t counts[SPAN_MAX];
    struct {
        int64_t id, user_id, likes;
        const u8 *body, *created, *author;
        u32 body_len, created_len, author_len;
    } row[FEED_N];
    /* copies: the posts cursor moves on, so page pointers do not survive */
    u8 body[FEED_N][BODY_MAX];
    u8 created[FEED_N][CREATED_MAX];
    u8 author[FEED_N][NAME_MAX];
};

/* ------------------------------------------------------------------ record format */

static inline const u8 *rd_varint(const u8 *p, const u8 *end, u64 *v) {
    u64 x = 0;
    for (int i = 0; i < 8; i++) {
        if (p >= end) return NULL;
        x = x << 7 | (p[0] & 0x7f);
        if (!(*p++ & 0x80)) {
            *v = x;
            return p;
        }
    }
    if (p >= end) return NULL;
    *v = x << 8 | *p++;
    return p;
}

static inline u32 serial_len(u64 t) {
    static const u8 n[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
    return t < 12 ? n[t] : (u32)((t - 12) / 2);
}

static inline int serial_is_int(u64 t) {
    return (t >= 1 && t <= 6) || t == 8 || t == 9;
}

static inline int64_t serial_int(u64 t, const u8 *p) {
    switch (t) {
    case 1: return (int8_t)p[0];
    case 2: return (int16_t)((u16)p[0] << 8 | p[1]);
    case 3: return ((int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8)) >> 8;
    case 4: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
    case 5: {
        int64_t v = 0;
        for (int i = 0; i < 6; i++) v = v << 8 | p[i];
        return (v << 16) >> 16;
    }
    case 6: {
        u64 v = 0;
        for (int i = 0; i < 8; i++) v = v << 8 | p[i];
        return (int64_t)v;
    }
    case 9: return 1;
    default: return 0;
    }
}

/* Decodes up to nf fields of a record held entirely in [rec, rec+amt).
 * Returns the number of fields decoded, or -1 if the record is malformed. */
static int rec_decode(const u8 *rec, u32 amt, int nf, u64 *types, const u8 **data) {
    const u8 *end = rec + amt;
    u64 hdr;
    const u8 *h = rd_varint(rec, end, &hdr);
    if (!h || hdr > amt) return -1;
    const u8 *he = rec + hdr, *d = he;
    int i = 0;
    while (h < he && i < nf) {
        h = rd_varint(h, he, &types[i]);
        if (!h) return -1;
        data[i] = d;
        d += serial_len(types[i]);
        if (d > end) return -1;
        i++;
    }
    return i;
}

/* The record of the row under the cursor, if it fits its leaf cell
 * (records that spill onto overflow pages are left to the SQL path). */
static inline const u8 *local_record(BtCursor *c, u32 *amt) {
    u32 sz = sqlite3BtreePayloadSize(c);
    const u8 *p = sqlite3BtreePayloadFetch(c, amt);
    if (*amt < sz) return NULL;
    *amt = sz;
    return p;
}

/* ------------------------------------------------------------------ setup */

static int schema_ok(sqlite3 *db, struct bt *bt, char *err, size_t err_cap) {
    Table *tp = sqlite3FindTable(db, "posts", "main");
    Table *tu = sqlite3FindTable(db, "users", "main");
    Table *tl = sqlite3FindTable(db, "likes", "main");
    Index *ilp = sqlite3FindIndex(db, "likes_post_id_idx", "main");
    Index *ipc = sqlite3FindIndex(db, "posts_created_at_id_idx", "main");
    if (!tp || !tu || !tl || !ilp || !ipc) {
        snprintf(err, err_cap, "btree: a table or index is missing");
        return -1;
    }
    /* the decoders below hard-code the column positions of the seed schema */
    if (tp->nCol != 4 || tp->iPKey != 0 || tu->nCol != 3 || tu->iPKey != 0 || tl->nCol != 3 || !HasRowid(tl) ||
        ilp->nKeyCol != 1 || ilp->aiColumn[0] != 1 || ipc->nKeyCol != 2 || ipc->aiColumn[0] != 3 ||
        ipc->aiColumn[1] != 0 || ipc->aSortOrder[0] != SQLITE_SO_DESC || ipc->aSortOrder[1] != SQLITE_SO_DESC) {
        snprintf(err, err_cap, "btree: schema shape differs from the one this code was written for");
        return -1;
    }
    bt->root[C_POSTS] = tp->tnum;
    bt->root[C_USERS] = tu->tnum;
    bt->root[C_LIKES_IDX] = ilp->tnum;
    bt->root[C_CREATED_IDX] = ipc->tnum;
    Parse ps;
    memset(&ps, 0, sizeof ps);
    ps.db = db;
    bt->ki_likes = sqlite3KeyInfoOfIndex(&ps, ilp);
    bt->ki_created = sqlite3KeyInfoOfIndex(&ps, ipc);
    if (!bt->ki_likes || !bt->ki_created) {
        snprintf(err, err_cap, "btree: key info");
        return -1;
    }
    bt->schema_cookie = db->aDb[0].pSchema->schema_cookie;
    return 0;
}

struct bt *bt_open(sqlite3 *conn, char *err, size_t err_cap) {
    struct bt *bt = calloc(1, sizeof *bt);
    if (!bt) {
        snprintf(err, err_cap, "btree: out of memory");
        return NULL;
    }
    bt->conn = conn;
    bt->btree = conn->aDb[0].pBt;
    if (schema_ok(conn, bt, err, err_cap)) {
        bt_close(bt);
        return NULL;
    }
    for (int i = 0; i < C_N; i++) {
        bt->cur[i] = calloc(1, (size_t)sqlite3BtreeCursorSize());
        if (!bt->cur[i]) {
            snprintf(err, err_cap, "btree: out of memory");
            bt_close(bt);
            return NULL;
        }
    }
    return bt;
}

void bt_close(struct bt *bt) {
    if (!bt) return;
    for (int i = 0; i < C_N; i++) free(bt->cur[i]);
    if (bt->ki_likes) sqlite3KeyInfoUnref(bt->ki_likes);
    if (bt->ki_created) sqlite3KeyInfoUnref(bt->ki_created);
    free(bt);
}

/* ------------------------------------------------------------------ transaction and cursors */

/* Joins the read transaction of the connection's open SQL transaction, or
 * begins one. A schema change since open is the one thing the cursors
 * cannot tolerate; the SQL path re-prepares and copes. */
static int txn_begin(struct bt *bt) {
    int was_none = sqlite3BtreeTxnState(bt->btree) == SQLITE_TXN_NONE;
    int cookie = 0;
    int rc = sqlite3BtreeBeginTrans(bt->btree, 0, &cookie);
    if (rc != SQLITE_OK) return -1;
    if (was_none && sqlite3_get_autocommit(bt->conn)) bt->txn_mine = 1;
    if (cookie != bt->schema_cookie) {
        bt_read_end(bt);
        return -1;
    }
    return 0;
}

void bt_read_end(struct bt *bt) {
    if (!bt || !bt->txn_mine) return;
    bt->txn_mine = 0;
    if (sqlite3_get_autocommit(bt->conn)) sqlite3BtreeCommit(bt->btree);
}

static BtCursor *cur(struct bt *bt, int k) {
    if (!(bt->open & (1u << k))) {
        KeyInfo *ki = k == C_LIKES_IDX ? bt->ki_likes : k == C_CREATED_IDX ? bt->ki_created : NULL;
        sqlite3BtreeCursorZero(bt->cur[k]); /* CloseCursor frees pKey/aOverflow without clearing */
        if (sqlite3BtreeCursor(bt->btree, bt->root[k], 0, ki, bt->cur[k]) != SQLITE_OK) return NULL;
        bt->open |= 1u << k;
    }
    return bt->cur[k];
}

static void cursors_close(struct bt *bt) {
    for (int i = 0; i < C_N; i++) {
        if (bt->open & (1u << i)) sqlite3BtreeCloseCursor(bt->cur[i]);
    }
    bt->open = 0;
}

/* ------------------------------------------------------------------ reads */

/* Counts likes_post_id_idx entries with post_id in [lo, hi] into
 * counts[post_id - lo] (counts may be NULL: just the total). -1 on error. */
static int64_t count_likes(struct bt *bt, int64_t lo, int64_t hi, uint32_t *counts) {
    BtCursor *c = cur(bt, C_LIKES_IDX);
    if (!c) return -1;
    Mem m;
    memset(&m, 0, sizeof m);
    m.flags = MEM_Int;
    m.u.i = lo;
    UnpackedRecord key;
    memset(&key, 0, sizeof key);
    key.pKeyInfo = bt->ki_likes;
    key.aMem = &m;
    key.nField = 1;
    key.default_rc = +1; /* an entry equal on post_id sorts after the key: land on the first one */
    int res;
    if (sqlite3BtreeIndexMoveto(c, &key, &res) != SQLITE_OK) return -1;
    if (sqlite3BtreeEof(c)) return 0;
    int rc = SQLITE_OK;
    if (res < 0) {
        rc = sqlite3BtreeNext(c, 0);
        if (rc == SQLITE_DONE) return 0;
        if (rc != SQLITE_OK) return -1;
    }
    int64_t total = 0;
    for (;;) {
        u32 amt;
        const u8 *rec = local_record(c, &amt);
        u64 t[1];
        const u8 *d[1];
        if (!rec || rec_decode(rec, amt, 1, t, d) != 1 || !serial_is_int(t[0])) return -1;
        int64_t pid = serial_int(t[0], d[0]);
        if (pid > hi) break;
        if (pid < lo) return -1; /* cannot happen after the seek */
        total++;
        if (counts) counts[pid - lo]++;
        rc = sqlite3BtreeNext(c, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return -1;
    }
    return total;
}

/* Fills row[i] from the posts row under the cursor plus its author.
 * Returns 1 ok, 0 if the author does not exist (an inner join drops the
 * post), -1 to fall back. */
static int load_row(struct bt *bt, BtCursor *cp, int i) {
    u32 amt;
    const u8 *rec = local_record(cp, &amt);
    u64 t[4];
    const u8 *d[4];
    if (!rec || rec_decode(rec, amt, 4, t, d) != 4 || !serial_is_int(t[1])) return -1;
    u32 bl = serial_len(t[2]), cl = serial_len(t[3]);
    /* text serial types are odd and >= 13; anything else is not a plain row */
    if (t[2] < 13 || !(t[2] & 1) || t[3] < 13 || !(t[3] & 1)) return -1;
    if (bl > BODY_MAX || cl > CREATED_MAX) return -1;
    bt->row[i].user_id = serial_int(t[1], d[1]);
    memcpy(bt->body[i], d[2], bl);
    memcpy(bt->created[i], d[3], cl);
    bt->row[i].body_len = bl;
    bt->row[i].created_len = cl;

    BtCursor *cu = cur(bt, C_USERS);
    if (!cu) return -1;
    int res;
    if (sqlite3BtreeTableMoveto(cu, bt->row[i].user_id, 0, &res) != SQLITE_OK) return -1;
    if (res != 0 || sqlite3BtreeEof(cu)) return 0;
    u64 tu[2];
    const u8 *du[2];
    rec = local_record(cu, &amt);
    if (!rec || rec_decode(rec, amt, 2, tu, du) != 2) return -1;
    if (tu[1] < 13 || !(tu[1] & 1)) return -1;
    u32 ul = serial_len(tu[1]);
    if (ul > NAME_MAX) return -1;
    memcpy(bt->author[i], du[1], ul);
    bt->row[i].author_len = ul;
    return 1;
}

static size_t write_rows(struct bt *bt, int n, const char *head, size_t head_len, char *out, size_t cap) {
    if (cap < head_len + 2) return 0;
    memcpy(out, head, head_len);
    size_t o = head_len;
    for (int i = 0; i < n; i++) {
        if (i) {
            if (o + 1 > cap) return 0;
            out[o++] = ',';
        }
        size_t w = db_write_post_json(out + o, cap - o, bt->row[i].id, (const char *)bt->body[i], bt->row[i].body_len,
                                      (const char *)bt->created[i], bt->row[i].created_len, (const char *)bt->author[i],
                                      bt->row[i].author_len, bt->row[i].likes);
        if (!w) return 0;
        o += w;
    }
    return o;
}

size_t bt_feed_json(struct bt *bt, char *out, size_t cap) {
    size_t result = 0;
    if (txn_begin(bt)) return 0;
    BtCursor *cc = cur(bt, C_CREATED_IDX), *cp = cur(bt, C_POSTS);
    if (!cc || !cp) goto done;

    /* 1. the 20 newest ids: the index is ordered (created_at DESC, id DESC),
     *    so its first entries are the feed's order; the rowid trails the key */
    int n = 0, res, rc;
    if (sqlite3BtreeFirst(cc, &res) != SQLITE_OK || res) goto done;
    for (;;) {
        u32 amt;
        const u8 *rec = local_record(cc, &amt);
        u64 t[3];
        const u8 *d[3];
        if (!rec || rec_decode(rec, amt, 3, t, d) != 3 || !serial_is_int(t[2])) goto done;
        bt->row[n].id = serial_int(t[2], d[2]);
        bt->row[n].likes = 0;
        n++;
        if (n == FEED_N) break;
        rc = sqlite3BtreeNext(cc, 0);
        if (rc == SQLITE_DONE) goto done; /* fewer than 20 posts: SQL handles the short feed */
        if (rc != SQLITE_OK) goto done;
    }
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (int i = 0; i < n; i++) {
        if (bt->row[i].id < lo) lo = bt->row[i].id;
        if (bt->row[i].id > hi) hi = bt->row[i].id;
    }
    if (hi - lo + 1 > SPAN_MAX) goto done;

    /* 2. one scan of likes_post_id_idx over the span */
    memset(bt->counts, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
    if (count_likes(bt, lo, hi, bt->counts) < 0) goto done;
    for (int i = 0; i < n; i++) bt->row[i].likes = bt->counts[bt->row[i].id - lo];

    /* 3. the rows and their authors; ids usually descend by one, so step
     *    the cursor back instead of seeking from the root */
    int64_t prev = INT64_MIN;
    for (int i = 0; i < n; i++) {
        int64_t id = bt->row[i].id, found = 0;
        if (prev == id + 1 && !sqlite3BtreeEof(cp)) {
            rc = sqlite3BtreePrevious(cp, 0);
            if (rc == SQLITE_OK && sqlite3BtreeIntegerKey(cp) == id)
                found = 1;
            else if (rc != SQLITE_OK && rc != SQLITE_DONE)
                goto done;
        }
        if (!found) {
            if (sqlite3BtreeTableMoveto(cp, id, 0, &res) != SQLITE_OK) goto done;
            found = res == 0 && !sqlite3BtreeEof(cp);
        }
        prev = id;
        if (!found) goto done;
        if (load_row(bt, cp, i) != 1) goto done; /* a dropped row changes the set: let SQL do it */
    }
    result = write_rows(bt, n, "{\"posts\":[", 10, out, cap);
    if (result) {
        if (result + 2 > cap) {
            result = 0;
        } else {
            out[result++] = ']';
            out[result++] = '}';
        }
    }
done:
    cursors_close(bt);
    if (!sqlite3_get_autocommit(bt->conn)) return result; /* the shared transaction ends it */
    bt_read_end(bt);
    return result;
}

size_t bt_post_json(struct bt *bt, int64_t id, char *out, size_t cap) {
    size_t result = (size_t)-1;
    if (txn_begin(bt)) return (size_t)-1;
    BtCursor *cp = cur(bt, C_POSTS);
    if (!cp) goto done;
    int res;
    if (sqlite3BtreeTableMoveto(cp, id, 0, &res) != SQLITE_OK) goto done;
    if (res != 0 || sqlite3BtreeEof(cp)) {
        result = 0;
        goto done;
    }
    bt->row[0].id = id;
    int k = load_row(bt, cp, 0);
    if (k < 0) goto done;
    if (k == 0) {
        result = 0; /* the reference JOIN drops a post whose author is missing */
        goto done;
    }
    int64_t likes = count_likes(bt, id, id, NULL);
    if (likes < 0) goto done;
    bt->row[0].likes = likes;
    result = write_rows(bt, 1, "{\"post\":", 8, out, cap);
    if (!result || result + 1 > cap) {
        result = (size_t)-1;
    } else {
        out[result++] = '}';
    }
done:
    cursors_close(bt);
    if (!sqlite3_get_autocommit(bt->conn)) return result;
    bt_read_end(bt);
    return result;
}
