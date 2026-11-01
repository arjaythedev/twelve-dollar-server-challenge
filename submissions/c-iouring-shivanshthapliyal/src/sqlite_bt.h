// Read helpers on SQLite's own B-tree cursor API (see sqlite_bt.c).
#ifndef SQLITE_BT_H
#define SQLITE_BT_H
#include <stddef.h>

typedef struct sqlite3 sqlite3;
typedef struct {
  long long id, likes;
  const char *body, *ca, *au;
  int bl, cl, al;
} bt_post;

int bt_init(sqlite3 *db);
int bt_begin(void);
void bt_end(void);
int bt_feed(bt_post *out, int *n, char *buf, size_t cap);
int bt_post_get(long long id, bt_post *out, char *buf, size_t cap);

#endif
