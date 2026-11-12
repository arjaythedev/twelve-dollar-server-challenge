/* Test-only control compiled alongside the ordinary reader in one binary.
 * Every component other than the order verification is the same C code. */
#define reader_feed rank_reader_feed
#include "../reader.c"
#undef reader_feed

int bench_all_keys;

static int matches_id(MemPage *page, unsigned i, i64 expected) {
    CellInfo cell;
    parse_cell(page, i, &cell);
    const u8 *p = cell.pPayload;
    /* Common index format: four-byte record header and a 24-bit post ID.
     * Decode directly for a competitive full-index control. */
    if (cell.nLocal == cell.nPayload && cell.nLocal >= 4 && p[0] == 4 &&
        p[1] >= 13 && (p[1] & 1) && p[1] < 128 && p[2] == 3) {
        unsigned offset = 4 + (p[1] - 13) / 2;
        if (offset + 3 <= cell.nLocal) {
            p += offset;
            int32_t v = (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8) >> 8;
            return v == expected;
        }
    }
    RecordView record;
    i64 id;
    return record_view(&cell, &record) && skip_field(&record) && next_integer(&record, &id) && id == expected;
}

static int verify_every_id(const PostView *rows, int count) {
    BtCursor c;
    int rc = read_cursor(&c, &reader.time);
    if (rc != SQLITE_OK) return rc;
    int empty;
    rc = sqlite3BtreeFirst(&c, &empty);
    if (rc == SQLITE_OK && empty != (count == 0)) rc = READ_FALLBACK;
    if (rc == SQLITE_OK && count) {
        if (c.pPage->leaf && c.pPage->nCell >= count) {
            for (int i = 0; i < count; ++i)
                if (!matches_id(c.pPage, i, rows[i].id)) { rc = READ_FALLBACK; break; }
        } else {
            for (int i = 0; i < count; ++i) {
                if (!matches_id(c.pPage, c.ix, rows[i].id)) { rc = READ_FALLBACK; break; }
                if (i + 1 == count) break;
                rc = sqlite3BtreeNext(&c, 0);
                if (rc != SQLITE_OK) break;
            }
        }
    }
    sqlite3BtreeCloseCursor(&c);
    return rc;
}

/* The test binary defines both plans in the same translation unit so its
 * compiler can perform the same link-time optimizations on either plan. */
int reader_feed(Buf *out) {
    if (!bench_all_keys) return rank_reader_feed(out);
    size_t original = out->len;
    if (READ_LIT(out, "{\"posts\":[")) return SQLITE_NOMEM;
    size_t mark = out->len;
    int rc = READ_FALLBACK;
    if (reader.fast) {
        rc = sqlite3_get_autocommit(reader.db) ? SQLITE_MISUSE : sqlite3BtreeBeginTrans(reader.bt, 0, 0);
        PostView rows[FEED_SIZE];
        DbPage *pages[FEED_SIZE];
        int count = 0, n_pages = 0, compare_timestamps = 0;
        if (rc == SQLITE_OK) rc = tail_posts(rows, &count, pages, &n_pages, &compare_timestamps);
        if (rc == SQLITE_OK) rc = verify_every_id(rows, count);
        if (rc == SQLITE_OK) rc = read_likes(rows, count);
        if (rc == SQLITE_OK) rc = render_with_users(rows, count, out);
        for (int i = 0; i < n_pages; ++i) sqlite3PagerUnrefNotNull(pages[i]);
    }
    if (rc == READ_FALLBACK) { out->len = mark; rc = sql_read(out, 1, 0); }
    if (rc == SQLITE_OK && READ_LIT(out, "]}")) rc = SQLITE_NOMEM;
    if (rc != SQLITE_OK) out->len = original;
    return rc;
}
