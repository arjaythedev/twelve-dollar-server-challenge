#ifndef SHIM_H
#define SHIM_H
#include "sqlite/sqlite3.h"

// One cgo call per query: bind up to two ints and two texts, step, copy column 0 (text) into out.
// Returns the number of bytes the result needs (may exceed cap: caller grows and retries),
// 0 when the statement produced no row, or -rc on an SQLite error.
int shim_query(sqlite3_stmt *s, sqlite3_int64 i1, sqlite3_int64 i2,
               const char *t1, int n1, const char *t2, int n2,
               unsigned char *out, int cap);

// Steps a statement without result columns to completion. Returns sqlite3_changes() or -rc.
int shim_exec(sqlite3_stmt *s, sqlite3_int64 i1, sqlite3_int64 i2);

// Frames in the WAL after the last commit (from sqlite3_wal_hook); 0 after a successful checkpoint.
int shim_wal_frames(void);
void shim_install_wal_hook(sqlite3 *db);
int shim_checkpoint(sqlite3 *db);

// Flushes gcc profile counters when built with -DSHIM_GCOV; Go's os.Exit skips atexit handlers.
void shim_gcov_dump(void);

#endif
