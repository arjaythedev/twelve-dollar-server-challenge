#ifndef DB_H
#define DB_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "btree.h"

#define CKPT_FRAMES_DEFAULT 4096 /* start a checkpoint cycle at 16 MiB of WAL */
#define CKPT_INLINE_FRAMES 65536 /* last resort: checkpoint on the loop */
#define HOLD_MAX_MS 250          /* longest time POSTs are parked for the tail copy */

enum { CK_IDLE = 0, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };

struct db_engine {
  sqlite3 *db;
  sqlite3 *ckpt_db;
  struct btree_state bt;

  sqlite3_stmt *st_begin, *st_commit, *st_rollback;
  sqlite3_stmt *st_health, *st_ins_post, *st_ins_like, *st_post_exists;
  sqlite3_stmt *st_feed_sql, *st_post_sql;

  int in_txn;
  int broken; /* SQLite rolled the pass's transaction back by itself */

  pthread_t ckpt_thread;
  pthread_mutex_t ckpt_mu;
  pthread_cond_t ckpt_cv;
  _Atomic int ck_state;
  _Atomic int ck_full; /* the tail pass copied the whole log */
  uint32_t wal_frames; /* log length after the last write commit (WAL hook) */
  uint32_t ck_frames;  /* cycle threshold */
  uint32_t ck_next;    /* log length that starts the next cycle */
  int writes_held;
  int64_t hold_since_ms;
};

static void *db_ckpt_main(void *arg) {
  struct db_engine *d = arg;
  /* Per-thread on Linux: the request loop keeps its priority. */
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
  for (;;) {
    pthread_mutex_lock(&d->ckpt_mu);
    int s;
    while ((s = atomic_load(&d->ck_state)) != CK_BULK && s != CK_TAIL) pthread_cond_wait(&d->ckpt_cv, &d->ckpt_mu);
    pthread_mutex_unlock(&d->ckpt_mu);

    int nlog = 0, ndone = 0;
    int rc = sqlite3_wal_checkpoint_v2(d->ckpt_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    if (s == CK_BULK) {
      atomic_store(&d->ck_state, CK_BULK_DONE);
    } else {
      atomic_store(&d->ck_full, rc == SQLITE_OK && nlog == ndone);
      atomic_store(&d->ck_state, CK_TAIL_DONE);
    }
  }
  return NULL;
}

static void db_ckpt_kick(struct db_engine *d, int state) {
  pthread_mutex_lock(&d->ckpt_mu);
  atomic_store(&d->ck_state, state);
  pthread_cond_signal(&d->ckpt_cv);
  pthread_mutex_unlock(&d->ckpt_mu);
}

static int db_wal_hook(void *arg, sqlite3 *db, const char *name, int frames) {
  (void)db;
  (void)name;
  ((struct db_engine *)arg)->wal_frames = (uint32_t)frames;
  return SQLITE_OK;
}

static int db_tick(struct db_engine *d, int64_t now_ms) {
  int released = 0;
  switch (atomic_load(&d->ck_state)) {
    case CK_IDLE:
      if (d->wal_frames >= d->ck_next) {
        db_ckpt_kick(d, CK_BULK); /* copy the bulk while writes continue */
      } else if (d->wal_frames >= CKPT_INLINE_FRAMES) {
        int nlog = 0, ndone = 0;
        sqlite3_wal_checkpoint_v2(d->db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
        d->ck_next = d->wal_frames + d->ck_frames;
      }
      break;
    case CK_BULK_DONE:
      d->writes_held = 1; /* nothing may be appended while the tail is copied */
      d->hold_since_ms = now_ms;
      db_ckpt_kick(d, CK_TAIL);
      break;
    case CK_TAIL:
      if (d->writes_held && now_ms - d->hold_since_ms > HOLD_MAX_MS) {
        d->writes_held = 0;
        released = 1;
      }
      break;
    case CK_TAIL_DONE:
      if (d->writes_held) {
        d->writes_held = 0;
        released = 1;
      }
      if (atomic_load(&d->ck_full)) {
        d->wal_frames = 0;
        d->ck_next = d->ck_frames;
      } else {
        d->ck_next = d->wal_frames + d->ck_frames;
      }
      atomic_store(&d->ck_state, CK_IDLE);
      break;
    default:
      break;
  }
  return released;
}

static inline int db_ckpt_active(struct db_engine *d) { return atomic_load(&d->ck_state) != CK_IDLE; }

static int db_exec(sqlite3 *db, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "db: %s: %s\n", sql, err ? err : "error");
    sqlite3_free(err);
    return 0;
  }
  return 1;
}

static int db_prep(struct db_engine *d, const char *sql, sqlite3_stmt **st) {
  if (sqlite3_prepare_v3(d->db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, NULL) != SQLITE_OK) {
    fprintf(stderr, "db: prepare failed: %s: %s\n", sql, sqlite3_errmsg(d->db));
    return 0;
  }
  return 1;
}

