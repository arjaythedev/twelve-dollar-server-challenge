#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "sqlite3.c"
#include "protocol.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#ifndef KEYINFO_ORDER_DESC
#define KEYINFO_ORDER_DESC 0x10
#endif

#define SL_FEED_LIMIT      20
#define SL_CK_BULK_FRAMES  4096
#define SL_CK_FORCE        65536
#define SL_CK_TAIL_MS      250
#define SL_REC_STACK       4096
#define SL_MAX_REC_FIELDS  8

#define SL_SCAN_OK          0
#define SL_SCAN_FALLBACK    1
#define SL_SCAN_ERROR      (-1)

enum sl_ck_state {
  SL_CK_IDLE = 0,
  SL_CK_BULK,
  SL_CK_TAIL
};

struct sl_buf {
  uint8_t *p;
  size_t cap;
  uint8_t stack[SL_REC_STACK];
};

struct sl_jw {
  uint8_t *out;
  size_t cap;
  size_t off;
  int overflow;
};

struct sl_field {
  uint32_t type;
  const uint8_t *ptr;
  size_t len;
};

struct sl_post_row {
  int64_t user_id;
  const uint8_t *body;
  size_t body_len;
  const uint8_t *created_at;
  size_t created_len;
};

struct sl_user_row {
  const uint8_t *username;
  size_t username_len;
  const uint8_t *created_at;
  size_t created_len;
};

struct sl_db {
  sqlite3 *db;
  sqlite3 *ckdb;
  Btree *bt;

  Pgno root_posts;
  Pgno root_users;
  Pgno root_likes;
  Pgno root_likes_idx;
  Pgno root_posts_idx;

  KeyInfo *ki_posts_idx;
  KeyInfo *ki_likes_idx;

  BtCursor *cur_posts;
  BtCursor *cur_users;
  BtCursor *cur_likes_idx;
  BtCursor *cur_posts_idx;
  int cursors_alloc;
  int cursors_open;

  sqlite3_stmt *st_begin;
  sqlite3_stmt *st_commit;
  sqlite3_stmt *st_rollback;
  sqlite3_stmt *st_ins_post;
  sqlite3_stmt *st_ins_like;
  sqlite3_stmt *st_sel1;
  sqlite3_stmt *st_feed_fallback;
  sqlite3_stmt *st_post_fallback;

  int opened;
  int in_batch;
  int batch_broken;

  int sync_init;
  pthread_mutex_t ck_mu;
  pthread_cond_t ck_cv;
  pthread_t ck_thread;
  int ck_thread_started;
  int ck_stop;
  int ck_wake;
  int ck_state;
  int ck_wal_frames;
  int ck_force;
  int ck_req_seq;
  int ck_done_seq;
  int ck_bulk_seq;
  int ck_tail_seq;
  int64_t ck_tail_until;
};

static struct sl_db g_sl;

static void sl_cleanup_state(void);
static void sl_cursors_close(void);
static void sl_rollback_internal(void);
static int sl_cursors_bind_all(void);
static int sl_buf_ensure_available(struct sl_buf *b, size_t need);

static void sl_set_err(char *err, size_t cap, const char *fmt, ...)
{
  va_list ap;
  if (err == NULL || cap == 0) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
  err[cap - 1] = '\0';
}

static void sl_set_sqlite_err(char *err, size_t cap, sqlite3 *db,
                              const char *what)
{
  const char *msg = db ? sqlite3_errmsg(db) : "unknown error";
  sl_set_err(err, cap, "%s: %s", what, msg ? msg : "unknown error");
}

static int64_t sl_now_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static int sl_ieq(const char *a, const char *b)
{
  unsigned char ca;
  unsigned char cb;
  if (a == NULL || b == NULL) {
    return 0;
  }
  do {
    ca = (unsigned char)*a++;
    cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (unsigned char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (unsigned char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return 0;
    }
  } while (ca != 0);
  return 1;
}

static void sl_buf_init(struct sl_buf *b)
{
  b->p = b->stack;
  b->cap = sizeof(b->stack);
}

static void sl_buf_free(struct sl_buf *b)
{
  if (b->p != b->stack) {
    sqlite3_free(b->p);
  }
  b->p = b->stack;
  b->cap = sizeof(b->stack);
}

static int sl_buf_ensure_available(struct sl_buf *b, size_t need)
{
  uint8_t *np;
  if (need <= b->cap) {
    return 0;
  }
  np = (uint8_t *)sqlite3_malloc64((sqlite3_uint64)need);
  if (np == NULL) {
    return -1;
  }
  if (b->p != b->stack) {
    sqlite3_free(b->p);
  }
  b->p = np;
  b->cap = need;
  return 0;
}

static int sl_exec(sqlite3 *db, const char *sql, char *err, size_t cap)
{
  char *msg = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
  if (rc != SQLITE_OK) {
    sl_set_err(err, cap, "sqlite exec failed for \"%s\": %s", sql,
               msg ? msg : sqlite3_errmsg(db));
    sqlite3_free(msg);
    return -1;
  }
  sqlite3_free(msg);
  return 0;
}

static int sl_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out,
                      char *err, size_t cap)
{
  int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
  if (rc != SQLITE_OK) {
    sl_set_err(err, cap, "sqlite prepare failed for \"%s\": %s", sql,
               sqlite3_errmsg(db));
    *out = NULL;
    return -1;
  }
  return 0;
}

