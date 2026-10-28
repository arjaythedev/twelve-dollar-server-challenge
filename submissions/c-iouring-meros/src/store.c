#include "store.h"

#include <stdio.h>
#include <stdlib.h>

#include "fatal.h"
#include <sqlite3.h>

/* Checkpoint when the write-ahead log holds this many pages. */
#ifndef STORE_CHECKPOINT_FRAMES
#define STORE_CHECKPOINT_FRAMES 4096
#endif

enum {
	FEED_SIZE = 20,
	FEED_BODY_CAP = 2000,   /* 500 characters of up to 4 bytes */
	FEED_AUTHOR_CAP = 256,
};

/* One feed post copied out of SQLite, so the whole feed can be checked before anything is returned. */
typedef struct feed_row {
	int64_t id;
	int64_t like_count;
	size_t body_len;
	size_t created_at_len;
	size_t author_len;
	char body[FEED_BODY_CAP];
	char created_at[CREATED_AT_CAP];
	char author[FEED_AUTHOR_CAP];
} feed_row;

struct store {
	sqlite3 *handle;
	sqlite3_stmt *read_feed;      /* the reference query: joins and a correlated count per post */
	sqlite3_stmt *feed_ids;       /* newest post ids, from the created_at index alone */
	sqlite3_stmt *feed_posts;     /* the posts and authors of an id range */
	sqlite3_stmt *feed_likes;     /* like counts of an id range */
	feed_row feed_rows[FEED_SIZE];
	bool checkpoint_due;
	sqlite3_stmt *find_post;
	sqlite3_stmt *insert_post;
	sqlite3_stmt *insert_like;
	sqlite3_stmt *post_exists;
	sqlite3_stmt *ping;
	sqlite3_stmt *begin;
	sqlite3_stmt *commit;
	sqlite3_stmt *rollback;
};

#define POST_COLUMNS \
	"SELECT p.id, p.body, p.created_at, u.username, " \
	"(SELECT count(*) FROM likes l WHERE l.post_id = p.id) " \
	"FROM posts p JOIN users u ON u.id = p.user_id "

/* Column order of POST_COLUMNS. */
enum { COL_ID, COL_BODY, COL_CREATED_AT, COL_AUTHOR, COL_LIKE_COUNT };

static sqlite3_stmt *prepare(sqlite3 *handle, const char *sql)
{
	sqlite3_stmt *statement;

	if (sqlite3_prepare_v3(handle, sql, -1, SQLITE_PREPARE_PERSISTENT, &statement, NULL) != SQLITE_OK)
		fatal("cannot prepare \"%s\": %s", sql, sqlite3_errmsg(handle));
	return statement;
}

static void run_script(sqlite3 *handle, const char *sql)
{
	char *message = NULL;

	if (sqlite3_exec(handle, sql, NULL, NULL, &message) != SQLITE_OK)
		fatal("cannot run \"%s\": %s", sql, message);
}

static strview column_text(sqlite3_stmt *statement, int column)
{
	return strview_make((const char *)sqlite3_column_text(statement, column), (size_t)sqlite3_column_bytes(statement, column));
}

static void read_post_row(sqlite3_stmt *statement, post_row *post)
{
	post->id = sqlite3_column_int64(statement, COL_ID);
	post->body = column_text(statement, COL_BODY);
	post->created_at = column_text(statement, COL_CREATED_AT);
	post->author = column_text(statement, COL_AUTHOR);
	post->like_count = sqlite3_column_int64(statement, COL_LIKE_COUNT);
}

/* Runs a statement that returns no rows and resets it. */
static bool run_to_completion(sqlite3_stmt *statement)
{
	int rc = sqlite3_step(statement);

	sqlite3_reset(statement);
	return rc == SQLITE_DONE;
}

#ifndef STORE_MMAP_BYTES
#define STORE_MMAP_BYTES 1073741824 /* map the whole file */
#endif
#ifndef STORE_CACHE_KIB
#define STORE_CACHE_KIB 16384 /* SQLite's own page cache */
#endif

