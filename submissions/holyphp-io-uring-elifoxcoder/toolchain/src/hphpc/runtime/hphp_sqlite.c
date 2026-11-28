/* hphp_sqlite.c — the generic SQLite binding, its background checkpoint
 * worker, the WAL write shim and the B-tree cursor binding.
 *
 * Ported from the twelve-dollar challenge runtime (runtime/hphp_std.c there).
 * The B-tree primitives live in SQLite's internals, so src/hp_btree.c is
 * appended to the amalgamation and linked as HPHP_SQLITE_LIB; without it the
 * bt_* builtins are weak and simply report failure, while all sqlite_*
 * builtins work against the system library.
 */
#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE 1
#endif
#include "hphp_modules.h"
#include <sqlite3.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#endif

/* ==== generic SQLite binding ====
 *
 * A thin, driver-level wrapper around the SQLite C API. It knows nothing
 * about this application: no SQL, no schema, no HTTP. Programs open a
 * database, prepare statements, bind values, step rows and read columns.
 *
 *   $db = sqlite_open("/path/feed.db");
 *   sqlite_exec($db, "PRAGMA journal_mode = WAL");
 *   $st = sqlite_prepare($db, "SELECT body FROM posts WHERE id = ?");
 *   sqlite_bind_int($st, 1, 7);
 *   while (sqlite_step($st) == 100) { $body = sqlite_column_text($st, 0); }
 *   sqlite_reset($st);
 *   sqlite_finalize($st);
 *   sqlite_close($db);
 *
 * Handles are the native pointers widened to integers: opaque to the
 * language, cheap to pass around, and 0 means "failed".
 */

static sqlite3 *hp_sqlite_db(hval v) {
    return (sqlite3 *)(intptr_t)hp_val_to_int(v);
}
static sqlite3_stmt *hp_sqlite_stmt(hval v) {
    return (sqlite3_stmt *)(intptr_t)hp_val_to_int(v);
}

/* Optional, generic SQLite/runtime diagnostics. The counters are only timed
 * when HPHP_DIAG=1; ordinary sqlite_step/exec calls take one cached branch. */


static uint64_t hp_sql_step_ns, hp_sql_read_ns, hp_sql_write_ns, hp_sql_txn_ns;
static uint64_t hp_sql_exec_ns, hp_sql_step_calls, hp_sql_exec_calls, hp_sql_busy;

static bool hp_sql_prefix(const char *s, const char *kw) {
    if (!s) return false;
    while (isspace((unsigned char)*s)) s++;
    while (*kw) {
        if (tolower((unsigned char)*s++) != tolower((unsigned char)*kw++)) return false;
    }
    return !isalnum((unsigned char)*s) && *s != '_';
}

static int hp_sqlite_step_timed(sqlite3_stmt *st) {
    if (!st) return SQLITE_MISUSE;
    bool diag = hp_diag_enabled();
    uint64_t t0 = diag ? hp_diag_now_ns() : 0;
    int rc = sqlite3_step(st);
    if (diag) {
        uint64_t t1 = hp_diag_now_ns();
        uint64_t dt = t1 >= t0 ? t1 - t0 : 0;
        const char *sql = sqlite3_sql(st);
        hp_sql_step_ns += dt;
        hp_sql_step_calls++;
        if (hp_sql_prefix(sql, "BEGIN") || hp_sql_prefix(sql, "COMMIT") ||
            hp_sql_prefix(sql, "ROLLBACK")) hp_sql_txn_ns += dt;
        else if (sqlite3_stmt_readonly(st)) hp_sql_read_ns += dt;
        else hp_sql_write_ns += dt;
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED ||
            rc == SQLITE_BUSY_SNAPSHOT || rc == SQLITE_LOCKED_SHAREDCACHE)
            hp_sql_busy++;
    }
    return rc;
}
/* ---- generic B-tree cursor binding ----
 * The implementation is appended to the SQLite amalgamation by build.sh
 * (toolchain/src/hp_btree.c) and exported as hp_bt_*_impl. A weak reference
 * keeps the program linkable against the system library: the builtins then
 * report failure and the server uses its SQL path. */
#if defined(__GNUC__)
#define HP_BT_WEAK __attribute__((weak))
#else
#define HP_BT_WEAK
#endif
HP_BT_WEAK unsigned int hp_bt_root_impl(sqlite3 *db, const char *z, int i);
HP_BT_WEAK void *hp_bt_open_impl(sqlite3 *db, const char *z, int isidx);
HP_BT_WEAK void hp_bt_close_impl(void *v);
HP_BT_WEAK int hp_bt_first_impl(void *v);
HP_BT_WEAK int hp_bt_last_impl(void *v);
HP_BT_WEAK int hp_bt_next_impl(void *v);
HP_BT_WEAK int hp_bt_prev_impl(void *v);
HP_BT_WEAK int hp_bt_seek_impl(void *v, int64_t r);
HP_BT_WEAK int64_t hp_bt_rowid_impl(void *v);
HP_BT_WEAK const unsigned char *hp_bt_payload_impl(void *v, int *pn);
HP_BT_WEAK int hp_bt_payload_size_impl(void *v);
HP_BT_WEAK void *hp_bt_payload_pin_impl(void *v);
HP_BT_WEAK void hp_bt_page_release_impl(void *v);
HP_BT_WEAK int hp_bt_count_key_impl(void *v, int64_t key);
HP_BT_WEAK int hp_bt_count_range_impl(void *v, int64_t lo, int64_t hi, int64_t *out, int64_t cap);
HP_BT_WEAK void *hp_bt_multi_seek_impl(void *v, const int64_t *keys, int n);
HP_BT_WEAK void *hp_bt_tail_impl(void *v, int n);
HP_BT_WEAK int hp_bt_multi_payloads_impl(void *v, int64_t *out, int64_t cap);
HP_BT_WEAK int hp_bt_multi_keys_impl(void *v, int64_t *out, int64_t cap);
HP_BT_WEAK void hp_bt_multi_release_impl(void *v);

static void *bt_h(hval v) { return (void *)(intptr_t)hp_val_to_int(v); }

