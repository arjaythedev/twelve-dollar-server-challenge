/* SQLite side of the server. The stock SQLite amalgamation (sqlite3.c, unchanged, checksum-verified by build.sh) is
 * compiled into this translation unit, so the B-tree read paths (btree_reads.c, included below) can call SQLite's
 * own cursor API (sqlite3BtreeTableMoveto, sqlite3BtreeIndexMoveto, sqlite3BtreeNext, ...). They only read; nothing
 * here writes SQLite's internal state.
 *
 * Connections (all in this one process):
 *   reader  - GET /feed, GET /posts/:id, /health. Its read transaction (snapshot) is kept across loop passes and
 *             dropped only after our own writer commits (so every request still sees every committed write) or
 *             when the checkpointer waits for readers. Idea credit: rust-iouring-rkkautsar (snapshot reuse).
 *   writer  - the group-commit transaction, on its own committer thread: BEGIN IMMEDIATE (busy timeout 10 s, so it
 *             waits out a checkpoint hold), INSERTs, COMMIT; the loop answers only after COMMIT has returned.
 *   checkpointer thread - PASSIVE every 50 ms; RESTART with a bounded 250 ms hold once the WAL is long and nearly
 *             copied; nice 5. Idea credit: swift-iouring-akbashev, rust-iouring-kensac (MIT).
 * WAL + synchronous=NORMAL (rule 6), mmap off, 64 MiB page cache. */
#include "sqlite3.c"
#include "kn.h"
#include <pthread.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

static sqlite3 *rdb, *wdb;
static sqlite3_stmt *st_rbegin, *st_rcommit, *st_health, *st_feed_sql, *st_post_sql;
/* The group-commit statements, once per connection that may commit: the reader (on the loop, the default) and the
 * writer (the committer thread: KN_COMMITTER=1, or the fallback after a slow commit). */
typedef struct wstmts { sqlite3 *db; sqlite3_stmt *begin, *commit, *rollback, *insert, *exists, *like; } wstmts;
static wstmts W_loop, W_thr;
static char g_path[4096];
static char g_vfs[64];
static int rd_open;                 /* reader holds a read transaction (snapshot) */
static int use_btree;
static atomic_int ckpt_want;        /* checkpointer waits for readers to leave their snapshot */
static int64_t n_bt, n_sql, n_snap;
static int keep_snapshot = 1;      /* KN_SNAPSHOT=0: a new read transaction every loop pass (for measuring) */
/* Where the group commit runs.
 * Default: on the loop, on the reader connection. A COMMIT by another connection makes the reader drop its whole page
 * cache at its next read transaction (SQLite's WAL-index header check, pager_reset), so with a separate committer
 * connection most post reads cost a pread. Committing on the reader's own connection keeps its cache.
 * Finding and measurement: b2-jvm (kotlin-jvm-uring-3x3xX3N0N). KN_COMMITTER=1: always the committer thread.
 * Slow-disk guard: a COMMIT on the loop blocks the loop for as long as its WAL write() takes. If one takes longer than
 * KN_SLOW_MS (default 50 ms), or a checkpoint pass takes longer than KN_SLOW_CK_MS (default 200 ms: the checkpointer's
 * fsync is the first to feel a slow disk), or memory use passes KN_SLOW_MEM_PCT (default 90 %) of the cgroup limit or
 * of RAM (near the wall, reclaim makes writes stall for seconds: two 2 s commits at 448k users locally),
 * the next commits go to the committer thread (own connection, the loop keeps serving reads) for slow_hold ms,
 * starting at KN_SLOW_HOLD_MS (10 s); while commits or checkpoints stay slow the period is extended (doubling, up to
 * 5 min), so the loop only takes the commits back after the disk has been fast for a whole period. */
static int committer_only;
static int ol_pending;              /* on the loop: a batch is staged but BEGIN IMMEDIATE was busy (checkpoint hold) */
static int64_t slow_ns = 50000000, slow_ck_ns = 200000000, slow_hold_ms = 10000, slow_hold0_ms = 10000;
static _Atomic int64_t thr_until_ns;   /* commits go to the committer thread until then (slow-disk guard) */
static int64_t n_ol_commits, n_thr_commits, n_ol_busy, n_slow;
static _Atomic int64_t n_ck_slow, n_mem_slow;
static int slow_mem_pct = 90;
static int test_wal_delay_us;       /* KN_TEST_WAL_DELAY_US: sleep in every WAL write (tests the slow-disk guard) */
static void ol_try(void);
static int64_t now_ns(void);
static pthread_mutex_t slow_mu = PTHREAD_MUTEX_INITIALIZER;
/* A slow write or checkpoint was seen (any thread): commits go to the committer thread for a while (see above). */
static void slow_seen(void) {
    static int64_t since;   /* start of the current period */
    if (committer_only) return;
    pthread_mutex_lock(&slow_mu);
    int64_t t = now_ns();
    if (t >= thr_until_ns) {   /* a new period; after a long calm the length starts over */
        if (t - thr_until_ns > 4 * slow_hold_ms * 1000000) slow_hold_ms = slow_hold0_ms;
        since = t;
    } else if (t - since > slow_hold_ms * 500000 && slow_hold_ms < 300000) {   /* still slow late in the period */
        slow_hold_ms *= 2;
        since = t;
    }
    thr_until_ns = t + slow_hold_ms * 1000000;
    pthread_mutex_unlock(&slow_mu);
}

