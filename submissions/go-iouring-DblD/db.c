// See db.h. Plain SQL through prepared statements (B-tree reads replace parts of this in stage 5),
// one transaction per loop pass, and a background thread that checkpoints the WAL.
#define _GNU_SOURCE // before any header: SQLite uses mremap
#include "db.h"
// The whole of SQLite is compiled as part of THIS file (the amalgamation is one C file made
// to be used this way). That makes its internal B-tree functions callable from the read
// path below; the source itself is the unmodified official release.
#include "sqlite/sqlite3.c"
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static sqlite3 *db;
static sqlite3_stmt *st_health, *st_feed, *st_post, *st_exists, *st_like, *st_create;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;

// ---- Checkpointing, off the loop ------------------------------------------------------
//
// In WAL mode a commit only appends the changed pages to the -wal file. A CHECKPOINT later
// copies them into the database file and syncs it. By default SQLite does that inside the
// commit that pushes the WAL past 1,000 pages, which means on our loop, with every other
// request waiting behind it. Here a second connection on its own thread does it instead.
//
// The box has ONE CPU, so this thread's work is taken straight from the loop. Every
// checkpoint sorts the WAL's whole index and syncs two files, however little it then copies.
// The first version ran one whenever the WAL was over 1,000 pages: under load that was 30 a
// second, none of which could finish because writes kept arriving, and in a busy moment they
// took half the CPU (measured on the scoring hardware: the loop got 43 % of the clock).
//
// So the WAL now goes round in long cycles, with a handful of checkpoints per cycle:
//
//   1. Nothing until the WAL reaches PRECOPY_AT pages.
//   2. Then PASSIVE checkpoints back to back, with writes still flowing. The first copies
//      most of the WAL; each one after it only what arrived during the one before. When what
//      is left is under TAIL_MAX pages (or after PRECOPY_RUNS), db_hold_wanted() turns true.
//   3. The loop stops committing writes (db_urgent, see db.h). With nothing arriving, one more
//      checkpoint copies the short tail and is complete. The loop resumes, and the next write
//      starts the WAL file again from the beginning.
//
// Under light load the WAL never gets that far, so a timer checkpoints it now and then.
#define PRECOPY_AT 12288 // pages (48 MB)
#define HOLD_AT 20480    // hold anyway at this size, even if the early copying has not caught up
#define TAIL_MAX 384
#define PRECOPY_RUNS 4
// The copier's priority. Measured by go-iouring-davidefrageri (#354) on the scoring box: at 19 a
// thread gets about 1.5 % of a contended CPU and falls behind, at 0 it takes half from the
// loop, at 5 about a quarter. swift-iouring-akbashev (#376) settled on 5 as well. The scheme
// itself (up to four PASSIVE rounds, stop when the tail is short, sync the database file
// ourselves on a separate descriptor) is theirs too; the code and the thresholds are ours.
#define COPIER_NICE 5
#define IDLE_AT 1000     // light load: checkpoint a WAL this large...
#define IDLE_GROWTH 250  // ...if it grew by less than this many pages in the last 250 ms

static sqlite3 *ckpt_db, *copy_db; // the finisher's connection and the copier's
static pthread_mutex_t ckpt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_cv = PTHREAD_COND_INITIALIZER, copy_cv = PTHREAD_COND_INITIALIZER;
static atomic_int wal_pages, wal_max, checkpoints, urgent, hold_seq, backfills;
static atomic_int precopied; // this WAL cycle's early copying has started
static atomic_int ready;     // ...and has caught up: time for the hold
static int db_fd = -1;       // our own descriptor on the database file, for fdatasync

static void sleep_ms(int ms) {
  struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&t, 0);
}

// SQLite calls this on the loop thread after every commit, with the WAL's size in pages.
// Installing it also replaces (turns off) the built-in automatic checkpoint.
static int on_commit(void *arg, sqlite3 *conn, const char *name, int pages) {
  (void)arg; (void)conn; (void)name;
  atomic_store(&wal_pages, pages);
  if (pages > atomic_load(&wal_max)) atomic_store(&wal_max, pages);
  if (pages < PRECOPY_AT) {
    if (atomic_load(&precopied)) { atomic_store(&precopied, 0); atomic_store(&ready, 0); } // a new cycle
  } else if (!atomic_load(&precopied)) {
    atomic_store(&precopied, 1);
    pthread_cond_signal(&copy_cv);
  }
  return SQLITE_OK;
}

