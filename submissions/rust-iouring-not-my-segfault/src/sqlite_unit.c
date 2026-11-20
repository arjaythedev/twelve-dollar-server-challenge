/* One translation unit: SQLite's pinned amalgamation and the modules that use its internals. */
#include "sqlite3.c"
#if SQLITE_VERSION_NUMBER != 3054001
#error "twelve: SQLite 3.54.1 required"
#endif
/* The amalgamation defines NDEBUG when neither NDEBUG nor SQLITE_DEBUG is set, which turns assert() off; undefine it and include assert.h again so readers.c and checkpointer.c keep their assertions in every build. */
#undef NDEBUG
#include <assert.h>
#include "readers.c"
#include "checkpointer.c"