/* ------------------------------------------------------------------ helpers */
static int prep(sqlite3 *db, const char *sql, sqlite3_stmt **st, char *err, int errcap) {
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, 0) == SQLITE_OK) return 0;
    snprintf(err, errcap, "prepare %.60s: %s", sql, sqlite3_errmsg(db));
    return -1;
}

static int exec(sqlite3 *db, const char *sql, char *err, int errcap) {
    char *e = 0;
    if (sqlite3_exec(db, sql, 0, 0, &e) == SQLITE_OK) return 0;
    snprintf(err, errcap, "%s: %s", sql, e ? e : "?");
    sqlite3_free(e);
    return -1;
}

/* Steps a statement that returns no rows, then resets it. */
static int step_reset(sqlite3_stmt *st) {
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return rc == SQLITE_DONE || rc == SQLITE_ROW ? SQLITE_OK : rc;
}

static int copy_text(sqlite3_stmt *st, int col, uint8_t *out, int cap) {
    int n = sqlite3_column_bytes(st, col);
    const unsigned char *t = sqlite3_column_text(st, col);
    if (!t || n > cap) return -1;
    memcpy(out, t, n);
    return n;
}

static int open_conn(sqlite3 **db, int busy_ms, char *err, int errcap, const char *cache) {
    if (sqlite3_open_v2(g_path, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, g_vfs[0] ? g_vfs : 0) != SQLITE_OK) {
        snprintf(err, errcap, "open %s: %s", g_path, *db ? sqlite3_errmsg(*db) : "out of memory");
        return -1;
    }
    sqlite3_busy_timeout(*db, busy_ms);
    if (exec(*db, "PRAGMA journal_mode=WAL", err, errcap) ||
        exec(*db, "PRAGMA synchronous=NORMAL", err, errcap) ||       /* rule 6 minimum */
        exec(*db, "PRAGMA mmap_size=0", err, errcap) ||              /* pages through the page cache */
        exec(*db, cache, err, errcap) ||                             /* allocated as touched */
        exec(*db, "PRAGMA temp_store=MEMORY", err, errcap) ||
        exec(*db, "PRAGMA wal_autocheckpoint=0", err, errcap) ||     /* the checkpointer thread does it */
        exec(*db, "PRAGMA journal_size_limit=67108864", err, errcap) ||
        exec(*db, "SELECT count(*) FROM sqlite_schema", err, errcap)) return -1;
    return 0;
}

/* ------------------------------------------------------------------ reader snapshot */
#include "btree_reads.c"

static int rd_begin(void) {
    if (rd_open) return 0;
    if (step_reset(st_rbegin) != SQLITE_OK) return -1;   /* BEGIN (deferred) */
    rd_open = 1;
    n_snap++;
    if (use_btree && bt_begin(rdb) != 0) {
        /* No B-tree read transaction: SQL alone still works inside the BEGIN. */
        fprintf(stderr, "btree: begin failed, SQL only from now on\n");
        use_btree = 0;
    }
    return 0;
}

static void rd_end(void) {
    if (!rd_open) return;
    if (use_btree) bt_end();
    step_reset(st_rcommit);
    rd_open = 0;
}

int db_pass_end(void) {
    int done = 0;
    if (ol_pending) { ol_try(); done = !ol_pending; }
    if (rd_open && (!keep_snapshot || atomic_load_explicit(&ckpt_want, memory_order_relaxed))) rd_end();
    return done;
}

/* ------------------------------------------------------------------ page cache: pcache1 with its own xRekey
 * SQLite's own pcache1 through the public SQLITE_CONFIG_PCACHE2 interface, with one method replaced. A B-tree split
 * that renumbers pages swaps two of them through a temporary page number, the lock-byte page's (262,145 for 4 KB
 * pages). pcache1 remembers the largest key it has held until a truncate walks the cache, so after each such swap
 * the next commit's truncate (drop pages past the end of the file) scanned the whole hash table to find nothing.
 * Here the page waits out the swap under key 0, which SQLite never gives a page; everything else is pcache1's.
 * Idea credit: fsharp-uring-delneg (xRekey), c-iouring-pavlyhalim (the diagnosis). KN_PCACHE=0 = stock pcache1. */
static sqlite3_pcache_methods2 pc_base;
static struct { sqlite3_pcache *c; unsigned lockbyte; sqlite3_pcache_page *parked; } pc_tab[64];
static pthread_mutex_t pc_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t n_parked;

static sqlite3_pcache *pc_create(int szPage, int szExtra, int bPurgeable) {
    sqlite3_pcache *c = pc_base.xCreate(szPage, szExtra, bPurgeable);
    if (!c) return 0;
    pthread_mutex_lock(&pc_mu);
    for (int i = 0; i < 64; i++)
        if (!pc_tab[i].c) { pc_tab[i].c = c; pc_tab[i].lockbyte = (unsigned)(PENDING_BYTE / szPage) + 1; pc_tab[i].parked = 0; break; }
    pthread_mutex_unlock(&pc_mu);
    return c;
}

static void pc_destroy(sqlite3_pcache *c) {
    pthread_mutex_lock(&pc_mu);
    for (int i = 0; i < 64; i++) if (pc_tab[i].c == c) { pc_tab[i].c = 0; break; }
    pthread_mutex_unlock(&pc_mu);
    pc_base.xDestroy(c);
}

