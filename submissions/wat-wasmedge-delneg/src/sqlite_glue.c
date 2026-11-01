// The only C in this entry besides stock SQLite and wasi-libc: it compiles the pinned amalgamation
// into a WASI reactor module and exports the SQLite calls the hand-written WAT server uses.
// No request logic lives here.
#include "sqlite3.h"
#include <stdlib.h>

#define X(name) __attribute__((export_name(#name)))
X(db_open) int db_open(const char *path, sqlite3 **db) {
  return sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE, "unix-none");
}
X(db_exec) int db_exec(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0); }
X(db_prepare) int db_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **st) {
  return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, 0);
}
X(st_bind_int64) int st_bind_int64(sqlite3_stmt *s, int i, long long v) { return sqlite3_bind_int64(s, i, v); }
X(st_bind_text) int st_bind_text(sqlite3_stmt *s, int i, const char *p, int n) { return sqlite3_bind_text(s, i, p, n, SQLITE_STATIC); }
X(st_step) int st_step(sqlite3_stmt *s) { return sqlite3_step(s); }
X(st_reset) int st_reset(sqlite3_stmt *s) { return sqlite3_reset(s); }
X(st_int64) long long st_int64(sqlite3_stmt *s, int i) { return sqlite3_column_int64(s, i); }
X(st_text) const unsigned char *st_text(sqlite3_stmt *s, int i) { return sqlite3_column_text(s, i); }
X(st_bytes) int st_bytes(sqlite3_stmt *s, int i) { return sqlite3_column_bytes(s, i); }
X(db_changes) int db_changes(sqlite3 *db) { return sqlite3_changes(db); }
X(db_last_rowid) long long db_last_rowid(sqlite3 *db) { return sqlite3_last_insert_rowid(db); }
X(db_errmsg) const char *db_errmsg(sqlite3 *db) { return sqlite3_errmsg(db); }
X(db_checkpoint) int db_checkpoint(sqlite3 *db) { int log = 0, ck = 0; sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, &log, &ck); return log; }
X(env_get) const char *env_get(const char *name) { return getenv(name); }
X(mem_alloc) void *mem_alloc(unsigned long n) { return malloc(n); }
X(mem_free) void mem_free(void *p) { free(p); }
X(mem_realloc) void *mem_realloc(void *p, unsigned long n) { return realloc(p, n); }