hval hpbi_bt_root(hval dbv, hval name, hval isidx) {
    sqlite3 *db = hp_sqlite_db(dbv);
    hstr *s = hp_val_to_str(name);
    return hp_of_int(hp_bt_root_impl ? (int64_t)hp_bt_root_impl(db, s->data, (int)hp_val_to_int(isidx)) : 0);
}
hval hpbi_bt_open(hval dbv, hval name, hval isidx) {
    sqlite3 *db = hp_sqlite_db(dbv);
    hstr *s;
    if (!hp_bt_open_impl) return hp_of_int(0);
    s = hp_val_to_str(name);
    return hp_of_int((int64_t)(intptr_t)hp_bt_open_impl(db, s->data, (int)hp_val_to_int(isidx)));
}
hval hpbi_bt_open_safe(hval dbv, hval name, hval isidx) {
    if (dbv.tag != HV_INT || dbv.u.i == 0 || name.tag != HV_STR || !name.u.s)
        return hp_of_int(0);
    return hpbi_bt_open(dbv, name, isidx);
}
hval hpbi_bt_close(hval h) { if (hp_bt_close_impl) hp_bt_close_impl(bt_h(h)); return hp_of_bool(true); }
hval hpbi_bt_first(hval h) { return hp_of_int(hp_bt_first_impl ? hp_bt_first_impl(bt_h(h)) : -1); }
hval hpbi_bt_last(hval h) { return hp_of_int(hp_bt_last_impl ? hp_bt_last_impl(bt_h(h)) : -1); }
hval hpbi_bt_next(hval h) { return hp_of_int(hp_bt_next_impl ? hp_bt_next_impl(bt_h(h)) : 0); }
hval hpbi_bt_prev(hval h) { return hp_of_int(hp_bt_prev_impl ? hp_bt_prev_impl(bt_h(h)) : 0); }
hval hpbi_bt_seek(hval h, hval rowid) { return hp_of_int(hp_bt_seek_impl ? hp_bt_seek_impl(bt_h(h), (int64_t)hp_val_to_int(rowid)) : -1); }
hval hpbi_bt_rowid(hval h) { return hp_of_int(hp_bt_rowid_impl ? hp_bt_rowid_impl(bt_h(h)) : -1); }
hval hpbi_bt_payload_ptr(hval h) {
    int n = 0;
    const unsigned char *p;
    if (!hp_bt_payload_impl) return hp_of_int(0);
    p = hp_bt_payload_impl(bt_h(h), &n);
    return hp_of_int((int64_t)(intptr_t)p);
}
hval hpbi_bt_payload_size(hval h) {
    return hp_of_int(hp_bt_payload_size_impl
        ? hp_bt_payload_size_impl(bt_h(h)) : 0);
}
hval hpbi_bt_payload_pin(hval h) {
    return hp_of_int((int64_t)(intptr_t)(hp_bt_payload_pin_impl
        ? hp_bt_payload_pin_impl(bt_h(h)) : 0));
}
hval hpbi_bt_page_release(hval pin) {
    if (hp_bt_page_release_impl)
        hp_bt_page_release_impl((void *)(intptr_t)hp_val_to_int(pin));
    return hp_of_bool(true);
}

/* ---- record decoder -------------------------------------------------------
 * SQLite stores a row as: header-size varint, one serial-type varint per
 * column, then the column payloads in order. The server used to decode that
 * byte by byte in the application loop (a VM iteration per byte); this
 * parses it once in C and caches offsets/lengths for the current record.
 * Generic: no schema knowledge, up to 16 columns. */
static _Thread_local struct { int off, len, st; } g_bt_col[16];
static _Thread_local int g_bt_ncol;
static _Thread_local const unsigned char *g_bt_base;

static int64_t hp_bt_varint(const unsigned char *p, int *used) {
    unsigned char c = p[0];
    if (!(c & 0x80)) {
        *used = 1;
        return (int64_t)c;
    }
    uint64_t v = (uint64_t)(c & 0x7f);
    for (int i = 1; i < 8; i++) {
        c = p[i];
        v = (v << 7) | (uint64_t)(c & 0x7f);
        if (!(c & 0x80)) {
            *used = i + 1;
            return (int64_t)v;
        }
    }
    /* SQLite's ninth varint byte contributes all eight bits. */
    *used = 9;
    return (int64_t)((v << 8) | p[8]);
}

static int64_t hp_bt_varint_n(const unsigned char *p, int n, int *used) {
    *used = 0;
    if (!p || n < 1) return -1;
    unsigned char c = p[0];
    if (!(c & 0x80)) { *used = 1; return (int64_t)c; }
    uint64_t v = (uint64_t)(c & 0x7f);
    for (int i = 1; i < 8 && i < n; i++) {
        c = p[i];
        v = (v << 7) | (uint64_t)(c & 0x7f);
        if (!(c & 0x80)) { *used = i + 1; return (int64_t)v; }
    }
    if (n >= 9) { *used = 9; return (int64_t)((v << 8) | p[8]); }
    return -1;
}

static int hp_bt_st_len(int64_t st) {
    if (st == 0 || st == 8 || st == 9) return 0;
    if (st >= 1 && st <= 4) return (int)st;
    if (st == 5) return 6;
    if (st == 6 || st == 7) return 8;
    if (st >= 12) return (int)((st - 12) / 2);
    return 0;
}

static int64_t hp_bt_decode_cols(const unsigned char *p, int n) {
    g_bt_ncol = 0;
    if (!p || n < 1) return -1;
    int used = 0;
    int64_t hdr = hp_bt_varint(p, &used);
    if (hdr < 1 || hdr > n) return -1;
    int k = used;
    int off = (int)hdr;
    int nc = 0;
    while (k < (int)hdr && nc < 16) {
        int u2 = 0;
        int64_t st = hp_bt_varint(p + k, &u2);
        k += u2;
        int len = hp_bt_st_len(st);
        g_bt_col[nc].st = (int)st;
        g_bt_col[nc].off = off;
        g_bt_col[nc].len = len;
        off += len;
        nc++;
    }
    g_bt_base = p;
    g_bt_ncol = nc;
    return nc;
}

int64_t hp_bt_cols_t(int64_t h) {
    int n = 0;
    const unsigned char *p = hp_bt_payload_impl
        ? hp_bt_payload_impl((void *)(intptr_t)h, &n) : NULL;
    return hp_bt_decode_cols(p, n);
}



int64_t hp_bt_col_off_t(int64_t i) {
    return (i >= 0 && i < g_bt_ncol) ? g_bt_col[i].off : 0;
}
int64_t hp_bt_col_len_t(int64_t i) {
    return (i >= 0 && i < g_bt_ncol) ? g_bt_col[i].len : 0;
}
static int64_t hp_bt_col_int_value(int64_t i) {
    if (i < 0 || i >= g_bt_ncol) return 0;
    int st = g_bt_col[i].st;
    int off = g_bt_col[i].off;
    int len = g_bt_col[i].len;
    if (st == 8) return 0;
    if (st == 9) return 1;
    if (len <= 0 || len > 8) return 0;
    const unsigned char *q = g_bt_base + off;
    uint64_t v = 0;
    for (int j = 0; j < len; j++) v = (v << 8) | q[j];
    if (len < 8 && (q[0] & 0x80)) v |= ~0ULL << (len * 8);
    return (int64_t)v;
}
int64_t hp_bt_col_int_t(int64_t i) { return hp_bt_col_int_value(i); }

/* Read one integer-like column from the cursor's current SQLite record.
 * Unlike bt_cols()+bt_col_int(), this skips decoding every other column and
 * does not touch the thread-local decoded-column state. */
int64_t hp_bt_col_int_at_t(int64_t h, int64_t col) {
    if (col < 0 || col >= 16 || !hp_bt_payload_impl) return 0;
    int n = 0;
    const unsigned char *p = hp_bt_payload_impl((void *)(intptr_t)h, &n);
    if (!p || n < 1) return 0;
    int used = 0;
    int64_t hdr = hp_bt_varint_n(p, n, &used);
    if (used <= 0 || hdr < used || hdr > n) return 0;
    int k = used;
    int64_t off = hdr;
    for (int64_t i = 0; i <= col; i++) {
        if (k >= hdr) return 0;
        int nst = 0;
        int64_t st = hp_bt_varint_n(p + k, (int)hdr - k, &nst);
        if (nst <= 0 || k + nst > hdr) return 0;
        k += nst;
        int len = hp_bt_st_len(st);
        if (len < 0 || off + len > n) return 0;
        if (i == col) {
            if (st == 8) return 0;
            if (st == 9) return 1;
            if (len <= 0 || len > 8) return 0;
            const unsigned char *q = p + off;
            uint64_t v = 0;
            for (int j = 0; j < len; j++) v = (v << 8) | q[j];
            if (len < 8 && (q[0] & 0x80)) v |= ~0ULL << (len * 8);
            return (int64_t)v;
        }
        off += len;
    }
    return 0;
}

