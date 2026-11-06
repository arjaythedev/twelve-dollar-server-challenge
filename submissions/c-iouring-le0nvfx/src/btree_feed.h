#ifndef BTREE_FEED_H
#define BTREE_FEED_H

enum { FEED_BTREE_ROWS = 20, FEED_BTREE_SPAN = 256 };

typedef struct {
    const unsigned char *data;
    uint64_t serial;
} FeedBtreeColumn;

typedef struct {
    const unsigned char *data;
    uint32_t size;
} FeedBtreeRecord;

typedef struct {
    sqlite3_int64 id;
    sqlite3_int64 user_id;
    sqlite3_int64 like_count;
    uint32_t body_len;
    uint32_t created_len;
    uint32_t author_len;
    unsigned char body[2048];
    unsigned char created[256];
    unsigned char author[1024];
} FeedBtreeRow;

typedef struct {
    Btree *tree;
    Pgno root_posts;
    Pgno root_users;
    Pgno root_likes;
    Pgno root_feed;
    KeyInfo *likes_key;
    KeyInfo *feed_key;
    BtCursor *posts;
    BtCursor *users;
    BtCursor *likes;
    BtCursor *feed;
    unsigned opened;
    size_t overflow_cap;
    unsigned char *overflow;
    uint64_t successes;
    uint64_t fallbacks;
    bool enabled;
} FeedBtree;

static FeedBtree g_feed_btree;

typedef struct {
    const char *name;
    const char *type;
    int primary_key;
} FeedSchemaColumn;

typedef struct {
    const char *name;
    int descending;
} FeedSchemaIndexColumn;

static int feed_btree_table_layout(sqlite3 *db, const char *table,
                                   const FeedSchemaColumn *expected, int expected_count) {
    char sql[96];
    int size = snprintf(sql, sizeof(sql), "PRAGMA table_xinfo(\"%s\")", table);
    if (size < 0 || (size_t)size >= sizeof(sql)) return 0;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int count = 0;
    int valid = 1;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= expected_count) {
            valid = 0;
            break;
        }
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *type = sqlite3_column_text(stmt, 2);
        if (!name || !type || strcmp((const char *)name, expected[count].name) != 0 ||
            strcmp((const char *)type, expected[count].type) != 0 ||
            sqlite3_column_int(stmt, 5) != expected[count].primary_key ||
            sqlite3_column_int(stmt, 6) != 0) {
            valid = 0;
            break;
        }
        ++count;
    }
    if (rc != SQLITE_DONE || count != expected_count) valid = 0;
    sqlite3_finalize(stmt);
    return valid;
}

static int feed_btree_rowid_table(sqlite3 *db, const char *name, int columns,
                                  int primary_key_column) {
    Table *table = sqlite3FindTable(db, name, "main");
    int valid = table && HasRowid(table) && table->iPKey == primary_key_column &&
                table->nCol == columns;
    if (!valid) {
        fprintf(stderr, "SQLite B-tree feed table check %s: table=%p has_rowid=%d iPKey=%d nCol=%d expected=%d\n",
                name, (void *)table, table ? HasRowid(table) : 0,
                table ? table->iPKey : -2, table ? table->nCol : -1, columns);
    }
    return valid;
}

static int feed_btree_index_layout(sqlite3 *db, const char *index,
                                   const FeedSchemaIndexColumn *expected, int expected_count) {
    char sql[96];
    int size = snprintf(sql, sizeof(sql), "PRAGMA index_xinfo(\"%s\")", index);
    if (size < 0 || (size_t)size >= sizeof(sql)) return 0;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int count = 0;
    int valid = 1;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (sqlite3_column_int(stmt, 5) == 0) continue;
        if (count >= expected_count) {
            valid = 0;
            break;
        }
        const unsigned char *name = sqlite3_column_text(stmt, 2);
        const unsigned char *collation = sqlite3_column_text(stmt, 4);
        if (!name || strcmp((const char *)name, expected[count].name) != 0 ||
            !collation || strcmp((const char *)collation, "BINARY") != 0 ||
            sqlite3_column_int(stmt, 3) != expected[count].descending) {
            valid = 0;
            break;
        }
        ++count;
    }
    if (rc != SQLITE_DONE || count != expected_count) valid = 0;
    sqlite3_finalize(stmt);
    return valid;
}