/*
 * One process owns the file, so:
 *  - EXCLUSIVE locking removes every file-lock system call, and keeps the WAL index in heap memory;
 *  - synchronous=NORMAL in WAL mode commits with one write() and no fsync, which still survives a
 *    process crash (an OS crash can lose the last commits; the challenge rules allow this level);
 *  - the whole file is memory-mapped, so reads are page-cache hits without read() calls;
 *  - automatic checkpoints are off: they would run inside COMMIT, before the clients get their response.
 */
static void configure(sqlite3 *handle)
{
	char script[512];

	snprintf(script, sizeof script,
	         "PRAGMA journal_mode=WAL;"
	         "PRAGMA synchronous=NORMAL;"
	         "PRAGMA locking_mode=EXCLUSIVE;"
	         "PRAGMA mmap_size=%d;"
	         "PRAGMA cache_size=-%d;"
	         "PRAGMA temp_store=MEMORY;"
	         "PRAGMA wal_autocheckpoint=0;", /* checkpoints run between event-loop rounds, see store_checkpoint_if_due */
	         STORE_MMAP_BYTES, STORE_CACHE_KIB);
	run_script(handle, script);
}

/* Reads every table and index once so the first requests do not pay for page faults. */
static void warm_up(sqlite3 *handle)
{
	run_script(handle,
	           "SELECT count(*), sum(length(body)) FROM posts;"
	           "SELECT count(*) FROM users;"
	           "SELECT count(*) FROM likes;"
	           "SELECT count(*) FROM posts WHERE user_id > 0;"
	           "SELECT count(*) FROM likes WHERE post_id > 0;");
}

/* Called by SQLite after every commit with the number of pages in the write-ahead log. */
static int note_wal_size(void *context, sqlite3 *handle, const char *name, int pages)
{
	(void)handle;
	(void)name;
	if (pages >= STORE_CHECKPOINT_FRAMES)
		((store *)context)->checkpoint_due = true;
	return SQLITE_OK;
}

