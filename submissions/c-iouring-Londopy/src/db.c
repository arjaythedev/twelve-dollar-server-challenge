/* Database layer. This file #includes the SQLite amalgamation so it can use
 * SQLite's internal B-tree API directly: reads position cursors on the
 * posts/users tables and the two indexes and decode records in place, and
 * writes insert into the tables and all four indexes (see "writes" below).
 * That is still SQLite reading and writing its own pages (page cache, WAL and
 * transactions included) on every request; it just skips SQL compilation and
 * the bytecode VM. The only SQL in normal operation is BEGIN IMMEDIATE/COMMIT.
 *
 * Anything unexpected (a record that doesn't decode, a post whose author row
 * is missing) falls back to the reference SQL from SPEC.md. */
#include "sqlite3.c"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "json.h"

#define FEED_N 20

#ifdef PROF
#include <x86intrin.h>
uint64_t db_prof[6]; /* feed stages: index walk, likes walk, rows+JSON, calls, row reads, JSON */
#define DBP(k, t) (db_prof[k] += __rdtsc() - (t))
#define DBP_NOW() __rdtsc()
#else
#define DBP(k, t) ((void)0)
#define DBP_NOW() 0
#endif

static sqlite3 *g_db;
static Btree *g_bt;
static const char *g_path;
static Pgno g_root_posts, g_root_users, g_root_likes_idx, g_root_created_idx;
static KeyInfo *g_ki_created, *g_ki_likes;
static UnpackedRecord *g_ur_likes;
static BtCursor *c_posts, *c_users, *c_likes, *c_created;
static BtCursor *c_wposts, *c_widx; /* used only inside write transactions */
static int g_read_active;
static int g_verify;
/* B-tree writes (see "writes" below) */
static Pgno g_root_likes, g_root_likes_pk, g_root_user_idx;
static KeyInfo *g_ki_user, *g_ki_likes_pk;
static UnpackedRecord *g_ur_pk;
static int g_file_format = 4;
static int g_sql_writes; /* WRITES=sql, or the schema isn't the expected one */
static int g_wpoison;    /* a write failed halfway: this transaction must not commit */

static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_ins_post, *st_post_exists,
    *st_ins_like, *st_sql_post, *st_sql_feed;

static _Atomic uint32_t g_wal_frames;
static _Atomic uint64_t g_commits;
static _Atomic uint64_t g_ckpt_gen;
static uint32_t g_ckpt_start = 2048;

#define SQL_POST_COLS                                                                    \
    "SELECT p.id, p.body, p.created_at, u.username, "                                    \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "                             \
    "FROM posts p JOIN users u ON u.id = p.user_id "

static void die(const char *what) {
    fprintf(stderr, "db: %s: %s\n", what, g_db ? sqlite3_errmsg(g_db) : "?");
    exit(1);
}

static sqlite3_stmt *prep(const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v3(g_db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) die(sql);
    return s;
}