static int feed_btree_index_is_full(sqlite3 *db, const char *table, const char *index) {
    char sql[96];
    int size = snprintf(sql, sizeof(sql), "PRAGMA index_list(\"%s\")", table);
    if (size < 0 || (size_t)size >= sizeof(sql)) return 0;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int found = 0;
    int valid = 1;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (!name || strcmp((const char *)name, index) != 0) continue;
        const unsigned char *origin = sqlite3_column_text(stmt, 3);
        if (!origin || strcmp((const char *)origin, "c") != 0 ||
            sqlite3_column_int(stmt, 2) != 0 || sqlite3_column_int(stmt, 4) != 0) {
            valid = 0;
        }
        found = 1;
        break;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) valid = 0;
    sqlite3_finalize(stmt);
    return valid && found;
}

static Pgno feed_btree_root(sqlite3 *db, const char *type, const char *name,
                            const char *table) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT rootpage FROM sqlite_schema "
                      "WHERE type=?1 AND name=?2 AND tbl_name=?3";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, table, -1, SQLITE_STATIC);
    Pgno root = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 value = sqlite3_column_int64(stmt, 0);
        if (value > 0 && (uint64_t)value <= UINT32_MAX) root = (Pgno)value;
    }
    sqlite3_finalize(stmt);
    return root;
}

static KeyInfo *feed_btree_key_info(sqlite3 *db, int key_columns,
                                    int extra_columns, bool descending) {
    KeyInfo *key = sqlite3KeyInfoAlloc(db, key_columns, extra_columns);
    if (!key) return NULL;
    for (int i = 0; i < key_columns + extra_columns; ++i) {
        key->aColl[i] = db->pDfltColl;
        if (descending && i < key_columns) key->aSortFlags[i] = KEYINFO_ORDER_DESC;
    }
    return key;
}

