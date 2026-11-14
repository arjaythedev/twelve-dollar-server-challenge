/* Database layer: SQLite in-process. Reads walk SQLite's own B-trees through
 * its internal cursor API (no SQL compilation or VDBE per request); writes use
 * prepared statements inside group-commit transactions; a background thread
 * checkpoints the WAL. Only the event-loop thread calls these, except where
 * noted. */
#ifndef DB_H
#define DB_H

#include <stddef.h>
#include <stdint.h>

int db_open(const char *path);
void db_start_checkpointer(uint32_t start_frames);

/* SELECT 1. Returns 0 if OK, else writes the error message to err. */
int db_health(char *err, size_t cap);

/* One read transaction spans many requests of a loop pass. db_read_begin is
 * idempotent; db_read_end must be called before any write or SQL statement. */
int db_read_begin(void);
void db_read_end(void);

/* Write the response body JSON into [o, oe). Return the end pointer, or NULL
 * if the post doesn't exist (post only), or (char *)-1 on error. */
#define DB_ERR ((char *)-1)
char *db_feed_json(char *o, char *oe);
char *db_post_json(int64_t id, char *o, char *oe);

/* Group commit. */
int db_write_begin(void);
int db_write_commit(void);
void db_write_rollback(void);
/* 0 ok; created must hold 32 bytes. */
int db_insert_post(int64_t uid, const char *body, size_t n, int64_t *id, char *created, size_t *clen);
/* 1 = new like, 0 = already liked, -1 = post not found, -2 = error */
int db_insert_like(int64_t uid, int64_t pid);

/* WAL state, readable from the loop thread. */
uint32_t db_wal_frames(void);   /* frames in the WAL after the last commit */
uint64_t db_cache_misses(void); /* page-cache misses on the main connection so far (DEBUG stats) */
uint64_t db_ckpt_generation(void); /* bumps when a checkpoint fully backfills */

/* Self-check: compare B-tree reads against the reference SQL (testing only). */
void db_set_verify(int on);

#endif
