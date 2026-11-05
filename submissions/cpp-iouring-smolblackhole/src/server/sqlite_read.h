#ifndef TDS_SQLITE_READ_H
#define TDS_SQLITE_READ_H

#include <sqlite3.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct tds_btree tds_btree;

    /* Best-effort Linux cache eviction on the exclusively owned checkpoint handle
     * Only call after a successful checkpoint; the file and its contents stay unchanged
     * event_capacity is the caller's remaining diagnostic-event capacity, capped at 16 */
    void tds_wal_drop_checkpointed(sqlite3 *db, int frames, unsigned event_capacity);

    typedef struct tds_btree_post
    {
        sqlite3_int64 id;
        const char *body;
        int body_bytes;
        const char *created_at;
        int created_at_bytes;
        const char *author;
        int author_bytes;
        sqlite3_int64 like_count;
    } tds_btree_post;

    /* The connection must outlive the reader and have one exclusive caller. */
    int tds_btree_create(sqlite3 *db, tds_btree **out);
    void tds_btree_destroy(tds_btree *reader);

    /* Call after SQL BEGIN. Always end before SQL COMMIT/ROLLBACK, including errors. */
    int tds_btree_begin(tds_btree *reader);
    void tds_btree_end(tds_btree *reader);

    /* Seek within the current SQL transaction, keeping only the posts cursor open
     * Return ROW, DONE, or an error requiring SQL fallback; end before COMMIT/ROLLBACK */
    int tds_btree_post_exists(tds_btree *reader, sqlite3_int64 id);

    /* Own one fresh read snapshot in autocommit mode with no active SQL or bridge cursors
     * Return ROW, DONE for a missing post/author, or an error requiring SQL fallback
     * Non-ROW output is zeroed and every owned cursor/transaction ends before return
     * Copied UTF-8 text survives until the next bridge call, end, or destroy */
    int tds_btree_get_post(tds_btree *reader, sqlite3_int64 id, tds_btree_post *out);

    /* Return ROW, DONE, or a SQLite error. Other errors require the SQL fallback.
     * Text is UTF-8, need not be terminated, and survives until the next read/end. */
    int tds_btree_feed_next(tds_btree *reader, tds_btree_post *out);

#ifdef __cplusplus
}
#endif

#endif
