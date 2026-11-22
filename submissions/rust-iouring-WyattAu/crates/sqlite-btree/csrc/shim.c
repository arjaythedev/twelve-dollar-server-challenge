/*
 * shim.c — SQLite internal B-tree cursor API surface for sqlite-btree.
 *
 * #include's the UNMODIFIED amalgamation (pinned 3.54.1, sha256-checked by
 * build.sh) so SQLITE_PRIVATE internals become visible. SQLite keeps full
 * ownership of page walking, WAL, and locking; this file only:
 *   - opens one process-exclusive connection (unix-excl VFS, heap WAL index),
 *   - opens 4 read cursors (posts, users, posts_created_at_id_idx,
 *     likes_post_id_idx) after verifying the schema layout,
 *   - drives one read transaction per event-loop pass (BEGIN/COMMIT via
 *     prepared statements; writes on the same connection group-commit),
 *   - copies raw record/index payload bytes out for Rust to decode,
 *   - runs a nice-19 checkpoint thread driven by the WAL frame hook,
 *     holding new passes at most 250 ms (rule 6 durability unchanged).
 *
 * Expected schema (refuse to start on any mismatch — layout()):
 *   users(id INTEGER PRIMARY KEY, username TEXT, created_at TEXT)
 *   posts(id INTEGER PRIMARY KEY, user_id INT, body TEXT, created_at TEXT)
 *   likes(user_id INT, post_id INT, created_at TEXT, PK(user_id, post_id))
 *   posts_created_at_id_idx(created_at DESC, id DESC)
 *   likes_post_id_idx(post_id)
 */
#include "sqlite3.c"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* ---- state ------------------------------------------------------------ */
static sqlite3 *db;            /* the one read/write connection          */
static Btree *bt;              /* db->aDb[0].pBt                        */
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;
static sqlite3_stmt *st_health, *st_post, *st_like;
static KeyInfo *ki_feed, *ki_likes;

#define NCUR 4
#define CUR_POSTS 0
#define CUR_USERS 1
#define CUR_FEED_IDX 2
#define CUR_LIKES_IDX 3
static BtCursor *cur[NCUR];

/* spill buffer for payload copies (grown on demand, freed never — 2 GB cap) */
static u8 *spill;
static u32 spill_cap;

/* ---- checkpoint thread ------------------------------------------------ */
#define CKPT_FRAMES 4096       /* passive ckpt once the WAL grows this much */
#define CKPT_FORCE  65536      /* hold writes to force a full checkpoint   */
#define CKPT_NICE   19
#define HOLD_MAX_MS 250
static sqlite3 *ckdb;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static int ck_state;           /* 0 idle, 1 running (written under ck_mu)  */
static int ck_wanted;          /* hook says a checkpoint is due            */
static volatile int writes_held;

static void *ck_main(void *arg) {
    (void)arg;
    nice(CKPT_NICE);
    pthread_mutex_lock(&ck_mu);
    for (;;) {
        while (!ck_wanted) pthread_cond_wait(&ck_cv, &ck_mu);
        ck_wanted = 0;
        ck_state = 1;
        pthread_mutex_unlock(&ck_mu);

        for (;;) {
            int log_out = 0, ckpt_out = 0;
            int rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE,
                                               &log_out, &ckpt_out);
            if (rc != SQLITE_OK || log_out == 0) break;
            sched_yield();
        }

        pthread_mutex_lock(&ck_mu);
        ck_state = 0;
    }
    return 0;
}

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
    (void)arg; (void)d; (void)name;
    if (frames >= CKPT_FRAMES) {
        pthread_mutex_lock(&ck_mu);
        ck_wanted = 1;
        pthread_cond_signal(&ck_cv);
        pthread_mutex_unlock(&ck_mu);
    }
    if (frames >= CKPT_FORCE) {
        /* drain before the next pass's writes: hold up to HOLD_MAX_MS */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            pthread_mutex_lock(&ck_mu);
            int running = ck_state;
            pthread_mutex_unlock(&ck_mu);
            if (!running) break;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
            if (ms >= HOLD_MAX_MS) break;
            sched_yield();
        }
    }
    return SQLITE_OK;
}

/* ---- helpers ---------------------------------------------------------- */
static int fail(char *err, int n, const char *msg) {
    if (err && n > 0) snprintf(err, (size_t)n, "%s", msg);
    return -1;
}

