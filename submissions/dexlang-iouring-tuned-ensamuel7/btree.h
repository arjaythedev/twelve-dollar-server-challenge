/* The read engine's interface, as server.dx's `external module bt` sees it.
 * Plain C types only: nothing from SQLite's internals crosses this line.
 *
 * Every read returns -1 when it cannot answer exactly as the reference SQL
 * would (no transaction open, an unexpected record, a NUL inside a text
 * value, a buffer too small), and the caller then runs the SQL instead. */
#ifndef DEX_BTREE_H
#define DEX_BTREE_H

/* The next connection the program opens is the one the engine reads
 * through. Call just before db.openSingleThreaded. */
int dexbt_attach_next(void);

/* The next connection opened is the checkpointer's (dexbt_sync_db). */
int dexbt_attach_checkpointer(void);

/* Grow the database file mb megabytes at a time, on both connections. */
int dexbt_chunk(int mb);

/* fdatasync the database file from the checkpointer's connection: 0 or -1. */
int dexbt_sync_db(void);

/* Busy handler that retries every 200 µs for up to ms milliseconds, on the
 * event loop's connection or (checkpointer != 0) the checkpointer's. 0 on
 * success. */
int dexbt_busy(int ms, int checkpointer);

/* on != 0: the busy handler gives up at once instead of waiting. */
int dexbt_nowait(int on);

/* After the schema is loaded (any statement prepared): 1 if the engine can
 * serve reads on that connection, 0 if it stays off. */
int dexbt_init(void);

/* GET /feed: the 20 newest posts that have an author, newest first, as the
 * response body (dexbt_json). The number found (0..20), or -1. */
int dexbt_feed(void);

/* GET /posts/:id: 1 found (the body is in dexbt_json), 0 no such post, -1. */
int dexbt_post(long id);

/* Does post `id` exist: 1, 0, or -1. */
int dexbt_exists(long id);

/* The JSON body of the last dexbt_feed or dexbt_post that answered, byte for
 * byte what json.encode makes of the same rows; "" after one that did not.
 * Valid until the next call into the engine. */
const char* dexbt_json(void);

#endif
