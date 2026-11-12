#ifndef DB_H
#define DB_H
#include <stddef.h>

#define FEED_N 20

/* One decoded row. The pointers live in the caller's arena (see db_feed/db_post). */
struct row {
    long long id;
    long long user_id;
    const char *body;
    int body_len;
    const char *created;
    int created_len;
    const char *author;
    int author_len;
    long long likes;
};

int db_open(const char *path, char *err, int errlen);

/* One transaction per event-loop pass. db_begin is idempotent inside a pass. */
int db_begin(void);
/* 0 committed, -1 rolled back (SQLite ended the transaction itself, or COMMIT failed). */
int db_commit(void);
/* Between passes, with no transaction open: drives the checkpoint state machine. */
void db_tick(void);
/* Writes must wait: the checkpoint thread is copying the log's last frames. */
int db_writes_held(void);

/* 20 newest posts: the number of rows (0..20) or -1 on unexpected data (caller uses
 * db_feed_sql). The tail/index plans always return 20; the SQL fallback can return fewer when a
 * post's author row is missing, exactly like the reference JOIN. */
int db_feed(struct row *rows, char *arena, size_t cap);
/* Reference SQL query, same output convention. */
int db_feed_sql(struct row *rows, char *arena, size_t cap);

/* 1 found, 0 absent (or no author row, like the JOIN), -1 error. The pointers in *out refer
 * into SQLite's B-tree pages and are valid until the next request moves the cursors; the caller
 * serializes the response before doing any other database work. */
int db_post(long long id, struct row *out);
int db_post_exists(long long id);

/* INSERT ... RETURNING id, created_at. 0 ok. */
int db_insert_post(long long user_id, const char *body, int len, long long *id,
                   char *created, int *created_len);
/* 1 newly inserted, 0 already liked, -1 error. */
int db_insert_like(long long user_id, long long post_id);

/* SELECT 1: 0 ok, -1 error. */
int db_health(void);

#endif