static int feed_btree_init(sqlite3 *db) {
    static const FeedSchemaColumn users[] = {
        {"id", "INTEGER", 1}, {"username", "TEXT", 0}, {"created_at", "TEXT", 0}
    };
    static const FeedSchemaColumn posts[] = {
        {"id", "INTEGER", 1}, {"user_id", "INTEGER", 0},
        {"body", "TEXT", 0}, {"created_at", "TEXT", 0}
    };
    static const FeedSchemaColumn likes[] = {
        {"user_id", "INTEGER", 1}, {"post_id", "INTEGER", 2}, {"created_at", "TEXT", 0}
    };
    static const FeedSchemaIndexColumn likes_index[] = {{"post_id", 0}};
    static const FeedSchemaIndexColumn feed_index[] = {{"created_at", 1}, {"id", 1}};

    memset(&g_feed_btree, 0, sizeof(g_feed_btree));
    g_feed_btree.tree = db->aDb[0].pBt;
    const char *schema_error = NULL;
    if (!g_feed_btree.tree) schema_error = "missing main B-tree";
    else if (db->enc != SQLITE_UTF8) schema_error = "database encoding is not UTF-8";
    else if (!feed_btree_table_layout(db, "users", users, (int)(sizeof(users) / sizeof(users[0]))))
        schema_error = "users columns differ";
    else if (!feed_btree_table_layout(db, "posts", posts, (int)(sizeof(posts) / sizeof(posts[0]))))
        schema_error = "posts columns differ";
    else if (!feed_btree_table_layout(db, "likes", likes, (int)(sizeof(likes) / sizeof(likes[0]))))
        schema_error = "likes columns differ";
    else if (!feed_btree_rowid_table(db, "users", (int)(sizeof(users) / sizeof(users[0])), 0) ||
             !feed_btree_rowid_table(db, "posts", (int)(sizeof(posts) / sizeof(posts[0])), 0) ||
             !feed_btree_rowid_table(db, "likes", (int)(sizeof(likes) / sizeof(likes[0])), -1))
        schema_error = "tables are not expected rowid tables";
    else if (!feed_btree_index_is_full(db, "likes", "likes_post_id_idx") ||
             !feed_btree_index_is_full(db, "posts", "posts_created_at_id_idx"))
        schema_error = "required index is missing, partial or unique";
    else if (!feed_btree_index_layout(db, "likes_post_id_idx", likes_index,
                                      (int)(sizeof(likes_index) / sizeof(likes_index[0]))))
        schema_error = "likes index columns differ";
    else if (!feed_btree_index_layout(db, "posts_created_at_id_idx", feed_index,
                                      (int)(sizeof(feed_index) / sizeof(feed_index[0]))))
        schema_error = "feed index columns differ";
    if (schema_error) {
        fprintf(stderr, "SQLite B-tree feed path disabled: %s\n", schema_error);
        return 0;
    }

    g_feed_btree.root_posts = feed_btree_root(db, "table", "posts", "posts");
    g_feed_btree.root_users = feed_btree_root(db, "table", "users", "users");
    g_feed_btree.root_likes = feed_btree_root(db, "index", "likes_post_id_idx", "likes");
    g_feed_btree.root_feed = feed_btree_root(db, "index", "posts_created_at_id_idx", "posts");
    if (!g_feed_btree.root_posts || !g_feed_btree.root_users ||
        !g_feed_btree.root_likes || !g_feed_btree.root_feed) {
        fprintf(stderr, "SQLite B-tree feed path disabled: missing root page\n");
        return 0;
    }

    g_feed_btree.likes_key = feed_btree_key_info(db, 1, 1, false);
    g_feed_btree.feed_key = feed_btree_key_info(db, 2, 1, true);
    int cursor_size = sqlite3BtreeCursorSize();
    if (!g_feed_btree.likes_key || !g_feed_btree.feed_key || cursor_size <= 0) return 0;
    g_feed_btree.posts = malloc((size_t)cursor_size);
    g_feed_btree.users = malloc((size_t)cursor_size);
    g_feed_btree.likes = malloc((size_t)cursor_size);
    g_feed_btree.feed = malloc((size_t)cursor_size);
    if (!g_feed_btree.posts || !g_feed_btree.users || !g_feed_btree.likes || !g_feed_btree.feed) {
        free(g_feed_btree.posts);
        free(g_feed_btree.users);
        free(g_feed_btree.likes);
        free(g_feed_btree.feed);
        g_feed_btree.posts = NULL;
        g_feed_btree.users = NULL;
        g_feed_btree.likes = NULL;
        g_feed_btree.feed = NULL;
        return 0;
    }
    g_feed_btree.enabled = true;
    fprintf(stderr, "SQLite B-tree feed path enabled\n");
    return 1;
}

static int feed_btree_open_cursors(unsigned count) {
    if (!g_feed_btree.enabled || g_feed_btree.opened != 0) return -1;
    if (count != 3U && count != 4U) return -1;
    if (!g_batch_write_txn && !g_batch_read_txn) {
        if (sqlite3BtreeBeginTrans(g_feed_btree.tree, 0, NULL) != SQLITE_OK) return -1;
        g_batch_read_txn = true;
    }

    BtCursor *cursor[] = {g_feed_btree.posts, g_feed_btree.users,
                          g_feed_btree.likes, g_feed_btree.feed};
    Pgno root[] = {g_feed_btree.root_posts, g_feed_btree.root_users,
                   g_feed_btree.root_likes, g_feed_btree.root_feed};
    KeyInfo *key[] = {NULL, NULL, g_feed_btree.likes_key, g_feed_btree.feed_key};
    for (unsigned i = 0; i < count; ++i) {
        sqlite3BtreeCursorZero(cursor[i]);
        if (sqlite3BtreeCursor(g_feed_btree.tree, root[i], 0, key[i], cursor[i]) != SQLITE_OK) {
            while (g_feed_btree.opened > 0) {
                BtCursor *opened = cursor[--g_feed_btree.opened];
                sqlite3BtreeCloseCursor(opened);
            }
            return -1;
        }
        ++g_feed_btree.opened;
    }
    return 0;
}