/* Each cache belongs to one connection, used by one thread at a time; the table lookup is a short linear scan. */
static void pc_rekey(sqlite3_pcache *c, sqlite3_pcache_page *pg, unsigned oldKey, unsigned newKey) {
    for (int i = 0; i < 64; i++) {
        if (pc_tab[i].c != c) continue;
        if (newKey == pc_tab[i].lockbyte && !pc_tab[i].parked) { pc_tab[i].parked = pg; n_parked++; newKey = 0; }
        else if (oldKey == pc_tab[i].lockbyte && pc_tab[i].parked == pg) { pc_tab[i].parked = 0; oldKey = 0; }
        break;
    }
    pc_base.xRekey(c, pg, oldKey, newKey);
}

static void pc_install(void) {
    if (getenv("KN_PCACHE") && getenv("KN_PCACHE")[0] == '0') return;
    sqlite3_pcache_methods2 m;
    if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pc_base) != SQLITE_OK) return;
    m = pc_base;
    m.xCreate = pc_create; m.xDestroy = pc_destroy; m.xRekey = pc_rekey;
    if (sqlite3_config(SQLITE_CONFIG_PCACHE2, &m) != SQLITE_OK) fprintf(stderr, "pcache wrapper not installed\n");
}

/* ------------------------------------------------------------------ WAL writes: one pwrite per commit
 * A VFS of its own over the base VFS (public sqlite3_vfs API) that changes only the WAL file's writes. SQLite writes
 * each frame as two writes (24-byte header, then the page), so a commit of n frames costs 2n pwrite calls. Here
 * writes that continue where the previous one ended are gathered in a buffer, and the buffer goes to the file when
 * the commit frame's page arrives (its header's database-size field is non-zero), inside SQLite's own xWrite call:
 * so the commit is in the kernel before SQLite publishes it in the WAL index and before COMMIT returns (rule 6
 * holds exactly as with the stock VFS). Any other write, and every read, sync, truncate, lock, size query, file
 * control and close writes the buffer first. Pieces of at most 128 KiB - 1 (the unix VFS takes at most 0x1ffff
 * bytes per write). Every other file (database, -shm) is the base VFS's own object, untouched.
 * Idea credit: c-iouring-pavlyhalim, fsharp-uring-delneg, goose-iouring-ahmadsh2007. KN_WALVFS=0 = stock VFS. */
typedef struct walf {
    sqlite3_file base;
    uint8_t *buf;
    sqlite3_int64 off;        /* file offset of buf[0] */
    int len, cap, commit;     /* commit: the last write was a commit frame's header */
    sqlite3_file *real;       /* the base VFS's file object, right after this struct */
} walf;

static sqlite3_vfs wv_vfs, *wv_base;
static sqlite3_io_methods wv_io;
static int64_t n_wal_writes, n_wal_flushes;

static int wv_flush(walf *f) {
    int rc = SQLITE_OK, done = 0;
    if (test_wal_delay_us && f->len) usleep((useconds_t)test_wal_delay_us);
    while (rc == SQLITE_OK && done < f->len) {
        int n = f->len - done;
        if (n > 0x1ffff) n = 0x1ffff;
        rc = f->real->pMethods->xWrite(f->real, f->buf + done, n, f->off + done);
        done += n;
        n_wal_flushes++;
    }
    f->len = 0; f->commit = 0;
    return rc;
}

static int wv_write(sqlite3_file *pf, const void *p, int amt, sqlite3_int64 off) {
    walf *f = (walf *)pf;
    n_wal_writes++;
    if (f->len && off != f->off + f->len) { int rc = wv_flush(f); if (rc) return rc; }
    if (f->len + amt > f->cap) {
        int cap = f->cap ? f->cap : 1 << 18;
        while (cap < f->len + amt) cap *= 2;
        if (cap > (16 << 20)) {   /* very large transaction: write what we have and this piece directly */
            int rc = wv_flush(f);
            return rc ? rc : f->real->pMethods->xWrite(f->real, p, amt, off);
        }
        uint8_t *b = realloc(f->buf, (size_t)cap);
        if (!b) { int rc = wv_flush(f); return rc ? rc : f->real->pMethods->xWrite(f->real, p, amt, off); }
        f->buf = b; f->cap = cap;
    }
    if (!f->len) f->off = off;
    memcpy(f->buf + f->len, p, (size_t)amt);
    f->len += amt;
    if (f->commit) return wv_flush(f);            /* the commit frame's page: the whole commit goes out now */
    const uint8_t *h = p;
    f->commit = amt == WAL_FRAME_HDRSIZE && (h[4] | h[5] | h[6] | h[7]) != 0;
    return SQLITE_OK;
}

