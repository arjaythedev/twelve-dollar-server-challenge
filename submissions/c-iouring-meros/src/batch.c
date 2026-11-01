#include "batch.h"

#include "http_response.h"
#include "memory.h"
#include "post_json.h"

/*
 * 0 (default): every GET /feed runs its own query.
 * 1: every GET /feed of one event-loop round shares one query and one rendering (see batch.h).
 *    Build with `make COALESCE_FEEDS=1`.
 *
 * Sharing is off by default because challenge rule 5 forbids "query-result caches". Nothing here outlives
 * a round, and every client gets a query that ran after its request arrived, so it is not stale. But one
 * query result still serves many requests, and a strict reading could call that a cache. It saves about
 * 4 us of CPU per request at 60,000 users; the server uses about 12% of one core at 100,000 users even
 * without it, so the saving does not decide the score. See README.md.
 */
#ifndef COALESCE_FEEDS
#define COALESCE_FEEDS 0
#endif

enum {
	MAX_WRITES = 128,
	MAX_FEEDS = 4096,
	INPUT_SLOT = 4096,   /* per write: author (<= 256 bytes) followed by the body (<= 2000 bytes) */
	AUTHOR_CAP = 256,
	REPLY_SLOT = 8192,   /* per write: a created post, with every character escaped, fits */
};

typedef enum write_kind { WRITE_CREATE_POST, WRITE_LIKE } write_kind;

typedef struct pending_write {
	write_kind kind;
	int id;
	bool close_after;
	int64_t user_id;
	int64_t post_id;      /* WRITE_LIKE */
	strview author;       /* WRITE_CREATE_POST: points into the input slot */
	strview body;
	strview reply;        /* set while executing; points into the reply slot */
} pending_write;

typedef struct pending_feed {
	int id;
	bool close_after;
} pending_feed;

struct batch {
	size_t write_count;
	size_t feed_count;
	pending_write writes[MAX_WRITES];
	pending_feed feeds[MAX_FEEDS];
	char inputs[MAX_WRITES][INPUT_SLOT];
	char replies[MAX_WRITES][REPLY_SLOT];
};

batch *batch_create(void)
{
	return xcalloc(1, sizeof(batch));
}

bool batch_has_room(const batch *b)
{
	return b->write_count < MAX_WRITES && b->feed_count < MAX_FEEDS;
}

bool batch_is_empty(const batch *b)
{
	return b->write_count == 0 && b->feed_count == 0;
}

static void await_batch(conn_table *conns, int id)
{
	conn_set(conn_get(conns, id), CONN_AWAITING_BATCH);
}

bool batch_add_post(batch *b, conn_table *conns, int id, bool close_after, int64_t user_id, strview author, strview body)
{
	size_t slot = b->write_count;
	char *input = b->inputs[slot];

	if (author.len > AUTHOR_CAP || body.len > INPUT_SLOT - AUTHOR_CAP)
		return false;
	memcpy(input, author.ptr, author.len);
	memcpy(input + AUTHOR_CAP, body.ptr, body.len);
	b->writes[b->write_count++] = (pending_write){
		.kind = WRITE_CREATE_POST,
		.id = id,
		.close_after = close_after,
		.user_id = user_id,
		.author = strview_make(input, author.len),
		.body = strview_make(input + AUTHOR_CAP, body.len),
	};
	await_batch(conns, id);
	return true;
}

void batch_add_like(batch *b, conn_table *conns, int id, bool close_after, int64_t user_id, int64_t post_id)
{
	b->writes[b->write_count++] = (pending_write){
		.kind = WRITE_LIKE, .id = id, .close_after = close_after, .user_id = user_id, .post_id = post_id,
	};
	await_batch(conns, id);
}

void batch_add_feed(batch *b, conn_table *conns, int id, bool close_after)
{
	b->feeds[b->feed_count++] = (pending_feed){ id, close_after };
	await_batch(conns, id);
}

/* Sends the response, lets the connection parse its next request, and reports it as done. */
static void answer(conn_table *conns, int id, strview reply, bool close_after, batch_done_fn on_done, void *context)
{
	conn *c = conn_get(conns, id);

	conn_send(conns, id, reply);
	conn_clear(c, CONN_AWAITING_BATCH);
	if (close_after)
		conn_set(c, CONN_CLOSE_AFTER_SEND);
	on_done(context, id);
}

/* ---- writes ---- */

static strview finish_or_internal_error(http_response *response, http_status status, bool close_after)
{
	strview reply = http_response_finish(response, status, close_after);

	return reply.len != 0 ? reply : http_response_error(API_ERROR_INTERNAL, close_after);
}

static void execute_create_post(store *db, pending_write *write, char *reply_buffer)
{
	new_post created;

	if (!store_insert_post(db, write->user_id, write->body, &created)) {
		write->reply = http_response_error(API_ERROR_INTERNAL, write->close_after);
		return;
	}
	post_row post = {
		.id = created.id,
		.body = write->body,
		.created_at = strview_make(created.created_at, created.created_at_len),
		.author = write->author,
		.like_count = 0,
	};
	http_response response;

	http_response_begin(&response, reply_buffer, REPLY_SLOT);
	writer_literal(&response.body, "{\"post\":");
	post_json_write(&response.body, &post);
	writer_literal(&response.body, "}");
	write->reply = finish_or_internal_error(&response, HTTP_CREATED, write->close_after);
}

