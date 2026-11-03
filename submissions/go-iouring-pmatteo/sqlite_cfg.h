/* SQLite build options, in one place.
 *
 * build.sh compiles the amalgamation with -include sqlite_cfg.h, and db.go includes this before
 * sqlite3.h, so the header is read with exactly the options the library was built with. Splitting
 * them would be a way to get a silently mismatched ABI: SQLITE_OMIT_DEPRECATED and
 * SQLITE_OMIT_DECLTYPE both change what sqlite3.h declares.
 */
#ifndef GO_IOURING_SQLITE_CFG_H
#define GO_IOURING_SQLITE_CFG_H

/* One thread ever enters SQLite, so every mutex is dead weight. */
#define SQLITE_THREADSAFE 0
/* No implicit sqlite3_initialize on first use; openDB calls it once. */
#define SQLITE_OMIT_AUTOINIT 1
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_DEFAULT_WAL_SYNCHRONOUS 1
/* Double-quoted strings are identifiers only: a typo becomes an error, not a string literal. */
#define SQLITE_DQS 0
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS 1
#define SQLITE_MAX_EXPR_DEPTH 0
#define SQLITE_OMIT_DECLTYPE 1
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_PROGRESS_CALLBACK 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_USE_ALLOCA 1
#define HAVE_FDATASYNC 1

#endif
