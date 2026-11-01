// SQLite plus the few helpers Swift can't express directly (macros, destructor sentinels, the WAL hook).
#ifndef CSQLITE_H
#define CSQLITE_H
#include "sqlite3.h"

static inline int csqlite_bind_text_static(sqlite3_stmt *s, int i, const void *p, int n) {
    return sqlite3_bind_text(s, i, (const char *)p, n, SQLITE_STATIC);
}

static inline int csqlite_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out) {
    return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, out, 0);
}

static int csqlite_wal_frames_cb(void *p, sqlite3 *db, const char *name, int frames) {
    (void)db; (void)name;
    *(int *)p = frames;
    return SQLITE_OK;
}

// Stores the WAL size in frames into *frames after every commit.
static inline void csqlite_track_wal(sqlite3 *db, int *frames) {
    sqlite3_wal_hook(db, csqlite_wal_frames_cb, frames);
}
#endif
