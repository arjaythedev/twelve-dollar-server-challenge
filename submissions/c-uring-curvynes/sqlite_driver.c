/* Unmodified, pinned SQLite plus a read-only C binding to its B-tree layer.
   Inspired by the authorized lean-derive-hargup read-plan design. This driver
   keeps the real feed-order SQL and does not assume dense or monotonic rowids. */
#include "sqlite3.c"
#include "native_db.h"

_Static_assert(SQLITE_VERSION_NUMBER == 3054001, "Review native driver for new SQLite versions");

struct NativePlan {
    sqlite3 *connection;
    Pgno posts, users, likes;
    u32 schema_cookie;
    KeyInfo *like_key;
};
typedef struct { BtCursor cursor; int open; } CvCursor;
typedef struct { const u8 *bytes; u32 length; u8 *spill; u32 capacity; } CvRecord;
typedef struct { const u8 *bytes; u32 length, serial; } CvColumn;

static int cv_cursor_open(Btree *tree, Pgno root, KeyInfo *key, CvCursor *cursor) {
    memset(cursor, 0, sizeof(*cursor));
    sqlite3BtreeCursorZero(&cursor->cursor);
    int rc = sqlite3BtreeCursor(tree, root, 0, key, &cursor->cursor);
    cursor->open = rc == SQLITE_OK;
    return rc;
}
static void cv_cursor_close(CvCursor *cursor) {
    if (cursor->open) sqlite3BtreeCloseCursor(&cursor->cursor);
}
static int cv_record(CvCursor *cursor, CvRecord *record) {
    u32 size = sqlite3BtreePayloadSize(&cursor->cursor), local = 0;
    const u8 *bytes = sqlite3BtreePayloadFetch(&cursor->cursor, &local);
    record->length = size;
    if (local >= size) { record->bytes = bytes; return SQLITE_OK; }
    if (size > record->capacity) {
        u8 *memory = sqlite3_realloc64(record->spill, (sqlite3_uint64)size + 9);
        if (!memory) return SQLITE_NOMEM;
        record->spill = memory; record->capacity = size;
    }
    int rc = sqlite3BtreePayload(&cursor->cursor, 0, size, record->spill);
    record->bytes = record->spill;
    return rc;
}
static int cv_varint(const u8 **at, const u8 *end, u64 *value) {
    u64 result = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (*at == end) return SQLITE_CORRUPT;
        unsigned byte = *(*at)++;
        result = (result << 7) | (byte & 127U);
        if (!(byte & 128U)) { *value = result; return SQLITE_OK; }
    }
    if (*at == end) return SQLITE_CORRUPT;
    *value = (result << 8) | *(*at)++;
    return SQLITE_OK;
}
static int cv_columns(const CvRecord *record, unsigned count, CvColumn *columns) {
    const u8 *at = record->bytes, *end = at + record->length;
    u64 header;
    int rc = cv_varint(&at, end, &header);
    if (rc || header > record->length || header < (u64)(at - record->bytes)) return SQLITE_CORRUPT;
    const u8 *header_end = record->bytes + header, *body = header_end;
    for (unsigned i = 0; i < count; i++) {
        u64 serial;
        rc = cv_varint(&at, header_end, &serial);
        if (rc || serial > UINT32_MAX || serial == 10 || serial == 11) return SQLITE_CORRUPT;
        u32 size = sqlite3VdbeSerialTypeLen((u32)serial);
        if (size > (u64)(end - body)) return SQLITE_CORRUPT;
        columns[i] = (CvColumn){body, size, (u32)serial}; body += size;
    }
    return SQLITE_OK;
}
static int cv_integer(const CvColumn *column, i64 *result) {
    if (!((column->serial >= 1 && column->serial <= 6) || column->serial == 8 || column->serial == 9)) return SQLITE_MISMATCH;
    Mem value; memset(&value, 0, sizeof(value));
    sqlite3VdbeSerialGet(column->bytes, column->serial, &value);
    *result = value.u.i; return SQLITE_OK;
}
static int cv_text(const CvColumn *column) { return column->serial >= 13 && (column->serial & 1); }
static int cv_seek(CvCursor *cursor, i64 id) {
    int comparison = 0;
    int rc = sqlite3BtreeTableMoveto(&cursor->cursor, id, 0, &comparison);
    return rc ? rc : comparison ? SQLITE_NOTFOUND : SQLITE_OK;
}
static int cv_like_start(CvCursor *cursor, KeyInfo *key, i64 id) {
    Mem value; memset(&value, 0, sizeof(value)); value.flags = MEM_Int; value.u.i = id;
    UnpackedRecord search; memset(&search, 0, sizeof(search));
    search.pKeyInfo = key; search.aMem = &value; search.nField = 1; search.default_rc = 1;
    int comparison = 0;
    int rc = sqlite3BtreeIndexMoveto(&cursor->cursor, &search, &comparison);
    if (rc) return rc;
    if (comparison < 0) return sqlite3BtreeNext(&cursor->cursor, 0);
    return sqlite3BtreeEof(&cursor->cursor) ? SQLITE_DONE : SQLITE_OK;
}

