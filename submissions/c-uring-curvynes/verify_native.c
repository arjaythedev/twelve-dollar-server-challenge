/* Differential test executable, never linked into the server. */
#include "native_db.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    NativePost rows[20];
    char text[20][3][8192];
    unsigned count;
} Collected;
static void collect(void *context, const NativePost *row) {
    Collected *result = context;
    assert(result->count < 20);
    unsigned i = result->count++;
    assert(row->body_len <= sizeof(result->text[i][0]));
    assert(row->created_at_len <= sizeof(result->text[i][1]));
    assert(row->author_len <= sizeof(result->text[i][2]));
    result->rows[i] = *row;
    memcpy(result->text[i][0], row->body, row->body_len);
    memcpy(result->text[i][1], row->created_at, row->created_at_len);
    memcpy(result->text[i][2], row->author, row->author_len);
    result->rows[i].body = result->text[i][0];
    result->rows[i].created_at = result->text[i][1];
    result->rows[i].author = result->text[i][2];
}
static void compare(sqlite3_stmt *reference, const NativePost *row) {
    sqlite3_bind_int64(reference, 1, row->id);
    assert(sqlite3_step(reference) == SQLITE_ROW);
    assert(sqlite3_column_int64(reference, 0) == row->id);
    const char *values[] = {row->body, row->created_at, row->author};
    size_t lengths[] = {row->body_len, row->created_at_len, row->author_len};
    for (int i = 0; i < 3; i++) {
        assert(lengths[i] == (size_t)sqlite3_column_bytes(reference, i + 1));
        assert(!memcmp(values[i], sqlite3_column_text(reference, i + 1), lengths[i]));
    }
    assert(row->like_count == (uint64_t)sqlite3_column_int64(reference, 4));
    assert(sqlite3_step(reference) == SQLITE_DONE);
    assert(sqlite3_reset(reference) == SQLITE_OK);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    sqlite3 *db = NULL;
    assert(sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=536870912", NULL, NULL, NULL) == SQLITE_OK);
    NativePlan *plan = native_plan_open(db); assert(plan);
    sqlite3_stmt *reference = NULL;
    const char *query = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1";
    assert(sqlite3_prepare_v2(db, query, -1, &reference, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    Collected result; uint64_t random = 123456789;
    for (unsigned iteration = 0; iteration < 20000; iteration++) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        sqlite3_int64 id = (sqlite3_int64)(random % 500000) + 1;
        result.count = 0;
        assert(native_read_posts(plan, &id, 1, collect, &result) == SQLITE_OK);
        assert(result.count == 1); compare(reference, &result.rows[0]);
    }
    for (unsigned pattern = 0; pattern < 3; pattern++)
    for (unsigned iteration = 0; iteration < 100; iteration++) {
        sqlite3_int64 ids[20];
        for (unsigned i = 0; i < 20; i++) {
            unsigned position = pattern == 0 ? i : pattern == 1 ? 19 - i : (i * 7) % 20;
            ids[i] = 499900 - (sqlite3_int64)(position * 3) - (sqlite3_int64)iteration;
        }
        result.count = 0;
        assert(native_read_posts(plan, ids, 20, collect, &result) == SQLITE_OK);
        assert(result.count == 20);
        for (unsigned i = 0; i < result.count; i++) { assert(result.rows[i].id == ids[i]); compare(reference, &result.rows[i]); }
    }
    sqlite3_int64 missing = 999999999, wide[] = {1,500000};
    result.count = 0; assert(native_read_posts(plan, &missing, 1, collect, &result) == SQLITE_NOTFOUND); assert(!result.count);
    assert(native_read_posts(plan, wide, 2, collect, &result) == SQLITE_MISMATCH); assert(!result.count);
    assert(sqlite3_exec(db, "UPDATE users SET username=printf('%06000d',1) WHERE id=1", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *pick;
    assert(sqlite3_prepare_v2(db, "SELECT id FROM posts WHERE user_id=1 LIMIT 1", -1, &pick, NULL) == SQLITE_OK);
    assert(sqlite3_step(pick) == SQLITE_ROW); sqlite3_int64 overflow_id = sqlite3_column_int64(pick, 0); sqlite3_finalize(pick);
    result.count = 0; assert(native_read_posts(plan, &overflow_id, 1, collect, &result) == SQLITE_OK); compare(reference, &result.rows[0]);
    sqlite3_stmt *update;
    assert(sqlite3_prepare_v2(db, "UPDATE posts SET created_at=printf('%06000d',2) WHERE id=?1", -1, &update, NULL) == SQLITE_OK);
    sqlite3_bind_int64(update, 1, overflow_id); assert(sqlite3_step(update) == SQLITE_DONE); sqlite3_finalize(update);
    result.count = 0; assert(native_read_posts(plan, &overflow_id, 1, collect, &result) == SQLITE_OK); compare(reference, &result.rows[0]);
    assert(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_finalize(reference); native_plan_close(plan); assert(sqlite3_close(db) == SQLITE_OK);
    puts("PASS: 20000 native post reads and 300 descending/ascending/shuffled sparse groups match public SQL");
    puts("PASS: missing/wide-range cases and overflow-page records; no test mutations committed");
}
