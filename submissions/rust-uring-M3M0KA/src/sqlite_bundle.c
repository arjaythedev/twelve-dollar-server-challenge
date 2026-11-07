/* Keep SQLite's private B-tree API and the application-specific read plan in
 * one translation unit. Public SQLite reads/writes still own transactions,
 * locking, WAL and pages; the fast path only bypasses SQL bytecode for reads. */
#include "../vendor/sqlite3.c"
#include "sqlite_fast.inc"
