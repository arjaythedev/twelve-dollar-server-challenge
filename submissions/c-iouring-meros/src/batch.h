/*
 * batch: the requests that wait for the database, answered together.
 *
 * Group commit: every write that arrives in one event-loop round runs inside a single
 * transaction, and each client gets its response only after COMMIT returned. If COMMIT fails,
 * every client of the group gets a 500.
 *
 * Feed sharing (optional, off by default, `make COALESCE_FEEDS=1`): every GET /feed that arrives in the
 * same round shares one query and one rendering. The query runs after all of those requests arrived,
 * so none of them sees stale data. Without it, every request runs its own query. See batch.c for why
 * it is off.
 */
#ifndef BATCH_H
#define BATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "conn.h"
#include "store.h"
#include "strview.h"

typedef struct batch batch;

batch *batch_create(void);

/* True while one more request of any kind can be added. */
bool batch_has_room(const batch *b);
bool batch_is_empty(const batch *b);

/*
 * Each add marks the connection as awaiting its response. `close_after` asks to close the
 * connection after the response. batch_add_post copies author and body.
 */
bool batch_add_post(batch *b, conn_table *conns, int id, bool close_after, int64_t user_id, strview author, strview body);
void batch_add_like(batch *b, conn_table *conns, int id, bool close_after, int64_t user_id, int64_t post_id);
void batch_add_feed(batch *b, conn_table *conns, int id, bool close_after);

/*
 * Called for each connection whose response was sent, so the caller can resume its pipeline.
 * It must not add to the batch while batch_run executes; queue the connection id and resume it later.
 */
typedef void (*batch_done_fn)(void *context, int id);

/* Executes and answers everything queued, then empties the batch. `scratch` is working space for responses. */
void batch_run(batch *b, store *db, conn_table *conns, char *scratch, size_t scratch_cap, batch_done_fn on_done, void *context);

#endif
