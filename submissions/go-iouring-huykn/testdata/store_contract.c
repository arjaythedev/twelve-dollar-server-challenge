// White-box contract checks: compile with sqlite_engine.c, never into the server.
#include "../store.c"
#include <assert.h>
#include <fcntl.h>
#include <time.h>

static int allocation_calls;
static int count_allocation(int fd, off_t offset, off_t length) {
  ++allocation_calls;
  return posix_fallocate(fd, offset, length);
}

static int reject_commit(void *unused, int action, const char *arg,
                         const char *b, const char *c, const char *d) {
  (void)unused; (void)b; (void)c; (void)d;
  return action == SQLITE_TRANSACTION && strcmp(arg, "COMMIT") == 0
         ? SQLITE_DENY : SQLITE_OK;
}

static int scalar(sqlite3 *db, const char *sql) {
  sqlite3_stmt *st = NULL;
  assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
  assert(sqlite3_step(st) == SQLITE_ROW);
  int n = sqlite3_column_int(st, 0);
  assert(sqlite3_finalize(st) == SQLITE_OK);
  return n;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  char message[256];
  hkn_store *s = hkn_connect(argv[1], message, sizeof message);
  if (!s) { fprintf(stderr, "%s\n", message); return 1; }
  if (!getenv("HKN_SQL_READS")) assert(s->reader);
  sqlite3 *observer = NULL;
  assert(sqlite3_open_v2(argv[1], &observer, SQLITE_OPEN_READONLY, getenv("HKN_VFS") ? getenv("HKN_VFS") : "unix-excl") == SQLITE_OK);
  assert(scalar(s->connection, "PRAGMA synchronous") == 1);
  assert(scalar(s->connection, "PRAGMA wal_autocheckpoint") == (s->checkpoint ? 0 : 8192));
  // Exercise SQLite's VFS syscall slot: chunk growth must use real allocation,
  // not the fallback that writes one byte per filesystem block.
  sqlite3_vfs *vfs = sqlite3_vfs_find(getenv("HKN_VFS") ? getenv("HKN_VFS") : "unix-excl");
  sqlite3_syscall_ptr allocate = vfs->xGetSystemCall(vfs, "fallocate");
  assert(allocate);
  assert(vfs->xSetSystemCall(vfs, "fallocate", (sqlite3_syscall_ptr)count_allocation) == SQLITE_OK);
  sqlite3_int64 hint = ((sqlite3_int64)scalar(s->connection, "PRAGMA page_count") + 16384) *
                       scalar(s->connection, "PRAGMA page_size");
  assert(sqlite3_file_control(s->connection, "main", SQLITE_FCNTL_SIZE_HINT, &hint) == SQLITE_OK);
  assert(allocation_calls > 0);
  assert(vfs->xSetSystemCall(vfs, "fallocate", allocate) == SQLITE_OK);
  const hkn_result *initial_feed = hkn_read(s, 0, 0);
  assert(initial_feed && initial_feed->length == 20);
  if (s->reader) assert(sqlite3_stmt_status(s->statement[RECENT], SQLITE_STMTSTATUS_VM_STEP, 0) == 0);
  assert(hkn_finish(s) == 0);
  int before = scalar(observer, "SELECT count(*) FROM posts");
  const hkn_result *r = hkn_create(s, 1, "transaction test", 16);
  assert(r && r->length == 1);
  int64_t id = r->items[0].key;
  assert(scalar(observer, "SELECT count(*) FROM posts") == before);
  assert(hkn_read(s, 1, id)->length == 1);
  if (s->reader) assert(sqlite3_stmt_status(s->statement[DETAIL], SQLITE_STMTSTATUS_VM_STEP, 0) == 0); // Read our own batch's writes.
  assert(hkn_finish(s) == 0);
  assert(scalar(observer, "SELECT count(*) FROM posts") == before + 1);
  assert(hkn_vote(s, 1, id) == 201);
  assert(hkn_vote(s, 1, id) == 200);
  assert(hkn_vote(s, 1, INT64_MAX) == 404);
  assert(hkn_finish(s) == 0);
  // The copied column bytes remain valid after SQLite resets the statement.
  r = hkn_read(s, 1, id);
  assert(r && r->items[0].count == 1);
  assert(r->items[0].content.size == 16);
  assert(memcmp(r->bytes+r->items[0].content.at, "transaction test", 16) == 0);
  // Public authorizer API denies COMMIT; the adapter must rollback all writes.
  assert(hkn_create(s, 1, "must roll back", 14));
  assert(sqlite3_set_authorizer(s->connection, reject_commit, NULL) == SQLITE_OK);
  assert(hkn_finish(s) == -1);
  assert(sqlite3_get_autocommit(s->connection));
  assert(scalar(observer, "SELECT count(*) FROM posts") == before + 1);
  assert(sqlite3_set_authorizer(s->connection, NULL, NULL) == SQLITE_OK);
  // Connection can process a new batch after a failed commit.
  assert(hkn_create(s, 1, "recovery", 8));
  assert(hkn_finish(s) == 0);
  assert(scalar(observer, "SELECT count(*) FROM posts") == before + 2);
  // Simulate SQLite automatically rolling back an earlier staged write.
  assert(hkn_create(s, 1, "implicit rollback", 17));
  assert(sqlite3_exec(s->connection, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
  assert(failed(s, SQLITE_IOERR) == -1);
  assert(hkn_read(s, 0, 0) == NULL);
  assert(hkn_finish(s) == -1);
  assert(scalar(observer, "SELECT count(*) FROM posts") == before + 2);
  assert(hkn_probe(s) == 0);
  // Rebinding these parameters must not cause a query-plan recompile per read.
  for (int i = 0; i < 10; ++i) assert(hkn_read(s, 1, id));
  assert(sqlite3_stmt_status(s->statement[DETAIL], SQLITE_STMTSTATUS_REPREPARE, 0) <= 2);
  assert(hkn_finish(s) == 0);
  assert(sqlite3_exec(s->connection, "UPDATE users SET username='fresh author' WHERE id=1", NULL, NULL, NULL) == SQLITE_OK);
  r = hkn_read(s, 1, id);
  assert(r && r->items[0].username.size == 12);
  assert(memcmp(r->bytes+r->items[0].username.at, "fresh author", 12) == 0);
  assert(hkn_finish(s) == 0);
  if (s->checkpoint) {
    // A live reader's snapshot must survive a background passive checkpoint.
    assert(sqlite3_exec(observer, "BEGIN;SELECT count(*) FROM posts", NULL, NULL, NULL) == SQLITE_OK);
    int snapshot = scalar(observer, "SELECT count(*) FROM posts");
    assert(hkn_create(s, 1, "snapshot test", 13));
    assert(hkn_finish(s) == 0);
    assert(hkn_checkpoint_request(s->checkpoint) == SQLITE_OK);
    // SQLITE_OK from PASSIVE does not imply that the old reader let it finish.
    for (int attempt = 0; attempt < 1000 && hkn_checkpoint_interval(s->checkpoint) != 2048; ++attempt) {
      struct timespec pause = {.tv_nsec = 1000000};
      nanosleep(&pause, NULL);
    }
    assert(hkn_checkpoint_interval(s->checkpoint) == 2048);
    assert(hkn_checkpoint_wait(s->checkpoint) == SQLITE_OK);
    assert(hkn_checkpoint_interval(s->checkpoint) >= 8192);
    s->checkpoint_phase = 1;
    assert(hkn_tick(s, 1000000000) == 1);
    assert(hkn_checkpoint_wait(s->checkpoint) == SQLITE_OK);
    assert(hkn_checkpoint_result(s->checkpoint) == 1);
    assert(hkn_tick(s, 1000000001) == 0);
    assert(s->wal_frames > 0); // Incomplete PASSIVE must not claim a reset.
    assert(scalar(observer, "SELECT count(*) FROM posts") == snapshot);
    assert(sqlite3_exec(observer, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    // A complete tail releases the hold; SQLite rewinds on its next write.
    assert(hkn_checkpoint_request(s->checkpoint) == SQLITE_OK);
    s->checkpoint_phase = 1;
    assert(hkn_checkpoint_wait(s->checkpoint) == SQLITE_OK);
    assert(hkn_tick(s, 2000000000) == 1);
    assert(hkn_checkpoint_wait(s->checkpoint) == SQLITE_OK);
    assert(hkn_tick(s, 2000000001) == 0 && s->wal_frames == 0);
    // An uncompleted worker result exercises the deadline deterministically.
    hkn_checkpoint *original = s->checkpoint;
    s->checkpoint = hkn_checkpoint_new(argv[1], getenv("HKN_VFS") ? getenv("HKN_VFS") : "unix-excl");
    assert(s->checkpoint);
    s->checkpoint_phase = 2;
    s->holding = 1;
    s->hold_started = 3000000000;
    s->tail_written = s->wal_written;
    assert(hkn_tick(s, 3249999999) == 1);
    assert(hkn_tick(s, 3250000000) == 0 && s->checkpoint_phase == 2);
    assert(hkn_create(s, 1, "after timeout", 13));
    assert(hkn_finish(s) == 0 && s->wal_written > s->tail_written);
    assert(hkn_checkpoint_tail(s->checkpoint) == SQLITE_OK);
    assert(hkn_checkpoint_wait(s->checkpoint) == SQLITE_OK);
    assert(hkn_tick(s, 3250000001) == 0 && s->wal_frames > 0);
    hkn_checkpoint_free(s->checkpoint);
    s->checkpoint = original;
    // Drive the growth-barrier branch without writing 256 MiB in a unit test.
    assert(begin_batch(s) == 0);
    s->checkpoint_sent = s->wal_written;
    s->wal_frames = 65536;
    assert(hkn_finish(s) == 0);
    assert(s->wal_frames == 65536); // COMMIT never waits for a checkpoint.
    assert(hkn_tick(s, 4000000000) == 0);
    assert(s->wal_frames == 0 && s->checkpoint_sent == s->wal_written);
    // WAL rewinds must not erase observed write pressure or underflow budgets.
    for (int cycle = 0; cycle < 3; ++cycle) {
      uint64_t written = s->wal_written;
      assert(hkn_create(s, 1, "rewind test", 11));
      assert(hkn_finish(s) == 0 && s->wal_written > written);
      assert(hkn_checkpoint_request(s->checkpoint) == SQLITE_OK);
      assert(hkn_checkpoint_wait(s->checkpoint) == SQLITE_OK);
      assert(sqlite3_wal_checkpoint_v2(s->connection, "main", SQLITE_CHECKPOINT_RESTART, NULL, NULL) == SQLITE_OK);
    }
  }
  // A changed schema cookie must switch the old read plan to SQL, not reuse
  // its cached root-page assumptions. This DDL is fixture-only.
  assert(sqlite3_exec(s->connection, "CREATE INDEX contract_schema_change ON users(created_at)", NULL, NULL, NULL) == SQLITE_OK);
  assert(hkn_read(s, 1, id));
  assert(sqlite3_stmt_status(s->statement[DETAIL], SQLITE_STMTSTATUS_VM_STEP, 0) > 0);
  assert(hkn_finish(s) == 0);
  sqlite3_close(observer);
  hkn_disconnect(s);
  puts("store contracts passed");
  return 0;
}