#define WV_FLUSHED(f) do { if ((f)->len) { int rc_ = wv_flush(f); if (rc_) return rc_; } } while (0)
static int wv_close(sqlite3_file *pf) {
    walf *f = (walf *)pf;
    int rc = f->len ? wv_flush(f) : SQLITE_OK;
    int rc2 = f->real->pMethods ? f->real->pMethods->xClose(f->real) : SQLITE_OK;
    free(f->buf); f->buf = 0; f->cap = 0;
    return rc ? rc : rc2;
}
static int wv_read(sqlite3_file *pf, void *p, int amt, sqlite3_int64 off) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xRead(f->real, p, amt, off); }
static int wv_truncate(sqlite3_file *pf, sqlite3_int64 size) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xTruncate(f->real, size); }
static int wv_sync(sqlite3_file *pf, int flags) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xSync(f->real, flags); }
static int wv_size(sqlite3_file *pf, sqlite3_int64 *size) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xFileSize(f->real, size); }
static int wv_lock(sqlite3_file *pf, int l) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xLock(f->real, l); }
static int wv_unlock(sqlite3_file *pf, int l) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xUnlock(f->real, l); }
static int wv_reserved(sqlite3_file *pf, int *out) { walf *f = (walf *)pf; return f->real->pMethods->xCheckReservedLock(f->real, out); }
static int wv_fcntl(sqlite3_file *pf, int op, void *arg) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xFileControl(f->real, op, arg); }
static int wv_sector(sqlite3_file *pf) { walf *f = (walf *)pf; return f->real->pMethods->xSectorSize(f->real); }
static int wv_devchar(sqlite3_file *pf) { walf *f = (walf *)pf; return f->real->pMethods->xDeviceCharacteristics(f->real); }
static int wv_shmmap(sqlite3_file *pf, int i, int sz, int ext, void volatile **pp) { walf *f = (walf *)pf; return f->real->pMethods->xShmMap(f->real, i, sz, ext, pp); }
static int wv_shmlock(sqlite3_file *pf, int o, int n, int fl) { walf *f = (walf *)pf; return f->real->pMethods->xShmLock(f->real, o, n, fl); }
static void wv_shmbarrier(sqlite3_file *pf) { walf *f = (walf *)pf; f->real->pMethods->xShmBarrier(f->real); }
static int wv_shmunmap(sqlite3_file *pf, int del) { walf *f = (walf *)pf; return f->real->pMethods->xShmUnmap(f->real, del); }
static int wv_fetch(sqlite3_file *pf, sqlite3_int64 off, int amt, void **pp) { walf *f = (walf *)pf; WV_FLUSHED(f); return f->real->pMethods->xFetch(f->real, off, amt, pp); }
static int wv_unfetch(sqlite3_file *pf, sqlite3_int64 off, void *p) { walf *f = (walf *)pf; return f->real->pMethods->xUnfetch(f->real, off, p); }

static int wv_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *pf, int flags, int *outFlags) {
    (void)vfs;
    if (!(flags & SQLITE_OPEN_WAL)) return wv_base->xOpen(wv_base, name, pf, flags, outFlags);   /* the base's own object */
    walf *f = (walf *)pf;
    memset(f, 0, sizeof *f);
    f->real = (sqlite3_file *)((uint8_t *)pf + ((sizeof(walf) + 15) & ~(size_t)15));
    int rc = wv_base->xOpen(wv_base, name, f->real, flags, outFlags);
    if (rc != SQLITE_OK) { f->base.pMethods = 0; return rc; }
    const sqlite3_io_methods *m = f->real->pMethods;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!wv_io.xWrite) wv_io = (sqlite3_io_methods){m->iVersion, wv_close, wv_read, wv_write, wv_truncate, wv_sync, wv_size, wv_lock,
                                 wv_unlock, wv_reserved, wv_fcntl, wv_sector, wv_devchar, wv_shmmap, wv_shmlock,
                                 wv_shmbarrier, wv_shmunmap, wv_fetch, wv_unfetch};
    pthread_mutex_unlock(&mu);
    f->base.pMethods = &wv_io;
    return SQLITE_OK;
}

/* Registers "knwal" over the base VFS (name, or NULL = the default). Returns its name, or NULL (stock VFS). */
static const char *wv_install(const char *base) {
    if (getenv("KN_WALVFS") && getenv("KN_WALVFS")[0] == '0') return 0;
    wv_base = sqlite3_vfs_find(base && base[0] ? base : 0);
    if (!wv_base) return 0;
    wv_vfs = *wv_base;
    wv_vfs.zName = "knwal";
    wv_vfs.szOsFile = (int)((sizeof(walf) + 15) & ~(size_t)15) + wv_base->szOsFile;
    wv_vfs.pNext = 0;
    wv_vfs.xOpen = wv_open;
    /* every other method (delete, access, full pathname, randomness, sleep, time, ...) is the base's: they get
     * &wv_vfs as their first argument, and the unix VFS methods read only pAppData from it, which is the base's */
    if (sqlite3_vfs_register(&wv_vfs, 0) != SQLITE_OK) return 0;
    return wv_vfs.zName;
}

/* ------------------------------------------------------------------ open */
#ifndef CACHE_KB_DEFAULT
#define CACHE_KB_DEFAULT 65536
#endif

static int wprep(wstmts *w, sqlite3 *db, char *err, int errcap) {
    w->db = db;
    return prep(db, "BEGIN IMMEDIATE", &w->begin, err, errcap) ||
        prep(db, "COMMIT", &w->commit, err, errcap) ||
        prep(db, "ROLLBACK", &w->rollback, err, errcap) ||
        prep(db, "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING '{\"post\":'||"
                 "json_object('id',id,'body',body,'created_at',created_at,'author',?3,'like_count',0)||'}'",
             &w->insert, err, errcap) ||
        prep(db, "SELECT 1 FROM posts WHERE id=?1", &w->exists, err, errcap) ||
        prep(db, "INSERT INTO likes(user_id,post_id) VALUES(?1,?2) ON CONFLICT(user_id,post_id) DO NOTHING",
             &w->like, err, errcap);
}

#define POST_JSON \
    "json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username," \
    "'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))"