static int sl_query_wal_mode(sqlite3 *db, char *err, size_t cap)
{
  sqlite3_stmt *st = NULL;
  int rc;
  if (sl_prepare(db, "PRAGMA journal_mode", &st, err, cap) != 0) {
    return -1;
  }
  rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sl_set_err(err, cap, "could not read journal_mode: %s",
               sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return -1;
  }
  if (!sl_ieq((const char *)sqlite3_column_text(st, 0), "wal")) {
    sl_set_err(err, cap, "database is not in WAL mode (journal_mode=%s)",
               (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);
  return 0;
}

static int sl_setup_connection(sqlite3 *db, char *err, size_t cap)
{
  if (sl_exec(db, "PRAGMA journal_mode=WAL", err, cap) != 0) {
    return -1;
  }
  if (sl_query_wal_mode(db, err, cap) != 0) {
    return -1;
  }
  if (sl_exec(db, "PRAGMA synchronous=NORMAL", err, cap) != 0) {
    return -1;
  }
  if (sl_exec(db, "PRAGMA temp_store=MEMORY", err, cap) != 0) {
    return -1;
  }
  if (sl_exec(db, "PRAGMA cache_size=-65536", err, cap) != 0) {
    return -1;
  }
  if (sl_exec(db, "PRAGMA mmap_size=0", err, cap) != 0) {
    return -1;
  }
  if (sl_exec(db, "PRAGMA wal_autocheckpoint=0", err, cap) != 0) {
    return -1;
  }
  return 0;
}

static int sl_schema_lookup(sqlite3 *db, const char *type, const char *name,
                            const char *expect_tbl, Pgno *root_out,
                            char *err, size_t cap)
{
  static const char sql[] =
      "SELECT rootpage, tbl_name FROM sqlite_schema "
      "WHERE type=?1 AND name=?2";
  sqlite3_stmt *st = NULL;
  int rc;
  int result = -1;
  sqlite3_int64 root;
  const unsigned char *tbl;

  if (sl_prepare(db, sql, &st, err, cap) != 0) {
    return -1;
  }
  sqlite3_bind_text(st, 1, type, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
  rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sl_set_err(err, cap, "schema entry %s \"%s\" is missing", type, name);
    goto done;
  }
  root = sqlite3_column_int64(st, 0);
  if (root <= 1) {
    sl_set_err(err, cap, "schema entry %s \"%s\" has invalid rootpage %lld",
               type, name, (long long)root);
    goto done;
  }
  tbl = sqlite3_column_text(st, 1);
  if (expect_tbl != NULL &&
      (tbl == NULL || strcmp((const char *)tbl, expect_tbl) != 0)) {
    sl_set_err(err, cap, "schema entry %s \"%s\" belongs to \"%s\", expected \"%s\"",
               type, name, tbl ? (const char *)tbl : "(null)", expect_tbl);
    goto done;
  }
  if ((sqlite3_uint64)root > (sqlite3_uint64)0xffffffffu) {
    sl_set_err(err, cap, "rootpage for %s \"%s\" is too large", type, name);
    goto done;
  }
  *root_out = (Pgno)root;
  result = 0;

done:
  sqlite3_finalize(st);
  return result;
}

static int sl_verify_table(sqlite3 *db, const char *table,
                           const char *const *names,
                           const char *const *decl_types,
                           const int *pk_order, int ncol,
                           char *err, size_t cap)
{
  char sql[128];
  sqlite3_stmt *st = NULL;
  int rc;
  int i = 0;
  int ok = 1;

  if (snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table) >=
      (int)sizeof(sql)) {
    sl_set_err(err, cap, "table name too long: %s", table);
    return -1;
  }
  if (sl_prepare(db, sql, &st, err, cap) != 0) {
    return -1;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const char *nm = (const char *)sqlite3_column_text(st, 1);
    const char *ty = (const char *)sqlite3_column_text(st, 2);
    int pk = sqlite3_column_int(st, 5);
    if (i >= ncol || nm == NULL || ty == NULL ||
        strcmp(nm, names[i]) != 0 || strcmp(ty, decl_types[i]) != 0 ||
        pk != pk_order[i]) {
      ok = 0;
      break;
    }
    i++;
  }
  if (rc != SQLITE_DONE || i != ncol) {
    ok = 0;
  }
  sqlite3_finalize(st);
  if (!ok) {
    sl_set_err(err, cap, "table \"%s\" does not match the required schema",
               table);
    return -1;
  }
  return 0;
}

static int sl_verify_index(sqlite3 *db, const char *index,
                           const char *const *names, const int *descs,
                           int nkey, char *err, size_t cap)
{
  char sql[160];
  sqlite3_stmt *st = NULL;
  int rc;
  int nk = 0;
  int naux = 0;
  int ok = 1;

  if (snprintf(sql, sizeof(sql), "PRAGMA index_xinfo(\"%s\")", index) >=
      (int)sizeof(sql)) {
    sl_set_err(err, cap, "index name too long: %s", index);
    return -1;
  }
  if (sl_prepare(db, sql, &st, err, cap) != 0) {
    return -1;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    int is_key = sqlite3_column_int(st, 5);
    if (is_key) {
      const char *nm = (const char *)sqlite3_column_text(st, 2);
      const char *coll = (const char *)sqlite3_column_text(st, 4);
      int cid = sqlite3_column_int(st, 1);
      int desc = sqlite3_column_int(st, 3);
      if (coll == NULL || strcmp(coll, "BINARY") != 0 || nk >= nkey) {
        ok = 0;
        break;
      }
      if (nm != NULL && strcmp(nm, names[nk]) == 0 && desc == descs[nk]) {
        nk++;
      } else if (cid < 0 && strcmp(names[nk], "id") == 0 &&
                 desc == descs[nk]) {

        nk++;
      } else {
        ok = 0;
        break;
      }
    } else {
      naux++;
    }
  }
  if (rc != SQLITE_DONE) {
    ok = 0;
  }
  while (ok && nk < nkey && strcmp(names[nk], "id") == 0) {
    nk++;
  }
  if (naux > 1 || nk != nkey) {
    ok = 0;
  }
  sqlite3_finalize(st);
  if (!ok) {
    sl_set_err(err, cap, "index \"%s\" does not match the required layout",
               index);
    return -1;
  }
  return 0;
}

static int sl_verify_schema(sqlite3 *db, char *err, size_t cap)
{
  static const char *const user_names[] = { "id", "username", "created_at" };
  static const char *const user_types[] = { "INTEGER", "TEXT", "TEXT" };
  static const int user_pk[] = { 1, 0, 0 };

  static const char *const post_names[] = {
    "id", "user_id", "body", "created_at"
  };
  static const char *const post_types[] = {
    "INTEGER", "INTEGER", "TEXT", "TEXT"
  };
  static const int post_pk[] = { 1, 0, 0, 0 };

  static const char *const like_names[] = {
    "user_id", "post_id", "created_at"
  };
  static const char *const like_types[] = {
    "INTEGER", "INTEGER", "TEXT"
  };
  static const int like_pk[] = { 1, 2, 0 };

  static const char *const posts_created_names[] = { "created_at", "id" };
  static const int posts_created_desc[] = { 1, 1 };
  static const char *const posts_user_names[] = { "user_id" };
  static const int posts_user_desc[] = { 0 };
  static const char *const likes_post_names[] = { "post_id" };
  static const int likes_post_desc[] = { 0 };

  if (sl_verify_table(db, "users", user_names, user_types, user_pk, 3,
                      err, cap) != 0) {
    return -1;
  }
  if (sl_verify_table(db, "posts", post_names, post_types, post_pk, 4,
                      err, cap) != 0) {
    return -1;
  }
  if (sl_verify_table(db, "likes", like_names, like_types, like_pk, 3,
                      err, cap) != 0) {
    return -1;
  }
  if (sl_verify_index(db, "posts_created_at_id_idx", posts_created_names,
                      posts_created_desc, 2, err, cap) != 0) {
    return -1;
  }
  if (sl_verify_index(db, "posts_user_id_idx", posts_user_names,
                      posts_user_desc, 1, err, cap) != 0) {
    return -1;
  }
  if (sl_verify_index(db, "likes_post_id_idx", likes_post_names,
                      likes_post_desc, 1, err, cap) != 0) {
    return -1;
  }

  if (sl_schema_lookup(db, "table", "users", "users",
                       &g_sl.root_users, err, cap) != 0) {
    return -1;
  }
  if (sl_schema_lookup(db, "table", "posts", "posts",
                       &g_sl.root_posts, err, cap) != 0) {
    return -1;
  }
  if (sl_schema_lookup(db, "table", "likes", "likes",
                       &g_sl.root_likes, err, cap) != 0) {
    return -1;
  }
  if (sl_schema_lookup(db, "index", "posts_created_at_id_idx", "posts",
                       &g_sl.root_posts_idx, err, cap) != 0) {
    return -1;
  }
  if (sl_schema_lookup(db, "index", "likes_post_id_idx", "likes",
                       &g_sl.root_likes_idx, err, cap) != 0) {
    return -1;
  }
  return 0;
}

static int sl_make_keyinfo(sqlite3 *db, int nkey, int nextra, int desc,
                           KeyInfo **out, char *err, size_t cap)
{
  KeyInfo *ki;
  int i;
  int nall = nkey + nextra;

  ki = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (ki == NULL) {
    sl_set_err(err, cap, "could not allocate KeyInfo");
    return -1;
  }
  for (i = 0; i < nall; i++) {
    ki->aColl[i] = db->pDfltColl;
    ki->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  }
  *out = ki;
  return 0;
}

static int sl_cursor_alloc(BtCursor **out)
{
  int sz = sqlite3BtreeCursorSize();
  BtCursor *c;
  if (sz <= 0) {
    return -1;
  }
  c = (BtCursor *)sqlite3_malloc64((sqlite3_uint64)sz);
  if (c == NULL) {
    return -1;
  }
  sqlite3BtreeCursorZero(c);
  *out = c;
  return 0;
}

static int sl_cursors_alloc_all(void)
{
  if (sl_cursor_alloc(&g_sl.cur_posts) != 0) {
    goto failed;
  }
  if (sl_cursor_alloc(&g_sl.cur_users) != 0) {
    goto failed;
  }
  if (sl_cursor_alloc(&g_sl.cur_likes_idx) != 0) {
    goto failed;
  }
  if (sl_cursor_alloc(&g_sl.cur_posts_idx) != 0) {
    goto failed;
  }
  g_sl.cursors_alloc = 1;
  return 0;

failed:
  sqlite3_free(g_sl.cur_posts);
  sqlite3_free(g_sl.cur_users);
  sqlite3_free(g_sl.cur_likes_idx);
  sqlite3_free(g_sl.cur_posts_idx);
  g_sl.cur_posts = NULL;
  g_sl.cur_users = NULL;
  g_sl.cur_likes_idx = NULL;
  g_sl.cur_posts_idx = NULL;
  return -1;
}

static void sl_cursors_free_all(void)
{
  if (!g_sl.cursors_alloc) {
    return;
  }
  sqlite3_free(g_sl.cur_posts);
  sqlite3_free(g_sl.cur_users);
  sqlite3_free(g_sl.cur_likes_idx);
  sqlite3_free(g_sl.cur_posts_idx);
  g_sl.cur_posts = NULL;
  g_sl.cur_users = NULL;
  g_sl.cur_likes_idx = NULL;
  g_sl.cur_posts_idx = NULL;
  g_sl.cursors_alloc = 0;
}

static int sl_cursors_bind_all(void)
{
  int mask = 0;
  int rc;

  if (!g_sl.cursors_alloc || g_sl.bt == NULL) {
    return -1;
  }

  sqlite3BtreeCursorZero(g_sl.cur_posts);
  rc = sqlite3BtreeCursor(g_sl.bt, g_sl.root_posts, 0, NULL,
                          g_sl.cur_posts);
  if (rc != SQLITE_OK) {
    goto failed;
  }
  mask |= 1;

  sqlite3BtreeCursorZero(g_sl.cur_users);
  rc = sqlite3BtreeCursor(g_sl.bt, g_sl.root_users, 0, NULL,
                          g_sl.cur_users);
  if (rc != SQLITE_OK) {
    goto failed;
  }
  mask |= 2;

  sqlite3BtreeCursorZero(g_sl.cur_likes_idx);
  rc = sqlite3BtreeCursor(g_sl.bt, g_sl.root_likes_idx, 0,
                          g_sl.ki_likes_idx, g_sl.cur_likes_idx);
  if (rc != SQLITE_OK) {
    goto failed;
  }
  mask |= 4;

  sqlite3BtreeCursorZero(g_sl.cur_posts_idx);
  rc = sqlite3BtreeCursor(g_sl.bt, g_sl.root_posts_idx, 0,
                          g_sl.ki_posts_idx, g_sl.cur_posts_idx);
  if (rc != SQLITE_OK) {
    goto failed;
  }
  mask |= 8;

  g_sl.cursors_open = 1;
  return 0;

failed:
  if (mask & 1) {
    sqlite3BtreeCloseCursor(g_sl.cur_posts);
  }
  if (mask & 2) {
    sqlite3BtreeCloseCursor(g_sl.cur_users);
  }
  if (mask & 4) {
    sqlite3BtreeCloseCursor(g_sl.cur_likes_idx);
  }
  if (mask & 8) {
    sqlite3BtreeCloseCursor(g_sl.cur_posts_idx);
  }
  return -1;
}

static void sl_cursors_close(void)
{
  if (!g_sl.cursors_open) {
    return;
  }
  if (g_sl.cur_posts != NULL) {
    sqlite3BtreeCloseCursor(g_sl.cur_posts);
  }
  if (g_sl.cur_users != NULL) {
    sqlite3BtreeCloseCursor(g_sl.cur_users);
  }
  if (g_sl.cur_likes_idx != NULL) {
    sqlite3BtreeCloseCursor(g_sl.cur_likes_idx);
  }
  if (g_sl.cur_posts_idx != NULL) {
    sqlite3BtreeCloseCursor(g_sl.cur_posts_idx);
  }
  g_sl.cursors_open = 0;
}

static int sl_get_varint(const uint8_t *p, size_t n, uint64_t *out,
                         size_t *used)
{
  uint64_t v = 0;
  size_t i;
  for (i = 0; i < 8; i++) {
    if (i >= n) {
      return -1;
    }
    v = (v << 7) | (uint64_t)(p[i] & 0x7f);
    if ((p[i] & 0x80) == 0) {
      *out = v;
      *used = i + 1;
      return 0;
    }
  }
  if (i < n) {
    v = (v << 8) | (uint64_t)p[8];
    *out = v;
    *used = 9;
    return 0;
  }
  return -1;
}

static int sl_serial_size(uint32_t type, uint32_t *out)
{
  uint64_t sz;
  switch (type) {
    case 0:
      *out = 0;
      return 0;
    case 1:
      *out = 1;
      return 0;
    case 2:
      *out = 2;
      return 0;
    case 3:
      *out = 3;
      return 0;
    case 4:
      *out = 4;
      return 0;
    case 5:
      *out = 6;
      return 0;
    case 6:
    case 7:
      *out = 8;
      return 0;
    case 8:
    case 9:
      *out = 0;
      return 0;
    case 10:
    case 11:
      return -1;
    default:
      sz = ((uint64_t)type - 12u) / 2u;
      if (sz > 0xffffffffu) {
        return -1;
      }
      *out = (uint32_t)sz;
      return 0;
  }
}

static int sl_record_parse(const uint8_t *rec, size_t n,
                           struct sl_field *fields, int max_fields,
                           int *nfields_out)
{
  uint64_t header;
  size_t used;
  size_t pos;
  size_t data;
  int nf = 0;
  int i;

  if (rec == NULL || n == 0) {
    return -1;
  }
  if (sl_get_varint(rec, n, &header, &used) != 0) {
    return -1;
  }
  if (header < (uint64_t)used || header > (uint64_t)n) {
    return -1;
  }
  pos = used;
  while (pos < (size_t)header) {
    uint64_t st;
    size_t su;
    if (nf >= max_fields) {
      return -1;
    }
    if (sl_get_varint(rec + pos, (size_t)header - pos, &st, &su) != 0) {
      return -1;
    }
    if (st > 0xffffffffu) {
      return -1;
    }
    pos += su;
    fields[nf].type = (uint32_t)st;
    nf++;
  }
  if (pos != (size_t)header) {
    return -1;
  }

  data = (size_t)header;
  for (i = 0; i < nf; i++) {
    uint32_t len;
    if (sl_serial_size(fields[i].type, &len) != 0) {
      return -1;
    }
    if ((size_t)len > n || data > n - (size_t)len) {
      return -1;
    }
    fields[i].ptr = rec + data;
    fields[i].len = (size_t)len;
    data += (size_t)len;
  }
  *nfields_out = nf;
  return 0;
}

static int sl_field_int(const struct sl_field *f, int64_t *out)
{
  uint32_t type = f->type;
  uint32_t len;
  uint64_t v = 0;
  uint32_t i;

  if (type == 0) {
    *out = 0;
    return 0;
  }
  if (type == 8) {
    *out = 0;
    return 0;
  }
  if (type == 9) {
    *out = 1;
    return 0;
  }
  if (type < 1 || type > 6) {
    return -1;
  }
  len = (type == 1) ? 1u : (type == 2) ? 2u : (type == 3) ? 3u :
        (type == 4) ? 4u : (type == 5) ? 6u : 8u;
  if (f->len != (size_t)len) {
    return -1;
  }
  for (i = 0; i < len; i++) {
    v = (v << 8) | (uint64_t)f->ptr[i];
  }
  if (len < 8 && (f->ptr[0] & 0x80) != 0) {
    v |= (~(uint64_t)0) << (len * 8);
  }
  *out = (int64_t)v;
  return 0;
}

static int sl_field_is_text(const struct sl_field *f)
{
  return f->type >= 13 && (f->type & 1u) != 0;
}

static int sl_parse_post_table(const uint8_t *rec, size_t n,
                               struct sl_post_row *out)
{
  struct sl_field f[SL_MAX_REC_FIELDS];
  int nf = 0;
  if (sl_record_parse(rec, n, f, SL_MAX_REC_FIELDS, &nf) != 0) {
    return -1;
  }
  if (nf != 4) {
    return -1;
  }
  if (f[0].type != 0) {
    return -1;
  }
  if (sl_field_int(&f[1], &out->user_id) != 0) {
    return -1;
  }
  if (!sl_field_is_text(&f[2]) || f[2].len == 0) {
    return -1;
  }
  if (!sl_field_is_text(&f[3]) || f[3].len == 0) {
    return -1;
  }
  out->body = f[2].ptr;
  out->body_len = f[2].len;
  out->created_at = f[3].ptr;
  out->created_len = f[3].len;
  return 0;
}

static int sl_parse_user_table(const uint8_t *rec, size_t n,
                               struct sl_user_row *out)
{
  struct sl_field f[SL_MAX_REC_FIELDS];
  int nf = 0;
  if (sl_record_parse(rec, n, f, SL_MAX_REC_FIELDS, &nf) != 0) {
    return -1;
  }
  if (nf != 3) {
    return -1;
  }
  if (f[0].type != 0) {
    return -1;
  }
  if (!sl_field_is_text(&f[1]) || f[1].len == 0) {
    return -1;
  }
  if (!sl_field_is_text(&f[2]) || f[2].len == 0) {
    return -1;
  }
  out->username = f[1].ptr;
  out->username_len = f[1].len;
  out->created_at = f[2].ptr;
  out->created_len = f[2].len;
  return 0;
}

static int sl_parse_posts_idx(const uint8_t *rec, size_t n, int64_t *id_out)
{
  struct sl_field f[SL_MAX_REC_FIELDS];
  int nf = 0;
  if (sl_record_parse(rec, n, f, SL_MAX_REC_FIELDS, &nf) != 0) {
    return -1;
  }
  if (nf < 2 || nf > 3) {
    return -1;
  }
  if (!sl_field_is_text(&f[0]) || f[0].len == 0) {
    return -1;
  }
  if (sl_field_int(&f[1], id_out) != 0) {
    return -1;
  }
  return 0;
}

static int sl_parse_likes_idx(const uint8_t *rec, size_t n, int64_t *key_out)
{
  struct sl_field f[SL_MAX_REC_FIELDS];
  int nf = 0;
  if (sl_record_parse(rec, n, f, SL_MAX_REC_FIELDS, &nf) != 0) {
    return -1;
  }
  if (nf < 1) {
    return -1;
  }
  if (sl_field_int(&f[0], key_out) != 0) {
    return -1;
  }
  return 0;
}

static int sl_cursor_read(BtCursor *cur, struct sl_buf *b,
                          const uint8_t **rec_out, size_t *len_out)
{
  uint32_t n;
  uint32_t nlocal = 0;
  const void *local;
  int rc;

  n = sqlite3BtreePayloadSize(cur);
  if (sl_buf_ensure_available(b, (size_t)n) != 0) {
    return -1;
  }
  if (n == 0) {
    *rec_out = b->p;
    *len_out = 0;
    return 0;
  }
  local = sqlite3BtreePayloadFetch(cur, &nlocal);
  if (local != NULL && nlocal >= n) {
    memcpy(b->p, local, (size_t)n);
  } else {
    rc = sqlite3BtreePayload(cur, 0, n, b->p);
    if (rc != SQLITE_OK) {
      return -1;
    }
  }
  *rec_out = b->p;
  *len_out = (size_t)n;
  return 0;
}

static int sl_jw_put(struct sl_jw *w, const void *src, size_t n)
{
  if (w->overflow) {
    return -1;
  }
  if (n == 0) {
    return 0;
  }
  if (w->off > w->cap || n > w->cap - w->off) {
    w->overflow = 1;
    return -1;
  }
  if (w->out == NULL) {
    w->overflow = 1;
    return -1;
  }
  memcpy(w->out + w->off, src, n);
  w->off += n;
  return 0;
}

static int sl_jw_lit(struct sl_jw *w, const char *s)
{
  return sl_jw_put(w, s, strlen(s));
}

static int sl_jw_i64(struct sl_jw *w, int64_t v)
{
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
  if (n < 0 || n >= (int)sizeof(tmp)) {
    return -1;
  }
  return sl_jw_put(w, tmp, (size_t)n);
}

static int sl_jw_escaped(struct sl_jw *w, const uint8_t *src, size_t len)
{
  size_t no;
  if (sl_jw_put(w, "\"", 1) != 0) {
    return -1;
  }
  if (src == NULL) {
    if (len != 0) {
      return -1;
    }
    src = (const uint8_t *)"";
  }
  if (w->off > w->cap) {
    w->overflow = 1;
    return -1;
  }
  no = json_escape_append(w->out, w->off, w->cap, src, len);
  if (no > w->cap) {
    w->overflow = 1;
    return -1;
  }
  w->off = no;
  return sl_jw_put(w, "\"", 1);
}

static int sl_emit_post_obj(struct sl_jw *w, int64_t id,
                            const uint8_t *body, size_t body_len,
                            const uint8_t *created_at, size_t created_len,
                            const uint8_t *author, size_t author_len,
                            int64_t likes)
{
  if (sl_jw_lit(w, "{\"id\":") != 0 ||
      sl_jw_i64(w, id) != 0 ||
      sl_jw_lit(w, ",\"body\":") != 0 ||
      sl_jw_escaped(w, body, body_len) != 0 ||
      sl_jw_lit(w, ",\"created_at\":") != 0 ||
      sl_jw_escaped(w, created_at, created_len) != 0 ||
      sl_jw_lit(w, ",\"author\":") != 0 ||
      sl_jw_escaped(w, author, author_len) != 0 ||
      sl_jw_lit(w, ",\"like_count\":") != 0 ||
      sl_jw_i64(w, likes) != 0 ||
      sl_jw_lit(w, "}") != 0) {
    return -1;
  }
  return 0;
}

static int sl_likes_count(int64_t post_id, int64_t *count_out,
                          int *unexpected_out)
{
  Mem mem;
  UnpackedRecord key;
  struct sl_buf b;
  int rc;
  int res = 0;
  int64_t keyval = 0;

  *count_out = 0;
  *unexpected_out = 0;

  memset(&mem, 0, sizeof(mem));
  mem.flags = MEM_Int;
  mem.u.i = post_id;

  memset(&key, 0, sizeof(key));
  key.pKeyInfo = g_sl.ki_likes_idx;
  key.nField = 1;
  key.aMem = &mem;
  key.default_rc = +1;

  sl_buf_init(&b);
  rc = sqlite3BtreeIndexMoveto(g_sl.cur_likes_idx, &key, &res);
  if (rc != SQLITE_OK) {
    sl_buf_free(&b);
    return -1;
  }
  if (sqlite3BtreeEof(g_sl.cur_likes_idx)) {
    sl_buf_free(&b);
    return 0;
  }

  {
    const uint8_t *rec = NULL;
    size_t n = 0;
    if (sl_cursor_read(g_sl.cur_likes_idx, &b, &rec, &n) != 0 ||
        sl_parse_likes_idx(rec, n, &keyval) != 0) {
      *unexpected_out = 1;
      sl_buf_free(&b);
      return 0;
    }
  }

  while (keyval < post_id) {
    const uint8_t *rec = NULL;
    size_t n = 0;
    rc = sqlite3BtreeNext(g_sl.cur_likes_idx, 0);
    if (rc == SQLITE_DONE) {
      sl_buf_free(&b);
      return 0;
    }
    if (rc != SQLITE_OK) {
      sl_buf_free(&b);
      return -1;
    }
    if (sqlite3BtreeEof(g_sl.cur_likes_idx)) {
      sl_buf_free(&b);
      return 0;
    }
    if (sl_cursor_read(g_sl.cur_likes_idx, &b, &rec, &n) != 0 ||
        sl_parse_likes_idx(rec, n, &keyval) != 0) {
      *unexpected_out = 1;
      sl_buf_free(&b);
      return 0;
    }
  }

  if (keyval > post_id) {
    sl_buf_free(&b);
    return 0;
  }

  for (;;) {
    const uint8_t *rec = NULL;
    size_t n = 0;
    (*count_out)++;
    rc = sqlite3BtreeNext(g_sl.cur_likes_idx, 0);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_OK) {
      sl_buf_free(&b);
      return -1;
    }
    if (sqlite3BtreeEof(g_sl.cur_likes_idx)) {
      break;
    }
    if (sl_cursor_read(g_sl.cur_likes_idx, &b, &rec, &n) != 0 ||
        sl_parse_likes_idx(rec, n, &keyval) != 0) {
      *unexpected_out = 1;
      sl_buf_free(&b);
      return 0;
    }
    if (keyval < post_id) {
      *unexpected_out = 1;
      break;
    }
    if (keyval > post_id) {
      break;
    }
  }

  sl_buf_free(&b);
  return 0;
}

