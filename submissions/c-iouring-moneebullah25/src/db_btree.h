/* Direct B-tree reads: the feed and single posts read SQLite's pages through
 * its own cursor API instead of the SQL layer. Every byte still comes from
 * the database on the request that serves it (rule 5); the only thing that
 * is skipped is the VDBE. The SQL path in db.c stays the oracle: any
 * condition this code does not handle makes it return "fall back".
 *
 * This translation unit includes the amalgamation (the cursor API and the
 * record format helpers are SQLITE_PRIVATE), so it is compiled in place of
 * sqlite3.o. */
#ifndef DB_BTREE_H
#define DB_BTREE_H

#include <stddef.h>
#include <stdint.h>

#include "sqlite3.h"

struct bt;

/* Validates the schema (table and index roots, column positions) and sets
 * up the cursors. The connection's prepared statements must exist already
 * so the schema is loaded. Returns NULL and fills err on any mismatch. */
struct bt *bt_open(sqlite3 *conn, char *err, size_t err_cap);
void bt_close(struct bt *bt);

/* Writes {"posts":[...]} for the 20 newest posts. Returns the length, or 0
 * to ask for the SQL path (any invariant failure, an unexpected record,
 * fewer than 20 posts, a schema change). */
size_t bt_feed_json(struct bt *bt, char *out, size_t cap);

/* Writes {"post":{...}}. Returns the length, 0 when the post does not
 * exist, or (size_t)-1 to ask for the SQL path. */
size_t bt_post_json(struct bt *bt, int64_t id, char *out, size_t cap);

/* Ends the B-tree read transaction this layer may have opened, when the
 * connection is in autocommit mode (otherwise the SQL COMMIT ends it).
 * Called by db_read_end and after a read outside the shared transaction,
 * so that no snapshot is held across requests. */
void bt_read_end(struct bt *bt);

#endif
