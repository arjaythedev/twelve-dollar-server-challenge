/*
 * http_response: builds "HTTP/1.1 <status>" messages with a JSON body in a single contiguous
 * buffer, so one write() sends the whole message.
 *
 * The body is written first, after a reserved gap; the headers (whose Content-Length depends on
 * the body size) are then written backwards into the gap.
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>

#include "strview.h"
#include "writer.h"

typedef enum http_status {
	HTTP_OK = 200,
	HTTP_CREATED = 201,
	HTTP_BAD_REQUEST = 400,
	HTTP_UNAUTHORIZED = 401,
	HTTP_NOT_FOUND = 404,
	HTTP_PAYLOAD_TOO_LARGE = 413,
	HTTP_INTERNAL_ERROR = 500,
	HTTP_SERVICE_UNAVAILABLE = 503,
} http_status;

typedef struct http_response {
	char *buffer;
	size_t capacity;
	writer body;   /* write the JSON body here */
} http_response;

/* Starts a response in `buffer`. The buffer must hold the headers (HTTP_HEADER_RESERVE bytes) and the body. */
void http_response_begin(http_response *response, char *buffer, size_t capacity);

/*
 * Completes the response and returns the whole message, which lies inside the buffer.
 * Returns an empty view when the body overflowed; callers then send the INTERNAL error instead.
 */
strview http_response_finish(http_response *response, http_status status, bool close_after);

/* The JSON errors of the API, each with a fixed status and message. */
typedef enum api_error {
	API_ERROR_NOT_FOUND,
	API_ERROR_INVALID_POST_ID,
	API_ERROR_POST_NOT_FOUND,
	API_ERROR_MISSING_TOKEN,
	API_ERROR_INVALID_TOKEN,
	API_ERROR_INVALID_PAYLOAD,
	API_ERROR_MALFORMED_JSON,
	API_ERROR_BODY_REQUIRED,
	API_ERROR_BODY_TOO_LONG,
	API_ERROR_INTERNAL,
	API_ERROR_BAD_REQUEST,
	API_ERROR_REQUEST_TOO_LARGE,
	API_ERROR_COUNT,
} api_error;

/* Builds the prebuilt error messages. Call once at startup. */
void http_response_init_errors(void);

/* The complete, prebuilt message for an error. */
strview http_response_error(api_error error, bool close_after);

#endif