static void exec_or_die(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db: %s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

static int wal_hook_cb(void *ctx, sqlite3 *db, const char *name, int nframe) {
    (void)ctx, (void)db, (void)name;
    atomic_store_explicit(&g_wal_frames, (uint32_t)nframe, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_commits, 1, memory_order_release);
    return SQLITE_OK;
}

int db_open(const char *path) {
    g_path = strdup(path);
    if (sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK)
        die("open");
    exec_or_die(g_db,
                "PRAGMA journal_mode=WAL;"
                "PRAGMA synchronous=NORMAL;"
                "PRAGMA wal_autocheckpoint=0;"
                "PRAGMA temp_store=MEMORY;"
                "PRAGMA mmap_size=0;"
                "PRAGMA busy_timeout=5000;");
    /* A small page cache. The random-access working set (posts, the likes
     * indexes) is ~135 MB, far more than any cache we could afford, so the OS
     * page cache serves the misses either way; a big SQLite cache only holds a
     * second copy of hot pages, in memory the kernel can't reclaim for sockets. */
    const char *ck = getenv("CACHE_KIB");
    long cache_kib = ck && *ck ? strtol(ck, NULL, 10) : 8192;
    if (cache_kib <= 0) cache_kib = 8192;
    if (cache_kib > (1L << 30)) cache_kib = 1L << 30; /* SQLite reads the pragma as a 32-bit int */
    char pragma[64];
    snprintf(pragma, sizeof pragma, "PRAGMA cache_size=-%ld;", cache_kib);
    exec_or_die(g_db, pragma);
    sqlite3_wal_hook(g_db, wal_hook_cb, NULL);

    sqlite3_stmt *s = prep("SELECT name, rootpage FROM sqlite_schema WHERE rootpage > 0");
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(s, 0);
        Pgno r = (Pgno)sqlite3_column_int64(s, 1);
        if (!strcmp(n, "posts")) g_root_posts = r;
        else if (!strcmp(n, "users")) g_root_users = r;
        else if (!strcmp(n, "likes_post_id_idx")) g_root_likes_idx = r;
        else if (!strcmp(n, "posts_created_at_id_idx")) g_root_created_idx = r;
        else if (!strcmp(n, "likes")) g_root_likes = r;
        else if (!strcmp(n, "sqlite_autoindex_likes_1")) g_root_likes_pk = r;
        else if (!strcmp(n, "posts_user_id_idx")) g_root_user_idx = r;
    }
    sqlite3_finalize(s);
    if (!g_root_posts || !g_root_users || !g_root_likes_idx || !g_root_created_idx) die("schema roots");
    const char *wm = getenv("WRITES");
    g_sql_writes = (wm && !strcmp(wm, "sql")) || !g_root_likes || !g_root_likes_pk || !g_root_user_idx;
    if (g_db->aDb[0].pSchema) g_file_format = g_db->aDb[0].pSchema->file_format;
    if (ENC(g_db) != SQLITE_UTF8) g_sql_writes = 1; /* records hold text as UTF-8 bytes */

    g_bt = g_db->aDb[0].pBt;
    /* posts_created_at_id_idx: (created_at DESC, id DESC) + rowid */
    g_ki_created = sqlite3KeyInfoAlloc(g_db, 2, 1);
    g_ki_created->aSortFlags[0] = KEYINFO_ORDER_DESC;
    g_ki_created->aSortFlags[1] = KEYINFO_ORDER_DESC;
    /* likes_post_id_idx: (post_id) + rowid */
    g_ki_likes = sqlite3KeyInfoAlloc(g_db, 1, 1);
    g_ur_likes = sqlite3VdbeAllocUnpackedRecord(g_ki_likes);
    if (!g_ki_created || !g_ki_likes || !g_ur_likes) die("keyinfo");
    memset(g_ur_likes->aMem, 0, sizeof(Mem));
    g_ur_likes->aMem[0].enc = SQLITE_UTF8;
    g_ur_likes->aMem[0].db = g_db;
    /* posts_user_id_idx: (user_id) + rowid; sqlite_autoindex_likes_1: (user_id, post_id) + rowid */
    g_ki_user = sqlite3KeyInfoAlloc(g_db, 1, 1);
    g_ki_likes_pk = sqlite3KeyInfoAlloc(g_db, 2, 1);
    g_ur_pk = g_ki_likes_pk ? sqlite3VdbeAllocUnpackedRecord(g_ki_likes_pk) : NULL;
    if (!g_ki_user || !g_ki_likes_pk || !g_ur_pk) die("keyinfo");
    memset(g_ur_pk->aMem, 0, 2 * sizeof(Mem));
    for (int i = 0; i < 2; i++) {
        g_ur_pk->aMem[i].enc = SQLITE_UTF8;
        g_ur_pk->aMem[i].db = g_db;
    }

    int csz = sqlite3BtreeCursorSize();
    BtCursor **cs[6] = {&c_posts, &c_users, &c_likes, &c_created, &c_wposts, &c_widx};
    for (int i = 0; i < 6; i++) {
        *cs[i] = sqlite3_malloc(csz);
        if (!*cs[i]) die("cursor alloc");
        memset(*cs[i], 0, (size_t)csz);
    }

    st_begin = prep("BEGIN IMMEDIATE");
    st_commit = prep("COMMIT");
    st_rollback = prep("ROLLBACK");
    st_ins_post = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    st_post_exists = prep("SELECT 1 FROM posts WHERE id = ?1");
    st_ins_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) "
                       "ON CONFLICT (user_id, post_id) DO NOTHING");
    st_sql_post = prep(SQL_POST_COLS "WHERE p.id = ?1");
    st_sql_feed = prep(SQL_POST_COLS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    return 0;
}

void db_set_verify(int on) { g_verify = on; }
uint32_t db_wal_frames(void) { return atomic_load_explicit(&g_wal_frames, memory_order_relaxed); }

uint64_t db_cache_misses(void) {
    static uint64_t total;
    int cur = 0, hi = 0;
    sqlite3_db_status(g_db, SQLITE_DBSTATUS_CACHE_MISS, &cur, &hi, 1); /* since the last call */
    return total += (unsigned)cur;
}
uint64_t db_ckpt_generation(void) { return atomic_load_explicit(&g_ckpt_gen, memory_order_acquire); }

/* ---------- checkpointer ---------- */

static void *ckpt_main(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    sqlite3 *cdb;
    if (sqlite3_open_v2(g_path, &cdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) {
        fprintf(stderr, "checkpointer: open failed\n");
        return NULL;
    }
    exec_or_die(cdb, "PRAGMA synchronous=NORMAL; PRAGMA cache_size=-1024; PRAGMA wal_autocheckpoint=0;");
    uint64_t done_at_commit = (uint64_t)-1;
    struct timespec ts = {0, 5 * 1000 * 1000};
    for (;;) {
        nanosleep(&ts, NULL);
        uint64_t commits = atomic_load_explicit(&g_commits, memory_order_acquire);
        if (commits == done_at_commit) continue; /* nothing new since the last full backfill */
        if (atomic_load_explicit(&g_wal_frames, memory_order_relaxed) < g_ckpt_start) continue;
        int nlog = 0, nckpt = 0;
        int rc = sqlite3_wal_checkpoint_v2(cdb, NULL, SQLITE_CHECKPOINT_PASSIVE, &nlog, &nckpt);
        if (rc == SQLITE_OK && nlog >= 0 && nckpt == nlog) {
            done_at_commit = commits;
            atomic_fetch_add_explicit(&g_ckpt_gen, 1, memory_order_release);
        }
    }
    return NULL;
}

void db_start_checkpointer(uint32_t start_frames) {
    g_ckpt_start = start_frames;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, ckpt_main, NULL) != 0) die("checkpointer thread");
}

