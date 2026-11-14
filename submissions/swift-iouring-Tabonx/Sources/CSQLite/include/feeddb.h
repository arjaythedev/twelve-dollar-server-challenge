// The database layer the Swift server calls. db.c compiles the unmodified SQLite amalgamation into
// the same translation unit, so reads can walk SQLite's B-tree cursors directly; Swift decodes the
// record bytes. Writes, BEGIN/COMMIT and /health go through ordinary SQL statements.
#ifndef FEEDDB_H
#define FEEDDB_H
#include <stdint.h>

enum {
    FD_POSTS = 0,     // posts table, keyed by rowid (id)
    FD_USERS = 1,     // users table, keyed by rowid (id)
    FD_FEED_IDX = 2,  // posts_created_at_id_idx: (created_at DESC, id DESC, rowid)
    FD_LIKES_IDX = 3, // likes_post_id_idx: (post_id, rowid)
};

// Opens the database and starts the checkpoint thread. 0 on success, -1 with a message in err.
int fd_open(const char *path, char *err, int n);

// Joins the pass's transaction, starting it (and the cursors) if needed. 0 ok, -1 failed.
int fd_begin(void);
// Ends the pass's transaction. 0 committed (or nothing to commit), -1 rolled back.
int fd_commit(void);
// Between passes: drives the checkpoint thread.
void fd_tick(void);
// 1 while writes must wait for the checkpoint thread to copy the end of the log.
int fd_writes_held(void);

// Table cursor to rowid id: 1 found, 0 absent, -1 error.
int fd_seek(int cursor, int64_t id);
// 1 on the first row, 0 empty, -1 error.
int fd_first(int cursor);
// 1 on the next row, 0 at the end, -1 error.
int fd_next(int cursor);
// The current row's record bytes, valid until the cursor moves. NULL on error.
const uint8_t *fd_payload(int cursor, uint32_t *len);

// Like counts for lo <= post_id <= hi off likes_post_id_idx in one walk: counts[post_id - lo]
// (hi - lo + 1 zeroed slots). 0 ok, -1 error.
int fd_count_likes(int64_t lo, int64_t hi, uint32_t *counts);

// INSERT ... RETURNING id, created_at. 0 ok and fills id and created (up to 63 bytes).
int fd_insert_post(int64_t user_id, const uint8_t *body, int len, int64_t *id, uint8_t *created, int *created_len);
// 1 inserted, 0 already liked, -1 error.
int fd_insert_like(int64_t user_id, int64_t post_id);
// SELECT 1: 0 ok, -1 with SQLite's message in err.
int fd_health(char *err, int n);
// The reference feed query, one row per call: 1 row, 0 done, -1 error.
int fd_feed_sql_step(int64_t *id, const uint8_t **body, int *body_len, const uint8_t **created, int *created_len,
                     const uint8_t **author, int *author_len, int64_t *likes);
#endif
