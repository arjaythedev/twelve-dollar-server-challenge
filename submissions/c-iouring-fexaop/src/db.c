#define _GNU_SOURCE
/* SQLite side: the unmodified amalgamation plus read plans over its B-tree cursors.
 *
 * Credits: the SQLite open/pragmas, per-pass transaction, group commit, prepared write statements
 * and the checkpoint/bulk-hold state machine are a C adaptation of rust-iouring-kensac #264's
 * shim.c (MIT), via this author's rust-iouring-fexaop entry; credited in README.md. The record
 * decoder, cursor steps, index seek and batched rowid descent are adapted from
 * c-iouring-pavlyhalim #245 (MIT), credited there as well. Writes, BEGIN/COMMIT and /health use
 * ordinary SQL. The checkpoint thread is the only checkpointer; the event loop never checkpoints.
 *
 * This file is compiled as its own translation unit and includes the amalgamation, so the
 * internal sqlite3Btree* entry points are visible. */
#include "sqlite3.c"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "db.h"

typedef unsigned char u8;
typedef long long i64;

static sqlite3 *db, *ckdb;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health, *st_feed;
static Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
static KeyInfo *ki_feed, *ki_likes;
static BtCursor *cur[4]; /* posts, users, posts_created_at_id_idx, likes_post_id_idx */
static int cur_open, in_tx, batch_broken;
static u8 *spill;
static u32 spill_cap;

/* posts.created_at never decreases with rowid: /feed is the table's last 20 rows. */
static int posts_ascending;
static char last_created[32];
static int last_created_n;

/* Checkpoints (see README): the thread is the only checkpointer. Past CKPT_FRAMES a bulk PASSIVE
 * checkpoint copies the log back; then writes are held (reads continue) while the thread copies
 * the frames written meanwhile, so the next write restarts the log. A hold lasts at most
 * HOLD_MAX_MS. Keeping the log short matters: the wal-index hash is merged when a new frame
 * segment is appended, so an ever-growing log makes every commit more expensive. */
#define CKPT_FRAMES 4096
#define HOLD_MAX_MS 250
enum { CK_IDLE, CK_BULK, CK_TAIL };
static _Atomic int wal_frames;
static _Atomic int ck_state;
static _Atomic int ck_wanted;
static _Atomic int ck_done;
static _Atomic int ck_full;
static _Atomic int writes_held;
static int ck_stop;
static struct timespec hold_t0;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static pthread_t ck_thread;

static int fail(char *err, int n, const char *what, sqlite3 *c) {
    snprintf(err, (size_t)n, "%s: %s", what, c ? sqlite3_errmsg(c) : "error");
    return -1;
}

/* ---- checkpoint thread ---------------------------------------------------------------- */

static void ck_signal(void) {
    pthread_mutex_lock(&ck_mu);
    atomic_store_explicit(&ck_wanted, 1, memory_order_relaxed);
    pthread_cond_signal(&ck_cv);
    pthread_mutex_unlock(&ck_mu);
}

static void *ck_main(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    for (;;) {
        pthread_mutex_lock(&ck_mu);
        while (!atomic_load_explicit(&ck_wanted, memory_order_acquire) && !ck_stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&ck_cv, &ck_mu, &ts);
        }
        if (ck_stop) {
            pthread_mutex_unlock(&ck_mu);
            return NULL;
        }
        atomic_store_explicit(&ck_wanted, 0, memory_order_relaxed);
        pthread_mutex_unlock(&ck_mu);
        if (atomic_load_explicit(&ck_state, memory_order_acquire) == CK_IDLE) continue;
        int nlog = 0, ndone = 0;
        int rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
        atomic_store_explicit(&ck_full, rc == SQLITE_OK && nlog == ndone, memory_order_relaxed);
        atomic_store_explicit(&ck_done, 1, memory_order_release);
    }
}

static int ck_start(const char *path) {
    if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl"))
        return -1;
    /* A connection that never ran a statement has not opened the WAL; a checkpoint on it is a
     * no-op. One read opens it. */
    if (sqlite3_exec(ckdb, "PRAGMA cache_size=-1024; SELECT count(*) FROM sqlite_schema", NULL, NULL, NULL))
        return -1;
    if (pthread_create(&ck_thread, NULL, ck_main, NULL)) return -1;
    return 0;
}