static int fail_sqlite(char *err, int n, const char *what, sqlite3 *d) {
    if (err && n > 0) snprintf(err, (size_t)n, "%s: %s", what, d ? sqlite3_errmsg(d) : "?");
    return -1;
}

/* Compare a PRAGMA column list against the expected names, in order. */
static int layout(char *err, int n, const char *pragma, const char *want) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, pragma, -1, &st, 0) != SQLITE_OK)
        return fail_sqlite(err, n, pragma, db);
    char got[512] = {0};
    size_t off = 0;
    int columns = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (!name) { sqlite3_finalize(st); return fail(err, n, "null column name"); }
        if (columns++) off += (size_t)snprintf(got + off, sizeof got - off, ",");
        off += (size_t)snprintf(got + off, sizeof got - off, "%s", (const char *)name);
    }
    sqlite3_finalize(st);
    if (strcmp(got, want) != 0) {
        if (err && n > 0) snprintf(err, (size_t)n, "layout mismatch for %s: got [%s] want [%s]",
                                   pragma, got, want);
        return -1;
    }
    return 0;
}

static Pgno rootpage(const char *name, char *err, int n) {
    sqlite3_stmt *st;
    Pgno out = 0;
    const char *sql = "SELECT rootpage FROM sqlite_schema WHERE name=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
        return (Pgno)fail_sqlite(err, n, "schema query", db);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) out = (Pgno)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (!out) fail(err, n, "rootpage not found");
    return out;
}

static sqlite3_stmt *prep(const char *sql, char *err, int n) {
    sqlite3_stmt *st = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) {
        fail_sqlite(err, n, sql, db);
        return 0;
    }
    return st;
}

static int keyinfo_sort(KeyInfo *k, int fields, int desc, char *err, int n) {
    if (!k) return fail(err, n, "KeyInfoAlloc failed");
    if (desc) {
        for (int i = 0; i < fields; i++) k->aSortFlags[i] = KEYINFO_ORDER_DESC;
    }
    return 0;
}

/* ---- open ------------------------------------------------------------- */
int td_open(const char *path, char *err, int n) {
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(path, &db, flags, "unix-excl") != SQLITE_OK)
        return fail_sqlite(err, n, "open unix-excl", db);
    sqlite3_busy_timeout(db, 5000);

    const char *pragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",       /* rule 6: NORMAL or stronger */
        "PRAGMA wal_autocheckpoint=0",     /* we drive checkpoints       */
        "PRAGMA cache_size=-65536",        /* 64 MiB page cache          */
        "PRAGMA temp_store=MEMORY",
        "PRAGMA cache_spill=false",
    };
    for (unsigned i = 0; i < sizeof pragmas / sizeof pragmas[0]; i++) {
        if (sqlite3_exec(db, pragmas[i], 0, 0, 0) != SQLITE_OK)
            return fail_sqlite(err, n, pragmas[i], db);
    }

    /* verify the exact schema we decode (rule: refuse on mismatch) */
    if (layout(err, n, "PRAGMA table_info(users)",
               ",id,username,created_at") != 0) return -1;
    if (layout(err, n, "PRAGMA table_info(posts)",
               ",id,user_id,body,created_at") != 0) return -1;
    if (layout(err, n, "PRAGMA table_info(likes)",
               ",user_id,post_id,created_at") != 0) return -1;
    if (layout(err, n, "PRAGMA index_info(posts_created_at_id_idx)",
               ",created_at,id") != 0) return -1;
    if (layout(err, n, "PRAGMA index_info(likes_post_id_idx)",
               ",post_id") != 0) return -1;

    bt = db->aDb[0].pBt;
    if (!bt) return fail(err, n, "no main btree");

    /* key infos: feed index = (created_at DESC, id DESC, rowid DESC),
       likes index = (post_id ASC, rowid ASC) */
    ki_feed = sqlite3KeyInfoAlloc(db, 3, 0);
    if (keyinfo_sort(ki_feed, 3, 1, err, n) != 0) return -1;
    ki_likes = sqlite3KeyInfoAlloc(db, 2, 0);
    if (keyinfo_sort(ki_likes, 2, 0, err, n) != 0) return -1;

    Pgno rp_posts = rootpage("posts", err, n);
    if (!rp_posts) return -1;
    Pgno rp_users = rootpage("users", err, n);
    if (!rp_users) return -1;
    Pgno rp_feed = rootpage("posts_created_at_id_idx", err, n);
    if (!rp_feed) return -1;
    Pgno rp_likes = rootpage("likes_post_id_idx", err, n);
    if (!rp_likes) return -1;

    Pgno roots[NCUR] = { rp_posts, rp_users, rp_feed, rp_likes };
    for (int i = 0; i < NCUR; i++) {
        cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
        if (!cur[i]) return fail(err, n, "cursor alloc");
        sqlite3BtreeCursorZero(cur[i]);
        /* table cursors take no KeyInfo; index cursors take the matching one */
        KeyInfo *ki = (i == CUR_FEED_IDX) ? ki_feed : (i == CUR_LIKES_IDX) ? ki_likes : 0;
        if (sqlite3BtreeCursor(bt, roots[i], 0, ki, cur[i]) != SQLITE_OK)
            return fail(err, n, "btree cursor open");
    }

    st_begin   = prep("BEGIN DEFERRED", err, n);   if (!st_begin)   return -1;
    st_commit  = prep("COMMIT", err, n);           if (!st_commit)  return -1;
    st_rollback= prep("ROLLBACK", err, n);         if (!st_rollback)return -1;
    st_health  = prep("SELECT 1", err, n);         if (!st_health)  return -1;
    st_post    = prep(
        "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
        err, n);
    if (!st_post) return -1;
    st_like    = prep(
        "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) "
        "ON CONFLICT (user_id, post_id) DO NOTHING",
        err, n);
    if (!st_like) return -1;

    sqlite3_wal_hook(db, wal_hook, 0);

    /* checkpoint thread on its own read-only connection (same process:
       unix-excl shares the heap WAL index, no -shm file) */
    if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READONLY, "unix-excl") != SQLITE_OK)
        return fail_sqlite(err, n, "ckdb open", ckdb);
    pthread_t th;
    if (pthread_create(&th, 0, ck_main, 0) != 0) return fail(err, n, "ckpt thread");
    pthread_detach(th);
    return 0;
}

