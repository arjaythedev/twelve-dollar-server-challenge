#ifndef SHIM_H
#define SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <sqlite3.h>

// Preserve the measured baseline. Lower thresholds require an isolated
// throughput and tail-latency comparison before changing the default.
enum { DB_CHECKPOINT_FRAMES = 262144 };

enum {
    OP_STATIC      = 0,
    OP_HEALTH      = 1,
    OP_FEED        = 2,
    OP_GET_POST    = 3,
    OP_CREATE_POST = 4,
    OP_LIKE_POST   = 5
};

enum {
    TX_NONE  = 0,
    TX_READ  = 1,
    TX_WRITE = 2
};

typedef struct {
    uint8_t  kind;
    uint8_t  close_conn;
    uint16_t status;
    int32_t  text_off;
    int32_t  text_len;
    int32_t  user_off;
    int32_t  user_len;
    int64_t  arg1;
    int64_t  arg2;
    int32_t  resp_off;
    int32_t  resp_len;
} db_op;

typedef struct {
    sqlite3      *db;
    sqlite3_stmt *s_health;
    sqlite3_stmt *s_ids;
    sqlite3_stmt *s_range_posts_desc;
    sqlite3_stmt *s_range_likes;
    sqlite3_stmt *s_feed_ref;
    sqlite3_stmt *s_post;
    sqlite3_stmt *s_insert;
    sqlite3_stmt *s_like;
    sqlite3_stmt *s_exists;
    sqlite3_stmt *s_begin_read;
    sqlite3_stmt *s_begin_write;
    sqlite3_stmt *s_commit;
    sqlite3_stmt *s_rollback;

    int           wal_pages;
    int           max_ops;
    int           req_cap;
    int           out_cap;
    int64_t       last_ms;
    char          now_iso[32];
    db_op        *ops;
    char         *req_buf;
    char         *out_buf;
    char          scratch[65536];
} db_ctx;

db_ctx *db_init(const char *path, const char *pragma_sql, int max_ops, int req_cap, int out_cap, char **err_out);
void db_free(db_ctx *ctx);
// Execute a prefix that fits the output arena; commit it before returning its length.
int db_exec_batch(db_ctx *ctx, int first_op, int n_ops, int64_t now_ms, int64_t uptime_s);
int db_checkpoint_if_due(db_ctx *ctx);

#endif