/* ---------- read transactions ---------- */

static int open_cursor(Pgno root, KeyInfo *ki, BtCursor *c) {
    sqlite3BtreeCursorZero(c);
    return sqlite3BtreeCursor(g_bt, root, 0, ki, c);
}

int db_read_begin(void) {
    if (g_read_active) return 0;
    /* A write transaction left open (a ROLLBACK that failed) must never be
     * committed by db_read_end's sqlite3BtreeCommit: retry the rollback first. */
    if (!sqlite3_get_autocommit(g_db)) {
        sqlite3_step(st_rollback);
        sqlite3_reset(st_rollback);
        if (!sqlite3_get_autocommit(g_db)) return SQLITE_ERROR;
    }
    int rc = sqlite3BtreeBeginTrans(g_bt, 0, 0);
    if (rc != SQLITE_OK) return rc;
    if ((rc = open_cursor(g_root_posts, NULL, c_posts)) ||
        (rc = open_cursor(g_root_users, NULL, c_users)) ||
        (rc = open_cursor(g_root_likes_idx, g_ki_likes, c_likes)) ||
        (rc = open_cursor(g_root_created_idx, g_ki_created, c_created))) {
        sqlite3BtreeCloseCursor(c_posts);
        sqlite3BtreeCloseCursor(c_users);
        sqlite3BtreeCloseCursor(c_likes);
        sqlite3BtreeCloseCursor(c_created);
        sqlite3BtreeCommit(g_bt);
        return rc;
    }
    g_read_active = 1;
    return 0;
}

void db_read_end(void) {
    if (!g_read_active) return;
    sqlite3BtreeCloseCursor(c_posts);
    sqlite3BtreeCloseCursor(c_users);
    sqlite3BtreeCloseCursor(c_likes);
    sqlite3BtreeCloseCursor(c_created);
    sqlite3BtreeCommit(g_bt);
    g_read_active = 0;
}

/* ---------- record decoding ---------- */

typedef struct { u32 t; const u8 *p; } fld;

static inline u32 serial_len(u32 t) {
    static const u8 L[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
    return t >= 12 ? (t - 12) >> 1 : L[t];
}

static inline int serial_int(const fld *f, i64 *v) {
    const u8 *p = f->p;
    switch (f->t) {
    case 1: *v = (int8_t)p[0]; return 1;
    case 2: *v = (int16_t)(p[0] << 8 | p[1]); return 1;
    case 3: *v = (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8) >> 8; return 1;
    case 4: *v = (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]); return 1;
    case 5: {
        i64 hi = (int16_t)(p[0] << 8 | p[1]);
        u32 lo = (u32)p[2] << 24 | (u32)p[3] << 16 | (u32)p[4] << 8 | p[5];
        *v = (i64)((u64)hi << 32 | lo);
        return 1;
    }
    case 6: {
        u64 x = 0;
        for (int i = 0; i < 8; i++) x = x << 8 | p[i];
        *v = (i64)x;
        return 1;
    }
    case 8: *v = 0; return 1;
    case 9: *v = 1; return 1;
    default: return 0;
    }
}

static inline int is_text(const fld *f) { return f->t >= 13 && (f->t & 1); }
static inline u32 text_len(const fld *f) { return (f->t - 13) >> 1; }

/* Decodes up to maxf columns. Returns the count, or -1 if malformed. */
static int rec_decode(const u8 *d, u32 n, fld *f, int maxf) {
    if (n == 0) return -1;
    u32 hdr;
    u32 i = getVarint32(d, hdr);
    if (hdr > n || hdr < i) return -1;
    u32 off = hdr;
    int k = 0;
    while (i < hdr && k < maxf) {
        u32 t;
        i += getVarint32(d + i, t);
        u32 len = serial_len(t);
        if (off + len > n) return -1;
        f[k].t = t;
        f[k].p = d + off;
        off += len;
        k++;
    }
    return k;
}

static u8 g_scratch[3][65536];

static const u8 *payload(BtCursor *c, u32 *n, u8 *scratch) {
    u32 sz = sqlite3BtreePayloadSize(c);
    u32 amt = 0;
    const u8 *p = sqlite3BtreePayloadFetch(c, &amt);
    if (amt >= sz) {
        *n = sz;
        return p;
    }
    if (sz > sizeof g_scratch[0] || sqlite3BtreePayload(c, 0, sz, scratch) != SQLITE_OK) return NULL;
    *n = sz;
    return scratch;
}

/* posts row: (id NULL-alias, user_id, body, created_at) */
typedef struct {
    i64 uid;
    const u8 *body, *created;
    u32 bl, cl;
} post_row;