store *store_open(const char *path)
{
	store *db = calloc(1, sizeof *db);

	if (db == NULL)
		fatal("out of memory");
	if (sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0) != SQLITE_OK)
		fatal("cannot configure SQLite");
	if (sqlite3_open_v2(path, &db->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
		fatal("cannot open database %s: %s", path, sqlite3_errmsg(db->handle));
	sqlite3_busy_timeout(db->handle, 0);
	configure(db->handle);

	db->read_feed = prepare(db->handle, POST_COLUMNS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
	db->feed_ids = prepare(db->handle, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
	db->feed_posts = prepare(db->handle,
	                         "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id "
	                         "WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC");
	db->feed_likes = prepare(db->handle,
	                         "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2 ORDER BY post_id DESC");
	db->find_post = prepare(db->handle, POST_COLUMNS "WHERE p.id = ?1");
	db->insert_post = prepare(db->handle, "INSERT INTO posts(user_id, body) VALUES(?1, ?2) RETURNING id, created_at");
	/* Inserting from a SELECT on posts means a missing post inserts nothing, in one statement. */
	db->insert_like = prepare(db->handle,
	                          "INSERT INTO likes(user_id, post_id) SELECT ?1, id FROM posts WHERE id = ?2 "
	                          "ON CONFLICT DO NOTHING");
	db->post_exists = prepare(db->handle, "SELECT 1 FROM posts WHERE id = ?1");
	db->ping = prepare(db->handle, "SELECT 1");
	db->begin = prepare(db->handle, "BEGIN");
	db->commit = prepare(db->handle, "COMMIT");
	db->rollback = prepare(db->handle, "ROLLBACK");

	sqlite3_wal_hook(db->handle, note_wal_size, db);
	warm_up(db->handle);
	return db;
}

void store_close(store *db)
{
	sqlite3_stmt **statements[] = { &db->read_feed, &db->feed_ids, &db->feed_posts, &db->feed_likes, &db->find_post, &db->insert_post,
	                                &db->insert_like, &db->post_exists, &db->ping, &db->begin, &db->commit, &db->rollback };

	for (size_t i = 0; i < sizeof statements / sizeof statements[0]; i++)
		sqlite3_finalize(*statements[i]);
	sqlite3_close(db->handle);
	free(db);
}

void store_checkpoint(store *db)
{
	sqlite3_wal_checkpoint_v2(db->handle, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
}

void store_checkpoint_if_due(store *db)
{
	if (!db->checkpoint_due)
		return;
	db->checkpoint_due = false;
	sqlite3_wal_checkpoint_v2(db->handle, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
}

bool store_ping(store *db)
{
	int rc = sqlite3_step(db->ping);

	sqlite3_reset(db->ping);
	return rc == SQLITE_ROW;
}

const char *store_error(store *db)
{
	return sqlite3_errmsg(db->handle);
}

find_result store_find_post(store *db, int64_t post_id, post_visitor visit, void *context)
{
	sqlite3_bind_int64(db->find_post, 1, post_id);
	int rc = sqlite3_step(db->find_post);

	if (rc == SQLITE_ROW) {
		post_row post;

		read_post_row(db->find_post, &post);
		visit(&post, context);
	}
	sqlite3_reset(db->find_post);
	if (rc == SQLITE_ROW)
		return FIND_FOUND;
	return rc == SQLITE_DONE ? FIND_MISSING : FIND_FAILED;
}

/* The reference query. Slow because every post costs three B-tree descents. */
static bool read_feed_with_joins(store *db, post_visitor visit, void *context)
{
	int rc;

	while ((rc = sqlite3_step(db->read_feed)) == SQLITE_ROW) {
		post_row post;

		read_post_row(db->read_feed, &post);
		visit(&post, context);
	}
	sqlite3_reset(db->read_feed);
	return rc == SQLITE_DONE;
}

/* Reads the ids of the newest posts. Returns how many there are, or -1 on error. */
static int read_newest_ids(store *db, int64_t ids[FEED_SIZE])
{
	int count = 0, rc = SQLITE_ROW;

	while (count < FEED_SIZE && (rc = sqlite3_step(db->feed_ids)) == SQLITE_ROW)
		ids[count++] = sqlite3_column_int64(db->feed_ids, 0);
	sqlite3_reset(db->feed_ids);
	return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? count : -1;
}

/* True when the ids are newest-first and without gaps: then the range [last, first] holds exactly these posts. */
static bool ids_are_consecutive_descending(const int64_t *ids, int count)
{
	for (int i = 1; i < count; i++) {
		if (ids[i] != ids[i - 1] - 1)
			return false;
	}
	return true;
}

static bool copy_text(char *destination, size_t capacity, size_t *length, strview text)
{
	if (text.len > capacity)
		return false;
	memcpy(destination, text.ptr, text.len);
	*length = text.len;
	return true;
}

/*
 * Fills db->feed_rows with the posts of ids [lowest, highest], newest first, merging the posts
 * with their like counts. Both queries walk their index in the same order, so the merge needs
 * no lookup. Returns false when the result is not exactly `expected` complete rows.
 */
static bool collect_feed_rows(store *db, int64_t lowest, int64_t highest, int expected)
{
	int count = 0, rc, likes_rc;

	sqlite3_bind_int64(db->feed_posts, 1, lowest);
	sqlite3_bind_int64(db->feed_posts, 2, highest);
	sqlite3_bind_int64(db->feed_likes, 1, lowest);
	sqlite3_bind_int64(db->feed_likes, 2, highest);
	likes_rc = sqlite3_step(db->feed_likes);

	while ((rc = sqlite3_step(db->feed_posts)) == SQLITE_ROW) {
		if (count == expected)
			break; /* more posts than expected: the range is not what the id scan promised */

		int64_t id = sqlite3_column_int64(db->feed_posts, 0);
		feed_row *row = &db->feed_rows[count];

		/* One index entry per like, newest post first: skip the entries of posts above this one, then count its own. */
		while (likes_rc == SQLITE_ROW && sqlite3_column_int64(db->feed_likes, 0) > id)
			likes_rc = sqlite3_step(db->feed_likes);
		row->id = id;
		row->like_count = 0;
		while (likes_rc == SQLITE_ROW && sqlite3_column_int64(db->feed_likes, 0) == id) {
			row->like_count++;
			likes_rc = sqlite3_step(db->feed_likes);
		}
		if (!copy_text(row->body, sizeof row->body, &row->body_len, column_text(db->feed_posts, COL_BODY)) ||
		    !copy_text(row->created_at, sizeof row->created_at, &row->created_at_len, column_text(db->feed_posts, COL_CREATED_AT)) ||
		    !copy_text(row->author, sizeof row->author, &row->author_len, column_text(db->feed_posts, COL_AUTHOR)))
			break;
		count++;
	}
	sqlite3_reset(db->feed_posts);
	sqlite3_reset(db->feed_likes);
	return rc == SQLITE_DONE && count == expected && (likes_rc == SQLITE_ROW || likes_rc == SQLITE_DONE);
}

/*
 * The newest posts with three cheap scans instead of 60 B-tree descents: the newest ids from
 * the created_at index, then the posts and the like counts of that id range in one pass each.
 * Whenever the shortcut's assumptions fail (ids with gaps, a post whose author row is missing,
 * oversized text) it falls back to the reference query, so the answer is always the same.
 */
bool store_read_feed(store *db, post_visitor visit, void *context)
{
	int64_t ids[FEED_SIZE];
	int count = read_newest_ids(db, ids);

	if (count < 0)
		return false;
	if (count == 0 || !ids_are_consecutive_descending(ids, count) || !collect_feed_rows(db, ids[count - 1], ids[0], count))
		return read_feed_with_joins(db, visit, context);

	for (int i = 0; i < count; i++) {
		const feed_row *row = &db->feed_rows[i];
		post_row post = {
			.id = row->id,
			.body = strview_make(row->body, row->body_len),
			.created_at = strview_make(row->created_at, row->created_at_len),
			.author = strview_make(row->author, row->author_len),
			.like_count = row->like_count,
		};

		visit(&post, context);
	}
	return true;
}

bool store_begin(store *db)
{
	return run_to_completion(db->begin);
}

bool store_in_transaction(store *db)
{
	return !sqlite3_get_autocommit(db->handle);
}

bool store_commit(store *db)
{
	return run_to_completion(db->commit);
}

void store_rollback(store *db)
{
	if (!sqlite3_get_autocommit(db->handle))
		run_to_completion(db->rollback);
}

bool store_insert_post(store *db, int64_t user_id, strview body, new_post *created)
{
	sqlite3_bind_int64(db->insert_post, 1, user_id);
	sqlite3_bind_text(db->insert_post, 2, body.ptr, (int)body.len, SQLITE_STATIC);
	if (sqlite3_step(db->insert_post) != SQLITE_ROW) {
		sqlite3_reset(db->insert_post);
		return false;
	}
	created->id = sqlite3_column_int64(db->insert_post, 0);
	strview timestamp = column_text(db->insert_post, 1);

	if (timestamp.len >= sizeof created->created_at) {
		sqlite3_reset(db->insert_post);
		return false;
	}
	memcpy(created->created_at, timestamp.ptr, timestamp.len);
	created->created_at_len = timestamp.len;
	/* RETURNING produced the row; a second step completes the INSERT itself. */
	return run_to_completion(db->insert_post);
}

like_result store_insert_like(store *db, int64_t user_id, int64_t post_id)
{
	sqlite3_bind_int64(db->insert_like, 1, user_id);
	sqlite3_bind_int64(db->insert_like, 2, post_id);
	int rc = sqlite3_step(db->insert_like);
	int rows_changed = sqlite3_changes(db->handle);

	sqlite3_reset(db->insert_like);
	if (rc != SQLITE_DONE)
		return LIKE_FAILED;
	if (rows_changed != 0)
		return LIKE_ADDED;

	/* Nothing inserted: either the like exists already, or the post does not exist. */
	sqlite3_bind_int64(db->post_exists, 1, post_id);
	rc = sqlite3_step(db->post_exists);
	sqlite3_reset(db->post_exists);
	if (rc == SQLITE_ROW)
		return LIKE_ALREADY_EXISTED;
	return rc == SQLITE_DONE ? LIKE_POST_MISSING : LIKE_FAILED;
}
