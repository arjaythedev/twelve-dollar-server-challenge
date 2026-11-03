#include "router.h"

#include "http_response.h"
#include "json_read.h"
#include "post_json.h"
#include "utf8.h"

enum {
	MAX_POST_CHARACTERS = 500,
	MAX_POST_ID_DIGITS = 18,
};

/* ---- sending ---- */

static void send_message(app *a, int id, strview message, bool close_after)
{
	conn_send(&a->conns, id, message);
	if (close_after)
		conn_set(conn_get(&a->conns, id), CONN_CLOSE_AFTER_SEND);
}

static void send_error(app *a, int id, api_error error, bool close_after)
{
	send_message(a, id, http_response_error(error, close_after), close_after);
}

static void send_response(app *a, int id, http_response *response, http_status status, bool close_after)
{
	strview message = http_response_finish(response, status, close_after);

	if (message.len == 0)
		send_error(a, id, API_ERROR_INTERNAL, close_after);
	else
		send_message(a, id, message, close_after);
}

/* ---- request parsing helpers ---- */

typedef enum post_id_check { POST_ID_VALID, POST_ID_INVALID, POST_ID_BEYOND_RANGE } post_id_check;

/*
 * A post id is a positive decimal integer. Text that is not one is invalid whatever its length;
 * a number with more digits than any real post id can have is "beyond range".
 */
static post_id_check parse_post_id(strview digits, int64_t *post_id)
{
	int64_t value = 0;
	bool nonzero = false;

	for (size_t i = 0; i < digits.len; i++) {
		if (digits.ptr[i] < '0' || digits.ptr[i] > '9')
			return POST_ID_INVALID;
		nonzero |= digits.ptr[i] != '0';
		if (digits.len <= MAX_POST_ID_DIGITS)
			value = value * 10 + (digits.ptr[i] - '0');
	}
	if (!nonzero)
		return POST_ID_INVALID;
	if (digits.len > MAX_POST_ID_DIGITS)
		return POST_ID_BEYOND_RANGE;
	*post_id = value;
	return POST_ID_VALID;
}

/* Answers an unusable id and returns false. */
static bool read_post_id(app *a, int id, const http_request *request, strview digits, int64_t *post_id)
{
	switch (parse_post_id(digits, post_id)) {
	case POST_ID_VALID:
		return true;
	case POST_ID_INVALID:
		send_error(a, id, API_ERROR_INVALID_POST_ID, request->close_after);
		return false;
	case POST_ID_BEYOND_RANGE:
		send_error(a, id, API_ERROR_POST_NOT_FOUND, request->close_after);
		return false;
	}
	return false;
}

/* Checks the Authorization header. On failure it answers and returns false. */
static bool authenticate(app *a, int id, const http_request *request, jwt_claims *claims)
{
	strview bearer = STRVIEW("Bearer ");

	if (!strview_starts_with(request->authorization, bearer)) {
		send_error(a, id, API_ERROR_MISSING_TOKEN, request->close_after);
		return false;
	}
	strview token = strview_drop_front(request->authorization, bearer.len);

	switch (jwt_verify(&a->jwt, token, a->now, claims)) {
	case JWT_OK:
		return true;
	case JWT_INVALID_PAYLOAD:
		send_error(a, id, API_ERROR_INVALID_PAYLOAD, request->close_after);
		return false;
	case JWT_INVALID_TOKEN:
		break;
	}
	send_error(a, id, API_ERROR_INVALID_TOKEN, request->close_after);
	return false;
}

/*
 * Reads {"body": "..."} from a request body. On success `text` is the trimmed post text, stored in
 * `buffer`. On failure `error` says why.
 */
static bool read_post_text(strview request_body, char *buffer, size_t buffer_cap, strview *text, api_error *error)
{
	json_field fields[] = { JSON_FIELD("body", buffer, buffer_cap) };

	switch (json_read_object(request_body, fields, 1)) {
	case JSON_DOC_INVALID:
		*error = API_ERROR_MALFORMED_JSON;
		return false;
	case JSON_DOC_NOT_OBJECT:
		*error = API_ERROR_BODY_REQUIRED;
		return false;
	case JSON_DOC_OBJECT:
		break;
	}
	if (fields[0].kind != JSON_STRING) {
		*error = API_ERROR_BODY_REQUIRED;
		return false;
	}
	if (fields[0].truncated) { /* cannot happen: the buffer holds a whole request, and decoding never grows text */
		*error = API_ERROR_INTERNAL;
		return false;
	}
	strview trimmed = utf8_trim_js_whitespace(strview_make(buffer, fields[0].text_len));

	if (trimmed.len == 0) {
		*error = API_ERROR_BODY_REQUIRED;
		return false;
	}
	if (utf8_codepoint_count(trimmed) > MAX_POST_CHARACTERS) {
		*error = API_ERROR_BODY_TOO_LONG;
		return false;
	}
	*text = trimmed;
	return true;
}