/* ---- pass transactions ------------------------------------------------- */
int td_begin(void) {
    int rc = sqlite3_step(st_begin);
    sqlite3_reset(st_begin);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int td_commit(void) {
    int rc = sqlite3_step(st_commit);
    sqlite3_reset(st_commit);
    if (rc == SQLITE_DONE) {
        int frames = sqlite3_wal_hook_frames(db);   /* NOT public — see note */
        (void)frames;
        return SQLITE_OK;
    }
    return rc ? rc : SQLITE_ERROR;
}

int td_rollback(void) {
    int rc = sqlite3_step(st_rollback);
    sqlite3_reset(st_rollback);
    return rc == SQLITE_DONE ? SQLITE_OK : (rc ? rc : SQLITE_ERROR);
}

/* ---- payload copies ---------------------------------------------------- */
static int copy_payload(BtCursor *c, u8 *out, int cap, int *out_n) {
    u32 amt = sqlite3BtreePayloadSize(c);
    if ((int)amt > cap) {
        /* grow the shared spill and copy from there; caller re-reads */
        if (amt > spill_cap) {
            u8 *bigger = sqlite3_realloc(spill, amt);
            if (!bigger) return SQLITE_NOMEM;
            spill = bigger;
            spill_cap = amt;
        }
        if (sqlite3BtreePayload(c, 0, amt, spill) != SQLITE_OK) return SQLITE_CORRUPT;
        if ((int)amt > cap) return 2; /* NEEDS_BIG: signal caller */
        memcpy(out, spill, amt);
    } else {
        if (sqlite3BtreePayload(c, 0, amt, out) != SQLITE_OK) return SQLITE_CORRUPT;
    }
    *out_n = (int)amt;
    return SQLITE_OK;
}

/* ---- reads ------------------------------------------------------------- */
/* feed index walk: 1 = positioned on an entry, 0 = EOF, <0 = error */
int td_feed_first(void) {
    int res = 1;
    int rc = sqlite3BtreeFirst(cur[CUR_FEED_IDX], &res);
    if (rc != SQLITE_OK) return rc < 0 ? rc : -abs(rc);
    return res == 0 ? 1 : 0;
}

int td_feed_next(void) {
    int rc = sqlite3BtreeNext(cur[CUR_FEED_IDX], 1);
    if (rc != SQLITE_OK) return rc < 0 ? rc : -abs(rc);
    return sqlite3BtreeEof(cur[CUR_FEED_IDX]) ? 0 : 1;
}

/* copy the current feed-index record (created_at, id, rowid) */
int td_feed_payload(u8 *out, int cap, int *out_n) {
    return copy_payload(cur[CUR_FEED_IDX], out, cap, out_n);
}

/* table row by rowid. returns SQLITE_OK(0) + bytes, 1 = not found, <0 = error */
int td_table_row(int which, i64 rowid, u8 *out, int cap, int *out_n) {
    if (which != CUR_POSTS && which != CUR_USERS) return -1;
    int res = 1;
    int rc = sqlite3BtreeTableMoveto(cur[which], rowid, 0, &res);
    if (rc != SQLITE_OK) return rc < 0 ? rc : -abs(rc);
    if (res != 0) return 1;
    return copy_payload(cur[which], out, cap, out_n);
}

/* like count for one post: seek (post_id) prefix on likes_post_id_idx,
   then walk while field 0 == post_id. returns 0 + count, <0 = error */
int td_like_count(i64 post_id, i64 *out_count) {
    /* seek key: single integer Mem */
    Mem mem;
    memset(&mem, 0, sizeof mem);
    mem.flags = MEM_Int;
    mem.u.i = post_id;
    mem.enc = SQLITE_UTF8;
    UnpackedRecord unr;
    memset(&unr, 0, sizeof unr);
    unr.pKeyInfo = ki_likes;
    unr.aMem = &mem;
    unr.n = 1;

    int res = 1;
    int rc = sqlite3BtreeIndexMoveto(cur[CUR_LIKES_IDX], &unr, &res);
    if (rc != SQLITE_OK) return rc < 0 ? rc : -abs(rc);

    i64 count = 0;
    Mem fields[2];
    UnpackedRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.pKeyInfo = ki_likes;
    rec.aMem = fields;

    for (;;) {
        if (sqlite3BtreeEof(cur[CUR_LIKES_IDX])) break;
        u32 amt = sqlite3BtreePayloadSize(cur[CUR_LIKES_IDX]);
        if (amt > spill_cap) {
            u8 *bigger = sqlite3_realloc(spill, amt);
            if (!bigger) return SQLITE_NOMEM;
            spill = bigger;
            spill_cap = amt;
        }
        if (sqlite3BtreePayload(cur[CUR_LIKES_IDX], 0, amt, spill) != SQLITE_OK)
            return SQLITE_CORRUPT;
        sqlite3VdbeRecordUnpack((int)amt, spill, &rec);
        if (fields[0].u.i != post_id) break;   /* walked past this post */
        count++;
        rc = sqlite3BtreeNext(cur[CUR_LIKES_IDX], 1);
        if (rc != SQLITE_OK) return rc < 0 ? rc : -abs(rc);
    }
    *out_count = count;
    return SQLITE_OK;
}

/* ---- writes (SQL, on the pass's transaction → group commit) ------------ */
int td_post(i64 user_id, const u8 *body, int body_len,
            i64 *out_id, u8 *out_created, int cap, int *created_len) {
    sqlite3_reset(st_post);
    sqlite3_bind_int64(st_post, 1, user_id);
    sqlite3_bind_text(st_post, 2, (const char *)body, body_len, SQLITE_STATIC);
    int rc = sqlite3_step(st_post);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(st_post);
        return rc == SQLITE_DONE ? SQLITE_CORRUPT : (rc ? rc : SQLITE_ERROR);
    }
    *out_id = sqlite3_column_int64(st_post, 0);
    const unsigned char *created = sqlite3_column_text(st_post, 1);
    int clen = sqlite3_column_bytes(st_post, 1);
    if (clen > cap) { sqlite3_reset(st_post); return SQLITE_TOOBIG; }
    memcpy(out_created, created, (size_t)clen);
    *created_len = clen;
    sqlite3_reset(st_post);
    return SQLITE_OK;
}

/* returns 0 = first like (201), 1 = already liked (200), <0 = error */
int td_like(i64 user_id, i64 post_id) {
    sqlite3_reset(st_like);
    sqlite3_bind_int64(st_like, 1, user_id);
    sqlite3_bind_int64(st_like, 2, post_id);
    int rc = sqlite3_step(st_like);
    sqlite3_reset(st_like);
    if (rc != SQLITE_DONE) return rc ? rc : SQLITE_ERROR;
    return sqlite3_changes(db) == 0 ? 1 : 0;
}

/* health: SELECT 1 */
int td_health(void) {
    int rc = sqlite3_step(st_health);
    sqlite3_reset(st_health);
    return rc == SQLITE_ROW ? SQLITE_OK : (rc ? rc : SQLITE_ERROR);
}
