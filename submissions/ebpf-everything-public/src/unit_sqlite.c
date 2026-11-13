// unit_sqlite.c — one translation unit: the SQLite amalgamation plus engine.c, which #includes sqlite3.c so the
// engine and SQLite compile together and the engine reads SQLite's B-trees directly. Built with build.sh's SQLOPTS.
#include "engine.c"