/* Decode only the requested leading columns directly into a caller-owned
 * int64 vector: {serial type, payload offset, payload length, integer value}. */
static int64_t hp_bt_read_payload_raw(const unsigned char *p, int n, int64_t *out, int64_t cap) {
    if (!p || !out || cap < 0 || n < 1) return -1;
    int used = 0;
    int64_t hdr = hp_bt_varint(p, &used);
    if (hdr < used || hdr > n) return -1;
    int k = used;
    int64_t off = hdr, nc = 0;
    while (k < hdr && nc < cap) {
        int u2 = 0;
        int64_t st = hp_bt_varint(p + k, &u2);
        if (u2 <= 0 || k + u2 > hdr) return -1;
        k += u2;
        int64_t len = hp_bt_st_len(st), value = 0;
        if (off < hdr || len < 0 || off + len > n) return -1;
        if (st == 9) value = 1;
        else if (st >= 1 && st <= 6 && len > 0) {
            const unsigned char *q = p + off;
            uint64_t v = 0;
            for (int64_t j = 0; j < len; j++) v = (v << 8) | q[j];
            if (len < 8 && (q[0] & 0x80)) v |= ~0ULL << (len * 8);
            value = (int64_t)v;
        }
        out[nc * 4] = st;
        out[nc * 4 + 1] = off;
        out[nc * 4 + 2] = len;
        out[nc * 4 + 3] = value;
        off += len;
        nc++;
    }
    return nc;
}
static int64_t hp_bt_read_raw(int64_t h, int64_t *out, int64_t cap) {
    int n = 0;
    const unsigned char *p = hp_bt_payload_impl
        ? hp_bt_payload_impl((void *)(intptr_t)h, &n) : NULL;
    return hp_bt_read_payload_raw(p, n, out, cap);
}
hval hpbi_bt_cols(hval h) { return hp_of_int(hp_bt_cols_t((int64_t)hp_val_to_int(h))); }
hval hpbi_bt_col_off(hval i) { return hp_of_int(hp_bt_col_off_t((int64_t)hp_val_to_int(i))); }
hval hpbi_bt_col_len(hval i) { return hp_of_int(hp_bt_col_len_t((int64_t)hp_val_to_int(i))); }
hval hpbi_bt_col_int(hval i) { return hp_of_int(hp_bt_col_int_t((int64_t)hp_val_to_int(i))); }
hval hpbi_bt_col_int_at(hval h, hval col) {
    return hp_of_int(hp_bt_col_int_at_t((int64_t)hp_val_to_int(h),
                                        (int64_t)hp_val_to_int(col)));
}
hval hpbi_bt_count_key(hval h, hval key) {
    if (!hp_bt_count_key_impl) return hp_of_int(-1);
    return hp_of_int((int64_t)hp_bt_count_key_impl(bt_h(h), (int64_t)hp_val_to_int(key)));
}

hval hpbi_bt_count_range(hval h, hval lo, hval hi, hval out) {
    hp_iarr_t *a = iarr_of(out);
    if (!hp_bt_count_range_impl || !a) return hp_of_int(-1);
    return hp_of_int((int64_t)hp_bt_count_range_impl(
        bt_h(h), hp_val_to_int(lo), hp_val_to_int(hi), a->v, a->cap));
}
hval hpbi_bt_multi_seek(hval h, hval keys, hval count) {
    hp_iarr_t *a = iarr_of(keys);
    int64_t n = hp_val_to_int(count);
    if (!hp_bt_multi_seek_impl || !a || !a->leased || n < 1 || n > a->cap)
        return hp_of_int(0);
    return hp_of_int((int64_t)(intptr_t)hp_bt_multi_seek_impl(bt_h(h), a->v, (int)n));
}
hval hpbi_bt_tail(hval h, hval count) {
    int64_t n = hp_val_to_int(count);
    if (!hp_bt_tail_impl || n < 1 || n > 64) return hp_of_int(0);
    return hp_of_int((int64_t)(intptr_t)hp_bt_tail_impl(bt_h(h), (int)n));
}
hval hpbi_bt_multi_payloads(hval result, hval out) {
    hp_iarr_t *a = iarr_of(out);
    if (!hp_bt_multi_payloads_impl || !a || !a->leased) return hp_of_bool(false);
    return hp_of_bool(hp_bt_multi_payloads_impl(
        (void *)(intptr_t)hp_val_to_int(result), a->v, a->cap) != 0);
}
hval hpbi_bt_multi_keys(hval result, hval out) {
    hp_iarr_t *a = iarr_of(out);
    if (!hp_bt_multi_keys_impl || !a || !a->leased) return hp_of_int(-1);
    return hp_of_int(hp_bt_multi_keys_impl(
        (void *)(intptr_t)hp_val_to_int(result), a->v, a->cap));
}
hval hpbi_bt_multi_release(hval result) {
    if (hp_bt_multi_release_impl)
        hp_bt_multi_release_impl((void *)(intptr_t)hp_val_to_int(result));
    return hp_of_bool(true);
}
hval hpbi_bt_read(hval h, hval out, hval cols) {
    hp_iarr_t *a = iarr_of(out);
    int64_t c = hp_val_to_int(cols);
    if (!a || c < 0 || c > a->cap / 4) return hp_of_int(-1);
    return hp_of_int(hp_bt_read_raw(hp_val_to_int(h), a->v, c));
}
int64_t hp_bt_read_t(int64_t h, int64_t out, int64_t cols) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)out;
    if (!a || cols < 0 || cols > a->cap / 4) return -1;
    return hp_bt_read_raw(h, a->v, cols);
}

#if !defined(_WIN32)
/* A generic, optional SQLite WAL-checkpoint worker. The application connection
 * remains owned by the event-loop thread; this worker owns a second connection
 * and only performs PASSIVE checkpoints. */
typedef struct {
    sqlite3 *main_db;
    sqlite3 *ckpt_db;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int state;                 /* 0 idle, 1 bulk request, 2 bulk running,
                                * 3 tail request, 4 tail running, 5 needs hold,
                                * 6 stop, 7 forced, 8 restart pending */
    int active_flag;           /* lock-free common-case poll by the event loop */
    int started;
    int threshold_frames;
    uint32_t wal_frames;         /* latest frame count from sqlite3_wal_hook */
    uint64_t wal_generation;     /* protected by mu; commits during checkpoints */
    uint32_t force_frames;       /* synchronous catch-up threshold */
    int max_hold_ms;
    int64_t hold_since_ms;
    /* A PASSIVE checkpoint never resets the log; SQLite restarts it on the
     * next write only when that writer's snapshot sees a fully backfilled WAL
     * and no checkpoint holds the read marks. Re-triggering a checkpoint on
     * every commit keeps the worker busy forever, so the log never restarts
     * and grows until the forced path. After a full checkpoint the worker
     * stays idle ("restart pending") until a commit shows the restart, or the
     * log grew another threshold without one. HPHP_CKPT_RESTART=0 disables. */
    int restart_wait;
    uint32_t restart_frames;     /* log size when the restart wait began */
    uint64_t restarts;
} HpSqliteCkpt;
static HpSqliteCkpt hp_sqlite_ckpt = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER
};