static int read_post_row(post_row *r) {
    u32 n;
    const u8 *d = payload(c_posts, &n, g_scratch[0]);
    if (!d) return -1;
    fld f[4];
    if (rec_decode(d, n, f, 4) != 4 || !serial_int(&f[1], &r->uid) || !is_text(&f[2]) || !is_text(&f[3]))
        return -1;
    r->body = f[2].p;
    r->bl = text_len(&f[2]);
    r->created = f[3].p;
    r->cl = text_len(&f[3]);
    return 0;
}

/* Returns 1 found (sets name), 0 missing, -1 error. */
static int read_username(i64 uid, const u8 **name, u32 *nl) {
    int res = 0;
    if (sqlite3BtreeTableMoveto(c_users, uid, 0, &res) != SQLITE_OK) return -1;
    if (res != 0 || sqlite3BtreeEof(c_users)) return 0;
    u32 n;
    const u8 *d = payload(c_users, &n, g_scratch[1]);
    if (!d) return -1;
    fld f[2];
    if (rec_decode(d, n, f, 2) != 2 || !is_text(&f[1])) return -1;
    *name = f[1].p;
    *nl = text_len(&f[1]);
    return 1;
}

/* Reads the post_id (first column) of the current likes_post_id_idx entry. */
static inline int likes_entry_post(i64 *pid) {
    u32 n;
    const u8 *d = payload(c_likes, &n, g_scratch[2]);
    if (!d) return -1;
    fld f[1];
    if (rec_decode(d, n, f, 1) != 1 || !serial_int(&f[0], pid)) return -1;
    return 0;
}

/* Positions c_likes on the first entry with post_id >= pid. Returns 0 ok with
 * *eof set when there is none, -1 on error. */
static int likes_seek(i64 pid, int *eof) {
    UnpackedRecord *r = g_ur_likes;
    r->aMem[0].flags = MEM_Int;
    r->aMem[0].u.i = pid;
    r->nField = 1;
    r->default_rc = 1;
    r->eqSeen = 0;
    r->errCode = 0;
    int res = 0;
    if (sqlite3BtreeIndexMoveto(c_likes, r, &res) != SQLITE_OK) return -1;
    *eof = 0;
    if (sqlite3BtreeEof(c_likes)) {
        *eof = 1;
        return 0;
    }
    if (res < 0) {
        int rc = sqlite3BtreeNext(c_likes, 0);
        if (rc == SQLITE_DONE) *eof = 1;
        else if (rc != SQLITE_OK) return -1;
    }
    return 0;
}

static int count_likes_one(i64 pid, u32 *cnt) {
    int eof;
    *cnt = 0;
    if (likes_seek(pid, &eof)) return -1;
    while (!eof) {
        i64 p;
        if (likes_entry_post(&p)) return -1;
        if (p != pid) break;
        (*cnt)++;
        int rc = sqlite3BtreeNext(c_likes, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return -1;
    }
    return 0;
}

/* Counts likes for n ids with one forward walk over the index when the ids
 * are close together (the common case: the newest posts), else per id. */
static int count_likes_many(const i64 *ids, int n, u32 *cnt) {
    i64 lo = ids[0], hi = ids[0];
    for (int i = 0; i < n; i++) {
        cnt[i] = 0;
        if (ids[i] < lo) lo = ids[i];
        if (ids[i] > hi) hi = ids[i];
    }
    if (hi - lo <= 4096) {
        int eof;
        if (likes_seek(lo, &eof)) return -1;
        long scanned = 0;
        while (!eof) {
            i64 p;
            if (likes_entry_post(&p)) return -1;
            if (p > hi) break;
            for (int i = 0; i < n; i++)
                if (ids[i] == p) { cnt[i]++; break; }
            if (++scanned > 100000) goto per_id; /* a pathological span; don't walk forever */
            int rc = sqlite3BtreeNext(c_likes, 0);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_OK) return -1;
        }
        return 0;
    }
per_id:
    for (int i = 0; i < n; i++)
        if (count_likes_one(ids[i], &cnt[i])) return -1;
    return 0;
}

static char *emit_post(char *o, char *oe, i64 id, const post_row *r, const u8 *un, u32 ul, u32 likes) {
    size_t need = 96 + JSON_ESCAPE_MAX((size_t)r->bl + r->cl + ul);
    if ((size_t)(oe - o) < need) return NULL;
    memcpy(o, "{\"id\":", 6);
    o = i64_to_dec(o + 6, id);
    memcpy(o, ",\"body\":\"", 9);
    o = json_escape(o + 9, (const char *)r->body, r->bl);
    memcpy(o, "\",\"created_at\":\"", 16);
    o = json_escape(o + 16, (const char *)r->created, r->cl);
    memcpy(o, "\",\"author\":\"", 12);
    o = json_escape(o + 12, (const char *)un, ul);
    memcpy(o, "\",\"like_count\":", 15);
    o = u64_to_dec(o + 15, likes);
    *o++ = '}';
    return o;
}

/* ---------- reference SQL (fallback and verification) ---------- */

