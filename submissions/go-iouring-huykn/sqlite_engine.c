// SQLite's source is pinned and unmodified. The read plan compiles after it so
// private cursor definitions remain confined to one translation unit.
#include "sqlite/sqlite3.c"
#include "btree_reader.h"