static int sl_lookup_post(int64_t post_id, struct sl_buf *b,
                          struct sl_post_row *out)
{
  int res = 0;
  const uint8_t *rec = NULL;
  size_t n = 0;
  int rc;

  rc = sqlite3BtreeTableMoveto(g_sl.cur_posts, post_id, 0, &res);
  if (rc != SQLITE_OK) {
    return -1;
  }
  if (res != 0) {
    return 1;
  }
  if (sl_cursor_read(g_sl.cur_posts, b, &rec, &n) != 0) {
    return -1;
  }
  if (sl_parse_post_table(rec, n, out) != 0) {
    return 2;
  }
  return 0;
}

static int sl_lookup_user(int64_t user_id, struct sl_buf *b,
                          struct sl_user_row *out)
{
  int res = 0;
  const uint8_t *rec = NULL;
  size_t n = 0;
  int rc;

  rc = sqlite3BtreeTableMoveto(g_sl.cur_users, user_id, 0, &res);
  if (rc != SQLITE_OK) {
    return -1;
  }
  if (res != 0) {
    return 1;
  }
  if (sl_cursor_read(g_sl.cur_users, b, &rec, &n) != 0) {
    return -1;
  }
  if (sl_parse_user_table(rec, n, out) != 0) {
    return 2;
  }
  return 0;
}