static char *emit_sql_row(char *o, char *oe, sqlite3_stmt *s) {
    post_row r;
    r.body = sqlite3_column_text(s, 1);
    r.bl = (u32)sqlite3_column_bytes(s, 1);
    r.created = sqlite3_column_text(s, 2);
    r.cl = (u32)sqlite3_column_bytes(s, 2);
    const u8 *un = sqlite3_column_text(s, 3);
    u32 ul = (u32)sqlite3_column_bytes(s, 3);
    if (!r.body || !r.created || !un) return NULL;
    return emit_post(o, oe, sqlite3_column_int64(s, 0), &r, un, ul, (u32)sqlite3_column_int64(s, 4));
}

static char *sql_feed(char *o, char *oe) {
    db_read_end();
    sqlite3_stmt *s = st_sql_feed;
    if ((size_t)(oe - o) < 16) return DB_ERR;
    memcpy(o, "{\"posts\":[", 10);
    o += 10;
    int rc, first = 1;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (!first) *o++ = ',';
        first = 0;
        o = emit_sql_row(o, oe - 2, s);
        if (!o) break;
    }
    sqlite3_reset(s);
    if (!o || rc != SQLITE_DONE) return DB_ERR;
    *o++ = ']';
    *o++ = '}';
    return o;
}

static char *sql_post(i64 id, char *o, char *oe) {
    db_read_end();
    sqlite3_stmt *s = st_sql_post;
    sqlite3_bind_int64(s, 1, id);
    int rc = sqlite3_step(s);
    char *r = NULL;
    if (rc == SQLITE_ROW) {
        if ((size_t)(oe - o) < 16) r = DB_ERR;
        else {
            memcpy(o, "{\"post\":", 8);
            r = emit_sql_row(o + 8, oe - 1, s);
            if (r) *r++ = '}';
            else r = DB_ERR;
        }
    } else if (rc != SQLITE_DONE) {
        r = DB_ERR;
    }
    sqlite3_reset(s);
    return r;
}

/* ---------- B-tree reads ---------- */

static char *bt_feed(char *o, char *oe) {
    if (db_read_begin()) return DB_ERR;
    i64 ids[FEED_N];
    u32 likes[FEED_N];
    int n = 0, res = 0, rc;
    uint64_t t0 = DBP_NOW();
    (void)t0;
    if (sqlite3BtreeFirst(c_created, &res) != SQLITE_OK) return NULL;
    while (!res && n < FEED_N) {
        u32 len;
        const u8 *d = payload(c_created, &len, g_scratch[2]);
        fld f[3];
        int k = d ? rec_decode(d, len, f, 3) : -1;
        if (k < 2 || !serial_int(&f[k - 1], &ids[n])) return NULL;
        n++;
        rc = sqlite3BtreeNext(c_created, 0);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_OK) return NULL;
    }
    uint64_t t1 = DBP_NOW();
    (void)t1;
    DBP(0, t0);
    if (n && count_likes_many(ids, n, likes)) return NULL;
    uint64_t t2 = DBP_NOW();
    (void)t2;
    DBP(1, t1);

    if ((size_t)(oe - o) < 16) return NULL;
    memcpy(o, "{\"posts\":[", 10);
    o += 10;
    i64 cur = 0;
    for (int i = 0; i < n; i++) {
        /* consecutive ids: step back one cell instead of a fresh search */
        int found = 0;
        if (i > 0 && ids[i] == cur - 1) {
            rc = sqlite3BtreePrevious(c_posts, 0);
            if (rc == SQLITE_OK && sqlite3BtreeIntegerKey(c_posts) == ids[i]) found = 1;
            else if (rc != SQLITE_OK && rc != SQLITE_DONE) return NULL;
        }
        if (!found) {
            if (sqlite3BtreeTableMoveto(c_posts, ids[i], 0, &res) != SQLITE_OK) return NULL;
            if (res != 0 || sqlite3BtreeEof(c_posts)) return NULL; /* index points at a missing row */
        }
        cur = ids[i];
        post_row r;
        const u8 *un;
        u32 ul;
        uint64_t tu = DBP_NOW();
        (void)tu;
        if (read_post_row(&r) || read_username(r.uid, &un, &ul) != 1) return NULL;
        uint64_t te = DBP_NOW();
        (void)te;
        DBP(4, tu);
        if (i) *o++ = ',';
        o = emit_post(o, oe - 2, ids[i], &r, un, ul, likes[i]);
        DBP(5, te);
        if (!o) return NULL;
    }
    *o++ = ']';
    *o++ = '}';
    DBP(2, t2);
#ifdef PROF
    db_prof[3]++;
#endif
    return o;
}

/* Returns end, NULL if the post doesn't exist, (char*)1 to request the SQL path. */
#define BT_FALLBACK ((char *)1)
static char *bt_post(i64 id, char *o, char *oe) {
    if (db_read_begin()) return DB_ERR;
    int res = 0;
    if (sqlite3BtreeTableMoveto(c_posts, id, 0, &res) != SQLITE_OK) return BT_FALLBACK;
    if (res != 0 || sqlite3BtreeEof(c_posts)) return NULL;
    post_row r;
    const u8 *un;
    u32 ul, cnt;
    if (read_post_row(&r)) return BT_FALLBACK;
    int u = read_username(r.uid, &un, &ul);
    if (u < 0) return BT_FALLBACK;
    if (u == 0) return NULL; /* inner join: no author row, no post */
    if (count_likes_one(id, &cnt)) return BT_FALLBACK;
    if ((size_t)(oe - o) < 16) return BT_FALLBACK;
    memcpy(o, "{\"post\":", 8);
    o = emit_post(o + 8, oe - 1, id, &r, un, ul, cnt);
    if (!o) return BT_FALLBACK;
    *o++ = '}';
    return o;
}

