/* Live SQLite B-tree read plans inspired by the challenge's Lean-derived
 * reference submission. This file is included after sqlite3.c so it can use
 * SQLite's private cursor API in the same translation unit. */
#include "sqlite_fast.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { BtCursor *p; } FastCursor;
typedef struct { u64 type; const u8 *data; } FastColumn;
typedef struct { const u8 *data; u32 size; } FastRecord;

typedef struct {
  sqlite3 *db;
  Btree *btree;
  Pgno posts_root, users_root, likes_root;
  KeyInfo *likes_key;
  FastCursor posts, users, likes;
  int schema_ok, native_ok, invariant_ok, active, cursors_open;
  int version_known, insert_pending;
  u32 data_version;
  i64 prior_id;
  u8 prior_created[32];
  u32 prior_created_len;
  u8 *spill;
  u32 spill_capacity;
} FastContext;

static int fast_sql(sqlite3 *db, const char *sql) {
  char *error = 0;
  int rc = sqlite3_exec(db, sql, 0, 0, &error);
  sqlite3_free(error);
  return rc;
}

static Pgno fast_root(sqlite3 *db, const char *name) {
  sqlite3_stmt *stmt = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name=?1", -1, &stmt, 0) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
  Pgno root = sqlite3_step(stmt) == SQLITE_ROW ? (Pgno)sqlite3_column_int64(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  return root;
}

static int fast_table_layout(sqlite3 *db, const char *table,
                             const char *const *names,
                             const char *const *types,
                             const int *pks, int expected) {
  char sql[96];
  sqlite3_snprintf(sizeof(sql), sql, "PRAGMA table_info(\"%w\")", table);
  sqlite3_stmt *stmt = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) return 0;
  int count = 0, ok = 1, rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *type = (const char *)sqlite3_column_text(stmt, 2);
    if (count >= expected || !name || !type || strcmp(name, names[count]) ||
        sqlite3_stricmp(type, types[count]) || sqlite3_column_int(stmt, 5) != pks[count]) {
      ok = 0;
      break;
    }
    count++;
  }
  if (rc != SQLITE_DONE) ok = 0;
  sqlite3_finalize(stmt);
  return ok && count == expected;
}