static int db_wal_mode(void *ok, int n, char **v, char **c) {
  (void)c;
  if (n == 1 && v[0] && sqlite3_stricmp(v[0], "wal") == 0) *(int *)ok = 1;
  return 0;
}

#define DB_POST_SELECT                                                        \
  "SELECT p.id, p.body, p.created_at, u.username,"                            \
  " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"                    \
  " FROM posts p JOIN users u ON u.id = p.user_id"

static int db_init(struct db_engine *d, const char *path) {
  memset(d, 0, sizeof *d);
  if (sqlite3_initialize() != SQLITE_OK) {
    fprintf(stderr, "db: sqlite3_initialize failed\n");
    return 0;
  }
  pthread_mutex_init(&d->ckpt_mu, NULL);
  pthread_cond_init(&d->ckpt_cv, NULL);

  if (sqlite3_open_v2(path, &d->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) {
    fprintf(stderr, "db: open %s: %s\n", path, sqlite3_errmsg(d->db));
    return 0;
  }
  sqlite3_busy_timeout(d->db, 2000);
  int wal = 0;
  sqlite3_exec(d->db, "PRAGMA journal_mode=WAL", db_wal_mode, &wal, NULL);
  if (!wal) {
    fprintf(stderr, "db: could not switch to WAL\n");
    return 0;
  }
  if (!db_exec(d->db, "PRAGMA synchronous=NORMAL") || !db_exec(d->db, "PRAGMA temp_store=MEMORY") ||
      !db_exec(d->db, "PRAGMA cache_size=-65536") || !db_exec(d->db, "PRAGMA mmap_size=0") ||
      !db_exec(d->db, "PRAGMA wal_autocheckpoint=0"))
    return 0;

  if (!db_prep(d, "BEGIN", &d->st_begin) || !db_prep(d, "COMMIT", &d->st_commit) ||
      !db_prep(d, "ROLLBACK", &d->st_rollback) || !db_prep(d, "SELECT 1", &d->st_health) ||
      !db_prep(d, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at", &d->st_ins_post) ||
      !db_prep(d, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
               &d->st_ins_like) ||
      !db_prep(d, "SELECT 1 FROM posts WHERE id = ?1", &d->st_post_exists) ||
      !db_prep(d, DB_POST_SELECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &d->st_feed_sql) ||
      !db_prep(d, DB_POST_SELECT " WHERE p.id = ?1", &d->st_post_sql))
    return 0;

  bt_setup(&d->bt, d->db);

  const char *e = getenv("CKPT_FRAMES");
  long cf = e ? atol(e) : 0;
  d->ck_frames = cf > 0 ? (uint32_t)cf : CKPT_FRAMES_DEFAULT;
  d->ck_next = d->ck_frames;
  sqlite3_wal_hook(d->db, db_wal_hook, d);

  if (sqlite3_open_v2(path, &d->ckpt_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) {
    fprintf(stderr, "db: open checkpoint connection: %s\n", sqlite3_errmsg(d->ckpt_db));
    return 0;
  }
  sqlite3_busy_timeout(d->ckpt_db, 2000);
  if (!db_exec(d->ckpt_db, "PRAGMA synchronous=NORMAL") || !db_exec(d->ckpt_db, "PRAGMA wal_autocheckpoint=0") ||
      !db_exec(d->ckpt_db, "PRAGMA cache_size=-2048") || !db_exec(d->ckpt_db, "SELECT 1 FROM sqlite_schema LIMIT 1"))
    return 0;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  if (pthread_create(&d->ckpt_thread, &at, db_ckpt_main, d) != 0) {
    fprintf(stderr, "db: could not start the checkpoint thread\n");
    return 0;
  }
  pthread_attr_destroy(&at);
  return 1;
}

static inline int db_step_reset(sqlite3_stmt *st) {
  int rc = sqlite3_step(st);
  sqlite3_reset(st);
  return rc;
}

static int db_txn_begin(struct db_engine *d) {
  d->broken = 0;
  if (db_step_reset(d->st_begin) != SQLITE_DONE) return -1;
  d->in_txn = 1;
  if (d->bt.ready && sqlite3BtreeBeginTrans(d->bt.pBt, 0, 0) == SQLITE_OK) {
    if (bt_open(&d->bt) != 0) bt_close(&d->bt);
  }
  return 0;
}

static int db_txn_end(struct db_engine *d) {
  if (!d->in_txn) return 0;
  bt_close(&d->bt);
  d->in_txn = 0;
  if (d->broken || sqlite3_get_autocommit(d->db)) {
    if (!sqlite3_get_autocommit(d->db)) db_step_reset(d->st_rollback);
    return -1;
  }
  if (db_step_reset(d->st_commit) == SQLITE_DONE) return 0;
  if (!sqlite3_get_autocommit(d->db)) db_step_reset(d->st_rollback);
  return -1;
}

static inline void db_check_broken(struct db_engine *d) {
  if (sqlite3_get_autocommit(d->db)) d->broken = 1;
}

#endif