static int sl_feed_direct(struct sl_jw *w)
{
  struct sl_buf idxb;
  struct sl_buf postb;
  struct sl_buf userb;
  int result = SL_SCAN_ERROR;
  int empty = 0;
  int count = 0;
  int first = 1;
  int rc;

  sl_buf_init(&idxb);
  sl_buf_init(&postb);
  sl_buf_init(&userb);

  if (sl_jw_lit(w, "{\"posts\":[") != 0) {
    goto done;
  }

  rc = sqlite3BtreeFirst(g_sl.cur_posts_idx, &empty);
  if (rc != SQLITE_OK) {
    goto done;
  }

  while (!empty && count < SL_FEED_LIMIT) {
    const uint8_t *rec = NULL;
    size_t n = 0;
    int64_t post_id = 0;
    int64_t likes = 0;
    int unexpected = 0;
    int lr;
    struct sl_post_row post;
    struct sl_user_row user;

    if (sl_cursor_read(g_sl.cur_posts_idx, &idxb, &rec, &n) != 0) {
      goto done;
    }
    if (sl_parse_posts_idx(rec, n, &post_id) != 0) {
      result = SL_SCAN_FALLBACK;
      goto done;
    }

    lr = sl_lookup_post(post_id, &postb, &post);
    if (lr < 0) {
      goto done;
    }
    if (lr > 0) {
      result = SL_SCAN_FALLBACK;
      goto done;
    }

    lr = sl_lookup_user(post.user_id, &userb, &user);
    if (lr < 0) {
      goto done;
    }
    if (lr > 0) {
      result = SL_SCAN_FALLBACK;
      goto done;
    }

    if (sl_likes_count(post_id, &likes, &unexpected) != 0) {
      goto done;
    }
    if (unexpected) {
      result = SL_SCAN_FALLBACK;
      goto done;
    }

    if (!first && sl_jw_lit(w, ",") != 0) {
      goto done;
    }
    if (sl_emit_post_obj(w, post_id, post.body, post.body_len,
                         post.created_at, post.created_len,
                         user.username, user.username_len, likes) != 0) {
      goto done;
    }
    first = 0;
    count++;

    rc = sqlite3BtreeNext(g_sl.cur_posts_idx, 0);
    if (rc == SQLITE_DONE) {
      empty = 1;
    } else if (rc != SQLITE_OK) {
      goto done;
    }
  }

  if (sl_jw_lit(w, "]}") != 0) {
    goto done;
  }
  result = SL_SCAN_OK;

done:
  sl_buf_free(&idxb);
  sl_buf_free(&postb);
  sl_buf_free(&userb);
  return result;
}