int db_open(const char *path, const char *vfs, int btree, char *err, int errcap) {
    snprintf(g_path, sizeof g_path, "%s", path);
    snprintf(g_vfs, sizeof g_vfs, "%s", vfs ? vfs : "");
    pc_install();
    const char *wv = wv_install(g_vfs);
    if (wv) snprintf(g_vfs, sizeof g_vfs, "%s", wv);
    keep_snapshot = !(getenv("KN_SNAPSHOT") && getenv("KN_SNAPSHOT")[0] == '0');
    committer_only = getenv("KN_COMMITTER") && getenv("KN_COMMITTER")[0] == '1';
    if (getenv("KN_SLOW_MS")) slow_ns = atoll(getenv("KN_SLOW_MS")) * 1000000;
    if (getenv("KN_SLOW_MEM_PCT")) slow_mem_pct = atoi(getenv("KN_SLOW_MEM_PCT"));
    if (getenv("KN_SLOW_CK_MS")) slow_ck_ns = atoll(getenv("KN_SLOW_CK_MS")) * 1000000;
    if (getenv("KN_SLOW_HOLD_MS")) slow_hold_ms = slow_hold0_ms = atoll(getenv("KN_SLOW_HOLD_MS"));
    if (getenv("KN_TEST_WAL_DELAY_US")) test_wal_delay_us = atoi(getenv("KN_TEST_WAL_DELAY_US"));
    /* Reader page cache (KN_CACHE_KB, default CACHE_KB_DEFAULT); the writer and the checkpointer touch far fewer
     * pages (16 MiB / 2 MiB): at ~400k connections the 2 GB box is short of memory, not CPU. */
    char rcache[64];
    snprintf(rcache, sizeof rcache, "PRAGMA cache_size=-%d", getenv("KN_CACHE_KB") ? atoi(getenv("KN_CACHE_KB")) : CACHE_KB_DEFAULT);
    if (open_conn(&rdb, 5000, err, errcap, rcache) ||
        /* The committer thread is off the loop, so its BEGIN can wait out a checkpoint hold that a slow disk
         * stretches (measured: 2 x 1 s was not enough at 416k users on a busy disk; 15 likes got a 500). */
        open_conn(&wdb, 10000, err, errcap, "PRAGMA cache_size=-16384")) return -1;
    if (prep(rdb, "BEGIN", &st_rbegin, err, errcap) ||
        prep(rdb, "COMMIT", &st_rcommit, err, errcap) ||
        prep(rdb, "SELECT 1", &st_health, err, errcap) ||
        prep(rdb,
             "SELECT '{\"posts\":['||coalesce(group_concat(j,',' ORDER BY ca DESC,id DESC),'')||']}' FROM ("
             "SELECT p.id id,p.created_at ca," POST_JSON " j FROM posts p JOIN users u ON u.id=p.user_id "
             "ORDER BY p.created_at DESC,p.id DESC LIMIT 20)", &st_feed_sql, err, errcap) ||
        prep(rdb, "SELECT '{\"post\":'||" POST_JSON "||'}' FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1",
             &st_post_sql, err, errcap) ||
        wprep(&W_loop, rdb, err, errcap) || wprep(&W_thr, wdb, err, errcap)) return -1;
    use_btree = 0;
    if (btree) {
        char why[256];
        if (bt_init(rdb, why, sizeof why) == 0) use_btree = 1;
        else fprintf(stderr, "btree reads off (SQL only): %s\n", why);
    }
    return 0;
}

/* ------------------------------------------------------------------ reads */
int db_health(char *err, int errcap) {
    if (rd_begin() != 0) { snprintf(err, errcap, "%s", sqlite3_errmsg(rdb)); return -1; }
    int rc = sqlite3_step(st_health);
    if (rc != SQLITE_ROW) {
        snprintf(err, errcap, "%s", sqlite3_errmsg(rdb));
        sqlite3_reset(st_health);
        rd_end();
        return -1;
    }
    sqlite3_reset(st_health);
    return 0;
}

static int feed_sql(uint8_t *out, int cap) {
    int n = -1;
    if (sqlite3_step(st_feed_sql) == SQLITE_ROW) n = copy_text(st_feed_sql, 0, out, cap);
    sqlite3_reset(st_feed_sql);
    return n;
}

static int post_sql(int64_t id, uint8_t *out, int cap) {
    sqlite3_bind_int64(st_post_sql, 1, id);
    int rc = sqlite3_step(st_post_sql), n;
    if (rc == SQLITE_ROW) n = copy_text(st_post_sql, 0, out, cap);
    else n = rc == SQLITE_DONE ? 0 : -1;
    sqlite3_reset(st_post_sql);
    return n;
}

int db_feed(uint8_t *out, int cap) {
    if (rd_begin() != 0) return -1;
    if (use_btree) {
        int n = bt_feed(out, cap);
        if (n > 0) { n_bt++; return n; }
    }
    n_sql++;
    return feed_sql(out, cap);
}

int db_post(int64_t id, uint8_t *out, int cap) {
    if (rd_begin() != 0) return -1;
    if (use_btree) {
        int n = bt_post(id, out, cap);
        if (n >= 0) { n_bt++; return n; }
    }
    n_sql++;
    return post_sql(id, out, cap);
}

/* ------------------------------------------------------------------ writes (group commit: on the loop, or the committer thread) */
typedef struct wop {
    int kind;                 /* 0 like, 1 post */
    int64_t uid, pid;
    int boff, blen, aoff, alen;   /* body/author bytes in the batch's arena */
    int code, joff, jlen;     /* results */
} wop;

typedef struct wbatch {
    wop *ops;
    int n, cap;
    uint8_t *arena;           /* staged body/author bytes, then result JSON */
    int alen, acap;
} wbatch;

static wbatch wb[2];
static wbatch *staging = &wb[0], *inflight = 0;
static pthread_mutex_t wq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wq_cv = PTHREAD_COND_INITIALIZER;
static int wq_state;          /* 0 idle, 1 batch handed over, 2 batch done */
static int wq_efd = -1;