static int64_t hp_mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int hp_sqlite_wal_hook(void *arg, sqlite3 *db, const char *name, int nframe) {
    HpSqliteCkpt *w = (HpSqliteCkpt *)arg;
    (void)db; (void)name;
    pthread_mutex_lock(&w->mu);
    if (nframe >= 0) {
        __atomic_store_n(&w->wal_frames, (uint32_t)nframe, __ATOMIC_RELEASE);
        w->wal_generation++;
    }
    if (w->state == 8 && nframe >= 0) {
        if ((uint32_t)nframe < w->restart_frames) {
            /* this commit restarted the log */
            w->state = 0;
            w->restarts++;
            __atomic_store_n(&w->active_flag, 0, __ATOMIC_RELEASE);
        } else if ((uint32_t)nframe >= w->restart_frames + (uint32_t)w->threshold_frames) {
            /* no restart after another threshold of frames: copy again */
            w->state = 0;
        }
    }
    if (nframe >= w->threshold_frames && w->state == 0) {
        w->state = 1;
        __atomic_store_n(&w->active_flag, 1, __ATOMIC_RELEASE);
        pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&w->mu);
    return SQLITE_OK;
}

static void *hp_sqlite_ckpt_main(void *arg) {
    HpSqliteCkpt *w = (HpSqliteCkpt *)arg;
    /* HPHP_CKPT_NICE (0..19, default 19): the worker's nice value. On one
     * CPU a nice-19 worker gets ~1.5% of the core while the event loop is
     * busy, which can be less than the copy work the write rate creates; the
     * WAL then grows until the synchronous forced checkpoint stalls the loop.
     * A moderate value lets it keep pace in small slices. An unprivileged
     * thread cannot lower its nice value later, so it is set once here. */
    int nice_v = 19;
    const char *nice_env = getenv("HPHP_CKPT_NICE");
    if (nice_env && *nice_env) {
        int v = atoi(nice_env);
        if (v >= 0 && v <= 19) nice_v = v;
    }
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice_v);
    for (;;) {
        int mode, nlog = -1, ndone = -1;
        uint64_t generation;
        pthread_mutex_lock(&w->mu);
        while (w->state != 1 && w->state != 3 && w->state != 6)
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->state == 6) { pthread_mutex_unlock(&w->mu); break; }
        mode = w->state;
        w->state = mode == 1 ? 2 : 4;
        generation = w->wal_generation;
        pthread_mutex_unlock(&w->mu);

        int rc = sqlite3_wal_checkpoint_v2(w->ckpt_db, "main",
                                            SQLITE_CHECKPOINT_PASSIVE,
                                            &nlog, &ndone);
        if (getenv("HPHP_STATS") && strcmp(getenv("HPHP_STATS"), "1") == 0)
            fprintf(stderr, "[sqlite-ckpt] mode=%s rc=%d log=%d done=%d restarts=%llu\n",
                    mode == 1 ? "bulk" : "tail", rc, nlog, ndone,
                    (unsigned long long)w->restarts);
        pthread_mutex_lock(&w->mu);
        if (rc == SQLITE_OK && nlog >= 0 && ndone >= nlog) {
            w->hold_since_ms = 0;
            if (w->restart_wait && nlog > 0 && w->wal_generation == generation) {
                /* Fully backfilled and nothing committed meanwhile: the
                 * next writer restarts the log. Stay idle until it does. */
                w->state = 8;
                w->restart_frames = (uint32_t)nlog;
            } else if (w->restart_wait && nlog > 0) {
                /* Commits overtook this checkpoint, so the WAL holds frames
                 * past what was copied and no writer can restart it. Copy
                 * the tail under a short write hold (state 5 -> 3), as for
                 * an incomplete checkpoint; it then ends in state 8. */
                w->state = 5;
            } else {
                w->state = 0;
                __atomic_store_n(&w->active_flag, 0, __ATOMIC_RELEASE);
            }
            if (w->wal_generation == generation)
                __atomic_store_n(&w->wal_frames, 0, __ATOMIC_RELEASE);
        } else {
            w->state = 5;
        }
        pthread_mutex_unlock(&w->mu);
        (void)mode;
    }
    return NULL;
}

static bool hp_sqlite_ckpt_hold(void) {
    HpSqliteCkpt *w = &hp_sqlite_ckpt;
    int64_t now = hp_mono_ms();
    bool hold = false;
    if (!__atomic_load_n(&w->active_flag, __ATOMIC_ACQUIRE)) return false;
    pthread_mutex_lock(&w->mu);
    if (w->state == 5) {
        if (w->hold_since_ms == 0) w->hold_since_ms = now;
        if (now - w->hold_since_ms >= w->max_hold_ms) {
            w->state = 0;
            w->hold_since_ms = 0;
            __atomic_store_n(&w->active_flag, 0, __ATOMIC_RELEASE);
        } else {
            w->state = 3;
            pthread_cond_signal(&w->cv);
            hold = true;
        }
    } else if (w->state == 3 || w->state == 4) {
        hold = w->hold_since_ms != 0 && now - w->hold_since_ms < w->max_hold_ms;
    }
    pthread_mutex_unlock(&w->mu);
    return hold;
}

static void hp_sqlite_ckpt_stop(sqlite3 *db) {
    HpSqliteCkpt *w = &hp_sqlite_ckpt;
    if (!w->started || w->main_db != db) return;
    sqlite3_wal_hook(db, NULL, NULL);
    pthread_mutex_lock(&w->mu);
    w->state = 6;
    __atomic_store_n(&w->active_flag, 0, __ATOMIC_RELEASE);
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    sqlite3_close(w->ckpt_db);
    w->main_db = w->ckpt_db = NULL;
    w->started = 0;
    w->state = 0;
    __atomic_store_n(&w->active_flag, 0, __ATOMIC_RELEASE);
    w->hold_since_ms = 0;
}

/* Bound WAL growth with a synchronous last-resort checkpoint once the log
 * keeps growing past its configured limit. Run this from the event-loop thread
 * between request batches, after the app has closed its B-tree cursors. */
