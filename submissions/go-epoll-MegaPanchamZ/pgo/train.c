#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shim.h"

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    sqlite3_initialize();
    sqlite3 *db = NULL;
    if (sqlite3_open(argv[1], &db) != SQLITE_OK) return 1;
    if (sqlite3_exec(db, argv[2], NULL, NULL, NULL) != SQLITE_OK) return 1;

    sqlite3_exec(db,
        "BEGIN IMMEDIATE;"
        "WITH RECURSIVE u(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM u WHERE x < 1000) "
        "INSERT INTO users (id, username) SELECT x, 'user_' || x FROM u;"
        "WITH RECURSIVE p(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM p WHERE x < 10000) "
        "INSERT INTO posts (id, user_id, body, created_at) "
        "SELECT x, (x % 1000) + 1, 'synthetic training post body ' || x || ' for profile guided optimization', '2025-12-31T00:00:00.000Z' FROM p;"
        "WITH RECURSIVE l(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM l WHERE x < 40000) "
        "INSERT OR IGNORE INTO likes (user_id, post_id, created_at) "
        "SELECT ((x * 37) % 1000) + 1, ((x * 13) % 10000) + 1, '2025-12-31T00:00:00.000Z' FROM l;"
        "COMMIT;",
        NULL, NULL, NULL);
    sqlite3_close(db);

    const char *pragmas =
        "PRAGMA locking_mode = EXCLUSIVE;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA foreign_keys = OFF;"
        "PRAGMA ignore_check_constraints = ON;"
        "PRAGMA mmap_size = 0;"
        "PRAGMA cache_size = -65536;"
        "PRAGMA temp_store = MEMORY;"
        "PRAGMA journal_size_limit = 67108864;"
        "PRAGMA wal_autocheckpoint = 0;";

    char *err = NULL;
    db_ctx *ctx = db_init(argv[1], pragmas, 64, 65536, 1048576, &err);
    if (!ctx) return 1;

    // Seed 2000 posts (10001..12000) and 4000 likes through db_exec_batch to PGO-train write paths
    int64_t now_ms = 1767225600000LL;
    for (int b = 0; b < 100; b++) {
        int pos = 0;
        for (int i = 0; i < 20; i++) {
            int id = 10000 + b * 20 + i + 1;
            int body_len = snprintf(ctx->req_buf + pos, 128, "synthetic training post body %d for profile guided optimization", id);
            int toff = pos; pos += body_len;
            int user_len = snprintf(ctx->req_buf + pos, 32, "user_%d", (id % 1000) + 1);
            int uoff = pos; pos += user_len;
            ctx->ops[i] = (db_op){
                .kind = OP_CREATE_POST,
                .arg1 = (id % 1000) + 1,
                .text_off = toff,
                .text_len = body_len,
                .user_off = uoff,
                .user_len = user_len,
            };
        }
        db_exec_batch(ctx, 0, 20, now_ms++, 10);
    }

    for (int b = 0; b < 200; b++) {
        for (int i = 0; i < 20; i++) {
            int k = b * 20 + i;
            ctx->ops[i] = (db_op){
                .kind = OP_LIKE_POST,
                .arg1 = ((k / 1000) * 37 + k) % 1000 + 1,
                .arg2 = 12000 - (k % 1000),
            };
        }
        db_exec_batch(ctx, 0, 20, now_ms++, 10);
    }

    // Train singleton, get_post-heavy, and mixed read batches after write seeding
    for (int i = 0; i < 2000; i++) {
        ctx->ops[0] = (db_op){ .kind = OP_GET_POST, .arg1 = 12000 - (i % 20) };
        db_exec_batch(ctx, 0, 1, now_ms, 10);

        for (int j = 0; j < 32; j++) {
            ctx->ops[j] = (db_op){ .kind = OP_GET_POST, .arg1 = 12000 - ((i + j) % 200) };
        }
        db_exec_batch(ctx, 0, 32, now_ms, 10);

        for (int j = 0; j < 8; j++) {
            ctx->ops[j] = (db_op){ .kind = (j % 2 == 0) ? OP_FEED : OP_GET_POST, .arg1 = 12000 - (j % 20) };
        }
        db_exec_batch(ctx, 0, 8, now_ms, 10);
        db_checkpoint_if_due(ctx);
    }

    db_free(ctx);
    return 0;
}