static int arena_put(wbatch *b, const uint8_t *p, int n) {
    if (b->alen + n > b->acap) {
        int cap = b->acap ? b->acap : 1 << 16;
        while (cap < b->alen + n) cap *= 2;
        uint8_t *a = realloc(b->arena, (size_t)cap);
        if (!a) return -1;
        b->arena = a; b->acap = cap;
    }
    memcpy(b->arena + b->alen, p, (size_t)n);
    b->alen += n;
    return b->alen - n;
}

static wop *wq_push(void) {
    wbatch *b = staging;
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 256;
        wop *o = realloc(b->ops, sizeof(wop) * (size_t)cap);
        if (!o) return 0;
        b->ops = o; b->cap = cap;
    }
    wop *o = &b->ops[b->n++];
    memset(o, 0, sizeof *o);
    return o;
}

int db_wq_add_like(int64_t uid, int64_t pid) {
    wop *o = wq_push();
    if (!o) return -1;
    o->kind = 0; o->uid = uid; o->pid = pid;
    return 0;
}

int db_wq_add_post(int64_t uid, const uint8_t *body, int blen, const uint8_t *author, int alen) {
    wop *o = wq_push();
    if (!o) return -1;
    o->kind = 1; o->uid = uid;
    o->boff = arena_put(staging, body, blen); o->blen = blen;
    o->aoff = arena_put(staging, author, alen); o->alen = alen;
    if (o->boff < 0 || o->aoff < 0) { staging->n--; return -1; }
    return 0;
}

int db_wq_start(void) {
    if (inflight) return 1;
    pthread_mutex_lock(&wq_mu);
    inflight = staging;
    staging = staging == &wb[0] ? &wb[1] : &wb[0];
    staging->n = 0; staging->alen = 0;
    if (!committer_only && now_ns() >= thr_until_ns) {
        /* on the loop: now, or at a later pass end (db_pass_end) if a checkpoint holds the write lock */
        pthread_mutex_unlock(&wq_mu);
        ol_pending = 1;
        ol_try();
        return ol_pending ? 0 : 2;
    }
    wq_state = 1;
    pthread_cond_signal(&wq_cv);
    pthread_mutex_unlock(&wq_mu);
    return 0;
}

int db_wq_done(void) {
    if (!inflight || ol_pending) return -1;
    pthread_mutex_lock(&wq_mu);
    int done = wq_state == 2;
    pthread_mutex_unlock(&wq_mu);
    return done ? inflight->n : -1;
}

int db_wq_result(int i, int *code, uint8_t *out, int cap) {
    wop *o = &inflight->ops[i];
    *code = o->code;
    if (o->kind == 1 && o->code == 201) {
        if (o->jlen > cap) { *code = -2; return 0; }
        memcpy(out, inflight->arena + o->joff, (size_t)o->jlen);
        return o->jlen;
    }
    return 0;
}

void db_wq_release(void) {
    if (!inflight) return;
    pthread_mutex_lock(&wq_mu);
    wq_state = 0;
    pthread_mutex_unlock(&wq_mu);
    inflight = 0;
    rd_end();   /* the next read takes a new snapshot that includes this commit */
}

static void run_body(wstmts *w, wbatch *b, int rc);

/* On the committer thread: BEGIN IMMEDIATE (waits out a checkpoint hold here, off the loop), the statements, COMMIT.
 * After a slow commit here the fallback period doubles (up to 5 min); a fast one lets it end on schedule. */
static void run_batch(wbatch *b) {
    int64_t t0 = now_ns();
    int rc = SQLITE_BUSY;
    for (int tries = 0; tries < 2 && (rc & 0xff) == SQLITE_BUSY; tries++) rc = step_reset(W_thr.begin);
    run_body(&W_thr, b, rc);
    n_thr_commits++;
    if (now_ns() - t0 > slow_ns) slow_seen();
}

/* On the loop, on the reader connection: BEGIN IMMEDIATE without waiting (a checkpoint RESTART can hold the write
 * lock; then the batch stays staged and the next pass end tries again while reads go on), the statements, COMMIT.
 * Results are ready when this returns with ol_pending == 0 (the loop answers them at once, no eventfd). */
static void ol_try(void) {
    rd_end();   /* BEGIN IMMEDIATE needs the connection out of its read transaction */
    int64_t t0 = now_ns();
    sqlite3_busy_timeout(rdb, 0);
    int rc = step_reset(W_loop.begin);
    sqlite3_busy_timeout(rdb, 5000);
    if ((rc & 0xff) == SQLITE_BUSY) { n_ol_busy++; return; }
    ol_pending = 0;
    run_body(&W_loop, inflight, rc);
    n_ol_commits++;
    int64_t dt = now_ns() - t0;
    if (dt > slow_ns) {
        n_slow++;
        slow_seen();
        fprintf(stderr, "group commit: slow commit on the loop (%lld ms, %d writes): committer thread for %lld s\n",
                (long long)(dt / 1000000), inflight->n, (long long)(slow_hold_ms / 1000));
    }
    pthread_mutex_lock(&wq_mu);
    wq_state = 2;
    pthread_mutex_unlock(&wq_mu);
}

