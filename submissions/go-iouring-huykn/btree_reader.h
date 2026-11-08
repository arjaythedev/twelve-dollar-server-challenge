// Included after the pinned SQLite amalgamation. This module uses SQLite's
// read-only cursor operations; it never modifies pager, page or cursor state
// directly. Its request-local join plan is separate from the SQL write adapter.
#include "read_plan.h"

enum { PLAN_POSTS, PLAN_USERS, PLAN_FEED, PLAN_LIKES, PLAN_CURSOR_COUNT };
struct hkn_read_plan {
  sqlite3 *connection;
  Btree *tree;
  Pgno roots[PLAN_CURSOR_COUNT];
  Pgno like_table_root;
  KeyInfo *keys[PLAN_CURSOR_COUNT];
  BtCursor cursors[PLAN_CURSOR_COUNT];
  int opened, cookie;
  unsigned char overflow[65536];
};

typedef struct { i64 key; int slot; } lookup_task;
typedef struct { const u8 *bytes; u32 size, serial; } cell_view;

static int named_columns(Table *table, const char *const *names, int count) {
  if (!table || !HasRowid(table) || table->nCol != count || table->iPKey != 0 || (table->tabFlags & TF_HasGenerated)) return 0;
  for (int i = 0; i < count; ++i) {
    if (strcmp(table->aCol[i].zCnName, names[i]) != 0) return 0;
  }
  return 1;
}

static hkn_read_plan *create_read_plan(sqlite3 *connection) {
  if (ENC(connection) != SQLITE_UTF8) return NULL;
  const char *const post_columns[] = {"id", "user_id", "body", "created_at"};
  const char *const user_columns[] = {"id", "username", "created_at"};
  Table *posts = sqlite3FindTable(connection, "posts", "main");
  Table *users = sqlite3FindTable(connection, "users", "main");
  Index *feed = sqlite3FindIndex(connection, "posts_created_at_id_idx", "main");
  Index *likes = sqlite3FindIndex(connection, "likes_post_id_idx", "main");
  if (!named_columns(posts, post_columns, 4) || !named_columns(users, user_columns, 3)) return NULL;
  if (!feed || feed->pTable != posts || feed->nKeyCol != 2 ||
      feed->aiColumn[0] != 3 || feed->aiColumn[1] != 0 ||
      !feed->aSortOrder[0] || !feed->aSortOrder[1] || feed->pPartIdxWhere) return NULL;
  if (!likes || likes->nKeyCol != 1 || likes->aiColumn[0] != 1 ||
      likes->aSortOrder[0] || likes->pPartIdxWhere ||
      strcmp(likes->pTable->zName, "likes") != 0) return NULL;
  // A binary ascending/descending plan is invalid for custom collations.
  if (strcmp(feed->azColl[0], "BINARY") || strcmp(feed->azColl[1], "BINARY") ||
      strcmp(likes->azColl[0], "BINARY")) return NULL;
  hkn_read_plan *plan = sqlite3_malloc64(sizeof *plan);
  if (!plan) return NULL; // The SQL path can still report any real allocation failure.
  memset(plan, 0, sizeof *plan);
  plan->connection = connection;
  plan->tree = connection->aDb[0].pBt;
  plan->cookie = connection->aDb[0].pSchema->schema_cookie;
  plan->roots[PLAN_POSTS] = posts->tnum;
  plan->roots[PLAN_USERS] = users->tnum;
  plan->roots[PLAN_FEED] = feed->tnum;
  plan->roots[PLAN_LIKES] = likes->tnum;
  plan->like_table_root = likes->pTable->tnum;
  Parse parser = {0};
  parser.db = connection;
  plan->keys[PLAN_FEED] = sqlite3KeyInfoOfIndex(&parser, feed);
  plan->keys[PLAN_LIKES] = sqlite3KeyInfoOfIndex(&parser, likes);
  if (!plan->keys[PLAN_FEED] || !plan->keys[PLAN_LIKES]) {
    hkn_plan_release(plan);
    return NULL;
  }
  return plan;
}

hkn_read_plan *hkn_plan_new(sqlite3 *connection) {
  sqlite3_mutex_enter(connection->mutex);
  sqlite3BtreeEnterAll(connection);
  hkn_read_plan *plan = create_read_plan(connection);
  sqlite3BtreeLeaveAll(connection);
  sqlite3_mutex_leave(connection->mutex);
  return plan;
}

static void end_plan_locked(hkn_read_plan *plan) {
  while (plan->opened) sqlite3BtreeCloseCursor(&plan->cursors[--plan->opened]);
}

