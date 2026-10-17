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

    sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    sqlite3_stmt *s_u = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1, &s_u, NULL);
    for (int i = 1; i <= 200; i++) {
        char name[32];
        snprintf(name, sizeof(name), "user_%d", i);
        sqlite3_bind_int64(s_u, 1, i);
        sqlite3_bind_text(s_u, 2, name, -1, SQLITE_TRANSIENT);
        sqlite3_step(s_u);
        sqlite3_reset(s_u);
    }
    sqlite3_finalize(s_u);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);

    const char *pragmas =
        "PRAGMA locking_mode = EXCLUSIVE;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA foreign_keys = ON;"
        "PRAGMA mmap_size = 1073741824;"
        "PRAGMA cache_size = -2000;"
        "PRAGMA temp_store = MEMORY;";

    char *err = NULL;
    db_ctx *ctx = db_init(argv[1], pragmas, 64, 65536, 1048576, &err);
    if (!ctx) return 1;

    // Seed 2000 posts and 4000 likes through db_exec_batch to PGO-train write paths
    for (int b = 0; b < 100; b++) {
        int pos = 0;
        for (int i = 0; i < 20; i++) {
            int id = b * 20 + i + 1;
            int body_len = snprintf(ctx->req_buf + pos, 128, "synthetic training post body %d for profile guided optimization", id);
            int toff = pos; pos += body_len;
            int user_len = snprintf(ctx->req_buf + pos, 32, "user_%d", (id % 200) + 1);
            int uoff = pos; pos += user_len;
            ctx->ops[i] = (db_op){
                .kind = OP_CREATE_POST,
                .arg1 = (id % 200) + 1,
                .text_off = toff,
                .text_len = body_len,
                .user_off = uoff,
                .user_len = user_len,
            };
        }
        db_exec_batch(ctx, 20, 10);
    }

    for (int b = 0; b < 200; b++) {
        for (int i = 0; i < 20; i++) {
            int k = b * 20 + i;
            ctx->ops[i] = (db_op){
                .kind = OP_LIKE_POST,
                .arg1 = (k % 200) + 1,
                .arg2 = 2000 - (k % 20),
            };
        }
        db_exec_batch(ctx, 20, 10);
    }

    // Train read and mixed batches through db_exec_batch
    for (int i = 0; i < 2000; i++) {
        ctx->ops[0] = (db_op){ .kind = OP_GET_POST, .arg1 = 2000 - (i % 20) };
        db_exec_batch(ctx, 1, 10);

        for (int j = 0; j < 8; j++) {
            ctx->ops[j] = (db_op){ .kind = (j % 2 == 0) ? OP_FEED : OP_GET_POST, .arg1 = 2000 - (j % 20) };
        }
        db_exec_batch(ctx, 8, 10);
        db_checkpoint_if_due(ctx);
    }

    sqlite3_close(ctx->db);
    return 0;
}
