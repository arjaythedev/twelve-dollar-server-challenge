/*
 * store: every SQLite access of the server.
 *
 * Statements are prepared once. Nothing is cached across calls: each read executes its query.
 * Text views in a post_row point into SQLite's result buffers and are valid only inside the
 * visitor callback that receives them.
 */
#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "strview.h"

typedef struct store store;

typedef struct post_row {
	int64_t id;
	strview body;
	strview created_at;
	strview author;
	int64_t like_count;
} post_row;

enum { CREATED_AT_CAP = 32 }; /* "2025-12-31T23:59:19.409Z" and room to spare */

typedef struct new_post {
	int64_t id;
	char created_at[CREATED_AT_CAP];
	size_t created_at_len;
} new_post;

typedef enum find_result { FIND_FOUND, FIND_MISSING, FIND_FAILED } find_result;
typedef enum like_result { LIKE_ADDED, LIKE_ALREADY_EXISTED, LIKE_POST_MISSING, LIKE_FAILED } like_result;

/* Opens the database file for exclusive use by this process. Exits the program on failure. */
store *store_open(const char *path);
void store_close(store *db);

/* SELECT 1. On failure, store_error describes why. */
bool store_ping(store *db);
const char *store_error(store *db);

/* ---- reads ---- */

/* The post row is only valid inside the visitor: SQLite frees its text when the statement resets. */
typedef void (*post_visitor)(const post_row *post, void *context);

/* Calls `visit` once if the post exists. */
find_result store_find_post(store *db, int64_t post_id, post_visitor visit, void *context);

/* Calls `visit` for each of the 20 newest posts, newest first. Returns false if the query failed. */
bool store_read_feed(store *db, post_visitor visit, void *context);

/* ---- writes: all of them happen between store_begin and store_commit ---- */

bool store_begin(store *db);

/* False once SQLite rolled the open transaction back by itself (disk full, I/O error, out of memory). */
bool store_in_transaction(store *db);
bool store_commit(store *db);
void store_rollback(store *db);

bool store_insert_post(store *db, int64_t user_id, strview body, new_post *created);
like_result store_insert_like(store *db, int64_t user_id, int64_t post_id);

/* Flushes the write-ahead log into the database file and truncates it. For orderly shutdown. */
void store_checkpoint(store *db);

/*
 * Copies a large write-ahead log into the database file. Call it between event-loop rounds, after the
 * responses went out: the copy and its fsync then never delay a request.
 */
void store_checkpoint_if_due(store *db);

#endif