static int feed_btree_open(void) {
    return feed_btree_open_cursors(4U);
}

static int feed_btree_open_post(void) {
    return feed_btree_open_cursors(3U);
}

static void feed_btree_close(void) {
    BtCursor *cursor[] = {g_feed_btree.posts, g_feed_btree.users,
                          g_feed_btree.likes, g_feed_btree.feed};
    while (g_feed_btree.opened > 0) {
        BtCursor *opened = cursor[--g_feed_btree.opened];
        sqlite3BtreeCloseCursor(opened);
    }
}

static const unsigned char *feed_btree_varint(const unsigned char *p,
                                               const unsigned char *end,
                                               uint64_t *value) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        if (p >= end) return NULL;
        unsigned char byte = *p++;
        result = (result << 7) | (uint64_t)(byte & 0x7fU);
        if ((byte & 0x80U) == 0) {
            *value = result;
            return p;
        }
    }
    if (p >= end) return NULL;
    *value = (result << 8) | (uint64_t)*p++;
    return p;
}

static uint64_t feed_btree_serial_size(uint64_t serial) {
    static const unsigned char fixed[] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
    return serial < sizeof(fixed) ? fixed[serial] : (serial - 12U) / 2U;
}

static int feed_btree_record_columns(const FeedBtreeRecord *record, int count,
                                     FeedBtreeColumn *columns) {
    const unsigned char *end = record->data + record->size;
    uint64_t header_size = 0;
    const unsigned char *header = feed_btree_varint(record->data, end, &header_size);
    if (!header || header_size > record->size ||
        header_size < (uint64_t)(header - record->data)) return -1;
    const unsigned char *header_end = record->data + header_size;
    const unsigned char *body = header_end;
    for (int i = 0; i < count; ++i) {
        if (header >= header_end) return -1;
        header = feed_btree_varint(header, header_end, &columns[i].serial);
        if (!header) return -1;
        columns[i].data = body;
        uint64_t bytes = feed_btree_serial_size(columns[i].serial);
        if (bytes > (uint64_t)(end - body)) return -1;
        body += (size_t)bytes;
    }
    return 0;
}

static bool feed_btree_is_integer(const FeedBtreeColumn *column) {
    return (column->serial >= 1 && column->serial <= 6) ||
           column->serial == 8 || column->serial == 9;
}

static bool feed_btree_is_text(const FeedBtreeColumn *column) {
    return column->serial >= 13 && (column->serial & 1U) != 0;
}

static uint32_t feed_btree_text_size(const FeedBtreeColumn *column) {
    return (uint32_t)feed_btree_serial_size(column->serial);
}

static sqlite3_int64 feed_btree_integer(const FeedBtreeColumn *column) {
    if (column->serial == 8) return 0;
    if (column->serial == 9) return 1;
    uint32_t bytes = (uint32_t)feed_btree_serial_size(column->serial);
    uint64_t value = 0;
    for (uint32_t i = 0; i < bytes; ++i)
        value = (value << 8) | column->data[i];
    if (bytes < 8 && (value & (UINT64_C(1) << (bytes * 8U - 1U))) != 0)
        value |= UINT64_MAX << (bytes * 8U);
    return (sqlite3_int64)value;
}

