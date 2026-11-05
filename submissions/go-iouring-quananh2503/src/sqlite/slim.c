#define _GNU_SOURCE 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "slim.h"
#include "amalg/sqlite3.h"
#include "amalg/sqlite3.c"

/* Cần khai báo biến này cho CGO link với sqlite3_libversion() */
const char sqlite3_version[] = SQLITE_VERSION;

/* Struct quản lý output JSON ghi trực tiếp vào RAM */
typedef struct {
    char *p;
    int cap;
    int n;
    int err;
} jout;

static inline void jraw(jout *o, const char *s, int n) {
    if (n <= 0 || o->err) return;
    if (n > o->cap - o->n) {
        o->err = 1;
        return;
    }
    memcpy(o->p + o->n, s, (size_t)n);
    o->n += n;
}

static inline void jch(jout *o, char c) {
    jraw(o, &c, 1);
}

static inline void jint(jout *o, sqlite3_int64 v) {
    char b[32];
    int n = snprintf(b, sizeof(b), "%lld", (long long)v);
    jraw(o, b, n);
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
                        buf[0] = '\\'; buf[1] = 'u'; buf[2] = '0'; buf[3] = '0';
                        buf[4] = hex[c >> 4]; buf[5] = hex[c & 15];
                        esc = buf; elen = 6;
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

/* Kiểu dữ liệu nhị phân bóc tách từ SQLite Record */
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

/* Parse header record nhị phân của SQLite không qua VDBE */
static int parse_vals(const unsigned char *p, u32 n, val *v, int want) {
    u32 hdr = 0;
    int hb = getVarint32(p, hdr);
    if (hb <= 0 || hdr > n || (u32)hb > hdr) {
        return SQLITE_CORRUPT;
    }
    const unsigned char *h = p + hb;
    const unsigned char *data = p + hdr;
    u32 off = 0;
    for (int i = 0; i < want; i++) {
        if (h >= p + hdr) {
            return SQLITE_CORRUPT;
        }
        u32 st = 0;
        int nb = getVarint32(h, st);
        if (nb <= 0) {
            return SQLITE_CORRUPT;
        }
        h += nb;
        u32 len = sqlite3VdbeSerialTypeLen(st);
        if (off + len > (u32)(p + n - data)) {
            return SQLITE_CORRUPT;
        }
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

/* Bộ Engine quản lý B-Tree Cursor và Prepared Statements */
typedef struct {
    sqlite3 *db;
    Btree *bt;
    BtCursor *feed;
    BtCursor *post;
    BtCursor *user;
    BtCursor *like;
    KeyInfo *feedKey;
    KeyInfo *likeKey;
    UnpackedRecord *likeFind;

    sqlite3_stmt *stmt_health;
    sqlite3_stmt *stmt_insert_post;
    sqlite3_stmt *stmt_insert_like;
} Engine;

static Engine g_engine;

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

static inline const unsigned char *payload(BtCursor *c, u32 *n) {
    *n = sqlite3BtreePayloadSize(c);
    return (const unsigned char *)sqlite3BtreePayloadFetch(c, n);
}

/* Đếm số like của một post_id trên cây likes_post_id_idx */
static int count_likes(Engine *e, sqlite3_int64 post_id, int *likes_out) {
    Mem m;
    memset(&m, 0, sizeof(m));
    m.flags = MEM_Int;
    m.u.i = post_id;

    UnpackedRecord r;
    memset(&r, 0, sizeof(r));
    r.pKeyInfo = e->likeKey;
    r.aMem = &m;
    r.nField = 1;
    r.default_rc = +1;

    int res = 0;
    int rc = sqlite3BtreeIndexMoveto(e->like, &r, &res);
    if (rc != SQLITE_OK) return rc;
    if (sqlite3BtreeEof(e->like)) {
        *likes_out = 0;
        return SQLITE_OK;
    }
    if (res < 0) {
        rc = sqlite3BtreeNext(e->like, 0);
        if (rc == SQLITE_DONE) {
            *likes_out = 0;
            return SQLITE_OK;
        }
        if (rc != SQLITE_OK) return rc;
    }

    int count = 0;
    while (!sqlite3BtreeEof(e->like)) {
        u32 n = 0;
        const unsigned char *p = payload(e->like, &n);
        if (!p) return SQLITE_CORRUPT;
        val f[1];
        if (parse_vals(p, n, f, 1) != SQLITE_OK) return SQLITE_CORRUPT;
        if (f[0].kind != V_INT || f[0].i != post_id) break;
        count++;
        rc = sqlite3BtreeNext(e->like, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return rc;
    }
    *likes_out = count;
    return SQLITE_OK;
}

/* Format 1 post object */
static int emit_post(Engine *e, sqlite3_int64 id, jout *o) {
    int res = 0;
    int rc = sqlite3BtreeTableMoveto(e->post, id, 0, &res);
    if (rc != SQLITE_OK) return rc;
    if (res != 0) return SQLITE_NOTFOUND;

    u32 pn = 0;
    const unsigned char *p = payload(e->post, &pn);
    if (!p) return SQLITE_CORRUPT;
    val pf[4];
    if (parse_vals(p, pn, pf, 4) != SQLITE_OK) return SQLITE_CORRUPT;
    if (pf[1].kind != V_INT || pf[2].kind != V_TEXT || pf[3].kind != V_TEXT) {
        return SQLITE_CORRUPT;
    }

    rc = sqlite3BtreeTableMoveto(e->user, pf[1].i, 0, &res);
    if (rc != SQLITE_OK) return rc;
    if (res != 0) return SQLITE_NOTFOUND;

    u32 un = 0;
    const unsigned char *up = payload(e->user, &un);
    if (!up) return SQLITE_CORRUPT;
    val uf[2];
    if (parse_vals(up, un, uf, 2) != SQLITE_OK || uf[1].kind != V_TEXT) {
        return SQLITE_CORRUPT;
    }

    int likes = 0;
    rc = count_likes(e, id, &likes);
    if (rc != SQLITE_OK) return rc;

    jraw(o, "{\"id\":", 6);
    jint(o, id);
    jraw(o, ",\"body\":", 8);
    jstr(o, pf[2].z, pf[2].n);
    jraw(o, ",\"created_at\":", 14);
    jstr(o, pf[3].z, pf[3].n);
    jraw(o, ",\"author\":", 10);
    jstr(o, uf[1].z, uf[1].n);
    jraw(o, ",\"like_count\":", 14);
    jint(o, (sqlite3_int64)likes);
    jch(o, '}');
    return SQLITE_OK;
}

static Pgno get_rootpage(sqlite3 *db, const char *name) {
    sqlite3_stmt *stmt;
    Pgno root = 0;
    if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_master WHERE name = ?", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            root = (Pgno)sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return root;
}

int init_feed_engine(const char *db_path) {
    memset(&g_engine, 0, sizeof(g_engine));
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(db_path, &g_engine.db, flags, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_busy_timeout(g_engine.db, 5000);
    sqlite3_exec(g_engine.db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA mmap_size=0; PRAGMA cache_size=-64000; PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);

    g_engine.bt = g_engine.db->aDb[0].pBt;
    if (!g_engine.bt) return -2;

    Pgno root_posts = get_rootpage(g_engine.db, "posts");
    Pgno root_users = get_rootpage(g_engine.db, "users");
    Pgno root_feed = get_rootpage(g_engine.db, "posts_created_at_id_idx");
    Pgno root_likes = get_rootpage(g_engine.db, "likes_post_id_idx");

    if (!root_posts || !root_users || !root_feed || !root_likes) return -3;

    g_engine.feedKey = sqlite3KeyInfoAlloc(g_engine.db, 2, 1);
    g_engine.likeKey = sqlite3KeyInfoAlloc(g_engine.db, 2, 0);
    if (!g_engine.feedKey || !g_engine.likeKey) return -4;

    g_engine.feedKey->aSortFlags[0] = KEYINFO_ORDER_DESC;
    g_engine.feedKey->aSortFlags[1] = KEYINFO_ORDER_DESC;

    int rc = SQLITE_OK;
    g_engine.feed = open_cursor(g_engine.bt, root_feed, g_engine.feedKey, &rc);
    g_engine.post = open_cursor(g_engine.bt, root_posts, NULL, &rc);
    g_engine.user = open_cursor(g_engine.bt, root_users, NULL, &rc);
    g_engine.like = open_cursor(g_engine.bt, root_likes, g_engine.likeKey, &rc);

    if (!g_engine.feed || !g_engine.post || !g_engine.user || !g_engine.like) return -5;

    g_engine.likeFind = sqlite3VdbeAllocUnpackedRecord(g_engine.likeKey);
    if (!g_engine.likeFind) return -6;
    g_engine.likeFind->nField = 2;
    g_engine.likeFind->r1 = -1;
    g_engine.likeFind->r2 = 1;
    g_engine.likeFind->default_rc = 0;

    // Prepared statements cho write và health check
    rc = sqlite3_prepare_v2(g_engine.db, "SELECT 1;", -1, &g_engine.stmt_health, NULL);
    if (rc != SQLITE_OK) return -7;

    rc = sqlite3_prepare_v2(g_engine.db, "INSERT INTO posts (user_id, body, created_at) VALUES (?, ?, ?) RETURNING id;", -1, &g_engine.stmt_insert_post, NULL);
    if (rc != SQLITE_OK) return -8;

    rc = sqlite3_prepare_v2(g_engine.db, "INSERT INTO likes (user_id, post_id, created_at) VALUES (?, ?, ?) ON CONFLICT (user_id, post_id) DO NOTHING;", -1, &g_engine.stmt_insert_like, NULL);
    if (rc != SQLITE_OK) return -9;

    return 0;
}

int db_health(void) {
    if (!g_engine.stmt_health) return 0;
    int rc = sqlite3_step(g_engine.stmt_health);
    sqlite3_reset(g_engine.stmt_health);
    return (rc == SQLITE_ROW) ? 1 : 0;
}

int get_feed(char *out, int cap, int *outlen) {
    if (!g_engine.bt) return -1;
    jout o = {out, cap, 0, 0};
    *outlen = 0;

    // 1. Bắt đầu Read Transaction trên B-tree (với retry nếu có write transaction vừa commit)
    int rc = sqlite3BtreeBeginTrans(g_engine.bt, 0, NULL);
    if (rc != SQLITE_OK) {
        for (int retry = 0; retry < 5 && rc != SQLITE_OK; retry++) {
            rc = sqlite3BtreeBeginTrans(g_engine.bt, 0, NULL);
        }
        if (rc != SQLITE_OK) return -2;
    }

    jraw(&o, "{\"posts\":[", 10);
    int res = 0;
    rc = sqlite3BtreeFirst(g_engine.feed, &res);
    if (rc == SQLITE_OK && res == 0) {
        int count = 0;
        while (!sqlite3BtreeEof(g_engine.feed) && count < 20) {
            u32 n = 0;
            const unsigned char *p = payload(g_engine.feed, &n);
            if (!p) { rc = SQLITE_CORRUPT; break; }

            val f[2];
            int prc = parse_vals(p, n, f, 2);
            if (prc != SQLITE_OK || f[1].kind != V_INT) {
                rc = SQLITE_CORRUPT;
                break;
            }
            if (count > 0) jch(&o, ',');
            rc = emit_post(&g_engine, f[1].i, &o);
            if (rc != SQLITE_OK) break;

            count++;
            rc = sqlite3BtreeNext(g_engine.feed, 0);
            if (rc != SQLITE_OK && rc != SQLITE_DONE) break;
        }
    }

    jraw(&o, "]}", 2);
    sqlite3BtreeCommit(g_engine.bt);

    if (o.err) return -3;
    *outlen = o.n;
    return 0;
}

int get_post(int64_t id, char *out, int cap, int *outlen) {
    if (!g_engine.bt) return -1;
    jout o = {out, cap, 0, 0};
    *outlen = 0;

    if (sqlite3BtreeBeginTrans(g_engine.bt, 0, NULL) != SQLITE_OK) {
        return -2;
    }

    jraw(&o, "{\"post\":", 8);
    int rc = emit_post(&g_engine, (sqlite3_int64)id, &o);
    if (rc == SQLITE_NOTFOUND) {
        sqlite3BtreeCommit(g_engine.bt);
        return 404;
    }
    if (rc != SQLITE_OK || o.err) {
        sqlite3BtreeCommit(g_engine.bt);
        return -3;
    }
    jch(&o, '}');

    sqlite3BtreeCommit(g_engine.bt);
    *outlen = o.n;
    return 200;
}

int post_exists(int64_t id) {
    if (!g_engine.bt) return -1;
    if (sqlite3BtreeBeginTrans(g_engine.bt, 0, NULL) != SQLITE_OK) {
        return -2;
    }
    int res = 0;
    int rc = sqlite3BtreeTableMoveto(g_engine.post, (sqlite3_int64)id, 0, &res);
    sqlite3BtreeCommit(g_engine.bt);
    if (rc != SQLITE_OK) return -3;
    return (res == 0) ? 1 : 0;
}

int get_posts_batch(const int64_t *ids, int n, char *out_buf, int out_cap, PostBatchItem *items) {
    if (!g_engine.bt) return -1;
    if (n <= 0) return 0;

    if (sqlite3BtreeBeginTrans(g_engine.bt, 0, NULL) != SQLITE_OK) {
        return -2;
    }

    jout o = {out_buf, out_cap, 0, 0};

    for (int i = 0; i < n; i++) {
        int64_t id = ids[i];
        int start_offset = o.n;
        jraw(&o, "{\"post\":", 8);
        int rc = emit_post(&g_engine, (sqlite3_int64)id, &o);
        if (rc == SQLITE_NOTFOUND) {
            o.n = start_offset; // rollback buffer
            items[i].status = 404;
            items[i].offset = 0;
            items[i].len = 0;
        } else if (rc != SQLITE_OK || o.err) {
            o.n = start_offset; // rollback buffer
            items[i].status = -3;
            items[i].offset = 0;
            items[i].len = 0;
        } else {
            jch(&o, '}');
            items[i].status = 200;
            items[i].offset = start_offset;
            items[i].len = o.n - start_offset;
        }
    }

    sqlite3BtreeCommit(g_engine.bt);
    return 0;
}

int post_exists_batch(const int64_t *ids, int n, int32_t *results) {
    if (!g_engine.bt) return -1;
    if (n <= 0) return 0;

    if (sqlite3BtreeBeginTrans(g_engine.bt, 0, NULL) != SQLITE_OK) {
        return -2;
    }

    for (int i = 0; i < n; i++) {
        if (i > 0 && ids[i] == ids[i - 1]) {
            results[i] = results[i - 1];
            continue;
        }
        int res = 0;
        int rc = sqlite3BtreeTableMoveto(g_engine.post, (sqlite3_int64)ids[i], 0, &res);
        if (rc != SQLITE_OK) {
            results[i] = -3;
        } else {
            results[i] = (res == 0) ? 1 : 0;
        }
    }

    sqlite3BtreeCommit(g_engine.bt);
    return 0;
}

int create_posts_batch(
    const int64_t *user_ids,
    const char *bodies_buf,
    const int32_t *body_offsets,
    const int32_t *body_lens,
    int n,
    const char *now_str,
    int64_t *out_ids,
    int32_t *out_statuses
) {
    if (!g_engine.db || !g_engine.stmt_insert_post) return -1;
    if (n <= 0) return 0;

    // Group Commit: 1 Write Transaction cho cả batch
    int rc = sqlite3_exec(g_engine.db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        for (int retry = 0; retry < 5 && rc != SQLITE_OK; retry++) {
            rc = sqlite3_exec(g_engine.db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        }
        if (rc != SQLITE_OK) return -2;
    }

    for (int i = 0; i < n; i++) {
        sqlite3_bind_int64(g_engine.stmt_insert_post, 1, user_ids[i]);
        sqlite3_bind_text(g_engine.stmt_insert_post, 2, bodies_buf + body_offsets[i], body_lens[i], SQLITE_STATIC);
        sqlite3_bind_text(g_engine.stmt_insert_post, 3, now_str, 24, SQLITE_STATIC);

        rc = sqlite3_step(g_engine.stmt_insert_post);
        if (rc == SQLITE_ROW) {
            out_ids[i] = sqlite3_column_int64(g_engine.stmt_insert_post, 0);
            out_statuses[i] = 201;
        } else {
            out_ids[i] = 0;
            out_statuses[i] = 500;
        }

        sqlite3_reset(g_engine.stmt_insert_post);
        sqlite3_clear_bindings(g_engine.stmt_insert_post);
    }

    sqlite3_exec(g_engine.db, "COMMIT;", NULL, NULL, NULL);
    return 0;
}

int insert_likes_batch(
    const int64_t *user_ids,
    const int64_t *post_ids,
    int n,
    const char *now_str,
    int32_t *results
) {
    if (!g_engine.db || !g_engine.stmt_insert_like || !g_engine.post) return -1;
    if (n <= 0) return 0;

    int rc = sqlite3_exec(g_engine.db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        for (int retry = 0; retry < 5 && rc != SQLITE_OK; retry++) {
            rc = sqlite3_exec(g_engine.db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        }
        if (rc != SQLITE_OK) return -2;
    }

    int64_t last_post_id = -1;
    int last_post_exists = 0;

    for (int i = 0; i < n; i++) {
        int64_t pid = post_ids[i];
        int64_t uid = user_ids[i];

        // 1. Kiểm tra post tồn tại (tận dụng mảng đã sort theo post_id từ Go)
        if (i == 0 || pid != last_post_id) {
            int res = 0;
            int mrc = sqlite3BtreeTableMoveto(g_engine.post, (sqlite3_int64)pid, 0, &res);
            last_post_id = pid;
            last_post_exists = (mrc == SQLITE_OK && res == 0) ? 1 : 0;
        }

        if (!last_post_exists) {
            results[i] = 404; // Post not found
            continue;
        }

        // 2. Thực thi INSERT INTO likes ON CONFLICT DO NOTHING với now_str đã format sẵn
        sqlite3_bind_int64(g_engine.stmt_insert_like, 1, uid);
        sqlite3_bind_int64(g_engine.stmt_insert_like, 2, pid);
        sqlite3_bind_text(g_engine.stmt_insert_like, 3, now_str, 24, SQLITE_STATIC);

        rc = sqlite3_step(g_engine.stmt_insert_like);
        if (rc == SQLITE_DONE) {
            int changes = sqlite3_changes(g_engine.db);
            results[i] = (changes == 1) ? 201 : 200;
        } else {
            results[i] = 500;
        }

        sqlite3_reset(g_engine.stmt_insert_like);
    }

    sqlite3_exec(g_engine.db, "COMMIT;", NULL, NULL, NULL);
    return 0;
}

void close_feed_engine(void) {
    if (g_engine.stmt_health) sqlite3_finalize(g_engine.stmt_health);
    if (g_engine.stmt_insert_post) sqlite3_finalize(g_engine.stmt_insert_post);
    if (g_engine.stmt_insert_like) sqlite3_finalize(g_engine.stmt_insert_like);
    close_cursor(g_engine.feed);
    close_cursor(g_engine.post);
    close_cursor(g_engine.user);
    close_cursor(g_engine.like);
    if (g_engine.likeFind) sqlite3DbFree(g_engine.db, g_engine.likeFind);
    if (g_engine.feedKey) sqlite3KeyInfoUnref(g_engine.feedKey);
    if (g_engine.likeKey) sqlite3KeyInfoUnref(g_engine.likeKey);
    if (g_engine.db) sqlite3_close(g_engine.db);
    memset(&g_engine, 0, sizeof(g_engine));
}