static int wal_hook(void *arg, sqlite3 *c, const char *name, int frames) {
    (void)arg;
    (void)c;
    (void)name;
    atomic_store_explicit(&wal_frames, frames, memory_order_relaxed);
    return SQLITE_OK;
}

static long ms_since(const struct timespec *t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

void db_tick(void) {
    /* Between passes, with no transaction open. The loop never checkpoints. */
    int st = atomic_load_explicit(&ck_state, memory_order_acquire);
    if (st == CK_IDLE) {
        if (atomic_load_explicit(&wal_frames, memory_order_relaxed) >= CKPT_FRAMES) {
            atomic_store_explicit(&ck_done, 0, memory_order_relaxed);
            atomic_store_explicit(&ck_state, CK_BULK, memory_order_release);
            ck_signal();
        }
    } else if (st == CK_BULK) {
        if (atomic_load_explicit(&ck_done, memory_order_acquire)) {
            atomic_store_explicit(&ck_done, 0, memory_order_relaxed);
            clock_gettime(CLOCK_MONOTONIC, &hold_t0);
            atomic_store_explicit(&writes_held, 1, memory_order_release);
            atomic_store_explicit(&ck_state, CK_TAIL, memory_order_release);
            ck_signal();
        }
    } else {
        if (atomic_load_explicit(&ck_done, memory_order_acquire)) {
            atomic_store_explicit(&ck_done, 0, memory_order_relaxed);
            if (atomic_load_explicit(&ck_full, memory_order_relaxed))
                atomic_store_explicit(&wal_frames, 0, memory_order_relaxed);
            atomic_store_explicit(&writes_held, 0, memory_order_release);
            atomic_store_explicit(&ck_state, CK_IDLE, memory_order_release);
        } else if (atomic_load_explicit(&writes_held, memory_order_relaxed) &&
                   ms_since(&hold_t0) > HOLD_MAX_MS) {
            atomic_store_explicit(&writes_held, 0, memory_order_release);
        }
    }
}

int db_writes_held(void) { return atomic_load_explicit(&writes_held, memory_order_acquire); }

/* ---- schema helpers ------------------------------------------------------------------- */

static Pgno rootpage(sqlite3 *c, const char *name) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(c, "SELECT rootpage FROM sqlite_schema WHERE name=?1", -1, &s, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    Pgno r = sqlite3_step(s) == SQLITE_ROW ? (Pgno)sqlite3_column_int64(s, 0) : 0;
    sqlite3_finalize(s);
    return r;
}

static int table_layout(sqlite3 *c, const char *table, const char *expect) {
    char sql[128], got[512];
    snprintf(sql, sizeof sql, "PRAGMA table_info(\"%s\")", table);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(c, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    size_t n = 0;
    got[0] = 0;
    while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got)
        n += (size_t)snprintf(got + n, sizeof got - n, "%s%s:%s:%d", n ? "," : "",
                              sqlite3_column_text(s, 1), sqlite3_column_text(s, 2),
                              sqlite3_column_int(s, 5));
    sqlite3_finalize(s);
    return strcmp(got, expect) == 0;
}

static int index_layout(sqlite3 *c, const char *index, const char *expect) {
    char sql[128], got[256];
    snprintf(sql, sizeof sql, "PRAGMA index_xinfo(\"%s\")", index);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(c, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    size_t n = 0;
    got[0] = 0;
    while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got) {
        if (!sqlite3_column_int(s, 5)) continue; /* key columns only */
        n += (size_t)snprintf(got + n, sizeof got - n, "%s%s:%d", n ? "," : "",
                              sqlite3_column_text(s, 2), sqlite3_column_int(s, 3));
    }
    sqlite3_finalize(s);
    return strcmp(got, expect) == 0;
}

static KeyInfo *make_keyinfo(sqlite3 *c, int nkey, int nextra, int desc) {
    KeyInfo *k = sqlite3KeyInfoAlloc(c, nkey, nextra);
    if (!k) return NULL;
    for (int i = 0; i < nkey + nextra; i++) {
        k->aColl[i] = c->pDfltColl;
        if (desc && i < nkey) k->aSortFlags[i] = KEYINFO_ORDER_DESC;
    }
    return k;
}

/* ---- records (https://sqlite.org/fileformat2.html#record_format) ----------------------- */

struct rec {
    const u8 *p;
    u32 n;
};
struct col {
    u64 t;
    const u8 *p;
};

static int record(BtCursor *c, struct rec *r) {
    u32 total = sqlite3BtreePayloadSize(c), local;
    const u8 *p = sqlite3BtreePayloadFetch(c, &local);
    if (local == total) {
        r->p = p;
        r->n = total;
        return 0;
    }
    if (total > spill_cap) {
        free(spill);
        spill_cap = total + 4096;
        spill = malloc(spill_cap);
        if (!spill) return -1;
    }
    if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return -1;
    r->p = spill;
    r->n = total;
    return 0;
}

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

static inline int rec_cols(const struct rec *r, int ncol, struct col *c) {
    const u8 *end = r->p + r->n;
    u64 hsz;
    const u8 *h = rd_varint(r->p, end, &hsz);
    if (!h || hsz > r->n) return -1;
    const u8 *hend = r->p + hsz, *body = hend;
    for (int i = 0; i < ncol; i++) {
        if (h >= hend) return -1;
        h = rd_varint(h, hend, &c[i].t);
        if (!h) return -1;
        c[i].p = body;
        body += serial_len(c[i].t);
    }
    return body <= end ? 0 : -1;
}

static inline int col_is_int(const struct col *c) { return (c->t >= 1 && c->t <= 6) || c->t == 8 || c->t == 9; }
static inline int col_is_text(const struct col *c) { return c->t >= 13 && (c->t & 1); }
static inline u32 col_len(const struct col *c) { return serial_len(c->t); }

static inline i64 col_int(const struct col *c) {
    const u8 *p = c->p;
    switch (c->t) {
    case 1: return (int8_t)p[0];
    case 2: return (int16_t)((u16)p[0] << 8 | p[1]);
    case 3: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8) >> 8;
    case 4: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
    case 5: return (i64)((u64)(int16_t)((u16)p[0] << 8 | p[1]) << 32 |
                         (u64)((u32)p[2] << 24 | (u32)p[3] << 16 | (u32)p[4] << 8 | p[5]));
    case 6: {
        u64 v = 0;
        for (int i = 0; i < 8; i++) v = v << 8 | p[i];
        return (i64)v;
    }
    case 8: return 0;
    case 9: return 1;
    default: return 0;
    }
}