static void run_body(wstmts *w, wbatch *b, int rc) {
    sqlite3 *db = w->db;
    int ok = rc == SQLITE_OK;
    if (!ok) fprintf(stderr, "group commit: BEGIN: %s\n", sqlite3_errmsg(db));
    for (int i = 0; ok && i < b->n; i++) {
        wop *o = &b->ops[i];
        if (o->kind == 0) {
            sqlite3_bind_int64(w->exists, 1, o->pid);
            rc = sqlite3_step(w->exists);
            sqlite3_reset(w->exists);
            if (rc == SQLITE_DONE) { o->code = -1; continue; }
            if (rc != SQLITE_ROW) { o->code = -2; continue; }
            sqlite3_bind_int64(w->like, 1, o->uid);
            sqlite3_bind_int64(w->like, 2, o->pid);
            rc = sqlite3_step(w->like);
            sqlite3_reset(w->like);
            o->code = rc != SQLITE_DONE ? -2 : sqlite3_changes(db) > 0 ? 1 : 0;
        } else {
            sqlite3_bind_int64(w->insert, 1, o->uid);
            sqlite3_bind_text(w->insert, 2, (const char *)b->arena + o->boff, o->blen, SQLITE_TRANSIENT);
            sqlite3_bind_text(w->insert, 3, (const char *)b->arena + o->aoff, o->alen, SQLITE_TRANSIENT);
            o->code = -2;
            if (sqlite3_step(w->insert) == SQLITE_ROW) {
                int n = sqlite3_column_bytes(w->insert, 0);
                const unsigned char *t = sqlite3_column_text(w->insert, 0);
                int off = t ? arena_put(b, t, n) : -1;
                if (off >= 0) { o->joff = off; o->jlen = n; o->code = 201; }
                while (sqlite3_step(w->insert) == SQLITE_ROW) {}
            }
            sqlite3_reset(w->insert);
            sqlite3_clear_bindings(w->insert);
        }
    }
    if (ok && step_reset(w->commit) != SQLITE_OK) {
        fprintf(stderr, "group commit: COMMIT: %s\n", sqlite3_errmsg(db));
        step_reset(w->rollback);
        ok = 0;
    }
    if (!ok) for (int i = 0; i < b->n; i++) b->ops[i].code = -2;
}

static void *wq_main(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "kn-commit");
    for (;;) {
        pthread_mutex_lock(&wq_mu);
        while (wq_state != 1) pthread_cond_wait(&wq_cv, &wq_mu);
        wbatch *b = inflight;
        pthread_mutex_unlock(&wq_mu);
        run_batch(b);
        pthread_mutex_lock(&wq_mu);
        wq_state = 2;
        pthread_mutex_unlock(&wq_mu);
        uint64_t one = 1;
        if (write(wq_efd, &one, sizeof one) < 0) perror("committer eventfd");
    }
    return 0;
}

int db_wq_init(void) {
    wq_efd = eventfd(0, EFD_CLOEXEC);
    if (wq_efd < 0) return -1;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 512 * 1024);
    return pthread_create(&t, &a, wq_main, 0) == 0 ? wq_efd : -1;
}

/* ------------------------------------------------------------------ verification */
int db_verify(void) {
    static uint8_t a[1 << 16], b[1 << 16];
    int bad = 0;
    if (!use_btree) { fprintf(stderr, "verify: btree reads are off\n"); return 1; }
    rd_end();
    if (rd_begin() != 0) return 1;
    sqlite3_stmt *mx;
    int64_t maxid = 0;
    if (sqlite3_prepare_v2(rdb, "SELECT max(id) FROM posts", -1, &mx, 0) == SQLITE_OK) {
        if (sqlite3_step(mx) == SQLITE_ROW) maxid = sqlite3_column_int64(mx, 0);
        sqlite3_finalize(mx);
    }
    int64_t checked = 0;
    for (int64_t id = 1; id <= maxid + 10; id++) {
        int n1 = bt_post(id, a, sizeof a), n2 = post_sql(id, b, sizeof b);
        checked++;
        if (n1 != n2 || (n1 > 0 && memcmp(a, b, n1) != 0)) {
            if (bad++ < 5) fprintf(stderr, "verify: post %lld: btree %d bytes, sql %d bytes\n  bt : %.*s\n  sql: %.*s\n",
                                   (long long)id, n1, n2, n1 > 0 ? n1 : 0, a, n2 > 0 ? n2 : 0, b);
        }
    }
    int n1 = bt_feed(a, sizeof a), n2 = feed_sql(b, sizeof b);
    if (n1 != n2 || n1 <= 0 || memcmp(a, b, n1) != 0) {
        bad++;
        fprintf(stderr, "verify: feed: btree %d bytes, sql %d bytes\n", n1, n2);
    }
    /* timing, same snapshot: 5,000 feeds and 5,000 random posts each way */
    int64_t t0 = now_ns();
    for (int i = 0; i < 5000; i++) bt_feed(a, sizeof a);
    int64_t t1 = now_ns();
    for (int i = 0; i < 5000; i++) feed_sql(b, sizeof b);
    int64_t t2 = now_ns();
    uint64_t x = 88172645463325252ULL;
    for (int i = 0; i < 5000; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; bt_post((int64_t)(x % (uint64_t)maxid) + 1, a, sizeof a); }
    int64_t t3 = now_ns();
    x = 88172645463325252ULL;
    for (int i = 0; i < 5000; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; post_sql((int64_t)(x % (uint64_t)maxid) + 1, b, sizeof b); }
    int64_t t4 = now_ns();
    fprintf(stderr, "verify: feed btree %.2f us, sql %.2f us; post btree %.2f us, sql %.2f us\n",
            (t1 - t0) / 5000e3, (t2 - t1) / 5000e3, (t3 - t2) / 5000e3, (t4 - t3) / 5000e3);
    rd_end();
    fprintf(stderr, "verify: %lld post ids + the feed checked, %d mismatches\n", (long long)checked, bad);
    return bad;
}

