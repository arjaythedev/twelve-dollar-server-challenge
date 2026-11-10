#ifndef HUYKN_CHECKPOINT_H
#define HUYKN_CHECKPOINT_H
#include "sqlite/sqlite3.h"
#include <stdint.h>
typedef struct hkn_checkpoint hkn_checkpoint;
hkn_checkpoint *hkn_checkpoint_new(const char *path, const char *vfs);
void hkn_checkpoint_free(hkn_checkpoint *worker);
int hkn_checkpoint_request(hkn_checkpoint *worker);
// Wait for quiescence and discard old progress before a caller-owned WAL restart.
int hkn_checkpoint_wait(hkn_checkpoint *worker);
void hkn_checkpoint_observe(hkn_checkpoint *worker, uint64_t written);
unsigned hkn_checkpoint_interval(hkn_checkpoint *worker);
#endif