static int feed_btree_read_record(FeedBtree *btree, BtCursor *cursor,
                                  FeedBtreeRecord *record) {
    uint32_t total = sqlite3BtreePayloadSize(cursor);
    uint32_t local = 0;
    const unsigned char *page_data = sqlite3BtreePayloadFetch(cursor, &local);
    if (local == total && (page_data || total == 0)) {
        record->data = page_data;
        record->size = total;
        return 0;
    }
    if ((size_t)total > btree->overflow_cap) {
        unsigned char *next = realloc(btree->overflow, (size_t)total);
        if (!next && total != 0) return -1;
        btree->overflow = next;
        btree->overflow_cap = total;
    }
    if (sqlite3BtreePayload(cursor, 0, total, btree->overflow) != SQLITE_OK) return -1;
    record->data = btree->overflow;
    record->size = total;
    return 0;
}

static int feed_btree_first(BtCursor *cursor) {
    int empty = 0;
    if (sqlite3BtreeFirst(cursor, &empty) != SQLITE_OK) return -1;
    return empty ? 1 : 0;
}

static int feed_btree_next(BtCursor *cursor) {
    int rc = sqlite3BtreeNext(cursor, 0);
    if (rc == SQLITE_OK) return 0;
    return rc == SQLITE_DONE ? 1 : -1;
}

static int feed_btree_seek_row(BtCursor *cursor, sqlite3_int64 rowid) {
    int result = 0;
    if (sqlite3BtreeTableMoveto(cursor, rowid, 0, &result) != SQLITE_OK) return -1;
    if (sqlite3BtreeEof(cursor)) return 1;
    return result < 0 ? feed_btree_next(cursor) : 0;
}

static int feed_btree_seek_index_ge(BtCursor *cursor, KeyInfo *key_info,
                                    sqlite3_int64 value) {
    Mem key_value;
    memset(&key_value, 0, sizeof(key_value));
    key_value.flags = MEM_Int;
    key_value.u.i = value;
    UnpackedRecord key;
    memset(&key, 0, sizeof(key));
    key.pKeyInfo = key_info;
    key.aMem = &key_value;
    key.nField = 1;
    key.default_rc = 1;
    int result = 0;
    if (sqlite3BtreeIndexMoveto(cursor, &key, &result) != SQLITE_OK) return -1;
    if (sqlite3BtreeEof(cursor)) return 1;
    return result < 0 ? feed_btree_next(cursor) : 0;
}

static int feed_btree_count_likes(sqlite3_int64 low, sqlite3_int64 high,
                                  sqlite3_int64 *counts) {
    size_t span = (size_t)(high - low + 1);
    memset(counts, 0, span * sizeof(*counts));
    int rc = feed_btree_seek_index_ge(g_feed_btree.likes, g_feed_btree.likes_key, low);
    while (rc == 0) {
        FeedBtreeRecord record;
        FeedBtreeColumn column[1];
        if (feed_btree_read_record(&g_feed_btree, g_feed_btree.likes, &record) != 0 ||
            feed_btree_record_columns(&record, 1, column) != 0 ||
            !feed_btree_is_integer(&column[0])) return -1;
        sqlite3_int64 post_id = feed_btree_integer(&column[0]);
        if (post_id > high) return 0;
        if (post_id < low) return -1;
        ++counts[(size_t)(post_id - low)];
        rc = feed_btree_next(g_feed_btree.likes);
    }
    return rc < 0 ? -1 : 0;
}

static int feed_btree_read_author(sqlite3_int64 user_id, FeedBtreeRow *row) {
    int rc = feed_btree_seek_row(g_feed_btree.users, user_id);
    if (rc != 0 || sqlite3BtreeIntegerKey(g_feed_btree.users) != user_id) return -1;
    FeedBtreeRecord record;
    FeedBtreeColumn columns[2];
    if (feed_btree_read_record(&g_feed_btree, g_feed_btree.users, &record) != 0 ||
        feed_btree_record_columns(&record, 2, columns) != 0 ||
        !feed_btree_is_text(&columns[1])) return -1;
    uint32_t size = feed_btree_text_size(&columns[1]);
    if (size > sizeof(row->author)) return -1;
    if (size) memcpy(row->author, columns[1].data, size);
    row->author_len = size;
    return 0;
}