static int fast_schema_ok(sqlite3 *db) {
  static const char *const user_names[] = {"id", "username", "created_at"};
  static const char *const user_types[] = {"INTEGER", "TEXT", "TEXT"};
  static const int user_pks[] = {1, 0, 0};
  static const char *const post_names[] = {"id", "user_id", "body", "created_at"};
  static const char *const post_types[] = {"INTEGER", "INTEGER", "TEXT", "TEXT"};
  static const int post_pks[] = {1, 0, 0, 0};
  static const char *const like_names[] = {"user_id", "post_id", "created_at"};
  static const char *const like_types[] = {"INTEGER", "INTEGER", "TEXT"};
  static const int like_pks[] = {1, 2, 0};
  if (!fast_table_layout(db, "users", user_names, user_types, user_pks, 3) ||
      !fast_table_layout(db, "posts", post_names, post_types, post_pks, 4) ||
      !fast_table_layout(db, "likes", like_names, like_types, like_pks, 3)) return 0;
  sqlite3_stmt *stmt = 0;
  if (sqlite3_prepare_v2(db, "PRAGMA index_info(\"likes_post_id_idx\")", -1, &stmt, 0) != SQLITE_OK) return 0;
  int ok = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0 &&
           sqlite3_column_int(stmt, 1) == 1 &&
           !strcmp((const char *)sqlite3_column_text(stmt, 2), "post_id") &&
           sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

static int fast_cursor_open(FastContext *ctx, FastCursor *cursor, Pgno root, KeyInfo *key) {
  cursor->p = malloc((size_t)sqlite3BtreeCursorSize());
  if (!cursor->p) return SQLITE_NOMEM;
  sqlite3BtreeCursorZero(cursor->p);
  int rc = sqlite3BtreeCursor(ctx->btree, root, 0, key, cursor->p);
  if (rc != SQLITE_OK) {
    free(cursor->p);
    cursor->p = 0;
  }
  return rc;
}

static void fast_cursor_close(FastCursor *cursor) {
  if (!cursor->p) return;
  sqlite3BtreeCloseCursor(cursor->p);
  free(cursor->p);
  cursor->p = 0;
}

static void fast_cursors_close(FastContext *ctx) {
  fast_cursor_close(&ctx->posts);
  fast_cursor_close(&ctx->users);
  fast_cursor_close(&ctx->likes);
  ctx->cursors_open = 0;
}

static int fast_cursors_open(FastContext *ctx) {
  if (fast_cursor_open(ctx, &ctx->posts, ctx->posts_root, 0) != SQLITE_OK) goto error;
  if (fast_cursor_open(ctx, &ctx->users, ctx->users_root, 0) != SQLITE_OK) goto error;
  if (fast_cursor_open(ctx, &ctx->likes, ctx->likes_root, ctx->likes_key) != SQLITE_OK) goto error;
  ctx->cursors_open = 1;
  return 1;
error:
  fast_cursors_close(ctx);
  return 0;
}

static int fast_record(FastContext *ctx, FastCursor *cursor, FastRecord *record) {
  u32 total = sqlite3BtreePayloadSize(cursor->p), local = 0;
  const u8 *bytes = sqlite3BtreePayloadFetch(cursor->p, &local);
  if (bytes && local == total) {
    record->data = bytes;
    record->size = total;
    return 0;
  }
  if (total > ctx->spill_capacity) {
    u32 capacity = total + 4096;
    u8 *spill = realloc(ctx->spill, capacity);
    if (!spill) return -1;
    ctx->spill = spill;
    ctx->spill_capacity = capacity;
  }
  if (sqlite3BtreePayload(cursor->p, 0, total, ctx->spill) != SQLITE_OK) return -1;
  record->data = ctx->spill;
  record->size = total;
  return 0;
}

static const u8 *fast_varint(const u8 *p, const u8 *end, u64 *value) {
  u64 x = 0;
  for (int i = 0; i < 8; i++) {
    if (p >= end) return 0;
    u8 b = *p++;
    x = (x << 7) | (b & 0x7f);
    if (!(b & 0x80)) { *value = x; return p; }
  }
  if (p >= end) return 0;
  *value = (x << 8) | *p++;
  return p;
}

static u32 fast_serial_length(u64 type) {
  static const u8 lengths[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return type < 12 ? lengths[type] : (u32)((type - 12) / 2);
}

static int fast_columns(const FastRecord *record, int n, FastColumn *columns) {
  const u8 *end = record->data + record->size;
  u64 header_size;
  const u8 *header = fast_varint(record->data, end, &header_size);
  if (!header || header_size > record->size) return -1;
  const u8 *header_end = record->data + header_size, *body = header_end;
  for (int i = 0; i < n; i++) {
    if (header >= header_end) return -1;
    header = fast_varint(header, header_end, &columns[i].type);
    if (!header) return -1;
    columns[i].data = body;
    u32 size = fast_serial_length(columns[i].type);
    if ((size_t)(end - body) < size) return -1;
    body += size;
  }
  return body <= end ? 0 : -1;
}

static int fast_is_integer(const FastColumn *column) {
  return (column->type >= 1 && column->type <= 6) || column->type == 8 || column->type == 9;
}

static int fast_is_text(const FastColumn *column) {
  return column->type >= 13 && (column->type & 1);
}

static i64 fast_integer(const FastColumn *column) {
  const u8 *p = column->data;
  switch (column->type) {
    case 1: return (int8_t)p[0];
    case 2: return (int16_t)((u16)p[0] << 8 | p[1]);
    case 3: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8) >> 8;
    case 4: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
    case 5: {
      u64 value = 0;
      for (int i = 0; i < 6; i++) value = (value << 8) | p[i];
      return (value & (1ULL << 47)) ? (i64)(value | 0xffff000000000000ULL) : (i64)value;
    }
    case 6: {
      u64 value = 0;
      for (int i = 0; i < 8; i++) value = (value << 8) | p[i];
      return (i64)value;
    }
    case 9: return 1;
    default: return 0;
  }
}

static int fast_seek(FastCursor *cursor, i64 rowid) {
  int result = 0;
  if (sqlite3BtreeTableMoveto(cursor->p, rowid, 0, &result) != SQLITE_OK) return -1;
  return result == 0 ? 0 : 1;
}

static int fast_first(FastCursor *cursor) {
  int empty = 0;
  if (sqlite3BtreeFirst(cursor->p, &empty) != SQLITE_OK) return -1;
  return empty ? 1 : 0;
}

static int fast_last(FastCursor *cursor) {
  int empty = 0;
  if (sqlite3BtreeLast(cursor->p, &empty) != SQLITE_OK) return -1;
  return empty ? 1 : 0;
}

static int fast_next(FastCursor *cursor) {
  int rc = sqlite3BtreeNext(cursor->p, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

static int fast_previous(FastCursor *cursor) {
  int rc = sqlite3BtreePrevious(cursor->p, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

static int fast_index_ge(FastContext *ctx, i64 key) {
  Mem mem;
  memset(&mem, 0, sizeof(mem));
  mem.flags = MEM_Int;
  mem.u.i = key;
  UnpackedRecord record;
  memset(&record, 0, sizeof(record));
  record.pKeyInfo = ctx->likes_key;
  record.aMem = &mem;
  record.nField = 1;
  record.default_rc = 1;
  int result = 0;
  if (sqlite3BtreeIndexMoveto(ctx->likes.p, &record, &result) != SQLITE_OK) return -1;
  if (result < 0) return fast_next(&ctx->likes);
  return sqlite3BtreeEof(ctx->likes.p) ? 1 : 0;
}

static int fast_range_likes(FastContext *ctx, i64 low, i64 high, i64 *counts) {
  for (i64 id = low; id <= high; id++) counts[id - low] = 0;
  int rc = fast_index_ge(ctx, low);
  while (rc == 0) {
    FastRecord record;
    FastColumn column[1];
    if (fast_record(ctx, &ctx->likes, &record) || fast_columns(&record, 1, column) || !fast_is_integer(&column[0])) return -1;
    i64 post_id = fast_integer(&column[0]);
    if (post_id > high) return 0;
    if (post_id >= low) counts[post_id - low]++;
    rc = fast_next(&ctx->likes);
  }
  return rc < 0 ? -1 : 0;
}

static int fast_user_name(FastContext *ctx, i64 user_id, const u8 **name, u32 *length) {
  int rc = fast_seek(&ctx->users, user_id);
  if (rc != 0) return rc;
  FastRecord record;
  FastColumn columns[3];
  if (fast_record(ctx, &ctx->users, &record) || fast_columns(&record, 3, columns) || !fast_is_text(&columns[1])) return -1;
  *name = columns[1].data;
  *length = fast_serial_length(columns[1].type);
  return 0;
}

static int fast_copy(const FastColumn *column, u8 *destination, u32 capacity, u32 *length) {
  if (!fast_is_text(column)) return -1;
  u32 size = fast_serial_length(column->type);
  if (size > capacity) return -1;
  memcpy(destination, column->data, size);
  *length = size;
  return 0;
}

static int fast_fill_row(FastContext *ctx, FastCursor *cursor, i64 id,
                         i64 likes, TwelveFastRow *row) {
  FastRecord record;
  FastColumn columns[4];
  if (fast_record(ctx, cursor, &record) || fast_columns(&record, 4, columns) ||
      !fast_is_integer(&columns[1]) || !fast_is_text(&columns[2]) || !fast_is_text(&columns[3])) return -1;
  const u8 *author = 0;
  u32 author_len = 0;
  if (fast_user_name(ctx, fast_integer(&columns[1]), &author, &author_len) != 0) return -1;
  memset(row, 0, sizeof(*row));
  row->id = id;
  row->like_count = likes;
  if (fast_copy(&columns[2], row->body, sizeof(row->body), &row->body_len) ||
      fast_copy(&columns[3], row->created_at, sizeof(row->created_at), &row->created_at_len) ||
      author_len > sizeof(row->author)) return -1;
  memcpy(row->author, author, author_len);
  row->author_len = author_len;
  return 0;
}

static int fast_text_le(const u8 *a, u32 an, const u8 *b, u32 bn) {
  u32 n = an < bn ? an : bn;
  int cmp = memcmp(a, b, n);
  return cmp < 0 || (cmp == 0 && an <= bn);
}

static int fast_check_invariant(FastContext *ctx) {
  int ok = 1, rc = fast_first(&ctx->posts);
  i64 expected = 1;
  u8 previous[32];
  u32 previous_len = 0;
  int have_previous = 0;
  while (ok && rc == 0) {
    FastRecord record;
    FastColumn columns[4];
    if (sqlite3BtreeIntegerKey(ctx->posts.p) != expected ||
        fast_record(ctx, &ctx->posts, &record) || fast_columns(&record, 4, columns) ||
        !fast_is_integer(&columns[1]) || !fast_is_text(&columns[3])) { ok = 0; break; }
    u32 timestamp_len = fast_serial_length(columns[3].type);
    if (timestamp_len > sizeof(previous) ||
        (have_previous && !fast_text_le(previous, previous_len, columns[3].data, timestamp_len)) ||
        fast_seek(&ctx->users, fast_integer(&columns[1])) != 0) { ok = 0; break; }
    memcpy(previous, columns[3].data, timestamp_len);
    previous_len = timestamp_len;
    have_previous = 1;
    expected++;
    rc = fast_next(&ctx->posts);
  }
  if (rc < 0) ok = 0;
  return ok;
}

void *twelve_fast_open(void *sqlite_db) {
  sqlite3 *db = (sqlite3 *)sqlite_db;
  FastContext *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return 0;
  ctx->db = db;
  ctx->btree = db->aDb[0].pBt;
  if (!fast_schema_ok(db)) return ctx;
  ctx->posts_root = fast_root(db, "posts");
  ctx->users_root = fast_root(db, "users");
  ctx->likes_root = fast_root(db, "likes_post_id_idx");
  if (!ctx->posts_root || !ctx->users_root || !ctx->likes_root) return ctx;
  ctx->likes_key = sqlite3KeyInfoAlloc(db, 1, 1);
  if (!ctx->likes_key) return ctx;
  for (int i = 0; i < 2; i++) ctx->likes_key->aColl[i] = db->pDfltColl;
  if (fast_sql(db, "BEGIN") != SQLITE_OK || sqlite3BtreeBeginTrans(ctx->btree, 0, 0) != SQLITE_OK) {
    fast_sql(db, "ROLLBACK");
    return ctx;
  }
  u32 version = 0;
  sqlite3BtreeGetMeta(ctx->btree, BTREE_DATA_VERSION, &version);
  ctx->data_version = version;
  ctx->version_known = 1;
  if (fast_cursors_open(ctx)) {
    ctx->invariant_ok = fast_check_invariant(ctx);
    fast_cursors_close(ctx);
    ctx->native_ok = 1;
  }
  if (fast_sql(db, "COMMIT") != SQLITE_OK) {
    fast_sql(db, "ROLLBACK");
    ctx->native_ok = 0;
    ctx->invariant_ok = 0;
  }
  fprintf(stderr, "SQLite B-tree reads: %s; feed invariant: %s\n",
          ctx->native_ok ? "enabled" : "disabled",
          ctx->invariant_ok ? "holds" : "does not hold (SQL feed fallback)");
  return ctx;
}

void twelve_fast_close(void *opaque) {
  FastContext *ctx = (FastContext *)opaque;
  if (!ctx) return;
  fast_cursors_close(ctx);
  if (ctx->likes_key) sqlite3KeyInfoUnref(ctx->likes_key);
  free(ctx->spill);
  free(ctx);
}

int twelve_fast_batch_begin(void *opaque) {
  FastContext *ctx = (FastContext *)opaque;
  if (!ctx || !ctx->native_ok || sqlite3BtreeBeginTrans(ctx->btree, 0, 0) != SQLITE_OK) return 0;
  u32 version = 0;
  sqlite3BtreeGetMeta(ctx->btree, BTREE_DATA_VERSION, &version);
  if (ctx->version_known && version != ctx->data_version) {
    ctx->native_ok = 0;
    ctx->invariant_ok = 0;
    fprintf(stderr, "external SQLite write detected; B-tree plans disabled\n");
    return 0;
  }
  ctx->data_version = version;
  ctx->version_known = 1;
  ctx->active = 1;
  return 1;
}

void twelve_fast_batch_end(void *opaque) {
  FastContext *ctx = (FastContext *)opaque;
  if (!ctx) return;
  fast_cursors_close(ctx);
}

void twelve_fast_prepare_write(void *opaque) {
  FastContext *ctx = (FastContext *)opaque;
  if (!ctx) return;
  fast_cursors_close(ctx);
  ctx->active = 0;
}

static int fast_feed(FastContext *ctx, TwelveFastRow *rows, int capacity, int *count) {
  if (!ctx->invariant_ok) return -2;
  int rc = fast_last(&ctx->posts);
  if (rc < 0) return -1;
  if (rc == 1) { *count = 0; return 0; }
  i64 high = sqlite3BtreeIntegerKey(ctx->posts.p);
  i64 low = high > 20 ? high - 19 : 1;
  if (high < low || high - low >= capacity || high - low >= 20) return -1;
  i64 likes[20];
  if (fast_range_likes(ctx, low, high, likes)) return -1;
  int n = 0;
  for (i64 id = high; id >= low; id--) {
    if (sqlite3BtreeIntegerKey(ctx->posts.p) != id || fast_fill_row(ctx, &ctx->posts, id, likes[id - low], &rows[n])) {
      ctx->invariant_ok = 0;
      return -1;
    }
    n++;
    if (id != low && fast_previous(&ctx->posts) != 0) { ctx->invariant_ok = 0; return -1; }
  }
  *count = n;
  return 0;
}

int twelve_fast_read(void *opaque, int is_feed, int64_t post_id,
                     TwelveFastRow *rows, int capacity, int *count) {
  FastContext *ctx = (FastContext *)opaque;
  *count = 0;
  if (!ctx || !ctx->active || !ctx->native_ok) return -2;
  (void)post_id;
  if (!is_feed) return -2;
  if (!ctx->cursors_open && !fast_cursors_open(ctx)) return -1;
  return fast_feed(ctx, rows, capacity, count);
}

int twelve_fast_before_insert(void *opaque) {
  FastContext *ctx = (FastContext *)opaque;
  if (!ctx || !ctx->active || !ctx->invariant_ok) return 0;
  fast_cursors_close(ctx);
  FastCursor posts = {0};
  if (fast_cursor_open(ctx, &posts, ctx->posts_root, 0) != SQLITE_OK) { ctx->invariant_ok = 0; return 0; }
  int rc = fast_last(&posts);
  if (rc < 0) { fast_cursor_close(&posts); ctx->invariant_ok = 0; return 0; }
  ctx->prior_id = rc == 1 ? 0 : sqlite3BtreeIntegerKey(posts.p);
  ctx->prior_created_len = 0;
  if (rc == 0) {
    FastRecord record;
    FastColumn columns[4];
    if (fast_record(ctx, &posts, &record) || fast_columns(&record, 4, columns) || !fast_is_text(&columns[3])) {
      fast_cursor_close(&posts);
      ctx->invariant_ok = 0;
      return 0;
    }
    ctx->prior_created_len = fast_serial_length(columns[3].type);
    if (ctx->prior_created_len > sizeof(ctx->prior_created)) {
      fast_cursor_close(&posts);
      ctx->invariant_ok = 0;
      return 0;
    }
    memcpy(ctx->prior_created, columns[3].data, ctx->prior_created_len);
  }
  fast_cursor_close(&posts);
  ctx->insert_pending = 1;
  return 1;
}

void twelve_fast_cancel_insert(void *opaque) {
  FastContext *ctx = (FastContext *)opaque;
  if (ctx) ctx->insert_pending = 0;
}

void twelve_fast_after_insert(void *opaque, int64_t id, int64_t user_id,
                              const uint8_t *created_at, uint32_t length) {
  FastContext *ctx = (FastContext *)opaque;
  if (!ctx || !ctx->insert_pending) return;
  ctx->insert_pending = 0;
  if (id != ctx->prior_id + 1 || length > sizeof(ctx->prior_created) ||
      (ctx->prior_created_len && !fast_text_le(ctx->prior_created, ctx->prior_created_len, created_at, length))) {
    ctx->invariant_ok = 0;
    return;
  }
  FastCursor users = {0};
  if (fast_cursor_open(ctx, &users, ctx->users_root, 0) != SQLITE_OK || fast_seek(&users, user_id) != 0) {
    fast_cursor_close(&users);
    ctx->invariant_ok = 0;
    return;
  }
  fast_cursor_close(&users);
}

void twelve_fast_rollback(void *opaque) {
  FastContext *ctx = (FastContext *)opaque;
  if (!ctx) return;
  ctx->invariant_ok = 0;
  ctx->insert_pending = 0;
}