/* ---- cursor steps: 0 on a row, 1 past the end, -1 error -------------------------------- */

static inline int cur_first(BtCursor *c) {
    int empty;
    return sqlite3BtreeFirst(c, &empty) != SQLITE_OK ? -1 : empty ? 1 : 0;
}
static inline int cur_next(BtCursor *c) {
    int rc = sqlite3BtreeNext(c, 0);
    return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static inline int cur_last(BtCursor *c) {
    int empty;
    return sqlite3BtreeLast(c, &empty) != SQLITE_OK ? -1 : empty ? 1 : 0;
}
static inline int cur_prev(BtCursor *c) {
    int rc = sqlite3BtreePrevious(c, 0);
    return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static inline int cur_seek(BtCursor *c, i64 rowid) {
    int res;
    if (sqlite3BtreeTableMoveto(c, rowid, 0, &res) != SQLITE_OK) return -1;
    return res == 0 ? 0 : 1;
}
/* First index entry whose leading integer key is >= k. */
static int idx_seek_ge(BtCursor *c, KeyInfo *ki, i64 k) {
    Mem m;
    memset(&m, 0, sizeof m);
    m.flags = MEM_Int;
    m.u.i = k;
    UnpackedRecord r;
    memset(&r, 0, sizeof r);
    r.pKeyInfo = ki;
    r.aMem = &m;
    r.nField = 1;
    r.default_rc = +1;
    int res;
    if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
    if (res < 0) return cur_next(c);
    return sqlite3BtreeEof(c) ? 1 : 0;
}

/* Likes per post for lo <= post_id <= hi, one range walk. counts must have hi-lo+1 slots. */
static int range_counts(i64 lo, i64 hi, i64 *counts) {
    memset(counts, 0, sizeof(i64) * (size_t)(hi - lo + 1));
    int rc = idx_seek_ge(cur[3], ki_likes, lo);
    while (rc == 0) {
        struct rec r;
        struct col c[1];
        if (record(cur[3], &r) || rec_cols(&r, 1, c) || !col_is_int(&c[0])) return -1;
        i64 pid = col_int(&c[0]);
        if (pid > hi) return 0;
        if (pid >= lo) counts[pid - lo]++;
        rc = cur_next(cur[3]);
    }
    return rc < 0 ? -1 : 0;
}

/* ---- transactions and cursors --------------------------------------------------------- */

static void close_cursors(void) {
    if (!cur_open) return;
    for (int i = 0; i < 4; i++) sqlite3BtreeCloseCursor(cur[i]);
    cur_open = 0;
}

static int open_cursors(void) {
    if (cur_open) return 0;
    Pgno r[4] = {root_posts, root_users, root_feed_idx, root_likes_idx};
    KeyInfo *kis[4] = {0, 0, ki_feed, ki_likes};
    for (int i = 0; i < 4; i++) {
        sqlite3BtreeCursorZero(cur[i]);
        if (sqlite3BtreeCursor(bt, r[i], 0, kis[i], cur[i]) != SQLITE_OK) {
            for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
            return -1;
        }
    }
    cur_open = 1;
    return 0;
}

int db_begin(void) {
    if (batch_broken) return -1;
    if (in_tx) {
        if (!sqlite3_get_autocommit(db)) return 0;
        in_tx = 0;
        batch_broken = 1;
        return -1;
    }
    if (sqlite3_step(st_begin) != SQLITE_DONE) {
        sqlite3_reset(st_begin);
        return -1;
    }
    sqlite3_reset(st_begin);
    if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK || open_cursors() != 0) {
        sqlite3_step(st_rollback);
        sqlite3_reset(st_rollback);
        return -1;
    }
    in_tx = 1;
    return 0;
}

int db_commit(void) {
    if (batch_broken) {
        batch_broken = 0;
        close_cursors();
        if (!sqlite3_get_autocommit(db)) {
            sqlite3_step(st_rollback);
            sqlite3_reset(st_rollback);
        }
        return -1;
    }
    if (!in_tx) return 0;
    in_tx = 0;
    close_cursors();
    if (sqlite3_step(st_commit) != SQLITE_DONE) {
        sqlite3_reset(st_commit);
        sqlite3_step(st_rollback);
        sqlite3_reset(st_rollback);
        return -1;
    }
    sqlite3_reset(st_commit);
    return 0;
}

static void check_tx(void) {
    if (in_tx && sqlite3_get_autocommit(db)) {
        in_tx = 0;
        batch_broken = 1;
    }
}

/* ---- arena ---------------------------------------------------------------------------- */

static int put(char **ap, char *aend, const u8 *p, u32 n, const char **out, int *outn) {
    if (n > (size_t)(aend - *ap)) return -1;
    memcpy(*ap, p, n);
    *out = *ap;
    *outn = (int)n;
    *ap += n;
    return 0;
}

/* ---- feed ----------------------------------------------------------------------------- */

static int fill_tail(struct row *rows, char **ap, char *aend) {
    int n = 0;
    int rc = cur_last(cur[0]);
    if (rc < 0) return -1;
    while (rc == 0 && n < FEED_N) {
        struct rec r;
        struct col c[4];
        if (record(cur[0], &r) || rec_cols(&r, 4, c)) return -1;
        rows[n].id = sqlite3BtreeIntegerKey(cur[0]);
        rows[n].user_id = col_is_int(&c[1]) ? col_int(&c[1]) : -1;
        if (rows[n].user_id <= 0 || !col_is_text(&c[2]) || !col_is_text(&c[3])) return -1;
        if (put(ap, aend, c[2].p, col_len(&c[2]), &rows[n].body, &rows[n].body_len)) return -1;
        if (put(ap, aend, c[3].p, col_len(&c[3]), &rows[n].created, &rows[n].created_len)) return -1;
        rows[n].author = NULL;
        rows[n].author_len = 0;
        rows[n].likes = 0;
        n++;
        rc = cur_prev(cur[0]);
        if (rc == 1) break;
        if (rc < 0) return -1;
    }
    return n == FEED_N ? 0 : -1;
}

static int fill_index(struct row *rows, char **ap, char *aend) {
    int n = 0;
    int rc = cur_first(cur[2]);
    if (rc < 0) return -1;
    while (rc == 0 && n < FEED_N) {
        struct rec r;
        struct col c[3];
        if (record(cur[2], &r) || rec_cols(&r, 3, c)) return -1;
        if (!col_is_text(&c[0]) || !col_is_int(&c[1])) return -1;
        rows[n].id = col_int(&c[1]);
        if (put(ap, aend, c[0].p, col_len(&c[0]), &rows[n].created, &rows[n].created_len)) return -1;
        rows[n].body = NULL;
        rows[n].body_len = 0;
        rows[n].author = NULL;
        rows[n].author_len = 0;
        rows[n].likes = 0;
        n++;
        rc = cur_next(cur[2]);
    }
    if (rc < 0 || n != FEED_N) return -1;
    /* The posts, in ascending id order so consecutive rows share leaves when they can. */
    int order[FEED_N];
    for (int i = 0; i < FEED_N; i++) order[i] = FEED_N - 1 - i;
    for (int a = 1; a < FEED_N; a++) {
        int v = order[a], b = a - 1;
        while (b >= 0 && rows[order[b]].id > rows[v].id) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = v;
    }
    for (int k = 0; k < FEED_N; k++) {
        int i = order[k];
        if (cur_seek(cur[0], rows[i].id) != 0) return -1;
        struct rec r;
        struct col c[4];
        if (record(cur[0], &r) || rec_cols(&r, 4, c)) return -1;
        if (!col_is_int(&c[1]) || !col_is_text(&c[2])) return -1;
        rows[i].user_id = col_int(&c[1]);
        if (put(ap, aend, c[2].p, col_len(&c[2]), &rows[i].body, &rows[i].body_len)) return -1;
    }
    return 0;
}

/* ---- batched rowid descent for the feed's 20 authors ----
 * The same descent as sqlite3BtreeTableMoveto, through the same pager calls, but for all 20 keys
 * at one level at a time: keys on the same page share one fetch, and the pages are prefetched.
 * Adapted from c-iouring-pavlyhalim #245's helper (MIT, credited in README.md). */
struct bt_hit {
    MemPage *pg;
    int ix;
};

static inline i64 bt_cell_key(MemPage *pg, int i) {
    u8 *c = findCellPastPtr(pg, i);
    if (pg->intKeyLeaf)
        while (0x80 <= *(c++))
            if (c >= pg->aDataEnd) return INT64_MAX;
    return sqlite3VarintValue(c);
}

static int bt_lower_bound(MemPage *pg, i64 k) {
    int n = pg->nCell;
    i64 k0 = bt_cell_key(pg, 0), kn = bt_cell_key(pg, n - 1);
    if (k <= k0) return 0;
    if (k > kn) return n;
    int lo = 1, hi = n - 1;
    int g = lo + (int)((double)(k - k0) / (double)(kn - k0) * (double)(n - 1) - 0.5);
    if (g < lo) g = lo;
    if (g > hi) g = hi;
    for (int probe = 0; lo < hi; probe++) {
        if (bt_cell_key(pg, g) >= k) hi = g;
        else lo = g + 1;
        if (probe < 2 && lo < hi) g = hi == g ? g - 1 : g + 1;
        else g = lo + ((hi - lo) >> 1);
        if (g < lo) g = lo;
        if (g > hi) g = hi;
    }
    return lo;
}

static void bt_release_hits(struct bt_hit *h, int n) {
    for (int i = 0; i < n; i++)
        if (h[i].pg) releasePageNotNull(h[i].pg);
}

static int bt_seek_many(Btree *tree, Pgno root, const i64 *key, int n, struct bt_hit *hit) {
    BtShared *bs = tree->pBt;
    MemPage *pg[FEED_N];
    Pgno next[FEED_N];
    if (n < 1 || n > FEED_N) return -1;
    for (int i = 0; i < n; i++) next[i] = root;
    for (int depth = 0;; depth++) {
        for (int i = 0; i < n; i++) {
            int j = i - 1;
            while (j >= 0 && next[j] != next[i]) j--;
            if (j >= 0) {
                pg[i] = pg[j];
                sqlite3PagerRef(pg[i]->pDbPage);
            } else if (getAndInitPage(bs, next[i], &pg[i], PAGER_GET_READONLY) != SQLITE_OK) {
                pg[i] = NULL;
            } else {
                __builtin_prefetch(pg[i]->aCellIdx);
                __builtin_prefetch(pg[i]->aCellIdx + 2 * (pg[i]->nCell - 1));
            }
            if (!pg[i] || !pg[i]->intKey || pg[i]->nCell == 0 || pg[i]->nOverflow ||
                depth >= BTCURSOR_MAX_DEPTH || (i > 0 && pg[i]->leaf != pg[0]->leaf)) {
                for (int k = 0; k <= i; k++)
                    if (pg[k]) releasePageNotNull(pg[k]);
                return -1;
            }
        }
        if (pg[0]->leaf) break;
        for (int i = 0; i < n; i++) {
            int lw = bt_lower_bound(pg[i], key[i]);
            next[i] = lw >= pg[i]->nCell ? get4byte(&pg[i]->aData[pg[i]->hdrOffset + 8])
                                         : get4byte(findCell(pg[i], lw));
        }
        for (int i = 0; i < n; i++) releasePageNotNull(pg[i]);
    }
    for (int i = 0; i < n; i++) {
        int ix = bt_lower_bound(pg[i], key[i]);
        if (ix < pg[i]->nCell && bt_cell_key(pg[i], ix) == key[i]) {
            hit[i].pg = pg[i];
            hit[i].ix = ix;
        } else {
            releasePageNotNull(pg[i]);
            hit[i].pg = NULL;
        }
    }
    return 0;
}

static int bt_hit_record(struct bt_hit *h, struct rec *r) {
    CellInfo info;
    h->pg->xParseCell(h->pg, findCell(h->pg, h->ix), &info);
    if (info.nLocal != info.nPayload) return -1;
    r->p = info.pPayload;
    r->n = info.nPayload;
    return 0;
}

/* usernames for the feed's 20 authors, in one batched descent (0 ok, -1 fall back to SQL) */
static int fill_authors_batch(struct row *rows, char **ap, char *aend) {
    i64 uids[FEED_N];
    struct bt_hit hits[FEED_N];
    for (int i = 0; i < FEED_N; i++) {
        if (rows[i].user_id <= 0) return -1;
        uids[i] = rows[i].user_id;
    }
    if (bt_seek_many(bt, root_users, uids, FEED_N, hits) != 0) return -1;
    int rc = 0;
    for (int i = 0; i < FEED_N; i++) {
        struct rec r;
        struct col c[2];
        if (!hits[i].pg || bt_hit_record(&hits[i], &r) || rec_cols(&r, 2, c) ||
            !col_is_text(&c[1]) ||
            put(ap, aend, c[1].p, col_len(&c[1]), &rows[i].author, &rows[i].author_len)) {
            rc = -1;
            break;
        }
    }
    bt_release_hits(hits, FEED_N);
    return rc;
}

static int fill_common(struct row *rows, char **ap, char *aend) {
    if (fill_authors_batch(rows, ap, aend) != 0) return -1;
    i64 lo = rows[0].id, hi = rows[0].id;
    for (int i = 1; i < FEED_N; i++) {
        if (rows[i].id < lo) lo = rows[i].id;
        if (rows[i].id > hi) hi = rows[i].id;
    }
    if (hi - lo <= 512) {
        i64 counts[513];
        if (range_counts(lo, hi, counts) != 0) return -1;
        for (int i = 0; i < FEED_N; i++) rows[i].likes = counts[rows[i].id - lo];
    } else {
        for (int i = 0; i < FEED_N; i++) {
            i64 one = 0;
            if (range_counts(rows[i].id, rows[i].id, &one) != 0) return -1;
            rows[i].likes = one;
        }
    }
    return FEED_N;
}

int db_feed(struct row *rows, char *arena, size_t cap) {
    if (db_begin() != 0) return -1;
    char *ap = arena, *aend = arena + cap;
    if (posts_ascending) {
        if (fill_tail(rows, &ap, aend) != 0) return -1;
    } else {
        if (fill_index(rows, &ap, aend) != 0) return -1;
    }
    return fill_common(rows, &ap, aend);
}

int db_feed_sql(struct row *rows, char *arena, size_t cap) {
    if (db_begin() != 0) return -1;
    char *ap = arena, *aend = arena + cap;
    int n = 0, rc;
    while (n < FEED_N && (rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
        rows[n].id = sqlite3_column_int64(st_feed, 0);
        const u8 *b = sqlite3_column_text(st_feed, 1);
        int bl = sqlite3_column_bytes(st_feed, 1);
        const u8 *c = sqlite3_column_text(st_feed, 2);
        int cl = sqlite3_column_bytes(st_feed, 2);
        const u8 *a = sqlite3_column_text(st_feed, 3);
        int al = sqlite3_column_bytes(st_feed, 3);
        rows[n].likes = sqlite3_column_int64(st_feed, 4);
        rows[n].user_id = 0;
        if (!b || !c || !a) { sqlite3_reset(st_feed); return -1; }
        if (put(&ap, aend, b, (u32)bl, &rows[n].body, &rows[n].body_len) ||
            put(&ap, aend, c, (u32)cl, &rows[n].created, &rows[n].created_len) ||
            put(&ap, aend, a, (u32)al, &rows[n].author, &rows[n].author_len)) {
            sqlite3_reset(st_feed);
            return -1;
        }
        n++;
    }
    if (n == FEED_N) rc = SQLITE_DONE;
    sqlite3_reset(st_feed);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) return -1;
    return n;
}

/* ---- single post / writes ------------------------------------------------------------- */

int db_post(long long id, struct row *out) {
    if (db_begin() != 0) return -1;
    int rc = cur_seek(cur[0], id);
    if (rc < 0) return -1;
    if (rc == 1) return 0;
    struct rec r;
    struct col c[4];
    if (record(cur[0], &r) || rec_cols(&r, 4, c) || !col_is_int(&c[1]) || !col_is_text(&c[2]) ||
        !col_is_text(&c[3]))
        return -1;
    out->id = id;
    out->user_id = col_int(&c[1]);
    /* The posts cursor does not move again (the likes cursor does), so these record bytes stay
     * valid until the caller serializes this row. */
    out->body = (const char *)c[2].p;
    out->body_len = (int)col_len(&c[2]);
    out->created = (const char *)c[3].p;
    out->created_len = (int)col_len(&c[3]);
    if (cur_seek(cur[1], out->user_id) != 0) return 0;
    struct col u[2];
    if (record(cur[1], &r) || rec_cols(&r, 2, u) || !col_is_text(&u[1])) return -1;
    out->author = (const char *)u[1].p;
    out->author_len = (int)col_len(&u[1]);
    i64 one = 0;
    if (range_counts(id, id, &one) != 0) return -1;
    out->likes = one;
    return 1;
}

int db_post_exists(long long id) {
    if (db_begin() != 0) return -1;
    int rc = cur_seek(cur[0], id);
    return rc < 0 ? -1 : rc == 0 ? 1 : 0;
}

int db_insert_post(long long user_id, const char *body, int len, long long *id, char *created,
                   int *created_len) {
    if (db_begin() != 0) return -1;
    sqlite3_bind_int64(st_post, 1, user_id);
    sqlite3_bind_text(st_post, 2, body, len, SQLITE_STATIC);
    int rc = sqlite3_step(st_post);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(st_post);
        check_tx();
        return -1;
    }
    *id = sqlite3_column_int64(st_post, 0);
    int n = sqlite3_column_bytes(st_post, 1);
    const unsigned char *t = sqlite3_column_text(st_post, 1);
    if (!t || n > 63) {
        sqlite3_reset(st_post);
        return -1;
    }
    memcpy(created, t, (size_t)n);
    *created_len = n;
    if (posts_ascending) {
        if (n != 24) {
            posts_ascending = 0;
        } else if (last_created_n == 0) {
            memcpy(last_created, created, 24);
            last_created_n = 24;
        } else if (memcmp(last_created, created, 24) > 0) {
            posts_ascending = 0;
        } else {
            memcpy(last_created, created, 24);
        }
    }
    rc = sqlite3_step(st_post);
    sqlite3_reset(st_post);
    sqlite3_clear_bindings(st_post);
    if (rc != SQLITE_DONE) {
        check_tx();
        return -1;
    }
    return 0;
}

