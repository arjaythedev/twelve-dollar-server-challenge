#ifndef CFEED_H
#define CFEED_H
#include <stddef.h>
#include <stdint.h>

/* Public SQLite C API declarations used by this helper. The implementation is
 * supplied once, by the pinned mattn/go-sqlite3 amalgamation in this package. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int sqlite3_close(sqlite3 *);
int sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
void sqlite3_free(void *);
int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int sqlite3_bind_int64(sqlite3_stmt *, int, int64_t);
int sqlite3_clear_bindings(sqlite3_stmt *);
int sqlite3_step(sqlite3_stmt *);
int sqlite3_reset(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
int sqlite3_column_type(sqlite3_stmt *, int);
int64_t sqlite3_column_int64(sqlite3_stmt *, int);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
int sqlite3_column_bytes(sqlite3_stmt *, int);
int sqlite3_libversion_number(void);
int sqlite3_errcode(sqlite3 *);
const char *sqlite3_errmsg(sqlite3 *);

typedef struct cf_reader cf_reader;
enum { CF_OK=0, CF_NOT_FOUND=1, CF_TOO_SMALL=2, CF_ERROR=3 };
cf_reader *cf_open(const char *path, const char *feed_sql, const char *post_sql);
void cf_close(cf_reader *reader);
int cf_read(cf_reader *reader, int feed, int64_t id, char *out, size_t capacity, size_t *used);
size_t cf_quote(const char *input, size_t length, char *out, size_t capacity);
#endif