// One PASSIVE checkpoint. Returns how many pages of the WAL are still not copied, or -1.
static int checkpoint_once(sqlite3 *conn) {
  int in_wal = -1, copied = -1;
  int rc = sqlite3_wal_checkpoint_v2(conn, 0, SQLITE_CHECKPOINT_PASSIVE, &in_wal, &copied);
  atomic_fetch_add(&checkpoints, 1);
  if (rc != SQLITE_OK || copied < 0) return -1;
  int left = atomic_load(&wal_pages) - copied;
  return left > 0 ? left : 0;
}

static void wait_on(pthread_cond_t *cv, int ms) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += ms * 1000000L;
  if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
  pthread_mutex_lock(&ckpt_mu);
  pthread_cond_timedwait(cv, &ckpt_mu, &until);
  pthread_mutex_unlock(&ckpt_mu);
}

// Two threads, because the two jobs want opposite priorities on a box with one CPU.
//
// The COPIER does the early copying: about 100 ms of CPU per cycle, none of it urgent. At
// normal priority it took half the CPU from the loop while it ran, so it runs at a lower
// one (on Linux a nice value belongs to the thread).
static void *copier(void *arg) {
  (void)arg;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), COPIER_NICE);
  for (;;) {
    if (!atomic_load(&precopied) || atomic_load(&ready) || atomic_load(&urgent)) {
      wait_on(&copy_cv, 250);
      continue;
    }
    for (int run = 0; run < PRECOPY_RUNS && !atomic_load(&urgent); run++) {
      if (checkpoint_once(copy_db) < 0) break;
      // SQLite syncs the database file only in a checkpoint that reaches the end of the WAL,
      // and with writes flowing these do not. Left alone, everything they copied would be
      // flushed inside the hold (measured: a 200 ms hold to copy 240 pages). Flush it here
      // instead, so the hold has only its own short tail to sync.
      int copied_to = atomic_load(&wal_pages);
      fdatasync(db_fd);
      if (atomic_load(&wal_pages) - copied_to <= TAIL_MAX) break; // little arrived meanwhile
    }
    atomic_store(&ready, 1);
  }
  return 0;
}

// The FINISHER does the short checkpoint inside a write hold, at normal priority: writes are
// waiting on it. (At the copier's priority it was starved at 84 % CPU and holds timed out.)
// It also checkpoints on a timer when the server is quiet.
static void *finisher(void *arg) {
  (void)arg;
  int done_hold = 0;   // the last write hold (by its number) that a run of ours completed
  int last_pages = -1; // WAL size at the previous timer tick, or -1 if that was not a tick
  for (;;) {
    int hold = atomic_load(&urgent) ? atomic_load(&hold_seq) : 0;
    if (hold) {
      last_pages = -1;
      if (hold == done_hold) {
        sleep_ms(1); // this hold's work is done: idle until the loop ends it
        continue;
      }
      // Writes are stopped. A run that STARTS now sees the final WAL; only such a run may
      // report the hold complete (a run already under way when the hold began copied an
      // older, shorter WAL: counting those ended holds early in the first version).
      if (checkpoint_once(ckpt_db) == 0) {
        done_hold = hold;
        atomic_store(&precopied, 0); // this cycle is over; the next commit starts a new WAL
        atomic_store(&ready, 0);
        atomic_fetch_add(&backfills, 1);
      } else {
        sleep_ms(1); // the copier or a reader is still in the way: try again shortly
      }
      continue;
    }
    double before = 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    before = t.tv_sec * 1e3 + t.tv_nsec / 1e6;
    wait_on(&ckpt_cv, 250);
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (atomic_load(&urgent) || t.tv_sec * 1e3 + t.tv_nsec / 1e6 - before < 240) { // woken early
      last_pages = -1;
      continue;
    }
    int pages = atomic_load(&wal_pages), grew = pages - last_pages, quiet = last_pages >= 0 && grew >= 0 && grew < IDLE_GROWTH;
    last_pages = pages;
    if (pages >= IDLE_AT && pages < PRECOPY_AT && quiet) {
      checkpoint_once(ckpt_db);
      last_pages = -1; // the run took time: measure growth afresh
    }
  }
  return 0;
}