static int feed_btree_read_post(sqlite3_int64 id, FeedBtreeRow *row) {
    int rc = feed_btree_seek_row(g_feed_btree.posts, id);
    if (rc == 1) return 1;
    if (rc != 0) return -1;
    if (sqlite3BtreeIntegerKey(g_feed_btree.posts) != id) return 1;

    FeedBtreeRecord record;
    FeedBtreeColumn columns[4];
    if (feed_btree_read_record(&g_feed_btree, g_feed_btree.posts, &record) != 0 ||
        feed_btree_record_columns(&record, 4, columns) != 0 ||
        !feed_btree_is_integer(&columns[1]) || !feed_btree_is_text(&columns[2]) ||
        !feed_btree_is_text(&columns[3])) return -1;
    uint32_t body_size = feed_btree_text_size(&columns[2]);
    uint32_t created_size = feed_btree_text_size(&columns[3]);
    if (body_size > sizeof(row->body) || created_size >= sizeof(row->created)) return -1;

    row->id = id;
    row->user_id = feed_btree_integer(&columns[1]);
    row->body_len = body_size;
    if (body_size) memcpy(row->body, columns[2].data, body_size);
    row->created_len = created_size;
    if (created_size) memcpy(row->created, columns[3].data, created_size);
    row->created[created_size] = '\0';

    sqlite3_int64 likes[1];
    if (feed_btree_count_likes(id, id, likes) != 0) return -1;
    row->like_count = likes[0];
    return feed_btree_read_author(row->user_id, row);
}

typedef struct {
    MemPage *page;
    int cell;
} FeedBtreeHit;

static int feed_btree_page_integer_key(MemPage *page, int cell,
                                       sqlite3_int64 *key) {
    if (!page || !page->intKey || cell < 0 || cell >= page->nCell || page->nOverflow)
        return -1;
    const unsigned char *end = page->aDataEnd;
    const unsigned char *p;
    uint64_t value = 0;
    if (page->leaf) {
        p = findCellPastPtr(page, cell);
        uint64_t payload_size = 0;
        p = feed_btree_varint(p, end, &payload_size);
        if (!p) return -1;
    } else {
        p = findCell(page, cell);
        if ((size_t)(end - p) < 4U) return -1;
        p += 4;
    }
    p = feed_btree_varint(p, end, &value);
    if (!p) return -1;
    *key = (sqlite3_int64)value;
    return 0;
}

static int feed_btree_page_lower_bound(MemPage *page, sqlite3_int64 key,
                                       int *cell_out) {
    if (!page || !page->intKey || page->nCell == 0 || page->nOverflow) return -1;
    int lo = 0;
    int hi = (int)page->nCell;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        sqlite3_int64 found;
        if (feed_btree_page_integer_key(page, mid, &found) != 0) return -1;
        if (found < key) lo = mid + 1;
        else hi = mid;
    }
    *cell_out = lo;
    return 0;
}

static void feed_btree_release_pages(MemPage **pages, int count) {
    for (int i = 0; i < count; ++i) {
        if (pages[i]) releasePageNotNull(pages[i]);
        pages[i] = NULL;
    }
}