/* ---- endpoints ---- */

static void handle_health(app *a, int id, const http_request *request)
{
	http_response response;

	http_response_begin(&response, a->scratch, sizeof a->scratch);
	if (store_ping(a->db)) {
		writer_literal(&response.body, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
		writer_u64(&response.body, (uint64_t)(a->now - a->started_at));
		writer_literal(&response.body, "}");
		send_response(a, id, &response, HTTP_OK, request->close_after);
		return;
	}
	const char *reason = store_error(a->db);

	writer_literal(&response.body, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
	writer_json_escaped(&response.body, strview_make(reason, strlen(reason)));
	writer_literal(&response.body, "\"}");
	send_response(a, id, &response, HTTP_SERVICE_UNAVAILABLE, request->close_after);
}

static void write_post_object(const post_row *post, void *context)
{
	writer *out = context;

	writer_literal(out, "{\"post\":");
	post_json_write(out, post);
	writer_literal(out, "}");
}

static void handle_get_post(app *a, int id, const http_request *request, strview id_digits)
{
	int64_t post_id;
	http_response response;

	if (!read_post_id(a, id, request, id_digits, &post_id))
		return;
	http_response_begin(&response, a->scratch, sizeof a->scratch);
	switch (store_find_post(a->db, post_id, write_post_object, &response.body)) {
	case FIND_FOUND:
		send_response(a, id, &response, HTTP_OK, request->close_after);
		break;
	case FIND_MISSING:
		send_error(a, id, API_ERROR_POST_NOT_FOUND, request->close_after);
		break;
	case FIND_FAILED:
		send_error(a, id, API_ERROR_INTERNAL, request->close_after);
		break;
	}
}

static void handle_create_post(app *a, int id, const http_request *request)
{
	jwt_claims claims;
	strview text;
	api_error error;

	if (!authenticate(a, id, request, &claims))
		return;
	if (!read_post_text(request->body, a->decoded_body, sizeof a->decoded_body, &text, &error)) {
		send_error(a, id, error, request->close_after);
		return;
	}
	if (!batch_add_post(a->pending, &a->conns, id, request->close_after, claims.user_id,
	                    strview_make(claims.username, claims.username_len), text))
		send_error(a, id, API_ERROR_INTERNAL, request->close_after);
}

static void handle_like(app *a, int id, const http_request *request, strview id_digits)
{
	jwt_claims claims;
	int64_t post_id;

	if (!authenticate(a, id, request, &claims)) /* authentication comes before id validation */
		return;
	if (!read_post_id(a, id, request, id_digits, &post_id))
		return;
	batch_add_like(a->pending, &a->conns, id, request->close_after, claims.user_id, post_id);
}

/* ---- routing ---- */

void route_request(app *a, int id, const http_request *request)
{
	strview path = request->path;
	strview posts_prefix = STRVIEW("/posts/");

	if (request->method == HTTP_METHOD_GET) {
		if (strview_equals(path, STRVIEW("/feed")))
			batch_add_feed(a->pending, &a->conns, id, request->close_after);
		else if (strview_equals(path, STRVIEW("/health")))
			handle_health(a, id, request);
		else if (strview_starts_with(path, posts_prefix) && path.len > posts_prefix.len)
			handle_get_post(a, id, request, strview_drop_front(path, posts_prefix.len));
		else
			send_error(a, id, API_ERROR_NOT_FOUND, request->close_after);
		return;
	}
	if (request->method == HTTP_METHOD_POST) {
		strview like_suffix = STRVIEW("/like");

		if (strview_equals(path, STRVIEW("/posts"))) {
			handle_create_post(a, id, request);
			return;
		}
		if (strview_starts_with(path, posts_prefix) && strview_ends_with(path, like_suffix) &&
		    path.len > posts_prefix.len + like_suffix.len) {
			strview digits = strview_drop_back(strview_drop_front(path, posts_prefix.len), like_suffix.len);

			handle_like(a, id, request, digits);
			return;
		}
	}
	send_error(a, id, API_ERROR_NOT_FOUND, request->close_after);
}