NativePlan *native_plan_open(sqlite3 *connection) {
    if (ENC(connection) != SQLITE_UTF8) return NULL;
    NativePlan *plan = sqlite3_malloc64(sizeof(*plan));
    if (!plan) return NULL;
    memset(plan, 0, sizeof(*plan)); plan->connection = connection;
    sqlite3_stmt *statement = NULL;
    const char *sql = "SELECT name,rootpage FROM sqlite_schema WHERE name IN ('posts','users','likes_post_id_idx')";
    if (sqlite3_prepare_v2(connection, sql, -1, &statement, NULL) != SQLITE_OK) goto failed;
    int rc;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(statement, 0);
        i64 root = sqlite3_column_int64(statement, 1);
        if (!name || root < 1 || root > UINT32_MAX) goto failed;
        if (!strcmp(name, "posts")) plan->posts = (Pgno)root;
        else if (!strcmp(name, "users")) plan->users = (Pgno)root;
        else plan->likes = (Pgno)root;
    }
    if (rc != SQLITE_DONE || !plan->posts || !plan->users || !plan->likes) goto failed;
    sqlite3_finalize(statement); statement = NULL;
    plan->schema_cookie = (u32)connection->aDb[0].pSchema->schema_cookie;
    plan->like_key = sqlite3KeyInfoAlloc(connection, 1, 1);
    if (!plan->like_key) goto failed;
    plan->like_key->aColl[0] = connection->pDfltColl;
    plan->like_key->aColl[1] = connection->pDfltColl;
    return plan;