/* Descend several rowid keys level by level, sharing references to common pages. */
static int feed_btree_seek_users_many(const sqlite3_int64 *keys, int count,
                                      FeedBtreeHit *hits) {
    if (count < 1 || count > FEED_BTREE_ROWS) return -1;
    Btree *tree = g_feed_btree.tree;
    BtShared *shared = tree->pBt;
    Pgno next[FEED_BTREE_ROWS];
    MemPage *pages[FEED_BTREE_ROWS];
    for (int i = 0; i < count; ++i) {
        next[i] = g_feed_btree.root_users;
        hits[i].page = NULL;
        hits[i].cell = -1;
    }

    for (int depth = 0; depth < BTCURSOR_MAX_DEPTH; ++depth) {
        memset(pages, 0, (size_t)count * sizeof(*pages));
        for (int i = 0; i < count; ++i) {
            int same = -1;
            for (int j = 0; j < i; ++j) {
                if (next[j] == next[i]) { same = j; break; }
            }
            if (same >= 0) {
                pages[i] = pages[same];
                sqlite3PagerRef(pages[i]->pDbPage);
            } else if (getAndInitPage(shared, next[i], &pages[i],
                                      PAGER_GET_READONLY) != SQLITE_OK) {
                feed_btree_release_pages(pages, i + 1);
                return -1;
            }
            if (!pages[i] || !pages[i]->intKey || pages[i]->nCell == 0 ||
                pages[i]->nOverflow ||
                (i > 0 && pages[i]->leaf != pages[0]->leaf)) {
                feed_btree_release_pages(pages, i + 1);
                return -1;
            }
        }

        if (pages[0]->leaf) {
            int cells[FEED_BTREE_ROWS];
            for (int i = 0; i < count; ++i) {
                cells[i] = -1;
                int cell;
                sqlite3_int64 found;
                if (feed_btree_page_lower_bound(pages[i], keys[i], &cell) != 0) {
                    feed_btree_release_pages(pages, count);
                    return -1;
                }
                if (cell < pages[i]->nCell) {
                    if (feed_btree_page_integer_key(pages[i], cell, &found) != 0) {
                        feed_btree_release_pages(pages, count);
                        return -1;
                    }
                    if (found == keys[i]) cells[i] = cell;
                }
            }
            for (int i = 0; i < count; ++i) {
                if (cells[i] < 0) {
                    releasePageNotNull(pages[i]);
                    pages[i] = NULL;
                } else {
                    hits[i].page = pages[i];
                    hits[i].cell = cells[i];
                    pages[i] = NULL;
                }
            }
            return 0;
        }

        bool valid = true;
        for (int i = 0; i < count; ++i) {
            int cell;
            if (feed_btree_page_lower_bound(pages[i], keys[i], &cell) != 0) {
                valid = false;
                break;
            }
            if (cell >= pages[i]->nCell) {
                next[i] = get4byte(&pages[i]->aData[pages[i]->hdrOffset + 8]);
            } else {
                next[i] = get4byte(findCell(pages[i], cell));
            }
            if (next[i] == 0) { valid = false; break; }
        }
        feed_btree_release_pages(pages, count);
        if (!valid) return -1;
    }
    return -1;
}

static int feed_btree_read_authors_many(FeedBtreeRow *rows, int count) {
    sqlite3_int64 keys[FEED_BTREE_ROWS];
    FeedBtreeHit hits[FEED_BTREE_ROWS];
    for (int i = 0; i < count; ++i) keys[i] = rows[i].user_id;
    if (feed_btree_seek_users_many(keys, count, hits) != 0) return -1;

    int rc = 0;
    for (int i = 0; i < count; ++i) {
        if (!hits[i].page) { rc = -1; break; }
        CellInfo info;
        hits[i].page->xParseCell(hits[i].page,
                                 findCell(hits[i].page, hits[i].cell), &info);
        if (info.nLocal != info.nPayload || !info.pPayload) { rc = 1; break; }
        FeedBtreeRecord record = {info.pPayload, info.nPayload};
        FeedBtreeColumn columns[2];
        if (feed_btree_record_columns(&record, 2, columns) != 0 ||
            !feed_btree_is_text(&columns[1])) { rc = 1; break; }
        uint32_t size = feed_btree_text_size(&columns[1]);
        if (size > sizeof(rows[i].author)) { rc = 1; break; }
        if (size) memcpy(rows[i].author, columns[1].data, size);
        rows[i].author_len = size;
    }
    for (int i = 0; i < count; ++i) {
        if (hits[i].page) releasePageNotNull(hits[i].page);
    }
    return rc;
}