static void verify_report(const char *what, const char *a, long al, const char *b, long bl) {
    fprintf(stderr, "VERIFY MISMATCH %s\n btree: %.*s\n sql:   %.*s\n", what, (int)al, a, (int)bl, b);
}

static char g_vbuf[1 << 20];

char *db_feed_json(char *o, char *oe) {
    char *e = bt_feed(o, oe);
    if (e == DB_ERR) return DB_ERR;
    if (!e) return sql_feed(o, oe);
    if (g_verify) {
        char *v = sql_feed(g_vbuf, g_vbuf + sizeof g_vbuf);
        if (v != DB_ERR && (v - g_vbuf != e - o || memcmp(g_vbuf, o, (size_t)(e - o))))
            verify_report("feed", o, e - o, g_vbuf, v - g_vbuf);
    }
    return e;
}

char *db_post_json(int64_t id, char *o, char *oe) {
    char *e = bt_post(id, o, oe);
    if (e == BT_FALLBACK) return sql_post(id, o, oe);
    if (g_verify && e != DB_ERR) {
        char *v = sql_post(id, g_vbuf, g_vbuf + sizeof g_vbuf);
        long el = e ? e - o : 0, vl = (v && v != DB_ERR) ? v - g_vbuf : 0;
        if (v != DB_ERR && (el != vl || memcmp(g_vbuf, o, (size_t)el)))
            verify_report("post", o, el, g_vbuf, vl);
    }
    return e;
}

/* ---------- health and writes ---------- */

/* No SQL here either: join the pass's read transaction and read the users
 * table's first page through a cursor, which fails if the file can't be read. */
int db_health(char *err, size_t cap) {
    int rc = db_read_begin(), empty = 0;
    if (rc == SQLITE_OK) rc = sqlite3BtreeFirst(c_users, &empty);
    if (rc == SQLITE_OK) return 0;
    snprintf(err, cap, "%s", sqlite3ErrStr(rc));
    return -1;
}

static int step_once(sqlite3_stmt *s) {
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc;
}

int db_write_begin(void) {
    db_read_end();
    g_wpoison = 0;
    return step_once(st_begin) == SQLITE_DONE ? 0 : -1;
}

/* A write that failed halfway left the transaction inconsistent: refuse to
 * commit it, so the caller rolls the whole batch back. */
int db_write_commit(void) {
    if (g_wpoison) return -1;
    return step_once(st_commit) == SQLITE_DONE ? 0 : -1;
}

void db_write_rollback(void) {
    if (!sqlite3_get_autocommit(g_db)) step_once(st_rollback);
}

/* ---------- writes, straight into the B-tree ----------
 *
 * Writes don't run SQL either. Each one puts the row into its table and an
 * entry into every index on that table, which is all INSERT does for this
 * schema (no triggers; foreign keys are off, as SQLite's default):
 *   posts (id INTEGER PRIMARY KEY = rowid, user_id, body, created_at)
 *     posts_created_at_id_idx  (created_at DESC, id DESC) + rowid
 *     posts_user_id_idx        (user_id) + rowid
 *   likes (rowid; user_id, post_id, created_at)
 *     sqlite_autoindex_likes_1 (user_id, post_id) + rowid, unique on the pair
 *     likes_post_id_idx        (post_id) + rowid
 * Records use SQLite's format, smallest integer encodings included, so the file
 * is what INSERT would have written. A new rowid is the largest plus one, as
 * SQLite assigns it without AUTOINCREMENT, and created_at is the column default,
 * strftime('%Y-%m-%dT%H:%M:%fZ', 'now').
 *
 * Every constraint is checked before anything changes: the body's length in the
 * server (which also requires valid UTF-8 without NUL, where its character count
 * equals SQLite's length() in the CHECK), the post's existence and the
 * (user, post) uniqueness here. So a write
 * can only fail halfway on an I/O or memory error, and then g_wpoison makes the
 * batch's commit fail: everything rolls back and every write in it gets a 500.
 * WRITES=sql switches back to the reference INSERT statements. */

typedef struct {
    int kind; /* 0 NULL, 1 integer, 2 text */
    i64 i;
    const void *s;
    u32 n;
} wval;

static u32 int_serial(i64 v, u32 *len) {
    if (g_file_format >= 4 && (v == 0 || v == 1)) {
        *len = 0;
        return v == 0 ? 8 : 9;
    }
    u64 u = v < 0 ? ~(u64)v : (u64)v;
    if (u <= 127) return *len = 1, 1;
    if (u <= 32767) return *len = 2, 2;
    if (u <= 8388607) return *len = 3, 3;
    if (u <= 2147483647) return *len = 4, 4;
    if (u <= 0x7fffffffffffULL) return *len = 6, 5;
    return *len = 8, 6;
}