static int sl_emit_stmt_row(struct sl_jw *w, sqlite3_stmt *st)
{
  sqlite3_int64 id = sqlite3_column_int64(st, 0);
  const unsigned char *body = sqlite3_column_text(st, 3);
  int body_len = sqlite3_column_bytes(st, 3);
  const unsigned char *created = sqlite3_column_text(st, 4);
  int created_len = sqlite3_column_bytes(st, 4);
  const unsigned char *author = sqlite3_column_text(st, 2);
  int author_len = sqlite3_column_bytes(st, 2);
  sqlite3_int64 likes = sqlite3_column_int64(st, 5);

  if (body == NULL) {
    body = (const unsigned char *)"";
    body_len = 0;
  }
  if (created == NULL) {
    created = (const unsigned char *)"";
    created_len = 0;
  }
  if (author == NULL) {
    author = (const unsigned char *)"";
    author_len = 0;
  }
  return sl_emit_post_obj(w, (int64_t)id, body, (size_t)body_len,
                          created, (size_t)created_len,
                          author, (size_t)author_len, (int64_t)likes);
}

static int sl_feed_fallback(struct sl_jw *w)
{
  sqlite3_stmt *st = g_sl.st_feed_fallback;
  int rc;
  int first = 1;

  if (st == NULL) {
    return -1;
  }
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  if (sl_jw_lit(w, "{\"posts\":[") != 0) {
    return -1;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (!first && sl_jw_lit(w, ",") != 0) {
      sqlite3_reset(st);
      return -1;
    }
    if (sl_emit_stmt_row(w, st) != 0) {
      sqlite3_reset(st);
      return -1;
    }
    first = 0;
  }
  sqlite3_reset(st);
  if (rc != SQLITE_DONE) {
    return -1;
  }
  if (sl_jw_lit(w, "]}") != 0) {
    return -1;
  }
  return 0;
}

