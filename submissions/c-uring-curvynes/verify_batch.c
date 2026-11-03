/* API-level transaction tests, including rollback fault injection. This is
   compiled separately from the server. Including api.c here gives the test
   access to its private SQLite handle without a production testing endpoint. */
#include "api.c"
#include <assert.h>

static Reply call(const char *method, const char *path, const char *auth, const char *body) {
    Reply r = {0};
    api_handle((Slice){method, strlen(method)}, (Slice){path, strlen(path)},
               (Slice){auth, strlen(auth)}, (Slice){body, strlen(body)}, &r);
    return r;
}
static uint64_t post_id(Reply *r) {
    yyjson_doc *doc = yyjson_read(r->b.p + 256, r->b.len - 256, 0);
    assert(doc);
    uint64_t id = yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "post"), "id"));
    yyjson_doc_free(doc); return id;
}
static uint64_t like_count(Reply *r) {
    yyjson_doc *doc = yyjson_read(r->b.p + 256, r->b.len - 256, 0);
    assert(doc);
    uint64_t n = yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "post"), "like_count"));
    yyjson_doc_free(doc); return n;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    assert(api_init(argv[1], "twelve-dollar-challenge") == 0);
    char authorization[2048]; snprintf(authorization, sizeof(authorization), "Bearer %s", argv[2]);
    Reply created = call("POST", "/posts", authorization, "{\"body\":\"batch isolation\"}");
    assert(created.status == 201 && created.pending_commit);
    uint64_t id = post_id(&created); free(created.b.p);
    char path[64]; snprintf(path, sizeof(path), "/posts/%llu", (unsigned long long)id);
    Reply before = call("GET", path, "", "");
    assert(before.status == 200 && before.pending_commit); free(before.b.p);
    sqlite3 *observer; sqlite3_stmt *observation;
    assert(sqlite3_open_v2(argv[1], &observer, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(observer, "SELECT count(*) FROM posts WHERE id=?1", -1, &observation, NULL) == SQLITE_OK);
    sqlite3_bind_int64(observation, 1, (sqlite3_int64)id);
    assert(sqlite3_step(observation) == SQLITE_ROW && sqlite3_column_int(observation, 0) == 0);
    sqlite3_reset(observation);
    assert(api_commit_batch() == 0);
    Reply after = call("GET", path, "", ""); assert(after.status == 200 && like_count(&after) == 0); free(after.b.p);
    assert(sqlite3_step(observation) == SQLITE_ROW && sqlite3_column_int(observation, 0) == 1);
    sqlite3_finalize(observation); sqlite3_close(observer);
    assert(api_commit_batch() == 0);
    puts("PASS: staged reads held at response barrier; external observer sees only committed post");
    char like_path[80]; snprintf(like_path, sizeof(like_path), "%s/like", path);
    Reply liked = call("POST", like_path, authorization, ""); assert(liked.status == 201 && liked.pending_commit); free(liked.b.p);
    before = call("GET", path, "", ""); assert(before.status == 200 && before.pending_commit && like_count(&before) == 1); free(before.b.p);
    assert(api_commit_batch() == 0);
    after = call("GET", path, "", ""); assert(after.status == 200 && like_count(&after) == 1); free(after.b.p);
    assert(api_commit_batch() == 0);
    puts("PASS: read of batched like is held until its COMMIT");
    Reply rollback = call("POST", "/posts", authorization, "{\"body\":\"must roll back\"}");
    assert(rollback.status == 201 && rollback.pending_commit);
    uint64_t rollback_id = post_id(&rollback); free(rollback.b.p);
    /* Emulate a SQLite transaction being aborted before the response barrier. */
    assert(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    assert(api_commit_batch() == -1);
    snprintf(path, sizeof(path), "/posts/%llu", (unsigned long long)rollback_id);
    after = call("GET", path, "", ""); assert(after.status == 404 && after.pending_commit); free(after.b.p);
    assert(api_commit_batch() == 0);
    puts("PASS: rollback is reported as commit failure and leaves no visible row");
    Reply recovery = call("POST", "/posts", authorization, "{\"body\":\"after rollback\"}");
    assert(recovery.status == 201 && recovery.pending_commit); free(recovery.b.p);
    assert(api_commit_batch() == 0);
    api_shutdown();
    puts("PASS: successful transaction after aborted batch");
    return 0;
}