int db_insert_like(long long user_id, long long post_id) {
    if (db_begin() != 0) return -1;
    sqlite3_bind_int64(st_like, 1, user_id);
    sqlite3_bind_int64(st_like, 2, post_id);
    int rc = sqlite3_step(st_like);
    sqlite3_reset(st_like);
    sqlite3_clear_bindings(st_like);
    if (rc != SQLITE_DONE) {
        check_tx();
        return -1;
    }
    return sqlite3_changes(db) > 0;
}

int db_health(void) {
    if (db_begin() != 0) return -1;
    int rc = sqlite3_step(st_health);
    int ok = rc == SQLITE_ROW && sqlite3_column_int(st_health, 0) == 1;
    sqlite3_reset(st_health);
    return ok ? 0 : -1;
}

/* ---- open ----------------------------------------------------------------------------- */

static int scan_ascending(sqlite3 *c) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(c, "SELECT created_at FROM posts ORDER BY id", -1, &s, NULL) != SQLITE_OK)
        return 0;
    char prev[32];
    int prevn = 0, ok = 1, rc = SQLITE_DONE;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(s, 0);
        int n = sqlite3_column_bytes(s, 0);
        if (!p || n != 24) {
            ok = 0;
            break;
        }
        if (prevn == 24 && memcmp(prev, p, 24) > 0) {
            ok = 0;
            break;
        }
        memcpy(prev, p, 24);
        prevn = 24;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) ok = 0;
    sqlite3_finalize(s);
    if (ok) {
        posts_ascending = 1;
        last_created_n = prevn;
        if (prevn) memcpy(last_created, prev, (size_t)prevn);
    }
    return ok;
}