static int sl_post_fallback(int64_t post_id, struct sl_jw *w)
{
  sqlite3_stmt *st = g_sl.st_post_fallback;
  int rc;

  if (st == NULL) {
    return -1;
  }
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  sqlite3_bind_int64(st, 1, post_id);
  rc = sqlite3_step(st);
  if (rc == SQLITE_DONE) {
    sqlite3_reset(st);
    return 1;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st);
    return -1;
  }
  if (sl_jw_lit(w, "{\"post\":") != 0 ||
      sl_emit_stmt_row(w, st) != 0 ||
      sl_jw_lit(w, "}") != 0) {
    sqlite3_reset(st);
    return -1;
  }
  sqlite3_reset(st);
  return 0;
}

static int sl_wal_hook(void *p_arg, sqlite3 *db, const char *zdb, int n_frame)
{
  struct sl_db *g = (struct sl_db *)p_arg;
  int request = 0;
  int set_bulk_seq = 0;

  (void)db;
  (void)zdb;
  if (g == NULL || !g->sync_init) {
    return SQLITE_OK;
  }

  pthread_mutex_lock(&g->ck_mu);
  g->ck_wal_frames = n_frame;

  if (n_frame >= SL_CK_FORCE) {
    g->ck_force = 1;
    request = 1;
    if (g->ck_state == SL_CK_IDLE) {
      g->ck_state = SL_CK_BULK;
      set_bulk_seq = 1;
    }
  } else if (n_frame >= SL_CK_BULK_FRAMES && g->ck_state == SL_CK_IDLE) {
    g->ck_state = SL_CK_BULK;
    request = 1;
    set_bulk_seq = 1;
  }

  if (request) {
    g->ck_req_seq++;
    if (set_bulk_seq) {
      g->ck_bulk_seq = g->ck_req_seq;
    }
    g->ck_wake = 1;
    pthread_cond_signal(&g->ck_cv);
  }
  pthread_mutex_unlock(&g->ck_mu);
  return SQLITE_OK;
}

static void *sl_ck_thread_main(void *arg)
{
  struct sl_db *g = (struct sl_db *)arg;

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
  (void)setpriority(PRIO_PROCESS, 0, 19);
#endif

  for (;;) {
    int req;
    int rc;
    int n_log = 0;
    int n_ckpt = 0;

    pthread_mutex_lock(&g->ck_mu);
    while (!g->ck_stop && !g->ck_wake) {
      struct timespec ts;
      if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
      }
      ts.tv_nsec += 250 * 1000000L;
      if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
      }
      pthread_cond_timedwait(&g->ck_cv, &g->ck_mu, &ts);
    }
    if (g->ck_stop) {
      pthread_mutex_unlock(&g->ck_mu);
      break;
    }
    req = g->ck_req_seq;
    g->ck_wake = 0;
    pthread_mutex_unlock(&g->ck_mu);

    rc = sqlite3_wal_checkpoint_v2(g->ckdb, NULL,
                                   SQLITE_CHECKPOINT_PASSIVE,
                                   &n_log, &n_ckpt);
    (void)rc;
    (void)n_log;
    (void)n_ckpt;

    pthread_mutex_lock(&g->ck_mu);
    if (req > g->ck_done_seq) {
      g->ck_done_seq = req;
    }
    if (n_ckpt >= n_log) {
      g->ck_wal_frames = 0;
    } else if (n_log > 0) {
      g->ck_wal_frames = n_log - n_ckpt;
    }
    g->ck_force = 0;
    pthread_mutex_unlock(&g->ck_mu);
  }
  return NULL;
}

static int sl_start_ck_thread(char *err, size_t cap)
{
  int rc = pthread_create(&g_sl.ck_thread, NULL, sl_ck_thread_main, &g_sl);
  if (rc != 0) {
    sl_set_err(err, cap, "could not start checkpoint thread: %d", rc);
    return -1;
  }
  g_sl.ck_thread_started = 1;
  return 0;
}

static void sl_stop_ck_thread(void)
{
  if (!g_sl.ck_thread_started) {
    return;
  }
  pthread_mutex_lock(&g_sl.ck_mu);
  g_sl.ck_stop = 1;
  g_sl.ck_wake = 1;
  pthread_cond_broadcast(&g_sl.ck_cv);
  pthread_mutex_unlock(&g_sl.ck_mu);
  pthread_join(g_sl.ck_thread, NULL);
  g_sl.ck_thread_started = 0;
}

static void sl_finalize_stmt(sqlite3_stmt **st)
{
  if (*st != NULL) {
    sqlite3_finalize(*st);
    *st = NULL;
  }
}

static void sl_rollback_internal(void)
{
  if (!g_sl.in_batch) {
    return;
  }
  sl_cursors_close();
  if (g_sl.st_rollback != NULL) {
    sqlite3_reset(g_sl.st_rollback);
    sqlite3_step(g_sl.st_rollback);
    sqlite3_reset(g_sl.st_rollback);
  }
  g_sl.in_batch = 0;
  g_sl.batch_broken = 0;
}

static void sl_cleanup_state(void)
{
  sl_stop_ck_thread();
  if (g_sl.in_batch) {
    sl_rollback_internal();
  }
  sl_cursors_close();
  sl_cursors_free_all();

  sl_finalize_stmt(&g_sl.st_begin);
  sl_finalize_stmt(&g_sl.st_commit);
  sl_finalize_stmt(&g_sl.st_rollback);
  sl_finalize_stmt(&g_sl.st_ins_post);
  sl_finalize_stmt(&g_sl.st_ins_like);
  sl_finalize_stmt(&g_sl.st_sel1);
  sl_finalize_stmt(&g_sl.st_feed_fallback);
  sl_finalize_stmt(&g_sl.st_post_fallback);

  if (g_sl.ki_posts_idx != NULL) {
    sqlite3KeyInfoUnref(g_sl.ki_posts_idx);
    g_sl.ki_posts_idx = NULL;
  }
  if (g_sl.ki_likes_idx != NULL) {
    sqlite3KeyInfoUnref(g_sl.ki_likes_idx);
    g_sl.ki_likes_idx = NULL;
  }
  if (g_sl.ckdb != NULL) {
    sqlite3_close_v2(g_sl.ckdb);
    g_sl.ckdb = NULL;
  }
  if (g_sl.db != NULL) {
    sqlite3_close_v2(g_sl.db);
    g_sl.db = NULL;
  }
  if (g_sl.sync_init) {
    pthread_cond_destroy(&g_sl.ck_cv);
    pthread_mutex_destroy(&g_sl.ck_mu);
    g_sl.sync_init = 0;
  }
  memset(&g_sl, 0, sizeof(g_sl));
}