/* Encodes values as a record: a header (its own size, then a serial type per
 * value) followed by the values. Returns the length, 0 if it doesn't fit. */
static u32 rec_build(u8 *out, u32 cap, const wval *v, int n) {
    u32 type[6], len[6], hdr = 1, body = 0;
    for (int k = 0; k < n; k++) {
        if (v[k].kind == 0) type[k] = 0, len[k] = 0;
        else if (v[k].kind == 1) type[k] = int_serial(v[k].i, &len[k]);
        else type[k] = v[k].n * 2 + 13, len[k] = v[k].n;
        hdr += (u32)sqlite3VarintLen(type[k]);
        body += len[k];
    }
    if (hdr > 127 || hdr + body > cap) return 0; /* the header size must fit one byte */
    u8 *p = out;
    *p++ = (u8)hdr;
    for (int k = 0; k < n; k++) p += sqlite3PutVarint(p, type[k]);
    for (int k = 0; k < n; k++) {
        if (v[k].kind == 1) {
            u64 x = (u64)v[k].i;
            for (int b = (int)len[k] - 1; b >= 0; b--, x >>= 8) p[b] = (u8)x;
            p += len[k];
        } else if (v[k].kind == 2) {
            memcpy(p, v[k].s, v[k].n);
            p += v[k].n;
        }
    }
    return (u32)(p - out);
}