int db_hold_wanted(void) { return atomic_load(&ready) || atomic_load(&wal_pages) >= HOLD_AT; }

void db_urgent(int on) {
  if (on) atomic_fetch_add(&hold_seq, 1);
  atomic_store(&urgent, on);
  if (on) pthread_cond_signal(&ckpt_cv);
}

int db_backfills(void) { return atomic_load(&backfills); }

void db_stats(int *pages, int *max, int *runs) {
  *pages = atomic_load(&wal_pages);
  *max = atomic_load(&wal_max);
  *runs = atomic_load(&checkpoints);
}
static char errbuf[256];

const char *db_error(void) { return errbuf; }

static int fail(const char *what) {
  sqlite3_snprintf(sizeof errbuf, errbuf, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

// The same query shape as the baseline entry: the author by join, the like count by a
// correlated count over the likes_post_id_idx index.
#define POST_COLS \
  "SELECT p.id, (SELECT count(*) FROM likes l WHERE l.post_id = p.id), p.body, p.created_at, u.username " \
  "FROM posts p JOIN users u ON u.id = p.user_id "

static void bt_setup(void);
static void bt_release(void);
static int bt_on;
static int run(sqlite3_stmt *st);

static int prep(const char *sql, sqlite3_stmt **st) {
  // PERSISTENT tells SQLite the statement lives as long as the connection.
  return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, 0) == SQLITE_OK ? 0 : fail(sql);
}

int db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, 0) != SQLITE_OK) return fail("open");
  // WAL is already set in the seed file. synchronous=NORMAL is the weakest setting rule 6 allows.
  // cache_size: SQLite's own page cache, 16 MB instead of the default 2 MB. With 2 MB the hot
  // pages did not fit and were fetched again from the kernel on most requests.
  if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-16384;", 0, 0, 0) != SQLITE_OK) return fail("pragma");
  if (prep("SELECT 1", &st_health)) return -1;
  if (prep(POST_COLS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &st_feed)) return -1;
  if (prep(POST_COLS "WHERE p.id = ?", &st_post)) return -1;
  if (prep("SELECT 1 FROM posts WHERE id = ?", &st_exists)) return -1;
  if (prep("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING", &st_like)) return -1;
  if (prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at", &st_create)) return -1;
  if (prep("BEGIN", &st_begin) || prep("COMMIT", &st_commit) || prep("ROLLBACK", &st_rollback)) return -1;

  bt_setup();

  if (sqlite3_open_v2(path, &ckpt_db, SQLITE_OPEN_READWRITE, 0) != SQLITE_OK) return fail("open (finisher)");
  if (sqlite3_open_v2(path, &copy_db, SQLITE_OPEN_READWRITE, 0) != SQLITE_OK) return fail("open (copier)");
  if ((db_fd = open(path, O_RDWR | O_CLOEXEC)) < 0) return fail("open (for sync)");
  sqlite3_exec(ckpt_db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
  sqlite3_exec(copy_db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
  sqlite3_wal_hook(db, on_commit, 0);
  pthread_t t;
  if (pthread_create(&t, 0, copier, 0) != 0 || pthread_create(&t, 0, finisher, 0) != 0) return fail("checkpoint threads");
  return 0;
}

static int run(sqlite3_stmt *st) {
  int rc = sqlite3_step(st);
  sqlite3_reset(st);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_begin(void) { return run(st_begin) == 0 ? 0 : fail("begin"); }

int db_commit(void) {
  if (bt_on) bt_release();
  if (run(st_commit) == 0) return 0;
  fail("commit");
  run(st_rollback);
  return -1;
}

int db_health(void) {
  int ok = sqlite3_step(st_health) == SQLITE_ROW;
  sqlite3_reset(st_health);
  return ok;
}

// ---- Reads, straight from the B-trees ---------------------------------------------------
//
// A SELECT is compiled to a small program that SQLite interprets, and every value passes
// through a general-purpose cell on the way out. For three fixed lookups that is most of the
// cost. SQLite's storage layer has its own interface underneath: open a cursor on a table or
// index, move it to a key, read the record it points at. The functions below use that
// interface (the organiser's ruling of 2026-10-31 allows it) and decode the records, whose
// format is part of SQLite's documented file format. Only SQLite's functions touch pages and
// cursors; nothing here writes SQLite's private state.
//
// If anything about the database is not as expected (checked once in db_open by comparing
// against the SQL answers), bt_on stays 0 and every read uses the SQL statements instead.

// Appends the statement's current row to out. Returns the new length, or -1 if it won't fit.
static int put_row(sqlite3_stmt *st, uint8_t *out, int n, int cap) {
  int64_t id = sqlite3_column_int64(st, 0), likes = sqlite3_column_int64(st, 1);
  const unsigned char *text[3];
  uint16_t len[3];
  int total = 16 + 6;
  for (int i = 0; i < 3; i++) {
    text[i] = sqlite3_column_text(st, 2 + i);
    len[i] = (uint16_t)sqlite3_column_bytes(st, 2 + i);
    total += len[i];
  }
  if (n + total > cap) return -1;
  memcpy(out + n, &id, 8);
  memcpy(out + n + 8, &likes, 8);
  memcpy(out + n + 16, len, 6);
  n += 22;
  for (int i = 0; i < 3; i++) {
    memcpy(out + n, text[i], len[i]);
    n += len[i];
  }
  return n;
}

static int sql_feed(uint8_t *out, int cap) {
  int n = 0, rc;
  while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW)
    if ((n = put_row(st_feed, out, n, cap)) < 0) break;
  sqlite3_reset(st_feed);
  return (rc == SQLITE_DONE && n >= 0) ? n : fail("feed");
}

static int sql_post(int64_t id, uint8_t *out, int cap) {
  sqlite3_bind_int64(st_post, 1, id);
  int rc = sqlite3_step(st_post), n = 0;
  if (rc == SQLITE_ROW) n = put_row(st_post, out, 0, cap);
  sqlite3_reset(st_post);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return fail("post");
  return n;
}

static int sql_post_exists(int64_t id) {
  sqlite3_bind_int64(st_exists, 1, id);
  int rc = sqlite3_step(st_exists);
  sqlite3_reset(st_exists);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : fail("exists");
}

static int bt_on;
static Btree *bt;
static BtCursor *c_feed, *c_posts, *c_users, *c_likes;
static Mem likes_val;            // the post id being counted
static UnpackedRecord likes_key; // ...as a search key for the likes index
static uint8_t spill[8192];      // a record that continues on overflow pages is copied here

// One column of a record: where its bytes are and its serial type (see "Record Format" in
// SQLite's file-format document). The names col_t and rec_cols are go-iouring-davidefrageri's
// (#354); the decoding here uses SQLite's own getVarint32 and sqlite3VdbeSerialTypeLen.
typedef struct { const u8 *p; u32 n, type; } col_t;

// Finds the first ncol columns of a record. Returns 0, or -1 if the record is malformed.
static int rec_cols(const u8 *rec, u32 len, int ncol, col_t *c) {
  u32 hdr, t;
  const u8 *h = rec + getVarint32(rec, hdr);
  if (hdr > len) return -1;
  const u8 *hend = rec + hdr, *d = hend, *end = rec + len;
  for (int i = 0; i < ncol; i++) {
    if (h >= hend) return -1;
    h += getVarint32(h, t);
    u32 size = sqlite3VdbeSerialTypeLen(t);
    if (size > (u32)(end - d)) return -1;
    c[i] = (col_t){d, size, t};
    d += size;
  }
  return 0;
}

// The last column of a record (an index entry ends with the row's id).
static int rec_last(const u8 *rec, u32 len, col_t *c) {
  u32 hdr, t = 0;
  const u8 *h = rec + getVarint32(rec, hdr);
  if (hdr > len || h >= rec + hdr) return -1;
  const u8 *hend = rec + hdr, *d = hend, *end = rec + len;
  u32 size = 0;
  while (h < hend) {
    d += size;
    h += getVarint32(h, t);
    size = sqlite3VdbeSerialTypeLen(t);
    if (size > (u32)(end - d)) return -1;
  }
  *c = (col_t){d, size, t};
  return 0;
}

static int col_int(col_t c, i64 *v) {
  if (c.type == 8 || c.type == 9) { *v = c.type - 8; return 0; }
  if (c.type < 1 || c.type > 6) return -1;
  i64 x = (signed char)c.p[0]; // big-endian, two's complement
  for (u32 i = 1; i < c.n; i++) x = (x << 8) | c.p[i];
  *v = x;
  return 0;
}

static int col_is_text(col_t c) { return c.type >= 13 && (c.type & 1) && c.n <= 0xffff; }

// The record under a cursor. It is usually inside the page; SQLite copies it for us if not.
static const u8 *record(BtCursor *cur, u32 *len) {
  u32 total = sqlite3BtreePayloadSize(cur), local;
  const u8 *p = sqlite3BtreePayloadFetch(cur, &local);
  *len = total;
  if (local >= total) return p;
  if (total > sizeof spill || sqlite3BtreePayload(cur, 0, total, spill) != SQLITE_OK) return 0;
  return spill;
}

// Cursors hold on to pages. Letting go of them after each request means nothing of ours is in
// the way when a write statement or the end of the transaction needs those pages.
static void bt_release(void) {
  sqlite3BtreeClearCursor(c_feed);
  sqlite3BtreeClearCursor(c_posts);
  sqlite3BtreeClearCursor(c_users);
  sqlite3BtreeClearCursor(c_likes);
}

// like_count: entries of likes_post_id_idx whose first column is this post. -1 on error.
static i64 bt_likes(i64 post_id) {
  int res;
  likes_val.u.i = post_id;
  // +1: entries equal to the key count as larger, so the search stops just before the first.
  likes_key.default_rc = 1;
  likes_key.eqSeen = 0;
  if (sqlite3BtreeIndexMoveto(c_likes, &likes_key, &res) != SQLITE_OK) return -1;
  int rc = res < 0 ? sqlite3BtreeNext(c_likes, 0) : SQLITE_OK;
  i64 n = 0, v;
  for (; rc == SQLITE_OK; rc = sqlite3BtreeNext(c_likes, 0)) {
    u32 len;
    col_t c;
    const u8 *rec = record(c_likes, &len);
    if (!rec || rec_cols(rec, len, 1, &c) || col_int(c, &v)) return -1;
    if (v != post_id) break;
    n++;
  }
  return rc == SQLITE_OK || rc == SQLITE_DONE ? n : -1;
}

// like_counts for every post id from lo to hi in one pass over likes_post_id_idx: one seek
// to the first entry at or after lo, then forward until an entry past hi. The feed's posts
// have neighbouring ids, so this visits the same entries as a seek per post would, without
// the other 19 descents from the root. (After bt_range_counts in c-iouring-pavlyhalim, #245:
// the signature, the clearing of counts and the counting line are as there. The span limit of
// 512 is the one rust-iouring-kensac, #264, uses.)
// Returns 0, or -1 on error.
#define LIKES_SPAN 512
static int bt_likes_range(i64 lo, i64 hi, i64 *counts) {
  int res;
  memset(counts, 0, sizeof(i64) * (size_t)(hi - lo + 1));
  likes_val.u.i = lo;
  likes_key.default_rc = 1;
  likes_key.eqSeen = 0;
  if (sqlite3BtreeIndexMoveto(c_likes, &likes_key, &res) != SQLITE_OK) return -1;
  int rc = res < 0 ? sqlite3BtreeNext(c_likes, 0) : SQLITE_OK;
  for (; rc == SQLITE_OK; rc = sqlite3BtreeNext(c_likes, 0)) {
    u32 len;
    col_t c;
    i64 v;
    const u8 *rec = record(c_likes, &len);
    if (!rec || rec_cols(rec, len, 1, &c) || col_int(c, &v) || v < lo) return -1;
    if (v > hi) break;
    counts[v - lo]++;
  }
  return rc == SQLITE_OK || rc == SQLITE_DONE ? 0 : -1;
}

// Appends post `id` in the row format. Returns the new length, n unchanged if there is no
// such post, or -1. likes is the post's like count if the caller already has it, or -1.
static int bt_row(i64 id, i64 likes, uint8_t *out, int n, int cap) {
  int res;
  u32 len;
  col_t p[4], u[2];
  i64 uid;
  if (sqlite3BtreeTableMoveto(c_posts, id, 0, &res) != SQLITE_OK) return -1;
  if (res != 0) return n;
  // posts: (id, user_id, body, created_at). The id is the row's key, not in the record.
  const u8 *rec = record(c_posts, &len);
  if (!rec || rec == spill || rec_cols(rec, len, 4, p) || col_int(p[1], &uid) || !col_is_text(p[2]) || !col_is_text(p[3])) return -1;
  if (sqlite3BtreeTableMoveto(c_users, uid, 0, &res) != SQLITE_OK || res != 0) return -1;
  // users: (id, username, created_at)
  rec = record(c_users, &len);
  if (!rec || rec == spill || rec_cols(rec, len, 2, u) || !col_is_text(u[1])) return -1;
  if (likes < 0) likes = bt_likes(id);
  if (likes < 0 || n + 22 + (int)(p[2].n + p[3].n + u[1].n) > cap) return -1;
  uint16_t lens[3] = {(uint16_t)p[2].n, (uint16_t)p[3].n, (uint16_t)u[1].n};
  memcpy(out + n, &id, 8);
  memcpy(out + n + 8, &likes, 8);
  memcpy(out + n + 16, lens, 6);
  n += 22;
  memcpy(out + n, p[2].p, p[2].n); n += p[2].n;
  memcpy(out + n, p[3].p, p[3].n); n += p[3].n;
  memcpy(out + n, u[1].p, u[1].n); n += u[1].n;
  return n;
}

// The read transaction for direct reads. The pass's BEGIN has already run; this starts (or
// joins) the storage layer's read transaction, which the pass's COMMIT ends.
static int bt_begin(void) { return sqlite3BtreeBeginTrans(bt, 0, 0) == SQLITE_OK ? 0 : -1; }

static int bt_feed(uint8_t *out, int cap) {
  i64 ids[20];
  int res, k = 0, n = 0;
  if (bt_begin()) return -1;
  // posts_created_at_id_idx is stored newest first, so the feed is its first 20 entries.
  int rc = sqlite3BtreeFirst(c_feed, &res);
  if (rc == SQLITE_OK && res) rc = SQLITE_DONE; // empty
  for (; rc == SQLITE_OK && k < 20; rc = sqlite3BtreeNext(c_feed, 0)) {
    u32 len;
    col_t c;
    const u8 *rec = record(c_feed, &len);
    if (!rec || rec_last(rec, len, &c) || col_int(c, &ids[k])) { rc = SQLITE_CORRUPT; break; }
    k++;
  }
  if (rc != SQLITE_OK && rc != SQLITE_DONE) n = -1;
  if (k == 0 || n < 0) {
    bt_release();
    return n;
  }
  // Like counts for all of them in one walk when their ids are close together.
  static i64 counts[LIKES_SPAN];
  i64 lo = ids[0], hi = ids[0];
  for (int i = 1; i < k; i++) {
    if (ids[i] < lo) lo = ids[i];
    if (ids[i] > hi) hi = ids[i];
  }
  int ranged = k > 1 && hi - lo < LIKES_SPAN && bt_likes_range(lo, hi, counts) == 0;

  // (Posts in ascending id order to get TableMoveto's next-row step is the plan
  // rust-iouring-kensac, #264, describes; authors in id order with repeats skipped is ours.)
  // The posts and their authors are looked up in ascending key order, on cursors that stay
  // where they are between lookups. Asked for the key right after the one it is on,
  // sqlite3BtreeTableMoveto steps to the next row instead of walking down from the root, and
  // the feed's posts have consecutive ids nearly always: 20 descents become one. (A profile
  // on the scoring hardware had these descents and their page fetches at about a fifth of
  // all CPU.) What is read is copied aside, because moving a cursor gives up its page, and the
  // rows are written out afterwards in feed order.
  static struct { i64 uid, likes; u32 body, body_n, at, at_n, name, name_n; } row[20];
  static uint8_t aside[20 * 2600];
  int order[20], used = 0;
  for (int i = 0; i < k; i++) { // insertion sort of 20 positions by post id
    int j = i;
    for (; j > 0 && ids[order[j - 1]] > ids[i]; j--) order[j] = order[j - 1];
    order[j] = i;
  }
  for (int o = 0; o < k; o++) {
    int i = order[o];
    u32 len;
    col_t p[4];
    if (sqlite3BtreeTableMoveto(c_posts, ids[i], 0, &res) != SQLITE_OK || res != 0) goto slow;
    // posts: (id, user_id, body, created_at). The id is the row's key, not in the record.
    const u8 *rec = record(c_posts, &len);
    if (!rec || rec == spill || rec_cols(rec, len, 4, p) || col_int(p[1], &row[i].uid) || !col_is_text(p[2]) || !col_is_text(p[3])) goto slow;
    if (used + p[2].n + p[3].n > sizeof aside) goto slow;
    row[i].body = used; row[i].body_n = p[2].n;
    memcpy(aside + used, p[2].p, p[2].n); used += p[2].n;
    row[i].at = used; row[i].at_n = p[3].n;
    memcpy(aside + used, p[3].p, p[3].n); used += p[3].n;
    row[i].likes = ranged ? counts[ids[i] - lo] : -1;
  }
  for (int i = 0; i < k; i++) { // the same positions, now by author id
    int j = i;
    for (; j > 0 && row[order[j - 1]].uid > row[i].uid; j--) order[j] = order[j - 1];
    order[j] = i;
  }
  for (int o = 0; o < k; o++) {
    int i = order[o];
    if (o > 0 && row[order[o - 1]].uid == row[i].uid) { // the same author as the one before
      row[i].name = row[order[o - 1]].name; row[i].name_n = row[order[o - 1]].name_n;
      continue;
    }
    u32 len;
    col_t u[2];
    if (sqlite3BtreeTableMoveto(c_users, row[i].uid, 0, &res) != SQLITE_OK || res != 0) goto slow;
    // users: (id, username, created_at)
    const u8 *rec = record(c_users, &len);
    if (!rec || rec == spill || rec_cols(rec, len, 2, u) || !col_is_text(u[1]) || used + u[1].n > sizeof aside) goto slow;
    row[i].name = used; row[i].name_n = u[1].n;
    memcpy(aside + used, u[1].p, u[1].n); used += u[1].n;
  }
  for (int i = 0; i < k; i++) {
    i64 likes = row[i].likes >= 0 ? row[i].likes : bt_likes(ids[i]);
    if (likes < 0 || n + 22 + (int)(row[i].body_n + row[i].at_n + row[i].name_n) > cap) goto slow;
    uint16_t lens[3] = {(uint16_t)row[i].body_n, (uint16_t)row[i].at_n, (uint16_t)row[i].name_n};
    memcpy(out + n, &ids[i], 8);
    memcpy(out + n + 8, &likes, 8);
    memcpy(out + n + 16, lens, 6);
    n += 22;
    memcpy(out + n, aside + row[i].body, row[i].body_n); n += row[i].body_n;
    memcpy(out + n, aside + row[i].at, row[i].at_n); n += row[i].at_n;
    memcpy(out + n, aside + row[i].name, row[i].name_n); n += row[i].name_n;
  }
  bt_release();
  return n;

slow: // anything unexpected: the plain way, one post at a time (and from there to SQL)
  n = 0;
  for (int i = 0; i < k && n >= 0; i++) {
    int m = bt_row(ids[i], -1, out, n, cap);
    n = m == n ? -1 : m; // a post in the index must be in the table
  }
  bt_release();
  return n;
}

static int bt_post(i64 id, uint8_t *out, int cap) {
  if (bt_begin()) return -1;
  int n = bt_row(id, -1, out, 0, cap);
  bt_release();
  return n;
}

static int bt_post_exists(i64 id) {
  int res;
  if (bt_begin()) return -1;
  int rc = sqlite3BtreeTableMoveto(c_posts, id, 0, &res);
  bt_release();
  return rc != SQLITE_OK ? -1 : res == 0;
}

static Pgno root_of(const char *name) {
  sqlite3_stmt *st;
  Pgno root = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?", -1, &st, 0) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW) root = (Pgno)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return root;
}

static BtCursor *cursor_on(Pgno root, int key_cols) {
  if (!root) return 0;
  KeyInfo *ki = 0;
  if (key_cols) { // an index: SQLite needs to know how its entries are ordered
    if (!(ki = sqlite3KeyInfoAlloc(db, key_cols, 1))) return 0;
    for (int i = 0; i < key_cols; i++) ki->aColl[i] = db->pDfltColl;
  }
  BtCursor *cur = sqlite3_malloc(sqlite3BtreeCursorSize());
  if (!cur) return 0;
  sqlite3BtreeCursorZero(cur);
  if (sqlite3BtreeCursor(bt, root, 0, ki, cur) != SQLITE_OK) return 0;
  return cur;
}

// Opens the cursors, then proves the direct reads right by comparing them with the SQL
// statements on the feed and on its posts. Any difference leaves bt_on at 0.
static void bt_setup(void) {
  static uint8_t a[1 << 17], b[1 << 17];
  bt = db->aDb[0].pBt;
  if (ENC(db) != SQLITE_UTF8 || run(st_begin)) return;
  if (bt_begin() == 0) {
    c_feed = cursor_on(root_of("posts_created_at_id_idx"), 2);
    c_posts = cursor_on(root_of("posts"), 0);
    c_users = cursor_on(root_of("users"), 0);
    c_likes = cursor_on(root_of("likes_post_id_idx"), 1);
  }
  if (c_feed && c_posts && c_users && c_likes) {
    likes_val.flags = MEM_Int;
    likes_key.pKeyInfo = c_likes->pKeyInfo;
    likes_key.aMem = &likes_val;
    likes_key.nField = 1;
    int n = bt_feed(a, sizeof a), m = sql_feed(b, sizeof b), ok = n >= 0 && n == m && memcmp(a, b, n) == 0;
    for (int off = 0; ok && off < n;) { // each post of the feed, and the id after it (a miss or a hit)
      i64 id;
      uint16_t lens[3];
      memcpy(&id, a + off, 8);
      memcpy(lens, a + off + 16, 6);
      off += 22 + lens[0] + lens[1] + lens[2];
      for (i64 probe = id; ok && probe <= id + 1; probe++) {
        int x = bt_post(probe, a + n, (int)sizeof a - n), y = sql_post(probe, b + n, (int)sizeof b - n);
        ok = x >= 0 && x == y && memcmp(a + n, b + n, x) == 0 && bt_post_exists(probe) == sql_post_exists(probe);
      }
    }
    bt_on = ok;
  }
  if (c_feed) bt_release();
  run(st_commit);
}

int db_reads_direct(void) { return bt_on; }

int db_feed(uint8_t *out, int cap) {
  if (bt_on) {
    int n = bt_feed(out, cap);
    if (n >= 0) return n;
  }
  return sql_feed(out, cap);
}

int db_post(int64_t id, uint8_t *out, int cap) {
  if (bt_on) {
    int n = bt_post(id, out, cap);
    if (n >= 0) return n;
  }
  return sql_post(id, out, cap);
}

int db_post_exists(int64_t id) {
  if (bt_on) {
    int r = bt_post_exists(id);
    if (r >= 0) return r;
  }
  return sql_post_exists(id);
}

int db_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(st_like, 1, user_id);
  sqlite3_bind_int64(st_like, 2, post_id);
  int rc = sqlite3_step(st_like);
  sqlite3_reset(st_like);
  return rc == SQLITE_DONE ? sqlite3_changes(db) : fail("like");
}

int db_create(int64_t user_id, const char *body, int body_len, int64_t *id, char *created_at) {
  sqlite3_bind_int64(st_create, 1, user_id);
  sqlite3_bind_text(st_create, 2, body, body_len, SQLITE_STATIC);
  int rc = sqlite3_step(st_create), ok = 0;
  if (rc == SQLITE_ROW && sqlite3_column_bytes(st_create, 1) == 24) {
    *id = sqlite3_column_int64(st_create, 0);
    memcpy(created_at, sqlite3_column_text(st_create, 1), 24);
    // The INSERT (and its commit) only finishes when the statement runs to the end.
    ok = sqlite3_step(st_create) == SQLITE_DONE;
  }
  sqlite3_reset(st_create);
  return ok ? 0 : fail("create");
}