int db_open(const char *path, char *err, size_t err_cap)
{
  int rc;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;

  if (err != NULL && err_cap != 0) {
    err[0] = '\0';
  }
  if (g_sl.opened || g_sl.db != NULL || g_sl.sync_init) {
    sl_cleanup_state();
  }
  memset(&g_sl, 0, sizeof(g_sl));
  g_sl.ck_state = SL_CK_IDLE;

  if (path == NULL || path[0] == '\0') {
    sl_set_err(err, err_cap, "database path is empty");
    return -1;
  }

  rc = pthread_mutex_init(&g_sl.ck_mu, NULL);
  if (rc != 0) {
    sl_set_err(err, err_cap, "could not initialize checkpoint mutex: %d", rc);
    return -1;
  }
  rc = pthread_cond_init(&g_sl.ck_cv, NULL);
  if (rc != 0) {
    pthread_mutex_destroy(&g_sl.ck_mu);
    memset(&g_sl, 0, sizeof(g_sl));
    sl_set_err(err, err_cap, "could not initialize checkpoint condvar: %d",
               rc);
    return -1;
  }
  g_sl.sync_init = 1;

  rc = sqlite3_initialize();
  if (rc != SQLITE_OK) {
    sl_set_err(err, err_cap, "could not initialize sqlite: %s",
               sqlite3_errstr(rc));
    sl_cleanup_state();
    return -1;
  }

  rc = sqlite3_open_v2(path, &g_sl.db, flags, "unix-excl");
  if (rc != SQLITE_OK) {
    sl_set_err(err, err_cap, "could not open database: %s",
               g_sl.db ? sqlite3_errmsg(g_sl.db) : "unknown error");
    sl_cleanup_state();
    return -1;
  }
  sqlite3_busy_timeout(g_sl.db, 5000);

  rc = sqlite3_open_v2(path, &g_sl.ckdb, flags, "unix-excl");
  if (rc != SQLITE_OK) {
    sl_set_err(err, err_cap, "could not open checkpoint database: %s",
               g_sl.ckdb ? sqlite3_errmsg(g_sl.ckdb) : "unknown error");
    sl_cleanup_state();
    return -1;
  }
  sqlite3_busy_timeout(g_sl.ckdb, 5000);

  if (sl_setup_connection(g_sl.db, err, err_cap) != 0) {
    sl_cleanup_state();
    return -1;
  }
  if (sl_setup_connection(g_sl.ckdb, err, err_cap) != 0) {
    sl_cleanup_state();
    return -1;
  }

  if ((g_sl.db->aDb[0].pBt == NULL) || ENC(g_sl.db) != SQLITE_UTF8 ||
      g_sl.db->pDfltColl == NULL) {
    sl_set_err(err, err_cap, "database connection is not UTF-8/BTREE");
    sl_cleanup_state();
    return -1;
  }
  g_sl.bt = g_sl.db->aDb[0].pBt;

  if (sl_verify_schema(g_sl.db, err, err_cap) != 0) {
    sl_cleanup_state();
    return -1;
  }

  if (sl_make_keyinfo(g_sl.db, 2, 1, 1, &g_sl.ki_posts_idx,
                      err, err_cap) != 0) {
    sl_cleanup_state();
    return -1;
  }
  if (sl_make_keyinfo(g_sl.db, 1, 1, 0, &g_sl.ki_likes_idx,
                      err, err_cap) != 0) {
    sl_cleanup_state();
    return -1;
  }

  if (sl_cursors_alloc_all() != 0) {
    sl_set_err(err, err_cap, "could not allocate B-tree cursors");
    sl_cleanup_state();
    return -1;
  }

  if (sl_prepare(g_sl.db, "BEGIN", &g_sl.st_begin, err, err_cap) != 0 ||
      sl_prepare(g_sl.db, "COMMIT", &g_sl.st_commit, err, err_cap) != 0 ||
      sl_prepare(g_sl.db, "ROLLBACK", &g_sl.st_rollback, err, err_cap) != 0 ||
      sl_prepare(g_sl.db,
                 "INSERT INTO posts (user_id, body) VALUES (?1, ?2) "
                 "RETURNING id, created_at",
                 &g_sl.st_ins_post, err, err_cap) != 0 ||
      sl_prepare(g_sl.db,
                 "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) "
                 "ON CONFLICT DO NOTHING",
                 &g_sl.st_ins_like, err, err_cap) != 0 ||
      sl_prepare(g_sl.db, "SELECT 1", &g_sl.st_sel1, err, err_cap) != 0 ||
      sl_prepare(g_sl.db,
                 "SELECT p.id, p.user_id, u.username, p.body, p.created_at, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
                 "FROM posts p JOIN users u ON u.id=p.user_id "
                 "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
                 &g_sl.st_feed_fallback, err, err_cap) != 0 ||
      sl_prepare(g_sl.db,
                 "SELECT p.id, p.user_id, u.username, p.body, p.created_at, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
                 "FROM posts p JOIN users u ON u.id=p.user_id "
                 "WHERE p.id=?1",
                 &g_sl.st_post_fallback, err, err_cap) != 0) {
    sl_cleanup_state();
    return -1;
  }

  (void)sqlite3_wal_hook(g_sl.db, sl_wal_hook, &g_sl);

  if (sl_start_ck_thread(err, err_cap) != 0) {
    sl_cleanup_state();
    return -1;
  }

  g_sl.opened = 1;
  return 0;
}

void db_close(void)
{
  sl_cleanup_state();
}

int db_health(char *err, size_t err_cap)
{
  int rc;
  int result = -1;
  if (!g_sl.opened || g_sl.st_sel1 == NULL) {
    sl_set_err(err, err_cap, "database is not open");
    return -1;
  }
  sqlite3_reset(g_sl.st_sel1);
  rc = sqlite3_step(g_sl.st_sel1);
  if (rc == SQLITE_ROW && sqlite3_column_int(g_sl.st_sel1, 0) == 1) {
    result = 0;
  } else {
    sl_set_sqlite_err(err, err_cap, g_sl.db, "health query failed");
  }
  sqlite3_reset(g_sl.st_sel1);
  return result;
}

void db_tick(void)
{
  int64_t now;
  if (!g_sl.opened || !g_sl.sync_init) {
    return;
  }
  pthread_mutex_lock(&g_sl.ck_mu);
  now = sl_now_ms();

  if (g_sl.ck_state == SL_CK_BULK) {
    if (g_sl.ck_done_seq >= g_sl.ck_bulk_seq) {
      g_sl.ck_state = SL_CK_TAIL;
      g_sl.ck_tail_until = now + SL_CK_TAIL_MS;
      g_sl.ck_req_seq++;
      g_sl.ck_tail_seq = g_sl.ck_req_seq;
      g_sl.ck_wake = 1;
    }
  } else if (g_sl.ck_state == SL_CK_TAIL) {
    if (g_sl.ck_done_seq >= g_sl.ck_tail_seq || now >= g_sl.ck_tail_until) {
      g_sl.ck_state = SL_CK_IDLE;
      if (g_sl.ck_wal_frames >= SL_CK_BULK_FRAMES) {
        g_sl.ck_state = SL_CK_BULK;
        g_sl.ck_req_seq++;
        g_sl.ck_bulk_seq = g_sl.ck_req_seq;
        g_sl.ck_wake = 1;
      }
    }
  }

  if (g_sl.ck_wake) {
    pthread_cond_signal(&g_sl.ck_cv);
  }
  pthread_mutex_unlock(&g_sl.ck_mu);
}

int db_writes_held(void)
{
  int held = 0;
  if (!g_sl.opened || !g_sl.sync_init) {
    return 0;
  }
  pthread_mutex_lock(&g_sl.ck_mu);
  held = (g_sl.ck_state == SL_CK_TAIL) &&
         (g_sl.ck_done_seq < g_sl.ck_tail_seq) &&
         (sl_now_ms() < g_sl.ck_tail_until);
  pthread_mutex_unlock(&g_sl.ck_mu);
  return held;
}

int db_begin(void)
{
  int rc;
  if (!g_sl.opened || g_sl.st_begin == NULL) {
    return -1;
  }
  if (g_sl.in_batch) {
    return 0;
  }
  sqlite3_reset(g_sl.st_begin);
  rc = sqlite3_step(g_sl.st_begin);
  sqlite3_reset(g_sl.st_begin);
  if (rc != SQLITE_DONE) {
    return -1;
  }
  g_sl.in_batch = 1;
  g_sl.batch_broken = 0;
  if (sqlite3BtreeBeginTrans(g_sl.bt, 0, 0) != SQLITE_OK) {
    sl_rollback_internal();
    g_sl.batch_broken = 1;
    return -1;
  }
  if (sl_cursors_bind_all() != 0) {
    sl_rollback_internal();
    g_sl.batch_broken = 1;
    return -1;
  }
  return 0;
}