static int feed_btree_render(Writer *writer) {
    sqlite3_int64 ids[FEED_BTREE_ROWS];
    int count = 0;
    int rc = feed_btree_first(g_feed_btree.feed);
    while (rc == 0 && count < FEED_BTREE_ROWS) {
        FeedBtreeRecord record;
        FeedBtreeColumn columns[2];
        if (feed_btree_read_record(&g_feed_btree, g_feed_btree.feed, &record) != 0 ||
            feed_btree_record_columns(&record, 2, columns) != 0 ||
            !feed_btree_is_text(&columns[0]) || !feed_btree_is_integer(&columns[1])) return -1;
        ids[count++] = feed_btree_integer(&columns[1]);
        if (count == FEED_BTREE_ROWS) break;
        rc = feed_btree_next(g_feed_btree.feed);
    }
    if (rc < 0) return -1;
    if (count == 0) {
        writer_str(writer, "{\"posts\":[]}");
        return writer->failed ? -1 : 0;
    }

    sqlite3_int64 low = ids[0], high = ids[0];
    for (int i = 1; i < count; ++i) {
        if (ids[i] < low) low = ids[i];
        if (ids[i] > high) high = ids[i];
    }
    if (low <= 0 || high < low || high - low >= FEED_BTREE_SPAN) return -1;

    sqlite3_int64 like_counts[FEED_BTREE_SPAN];
    if (feed_btree_count_likes(low, high, like_counts) != 0) return -1;
    int8_t slots[FEED_BTREE_SPAN];
    memset(slots, -1, sizeof(slots));
    for (int i = 0; i < count; ++i) slots[(size_t)(ids[i] - low)] = (int8_t)i;

    FeedBtreeRow rows[FEED_BTREE_ROWS];
    for (int i = 0; i < count; ++i) {
        rows[i].id = ids[i];
        rows[i].like_count = like_counts[(size_t)(ids[i] - low)];
    }

    int found = 0;
    rc = feed_btree_seek_row(g_feed_btree.posts, low);
    while (rc == 0) {
        sqlite3_int64 id = sqlite3BtreeIntegerKey(g_feed_btree.posts);
        if (id > high) break;
        if (id < low) return -1;
        size_t offset = (size_t)(id - low);
        int slot = slots[offset];
        if (slot >= 0) {
            FeedBtreeRecord record;
            FeedBtreeColumn columns[4];
            if (feed_btree_read_record(&g_feed_btree, g_feed_btree.posts, &record) != 0 ||
                feed_btree_record_columns(&record, 4, columns) != 0 ||
                !feed_btree_is_integer(&columns[1]) || !feed_btree_is_text(&columns[2]) ||
                !feed_btree_is_text(&columns[3])) return -1;
            uint32_t body_size = feed_btree_text_size(&columns[2]);
            uint32_t created_size = feed_btree_text_size(&columns[3]);
            if (body_size > sizeof(rows[slot].body) || created_size >= sizeof(rows[slot].created)) return -1;
            if (body_size) memcpy(rows[slot].body, columns[2].data, body_size);
            if (created_size) memcpy(rows[slot].created, columns[3].data, created_size);
            rows[slot].created[created_size] = '\0';
            rows[slot].body_len = body_size;
            rows[slot].created_len = created_size;
            rows[slot].user_id = feed_btree_integer(&columns[1]);
            ++found;
            if (found == count) break;
        }
        rc = feed_btree_next(g_feed_btree.posts);
    }
    if (rc < 0 || found != count) return -1;

    if (feed_btree_read_authors_many(rows, count) != 0) {
        for (int i = 0; i < count; ++i) {
            if (feed_btree_read_author(rows[i].user_id, &rows[i]) != 0) return -1;
        }
    }

    writer_str(writer, "{\"posts\":[");
    for (int i = 0; i < count; ++i) {
        if (i) writer_char(writer, ',');
        writer_post(writer, rows[i].id, rows[i].body, (int)rows[i].body_len,
                    rows[i].created, rows[i].author,
                    (int)rows[i].author_len, rows[i].like_count);
    }
    writer_str(writer, "]}");
    return writer->failed ? -1 : 0;
}

#endif
