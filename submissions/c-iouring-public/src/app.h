// app.h — the API layer (app.c) between the event loop and the engine.
#ifndef CURSED_APP_H
#define CURSED_APP_H
#include <stddef.h>
#include <stdint.h>
#include "loop.h"
typedef struct {
  uint64_t batches, requests, lanes, slow, slow_checked, jwts, held, commit_failures, deadline_503;
} app_stats;
int app_init(const char *db_path, const char *secret, char *err, size_t errn);  // 0 or -1 (message in err)
void app_close(void);
int app_batch(loop_t *L, loop_in *in, int n);  // the loop's on_batch
void app_tick(loop_t *L);                      // the loop's on_tick
void app_quiesce(void);                        // shutting down: start no new checkpoint cycle
int app_inflight(void);                        // file ops not yet completed (DB_CKPT=uring): keep the loop running
const app_stats *app_get_stats(void);
const char *app_parsers(void);
const char *app_db_mode(
    void);  // e.g. "direct reads, cursed-wal VFS"                   // "<fast>+<slow>" as built, e.g. "batch+fbparse"
struct eng;
struct eng *app_engine(void);  // for stats only
#endif
