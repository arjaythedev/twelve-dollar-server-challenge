#ifndef TDS_SQLITE_READ_H
#define TDS_SQLITE_READ_H

#include <sqlite3.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct tds_btree tds_btree;

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

    /* Return ROW, DONE, or a SQLite error. Other errors require the SQL fallback.
     * Text is UTF-8, need not be terminated, and survives until the next read/end. */
    int tds_btree_feed_next(tds_btree *reader, tds_btree_post *out);

#ifdef __cplusplus
}
#endif

#endif