void hkn_plan_end(hkn_read_plan *plan) {
  if (!plan) return;
  sqlite3_mutex_enter(plan->connection->mutex);
  sqlite3BtreeEnter(plan->tree);
  end_plan_locked(plan);
  sqlite3BtreeLeave(plan->tree);
  sqlite3_mutex_leave(plan->connection->mutex);
}

void hkn_plan_release(hkn_read_plan *plan) {
  if (!plan) return;
  sqlite3_mutex_enter(plan->connection->mutex);
  sqlite3BtreeEnter(plan->tree);
  end_plan_locked(plan);
  for (int i = 0; i < PLAN_CURSOR_COUNT; ++i) sqlite3KeyInfoUnref(plan->keys[i]);
  sqlite3BtreeLeave(plan->tree);
  sqlite3_mutex_leave(plan->connection->mutex);
  sqlite3_free(plan);
}

static int plan_begin(hkn_read_plan *plan) {
  if (plan->opened) return SQLITE_OK;
  int code = sqlite3BtreeBeginTrans(plan->tree, 0, NULL);
  if (code != SQLITE_OK) return code;
  u32 cookie;
  sqlite3BtreeGetMeta(plan->tree, BTREE_SCHEMA_VERSION, &cookie);
  if (cookie != (u32)plan->cookie) return SQLITE_NOTFOUND;
  // Index cursors require the read lock of their owning table, as SQLite's
  // VDBE does. These are no-ops with the normal private-cache connection.
  code = sqlite3BtreeLockTable(plan->tree, plan->roots[PLAN_POSTS], 0);
  if (code == SQLITE_OK) code = sqlite3BtreeLockTable(plan->tree, plan->roots[PLAN_USERS], 0);
  if (code == SQLITE_OK) code = sqlite3BtreeLockTable(plan->tree, plan->like_table_root, 0);
  if (code != SQLITE_OK) return code;
  for (int i = 0; i < PLAN_CURSOR_COUNT; ++i) {
    sqlite3BtreeCursorZero(&plan->cursors[i]);
    code = sqlite3BtreeCursor(plan->tree, plan->roots[i], 0, plan->keys[i], &plan->cursors[i]);
    if (code != SQLITE_OK) { hkn_plan_end(plan); return code; }
    ++plan->opened;
  }
  return SQLITE_OK;
}

// Consume a SQLite varint without reading past the supplied header boundary.
static int bounded_varint(const u8 *bytes, u32 limit, u32 *at, u64 *value) {
  u64 n = 0;
  for (int group = 0; group < 9 && *at < limit; ++group) {
    u8 byte = bytes[(*at)++];
    if (group == 8) { *value = (n << 8) | byte; return 1; }
    n = (n << 7) | (byte & 127);
    if (!(byte & 128)) { *value = n; return 1; }
  }
  return 0;
}

static int decode_prefix(const u8 *record, u32 size, cell_view *fields, int count) {
  u32 header_at = 0;
  u64 header;
  if (!bounded_varint(record, size, &header_at, &header) || header > size || header < header_at) return SQLITE_NOTFOUND;
  u32 data_at = (u32)header;
  for (int i = 0; i < count; ++i) {
    u64 serial;
    if (!bounded_varint(record, (u32)header, &header_at, &serial) || serial > UINT32_MAX ||
        serial == 10 || serial == 11) return SQLITE_NOTFOUND;
    u32 width = sqlite3VdbeSerialTypeLen((u32)serial);
    if (width > size - data_at) return SQLITE_NOTFOUND;
    fields[i] = (cell_view){record + data_at, width, (u32)serial};
    data_at += width;
  }
  return SQLITE_OK;
}

static int project_record(hkn_read_plan *plan, int cursor, cell_view *fields, int count) {
  BtCursor *source = &plan->cursors[cursor];
  // PayloadSize initializes the current cell metadata required by PayloadFetch.
  u32 size = sqlite3BtreePayloadSize(source), available = 0;
  const u8 *record = sqlite3BtreePayloadFetch(source, &available);
  int code = decode_prefix(record, available, fields, count);
  if (code == SQLITE_OK) return code;
  // Only fetch overflow pages if a requested field is not entirely local.
  if (size <= available || size > sizeof plan->overflow) return SQLITE_NOTFOUND;
  code = sqlite3BtreePayload(source, 0, size, plan->overflow);
  return code == SQLITE_OK ? decode_prefix(plan->overflow, size, fields, count) : code;
}