/* strftime('%Y-%m-%dT%H:%M:%fZ', 'now'): UTC, milliseconds truncated like SQLite's clock. */
static u32 now_text(char *o) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t s = ts.tv_sec;
    struct tm tm;
    gmtime_r(&s, &tm);
    return (u32)snprintf(o, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

static int put_row(BtCursor *c, i64 rowid, const u8 *rec, u32 n) {
    BtreePayload x;
    memset(&x, 0, sizeof x);
    x.nKey = rowid;
    x.pData = rec;
    x.nData = (int)n;
    return sqlite3BtreeInsert(c, &x, BTREE_APPEND, 0);
}

/* Inserts one index entry, opening and closing a cursor on that index. */
static int put_index(Pgno root, KeyInfo *ki, const wval *v, int n) {
    u8 rec[64];
    u32 rl = rec_build(rec, sizeof rec, v, n);
    if (!rl) return SQLITE_TOOBIG;
    sqlite3BtreeCursorZero(c_widx);
    int rc = sqlite3BtreeCursor(g_bt, root, BTREE_WRCSR, ki, c_widx);
    if (rc != SQLITE_OK) return rc;
    BtreePayload x;
    memset(&x, 0, sizeof x);
    x.pKey = rec;
    x.nKey = rl;
    rc = sqlite3BtreeInsert(c_widx, &x, 0, 0);
    sqlite3BtreeCloseCursor(c_widx);
    return rc;
}

/* Opens a write cursor on a table and finds the next rowid (largest + 1). */
static int open_for_append(Pgno root, i64 *rowid) {
    sqlite3BtreeCursorZero(c_wposts);
    int rc = sqlite3BtreeCursor(g_bt, root, BTREE_WRCSR, NULL, c_wposts);
    if (rc != SQLITE_OK) return rc;
    int empty = 0;
    rc = sqlite3BtreeLast(c_wposts, &empty);
    if (rc == SQLITE_OK) {
        i64 last = empty ? 0 : sqlite3BtreeIntegerKey(c_wposts);
        if (last >= LARGEST_INT64) rc = SQLITE_FULL; /* SQLite would look for a free random rowid */
        else *rowid = last + 1;
    }
    if (rc != SQLITE_OK) sqlite3BtreeCloseCursor(c_wposts);
    return rc;
}

static int bt_insert_post(i64 uid, const char *body, size_t n, i64 *id, char *created, size_t *clen) {
    if (g_wpoison) return -1; /* the transaction is already lost */
    char ts[32];
    u32 tl = now_text(ts);
    /* The record doesn't depend on the rowid (id is the rowid, stored as NULL),
     * so build it first: a body that doesn't fit fails before anything changes. */
    static u8 rec[4096];
    wval row[4] = {{0, 0, 0, 0}, {1, uid, 0, 0}, {2, 0, body, (u32)n}, {2, 0, ts, tl}};
    u32 rl = rec_build(rec, sizeof rec, row, 4);
    i64 rowid;
    if (!rl || open_for_append(g_root_posts, &rowid) != SQLITE_OK) return -1; /* nothing changed yet */
    int rc = put_row(c_wposts, rowid, rec, rl);
    sqlite3BtreeCloseCursor(c_wposts);
    if (rc == SQLITE_OK) {
        wval k1[3] = {{2, 0, ts, tl}, {1, rowid, 0, 0}, {1, rowid, 0, 0}};
        rc = put_index(g_root_created_idx, g_ki_created, k1, 3);
    }
    if (rc == SQLITE_OK) {
        wval k2[2] = {{1, uid, 0, 0}, {1, rowid, 0, 0}};
        rc = put_index(g_root_user_idx, g_ki_user, k2, 2);
    }
    if (rc != SQLITE_OK) {
        g_wpoison = 1;
        return -1;
    }
    *id = rowid;
    memcpy(created, ts, tl);
    *clen = tl;
    return 0;
}

/* 1 inserted, 0 the user already liked it, -2 error. */
static int bt_insert_like(i64 uid, i64 pid) {
    if (g_wpoison) return -2; /* the transaction is already lost */
    /* uniqueness first: an entry with this (user_id, post_id) prefix */
    sqlite3BtreeCursorZero(c_widx);
    if (sqlite3BtreeCursor(g_bt, g_root_likes_pk, BTREE_WRCSR, g_ki_likes_pk, c_widx) != SQLITE_OK) return -2;
    UnpackedRecord *r = g_ur_pk;
    r->aMem[0].flags = MEM_Int;
    r->aMem[0].u.i = uid;
    r->aMem[1].flags = MEM_Int;
    r->aMem[1].u.i = pid;
    r->nField = 2;
    r->default_rc = 0; /* a matching prefix compares equal */
    r->eqSeen = 0;
    r->errCode = 0;
    int res = 0, rc = sqlite3BtreeIndexMoveto(c_widx, r, &res);
    int exists = rc == SQLITE_OK && res == 0 && !sqlite3BtreeEof(c_widx);
    sqlite3BtreeCloseCursor(c_widx);
    if (rc != SQLITE_OK) return -2;
    if (exists) return 0;

    char ts[32];
    u32 tl = now_text(ts);
    u8 rec[64];
    wval row[3] = {{1, uid, 0, 0}, {1, pid, 0, 0}, {2, 0, ts, tl}};
    u32 rl = rec_build(rec, sizeof rec, row, 3);
    i64 rowid;
    if (!rl || open_for_append(g_root_likes, &rowid) != SQLITE_OK) return -2; /* nothing changed yet */
    rc = put_row(c_wposts, rowid, rec, rl);
    sqlite3BtreeCloseCursor(c_wposts);
    if (rc == SQLITE_OK) {
        wval k1[3] = {{1, uid, 0, 0}, {1, pid, 0, 0}, {1, rowid, 0, 0}};
        rc = put_index(g_root_likes_pk, g_ki_likes_pk, k1, 3);
    }
    if (rc == SQLITE_OK) {
        wval k2[2] = {{1, pid, 0, 0}, {1, rowid, 0, 0}};
        rc = put_index(g_root_likes_idx, g_ki_likes, k2, 2);
    }
    if (rc != SQLITE_OK) {
        g_wpoison = 1;
        return -2;
    }
    return 1;
}

int db_insert_post(int64_t uid, const char *body, size_t n, int64_t *id, char *created, size_t *clen) {
    if (!g_sql_writes) {
        i64 rid = 0;
        int r = bt_insert_post(uid, body, n, &rid, created, clen);
        *id = rid;
        return r;
    }
    sqlite3_stmt *s = st_ins_post;
    sqlite3_bind_int64(s, 1, uid);
    sqlite3_bind_text(s, 2, body, (int)n, SQLITE_STATIC);
    int rc = sqlite3_step(s), ret = -1;
    if (rc == SQLITE_ROW) {
        *id = sqlite3_column_int64(s, 0);
        const unsigned char *c = sqlite3_column_text(s, 1);
        int cl = sqlite3_column_bytes(s, 1);
        if (c && cl > 0 && cl < 32) {
            memcpy(created, c, (size_t)cl);
            *clen = (size_t)cl;
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE) ret = 0;
        }
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return ret;
}

/* Inside the write transaction: does post pid exist? A raw cursor sees rows
 * this same transaction inserted. 1 yes, 0 no, -1 error. */
static int post_exists_bt(i64 pid) {
    sqlite3BtreeCursorZero(c_wposts);
    if (sqlite3BtreeCursor(g_bt, g_root_posts, 0, NULL, c_wposts) != SQLITE_OK) return -1;
    int res = 0, rc = sqlite3BtreeTableMoveto(c_wposts, pid, 0, &res);
    int found = rc == SQLITE_OK && res == 0 && !sqlite3BtreeEof(c_wposts);
    sqlite3BtreeCloseCursor(c_wposts);
    return rc == SQLITE_OK ? found : -1;
}

int db_insert_like(int64_t uid, int64_t pid) {
    int ex = post_exists_bt(pid);
    if (ex < 0) { /* fall back to SQL */
        sqlite3_bind_int64(st_post_exists, 1, pid);
        int r = step_once(st_post_exists);
        ex = r == SQLITE_ROW ? 1 : r == SQLITE_DONE ? 0 : -1;
    }
    if (ex == 0) return -1;
    if (ex < 0) return -2;
    if (!g_sql_writes) return bt_insert_like(uid, pid);
    int rc;
    sqlite3_stmt *s = st_ins_like;
    sqlite3_bind_int64(s, 1, uid);
    sqlite3_bind_int64(s, 2, pid);
    rc = step_once(s);
    if (rc != SQLITE_DONE) return -2;
    return sqlite3_changes(g_db) > 0 ? 1 : 0;
}
