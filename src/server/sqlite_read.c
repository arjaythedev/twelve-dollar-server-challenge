/* Keep SQLite's private APIs local to its amalgamation translation unit. */
// clang-format off
#include "sqlite3.c"
#include "sqlite_read.h"
// clang-format on

enum
{
    TDS_POSTS,
    TDS_USERS,
    TDS_LIKES,
    TDS_FEED,
    TDS_CURSOR_COUNT
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

struct tds_btree
{
    sqlite3 *db;
    Btree *tree;
    Pgno roots[TDS_CURSOR_COUNT];
    Pgno likes_table;
    u32 schema_cookie;
    KeyInfo *likes_key;
    KeyInfo *feed_key;
    BtCursor *cursors[TDS_CURSOR_COUNT];
    unsigned opened;
    int active;
    int feed_started;
    int feed_done;
    int feed_rows;
    tds_record post_record;
    tds_record user_record;
    tds_record index_record;
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
    reader->post_record.size = 0;
    reader->user_record.size = 0;
    reader->index_record.size = 0;
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
    if (!reader)
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
    sqlite3_free(reader->post_record.data);
    sqlite3_free(reader->user_record.data);
    sqlite3_free(reader->index_record.data);
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

static int tds_record_columns(const tds_record *record, int count, tds_column *columns)
{
    static const u8 sizes[] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0};
    const u8 *header = record->data;
    const u8 *end = header + record->size;
    const u8 *header_end;
    const u8 *body;
    u64 header_size;
    int i;
    if (tds_varint(&header, end, &header_size) != SQLITE_OK || header_size > record->size ||
        header_size < (u64)(header - record->data))
    {
        return SQLITE_CORRUPT;
    }
    header_end = record->data + (u32)header_size;
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

static int tds_like_count(tds_btree *reader, i64 id, i64 *count)
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
    *count = 0;
    rc = sqlite3BtreeIndexMoveto(cursor, &search, &comparison);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    if (sqlite3BtreeEof(cursor))
    {
        return SQLITE_OK;
    }
    if (comparison < 0)
    {
        rc = sqlite3BtreeNext(cursor, 0);
    }
    while (rc == SQLITE_OK)
    {
        tds_column columns[2];
        i64 post_id;
        rc = tds_record_read(reader, TDS_LIKES, &reader->index_record);
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        rc = tds_record_columns(&reader->index_record, 2, columns);
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        rc = tds_integer(&columns[0], &post_id);
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        if (post_id != id)
        {
            return SQLITE_OK;
        }
        if (*count == LARGEST_INT64)
        {
            return SQLITE_TOOBIG;
        }
        ++*count;
        rc = sqlite3BtreeNext(cursor, 0);
    }
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int tds_post_read(tds_btree *reader, i64 id, tds_btree_post *out)
{
    tds_column post[4];
    tds_column user[3];
    i64 user_id;
    int rc = tds_seek_table(reader->cursors[TDS_POSTS], id);
    if (rc != SQLITE_ROW)
    {
        return rc;
    }
    rc = tds_record_read(reader, TDS_POSTS, &reader->post_record);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    rc = tds_record_columns(&reader->post_record, 4, post);
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
    rc = tds_record_read(reader, TDS_USERS, &reader->user_record);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    rc = tds_record_columns(&reader->user_record, 3, user);
    if (rc != SQLITE_OK)
    {
        return rc;
    }
    if (!tds_text(&user[1]))
    {
        return SQLITE_MISMATCH;
    }
    rc = tds_like_count(reader, id, &out->like_count);
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
    return SQLITE_ROW;
}

int tds_btree_get_post(tds_btree *reader, sqlite3_int64 id, tds_btree_post *out)
{
    int rc;
    if (!reader || !out)
    {
        return SQLITE_MISUSE;
    }
    memset(out, 0, sizeof(*out));
    sqlite3_mutex_enter(reader->db->mutex);
    sqlite3BtreeEnter(reader->tree);
    rc = reader->active ? tds_post_read(reader, id, out) : SQLITE_MISUSE;
    sqlite3BtreeLeave(reader->tree);
    sqlite3_mutex_leave(reader->db->mutex);
    return rc;
}

static int tds_feed_read(tds_btree *reader, tds_btree_post *out)
{
    BtCursor *cursor = reader->cursors[TDS_FEED];
    int rc;
    if (reader->feed_done || reader->feed_rows == 20)
    {
        return SQLITE_DONE;
    }
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
        i64 id;
        rc = tds_record_read(reader, TDS_FEED, &reader->index_record);
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        rc = tds_record_columns(&reader->index_record, 3, columns);
        if (rc != SQLITE_OK)
        {
            return rc;
        }
        if (!tds_text(&columns[0]) || tds_integer(&columns[1], &id) != SQLITE_OK)
        {
            return SQLITE_MISMATCH;
        }
        rc = tds_post_read(reader, id, out);
        if (rc == SQLITE_ROW)
        {
            ++reader->feed_rows;
            return rc;
        }
        if (rc != SQLITE_DONE)
        {
            return rc;
        }
        /* An absent author is omitted by the SQL INNER JOIN before LIMIT. */
        rc = sqlite3BtreeNext(cursor, 0);
        if (rc != SQLITE_OK)
        {
            reader->feed_done = rc == SQLITE_DONE;
            return rc;
        }
    }
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
