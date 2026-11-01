#include "http_response.h"

#include "fatal.h"

enum {
	HEADER_RESERVE = 160,    /* longest status line + headers: ~120 bytes with "Connection: close" */
	ERROR_MESSAGE_CAP = 352, /* one prebuilt error message */
};

#define STATUS_PREFIX(code_and_reason) \
	"HTTP/1.1 " code_and_reason "\r\nContent-Type: application/json\r\nContent-Length: "

/* http_response_finish writes the headers in front of the body, into HEADER_RESERVE bytes: the longest case must fit. */
_Static_assert(sizeof STATUS_PREFIX("500 Internal Server Error") - 1 + 20 + sizeof "\r\nConnection: close\r\n\r\n" - 1 <= HEADER_RESERVE,
               "the longest response headers fit in HEADER_RESERVE");

static strview status_prefix(http_status status)
{
	switch (status) {
	case HTTP_OK:                  return STRVIEW(STATUS_PREFIX("200 OK"));
	case HTTP_CREATED:             return STRVIEW(STATUS_PREFIX("201 Created"));
	case HTTP_BAD_REQUEST:         return STRVIEW(STATUS_PREFIX("400 Bad Request"));
	case HTTP_UNAUTHORIZED:        return STRVIEW(STATUS_PREFIX("401 Unauthorized"));
	case HTTP_NOT_FOUND:           return STRVIEW(STATUS_PREFIX("404 Not Found"));
	case HTTP_PAYLOAD_TOO_LARGE:   return STRVIEW(STATUS_PREFIX("413 Content Too Large"));
	case HTTP_SERVICE_UNAVAILABLE: return STRVIEW(STATUS_PREFIX("503 Service Unavailable"));
	case HTTP_INTERNAL_ERROR:
	default:                       return STRVIEW(STATUS_PREFIX("500 Internal Server Error"));
	}
}

void http_response_begin(http_response *response, char *buffer, size_t capacity)
{
	response->buffer = buffer;
	response->capacity = capacity;
	response->body = writer_make(buffer + HEADER_RESERVE, capacity - HEADER_RESERVE);
}

strview http_response_finish(http_response *response, http_status status, bool close_after)
{
	if (response->body.overflowed)
		return strview_make(NULL, 0);

	char *body_start = response->buffer + HEADER_RESERVE;
	size_t body_len = (size_t)(response->body.cursor - body_start);
	strview prefix = status_prefix(status);
	strview terminator = close_after ? STRVIEW("\r\nConnection: close\r\n\r\n") : STRVIEW("\r\n\r\n");

	char digits[20];
	size_t digit_count = 0;

	for (size_t rest = body_len; digit_count == 0 || rest != 0; rest /= 10)
		digits[digit_count++] = (char)('0' + rest % 10);

	char *start = body_start - (prefix.len + digit_count + terminator.len);
	writer header = writer_make(start, prefix.len + digit_count + terminator.len);

	writer_view(&header, prefix);
	while (digit_count != 0)
		writer_bytes(&header, &digits[--digit_count], 1);
	writer_view(&header, terminator);
	return strview_make(start, (size_t)(response->body.cursor - start));
}

/* ---- prebuilt errors ---- */

typedef struct error_message {
	char keep_alive[ERROR_MESSAGE_CAP];
	size_t keep_alive_len;
	char close[ERROR_MESSAGE_CAP];
	size_t close_len;
} error_message;

static error_message error_messages[API_ERROR_COUNT];

static const struct {
	http_status status;
	const char *text;
} error_definitions[API_ERROR_COUNT] = {
	[API_ERROR_NOT_FOUND]         = { HTTP_NOT_FOUND, "not found" },
	[API_ERROR_INVALID_POST_ID]   = { HTTP_BAD_REQUEST, "invalid post id" },
	[API_ERROR_POST_NOT_FOUND]    = { HTTP_NOT_FOUND, "post not found" },
	[API_ERROR_MISSING_TOKEN]     = { HTTP_UNAUTHORIZED, "missing bearer token" },
	[API_ERROR_INVALID_TOKEN]     = { HTTP_UNAUTHORIZED, "invalid or expired token" },
	[API_ERROR_INVALID_PAYLOAD]   = { HTTP_UNAUTHORIZED, "invalid token payload" },
	[API_ERROR_MALFORMED_JSON]    = { HTTP_BAD_REQUEST, "malformed JSON body" },
	[API_ERROR_BODY_REQUIRED]     = { HTTP_BAD_REQUEST, "body is required" },
	[API_ERROR_BODY_TOO_LONG]     = { HTTP_BAD_REQUEST, "body must be at most 500 characters" },
	[API_ERROR_INTERNAL]          = { HTTP_INTERNAL_ERROR, "internal server error" },
	[API_ERROR_BAD_REQUEST]       = { HTTP_BAD_REQUEST, "bad request" },
	[API_ERROR_REQUEST_TOO_LARGE] = { HTTP_PAYLOAD_TOO_LARGE, "request too large" },
};

static size_t build_error(char *destination, api_error error, bool close_after)
{
	char scratch[ERROR_MESSAGE_CAP + HEADER_RESERVE];
	http_response response;

	http_response_begin(&response, scratch, sizeof scratch);
	writer_literal(&response.body, "{\"error\":\"");
	writer_bytes(&response.body, error_definitions[error].text, strlen(error_definitions[error].text));
	writer_literal(&response.body, "\"}");

	strview message = http_response_finish(&response, error_definitions[error].status, close_after);

	if (message.len == 0)
		fatal("error message %d does not fit in %d bytes", (int)error, ERROR_MESSAGE_CAP);
	memcpy(destination, message.ptr, message.len);
	return message.len;
}

void http_response_init_errors(void)
{
	for (int e = 0; e < API_ERROR_COUNT; e++) {
		error_messages[e].keep_alive_len = build_error(error_messages[e].keep_alive, (api_error)e, false);
		error_messages[e].close_len = build_error(error_messages[e].close, (api_error)e, true);
	}
}

strview http_response_error(api_error error, bool close_after)
{
	const error_message *message = &error_messages[error];

	return close_after ? strview_make(message->close, message->close_len)
	                   : strview_make(message->keep_alive, message->keep_alive_len);
}
