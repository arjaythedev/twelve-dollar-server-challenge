/* Two read plans over the stock amalgamation's pager, WAL and B-tree. */
#include "sqlite3.c"
#include "challenge.h"
#include <limits.h>

enum { FEED_SIZE = 20 };
enum { READ_FALLBACK = -1 };

typedef struct {
    const unsigned char *p;
    unsigned n;
} TextView;

typedef struct {
    i64 id, uid, likes;
    TextView body, timestamp;
} PostView;

typedef struct {
    Pgno root;
    KeyInfo *key;
} ReadTree;

static struct {
    sqlite3 *db;
    Btree *bt;
    ReadTree posts, users, time, likes;
    sqlite3_stmt *sql_feed, *sql_post;
    int fast;
} reader;

static int table_layout(Table *t, int n, const char *const *names) {
    if (!t || !HasRowid(t) || t->iPKey != 0 || t->nCol != n) return 0;
    for (int i = 0; i < n; ++i)
        if (strcmp(t->aCol[i].zCnName, names[i])) return 0;
    return 1;
}

static int index_layout(Index *idx, Table *t, int n, const int *columns, int descending) {
    if (!idx || idx->pTable != t || idx->nKeyCol != n || idx->pPartIdxWhere) return 0;
    for (int i = 0; i < n; ++i)
        if (idx->aiColumn[i] != columns[i] || idx->aSortOrder[i] != descending ||
            sqlite3_stricmp(idx->azColl[i], "BINARY")) return 0;
    return 1;
}

static KeyInfo *index_key(int n, int descending) {
    KeyInfo *ki = sqlite3KeyInfoAlloc(reader.db, n, 1);
    if (ki) {
        for (int i = 0; i <= n; ++i) ki->aColl[i] = reader.db->pDfltColl;
        for (int i = 0; i < n; ++i) ki->aSortFlags[i] = descending ? KEYINFO_ORDER_DESC : 0;
    }
    return ki;
}

int reader_init(sqlite3 *db) {
    static const char *const posts_columns[] = {"id", "user_id", "body", "created_at"};
    static const char *const users_columns[] = {"id", "username", "created_at"};
    static const int time_columns[] = {3, 0}, like_columns[] = {1};
    static const char select_row[] =
        "SELECT p.id,p.body,p.created_at,u.username,"
        "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
        "FROM posts p JOIN users u ON u.id=p.user_id ";
    char query[384];
    reader.db = db;
    snprintf(query, sizeof query, "%s ORDER BY p.created_at DESC,p.id DESC LIMIT 20", select_row);
    int rc = sqlite3_prepare_v2(db, query, -1, &reader.sql_feed, 0);
    if (rc != SQLITE_OK) return rc;
    snprintf(query, sizeof query, "%s WHERE p.id=?1", select_row);
    rc = sqlite3_prepare_v2(db, query, -1, &reader.sql_post, 0);
    if (rc != SQLITE_OK) return rc;
    Table *posts = sqlite3FindTable(db, "posts", "main");
    Table *users = sqlite3FindTable(db, "users", "main");
    Table *likes = sqlite3FindTable(db, "likes", "main");
    Index *order = sqlite3FindIndex(db, "posts_created_at_id_idx", "main");
    Index *counts = sqlite3FindIndex(db, "likes_post_id_idx", "main");
    if (ENC(db) != SQLITE_UTF8 || !table_layout(posts, 4, posts_columns) ||
        !table_layout(users, 3, users_columns) ||
        !index_layout(order, posts, 2, time_columns, 1) ||
        !index_layout(counts, likes, 1, like_columns, 0)) return SQLITE_OK;
    reader.bt = db->aDb[0].pBt;
    reader.posts.root = posts->tnum;
    reader.users.root = users->tnum;
    reader.time.root = order->tnum;
    reader.likes.root = counts->tnum;
    reader.time.key = index_key(2, 1);
    reader.likes.key = index_key(1, 0);
    if (!reader.time.key || !reader.likes.key) return SQLITE_NOMEM;
    reader.fast = 1;
    return SQLITE_OK;
}

void reader_close(void) {
    sqlite3_finalize(reader.sql_feed);
    sqlite3_finalize(reader.sql_post);
    sqlite3KeyInfoUnref(reader.time.key);
    sqlite3KeyInfoUnref(reader.likes.key);
    memset(&reader, 0, sizeof reader);
}

/* Cursors and page references belong to this request, even if BEGIN/COMMIT
 * enclose a batch. A deferred SQL BEGIN acquires its snapshot on first access. */
