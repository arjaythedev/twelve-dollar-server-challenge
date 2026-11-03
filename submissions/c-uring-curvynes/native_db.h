#ifndef CURVYNES_NATIVE_DB_H
#define CURVYNES_NATIVE_DB_H
#include "sqlite3.h"
#include <stddef.h>
#include <stdint.h>

typedef struct NativePlan NativePlan;
typedef struct {
    sqlite3_int64 id;
    const char *body, *created_at, *author;
    size_t body_len, created_at_len, author_len;
    uint64_t like_count;
} NativePost;
typedef void (*NativePostSink)(void *, const NativePost *);

NativePlan *native_plan_open(sqlite3 *db);
void native_plan_close(NativePlan *plan);
/* All rows are read inside the caller's SQLite transaction. Callback views
   expire when the callback returns; no row data survives this function. */
int native_read_posts(NativePlan *plan, const sqlite3_int64 *ids, unsigned count,
                      NativePostSink sink, void *context);
#endif