static bool hp_sqlite_ckpt_tick(sqlite3 *db) {
    HpSqliteCkpt *w = &hp_sqlite_ckpt;
    if (!w->started || w->main_db != db || !w->force_frames ||
        __atomic_load_n(&w->wal_frames, __ATOMIC_ACQUIRE) < w->force_frames)
        return false;

    pthread_mutex_lock(&w->mu);
    /* The WAL hook queues a bulk checkpoint immediately at commit, before
     * this between-batches tick can run. Requiring IDLE alone therefore
     * makes the force limit unreachable under sustained writes. Queued
     * bulk, tail-pending, and restart-pending states have no worker inside
     * SQLite and can safely be claimed under the same mutex. */
    if ((w->state != 0 && w->state != 1 && w->state != 5 && w->state != 8) ||
        __atomic_load_n(&w->wal_frames, __ATOMIC_ACQUIRE) < w->force_frames) {
        pthread_mutex_unlock(&w->mu);
        return false;
    }
    /* State 7 reserves the main connection for this checkpoint. The worker
     * sleeps in every state except its bulk/tail requests and stop. */
    w->state = 7;
    w->hold_since_ms = 0;
    uint64_t generation = w->wal_generation;
    pthread_mutex_unlock(&w->mu);

    struct timespec started, ended;
    clock_gettime(CLOCK_MONOTONIC, &started);
    int nlog = -1, ndone = -1;
    int rc = sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE,
                                       &nlog, &ndone);
    bool full = rc == SQLITE_OK && nlog >= 0 && ndone >= nlog;
    clock_gettime(CLOCK_MONOTONIC, &ended);
    int64_t elapsed_us = (int64_t)(ended.tv_sec - started.tv_sec) * 1000000 +
                         (ended.tv_nsec - started.tv_nsec) / 1000;

    pthread_mutex_lock(&w->mu);
    if (full && w->wal_generation == generation)
        __atomic_store_n(&w->wal_frames, 0, __ATOMIC_RELEASE);
    if (w->state == 7) {
        if (full && w->restart_wait && nlog > 0) {
            w->state = 8;
            w->restart_frames = (uint32_t)nlog;
        } else {
            w->state = 0;
            __atomic_store_n(&w->active_flag, 0, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&w->mu);
    if (getenv("HPHP_STATS") && strcmp(getenv("HPHP_STATS"), "1") == 0)
        fprintf(stderr, "[sqlite-ckpt] mode=forced rc=%d log=%d done=%d full=%d us=%lld\n",
                rc, nlog, ndone, full ? 1 : 0, (long long)elapsed_us);
    return full;
}
#endif

/* Optional generic SQLite VFS wrapper. SQLite emits separate xWrite calls
 * for WAL frame headers and pages; sequential frames are buffered until a
 * commit frame, then written as one contiguous range. All reads, syncs,
 * truncates, locks, and file controls flush first, so SQLite observes the
 * same bytes and durability boundary as the underlying VFS. */
#if !defined(_WIN32)
typedef struct {
    sqlite3_file base;
    sqlite3_file *real;
    unsigned char *buf;
    size_t n, cap;
    sqlite3_int64 off, next;
    int wal, commit_hdr;
} HpWalFile;
static sqlite3_vfs *hp_wal_base_vfs;
static sqlite3_vfs hp_wal_vfs;
static char hp_wal_vfs_name[96];

static int hp_wal_flush(HpWalFile *f) {
    if (!f->n) return SQLITE_OK;
    int rc = SQLITE_OK;
    for (size_t at = 0; at < f->n && rc == SQLITE_OK; at += 0x1f000) {
        size_t n = f->n - at;
        if (n > 0x1f000) n = 0x1f000;
        rc = f->real->pMethods->xWrite(f->real, f->buf + at, (int)n,
                                       f->off + (sqlite3_int64)at);
    }
    f->n = 0;
    return rc;
}
#define HP_WAL_FLUSH(f) do { int hp_rc_ = hp_wal_flush((HpWalFile *)(f)); if (hp_rc_ != SQLITE_OK) return hp_rc_; } while (0)
static int hp_wal_close(sqlite3_file *file) {
    HpWalFile *f = (HpWalFile *)file;
    int rc = hp_wal_flush(f);
    free(f->buf);
    f->buf = NULL;
    int rc2 = f->real->pMethods->xClose(f->real);
    return rc != SQLITE_OK ? rc : rc2;
}
static int hp_wal_read(sqlite3_file *file, void *buf, int n, sqlite3_int64 off) {
    HP_WAL_FLUSH(file);
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xRead(f->real, buf, n, off);
}
static int hp_wal_write(sqlite3_file *file, const void *buf, int n, sqlite3_int64 off) {
    HpWalFile *f = (HpWalFile *)file;
    if (!f->wal) return f->real->pMethods->xWrite(f->real, buf, n, off);
    /* SQLite can rewrite a previously emitted frame header after a cache
     * spill. Write that header through immediately; otherwise a commit could
     * return before its final checksum reaches the underlying VFS. */
    if (n == 24 && off != f->next) {
        HP_WAL_FLUSH(file);
        f->next = off + n;
        const unsigned char *h = (const unsigned char *)buf;
        f->commit_hdr = (h[4] | h[5] | h[6] | h[7]) != 0;
        return f->real->pMethods->xWrite(f->real, buf, n, off);
    }
    f->next = off + n;
    if (f->n && off != f->off + (sqlite3_int64)f->n) HP_WAL_FLUSH(file);
    if (f->n + (size_t)n > f->cap) {
        if (f->n + (size_t)n > (8u << 20)) {
            HP_WAL_FLUSH(file);
            if ((size_t)n > f->cap) return f->real->pMethods->xWrite(f->real, buf, n, off);
        } else {
            size_t cap = f->cap ? f->cap : 65536;
            while (cap < f->n + (size_t)n) cap *= 2;
            unsigned char *next = (unsigned char *)realloc(f->buf, cap);
            if (!next) {
                HP_WAL_FLUSH(file);
                return f->real->pMethods->xWrite(f->real, buf, n, off);
            }
            f->buf = next;
            f->cap = cap;
        }
    }
    if (!f->n) f->off = off;
    memcpy(f->buf + f->n, buf, (size_t)n);
    f->n += (size_t)n;
    if (n == 24) {
        const unsigned char *h = (const unsigned char *)buf;
        f->commit_hdr = (h[4] | h[5] | h[6] | h[7]) != 0;
        return SQLITE_OK;
    }
    if (f->commit_hdr) {
        f->commit_hdr = 0;
        return hp_wal_flush(f);
    }
    return SQLITE_OK;
}
static int hp_wal_truncate(sqlite3_file *file, sqlite3_int64 size) {
    HP_WAL_FLUSH(file);
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xTruncate(f->real, size);
}
static int hp_wal_sync(sqlite3_file *file, int flags) {
    HP_WAL_FLUSH(file);
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xSync(f->real, flags);
}
static int hp_wal_size(sqlite3_file *file, sqlite3_int64 *size) {
    HP_WAL_FLUSH(file);
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xFileSize(f->real, size);
}
static int hp_wal_lock(sqlite3_file *file, int level) {
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xLock(f->real, level);
}
static int hp_wal_unlock(sqlite3_file *file, int level) {
    HP_WAL_FLUSH(file);
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xUnlock(f->real, level);
}
static int hp_wal_reserved(sqlite3_file *file, int *result) {
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xCheckReservedLock(f->real, result);
}
static int hp_wal_control(sqlite3_file *file, int op, void *arg) {
    HP_WAL_FLUSH(file);
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xFileControl(f->real, op, arg);
}
static int hp_wal_sector(sqlite3_file *file) {
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xSectorSize(f->real);
}
static int hp_wal_device(sqlite3_file *file) {
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xDeviceCharacteristics(f->real);
}
static int hp_wal_shm_map(sqlite3_file *file, int page, int size, int extend,
                          void volatile **out) {
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xShmMap(f->real, page, size, extend, out);
}
static int hp_wal_shm_lock(sqlite3_file *file, int off, int n, int flags) {
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xShmLock(f->real, off, n, flags);
}
static void hp_wal_shm_barrier(sqlite3_file *file) {
    HpWalFile *f = (HpWalFile *)file;
    f->real->pMethods->xShmBarrier(f->real);
}
static int hp_wal_shm_unmap(sqlite3_file *file, int del) {
    HpWalFile *f = (HpWalFile *)file;
    return f->real->pMethods->xShmUnmap(f->real, del);
}
static int hp_wal_fetch(sqlite3_file *file, sqlite3_int64 off, int n, void **out) {
    HpWalFile *f = (HpWalFile *)file;
    if (f->real->pMethods->iVersion < 3 || !f->real->pMethods->xFetch) { *out = NULL; return SQLITE_OK; }
    return f->real->pMethods->xFetch(f->real, off, n, out);
}
static int hp_wal_unfetch(sqlite3_file *file, sqlite3_int64 off, void *ptr) {
    HpWalFile *f = (HpWalFile *)file;
    if (f->real->pMethods->iVersion < 3 || !f->real->pMethods->xUnfetch) return SQLITE_OK;
    return f->real->pMethods->xUnfetch(f->real, off, ptr);
}
static const sqlite3_io_methods hp_wal_io = {
    3, hp_wal_close, hp_wal_read, hp_wal_write, hp_wal_truncate, hp_wal_sync,
    hp_wal_size, hp_wal_lock, hp_wal_unlock, hp_wal_reserved, hp_wal_control,
    hp_wal_sector, hp_wal_device, hp_wal_shm_map, hp_wal_shm_lock,
    hp_wal_shm_barrier, hp_wal_shm_unmap, hp_wal_fetch, hp_wal_unfetch
};
static int hp_wal_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file,
                       int flags, int *out_flags) {
    (void)vfs;
    HpWalFile *f = (HpWalFile *)file;
    memset(f, 0, sizeof(*f));
    f->real = (sqlite3_file *)(f + 1);
    int rc = hp_wal_base_vfs->xOpen(hp_wal_base_vfs, name, f->real, flags, out_flags);
    f->wal = (flags & SQLITE_OPEN_WAL) != 0;
    f->base.pMethods = rc == SQLITE_OK && f->real->pMethods ? &hp_wal_io : NULL;
    return rc;
}
/* Register the wrapper over `base` (NULL: SQLite's default VFS) and return
 * its name. One wrapper per process: both SQLite connections use it. */