static int read_cursor(BtCursor *cursor, const ReadTree *tree) {
    sqlite3BtreeCursorZero(cursor);
    return sqlite3BtreeCursor(reader.bt, tree->root, 0, tree->key, cursor);
}

typedef struct {
    const u8 *types, *end_types, *values, *end;
} RecordView;

static int record_view(const CellInfo *cell, RecordView *r) {
    if (!cell->nLocal || cell->nLocal != cell->nPayload) return 0;
    const u8 *p = cell->pPayload;
    /* These tables have at most four columns. Larger headers use the SQL plan. */
    unsigned header = p[0];
    if (header < 2 || header >= 128 || header > cell->nLocal) return 0;
    *r = (RecordView){p + 1, p + header, p + header, p + cell->nLocal};
    return 1;
}

static inline int next_field(RecordView *r, unsigned *type, TextView *value) {
    if (r->types == r->end_types) return 0;
    unsigned t = *r->types++;
    if (t >= 128) {
        t &= 127;
        for (;;) {
            if (r->types == r->end_types || t > (UINT_MAX >> 7)) return 0;
            unsigned b = *r->types++;
            t = (t << 7) | (b & 127);
            if (b < 128) break;
        }
    }
    unsigned length = sqlite3VdbeSerialTypeLen(t);
    if (length > (size_t)(r->end - r->values)) return 0;
    *type = t;
    *value = (TextView){r->values, length};
    r->values += length;
    return 1;
}

static inline int next_integer(RecordView *r, i64 *out) {
    unsigned t;
    TextView bytes;
    if (!next_field(r, &t, &bytes) || (!(t >= 1 && t <= 6) && t != 8 && t != 9)) return 0;
    Mem value;
    sqlite3VdbeSerialGet(bytes.p, t, &value);
    *out = value.u.i;
    return 1;
}

static inline int next_text(RecordView *r, TextView *out) {
    unsigned t;
    return next_field(r, &t, out) && t >= 13 && (t & 1);
}

static inline int skip_field(RecordView *r) {
    unsigned t;
    TextView ignored;
    return next_field(r, &t, &ignored);
}

static inline void parse_cell(MemPage *page, unsigned index, CellInfo *cell) {
    page->xParseCell(page, findCell(page, index), cell);
}

static int post_view(const CellInfo *cell, PostView *post) {
    /* Fast decode the common local INTEGER/TEXT/TEXT record. Both the
     * original serial header and total payload length must agree. */
    if (cell->nLocal == cell->nPayload && cell->nLocal >= 8) {
        const u8 *p = cell->pPayload;
        unsigned header = p[0], uid_type = p[2], body_type = p[3], time_type = 0;
        if (header == 5 && body_type < 128) {
            time_type = p[4];
        } else if (header == 6 && body_type >= 128 && p[4] < 128) {
            body_type = ((body_type & 127) << 7) | p[4];
            time_type = p[5];
        }
        if (p[1] == 0 && uid_type >= 1 && uid_type <= 3 &&
            body_type >= 13 && (body_type & 1) &&
            time_type >= 13 && time_type < 128 && (time_type & 1)) {
            unsigned body_len = (body_type - 13) / 2;
            unsigned time_len = (time_type - 13) / 2;
            if (header + uid_type + body_len + time_len == cell->nLocal) {
                const u8 *v = p + header;
                int uid = (int8_t)*v++;
                if (uid_type >= 2) uid = uid * 256 + *v++;
                if (uid_type == 3) uid = uid * 256 + *v++;
                *post = (PostView){.id = cell->nKey, .uid = uid, .likes = 0,
                    .body = {v, body_len}, .timestamp = {v + body_len, time_len}};
                return 1;
            }
        }
    }
    RecordView r;
    post->id = cell->nKey;
    post->likes = 0;
    return record_view(cell, &r) && skip_field(&r) && next_integer(&r, &post->uid) &&
           next_text(&r, &post->body) && next_text(&r, &post->timestamp);
}

static int text_compare(TextView a, TextView b) {
    unsigned n = a.n < b.n ? a.n : b.n;
    int v = memcmp(a.p, b.p, n);
    return v ? v : (a.n > b.n) - (a.n < b.n);
}

/* An ordinal within SQLite's first index leaf is its ordinal in the whole
 * index. If the leaf is short, normal cursor steps include its parent keys. */