static void execute_like(store *db, pending_write *write, char *reply_buffer)
{
	like_result result = store_insert_like(db, write->user_id, write->post_id);
	http_response response;

	switch (result) {
	case LIKE_POST_MISSING:
		write->reply = http_response_error(API_ERROR_POST_NOT_FOUND, write->close_after);
		return;
	case LIKE_FAILED:
		write->reply = http_response_error(API_ERROR_INTERNAL, write->close_after);
		return;
	case LIKE_ADDED:
	case LIKE_ALREADY_EXISTED:
		break;
	}
	http_response_begin(&response, reply_buffer, REPLY_SLOT);
	writer_literal(&response.body, "{\"liked\":true,\"already_liked\":");
	if (result == LIKE_ALREADY_EXISTED)
		writer_literal(&response.body, "true");
	else
		writer_literal(&response.body, "false");
	writer_literal(&response.body, ",\"post_id\":");
	writer_u64(&response.body, (uint64_t)write->post_id);
	writer_literal(&response.body, "}");
	write->reply = finish_or_internal_error(&response, result == LIKE_ADDED ? HTTP_CREATED : HTTP_OK, write->close_after);
}

static void fail_write(pending_write *write)
{
	write->reply = http_response_error(API_ERROR_INTERNAL, write->close_after);
}

static void run_writes(batch *b, store *db, conn_table *conns, batch_done_fn on_done, void *context)
{
	bool began = store_begin(db);

	for (size_t i = 0; i < b->write_count; i++) {
		pending_write *write = &b->writes[i];

		/* A statement that fails badly makes SQLite roll the whole transaction back; the writes after it must not commit alone. */
		if (!began || !store_in_transaction(db))
			fail_write(write);
		else if (write->kind == WRITE_CREATE_POST)
			execute_create_post(db, write, b->replies[i]);
		else
			execute_like(db, write, b->replies[i]);
	}
	bool committed = began && store_in_transaction(db) && store_commit(db);

	if (!committed) {
		store_rollback(db);
		for (size_t i = 0; i < b->write_count; i++) /* the group stands or falls together */
			fail_write(&b->writes[i]);
	}
	/* Only now, after COMMIT, may a client learn that its write happened. */
	for (size_t i = 0; i < b->write_count; i++)
		answer(conns, b->writes[i].id, b->writes[i].reply, b->writes[i].close_after, on_done, context);
	b->write_count = 0;
}

/* ---- feed ---- */

typedef struct feed_render {
	writer *out;
	bool first;
} feed_render;

static void append_feed_post(const post_row *post, void *context)
{
	feed_render *render = context;

	if (!render->first)
		writer_literal(render->out, ",");
	render->first = false;
	post_json_write(render->out, post);
}

/* Renders {"posts":[...]} into `response`. Returns false if the query failed. */
static bool render_feed(store *db, http_response *response)
{
	feed_render render = { &response->body, true };

	http_response_begin(response, response->buffer, response->capacity);
	writer_literal(&response->body, "{\"posts\":[");
	bool queried = store_read_feed(db, append_feed_post, &render);

	writer_literal(&response->body, "]}");
	return queried;
}

static strview feed_reply(http_response *response, bool queried, bool close_after)
{
	strview reply = http_response_finish(response, HTTP_OK, close_after);

	return queried && reply.len != 0 ? reply : http_response_error(API_ERROR_INTERNAL, close_after);
}

#if COALESCE_FEEDS
static void run_feeds(batch *b, store *db, conn_table *conns, char *scratch, size_t scratch_cap, batch_done_fn on_done, void *context)
{
	http_response response = { .buffer = scratch, .capacity = scratch_cap };
	bool queried = render_feed(db, &response);

	/*
	 * http_response_finish writes the headers just before the body, so a second call with a
	 * different header length overwrites the first one. Send every keep-alive response first.
	 */
	for (int pass = 0; pass < 2; pass++) {
		bool close_pass = pass == 1;
		strview reply = feed_reply(&response, queried, close_pass);

		for (size_t i = 0; i < b->feed_count; i++) {
			if (b->feeds[i].close_after == close_pass)
				answer(conns, b->feeds[i].id, reply, close_pass, on_done, context);
		}
	}
	b->feed_count = 0;
}
#else
static void run_feeds(batch *b, store *db, conn_table *conns, char *scratch, size_t scratch_cap, batch_done_fn on_done, void *context)
{
	for (size_t i = 0; i < b->feed_count; i++) {
		http_response response = { .buffer = scratch, .capacity = scratch_cap };
		bool queried = render_feed(db, &response);
		bool close_after = b->feeds[i].close_after;

		answer(conns, b->feeds[i].id, feed_reply(&response, queried, close_after), close_after, on_done, context);
	}
	b->feed_count = 0;
}
#endif

void batch_run(batch *b, store *db, conn_table *conns, char *scratch, size_t scratch_cap, batch_done_fn on_done, void *context)
{
	if (b->write_count != 0)
		run_writes(b, db, conns, on_done, context);
	if (b->feed_count != 0)
		run_feeds(b, db, conns, scratch, scratch_cap, on_done, context);
}