int64_t db_stat(int which) {
    switch (which) {
    case 0: return n_bt;
    case 1: return n_sql;
    case 2: return n_snap;
    case 3: return n_parked;
    case 4: return n_wal_writes;
    case 5: return n_wal_flushes;
    case 6: return n_ol_commits;
    case 7: return n_thr_commits;
    case 8: return n_ol_busy;
    case 9: return n_slow;
    case 10: return n_ck_slow;
    case 11: return n_mem_slow;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ memory use (for the slow-disk guard)
 * The cgroup's memory.current / memory.max if this process's cgroup (v2) has a limit, else 1 - MemAvailable /
 * MemTotal. Files stay open and are re-read with pread. Returns percent, or -1 if unknown. */
static int mem_cur = -1, mem_info = -1;
static int64_t mem_max;

static int64_t read_num(int fd, const char *key) {
    char b[4096];
    ssize_t n = pread(fd, b, sizeof b - 1, 0);
    if (n <= 0) return -1;
    b[n] = 0;
    if (!key) return atoll(b);
    char *p = strstr(b, key);
    return p ? atoll(p + strlen(key)) : -1;
}

static void mem_init(void) {
    FILE *f = fopen("/proc/self/cgroup", "r");
    char line[4096], path[4400];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "0::", 3)) continue;
            line[strcspn(line, "\n")] = 0;
            snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.max", line + 3);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                char b[64];
                ssize_t n = pread(fd, b, sizeof b - 1, 0);
                close(fd);
                if (n > 0 && b[0] >= '0' && b[0] <= '9') {
                    mem_max = atoll(b);
                    snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.current", line + 3);
                    mem_cur = open(path, O_RDONLY | O_CLOEXEC);
                }
            }
        }
        fclose(f);
    }
    if (mem_cur < 0) mem_info = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
}

static int mem_pct(void) {
    if (mem_cur >= 0 && mem_max > 0) { int64_t c = read_num(mem_cur, 0); return c < 0 ? -1 : (int)(c * 100 / mem_max); }
    if (mem_info >= 0) {
        int64_t t = read_num(mem_info, "MemTotal:"), a = read_num(mem_info, "MemAvailable:");
        return t > 0 && a >= 0 ? (int)(100 - a * 100 / t) : -1;
    }
    return -1;
}

/* ------------------------------------------------------------------ checkpointer */
#define CK_TICK_MS 50
#define CK_RESTART_FRAMES 4096   /* 16 MB of 4 KB pages */
#define CK_TAIL_FRAMES 128
#define CK_HOLD_MS 250

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void *ckpt_main(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "kn-ckpt");
    /* nice 5 for this thread only: at 19 it starves under saturation, at 0 it competes with the loop as an equal. */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
    sqlite3 *c = 0;
    char err[256];
    if (open_conn(&c, CK_HOLD_MS, err, sizeof err, "PRAGMA cache_size=-2048")) { fprintf(stderr, "checkpointer: %s\n", err); return 0; }
    int logst = getenv("KN_CKPT_LOG") && getenv("KN_CKPT_LOG")[0] == '1';
    int64_t last = now_ns(), maxhold = 0, sumhold = 0;
    mem_init();
    int restarts = 0, fails = 0, maxlog = 0;
    for (;;) {
        usleep(CK_TICK_MS * 1000);
        int log = 0, copied = 0;
        int64_t tp = now_ns();
        sqlite3_wal_checkpoint_v2(c, 0, SQLITE_CHECKPOINT_PASSIVE, &log, &copied);
        if (now_ns() - tp > slow_ck_ns) { n_ck_slow++; slow_seen(); }   /* slow disk: keep commits off the loop */
        if (slow_mem_pct > 0 && slow_mem_pct < 100 && mem_pct() >= slow_mem_pct) { n_mem_slow++; slow_seen(); }
        if (log > maxlog) maxlog = log;
        if (log >= CK_RESTART_FRAMES && log - copied <= CK_TAIL_FRAMES) {
            /* RESTART takes the WAL write lock (our writer's BEGIN IMMEDIATE gets SQLITE_BUSY and retries next pass),
             * copies the tail and waits for readers to leave the old frames; the next write then starts the WAL
             * from frame 1. The loop drops its kept snapshot while ckpt_want is set. */
            atomic_store(&ckpt_want, 1);
            int64_t t0 = now_ns();
            int rc = sqlite3_wal_checkpoint_v2(c, 0, SQLITE_CHECKPOINT_RESTART, 0, 0);
            int64_t dt = now_ns() - t0;
            atomic_store(&ckpt_want, 0);
            if (rc == SQLITE_OK) restarts++; else fails++;
            sumhold += dt;
            if (dt > maxhold) maxhold = dt;
        }
        if (logst && now_ns() - last > 10000000000LL) {
            fprintf(stderr, "checkpointer: restarts=%d busy=%d maxLog=%d hold avg=%lldus max=%lldus\n", restarts, fails,
                    maxlog, restarts + fails ? (long long)(sumhold / (restarts + fails) / 1000) : 0LL,
                    (long long)(maxhold / 1000));
            restarts = fails = maxlog = 0;
            maxhold = sumhold = 0;
            last = now_ns();
        }
    }
    return 0;
}

int ckpt_start(void) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 * 1024);
    return pthread_create(&t, &a, ckpt_main, 0) == 0 ? 0 : -1;
}
