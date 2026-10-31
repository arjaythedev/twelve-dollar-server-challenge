/* dbtest.c — scratch harness for db.c (not part of the server).
 *   dbtest <db>            full run: plan, reads, selfcheck, writes, data_version
 *   dbtest <db> dump <dir> write feed.json, post-{1,250000,500000}.json (no newline)
 *   dbtest <db> bench      checkInv/open time + us per feed render for each plan
 *   dbtest --pcache        page-cache wrapper test on a small DB it creates
 *                          (and deletes) in a new directory under $TMPDIR or /tmp
 *   dbtest --feedlikes     feed like counts (one index range walk): B-tree feed ==
 *                          SQL feed byte for byte over edge cases, same temp dir rule
 * Build by hand (same SQLite flags as build.sh):
 *   gcc -O2 -std=gnu11 -w <build.sh SQLITE_FLAGS> -c src/db.c -o /tmp/db.o
 *   gcc -O2 -std=gnu11 -Wall -Wextra src/dbtest.c /tmp/db.o -o /tmp/dbtest -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "db.h"
#include "../deps/sqlite3.h"

/* test-only helpers exported by db.c (not in db.h) */
void db_force_plan(db_t *d, int which);       /* 0 none, 1 btree, 2 sql */
int64_t db_data_version(db_t *d);
int db_txn_state(db_t *d);                    /* SQLITE_TXN_* of the main btree */
int db_autocommit(db_t *d);

/* After every db_batch_commit the batch txn must be fully ended. */
static int idle_bad = 0, idle_checks = 0;
static int commit_idle(db_t *d) {
  int rc = db_batch_commit(d);
  idle_checks++;
  if (db_txn_state(d) != SQLITE_TXN_NONE || db_autocommit(d) != 1) {
    printf("NOT IDLE after commit: txn_state=%d autocommit=%d\n", db_txn_state(d), db_autocommit(d));
    idle_bad++;
  }
  return rc;
}

static char buf[1 << 20];

