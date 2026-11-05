/* Keep SQLite's private APIs local to its amalgamation translation unit. */
// clang-format off
#include "sqlite3.c"
#include "sqlite_read.h"
#include "storage/wal_vfs.hpp"
#include "telemetry/wal_advice.h"
// clang-format on

#if TDS_TELEMETRY_LEVEL >= 1
static _Thread_local struct
{
    uint64_t calls;
    uint64_t errors;
    uint64_t requested_bytes;
#if TDS_TELEMETRY_LEVEL >= 2
    uint64_t event_count;
    uint64_t dropped_events;
    tds_wal_advice_event events[TDS_WAL_ADVICE_EVENT_CAPACITY];
#endif
} wal_advice;
#endif

static void record_wal_advice(int offset, i64 length, int result, unsigned event_capacity)
{
#if TDS_TELEMETRY_LEVEL < 2
    (void)event_capacity;
#endif
#if TDS_TELEMETRY_LEVEL == 1
    (void)offset;
#endif
#if TDS_TELEMETRY_LEVEL >= 1
    ++wal_advice.calls;
    wal_advice.errors += result != 0;
    wal_advice.requested_bytes += (uint64_t)length;
#if TDS_TELEMETRY_LEVEL >= 2
    if (wal_advice.event_count < event_capacity &&
        wal_advice.event_count < TDS_WAL_ADVICE_EVENT_CAPACITY)
    {
        const int saved_errno = errno;
        struct timespec now = {0, 0};
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        wal_advice.events[wal_advice.event_count++] =
            (tds_wal_advice_event){(uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec,
                                   (uint64_t)offset, (uint64_t)length, result};
        errno = saved_errno;
    }
    else
    {
        ++wal_advice.dropped_events;
    }
#endif
#else
    (void)offset;
    (void)length;
    (void)result;
#endif
}

void tds_wal_advice_take(tds_wal_advice_snapshot *snapshot)
{
#if TDS_TELEMETRY_LEVEL >= 1
    snapshot->calls = wal_advice.calls;
    snapshot->errors = wal_advice.errors;
    snapshot->requested_bytes = wal_advice.requested_bytes;
#else
    snapshot->calls = 0;
    snapshot->errors = 0;
    snapshot->requested_bytes = 0;
#endif
#if TDS_TELEMETRY_LEVEL >= 2
    snapshot->event_count = wal_advice.event_count;
    snapshot->dropped_events = wal_advice.dropped_events;
    memcpy(snapshot->events, wal_advice.events,
           (size_t)wal_advice.event_count * sizeof(*snapshot->events));
#else
    snapshot->event_count = 0;
    snapshot->dropped_events = 0;
#endif
#if TDS_TELEMETRY_LEVEL >= 1
    wal_advice.calls = 0;
    wal_advice.errors = 0;
    wal_advice.requested_bytes = 0;
#if TDS_TELEMETRY_LEVEL >= 2
    wal_advice.event_count = 0;
    wal_advice.dropped_events = 0;
#endif
#endif
}

void tds_wal_drop_checkpointed(sqlite3 *db, int frames, unsigned event_capacity)
{
    Wal *wal;
    sqlite3_file *file;
    i64 end;
    int page_size;
    int result;
    if (frames <= 0)
    {
        return;
    }

    wal = sqlite3BtreePager(db->aDb[0].pBt)->pWal;
    file = wal ? tds_wal_unwrap(wal->pWalFd) : 0;
    if (!file || file->pMethods->xRead != unixRead)
    {
        return;
    }

    page_size = osGetpagesize();
    end = WAL_HDRSIZE + (i64)frames * (sqlite3BtreeGetPageSize(db->aDb[0].pBt) + WAL_FRAME_HDRSIZE);
    end -= end % page_size;
    if (end <= page_size)
    {
        return;
    }

    // Slightly cursed: keep SQLites unixFile cast inside its own translation unit
    // Keep the header page and partial boundary pages; advice never changes file bytes
    result = posix_fadvise(((unixFile *)file)->h, page_size, end - page_size, POSIX_FADV_DONTNEED);
    record_wal_advice(page_size, end - page_size, result, event_capacity);
}

enum
{
    TDS_POSTS,
    TDS_USERS,
    TDS_LIKES,
    TDS_FEED,
    TDS_CURSOR_COUNT
};

enum
{
    TDS_FEED_LIMIT = 20,
    TDS_LIKE_SKIP_BUDGET = 16,
    TDS_ROW_SKIP_BUDGET = 16
};

typedef struct tds_record
{
    u8 *data;
    u32 capacity;
    u32 size;
} tds_record;

typedef struct tds_column
{
    u64 type;
    const u8 *data;
    u32 size;
} tds_column;

typedef struct tds_feed_entry
{
    i64 id;
    i64 like_count;
    i64 user_id;
    u32 body_offset;
    u32 body_bytes;
    u32 created_offset;
    u32 created_bytes;
    u32 author_offset;
    u32 author_bytes;
    int present;
} tds_feed_entry;

struct tds_btree
{
    sqlite3 *db;
    Btree *tree;
    Schema *schema;
    Pgno roots[TDS_CURSOR_COUNT];
    Pgno likes_table;
    u32 schema_cookie;
    int schema_generation;
    KeyInfo *likes_key;
    KeyInfo *feed_key;
    BtCursor *cursors[TDS_CURSOR_COUNT];
    unsigned opened;
    int active;
    int feed_started;
    int feed_done;
    int feed_rows;
    int feed_batch_count;
    int feed_batch_next;
    tds_feed_entry feed_batch[TDS_FEED_LIMIT];
    tds_record index_record;
    tds_record feed_posts;
    tds_record feed_users;
};

static int tds_table_layout(Table *table, int count, int primary_key, const char *const *names,
                            const char *affinities)
{
    int i;
    if (!table || !IsOrdinaryTable(table) || !HasRowid(table) || table->nCol != count ||
        table->iPKey != primary_key || !table->tnum || table->tnum > 0x7fffffffu ||
        (table->tabFlags & (TF_HasGenerated | TF_HasHidden | TF_Imposter)))
    {
        return 0;
    }
    for (i = 0; i < count; ++i)
    {
        if (strcmp(table->aCol[i].zCnName, names[i]) != 0 ||
            table->aCol[i].affinity != affinities[i])
        {
            return 0;
        }
    }
    return 1;
}

static int tds_index_layout(Index *index, Table *table, int keys, const int *columns,
                            const u8 *descending)
{
    int i;
    if (!index || index->pTable != table || !index->tnum || index->nKeyCol != keys ||
        index->nColumn != keys + 1 || index->pPartIdxWhere || index->aColExpr || index->bHasExpr ||
        index->aiColumn[keys] != XN_ROWID)
    {
        return 0;
    }
    for (i = 0; i < keys; ++i)
    {
        int column = index->aiColumn[i];
        if (column == XN_ROWID && columns[i] == table->iPKey)
        {
            column = table->iPKey;
        }
        if (column != columns[i] || index->aSortOrder[i] != descending[i] ||
            sqlite3StrICmp(index->azColl[i], "BINARY") != 0)
        {
            return 0;
        }
    }
    return 1;
}

static KeyInfo *tds_key_info(sqlite3 *db, Index *index)
{
    int i;
    KeyInfo *key = sqlite3KeyInfoAlloc(db, index->nKeyCol, index->nColumn - index->nKeyCol);
    if (!key)
    {
        return 0;
    }
    for (i = 0; i < index->nColumn; ++i)
    {
        key->aColl[i] = db->pDfltColl;
        key->aSortFlags[i] = index->aSortOrder[i];
    }
    return key;
}

static int tds_schema(tds_btree *reader)
{
    static const char *const user_names[] = {"id", "username", "created_at"};
    static const char *const post_names[] = {"id", "user_id", "body", "created_at"};
    static const char *const like_names[] = {"user_id", "post_id", "created_at"};
    static const char user_aff[] = {SQLITE_AFF_INTEGER, SQLITE_AFF_TEXT, SQLITE_AFF_TEXT};
    static const char post_aff[] = {SQLITE_AFF_INTEGER, SQLITE_AFF_INTEGER, SQLITE_AFF_TEXT,
                                    SQLITE_AFF_TEXT};
    static const char like_aff[] = {SQLITE_AFF_INTEGER, SQLITE_AFF_INTEGER, SQLITE_AFF_TEXT};
    static const int like_columns[] = {1};
    static const int feed_columns[] = {3, 0};
    static const u8 like_desc[] = {0};
    static const u8 feed_desc[] = {1, 1};
    sqlite3 *db = reader->db;
    Table *posts = sqlite3FindTable(db, "posts", "main");
    Table *users = sqlite3FindTable(db, "users", "main");
    Table *likes = sqlite3FindTable(db, "likes", "main");
    Index *like_index = sqlite3FindIndex(db, "likes_post_id_idx", "main");
    Index *feed_index = sqlite3FindIndex(db, "posts_created_at_id_idx", "main");
    const char *collation;

    if (ENC(db) != SQLITE_UTF8 || !tds_table_layout(posts, 4, 0, post_names, post_aff) ||
        !tds_table_layout(users, 3, 0, user_names, user_aff) ||
        !tds_table_layout(likes, 3, -1, like_names, like_aff) ||
        !tds_index_layout(like_index, likes, 1, like_columns, like_desc) ||
        !tds_index_layout(feed_index, posts, 2, feed_columns, feed_desc))
    {
        return SQLITE_MISMATCH;
    }
    collation = sqlite3ColumnColl(&posts->aCol[3]);
    if (collation && sqlite3StrICmp(collation, "BINARY") != 0)
    {
        return SQLITE_MISMATCH;
    }

    reader->roots[TDS_POSTS] = posts->tnum;
    reader->roots[TDS_USERS] = users->tnum;
    reader->roots[TDS_LIKES] = like_index->tnum;
    reader->roots[TDS_FEED] = feed_index->tnum;
    reader->likes_table = likes->tnum;
    reader->schema_cookie = (u32)db->aDb[0].pSchema->schema_cookie;
    reader->schema = db->aDb[0].pSchema;
    reader->schema_generation = reader->schema->iGeneration;
    reader->likes_key = tds_key_info(db, like_index);
    reader->feed_key = tds_key_info(db, feed_index);
    return reader->likes_key && reader->feed_key ? SQLITE_OK : SQLITE_NOMEM;
}

static void tds_close_cursors(tds_btree *reader)
{
    int i;
    for (i = 0; i < TDS_CURSOR_COUNT; ++i)
    {
        if (reader->opened & (1u << i))
        {
            sqlite3BtreeCloseCursor(reader->cursors[i]);
        }
    }
    reader->opened = 0;
    reader->active = 0;
    reader->feed_started = 0;
    reader->feed_done = 0;
    reader->feed_rows = 0;
    reader->feed_batch_count = 0;
    reader->feed_batch_next = 0;
    reader->index_record.size = 0;
    reader->feed_posts.size = 0;
    reader->feed_users.size = 0;
    /* Feed batches initialize their entries when filled, including after point reads */
}

int tds_btree_create(sqlite3 *db, tds_btree **out)
{
    sqlite3_stmt *schema = 0;
    tds_btree *reader;
    int i;
    int rc;
    if (!out)
    {
        return SQLITE_MISUSE;
    }
    *out = 0;
    if (!db)
    {
        return SQLITE_MISUSE;
    }
    /* Preparing loads SQLite's schema without retaining a result or statement. */
    rc = sqlite3_prepare_v2(
        db, "SELECT p.id,u.id,l.post_id FROM main.posts p,main.users u,main.likes l LIMIT 0", -1,
        &schema, 0);
    sqlite3_finalize(schema);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    reader = sqlite3_malloc64(sizeof(*reader));
    if (!reader)
    {
        return SQLITE_NOMEM;
    }
    memset(reader, 0, sizeof(*reader));
    reader->db = db;
    sqlite3_mutex_enter(db->mutex);
    sqlite3BtreeEnterAll(db);
    reader->tree = db->aDb[0].pBt;
    rc = reader->tree ? tds_schema(reader) : SQLITE_MISMATCH;
    for (i = 0; rc == SQLITE_OK && i < TDS_CURSOR_COUNT; ++i)
    {
        reader->cursors[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
        if (!reader->cursors[i])
        {
            rc = SQLITE_NOMEM;
        }
        else
        {
            sqlite3BtreeCursorZero(reader->cursors[i]);
        }
    }
    sqlite3BtreeLeaveAll(db);
    sqlite3_mutex_leave(db->mutex);
    if (rc != SQLITE_OK)
    {
        tds_btree_destroy(reader);
        return rc;
    }
    *out = reader;
    return SQLITE_OK;
}

void tds_btree_end(tds_btree *reader)
{
    if (!reader || !reader->opened)
    {
        return;
    }
    sqlite3_mutex_enter(reader->db->mutex);
    if (reader->tree)
    {
        sqlite3BtreeEnter(reader->tree);
    }
    tds_close_cursors(reader);
    if (reader->tree)
    {
        sqlite3BtreeLeave(reader->tree);
    }
    sqlite3_mutex_leave(reader->db->mutex);
}

void tds_btree_destroy(tds_btree *reader)
{
    int i;
    if (!reader)
    {
        return;
    }
    tds_btree_end(reader);
    for (i = 0; i < TDS_CURSOR_COUNT; ++i)
    {
        sqlite3_free(reader->cursors[i]);
    }
    sqlite3KeyInfoUnref(reader->likes_key);
    sqlite3KeyInfoUnref(reader->feed_key);
    sqlite3_free(reader->index_record.data);
    sqlite3_free(reader->feed_posts.data);
    sqlite3_free(reader->feed_users.data);
    sqlite3_free(reader);
}

int tds_btree_begin(tds_btree *reader)
{
    int rc = SQLITE_OK;
    int i;
    u32 cookie;
    if (!reader)
    {
        return SQLITE_MISUSE;
    }
    sqlite3_mutex_enter(reader->db->mutex);
    sqlite3BtreeEnter(reader->tree);
    if (reader->opened || reader->db->autoCommit)
    {
        rc = SQLITE_MISUSE;
        goto done;
    }
    rc = sqlite3BtreeBeginTrans(reader->tree, 0, 0);
    if (rc != SQLITE_OK)
    {
        goto done;
    }
    sqlite3BtreeGetMeta(reader->tree, BTREE_SCHEMA_VERSION, &cookie);
    if (cookie != reader->schema_cookie || ENC(reader->db) != SQLITE_UTF8)
    {
        rc = SQLITE_SCHEMA;
        goto done;
    }
    rc = sqlite3BtreeLockTable(reader->tree, (int)reader->roots[TDS_POSTS], 0);
    if (rc == SQLITE_OK)
    {
        rc = sqlite3BtreeLockTable(reader->tree, (int)reader->roots[TDS_USERS], 0);
    }
    if (rc == SQLITE_OK)
    {
        rc = sqlite3BtreeLockTable(reader->tree, (int)reader->likes_table, 0);
    }
    for (i = 0; rc == SQLITE_OK && i < TDS_CURSOR_COUNT; ++i)
    {
        KeyInfo *key = i == TDS_LIKES ? reader->likes_key : i == TDS_FEED ? reader->feed_key : 0;
        sqlite3BtreeCursorZero(reader->cursors[i]);
        rc = sqlite3BtreeCursor(reader->tree, reader->roots[i], 0, key, reader->cursors[i]);
        if (rc == SQLITE_OK)
        {
            reader->opened |= 1u << i;
        }
    }
    if (rc == SQLITE_OK)
    {
        reader->active = 1;
    }
    else
    {
        tds_close_cursors(reader);
    }
done:
    sqlite3BtreeLeave(reader->tree);
    sqlite3_mutex_leave(reader->db->mutex);
    return rc;
}

static int tds_record_read(tds_btree *reader, int cursor, tds_record *record)
{
    u32 size = sqlite3BtreePayloadSize(reader->cursors[cursor]);
    u8 *allocation;
    int rc;
    record->size = 0;
    if (!size)
    {
        return SQLITE_CORRUPT;
    }
    if (size > (u32)reader->db->aLimit[SQLITE_LIMIT_LENGTH])
    {
        return SQLITE_TOOBIG;
    }
    if (size > record->capacity)
    {
        allocation = sqlite3_realloc64(record->data, size);
        if (!allocation)
        {
            return SQLITE_NOMEM;
        }
        record->data = allocation;
        record->capacity = size;
    }
    rc = sqlite3BtreePayload(reader->cursors[cursor], 0, size, record->data);
    if (rc == SQLITE_OK)
    {
        record->size = size;
    }
    return rc;
}

static int tds_record_append(tds_btree *reader, int cursor, tds_record *record, u32 *offset)
{
    u32 size = sqlite3BtreePayloadSize(reader->cursors[cursor]);
    u64 total = (u64)record->size + size;
    int rc;
    if (!size)
    {
        return SQLITE_CORRUPT;
    }
    if (total > (u64)reader->db->aLimit[SQLITE_LIMIT_LENGTH])
    {
        return SQLITE_TOOBIG;
    }
    if (total > record->capacity)
    {
        u8 *allocation = sqlite3_realloc64(record->data, total);
        if (!allocation)
        {
            return SQLITE_NOMEM;
        }
        record->data = allocation;
        record->capacity = (u32)total;
    }
    *offset = record->size;
    rc = sqlite3BtreePayload(reader->cursors[cursor], 0, size, record->data + *offset);
    if (rc == SQLITE_OK)
    {
        record->size = (u32)total;
    }
    return rc;
}

static int tds_varint(const u8 **position, const u8 *end, u64 *value)
{
    u64 decoded = 0;
    int i;
    for (i = 0; i < 8; ++i)
    {
        u8 byte;
        if (*position == end)
        {
            return SQLITE_CORRUPT;
        }
        byte = *(*position)++;
        decoded = (decoded << 7) | (byte & 0x7f);
        if (!(byte & 0x80))
        {
            *value = decoded;
            return SQLITE_OK;
        }
    }
    if (*position == end)
    {
        return SQLITE_CORRUPT;
    }
    *value = (decoded << 8) | *(*position)++;
    return SQLITE_OK;
}

static int tds_record_columns(const u8 *data, u32 record_size, int count, tds_column *columns)
{
    static const u8 sizes[] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0};
    const u8 *header = data;
    const u8 *end = data + record_size;
    const u8 *header_end;
    const u8 *body;
    u64 header_size;
    int i;
    if (tds_varint(&header, end, &header_size) != SQLITE_OK || header_size > record_size ||
        header_size < (u64)(header - data))
    {
        return SQLITE_CORRUPT;
    }
    header_end = data + (u32)header_size;
    body = header_end;
    for (i = 0; i < count; ++i)
    {
        u64 type;
        u64 size;
        if (tds_varint(&header, header_end, &type) != SQLITE_OK || type == 10 || type == 11)
        {
            return SQLITE_CORRUPT;
        }
        size = type < 10 ? sizes[type] : (type - 12) / 2;
        if (size > (u64)(end - body))
        {
            return SQLITE_CORRUPT;
        }
        columns[i].type = type;
        columns[i].data = body;
        columns[i].size = (u32)size;
        body += (u32)size;
    }
    return header == header_end && body == end ? SQLITE_OK : SQLITE_MISMATCH;
}

static int tds_integer(const tds_column *column, i64 *out)
{
    u64 value = 0;
    u32 i;
    if (column->type == 8 || column->type == 9)
    {
        *out = (i64)(column->type - 8);
        return SQLITE_OK;
    }
    if (column->type < 1 || column->type > 6)
    {
        return SQLITE_MISMATCH;
    }
    for (i = 0; i < column->size; ++i)
    {
        value = (value << 8) | column->data[i];
    }
    if (!(column->data[0] & 0x80))
    {
        *out = (i64)value;
    }
    else
    {
        if (column->size < 8)
        {
            value |= (~(u64)0) << (column->size * 8);
        }
        *out = -1 - (i64)(~value);
    }
    return SQLITE_OK;
}

static int tds_text(const tds_column *column)
{
    return column->type >= 13 && (column->type & 1);
}

static int tds_seek_table(BtCursor *cursor, i64 id)
{
    int comparison;
    int rc = sqlite3BtreeTableMoveto(cursor, id, 0, &comparison);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    return comparison == 0 && !sqlite3BtreeEof(cursor) ? SQLITE_ROW : SQLITE_DONE;
}

int tds_btree_post_exists(tds_btree *reader, sqlite3_int64 id)
{
    int rc;
    u32 cookie;
    if (!reader)
    {
        return SQLITE_MISUSE;
    }
    sqlite3_mutex_enter(reader->db->mutex);
    sqlite3BtreeEnter(reader->tree);
    if (reader->active || reader->db->autoCommit)
    {
        rc = SQLITE_MISUSE;
        goto done;
    }
    rc = sqlite3BtreeBeginTrans(reader->tree, 0, 0);
    if (rc != SQLITE_OK)
    {
        goto done;
    }
    sqlite3BtreeGetMeta(reader->tree, BTREE_SCHEMA_VERSION, &cookie);
    if (cookie != reader->schema_cookie || ENC(reader->db) != SQLITE_UTF8)
    {
        rc = SQLITE_SCHEMA;
        goto done;
    }
    if (!(reader->opened & (1u << TDS_POSTS)))
    {
        rc = sqlite3BtreeLockTable(reader->tree, (int)reader->roots[TDS_POSTS], 0);
        if (rc != SQLITE_OK)
        {
            goto done;
        }
        sqlite3BtreeCursorZero(reader->cursors[TDS_POSTS]);
        rc = sqlite3BtreeCursor(reader->tree, reader->roots[TDS_POSTS], 0, 0,
                                reader->cursors[TDS_POSTS]);
        if (rc != SQLITE_OK)
        {
            goto done;
        }
        reader->opened |= 1u << TDS_POSTS;
    }
    // Keep the cursor within this transaction; every call still performs its own rowid seek
    rc = tds_seek_table(reader->cursors[TDS_POSTS], id);
done:
    sqlite3BtreeLeave(reader->tree);
    sqlite3_mutex_leave(reader->db->mutex);
    return rc;
}

static int tds_seek_ordered(BtCursor *cursor, i64 id, int *started, int *skipped)
{
    i64 current;
    int rc;
    if (!*started || sqlite3BtreeEof(cursor))
    {
        *started = 1;
        return tds_seek_table(cursor, id);
    }
    current = sqlite3BtreeIntegerKey(cursor);
    for (;;)
    {
        if (current == id)
        {
            return SQLITE_ROW;
        }
        if (current > id)
        {
            return SQLITE_DONE;
        }
        if (*skipped == TDS_ROW_SKIP_BUDGET)
        {
            return tds_seek_table(cursor, id);
        }
        rc = sqlite3BtreeNext(cursor, 0);
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        current = sqlite3BtreeIntegerKey(cursor);
        if (current < id)
        {
            ++*skipped;
        }
    }
}

static int tds_like_seek(tds_btree *reader, i64 id)
{
    Mem key;
    UnpackedRecord search;
    BtCursor *cursor = reader->cursors[TDS_LIKES];
    int comparison;
    int rc;
    memset(&key, 0, sizeof(key));
    memset(&search, 0, sizeof(search));
    key.flags = MEM_Int;
    key.u.i = id;
    search.pKeyInfo = reader->likes_key;
    search.aMem = &key;
    search.nField = 1;
    search.default_rc = 1;
    rc = sqlite3BtreeIndexMoveto(cursor, &search, &comparison);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    if (sqlite3BtreeEof(cursor))
    {
        return SQLITE_DONE;
    }
    if (comparison < 0)
    {
        rc = sqlite3BtreeNext(cursor, 0);
        return rc == SQLITE_OK ? SQLITE_ROW : rc;
    }
    return SQLITE_ROW;
}

/* Borrowed index fields must be decoded before the next Btree call. */
static int tds_index_columns(tds_btree *reader, int cursor, int count, tds_column *columns)
{
    u32 size = sqlite3BtreePayloadSize(reader->cursors[cursor]);
    u32 local;
    const u8 *data;
    int rc;
    if (!size)
    {
        return SQLITE_CORRUPT;
    }
    if (size > (u32)reader->db->aLimit[SQLITE_LIMIT_LENGTH])
    {
        return SQLITE_TOOBIG;
    }
    data = sqlite3BtreePayloadFetch(reader->cursors[cursor], &local);
    if (local == size)
    {
        return tds_record_columns(data, size, count, columns);
    }
    rc = tds_record_read(reader, cursor, &reader->index_record);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    return tds_record_columns(reader->index_record.data, reader->index_record.size, count, columns);
}

static int tds_like_key(tds_btree *reader, i64 *post_id)
{
    tds_column columns[2];
    int rc = tds_index_columns(reader, TDS_LIKES, 2, columns);
    return rc == SQLITE_OK ? tds_integer(&columns[0], post_id) : rc;
}

static int tds_like_cell(MemPage *page, int index, i64 *post_id)
{
    static const u8 sizes[] = {0, 1, 2, 3, 4, 6, 8, 0, 0, 0};
    const u8 *cell = findCell(page, index);
    const u8 *data;
    u32 size;
    u8 type;
    u8 rowid_type;
    tds_column column;
    if (cell >= page->aDataEnd || page->aDataEnd - cell <= page->childPtrSize)
    {
        return SQLITE_MISMATCH;
    }
    cell += page->childPtrSize;
    size = *cell;
    data = cell + 1;
    /* Two integer fields fit locally, with one-byte payload and header sizes */
    if (size < 3 || size > 19 || size > page->maxLocal || (size_t)(page->aDataEnd - data) < size ||
        data[0] != 3)
    {
        return SQLITE_MISMATCH;
    }
    type = data[1];
    rowid_type = data[2];
    if (type == 0 || type == 7 || type > 9 || rowid_type == 0 || rowid_type == 7 ||
        rowid_type > 9 || size != 3u + sizes[type] + sizes[rowid_type])
    {
        return SQLITE_MISMATCH;
    }
    column.type = type;
    column.data = data + 3;
    column.size = sizes[type];
    return tds_integer(&column, post_id);
}

static void tds_like_position(BtCursor *cursor, int index)
{
    /* Cursed cursor bookkeeping: cached cell metadata still describes the old index */
    cursor->ix = (u16)index;
    cursor->info.nSize = 0;
    cursor->curFlags &= (u8) ~(BTCF_ValidNKey | BTCF_ValidOvfl);
}

static int tds_like_counts(tds_btree *reader, tds_feed_entry *entries, int count, const int *order)
{
    BtCursor *cursor = reader->cursors[TDS_LIKES];
    int skipped = 0;
    int i;
    int rc;
    for (i = 0; i < count; ++i)
    {
        entries[i].like_count = 0;
    }
    if (!count)
    {
        return SQLITE_OK;
    }
    rc = tds_like_seek(reader, entries[order[0]].id);
    i = 0;
    while (rc == SQLITE_ROW)
    {
        MemPage *page = cursor->pPage;
        int cell = cursor->ix;
        /* Interior index entries count too, but their successor is in a child page */
        int end = page->leaf ? page->nCell : cell + 1;
        for (; cell < end; ++cell)
        {
            tds_feed_entry *entry;
            i64 post_id;
            int result = tds_like_cell(page, cell, &post_id);
            if (result != SQLITE_OK)
            {
                tds_like_position(cursor, cell);
                result = tds_like_key(reader, &post_id);
                if (result != SQLITE_OK)
                {
                    return result;
                }
            }
            while (post_id > entries[order[i]].id)
            {
                if (++i == count)
                {
                    tds_like_position(cursor, cell);
                    return SQLITE_OK;
                }
            }
            entry = &entries[order[i]];
            if (post_id < entry->id)
            {
                /* Keep the gap budget across seeks, even when feed IDs are far apart */
                if (skipped == TDS_LIKE_SKIP_BUDGET)
                {
                    tds_like_position(cursor, cell);
                    rc = tds_like_seek(reader, entry->id);
                    break;
                }
                ++skipped;
            }
            else
            {
                if (entry->like_count == LARGEST_INT64)
                {
                    return SQLITE_TOOBIG;
                }
                ++entry->like_count;
            }
        }
        if (cell == end)
        {
            tds_like_position(cursor, end - 1);
            rc = sqlite3BtreeNext(cursor, 0);
            if (rc == SQLITE_OK)
            {
                rc = SQLITE_ROW;
            }
        }
    }
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int tds_post_read(tds_btree *reader, i64 id, tds_btree_post *out)
{
    tds_column post[4];
    tds_column user[3];
    tds_feed_entry entry = {0};
    const int order = 0;
    i64 user_id;
    int rc = tds_seek_table(reader->cursors[TDS_POSTS], id);
    if (rc != SQLITE_ROW)
    {
        return rc;
    }
    rc = tds_record_read(reader, TDS_POSTS, &reader->feed_posts);
    if (rc == SQLITE_OK)
    {
        rc = tds_record_columns(reader->feed_posts.data, reader->feed_posts.size, 4, post);
    }
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    if (tds_integer(&post[1], &user_id) != SQLITE_OK || !tds_text(&post[2]) || !tds_text(&post[3]))
    {
        return SQLITE_MISMATCH;
    }
    rc = tds_seek_table(reader->cursors[TDS_USERS], user_id);
    if (rc != SQLITE_ROW)
    {
        return rc;
    }
    rc = tds_record_read(reader, TDS_USERS, &reader->feed_users);
    if (rc == SQLITE_OK)
    {
        rc = tds_record_columns(reader->feed_users.data, reader->feed_users.size, 3, user);
    }
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    if (!tds_text(&user[1]))
    {
        return SQLITE_MISMATCH;
    }
    entry.id = id;
    rc = tds_like_counts(reader, &entry, 1, &order);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    out->id = id;
    out->body = (const char *)post[2].data;
    out->body_bytes = (int)post[2].size;
    out->created_at = (const char *)post[3].data;
    out->created_at_bytes = (int)post[3].size;
    out->author = (const char *)user[1].data;
    out->author_bytes = (int)user[1].size;
    out->like_count = entry.like_count;
    return SQLITE_ROW;
}

int tds_btree_get_post(tds_btree *reader, sqlite3_int64 id, tds_btree_post *out)
{
    tds_btree_post row = {0};
    sqlite3 *db;
    int cookie;
    int i;
    int rc;
    int ended;
    if (!out)
    {
        return SQLITE_MISUSE;
    }
    memset(out, 0, sizeof(*out));
    if (!reader)
    {
        return SQLITE_MISUSE;
    }
    db = reader->db;
    sqlite3_mutex_enter(db->mutex);
    if (!db->autoCommit || db->nVdbeActive || db->nVdbeExec || reader->active || reader->opened)
    {
        rc = SQLITE_MISUSE;
        goto done;
    }
    if (reader->tree != db->aDb[0].pBt || reader->schema != db->aDb[0].pSchema ||
        !DbHasProperty(db, 0, DB_SchemaLoaded) || ENC(db) != SQLITE_UTF8 ||
        reader->schema_generation != reader->schema->iGeneration)
    {
        rc = SQLITE_SCHEMA;
        goto done;
    }
    sqlite3BtreeEnter(reader->tree);
    if (sqlite3BtreeTxnState(reader->tree) != SQLITE_TXN_NONE)
    {
        rc = SQLITE_MISUSE;
        goto leave;
    }
    rc = sqlite3BtreeBeginTrans(reader->tree, 0, &cookie);
    if (rc == SQLITE_OK && (u32)cookie != reader->schema_cookie)
    {
        rc = SQLITE_SCHEMA;
    }
    for (i = TDS_POSTS; rc == SQLITE_OK && i < TDS_FEED; ++i)
    {
        KeyInfo *key = i == TDS_LIKES ? reader->likes_key : 0;
        Pgno table = i == TDS_LIKES ? reader->likes_table : reader->roots[i];
        rc = sqlite3BtreeLockTable(reader->tree, (int)table, 0);
        if (rc != SQLITE_OK)
        {
            break;
        }
        sqlite3BtreeCursorZero(reader->cursors[i]);
        rc = sqlite3BtreeCursor(reader->tree, reader->roots[i], 0, key, reader->cursors[i]);
        if (rc == SQLITE_OK)
        {
            reader->opened |= 1u << i;
        }
    }
    if (rc == SQLITE_OK)
    {
        rc = tds_post_read(reader, id, &row);
    }
    /* Cursed ownership detail: schema failure can own a snapshot without opening any cursor */
    tds_close_cursors(reader);
    /* Read-only phase one is a no-op, phase two releases the pager and WAL read mark */
    ended = sqlite3BtreeCommitPhaseTwo(reader->tree, 0);
    if (ended != SQLITE_OK)
    {
        rc = ended;
    }
leave:
    sqlite3BtreeLeave(reader->tree);
done:
    sqlite3_mutex_leave(db->mutex);
    if (rc == SQLITE_ROW)
    {
        *out = row;
    }
    return rc;
}

static int tds_feed_posts(tds_btree *reader, const int *order)
{
    int started = 0;
    int skipped = 0;
    int i;
    for (i = 0; i < reader->feed_batch_count; ++i)
    {
        tds_feed_entry *entry = &reader->feed_batch[order[i]];
        tds_column columns[4];
        u32 offset;
        int rc = tds_seek_ordered(reader->cursors[TDS_POSTS], entry->id, &started, &skipped);
        if (rc == SQLITE_DONE)
        {
            continue;
        }
        if (rc != SQLITE_ROW)
        {
            return rc;
        }
        rc = tds_record_append(reader, TDS_POSTS, &reader->feed_posts, &offset);
        if (rc == SQLITE_OK)
        {
            rc = tds_record_columns(reader->feed_posts.data + offset,
                                    reader->feed_posts.size - offset, 4, columns);
        }
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        if (tds_integer(&columns[1], &entry->user_id) != SQLITE_OK || !tds_text(&columns[2]) ||
            !tds_text(&columns[3]))
        {
            return SQLITE_MISMATCH;
        }
        entry->body_offset = (u32)(columns[2].data - reader->feed_posts.data);
        entry->body_bytes = columns[2].size;
        entry->created_offset = (u32)(columns[3].data - reader->feed_posts.data);
        entry->created_bytes = columns[3].size;
        entry->present = 1;
    }
    return SQLITE_OK;
}

static int tds_feed_authors(tds_btree *reader)
{
    int order[TDS_FEED_LIMIT];
    int count = 0;
    int started = 0;
    int skipped = 0;
    int i;
    for (i = 0; i < reader->feed_batch_count; ++i)
    {
        int j = count;
        if (!reader->feed_batch[i].present)
        {
            continue;
        }
        while (j > 0 && reader->feed_batch[order[j - 1]].user_id > reader->feed_batch[i].user_id)
        {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
        ++count;
    }
    for (i = 0; i < count; ++i)
    {
        tds_feed_entry *entry = &reader->feed_batch[order[i]];
        tds_column columns[3];
        u32 offset;
        int rc;
        if (i > 0)
        {
            const tds_feed_entry *previous = &reader->feed_batch[order[i - 1]];
            if (entry->user_id == previous->user_id)
            {
                entry->present = previous->present;
                entry->author_offset = previous->author_offset;
                entry->author_bytes = previous->author_bytes;
                continue;
            }
        }
        rc = tds_seek_ordered(reader->cursors[TDS_USERS], entry->user_id, &started, &skipped);
        if (rc == SQLITE_DONE)
        {
            entry->present = 0;
            continue;
        }
        if (rc != SQLITE_ROW)
        {
            return rc;
        }
        rc = tds_record_append(reader, TDS_USERS, &reader->feed_users, &offset);
        if (rc == SQLITE_OK)
        {
            rc = tds_record_columns(reader->feed_users.data + offset,
                                    reader->feed_users.size - offset, 3, columns);
        }
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        if (!tds_text(&columns[1]))
        {
            return SQLITE_MISMATCH;
        }
        entry->author_offset = (u32)(columns[1].data - reader->feed_users.data);
        entry->author_bytes = columns[1].size;
    }
    return SQLITE_OK;
}

static int tds_feed_materialize(tds_btree *reader)
{
    int order[TDS_FEED_LIMIT];
    int i;
    int rc;
    for (i = 0; i < reader->feed_batch_count; ++i)
    {
        int j = i;
        while (j > 0 && reader->feed_batch[order[j - 1]].id > reader->feed_batch[i].id)
        {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
    }
    rc = tds_like_counts(reader, reader->feed_batch, reader->feed_batch_count, order);
    if (rc == SQLITE_OK)
    {
        rc = tds_feed_posts(reader, order);
    }
    return rc == SQLITE_OK ? tds_feed_authors(reader) : rc;
}

static int tds_feed_batch(tds_btree *reader)
{
    BtCursor *cursor = reader->cursors[TDS_FEED];
    int rc;
    reader->feed_batch_count = 0;
    reader->feed_batch_next = 0;
    reader->feed_posts.size = 0;
    reader->feed_users.size = 0;
    memset(reader->feed_batch, 0, sizeof(reader->feed_batch));
    if (!reader->feed_started)
    {
        int empty;
        rc = sqlite3BtreeFirst(cursor, &empty);
        reader->feed_started = 1;
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        if (empty)
        {
            reader->feed_done = 1;
            return SQLITE_DONE;
        }
    }
    else
    {
        rc = sqlite3BtreeNext(cursor, 0);
        if (rc != SQLITE_OK)
        {
            reader->feed_done = rc == SQLITE_DONE;
            return rc;
        }
    }
    for (;;)
    {
        tds_column columns[3];
        tds_feed_entry *entry = &reader->feed_batch[reader->feed_batch_count];
        rc = tds_index_columns(reader, TDS_FEED, 3, columns);
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        if (!tds_text(&columns[0]) || tds_integer(&columns[1], &entry->id) != SQLITE_OK)
        {
            return SQLITE_MISMATCH;
        }
        ++reader->feed_batch_count;
        if (reader->feed_batch_count == TDS_FEED_LIMIT)
        {
            break;
        }
        rc = sqlite3BtreeNext(cursor, 0);
        if (rc != SQLITE_OK)
        {
            reader->feed_done = rc == SQLITE_DONE;
            if (rc != SQLITE_DONE)
            {
                return rc;
            }
            break;
        }
    }
    return tds_feed_materialize(reader);
}

static int tds_feed_read(tds_btree *reader, tds_btree_post *out)
{
    while (reader->feed_rows < TDS_FEED_LIMIT)
    {
        tds_feed_entry *entry;
        int rc;
        if (reader->feed_batch_next == reader->feed_batch_count)
        {
            if (reader->feed_done)
            {
                return SQLITE_DONE;
            }
            rc = tds_feed_batch(reader);
            if (rc != SQLITE_OK)
            {
                return rc;
            }
        }
        entry = &reader->feed_batch[reader->feed_batch_next++];
        if (entry->present)
        {
            out->id = entry->id;
            out->body = (const char *)reader->feed_posts.data + entry->body_offset;
            out->body_bytes = (int)entry->body_bytes;
            out->created_at = (const char *)reader->feed_posts.data + entry->created_offset;
            out->created_at_bytes = (int)entry->created_bytes;
            out->author = (const char *)reader->feed_users.data + entry->author_offset;
            out->author_bytes = (int)entry->author_bytes;
            out->like_count = entry->like_count;
            ++reader->feed_rows;
            return SQLITE_ROW;
        }
    }
    return SQLITE_DONE;
}

int tds_btree_feed_next(tds_btree *reader, tds_btree_post *out)
{
    int rc;
    if (!reader || !out)
    {
        return SQLITE_MISUSE;
    }
    memset(out, 0, sizeof(*out));
    sqlite3_mutex_enter(reader->db->mutex);
    sqlite3BtreeEnter(reader->tree);
    rc = reader->active ? tds_feed_read(reader, out) : SQLITE_MISUSE;
    sqlite3BtreeLeave(reader->tree);
    sqlite3_mutex_leave(reader->db->mutex);
    return rc;
}