static const char *hp_wal_vfs_register(const char *base) {
    if (hp_wal_base_vfs) return hp_wal_vfs_name;
    sqlite3_vfs *b = sqlite3_vfs_find(base);
    if (!b) return NULL;
    hp_wal_base_vfs = b;
    snprintf(hp_wal_vfs_name, sizeof hp_wal_vfs_name, "hphp-wal-coalesce-%s", b->zName);
    hp_wal_vfs = *b;
    hp_wal_vfs.zName = hp_wal_vfs_name;
    hp_wal_vfs.szOsFile = (int)sizeof(HpWalFile) + b->szOsFile;
    hp_wal_vfs.xOpen = hp_wal_open;
    hp_wal_vfs.pNext = NULL;
    if (sqlite3_vfs_register(&hp_wal_vfs, 0) != SQLITE_OK) { hp_wal_base_vfs = NULL; return NULL; }
    return hp_wal_vfs_name;
}
#else
static const char *hp_wal_vfs_register(const char *base) { (void)base; return NULL; }
#endif

static hval hpbi_sqlite_open_impl(hval path, hval vfs) {
    hstr *p = hp_val_to_str(path);
    hstr *v = vfs.tag == HV_STR ? hp_val_to_str(vfs) : NULL;
    sqlite3 *db = NULL;
    if (sqlite3_initialize() != SQLITE_OK) return hp_of_int(0);
    const char *vfs_name = v && v->len ? v->data : NULL;
    /* HPHP_SQLITE_WAL_COALESCE=1: write each commit's WAL frames with one
     * write call, through a thin wrapper over the selected VFS. */
    const char *co = getenv("HPHP_SQLITE_WAL_COALESCE");
    if (co && strcmp(co, "1") == 0) {
        vfs_name = hp_wal_vfs_register(vfs_name);
        if (!vfs_name) return hp_of_int(0);
    }
    int rc = sqlite3_open_v2(p->data, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                             vfs_name);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return hp_of_int(0);
    }
    return hp_of_int((int64_t)(intptr_t)db);
}

hval hpbi_sqlite_open(hval path) {
    hval nullv = { .tag = HV_NULL, .u.i = 0 };
    return hpbi_sqlite_open_impl(path, nullv);
}
hval hpbi_sqlite_open_safe(hval path) {
    if (path.tag != HV_STR || !path.u.s) return hp_of_int(0);
    return hpbi_sqlite_open(path);
}

hval hpbi_sqlite_open_vfs(hval path, hval vfs) {
    return hpbi_sqlite_open_impl(path, vfs);
}

