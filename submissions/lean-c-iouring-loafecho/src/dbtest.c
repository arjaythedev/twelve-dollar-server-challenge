/* dbtest.c — scratch harness for db.c (not part of the server).
 *   dbtest <db>            full run: plan, reads, selfcheck, writes, data_version
 *   dbtest <db> dump <dir> write feed.json, post-{1,250000,500000}.json (no newline)
 *   dbtest <db> bench      checkInv/open time + us per feed render for each plan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: dbtest <db> [dump <dir> | bench]\n"); return 2; }
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