static int rank_twenty(i64 *id) {
    BtCursor c;
    int rc = read_cursor(&c, &reader.time);
    if (rc != SQLITE_OK) return rc;
    int empty;
    rc = sqlite3BtreeFirst(&c, &empty);
    if (rc == SQLITE_OK && empty) rc = READ_FALLBACK;
    if (rc == SQLITE_OK) {
        unsigned cell = FEED_SIZE - 1;
        if (!c.pPage->leaf || c.pPage->nCell < FEED_SIZE) {
            for (int i = 1; i < FEED_SIZE && rc == SQLITE_OK; ++i) rc = sqlite3BtreeNext(&c, 0);
            if (rc == SQLITE_DONE) rc = READ_FALLBACK;
            cell = c.ix;
        }
        if (rc == SQLITE_OK) {
            CellInfo info;
            RecordView record;
            parse_cell(c.pPage, cell, &info);
            if (!record_view(&info, &record) || !skip_field(&record) || !next_integer(&record, id))
                rc = READ_FALLBACK;
        }
    }
    sqlite3BtreeCloseCursor(&c);
    return rc;
}

/* Keep leaf pages behind the borrowed text until the JSON is written. This
 * avoids copying 20 bodies and timestamps to scratch before serializing them. */
static int tail_posts(PostView *rows, int *count, DbPage **pages, int *n_pages, int *ordered) {
    BtCursor c;
    int rc = read_cursor(&c, &reader.posts);
    if (rc != SQLITE_OK) return rc;
    int empty;
    rc = sqlite3BtreeLast(&c, &empty);
    while (rc == SQLITE_OK && !empty) {
        MemPage *page = c.pPage;
        if (!page->intKeyLeaf) { rc = READ_FALLBACK; break; }
        sqlite3PagerRef(page->pDbPage);
        pages[(*n_pages)++] = page->pDbPage;
        for (int i = c.ix; i >= 0 && *count < FEED_SIZE; --i) {
            CellInfo cell;
            PostView *p = &rows[*count];
            parse_cell(page, i, &cell);
            if (!post_view(&cell, p)) {
                rc = READ_FALLBACK;
                break;
            }
            if (*ordered && *count && text_compare(rows[*count - 1].timestamp, p->timestamp) < 0)
                *ordered = 0;
            ++*count;
        }
        if (rc != SQLITE_OK || *count == FEED_SIZE) break;
        /* We inspected the leaf without changing SQLite's cursor. Walk it to
         * its predecessor through the SQLite API, including the page boundary. */
        for (int i = c.ix; i >= 0 && rc == SQLITE_OK; --i) rc = sqlite3BtreePrevious(&c, 0);
        if (rc == SQLITE_DONE) { rc = SQLITE_OK; break; }
    }
    sqlite3BtreeCloseCursor(&c);
    return rc;
}

static int like_key(MemPage *page, unsigned pos, i64 *id) {
    /* The first column of this index is an integer. Its physical length is
     * fixed by the serial type; most two-column index records fit one cell. */
    const u8 *cell = findCellPastPtr(page, pos);
    unsigned len = cell[0];
    if (len >= 3 && len < 128 && (size_t)(page->aDataEnd - cell) > len && cell[1] == 3) {
        unsigned t = cell[2];
        const u8 *p = cell + 4;
        if (t == 3 && len >= 6) {
            *id = (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8) >> 8;
            return SQLITE_OK;
        }
        if (t == 2 && len >= 5) { *id = (int16_t)get2byte(p); return SQLITE_OK; }
        if (t == 4 && len >= 7) { *id = (int32_t)get4byte(p); return SQLITE_OK; }
    }
    CellInfo info;
    RecordView record;
    parse_cell(page, pos, &info);
    return record_view(&info, &record) && next_integer(&record, id) ? SQLITE_OK : READ_FALLBACK;
}

static int seek_like(BtCursor *c, i64 id) {
    Mem field = {.flags = MEM_Int, .u.i = id};
    UnpackedRecord key = {.pKeyInfo = reader.likes.key, .aMem = &field, .nField = 1, .default_rc = 1};
    int cmp;
    int rc = sqlite3BtreeIndexMoveto(c, &key, &cmp);
    if (rc != SQLITE_OK) return rc;
    if (sqlite3BtreeEof(c)) return SQLITE_DONE;
    return cmp < 0 ? sqlite3BtreeNext(c, 0) : SQLITE_OK;
}

/* IDs are in decreasing order. A fresh maximum key proves every id above it
 * has zero likes. Else merge an ascending scan with the requested ids, using
 * a new lower_bound across large gaps. Never walk the rest of a sparse range. */