int db_commit(void)
{
  int rc;
  if (!g_sl.opened || !g_sl.in_batch) {
    return 0;
  }
  if (g_sl.batch_broken) {
    sl_rollback_internal();
    return -1;
  }
  sl_cursors_close();
  if (g_sl.st_commit == NULL) {
    sl_rollback_internal();
    return -1;
  }
  sqlite3_reset(g_sl.st_commit);
  rc = sqlite3_step(g_sl.st_commit);
  sqlite3_reset(g_sl.st_commit);
  if (rc != SQLITE_DONE) {
    sl_rollback_internal();
    return -1;
  }
  g_sl.in_batch = 0;
  g_sl.batch_broken = 0;
  return 0;
}

void db_rollback(void)
{
  sl_rollback_internal();
}

int db_read_feed(uint8_t *out, size_t out_cap, size_t *out_len)
{
  struct sl_jw w;
  int result;

  if (out_len != NULL) {
    *out_len = 0;
  }
  if (!g_sl.opened || out == NULL || out_len == NULL || out_cap == 0) {
    return -1;
  }

  w.out = out;
  w.cap = out_cap;
  w.off = 0;
  w.overflow = 0;

  if (!g_sl.in_batch || !g_sl.cursors_open) {
    if (sl_feed_fallback(&w) != 0) {
      return -1;
    }
  } else {
    result = sl_feed_direct(&w);
    if (result == SL_SCAN_FALLBACK) {
      w.off = 0;
      w.overflow = 0;
      if (sl_feed_fallback(&w) != 0) {
        return -1;
      }
    } else if (result != SL_SCAN_OK) {
      return -1;
    }
  }

  *out_len = w.off;
  return 0;
}

int db_read_post(int64_t post_id, uint8_t *out, size_t out_cap,
                 size_t *out_len)
{
  struct sl_jw w;
  struct sl_buf postb;
  struct sl_buf userb;
  struct sl_post_row post;
  struct sl_user_row user;
  int64_t likes = 0;
  int unexpected = 0;
  int lr;
  int result = -1;

  if (out_len != NULL) {
    *out_len = 0;
  }
  if (!g_sl.opened || out == NULL || out_len == NULL || out_cap == 0) {
    return -1;
  }

  w.out = out;
  w.cap = out_cap;
  w.off = 0;
  w.overflow = 0;

  if (!g_sl.in_batch || !g_sl.cursors_open) {
    result = sl_post_fallback(post_id, &w);
    if (result != 0) {
      return result;
    }
    *out_len = w.off;
    return 0;
  }

  sl_buf_init(&postb);
  sl_buf_init(&userb);

  lr = sl_lookup_post(post_id, &postb, &post);
  if (lr < 0) {
    result = -1;
    goto done;
  }
  if (lr == 1) {
    result = 1;
    goto done;
  }
  if (lr > 1) {
    result = 2;
    goto done;
  }

  lr = sl_lookup_user(post.user_id, &userb, &user);
  if (lr < 0) {
    result = -1;
    goto done;
  }
  if (lr > 0) {
    result = 2;
    goto done;
  }

  if (sl_likes_count(post_id, &likes, &unexpected) != 0) {
    result = -1;
    goto done;
  }
  if (unexpected) {
    result = 2;
    goto done;
  }

  if (sl_jw_lit(&w, "{\"post\":") != 0 ||
      sl_emit_post_obj(&w, post_id, post.body, post.body_len,
                       post.created_at, post.created_len,
                       user.username, user.username_len, likes) != 0 ||
      sl_jw_lit(&w, "}") != 0) {
    result = -1;
    goto done;
  }
  *out_len = w.off;
  result = 0;

done:
  sl_buf_free(&postb);
  sl_buf_free(&userb);

  if (result == 2) {
    w.off = 0;
    w.overflow = 0;
    result = sl_post_fallback(post_id, &w);
    if (result != 0) {
      return result;
    }
    *out_len = w.off;
    return 0;
  }
  return result;
}

int db_insert_post(int64_t user_id, const uint8_t *body, size_t body_len,
                   int64_t *id_out, char *created_out, size_t created_cap,
                   size_t *created_len_out)
{
  sqlite3_stmt *st;
  int rc;
  sqlite3_int64 id;
  const unsigned char *created;
  int created_len;
  size_t need;

  if (created_len_out != NULL) {
    *created_len_out = 0;
  }
  if (!g_sl.opened || g_sl.st_ins_post == NULL) {
    return -1;
  }
  if (g_sl.in_batch && g_sl.batch_broken) {
    return -1;
  }
  if (body == NULL) {
    body = (const uint8_t *)"";
  }
  if (body_len > (size_t)0x7fffffff) {
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }

  st = g_sl.st_ins_post;
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  sqlite3_bind_int64(st, 1, user_id);
  rc = sqlite3_bind_text(st, 2, (const char *)body, (int)body_len,
                         SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    sqlite3_reset(st);
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }

  rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st);
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }

  id = sqlite3_column_int64(st, 0);
  created = sqlite3_column_text(st, 1);
  created_len = sqlite3_column_bytes(st, 1);
  if (created == NULL || created_len < 0) {
    sqlite3_reset(st);
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }
  need = (size_t)created_len + 1;
  if (created_out == NULL || need > created_cap) {
    sqlite3_reset(st);
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }
  memcpy(created_out, created, (size_t)created_len);
  created_out[created_len] = '\0';
  if (created_len_out != NULL) {
    *created_len_out = (size_t)created_len;
  }

  rc = sqlite3_step(st);
  sqlite3_reset(st);
  if (rc != SQLITE_DONE) {
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }
  if (id_out != NULL) {
    *id_out = (int64_t)id;
  }
  return 0;
}

static int sl_post_exists(int64_t post_id, int *exists_out)
{
  int res = 0;
  int rc;
  sqlite3_stmt *st = NULL;
  int exists = 0;

  if (g_sl.cursors_open) {
    rc = sqlite3BtreeTableMoveto(g_sl.cur_posts, post_id, 0, &res);
    if (rc != SQLITE_OK) {
      return -1;
    }
    *exists_out = (res == 0);
    return 0;
  }

  rc = sqlite3_prepare_v2(g_sl.db, "SELECT 1 FROM posts WHERE id=?1", -1,
                          &st, NULL);
  if (rc != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_int64(st, 1, post_id);
  rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    exists = 1;
  } else if (rc != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);
  *exists_out = exists;
  return 0;
}

int db_insert_like(int64_t user_id, int64_t post_id, int *already_liked_out)
{
  int exists = 0;
  int rc;
  int changes;
  sqlite3_stmt *st;

  if (already_liked_out != NULL) {
    *already_liked_out = 0;
  }
  if (!g_sl.opened || g_sl.st_ins_like == NULL || already_liked_out == NULL) {
    return -1;
  }
  if (g_sl.in_batch && g_sl.batch_broken) {
    return -1;
  }

  if (sl_post_exists(post_id, &exists) != 0) {
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }
  if (!exists) {
    return 1;
  }

  st = g_sl.st_ins_like;
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  sqlite3_bind_int64(st, 1, user_id);
  sqlite3_bind_int64(st, 2, post_id);
  rc = sqlite3_step(st);
  if (rc != SQLITE_DONE) {
    sqlite3_reset(st);
    if (g_sl.in_batch) {
      g_sl.batch_broken = 1;
    }
    return -1;
  }
  changes = sqlite3_changes(g_sl.db);
  sqlite3_reset(st);
  *already_liked_out = (changes == 0) ? 1 : 0;
  return 0;
}