failed:
    sqlite3_finalize(statement); native_plan_close(plan); return NULL;
}
void native_plan_close(NativePlan *plan) {
    if (plan) { sqlite3KeyInfoUnref(plan->like_key); sqlite3_free(plan); }
}
int native_read_posts(NativePlan *plan, const sqlite3_int64 *ids, unsigned count,
                      NativePostSink sink, void *context) {
    if (!plan || !count || count > 20) return SQLITE_MISUSE;
    sqlite3 *connection = plan->connection;
    Btree *tree = connection->aDb[0].pBt;
    if (sqlite3_get_autocommit(connection) || tree->inTrans == TRANS_NONE) return SQLITE_MISUSE;
    sqlite3_mutex_enter(connection->mutex); sqlite3BtreeEnter(tree);
    u32 cookie = 0; sqlite3BtreeGetMeta(tree, BTREE_SCHEMA_VERSION, &cookie);
    int rc = SQLITE_SCHEMA;
    CvCursor posts = {0}, users = {0}, likes = {0};
    CvRecord post_record = {0}, user_record = {0}, like_record = {0};
    if (cookie != plan->schema_cookie) goto done;
    i64 low = ids[0], high = ids[0];
    for (unsigned i = 1; i < count; i++) { if (ids[i] < low) low = ids[i]; if (ids[i] > high) high = ids[i]; }
    if (low < 1 || high - low > 64) { rc = SQLITE_MISMATCH; goto done; }
    rc = cv_cursor_open(tree, plan->posts, NULL, &posts); if (rc) goto done;
    rc = cv_cursor_open(tree, plan->users, NULL, &users); if (rc) goto done;
    rc = cv_cursor_open(tree, plan->likes, plan->like_key, &likes); if (rc) goto done;
    uint64_t counts[65] = {0};
    rc = cv_like_start(&likes, plan->like_key, low);
    while (rc == SQLITE_OK) {
        CvColumn field;
        rc = cv_record(&likes, &like_record); if (rc) goto done;
        rc = cv_columns(&like_record, 1, &field); if (rc) goto done;
        i64 id; rc = cv_integer(&field, &id); if (rc) goto done;
        if (id > high) break;
        if (id < low) { rc = SQLITE_CORRUPT; goto done; }
        counts[id - low]++;
        rc = sqlite3BtreeNext(&likes.cursor, 0);
    }
    if (rc != SQLITE_OK && rc != SQLITE_DONE) goto done;
    /* The caller already obtained the true timestamp/id feed order. When its
       requested ids happen to be monotonic, walk the table cursor rather than
       repeating a root-to-leaf seek for every row. Check this per request;
       arbitrary order continues to use independent seeks. */
    int direction = count > 1 && ids[1] < ids[0] ? -1 : 1;
    int ordered = 1;
    for (unsigned i = 1; i < count; i++)
        if (direction < 0 ? ids[i] >= ids[i - 1] : ids[i] <= ids[i - 1]) ordered = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!i || !ordered) { rc = cv_seek(&posts, ids[i]); if (rc) goto done; }
        else {
            i64 rowid;
            do {
                rc = direction < 0 ? sqlite3BtreePrevious(&posts.cursor, 0) : sqlite3BtreeNext(&posts.cursor, 0);
                if (rc) goto done;
                rowid = sqlite3BtreeIntegerKey(&posts.cursor);
            } while (direction < 0 ? rowid > ids[i] : rowid < ids[i]);
            if (rowid != ids[i]) { rc = SQLITE_NOTFOUND; goto done; }
        }
        rc = cv_record(&posts, &post_record); if (rc) goto done;
        CvColumn post[4], user[2]; i64 author;
        rc = cv_columns(&post_record, 4, post); if (rc) goto done;
        rc = cv_integer(&post[1], &author); if (rc) goto done;
        if (!cv_text(&post[2]) || !cv_text(&post[3])) { rc = SQLITE_MISMATCH; goto done; }
        rc = cv_seek(&users, author); if (rc) goto done;
        rc = cv_record(&users, &user_record); if (rc) goto done;
        rc = cv_columns(&user_record, 2, user); if (rc) goto done;
        if (!cv_text(&user[1])) { rc = SQLITE_MISMATCH; goto done; }
        NativePost result = {.id = ids[i], .body = (const char *)post[2].bytes,
            .created_at = (const char *)post[3].bytes, .author = (const char *)user[1].bytes,
            .body_len = post[2].length, .created_at_len = post[3].length,
            .author_len = user[1].length, .like_count = counts[ids[i] - low]};
        sink(context, &result);
    }
    rc = SQLITE_OK;
done:
    cv_cursor_close(&likes); cv_cursor_close(&users); cv_cursor_close(&posts);
    sqlite3_free(like_record.spill); sqlite3_free(user_record.spill); sqlite3_free(post_record.spill);
    sqlite3BtreeLeave(tree); sqlite3_mutex_leave(connection->mutex);
    return rc;
}