static int read_likes(PostView *rows, int count) {
    if (!count) return SQLITE_OK;
    BtCursor c;
    int rc = read_cursor(&c, &reader.likes);
    if (rc != SQLITE_OK) return rc;
    int empty;
    rc = sqlite3BtreeLast(&c, &empty);
    i64 last = 0;
    Pgno rightmost = 0;
    if (rc == SQLITE_OK && !empty) {
        rightmost = c.pPage->pgno;
        rc = like_key(c.pPage, c.ix, &last);
    }
    int i = count - 1;
    if (rc == SQLITE_OK && !empty && rows[i].id <= last) {
        rc = seek_like(&c, rows[i].id);
        while (rc == SQLITE_OK) {
            MemPage *page = c.pPage;
            int start = c.ix;
            int stop = page->leaf ? page->nCell : start + 1;
            int seek = 0;
            for (int j = start; j < stop; ++j) {
                i64 key;
                rc = like_key(page, j, &key);
                if (rc != SQLITE_OK) break;
                while (i >= 0 && rows[i].id < key) --i;
                if (i < 0 || rows[i].id > last) { i = -1; break; }
                if ((u64)rows[i].id - (u64)key > 64) { seek = 1; break; }
                if (rows[i].id != key) continue;
                /* Several hundred users can like the same post. For a long
                 * equal-key run, subtract cell ordinals instead of decoding
                 * each like; all keys between equal endpoints are equal. */
                int end = j + 1;
                if (j + 8 < stop) {
                    i64 ahead;
                    rc = like_key(page, j + 8, &ahead);
                    if (rc != SQLITE_OK) break;
                    if (ahead == key) {
                        int lo = j + 9, hi = stop;
                        while (lo < hi) {
                            int mid = lo + (hi - lo) / 2;
                            rc = like_key(page, mid, &ahead);
                            if (rc != SQLITE_OK) break;
                            if (ahead == key) lo = mid + 1; else hi = mid;
                        }
                        if (rc != SQLITE_OK) break;
                        end = lo;
                    }
                }
                rows[i].likes += end - j;
                j = end - 1;
            }
            if (rc != SQLITE_OK || i < 0) break;
            if (seek) {
                rc = seek_like(&c, rows[i].id);
            } else {
                if (page->leaf && page->pgno == rightmost) break;
                for (int j = start; j < stop && rc == SQLITE_OK; ++j) rc = sqlite3BtreeNext(&c, 0);
            }
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    sqlite3BtreeCloseCursor(&c);
    return rc;
}

#define READ_LIT(out, string) buf_append(out, string, sizeof(string) - 1)

static int render_row(Buf *out, const PostView *p, TextView author) {
    return buf_post_row(out, p->id, (const char *)p->body.p, p->body.n,
                        (const char *)p->timestamp.p, p->timestamp.n,
                        (const char *)author.p, author.n, p->likes) ? SQLITE_NOMEM : SQLITE_OK;
}

/* SQLite's integer table separators also provide a candidate cell ordinal:
 * on a dense leaf ending at last, the row for k is nCell-1-(last-k). Verify
 * the cell's actual rowid before using it. Holes or stale separators use a
 * binary search of that leaf. The same request may retain a parent while
 * joining several authors; the path is released before it returns. */
typedef struct {
    i64 first, last;
    double scale;
    int useful;
} ChildEstimate;

typedef struct {
    MemPage *level[BTCURSOR_MAX_DEPTH];
    ChildEstimate estimate[BTCURSOR_MAX_DEPTH];
    int used;
} TablePath;

static void release_path(TablePath *path) {
    for (int i = 0; i < path->used; ++i) releasePageNotNull(path->level[i]);
}

static inline i64 separator(MemPage *page, int i) {
    return sqlite3VarintValue(findCellPastPtr(page, i));
}

/* Return the first separator >= key. Interpolation supplies the first probe;
 * binary search finishes even with arbitrary signed rowids or uneven leaves. */
static int choose_child(MemPage *page, i64 key, ChildEstimate *est) {
    int end = page->nCell - 1;
    i64 hi = est->useful ? est->last : separator(page, end);
    if (key > hi) return end + 1;
    if (!end) return 0;
    i64 lo = est->useful ? est->first : separator(page, 0);
    if (key <= lo) return 0;
    int low = 1, high = end;
    if (end >= 8 && hi > lo) {
        /* The same parent serves many author joins inside this one feed.
         * Compute its interpolation slope on the first visit; this never
         * survives the request and never establishes an answer by itself. */
        if (!est->useful) {
            est->first = lo;
            est->last = hi;
            est->scale = (double)end / (double)((u64)hi - (u64)lo);
            est->useful = 1;
        }
        int guess = (int)((double)((u64)key - (u64)lo) * est->scale);
        if (guess < low) guess = low;
        if (guess > high) guess = high;
        if (separator(page, guess) < key) {
            low = guess + 1;
            int limit = end - low < 3 ? end : low + 3;
            while (low <= limit) {
                if (separator(page, low) >= key) return low;
                ++low;
            }
        } else {
            high = guess;
            int limit = high < 4 ? 0 : high - 4;
            while (high > limit) {
                if (separator(page, high - 1) < key) return high;
                --high;
            }
        }
    }
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (separator(page, mid) < key) low = mid + 1; else high = mid;
    }
    return low;
}

static int table_cell(TablePath *path, const ReadTree *tree, i64 key, CellInfo *cell) {
    Pgno number = tree->root;
    i64 fence = 0;
    int upper = 0, lower = 0;
    for (int depth = 0; depth < BTCURSOR_MAX_DEPTH; ++depth) {
        MemPage *page = depth < path->used ? path->level[depth] : NULL;
        if (!page || page->pgno != number) {
            MemPage *next;
            int rc = getAndInitPage(reader.bt->pBt, number, &next, PAGER_GET_READONLY);
            if (rc != SQLITE_OK) return rc;
            if (page) releasePageNotNull(page); else ++path->used;
            path->level[depth] = page = next;
            path->estimate[depth].useful = 0;
        }
        if (!page->intKey || !page->nCell || page->nOverflow) return READ_FALLBACK;
        if (page->intKeyLeaf) {
            unsigned n = page->nCell;
            if (!upper && !lower) {
                parse_cell(page, n - 1, cell);
                fence = cell->nKey;
                upper = 1;
            }
            u64 delta = upper ? (u64)fence - (u64)key : (u64)key - (u64)fence - 1;
            if (delta < n) {
                unsigned offset = upper ? n - 1 - (unsigned)delta : (unsigned)delta;
                parse_cell(page, offset, cell);
                if (cell->nKey == key) return SQLITE_OK;
            }
            /* Sparse IDs: this is still the correct leaf. */
            unsigned lo = 0, hi = n;
            while (lo < hi) {
                unsigned mid = lo + (hi - lo) / 2;
                parse_cell(page, mid, cell);
                if (cell->nKey == key) return SQLITE_OK;
                if (cell->nKey < key) lo = mid + 1; else hi = mid;
            }
            return READ_FALLBACK;
        }
        int i = choose_child(page, key, &path->estimate[depth]);
        if (i < page->nCell) {
            u8 *entry = findCell(page, i);
            number = get4byte(entry);
            fence = sqlite3VarintValue(entry + 4);
            upper = 1;
            lower = 0;
        } else {
            number = get4byte(page->aData + page->hdrOffset + 8);
            if (!upper) {
                fence = separator(page, i - 1);
                lower = 1;
            }
        }
    }
    return READ_FALLBACK;
}

static int render_with_users(PostView *rows, int count, Buf *out) {
    if (!count) return SQLITE_OK;
    TablePath users;
    users.used = 0;
    int rc = SQLITE_OK;
    for (int i = 0; i < count; ++i) {
        PostView *p = &rows[i];
        /* JOIN skips a post without its author and may pull an older row into
         * the feed. Let the reference query perform the full join in that case. */
        CellInfo info;
        rc = table_cell(&users, &reader.users, p->uid, &info);
        if (rc != SQLITE_OK) break;
        RecordView r;
        TextView name;
        const u8 *u = info.pPayload;
        if (info.nLocal == info.nPayload && info.nLocal >= 4 &&
            u[0] == 4 && u[1] == 0 && u[2] >= 13 && u[2] < 128 && (u[2] & 1) &&
            4u + (unsigned)(u[2] - 13) / 2u <= info.nLocal) {
            name = (TextView){u + 4, (unsigned)(u[2] - 13) / 2u};
        } else if (!record_view(&info, &r) || !skip_field(&r) || !next_text(&r, &name)) {
            rc = READ_FALLBACK;
            break;
        }
        if (i && READ_LIT(out, ",")) { rc = SQLITE_NOMEM; break; }
        rc = render_row(out, p, name);
        if (rc != SQLITE_OK) break;
    }
    release_path(&users);
    return rc;
}

static TextView sql_text(sqlite3_stmt *s, int col) {
    const u8 *p = sqlite3_column_text(s, col);
    return (TextView){p, (unsigned)sqlite3_column_bytes(s, col)};
}

static int sql_read(Buf *out, int feed, i64 id) {
    sqlite3_stmt *s = feed ? reader.sql_feed : reader.sql_post;
    if (!feed) sqlite3_bind_int64(s, 1, id);
    int count = 0;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        PostView p = {.id = sqlite3_column_int64(s, 0),
                      .body = sql_text(s, 1), .timestamp = sql_text(s, 2),
                      .likes = sqlite3_column_int64(s, 4)};
        TextView author = sql_text(s, 3);
        if (!p.body.p || !p.timestamp.p || !author.p) { rc = SQLITE_NOMEM; break; }
        if (count++ && READ_LIT(out, ",")) { rc = SQLITE_NOMEM; break; }
        rc = render_row(out, &p, author);
        if (rc != SQLITE_OK) break;
    }
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return rc;
    return !feed && !count ? SQLITE_NOTFOUND : SQLITE_OK;
}