hval hpbi_sqlite_bg_checkpoint_start(hval dbv, hval pathv, hval framesv, hval holdv) {
#if !defined(_WIN32)
    HpSqliteCkpt *w = &hp_sqlite_ckpt;
    sqlite3 *db = hp_sqlite_db(dbv), *ckdb = NULL;
    hstr *path = hp_val_to_str(pathv);
    int frames = (int)hp_val_to_int(framesv);
    int hold_ms = (int)hp_val_to_int(holdv);
    if (!db || !path || frames < 1 || hold_ms < 1 || sqlite3_threadsafe() == 0 || w->started)
        return hp_of_bool(false);
    /* Use the same VFS as the request connection. With unix-excl the WAL
     * index lives in process heap memory shared by this process's
     * connections, so both connections must use it; mixing it with the
     * default unix VFS would give them different WAL indexes. */
    sqlite3_vfs *vfs = NULL;
    if (sqlite3_file_control(db, "main", SQLITE_FCNTL_VFS_POINTER, &vfs) != SQLITE_OK)
        vfs = NULL;
    int rc = sqlite3_open_v2(path->data, &ckdb,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                             vfs ? vfs->zName : NULL);
    if (rc != SQLITE_OK) {
        if (ckdb) sqlite3_close(ckdb);
        return hp_of_bool(false);
    }
    sqlite3_busy_timeout(ckdb, 5000);
    if (sqlite3_exec(ckdb,
                     "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0",
                     NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(ckdb);
        return hp_of_bool(false);
    }
    /* A checkpoint connection copies WAL pages but never serves requests.
     * Keep its pager cache small (matching the Rust leader's 16-page cache)
     * instead of allowing a second 64 MiB request-sized cache. Operators can
     * override SQLite's cache_size value for same-binary A/B measurements. */
    int ckpt_cache = 16;
    const char *cache_env = getenv("HPHP_SQLITE_CKPT_CACHE");
    if (cache_env && *cache_env) {
        int v = atoi(cache_env);
        if (v >= -65536 && v <= 65536 && v != 0) ckpt_cache = v;
    }
    char cache_pragma[48];
    snprintf(cache_pragma, sizeof cache_pragma, "PRAGMA cache_size=%d", ckpt_cache);
    if (sqlite3_exec(ckdb, cache_pragma, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(ckdb);
        return hp_of_bool(false);
    }
    memset(&w->thread, 0, sizeof w->thread);
    w->main_db = db;
    w->ckpt_db = ckdb;
    w->threshold_frames = frames;
    w->max_hold_ms = hold_ms;
    w->force_frames = 65536;
    const char *force_env = getenv("HPHP_CKPT_FORCE_FRAMES");
    if (force_env && *force_env) {
        long v = strtol(force_env, NULL, 10);
        if (v >= 1 && v <= 1048576) w->force_frames = (uint32_t)v;
    }
    __atomic_store_n(&w->wal_frames, 0, __ATOMIC_RELEASE);
    w->wal_generation = 0;
    w->hold_since_ms = 0;
    const char *restart_env = getenv("HPHP_CKPT_RESTART");
    w->restart_wait = !(restart_env && strcmp(restart_env, "0") == 0);
    w->restart_frames = 0;
    w->restarts = 0;
    w->state = 0;
    __atomic_store_n(&w->active_flag, 0, __ATOMIC_RELEASE);
    if (sqlite3_wal_autocheckpoint(db, 0) != SQLITE_OK) {
        sqlite3_close(ckdb);
        w->main_db = w->ckpt_db = NULL;
        return hp_of_bool(false);
    }
    if (pthread_create(&w->thread, NULL, hp_sqlite_ckpt_main, w) != 0) {
        sqlite3_close(ckdb);
        w->main_db = w->ckpt_db = NULL;
        return hp_of_bool(false);
    }
    w->started = 1;
    sqlite3_wal_hook(db, hp_sqlite_wal_hook, w);
    return hp_of_bool(true);
#else
    (void)dbv; (void)pathv; (void)framesv; (void)holdv;
    return hp_of_bool(false);
#endif
}

hval hpbi_sqlite_bg_checkpoint_hold(hval dbv) {
#if !defined(_WIN32)
    if (hp_sqlite_db(dbv) != hp_sqlite_ckpt.main_db || !hp_sqlite_ckpt.started)
        return hp_of_bool(false);
    return hp_of_bool(hp_sqlite_ckpt_hold());
#else
    (void)dbv;
    return hp_of_bool(false);
#endif
}

hval hpbi_sqlite_bg_checkpoint_tick(hval dbv) {
#if !defined(_WIN32)
    return hp_of_bool(hp_sqlite_ckpt_tick(hp_sqlite_db(dbv)));
#else
    (void)dbv;
    return hp_of_bool(false);
#endif
}

hval hpbi_sqlite_bg_checkpoint_active(hval dbv) {
#if !defined(_WIN32)
    HpSqliteCkpt *w = &hp_sqlite_ckpt;
    if (hp_sqlite_db(dbv) != w->main_db || !w->started) return hp_of_bool(false);
    return hp_of_bool(__atomic_load_n(&w->active_flag, __ATOMIC_ACQUIRE) != 0);
#else
    (void)dbv;
    return hp_of_bool(false);
#endif
}

hval hpbi_sqlite_close(hval dbv) {
    sqlite3 *db = hp_sqlite_db(dbv);
    if (!db) return hp_of_bool(false);
#if !defined(_WIN32)
    hp_sqlite_ckpt_stop(db);
#endif
    return hp_of_bool(sqlite3_close(db) == SQLITE_OK);
}

hval hpbi_sqlite_exec(hval dbv, hval sql) {
    sqlite3 *db = hp_sqlite_db(dbv);
    if (!db) return hp_of_bool(false);
    hstr *s = hp_val_to_str(sql);
    bool diag = hp_diag_enabled();
    uint64_t t0 = diag ? hp_diag_now_ns() : 0;
    int rc = sqlite3_exec(db, s->data, NULL, NULL, NULL);
    if (diag) {
        uint64_t t1 = hp_diag_now_ns();
        hp_sql_exec_ns += t1 >= t0 ? t1 - t0 : 0;
        hp_sql_exec_calls++;
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) hp_sql_busy++;
    }
    return hp_of_bool(rc == SQLITE_OK);
}

hval hpbi_sqlite_status(hval dbv) {
    sqlite3 *db = hp_sqlite_db(dbv);
    int hit = 0, miss = 0, write = 0, spill = 0;
    int cache = 0, schema = 0, stmt = 0, hi = 0;
    if (!db) return hp_of_str(hp_str_new("{}", 2));
    (void)sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_HIT, &hit, &hi, 1);
    (void)sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_MISS, &miss, &hi, 1);
    (void)sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_WRITE, &write, &hi, 1);
    (void)sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_SPILL, &spill, &hi, 1);
    (void)sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_USED, &cache, &hi, 0);
    (void)sqlite3_db_status(db, SQLITE_DBSTATUS_SCHEMA_USED, &schema, &hi, 0);
    (void)sqlite3_db_status(db, SQLITE_DBSTATUS_STMT_USED, &stmt, &hi, 0);
    uint64_t step_ns = hp_sql_step_ns, read_ns = hp_sql_read_ns;
    uint64_t write_ns = hp_sql_write_ns, txn_ns = hp_sql_txn_ns;
    uint64_t exec_ns = hp_sql_exec_ns, step_calls = hp_sql_step_calls;
    uint64_t exec_calls = hp_sql_exec_calls, busy = hp_sql_busy;
    hp_sql_step_ns = hp_sql_read_ns = hp_sql_write_ns = hp_sql_txn_ns = 0;
    hp_sql_exec_ns = hp_sql_step_calls = hp_sql_exec_calls = hp_sql_busy = 0;
    char tmp[640];
    int n = snprintf(tmp, sizeof tmp,
        "{\"cache_hit\":%d,\"cache_miss\":%d,\"cache_write\":%d,\"cache_spill\":%d,"
        "\"cache_used_bytes\":%d,\"schema_used_bytes\":%d,\"stmt_used_bytes\":%d,"
        "\"step_calls\":%llu,\"step_ns\":%llu,\"read_ns\":%llu,\"write_ns\":%llu,"
        "\"txn_ns\":%llu,\"exec_calls\":%llu,\"exec_ns\":%llu,\"busy_locked\":%llu}",
        hit, miss, write, spill, cache, schema, stmt,
        (unsigned long long)step_calls, (unsigned long long)step_ns,
        (unsigned long long)read_ns, (unsigned long long)write_ns,
        (unsigned long long)txn_ns, (unsigned long long)exec_calls,
        (unsigned long long)exec_ns, (unsigned long long)busy);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    return hp_of_str(hp_str_new(tmp, (size_t)n));
}

hval hpbi_sqlite_prepare(hval dbv, hval sql) {
    sqlite3 *db = hp_sqlite_db(dbv);
    if (!db) return hp_of_int(0);
    hstr *s = hp_val_to_str(sql);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, s->data, (int)s->len, &st, NULL) != SQLITE_OK) {
        if (st) sqlite3_finalize(st);
        return hp_of_int(0);
    }
    return hp_of_int((int64_t)(intptr_t)st);
}
hval hpbi_sqlite_prepare_safe(hval dbv, hval sql) {
    if (dbv.tag != HV_INT || dbv.u.i == 0 || sql.tag != HV_STR || !sql.u.s)
        return hp_of_int(0);
    return hpbi_sqlite_prepare(dbv, sql);
}

hval hpbi_sqlite_finalize(hval stv) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    if (!st) return hp_of_bool(false);
    return hp_of_bool(sqlite3_finalize(st) == SQLITE_OK);
}

hval hpbi_sqlite_bind_int(hval stv, hval idx, hval v) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    if (!st) return hp_of_bool(false);
    return hp_of_bool(sqlite3_bind_int64(st, (int)hp_val_to_int(idx),
                                         hp_val_to_int(v)) == SQLITE_OK);
}

hval hpbi_sqlite_bind_text(hval stv, hval idx, hval v) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    if (!st) return hp_of_bool(false);
    hstr *s = hp_val_to_str(v);
    return hp_of_bool(sqlite3_bind_text(st, (int)hp_val_to_int(idx),
                                        s->data, (int)s->len,
                                        SQLITE_TRANSIENT) == SQLITE_OK);
}

hval hpbi_sqlite_step(hval stv) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    return hp_of_int(hp_sqlite_step_timed(st));
}

hval hpbi_sqlite_reset(hval stv) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    if (!st) return hp_of_int(SQLITE_MISUSE);
    return hp_of_int(sqlite3_reset(st));
}

hval hpbi_sqlite_clear_bindings(hval stv) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    if (!st) return hp_of_bool(false);
    return hp_of_bool(sqlite3_clear_bindings(st) == SQLITE_OK);
}