static int integer_cell(const cell_view *cell, i64 *value) {
  if (cell->serial == 8 || cell->serial == 9) {
    *value = cell->serial - 8;
    return 1;
  }
  if (cell->serial < 1 || cell->serial > 6) return 0;
  // Only an integer is needed here, not SQLite's general-purpose Mem value.
  // Unsigned shifts preserve sign extension without shifting a negative value.
  u64 bits = UINT64_C(0) - (cell->bytes[0] >> 7);
  for (u32 i = 0; i < cell->size; ++i) bits = (bits << 8) | cell->bytes[i];
  memcpy(value, &bits, sizeof bits);
  return 1;
}

static int arena_text(hkn_result *result, const cell_view *value, hkn_span *span) {
  if (value->serial < 13 || !(value->serial & 1)) return SQLITE_NOTFOUND;
  if (value->size > sizeof result->bytes - result->used) return SQLITE_TOOBIG;
  span->at = result->used;
  span->size = value->size;
  memcpy(result->bytes + result->used, value->bytes, value->size);
  result->used += value->size;
  return SQLITE_OK;
}

static void order_tasks(lookup_task *tasks, int count) {
  for (int i = 1; i < count; ++i) {
    lookup_task value = tasks[i];
    int at = i;
    while (at && tasks[at-1].key > value.key) { tasks[at] = tasks[at-1]; --at; }
    tasks[at] = value;
  }
}

static int table_row(hkn_read_plan *plan, int cursor, i64 id) {
  int relation;
  int code = sqlite3BtreeTableMoveto(&plan->cursors[cursor], id, 0, &relation);
  if (code != SQLITE_OK) return code;
  return relation == 0 ? SQLITE_OK : SQLITE_NOTFOUND;
}

static int select_keys(hkn_read_plan *plan, hkn_result *result, int single, i64 key,
                       int *descending) {
  if (single) { result->length = 1; result->items[0].key = key; return SQLITE_OK; }
  int empty;
  BtCursor *index = &plan->cursors[PLAN_FEED];
  int code = sqlite3BtreeFirst(index, &empty);
  if (code != SQLITE_OK || empty) return code;
  while (result->length < 20) {
    cell_view entry[2];
    code = project_record(plan, PLAN_FEED, entry, 2);
    if (code != SQLITE_OK) return code;
    hkn_record *row = &result->items[result->length];
    i64 selected;
    if (!integer_cell(&entry[1], &selected)) return SQLITE_NOTFOUND;
    if (result->length && selected > result->items[result->length-1].key) *descending = 0;
    row->key = selected;
    code = arena_text(result, &entry[0], &row->timestamp);
    if (code != SQLITE_OK) return code;
    ++result->length;
    if (result->length == 20) break;
    code = sqlite3BtreeNext(index, 0);
    if (code == SQLITE_DONE) return SQLITE_OK;
    if (code != SQLITE_OK) return code;
  }
  return SQLITE_OK;
}

// Resolve posts in rowid order, preserving output order through task slots.
// Sorting does not assume that timestamps increase with post IDs.
static int load_posts(hkn_read_plan *plan, hkn_result *result,
                      lookup_task *posts, lookup_task *authors, int single,
                      int descending) {
  if (!descending) {
    for (int i = 0; i < result->length; ++i) {
      int slot = result->length - i - 1;
      posts[i] = (lookup_task){result->items[slot].key, slot};
    }
    order_tasks(posts, result->length);
  }
  for (int i = 0; i < result->length; ++i) {
    int slot = descending ? result->length - i - 1 : posts[i].slot;
    if (descending) posts[i] = (lookup_task){result->items[slot].key, slot};
    int code = table_row(plan, PLAN_POSTS, posts[i].key);
    if (code != SQLITE_OK) return code;
    cell_view fields[4];
    code = project_record(plan, PLAN_POSTS, fields, single ? 4 : 3);
    if (code != SQLITE_OK) return code;
    authors[i].slot = slot;
    if (!integer_cell(&fields[1], &authors[i].key)) return SQLITE_NOTFOUND;
    hkn_record *row = &result->items[slot];
    code = arena_text(result, &fields[2], &row->content);
    if (code == SQLITE_OK && single) code = arena_text(result, &fields[3], &row->timestamp);
    if (code != SQLITE_OK) return code;
    row->count = 0;
  }
  return SQLITE_OK;
}

// Adjacent equal authors share one lookup and copied span within this request.
// A run needs only its previous key/span, rather than a per-row hash probe.
static int join_authors(hkn_read_plan *plan, hkn_result *result, lookup_task *authors) {
  i64 previous = 0;
  hkn_span name = {0};
  for (int i = 0; i < result->length; ++i) {
    i64 key = authors[i].key;
    if (!i || key != previous) {
      int code = table_row(plan, PLAN_USERS, key);
      if (code != SQLITE_OK) return code;
      cell_view fields[2];
      code = project_record(plan, PLAN_USERS, fields, 2);
      if (code == SQLITE_OK) code = arena_text(result, &fields[1], &name);
      if (code != SQLITE_OK) return code;
      previous = key;
    }
    result->items[authors[i].slot].username = name;
  }
  return SQLITE_OK;
}