static sqlite3_stmt *prep(sqlite3 *c, const char *sql, char *err, int n) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(c, sql, -1, &s, NULL) != SQLITE_OK) {
        snprintf(err, (size_t)n, "prepare %s: %s", sql, sqlite3_errmsg(c));
        return NULL;
    }
    return s;
}

int db_open(const char *path, char *err, int n) {
    if (sqlite3_initialize()) {
        snprintf(err, (size_t)n, "sqlite init");
        return -1;
    }
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl"))
        return fail(err, n, "open", db);
    const char *pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA cache_size=-65536;"
        "PRAGMA mmap_size=0;"
        "PRAGMA wal_autocheckpoint=0;";
    if (sqlite3_exec(db, pragmas, NULL, NULL, NULL)) return fail(err, n, "pragmas", db);
    sqlite3_wal_hook(db, wal_hook, NULL);

    if (!table_layout(db, "users", "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0") ||
        !table_layout(db, "posts", "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0") ||
        !table_layout(db, "likes", "user_id:INTEGER:1,post_id:INTEGER:2,created_at:TEXT:0") ||
        !index_layout(db, "likes_post_id_idx", "post_id:0") ||
        !index_layout(db, "posts_created_at_id_idx", "created_at:1,id:1")) {
        snprintf(err, (size_t)n, "unexpected schema");
        return -1;
    }
    if (ENC(db) != SQLITE_UTF8) {
        snprintf(err, (size_t)n, "database is not UTF-8");
        return -1;
    }
    root_posts = rootpage(db, "posts");
    root_users = rootpage(db, "users");
    root_feed_idx = rootpage(db, "posts_created_at_id_idx");
    root_likes_idx = rootpage(db, "likes_post_id_idx");
    if (!root_posts || !root_users || !root_feed_idx || !root_likes_idx) {
        snprintf(err, (size_t)n, "root pages");
        return -1;
    }
    ki_feed = make_keyinfo(db, 2, 1, 1);
    ki_likes = make_keyinfo(db, 1, 1, 0);
    if (!ki_feed || !ki_likes) {
        snprintf(err, (size_t)n, "keyinfo");
        return -1;
    }
    bt = db->aDb[0].pBt;
    for (int i = 0; i < 4; i++) {
        cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
        if (!cur[i]) {
            snprintf(err, (size_t)n, "cursor alloc");
            return -1;
        }
    }

    /* The feed-tail invariant, verified once on the actual data. */
    /* Run the invariant scan on a throwaway connection with a tiny cache, so the serving
     * connection's page cache starts empty and only ever holds the hot set. */
    sqlite3 *sc = NULL;
    if (sqlite3_open_v2(path, &sc, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") == 0) {
        sqlite3_exec(sc, "PRAGMA cache_size=-1024; PRAGMA mmap_size=0;", NULL, NULL, NULL);
        scan_ascending(sc);
        sqlite3_close(sc);
    } else {
        scan_ascending(db);
    }
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }

    if (!(st_begin = prep(db, "BEGIN", err, n)) || !(st_commit = prep(db, "COMMIT", err, n)) ||
        !(st_rollback = prep(db, "ROLLBACK", err, n)) ||
        !(st_post = prep(db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
                         err, n)) ||
        !(st_like = prep(db,
                         "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) "
                         "ON CONFLICT (user_id, post_id) DO NOTHING",
                         err, n)) ||
        !(st_health = prep(db, "SELECT 1", err, n)) ||
        !(st_feed = prep(db,
                         "SELECT p.id, p.body, p.created_at, u.username, "
                         "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                         "FROM posts p JOIN users u ON u.id = p.user_id "
                         "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
                         err, n)))
        return -1;

    if (ck_start(path)) {
        snprintf(err, (size_t)n, "checkpoint connection: %s", sqlite3_errmsg(ckdb ? ckdb : db));
        return -1;
    }
    return 0;
}
