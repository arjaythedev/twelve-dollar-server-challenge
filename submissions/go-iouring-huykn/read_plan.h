#ifndef HUYKN_READ_PLAN_H
#define HUYKN_READ_PLAN_H
#include "store.h"
#include "sqlite/sqlite3.h"

typedef struct hkn_read_plan hkn_read_plan;
hkn_read_plan *hkn_plan_new(sqlite3 *connection);
void hkn_plan_release(hkn_read_plan *plan);
void hkn_plan_end(hkn_read_plan *plan);
// SQLITE_OK: result is complete. SQLITE_NOTFOUND: execute reference SQL instead.
int hkn_plan_read(hkn_read_plan *plan, int single, int64_t key, hkn_result *result);
#endif
