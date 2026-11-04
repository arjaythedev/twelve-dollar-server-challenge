// Read helpers on SQLite's own B-tree cursor API (see sqlite_bt.c).
// Adapted from submissions/c-iouring-shivanshthapliyal/src/sqlite_bt.h (MIT, this repo).
#ifndef SQLITE_BT_H
#define SQLITE_BT_H
#include <stddef.h>

typedef struct sqlite3 sqlite3;
typedef struct {
  long long id, likes;
  const char *body, *ca, *au;
  int bl, cl, al;
} bt_post;

// Resolves the tables/indexes and their root pages once at startup.
// Returns 0 on success, -1 if the schema is not the expected one (caller then uses SQL only).
int bt_init(sqlite3 *db);
// Opens the four read cursors inside the caller's already-started batch transaction.
// Returns 0 on success. bt_end() must be called before the transaction commits/rolls back.
int bt_begin(void);
void bt_end(void);
// Fills out[0..*n) with the 20 newest posts (feed order). Strings point into buf.
// Returns 0 on success, -1 on any surprise (caller falls back to the SQL feed).
int bt_feed(bt_post *out, int *n, char *buf, size_t cap);
// One post by id: 1 found, 0 no such post (or no author row: the JOIN drops it),
// -1 on any surprise (caller falls back to SQL).
int bt_post_get(long long id, bt_post *out, char *buf, size_t cap);

#endif