static int like_seek(hkn_read_plan *plan, i64 post) {
  Mem value = {0};
  value.flags = MEM_Int;
  value.u.i = post;
  UnpackedRecord target = {0};
  target.pKeyInfo = plan->keys[PLAN_LIKES];
  target.aMem = &value;
  target.nField = 1;
  target.default_rc = 1; // Same lower-bound comparison as SQLite's OP_SeekGE.
  int relation;
  int code = sqlite3BtreeIndexMoveto(&plan->cursors[PLAN_LIKES], &target, &relation);
  if (code != SQLITE_OK) return code;
  if (sqlite3BtreeEof(&plan->cursors[PLAN_LIKES])) return SQLITE_DONE;
  return relation < 0 ? sqlite3BtreeNext(&plan->cursors[PLAN_LIKES], 0) : SQLITE_OK;
}

static inline __attribute__((always_inline)) int read_count_key(hkn_read_plan *plan, i64 *key) {
  // This index starts with an integer and contains only its rowid after that.
  // Project that integer directly instead of running the generic field loop
  // for every like. Unusual headers/types retain the reference SQL fallback.
  BtCursor *cursor = &plan->cursors[PLAN_LIKES];
  (void)sqlite3BtreePayloadSize(cursor); // Refresh cell metadata before Fetch.
  u32 available;
  const u8 *record = sqlite3BtreePayloadFetch(cursor, &available);
  if (available < 2 || record[0] < 2 || record[0] >= 128 || record[0] > available) return SQLITE_NOTFOUND;
  u32 serial = record[1];
  if (serial < 1 || serial == 7 || serial > 9) return SQLITE_NOTFOUND;
  u32 width = sqlite3VdbeSerialTypeLen(serial);
  if (width > available - record[0]) return SQLITE_NOTFOUND;
  cell_view field = {record + record[0], width, serial};
  return integer_cell(&field, key) ? SQLITE_OK : SQLITE_NOTFOUND;
}

// Merge requested IDs with index runs. A decoded key stays live across posts;
// matching likes take only the run loop, while gaps use a lower-bound seek.
static int join_counts(hkn_read_plan *plan, hkn_result *result, lookup_task *posts) {
  if (!result->length) return SQLITE_OK;
  int code = like_seek(plan, posts[0].key);
  i64 current = 0;
  if (code == SQLITE_OK) code = read_count_key(plan, &current);
  if (code != SQLITE_OK) return code == SQLITE_DONE ? SQLITE_OK : code;
  for (int i = 0; i < result->length; ++i) {
    i64 wanted = posts[i].key;
    if (current < wanted) {
      code = like_seek(plan, wanted);
      if (code == SQLITE_OK) code = read_count_key(plan, &current);
      if (code != SQLITE_OK) return code == SQLITE_DONE ? SQLITE_OK : code;
    }
    i64 count = 0;
    while (current == wanted) {
      ++count;
      code = sqlite3BtreeNext(&plan->cursors[PLAN_LIKES], 0);
      if (code == SQLITE_OK) code = read_count_key(plan, &current);
      if (code != SQLITE_OK) break;
    }
    result->items[posts[i].slot].count = count;
    if (code != SQLITE_OK) return code == SQLITE_DONE ? SQLITE_OK : code;
  }
  return SQLITE_OK;
}

static int read_plan_locked(hkn_read_plan *plan, int single, int64_t key, hkn_result *result) {
  if (!plan) return SQLITE_NOTFOUND;
  result->length = 0;
  result->used = 0;
  int code = plan_begin(plan);
  int descending = 1;
  if (code == SQLITE_OK) code = select_keys(plan, result, single, key, &descending);
  lookup_task posts[20], authors[20];
  if (code == SQLITE_OK) code = load_posts(plan, result, posts, authors, single, descending);
  if (code == SQLITE_OK) code = join_authors(plan, result, authors);
  if (code == SQLITE_OK) code = join_counts(plan, result, posts);
  return code;
}

int hkn_plan_read(hkn_read_plan *plan, int single, int64_t key, hkn_result *result) {
  if (!plan) return SQLITE_NOTFOUND;
  sqlite3_mutex_enter(plan->connection->mutex);
  sqlite3BtreeEnter(plan->tree);
  int code = read_plan_locked(plan, single, key, result);
  sqlite3BtreeLeave(plan->tree);
  sqlite3_mutex_leave(plan->connection->mutex);
  return code;
}