static int chronological(const void *left, const void *right) {
    const PostView *a = left, *b = right;
    int cmp = text_compare(b->timestamp, a->timestamp);
    return cmp ? cmp : (b->id > a->id) - (b->id < a->id);
}

static int certified_feed(Buf *out) {
    PostView rows[FEED_SIZE];
    DbPage *pages[FEED_SIZE];
    int count = 0, n_pages = 0, ordered = 1;
    int rc = tail_posts(rows, &count, pages, &n_pages, &ordered);
    if (rc == SQLITE_OK && count == FEED_SIZE) {
        i64 boundary;
        rc = rank_twenty(&boundary);
        /* If the worst of 20 distinct candidates is the global 20th row, the
         * other nineteen exhaust all nineteen ranks above it. Their original
         * order and the distribution of IDs do not affect this argument. */
        int worst = count - 1;
        if (!ordered) {
            for (int i = 0; i < count - 1; ++i)
                if (chronological(&rows[worst], &rows[i]) < 0) worst = i;
        }
        if (rc == SQLITE_OK && rows[worst].id != boundary) rc = READ_FALLBACK;
    }
    /* The likes merge wants rowid order; the response wants chronology. */
    if (rc == SQLITE_OK) rc = read_likes(rows, count);
    if (rc == SQLITE_OK && !ordered) qsort(rows, count, sizeof rows[0], chronological);
    if (rc == SQLITE_OK) rc = render_with_users(rows, count, out);
    for (int i = 0; i < n_pages; ++i) sqlite3PagerUnrefNotNull(pages[i]);
    return rc;
}

