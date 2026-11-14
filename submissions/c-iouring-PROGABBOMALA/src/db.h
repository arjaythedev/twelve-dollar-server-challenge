#ifndef DB_H
#define DB_H

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "btree.h"

#define CKPT_FRAMES_DEFAULT 32768 /* restart the log every 128 MiB of WAL */
#define CKPT_PASS_FRAMES 4096     /* copy and sync in between, every 16 MiB */
#define CKPT_INLINE_FRAMES 524288 /* last resort: checkpoint on the loop */
#define CKPT_MAX_FRAMES 131072    /* the log is never left to pass 512 MiB by plan */
#define HOLD_MAX_MS 4000          /* longest time POSTs are parked for the tail copy */
#define HOLD_SPACING 32           /* a restart is this many hold lengths after the last */
#define CKPT_FAILS_INLINE 3

enum { CK_IDLE = 0, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_COPIED, CK_RESTART, CK_TAIL_DONE };

struct db_engine {
  sqlite3 *db;
  sqlite3 *ckpt_db; /* bulk copies, low priority thread */
  sqlite3 *tail_db; /* the copy made while writes are held */
  int sync_fd;      /* the database file, for syncing between copies */
  struct btree_state bt;

  sqlite3_stmt *st_begin, *st_commit, *st_rollback;
  sqlite3_stmt *st_health, *st_ins_post, *st_ins_like, *st_post_exists;
  sqlite3_stmt *st_feed_sql, *st_post_sql;

  int in_txn;
  int broken; /* SQLite rolled the pass's transaction back by itself */

  pthread_t ckpt_thread, tail_thread;
  pthread_mutex_t ckpt_mu;
  pthread_cond_t ckpt_cv;
  _Atomic int ck_state;
  _Atomic int ck_full; /* the tail pass copied the whole log */
  _Atomic int tail_frames; /* log length after the tail thread's own commit */
  char restart_sql[48];
  uint32_t wal_frames; /* log length after the last write commit (WAL hook) */
  uint32_t ck_frames;  /* log length that asks for a restart */
  uint32_t ck_pass;    /* log growth between copies */
  uint32_t ck_next;    /* log length of the next restart attempt */
  uint32_t ck_copy_at; /* log length of the next copy */
  _Atomic int ck_final; /* this copy leads to a hold */
  int writes_held;
  int64_t hold_since_ms;
  int ck_fails; /* cycles in a row that left the log running */
  int ck_fixed; /* CKPT_FRAMES given: no spacing by hold length */
  int64_t copy_since_ms;
  uint32_t copy_frames; /* log length at the last copy */
  uint32_t rate;        /* frames a second, over the last stretch between copies */
};

static int db_exec_fwd(sqlite3 *db, const char *sql);

static int db_ckpt_wait(struct db_engine *d, int a, int b) {
  pthread_mutex_lock(&d->ckpt_mu);
  int st;
  while ((st = atomic_load(&d->ck_state)) != a && st != b) pthread_cond_wait(&d->ckpt_cv, &d->ckpt_mu);
  pthread_mutex_unlock(&d->ckpt_mu);
  return st;
}

static void *db_ckpt_main(void *arg) {
  struct db_engine *d = arg;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19); /* this thread only */
  for (;;) {
    db_ckpt_wait(d, CK_BULK, CK_BULK);
    uint32_t prev = 0;
    int rounds = atomic_load(&d->ck_final) ? 4 : 1;
    for (int round = 0; round < rounds; round++) {
      int nlog = 0, ndone = 0;
      sqlite3_wal_checkpoint_v2(d->ckpt_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
      if (d->sync_fd >= 0) fdatasync(d->sync_fd);
      uint32_t now = d->wal_frames; /* written by the loop, a stale value is fine */
      uint32_t left = now > (uint32_t)ndone ? now - (uint32_t)ndone : 0;
      if (round && (left <= 2048 || left * 2 > prev)) break; /* little left, or not gaining */
      prev = left;
    }
    atomic_store(&d->ck_state, CK_BULK_DONE);
  }
  return NULL;
}

