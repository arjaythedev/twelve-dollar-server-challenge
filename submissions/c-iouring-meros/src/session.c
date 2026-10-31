#include "session.h"

#include "http_parse.h"
#include "http_response.h"
#include "router.h"

static const strview continue_response = { "HTTP/1.1 100 Continue\r\n\r\n", 25 };

static bool is_blocked(const conn *c)
{
	return conn_has(c, CONN_AWAITING_BATCH) || conn_has(c, CONN_SEND_PENDING) || conn_has(c, CONN_CLOSE_AFTER_SEND);
}

static void close_if_finished(app *a, int id)
{
	conn *c = conn_get(&a->conns, id);

	if (!conn_has(c, CONN_OPEN) || !conn_has(c, CONN_CLOSE_AFTER_SEND) ||
	    conn_has(c, CONN_SEND_PENDING) || conn_has(c, CONN_AWAITING_BATCH))
		return;
	if (conn_has(c, CONN_ABORTIVE))
		conn_close(&a->conns, id);
	else if (!conn_has(c, CONN_DRAINING))
		conn_drain_start(&a->conns, id, a->now_ns); /* let the client close first */
}

/* Keeps the bytes from `consumed` on for later; `input` may be the carry itself. */
static void keep_unconsumed(conn *c, const char *input, size_t len, size_t consumed)
{
	if (consumed == len || conn_has(c, CONN_CLOSE_AFTER_SEND))
		conn_carry_clear(c);
	else
		conn_carry_set(c, input + consumed, len - consumed);
}

static void reject(app *a, int id, api_error error)
{
	conn_send(&a->conns, id, http_response_error(error, true));
	conn_set(conn_get(&a->conns, id), CONN_CLOSE_AFTER_SEND | CONN_ABORTIVE);
}

/* Runs the complete requests in input[0..len). Stops at a request that must wait. */
static void process_input(app *a, int id, const char *input, size_t len)
{
	conn *c = conn_get(&a->conns, id);
	size_t consumed = 0;

	while (consumed < len && !is_blocked(c)) {
		http_request request;
		http_parse_result result = http_parse_request(strview_make(input + consumed, len - consumed), &request);

		if (result == HTTP_NEED_MORE) {
			if (request.expects_continue && request.headers_complete && !conn_has(c, CONN_SENT_CONTINUE)) {
				conn_set(c, CONN_SENT_CONTINUE);
				conn_send(&a->conns, id, continue_response);
			}
			break;
		}
		if (result != HTTP_COMPLETE) {
			reject(a, id, result == HTTP_TOO_LARGE ? API_ERROR_REQUEST_TOO_LARGE : API_ERROR_BAD_REQUEST);
			break;
		}
		conn_clear(c, CONN_SENT_CONTINUE);
		consumed += request.total_len;
		if (conn_table_is_crowded(&a->conns) || memwatch_should_shed(&a->memory))
			request.close_after = true; /* free the descriptor, or the socket's memory, for the next user */
		route_request(a, id, &request);
	}
	keep_unconsumed(c, input, len, consumed);
	close_if_finished(a, id);
}

/* Continues with carried input, if any. */
static void continue_carried_input(app *a, int id)
{
	conn *c = conn_get(&a->conns, id);

	if (c->carry_len != 0)
		process_input(a, id, c->carry, c->carry_len);
	else
		close_if_finished(a, id);
}

void session_on_data(app *a, int id, const char *data, size_t len)
{
	enum { CARRY_LIMIT = 2 * HTTP_MAX_REQUEST_BYTES };
	conn *c = conn_get(&a->conns, id);
	bool blocked = conn_has(c, CONN_AWAITING_BATCH) || conn_has(c, CONN_SEND_PENDING);

	c->last_active = (uint32_t)a->now;
	if (conn_has(c, CONN_DRAINING) || conn_has(c, CONN_CLOSE_AFTER_SEND))
		return; /* the response is out and the client is closing, or the connection is on its way out: noise to ignore */

	/* Parse straight from the caller's buffer; only bytes that must wait are copied into the connection. */
	if (!blocked && c->carry_len == 0) {
		process_input(a, id, data, len);
		return;
	}
	if ((size_t)c->carry_len + len > CARRY_LIMIT) {
		if (blocked) {
			/* A response is still due: it must go out first, so no error message jumps ahead of it. */
			conn_set(c, CONN_CLOSE_AFTER_SEND | CONN_ABORTIVE);
			conn_carry_clear(c);
		} else {
			reject(a, id, API_ERROR_REQUEST_TOO_LARGE);
			close_if_finished(a, id);
		}
		return;
	}
	conn_carry_append(c, data, len);
	if (!blocked) /* a partial request completed */
		process_input(a, id, c->carry, c->carry_len);
}

void session_on_eof(app *a, int id)
{
	conn *c = conn_get(&a->conns, id);

	if (conn_has(c, CONN_AWAITING_BATCH) || conn_has(c, CONN_SEND_PENDING)) {
		/* A response is on its way: let it go out, then close. */
		conn_set(c, CONN_CLOSE_AFTER_SEND | CONN_ABORTIVE);
		return;
	}
	conn_close(&a->conns, id);
}

void session_resume(app *a, int id)
{
	conn *c = conn_get(&a->conns, id);

	if (!conn_has(c, CONN_OPEN) || conn_has(c, CONN_SEND_PENDING))
		return; /* a send in progress resumes this when it completes */
	continue_carried_input(a, id);
}
