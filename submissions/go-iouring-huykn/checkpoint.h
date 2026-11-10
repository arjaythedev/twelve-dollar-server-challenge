#ifndef HUYKN_CHECKPOINT_H
#define HUYKN_CHECKPOINT_H
#include "sqlite/sqlite3.h"
typedef struct hkn_checkpoint hkn_checkpoint;
hkn_checkpoint *hkn_checkpoint_new(const char *path, const char *vfs);
void hkn_checkpoint_free(hkn_checkpoint *worker);
int hkn_checkpoint_request(hkn_checkpoint *worker);
int hkn_checkpoint_wait(hkn_checkpoint *worker);
int hkn_checkpoint_tail(hkn_checkpoint *worker);
// 0 pending, 1 incomplete, 2 fully copied, negative SQLite error on failure.
int hkn_checkpoint_result(hkn_checkpoint *worker);
#endif