hval hpbi_sqlite_column_text(hval stv, hval col) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    if (!st) return hp_of_str(hp_str_lit(""));
    int i = (int)hp_val_to_int(col);
    const unsigned char *p = sqlite3_column_text(st, i);
    int n = sqlite3_column_bytes(st, i);
    if (!p) return hp_of_str(hp_str_lit(""));
    return hp_of_str(hp_str_new((const char *)p, (size_t)n));
}

hval hpbi_sqlite_column_int(hval stv, hval col) {
    sqlite3_stmt *st = hp_sqlite_stmt(stv);
    if (!st) return hp_of_int(0);
    return hp_of_int((int64_t)sqlite3_column_int64(st, (int)hp_val_to_int(col)));
}

hval hpbi_sqlite_changes(hval dbv) {
    sqlite3 *db = hp_sqlite_db(dbv);
    return hp_of_int(db ? (int64_t)sqlite3_changes(db) : 0);
}

hval hpbi_sqlite_last_insert_rowid(hval dbv) {
    sqlite3 *db = hp_sqlite_db(dbv);
    return hp_of_int(db ? (int64_t)sqlite3_last_insert_rowid(db) : 0);
}

hval hpbi_sqlite_errmsg(hval dbv) {
    sqlite3 *db = hp_sqlite_db(dbv);
    return hp_of_str(hp_str_cstr(db ? sqlite3_errmsg(db) : ""));
}

hval hpbi_sqlite_extended_errcode(hval dbv) {
    sqlite3 *db = hp_sqlite_db(dbv);
    return hp_of_int(db ? (int64_t)sqlite3_extended_errcode(db) : SQLITE_MISUSE);
}

hval hpbi_sqlite_is_lock_error(hval dbv) {
    sqlite3 *db = hp_sqlite_db(dbv);
    int primary = db ? (sqlite3_errcode(db) & 0xff) : SQLITE_OK;
    return hp_of_bool(primary == SQLITE_BUSY || primary == SQLITE_LOCKED);
}

hval hpbi_sqlite_busy_timeout(hval dbv, hval ms) {
    sqlite3 *db = hp_sqlite_db(dbv);
    if (!db) return hp_of_bool(false);
    return hp_of_bool(sqlite3_busy_timeout(db, (int)hp_val_to_int(ms)) == SQLITE_OK);
}

int64_t hp_bt_last_t(int64_t h) { return hp_bt_last_impl ? hp_bt_last_impl((void *)(intptr_t)h) : -1; }
int64_t hp_bt_first_t(int64_t h) { return hp_bt_first_impl ? hp_bt_first_impl((void *)(intptr_t)h) : -1; }
int64_t hp_bt_prev_t(int64_t h) { return hp_bt_prev_impl ? hp_bt_prev_impl((void *)(intptr_t)h) : 0; }
int64_t hp_bt_next_t(int64_t h) { return hp_bt_next_impl ? hp_bt_next_impl((void *)(intptr_t)h) : 0; }
int64_t hp_bt_seek_t(int64_t h, int64_t rowid) {
    return hp_bt_seek_impl ? hp_bt_seek_impl((void *)(intptr_t)h, (sqlite3_int64)rowid) : -1;
}
int64_t hp_bt_rowid_t(int64_t h) {
    return hp_bt_rowid_impl ? (int64_t)hp_bt_rowid_impl((void *)(intptr_t)h) : -1;
}
int64_t hp_bt_payload_ptr_t(int64_t h) {
    int n = 0;
    const unsigned char *p;
    if (!hp_bt_payload_impl) return 0;
    p = hp_bt_payload_impl((void *)(intptr_t)h, &n);
    return (int64_t)(intptr_t)p;
}
int64_t hp_bt_payload_size_t(int64_t h) {
    return hp_bt_payload_size_impl
        ? hp_bt_payload_size_impl((void *)(intptr_t)h) : 0;
}
int64_t hp_bt_payload_pin_t(int64_t h) {
    return (int64_t)(intptr_t)(hp_bt_payload_pin_impl
        ? hp_bt_payload_pin_impl((void *)(intptr_t)h) : 0);
}
bool hp_bt_page_release_t(int64_t pin) {
    if (hp_bt_page_release_impl) hp_bt_page_release_impl((void *)(intptr_t)pin);
    return true;
}
int64_t hp_bt_multi_seek_t(int64_t h, int64_t keys, int64_t count) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)keys;
    if (!hp_bt_multi_seek_impl || !a || !a->leased || count < 1 || count > a->cap)
        return 0;
    return (int64_t)(intptr_t)hp_bt_multi_seek_impl(
        (void *)(intptr_t)h, a->v, (int)count);
}
int64_t hp_bt_tail_t(int64_t h, int64_t count) {
    if (!hp_bt_tail_impl || count < 1 || count > 64) return 0;
    return (int64_t)(intptr_t)hp_bt_tail_impl((void *)(intptr_t)h, (int)count);
}
bool hp_bt_multi_payloads_t(int64_t result, int64_t out) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)out;
    if (!hp_bt_multi_payloads_impl || !a || !a->leased) return false;
    return hp_bt_multi_payloads_impl((void *)(intptr_t)result, a->v, a->cap) != 0;
}
int64_t hp_bt_multi_keys_t(int64_t result, int64_t out) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)out;
    if (!hp_bt_multi_keys_impl || !a || !a->leased) return -1;
    return hp_bt_multi_keys_impl((void *)(intptr_t)result, a->v, a->cap);
}
bool hp_bt_multi_release_t(int64_t result) {
    if (hp_bt_multi_release_impl) hp_bt_multi_release_impl((void *)(intptr_t)result);
    return true;
}
bool hp_sqlite_bind_int_t(int64_t stv, int64_t idx, int64_t v) {
    sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)stv;
    if (!st) return false;
    return sqlite3_bind_int64(st, (int)idx, (sqlite3_int64)v) == SQLITE_OK;
}
bool hp_sqlite_bind_text_t(int64_t stv, int64_t idx, hstr *s) {
    sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)stv;
    if (!st) return false;
    return sqlite3_bind_text(st, (int)idx, s->data, (int)s->len, SQLITE_TRANSIENT) == SQLITE_OK;
}
int64_t hp_sqlite_step_t(int64_t stv) {
    sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)stv;
    return hp_sqlite_step_timed(st);
}
int64_t hp_sqlite_reset_t(int64_t stv) {
    sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)stv;
    if (!st) return SQLITE_MISUSE;
    return sqlite3_reset(st);
}
bool hp_sqlite_clear_bindings_t(int64_t stv) {
    sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)stv;
    if (!st) return false;
    return sqlite3_clear_bindings(st) == SQLITE_OK;
}
hstr *hp_sqlite_column_text_t(int64_t stv, int64_t col) {
    sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)stv;
    if (!st) return hp_str_lit("");
    int i = (int)col;
    const unsigned char *p = sqlite3_column_text(st, i);
    int n = sqlite3_column_bytes(st, i);
    if (!p) return hp_str_lit("");
    return hp_str_new((const char *)p, (size_t)n);
}
int64_t hp_sqlite_column_int_t(int64_t stv, int64_t col) {
    sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)stv;
    if (!st) return 0;
    return (int64_t)sqlite3_column_int64(st, (int)col);
}
int64_t hp_sqlite_changes_t(int64_t dbv) {
    sqlite3 *db = (sqlite3 *)(intptr_t)dbv;
    return db ? (int64_t)sqlite3_changes(db) : 0;
}