static double now_us(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static db_t *open_or_die(const char *p) {
  char err[512];
  double t0 = now_us();
  db_t *d = db_open(p, err, sizeof err);
  double t1 = now_us();
  if (!d) { fprintf(stderr, "open: %s\n", err); exit(1); }
  printf("open+checkInv %.1f ms\n%s\n", (t1 - t0) / 1e3, db_plan_line(d));
  return d;
}

static void show_post(db_t *d, int64_t id) {
  int n = db_post_json(d, id, buf, sizeof buf);
  printf("post %lld -> %d: %.*s\n", (long long)id, n, n > 0 ? n : 0, buf);
}

/* WAL-index header: mxFrame at byte 16, WalCkptInfo.nBackfill at byte 96 */
static void shm_info(const char *db, const char *when) {
  char p[512]; unsigned mx = 0, bf = 0;
  snprintf(p, sizeof p, "%s-shm", db);
  FILE *f = fopen(p, "rb");
  if (!f) { printf("shm %s: none\n", when); return; }
  fseek(f, 16, SEEK_SET); if (fread(&mx, 4, 1, f) != 1) mx = 0;
  fseek(f, 96, SEEK_SET); if (fread(&bf, 4, 1, f) != 1) bf = 0;
  fclose(f);
  printf("shm %s: wal mxFrame=%u nBackfill=%u\n", when, mx, bf);
}

static void dump(db_t *d, const char *dir) {
  char path[512]; int n;
  long ids[] = {1, 250000, 500000};
  n = db_feed_json(d, buf, sizeof buf);
  snprintf(path, sizeof path, "%s/feed.json", dir);
  FILE *f = fopen(path, "wb"); fwrite(buf, 1, (size_t)(n > 0 ? n : 0), f); fclose(f);
  for (int i = 0; i < 3; i++) {
    n = db_post_json(d, ids[i], buf, sizeof buf);
    snprintf(path, sizeof path, "%s/post-%ld.json", dir, ids[i]);
    f = fopen(path, "wb"); fwrite(buf, 1, (size_t)(n > 0 ? n : 0), f); fclose(f);
  }
  printf("dumped plan=%s\n", db_plan(d));
}

static void bench(db_t *d) {
  for (int p = 1; p <= 2; p++) {
    db_force_plan(d, p);
    db_feed_json(d, buf, sizeof buf);
    double t0 = now_us();
    for (int i = 0; i < 10000; i++)
      if (db_feed_json(d, buf, sizeof buf) <= 0) { printf("feed failed\n"); return; }
    double dt = now_us() - t0;
    printf("feed %s: %.2f us/render (10000 iters)\n", p == 1 ? "btree" : "sql", dt / 1e4);
  }
  db_force_plan(d, 0);
}

static int64_t q1(sqlite3 *h, const char *sql) {
  sqlite3_stmt *s; int64_t v = -1;
  if (sqlite3_prepare_v2(h, sql, -1, &s, 0) != SQLITE_OK) return -1;
  if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return v;
}

/* Page-cache wrapper: random index inserts (balance_nonroot rekeys pages
 * through the lock-page key) must leave COMMIT's truncate skipped; a ROLLBACK
 * of appended pages must still forward a real truncate; the data must stay
 * intact. Returns 0 on success. */
static int pcache_test(void) {
  enum { NUSERS = 300, NPOSTS = 4000, NLIKES = 15000, CYCLES = 3000, LIKES_PER = 6 };
  const char *tmp = getenv("TMPDIR");
  char dir[512], path[600], side[640], err[512];
  snprintf(dir, sizeof dir, "%s/dbtest-pcache.XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  snprintf(path, sizeof path, "%s/t.db", dir);
  int fail = 0;
  unsigned long s0, f0, s1, f1, s2, f2;
  if (db_pgo_make(path, NUSERS, NPOSTS, NLIKES, err, sizeof err)) { printf("pgo_make: %s\n", err); fail = 1; goto rm; }
  db_t *d = db_open(path, err, sizeof err);
  if (!d) { printf("open: %s\n", err); fail = 1; goto rm; }
  printf("%s\n", db_plan_line(d));
  sqlite3 *h;
  if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { printf("open h\n"); fail = 1; db_close(d); goto rm; }
  int64_t posts0 = q1(h, "SELECT count(*) FROM posts"), likes0 = q1(h, "SELECT count(*) FROM likes");

  /* 1. batches of random likes + one post each */
  db_pcache_stats(&s0, &f0);
  uint64_t x = 12345;
  int64_t maxid = posts0, new_posts = 0, new_likes = 0;
  char body[64];
  for (int c = 0; c < CYCLES && !fail; c++) {
    db_batch_begin(d);
    for (int k = 0; k < LIKES_PER; k++) {
      x = x * 6364136223846793005ull + 1442695040888963407ull;
      int64_t uid = 1 + (int64_t)((x >> 33) % NUSERS);
      x = x * 6364136223846793005ull + 1442695040888963407ull;
      int64_t pid = 1 + (int64_t)((x >> 33) % (uint64_t)maxid);
      int al = -1, r = db_like(d, uid, pid, &al);
      if (r != 1) { printf("like rc=%d\n", r); fail = 1; break; }
      if (!al) new_likes++;
    }
    int bl = snprintf(body, sizeof body, "pcache test post %d", c);
    int n = db_insert_post(d, 1 + c % NUSERS, "u", 1, body, (size_t)bl, buf, sizeof buf);
    if (n <= 0) { printf("insert_post rc=%d\n", n); fail = 1; }
    if (db_batch_commit(d) != 0) { printf("commit failed\n"); fail = 1; }
    if (!fail) { new_posts++; maxid++; }
    db_maybe_checkpoint(d, 1000);
  }
  db_pcache_stats(&s1, &f1);
  printf("cycles: skipped %lu forwarded %lu truncates\n", s1 - s0, f1 - f0);
  if (s1 == s0) { printf("FAIL no truncate was skipped\n"); fail = 1; }

  /* 2. a real truncate: appended pages rolled back (other connection, same
   *    process-wide wrapper) */
  int rc = sqlite3_exec(h, "BEGIN; WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<3000) "
                           "INSERT INTO posts (user_id, body) SELECT 1 + i % 300, 'rolled back ' || i FROM c", 0, 0, 0);
  int64_t posts_in_txn = q1(h, "SELECT count(*) FROM posts");
  int rc2 = sqlite3_exec(h, "ROLLBACK", 0, 0, 0);
  db_pcache_stats(&s2, &f2);
  printf("rollback (rc=%d,%d, %lld posts inside the txn): forwarded %lu skipped %lu\n", rc, rc2,
         (long long)posts_in_txn, f2 - f1, s2 - s1);
  if (rc || rc2 || posts_in_txn != posts0 + new_posts + 3000) { printf("FAIL rollback txn\n"); fail = 1; }
  if (f2 == f1) { printf("FAIL rollback forwarded no truncate\n"); fail = 1; }

  /* 3. data intact: integrity_check, counts, both read plans agree */
  sqlite3_stmt *s; char ic[64] = "";
  if (sqlite3_prepare_v2(h, "PRAGMA integrity_check", -1, &s, 0) == SQLITE_OK) {
    if (sqlite3_step(s) == SQLITE_ROW) snprintf(ic, sizeof ic, "%s", (const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
  }
  int64_t posts1 = q1(h, "SELECT count(*) FROM posts"), likes1 = q1(h, "SELECT count(*) FROM likes");
  printf("integrity_check=%s posts %lld->%lld (+%lld) likes %lld->%lld (+%lld)\n", ic,
         (long long)posts0, (long long)posts1, (long long)new_posts,
         (long long)likes0, (long long)likes1, (long long)new_likes);
  if (strcmp(ic, "ok") || posts1 != posts0 + new_posts || likes1 != likes0 + new_likes) {
    printf("FAIL integrity/counts\n"); fail = 1;
  }
  sqlite3_close(h);
  db_close(d);
  if (db_selfcheck(path, 7) != 0) { printf("FAIL selfcheck\n"); fail = 1; }
  if (!fail) printf("pcache test ok skipped=%lu forwarded=%lu\n", s1 - s0, f2 - f1);
rm:
  {
    const char *sfx[] = {"", "-wal", "-shm", "-journal"};
    for (int i = 0; i < 4; i++) { snprintf(side, sizeof side, "%s%s", path, sfx[i]); unlink(side); }
    rmdir(dir);
  }
  return fail;
}

/* Feed like counts (one likes_post_id_idx range walk over the 20 feed ids):
 * the B-tree feed must equal the reference SQL feed byte for byte. */
static char buf2[1 << 20];
static int fl_cmp = 0;

static int fl_exec(sqlite3 *h, const char *sql) {
  char *e = NULL;
  if (sqlite3_exec(h, sql, 0, 0, &e) != SQLITE_OK) { printf("FAIL exec %s: %s\n", sql, e ? e : "?"); sqlite3_free(e); return 1; }
  return 0;
}

/* n likes on post pid by users first_uid .. first_uid+n-1 (no conflicts:
 * callers delete the post's likes first) */
static int fl_like_n(sqlite3 *h, int64_t pid, int n, int first_uid) {
  char sql[256];
  snprintf(sql, sizeof sql, "WITH RECURSIVE c(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
           "INSERT INTO likes (user_id, post_id) SELECT %d + i, %lld FROM c WHERE i < %d",
           n, first_uid, (long long)pid, n);
  return n > 0 ? fl_exec(h, sql) : 0;
}

static int fl_has(const char *b, int n, const char *s) {
  size_t l = strlen(s);
  for (int i = 0; i + (int)l <= n; i++) if (!memcmp(b + i, s, l)) return 1;
  return 0;
}

/* btree vs SQL feed on an open db; expect (optional) must occur in the btree
 * rendering. 0 equal, 1 mismatch. */
static int fl_compare(db_t *d, const char *label, const char *expect) {
  db_force_plan(d, 1); int la = db_feed_json(d, buf, sizeof buf);
  db_force_plan(d, 2); int lb = db_feed_json(d, buf2, sizeof buf2);
  db_force_plan(d, 0);
  fl_cmp++;
  if (la <= 0 || la != lb || memcmp(buf, buf2, (size_t)la)) {
    printf("FAIL feedlikes %s: btree_len=%d sql_len=%d\n", label, la, lb);
    if (la > 0) printf("  btree: %.*s\n", la > 400 ? 400 : la, buf);
    if (lb > 0) printf("  sql:   %.*s\n", lb > 400 ? 400 : lb, buf2);
    return 1;
  }
  if (expect && !fl_has(buf, la, expect)) {
    printf("FAIL feedlikes %s: %s not in the feed\n", label, expect);
    return 1;
  }
  return 0;
}

/* open path, compare, close; want_btree: the invariant must hold */
static int fl_open_compare(const char *path, const char *label, const char *expect, int want_btree) {
  char err[512];
  db_t *d = db_open(path, err, sizeof err);
  if (!d) { printf("FAIL feedlikes %s: open: %s\n", label, err); return 1; }
  int fail = 0;
  if (want_btree && strcmp(db_plan(d), "btree")) { printf("FAIL feedlikes %s: %s\n", label, db_plan_line(d)); fail = 1; }
  fail |= fl_compare(d, label, expect);
  db_close(d);
  if (!fail) printf("feedlikes %s: ok\n", label);
  return fail;
}

static void fl_rm(const char *path) {
  const char *sfx[] = {"", "-wal", "-shm", "-journal"};
  char side[700];
  for (int i = 0; i < 4; i++) { snprintf(side, sizeof side, "%s%s", path, sfx[i]); unlink(side); }
}

static int feedlikes_test(void) {
  enum { NUSERS = 400, NPOSTS = 600, NLIKES = 3000 };
  const char *tmp = getenv("TMPDIR");
  char dir[512], path[600], p5[600], p20[600], err[512], sql[512];
  snprintf(dir, sizeof dir, "%s/dbtest-feedlikes.XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  snprintf(path, sizeof path, "%s/t.db", dir);
  snprintf(p5, sizeof p5, "%s/five.db", dir);
  snprintf(p20, sizeof p20, "%s/twenty.db", dir);
  int fail = 0, scen = 0;
  sqlite3 *h = NULL;
  db_t *d = NULL;
  const int64_t M = NPOSTS;   /* max id */

  if (db_pgo_make(path, NUSERS, NPOSTS, NLIKES, err, sizeof err)) { printf("pgo_make: %s\n", err); fail = 1; goto rm; }
  if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { printf("open h\n"); fail = 1; goto rm; }

  /* (a) no likes on the newest 20; likes on the 21st newest, so the SeekGE
   *     lands past the last entry of the index */
  snprintf(sql, sizeof sql, "DELETE FROM likes WHERE post_id > %lld", (long long)(M - 22));
  fail |= fl_exec(h, sql) || fl_like_n(h, M - 20, 7, 1) || fl_like_n(h, M - 21, 3, 1);
  fail |= fl_open_compare(path, "(a) no likes on the newest 20", "\"like_count\":0}]}", 1); scen++;

  /* (b) 300 likes on the newest post only (distinct users) */
  fail |= fl_like_n(h, M, 300, 1);
  fail |= fl_open_compare(path, "(b) 300 likes on the newest post", "{\"posts\":[{\"id\":600,", 1);
  fail |= fl_open_compare(path, "(b) 300 likes on the newest post (count)", "\"like_count\":300},{", 1); scen++;

  /* (c) likes on all 20 (j+1 on the j-th newest, j = 0..19) plus likes on
   *     the 21st and 22nd newest, which must not be counted */
  snprintf(sql, sizeof sql, "DELETE FROM likes WHERE post_id > %lld", (long long)(M - 22));
  fail |= fl_exec(h, sql);
  for (int j = 0; j < 22 && !fail; j++) fail |= fl_like_n(h, M - j, j < 20 ? j + 1 : 50, 1 + j);
  fail |= fl_open_compare(path, "(c) likes on all 20 + 21st/22nd", "\"like_count\":20}]}", 1); scen++;

  /* (f) new posts and likes through db_insert_post / db_like inside batches:
   *     compared inside the write batch and after its commit (pages in the WAL) */
  sqlite3_close(h); h = NULL;
  d = db_open(path, err, sizeof err);
  if (!d) { printf("FAIL open: %s\n", err); fail = 1; goto rm; }
  {
    int64_t maxid = M;
    uint64_t x = 99;
    char body[64], label[96];
    for (int b = 0; b < 6 && !fail; b++) {
      db_batch_begin(d);
      for (int k = 0; k < 3 && !fail; k++) {
        int bl = snprintf(body, sizeof body, "feedlikes post %d.%d", b, k);
        if (db_insert_post(d, 1 + (b * 3 + k) % NUSERS, "u", 1, body, (size_t)bl, buf, sizeof buf) <= 0) {
          printf("FAIL insert_post\n"); fail = 1;
        } else maxid++;
      }
      for (int k = 0; k < 60 && !fail; k++) {   /* newest 24 posts, random users */
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        int64_t pid = maxid - (int64_t)((x >> 33) % 24);
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        int al = -1;
        if (db_like(d, 1 + (int64_t)((x >> 33) % NUSERS), pid, &al) != 1) { printf("FAIL like\n"); fail = 1; }
      }
      snprintf(label, sizeof label, "(f) batch %d in txn", b);
      fail |= fl_compare(d, label, NULL);
      if (db_batch_commit(d) != 0) { printf("FAIL commit\n"); fail = 1; }
      snprintf(label, sizeof label, "(f) batch %d committed", b);
      fail |= fl_compare(d, label, NULL);
    }
    if (strcmp(db_plan(d), "btree")) { printf("FAIL (f): %s\n", db_plan_line(d)); fail = 1; }
    if (!fail) printf("feedlikes (f) inserts + likes in batches (WAL): ok, max id %lld\n", (long long)maxid);
    scen++;
  }
  db_close(d); d = NULL;

  /* (g) Inv broken (a gap among the newest 20; btree forced as selfcheck
   *     does): the post below the range gets the per-post seek */
  if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { printf("open h\n"); fail = 1; goto rm; }
  fail |= fl_exec(h, "DELETE FROM likes WHERE post_id = (SELECT max(id) - 5 FROM posts);"
                     "DELETE FROM posts WHERE id = (SELECT max(id) - 5 FROM posts);");
  /* max id is M + 18 after (f); with M + 13 gone the 20th row is M - 2,
   * one below [max-19, max] */
  snprintf(sql, sizeof sql, "DELETE FROM likes WHERE post_id = %lld", (long long)(M - 2));
  fail |= fl_exec(h, sql) || fl_like_n(h, M - 2, 11, 100);
  sqlite3_close(h); h = NULL;
  snprintf(sql, sizeof sql, "{\"id\":%lld,", (long long)(M - 2));
  fail |= fl_open_compare(path, "(g) gap: row below the range (per-post seek)", sql, 0);
  fail |= fl_open_compare(path, "(g) gap: row below the range (count)", "\"like_count\":11}]}", 0); scen++;

  /* (d) only 5 posts, (e) exactly 20 posts; (h) 20 posts, no likes at all */
  if (db_pgo_make(p5, 30, 5, 40, err, sizeof err) || db_pgo_make(p20, 30, 20, 120, err, sizeof err)) {
    printf("pgo_make: %s\n", err); fail = 1; goto rm;
  }
  fail |= fl_open_compare(p5, "(d) 5 posts", NULL, 1); scen++;
  fail |= fl_open_compare(p20, "(e) 20 posts", NULL, 1); scen++;
  if (sqlite3_open_v2(p20, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { printf("open h\n"); fail = 1; goto rm; }
  fail |= fl_exec(h, "DELETE FROM likes");
  sqlite3_close(h); h = NULL;
  fail |= fl_open_compare(p20, "(h) 20 posts, empty likes index", NULL, 1); scen++;

  printf("feedlikes compared btree vs SQL feed %d times\n", fl_cmp);
  if (!fail) printf("feedlikes test ok scenarios=%d\n", scen);
rm:
  if (h) sqlite3_close(h);
  if (d) db_close(d);
  fl_rm(path); fl_rm(p5); fl_rm(p20);
  rmdir(dir);
  return fail;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--pcache")) return pcache_test();
  if (argc >= 2 && !strcmp(argv[1], "--feedlikes")) return feedlikes_test();
  if (argc < 2) { fprintf(stderr, "usage: dbtest <db> [dump <dir> | bench] | dbtest --pcache | dbtest --feedlikes\n"); return 2; }
  db_t *d = open_or_die(argv[1]);
  if (argc >= 4 && !strcmp(argv[2], "dump")) { dump(d, argv[3]); db_close(d); return 0; }
  if (argc >= 3 && !strcmp(argv[2], "bench")) { bench(d); db_close(d); return 0; }

  if (argc >= 3 && !strcmp(argv[2], "badinsert")) {  /* author 999999 does not exist */
    db_batch_begin(d);
    int m = db_insert_post(d, 999999, "ghost", 5, "boo", 3, buf, sizeof buf);
    printf("insert -> %d commit=%d\n%s\n", m, commit_idle(d), db_plan_line(d));
    m = db_feed_json(d, buf, sizeof buf);
    printf("feed -> %d: %.60s...\n", m, buf);
    db_close(d); return 0;
  }
  int n = db_feed_json(d, buf, sizeof buf);
  printf("feed -> %d: %.300s...\n", n, buf);
  show_post(d, 1); show_post(d, 250000); show_post(d, 500000); show_post(d, 500001);
  printf("db_selfcheck rc=%d\n", db_selfcheck(argv[1], 42));

  int64_t v0 = db_data_version(d);
  /* read-only batch: one snapshot, cursors kept open across requests */
  db_batch_begin(d);
  int f1 = db_feed_json(d, buf, sizeof buf);
  show_post(d, 1); show_post(d, 250000);
  int f2 = db_feed_json(d, buf, sizeof buf);
  printf("read-only batch: feed %d, feed again %d, commit rc=%d\n", f1, f2, commit_idle(d));
  db_batch_begin(d);
  /* read first: the cursor set is open across the writes below */
  printf("in-txn read before write (%s): ", db_plan(d)); show_post(d, 500000);
  n = db_insert_post(d, 1, "golden_ember_1", 14, "hello", 5, buf, sizeof buf);
  printf("insert -> %d: %.*s\n", n, n > 0 ? n : 0, buf);
  int64_t newid = 0;
  if (n > 0) sscanf(buf, "{\"post\":{\"id\":%lld", (long long *)&newid);
  int al = -1, r1 = db_like(d, 1, newid, &al);
  printf("like#1 rc=%d already_liked=%d\n", r1, al);
  int r2 = db_like(d, 1, newid, &al);
  printf("like#2 rc=%d already_liked=%d\n", r2, al);
  int r3 = db_like(d, 1, 999999999, &al);
  printf("like missing post rc=%d\n", r3);
  printf("in-txn read (%s): ", db_plan(d)); show_post(d, newid);
  n = db_feed_json(d, buf, sizeof buf);
  printf("in-txn feed -> %d: %.60s...\n", n, buf);
  printf("commit rc=%d\n", commit_idle(d));
  int64_t v1 = db_data_version(d);
  n = db_feed_json(d, buf, sizeof buf);
  printf("feed after -> %d: %.200s...\n", n, buf);
  show_post(d, newid);
  shm_info(argv[1], "before checkpoint");
  db_maybe_checkpoint(d, 0);
  shm_info(argv[1], "after checkpoint");
  int64_t v2 = db_data_version(d);
  db_batch_begin(d);
  commit_idle(d);
  printf("data_version start=%lld after-own-commit=%lld after-own-passive-checkpoint=%lld\n",
         (long long)v0, (long long)v1, (long long)v2);
  db_batch_begin(d);
  db_like(d, 3, 1, &al);
  commit_idle(d);
  shm_info(argv[1], "after post-checkpoint write (WAL restart)");
  printf("data_version after own write that restarted the WAL=%lld\n", (long long)db_data_version(d));
  db_batch_begin(d);
  printf("%s\n", db_plan_line(d));
  commit_idle(d);

  /* control: a write from another connection must flip the plan */
  sqlite3 *h2;
  sqlite3_open_v2(argv[1], &h2, SQLITE_OPEN_READWRITE, NULL);
  int erc = sqlite3_exec(h2, "INSERT INTO likes (user_id, post_id) VALUES (2, 1)", 0, 0, 0);
  sqlite3_close(h2);
  db_batch_begin(d);
  commit_idle(d);
  printf("after foreign write (rc=%d): data_version=%lld %s\n", erc,
         (long long)db_data_version(d), db_plan_line(d));
  printf("idle after commit: %d/%d checks ok (txn_state=NONE autocommit=1)\n",
         idle_checks - idle_bad, idle_checks);
  db_close(d);
  return idle_bad ? 1 : 0;
}