static void *db_tail_main(void *arg) {
  struct db_engine *d = arg;
  for (;;) {
    if (db_ckpt_wait(d, CK_TAIL, CK_RESTART) == CK_TAIL) {
      int nlog = 0, ndone = 0;
      int rc = sqlite3_wal_checkpoint_v2(d->tail_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
      atomic_store(&d->ck_full, rc == SQLITE_OK && nlog == ndone);
      atomic_store(&d->ck_state, CK_TAIL_COPIED);
    } else {
      /* first commit into the new log: its header is synced here */
      atomic_store(&d->tail_frames, 0);
      int rc = sqlite3_exec(d->tail_db, d->restart_sql, NULL, NULL, NULL);
      atomic_store(&d->ck_full, rc == SQLITE_OK && atomic_load(&d->tail_frames) == 1);
      atomic_store(&d->ck_state, CK_TAIL_DONE);
    }
  }
  return NULL;
}

static void db_ckpt_kick(struct db_engine *d, int state) {
  pthread_mutex_lock(&d->ckpt_mu);
  atomic_store(&d->ck_state, state);
  pthread_cond_broadcast(&d->ckpt_cv);
  pthread_mutex_unlock(&d->ckpt_mu);
}

/* the file grows in 64 MiB steps, not page by page */
static void db_chunk(sqlite3 *db) {
  int chunk = 64 << 20;
  sqlite3_file_control(db, "main", SQLITE_FCNTL_CHUNK_SIZE, &chunk);
}

static int db_open_ckpt(const char *path, sqlite3 **out) {
  if (sqlite3_open_v2(path, out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) {
    fprintf(stderr, "db: open checkpoint connection: %s\n", sqlite3_errmsg(*out));
    return 0;
  }
  sqlite3_busy_timeout(*out, 2000);
  db_chunk(*out);
  return db_exec_fwd(*out, "PRAGMA synchronous=NORMAL") && db_exec_fwd(*out, "PRAGMA wal_autocheckpoint=0") &&
         db_exec_fwd(*out, "PRAGMA journal_size_limit=1073741824") && db_exec_fwd(*out, "PRAGMA cache_size=-2048") &&
         db_exec_fwd(*out, "SELECT 1 FROM sqlite_schema LIMIT 1");
}

static int db_wal_hook(void *arg, sqlite3 *db, const char *name, int frames) {
  (void)db;
  (void)name;
  ((struct db_engine *)arg)->wal_frames = (uint32_t)frames;
  return SQLITE_OK;
}

static int db_tail_hook(void *arg, sqlite3 *db, const char *name, int frames) {
  (void)db;
  (void)name;
  atomic_store(&((struct db_engine *)arg)->tail_frames, frames);
  return SQLITE_OK;
}

static int db_user_version(void *out, int n, char **v, char **c) {
  (void)c;
  if (n == 1 && v[0]) *(long *)out = atol(v[0]);
  return 0;
}

/* 1 = held POSTs can go on */
static int db_cycle_end(struct db_engine *d, int restarted, int64_t now_ms) {
  if (restarted) {
    /* the longer a hold lasts, the rarer the restarts */
    uint64_t next = d->ck_frames;
    int64_t hold = now_ms - d->hold_since_ms;
    if (!d->ck_fixed && hold > 0) {
      next = (uint64_t)d->rate * (uint64_t)hold * HOLD_SPACING / 1000;
      if (next < d->ck_frames) next = d->ck_frames;
      if (next > CKPT_MAX_FRAMES) next = CKPT_MAX_FRAMES;
    }
    d->wal_frames = 1;
    d->ck_next = (uint32_t)next;
    d->ck_fails = 0;
    d->copy_frames = 1;
    d->copy_since_ms = now_ms;
  } else {
    d->ck_next = d->wal_frames + d->ck_frames;
    d->ck_fails++;
  }
  d->ck_copy_at = d->wal_frames + d->ck_pass;
  int released = d->writes_held;
  d->writes_held = 0;
  atomic_store(&d->ck_state, CK_IDLE);
  return released;
}

static int db_tick(struct db_engine *d, int64_t now_ms) {
  int released = 0;
  switch (atomic_load(&d->ck_state)) {
    case CK_IDLE:
      if (d->wal_frames >= CKPT_INLINE_FRAMES && d->ck_fails > CKPT_FAILS_INLINE) {
        int nlog = 0, ndone = 0;
        sqlite3_wal_checkpoint_v2(d->db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
        d->ck_next = d->wal_frames + d->ck_frames;
        d->ck_copy_at = d->wal_frames + d->ck_pass;
        d->ck_fails = 0;
      } else if (d->wal_frames >= d->ck_copy_at) {
        if (d->copy_since_ms && now_ms > d->copy_since_ms && d->wal_frames > d->copy_frames)
          d->rate = (uint32_t)((uint64_t)(d->wal_frames - d->copy_frames) * 1000 / (uint64_t)(now_ms - d->copy_since_ms));
        d->copy_frames = d->wal_frames;
        d->copy_since_ms = now_ms;
        atomic_store(&d->ck_final, d->wal_frames >= d->ck_next);
        db_ckpt_kick(d, CK_BULK); /* copy the bulk while writes continue */
      }
      break;
    case CK_BULK_DONE:
      if (!atomic_load(&d->ck_final)) { /* a copy in between, the log goes on */
        d->ck_copy_at = d->wal_frames + d->ck_pass;
        atomic_store(&d->ck_state, CK_IDLE);
        break;
      }
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
    case CK_TAIL_COPIED:
      /* all copied, writes still parked: the thread restarts the log */
      if (d->writes_held && atomic_load(&d->ck_full)) db_ckpt_kick(d, CK_RESTART);
      else released = db_cycle_end(d, 0, now_ms);
      break;
    case CK_TAIL_DONE: /* no time limit while the thread holds the write lock */
      released = db_cycle_end(d, atomic_load(&d->ck_full), now_ms);
      break;
    default:
      break;
  }
  return released;
}

static inline int db_ckpt_active(struct db_engine *d) { return atomic_load(&d->ck_state) != CK_IDLE; }

static int db_exec(sqlite3 *db, const char *sql);
static int db_exec_fwd(sqlite3 *db, const char *sql) { return db_exec(db, sql); }

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
  db_chunk(d->db);
  int wal = 0;
  sqlite3_exec(d->db, "PRAGMA journal_mode=WAL", db_wal_mode, &wal, NULL);
  if (!wal) {
    fprintf(stderr, "db: could not switch to WAL\n");
    return 0;
  }
  if (!db_exec(d->db, "PRAGMA synchronous=NORMAL") || !db_exec(d->db, "PRAGMA temp_store=MEMORY") ||
      !db_exec(d->db, "PRAGMA cache_size=-65536") || !db_exec(d->db, "PRAGMA mmap_size=0") ||
      !db_exec(d->db, "PRAGMA wal_autocheckpoint=0") || !db_exec(d->db, "PRAGMA journal_size_limit=1073741824"))
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
  d->ck_fixed = cf > 0;
  d->ck_pass = d->ck_frames < CKPT_PASS_FRAMES ? d->ck_frames : CKPT_PASS_FRAMES;
  d->ck_next = d->ck_frames;
  d->ck_copy_at = d->ck_pass;
  sqlite3_wal_hook(d->db, db_wal_hook, d);

  if (!db_open_ckpt(path, &d->ckpt_db) || !db_open_ckpt(path, &d->tail_db)) return 0;
  long uv = 0;
  sqlite3_exec(d->tail_db, "PRAGMA user_version", db_user_version, &uv, NULL);
  snprintf(d->restart_sql, sizeof d->restart_sql, "PRAGMA user_version=%ld", uv); /* same value, one frame */
  sqlite3_wal_hook(d->tail_db, db_tail_hook, d);
  sqlite3_exec(d->tail_db, d->restart_sql, NULL, NULL, NULL); /* the log's header is written and synced now */
  d->sync_fd = open(path, O_RDWR | O_CLOEXEC);
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  if (pthread_create(&d->ckpt_thread, &at, db_ckpt_main, d) != 0 ||
      pthread_create(&d->tail_thread, &at, db_tail_main, d) != 0) {
    fprintf(stderr, "db: could not start the checkpoint threads\n");
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
