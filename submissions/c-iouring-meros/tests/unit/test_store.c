#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "check.h"
#include "store.h"

/* The challenge's schema (schema.sql), without the data. */
static const char schema[] =
	"CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
	" created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
	"CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
	" body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
	" created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
	"CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
	" created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
	"CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
	"CREATE INDEX posts_user_id_idx ON posts (user_id);"
	"CREATE INDEX likes_post_id_idx ON likes (post_id);"
	"INSERT INTO users(id, username) VALUES (1, 'ada'), (2, 'bo'), (3, 'cy');";

typedef struct feed_capture {
	int count;
	int64_t ids[32];
	int64_t likes[32];
} feed_capture;

static void capture_post(const post_row *post, void *context)
{
	feed_capture *capture = context;

	if (capture->count < 32) {
		capture->ids[capture->count] = post->id;
		capture->likes[capture->count] = post->like_count;
	}
	capture->count++;
}

static void create_database(const char *path)
{
	sqlite3 *handle;
	char *message = NULL;

	CHECK(sqlite3_open(path, &handle) == SQLITE_OK);
	CHECK(sqlite3_exec(handle, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL) == SQLITE_OK);
	CHECK(sqlite3_exec(handle, schema, NULL, NULL, &message) == SQLITE_OK);
	for (int id = 1; id <= 25; id++) {
		char sql[256];

		/* Increasing timestamps, so the newest posts by time are the highest ids. */
		snprintf(sql, sizeof sql, "INSERT INTO posts(id, user_id, body, created_at) VALUES (%d, %d, 'post %d', '2025-01-01T00:00:%02d.000Z');",
		         id, 1 + id % 3, id, id);
		CHECK(sqlite3_exec(handle, sql, NULL, NULL, NULL) == SQLITE_OK);
	}
	CHECK(sqlite3_exec(handle, "INSERT INTO likes(user_id, post_id) VALUES (1, 25), (2, 25), (3, 25), (1, 24), (1, 6);", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(handle);
}

int main(void)
{
	char directory[] = "/tmp/test_store.XXXXXX";
	char path[128], shm_path[140];

	if (mkdtemp(directory) == NULL) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(path, sizeof path, "%s/feed.db", directory);
	snprintf(shm_path, sizeof shm_path, "%s-shm", path);
	create_database(path);
	sqlite3_shutdown(); /* store_open configures SQLite, which is only allowed before it is initialised */

	store *db = store_open(path);

	/* Exclusive locking is set before the log is first used, so its index stays in memory: no -shm file, no lock calls. */
	CHECK(access(shm_path, F_OK) != 0);

	feed_capture feed = { 0 };

	CHECK(store_read_feed(db, capture_post, &feed));
	CHECK(feed.count == 20);
	CHECK(feed.ids[0] == 25 && feed.ids[19] == 6);
	CHECK(feed.likes[0] == 3 && feed.likes[1] == 1 && feed.likes[2] == 0 && feed.likes[19] == 1);

	/* Reads share one transaction until it is ended, and see the same data as a read of their own would. */
	CHECK(store_in_transaction(db));
	feed.count = 0;
	CHECK(store_read_feed(db, capture_post, &feed));
	CHECK(feed.count == 20 && feed.ids[0] == 25);
	CHECK(store_find_post(db, 25, capture_post, &feed) == FIND_FOUND);
	CHECK(store_find_post(db, 26, capture_post, &feed) == FIND_MISSING);
	CHECK(store_in_transaction(db));
	store_end_reads(db);
	CHECK(!store_in_transaction(db));
	store_end_reads(db); /* nothing open: no effect */
	CHECK(!store_in_transaction(db));

	/* A write after reads ends the shared transaction itself, and the next read sees the new row. */
	CHECK(store_find_post(db, 25, capture_post, &feed) == FIND_FOUND);
	CHECK(store_begin(db));
	new_post created;
	CHECK(store_insert_post(db, 2, STRVIEW("written after a read"), &created));
	CHECK(created.id == 26);
	CHECK(store_insert_like(db, 3, 26) == LIKE_ADDED);
	CHECK(store_insert_like(db, 3, 26) == LIKE_ALREADY_EXISTED);
	CHECK(store_insert_like(db, 3, 999) == LIKE_POST_MISSING);
	CHECK(store_commit(db));
	CHECK(!store_in_transaction(db));

	feed.count = 0;
	CHECK(store_read_feed(db, capture_post, &feed));
	CHECK(feed.count == 20 && feed.ids[0] == 26 && feed.likes[0] == 1 && feed.ids[1] == 25 && feed.likes[1] == 3);
	CHECK(store_find_post(db, 26, capture_post, &feed) == FIND_FOUND);

	/* The log is copied into the database file with a read transaction open: the checkpoint ends it first. */
	store_checkpoint(db);
	CHECK(!store_in_transaction(db));
	CHECK(store_find_post(db, 26, capture_post, &feed) == FIND_FOUND);

	store_close(db);

	char command[160];

	snprintf(command, sizeof command, "rm -rf %s", directory);
	if (system(command) != 0)
		fprintf(stderr, "could not remove %s\n", directory);
	TEST_MAIN_END();
}