static int direct_post(i64 id, Buf *out) {
    TablePath path;
    path.used = 0;
    CellInfo cell;
    int rc = table_cell(&path, &reader.posts, id, &cell);
    if (rc == SQLITE_OK) {
        PostView p;
        if (!post_view(&cell, &p)) rc = READ_FALLBACK;
        if (rc == SQLITE_OK) rc = read_likes(&p, 1);
        if (rc == SQLITE_OK) rc = render_with_users(&p, 1, out);
    }
    release_path(&path);
    return rc;
}

static int read_response(Buf *out, int feed, i64 id) {
    size_t original = out->len;
    if (feed ? READ_LIT(out, "{\"posts\":[") : READ_LIT(out, "{\"post\":")) return SQLITE_NOMEM;
    size_t mark = out->len;
    int rc = READ_FALLBACK;
    if (reader.fast) {
        if (sqlite3_get_autocommit(reader.db)) rc = SQLITE_MISUSE;
        else rc = sqlite3BtreeBeginTrans(reader.bt, 0, 0);
        if (rc == SQLITE_OK) rc = feed ? certified_feed(out) : direct_post(id, out);
    }
    if (rc == READ_FALLBACK) {
        out->len = mark;
        rc = sql_read(out, feed, id);
    }
    if (rc == SQLITE_OK && (feed ? READ_LIT(out, "]}") : READ_LIT(out, "}"))) rc = SQLITE_NOMEM;
    if (rc != SQLITE_OK) out->len = original;
    return rc;
}

int reader_feed(Buf *out) { return read_response(out, 1, 0); }
int reader_post(int64_t id, Buf *out) { return read_response(out, 0, id); }
