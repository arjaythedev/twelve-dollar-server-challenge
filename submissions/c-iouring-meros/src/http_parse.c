#include "http_parse.h"

#include <strings.h>

typedef struct header_scan {
	size_t content_length;
	bool chunked;
} header_scan;

static bool header_name_is(strview name, const char *expected, size_t expected_len)
{
	return name.len == expected_len && strncasecmp(name.ptr, expected, expected_len) == 0;
}
#define NAME_IS(name, literal) header_name_is((name), literal, sizeof(literal) - 1)

static bool contains_token_ignoring_case(strview value, const char *token, size_t token_len)
{
	for (size_t i = 0; i + token_len <= value.len; i++) {
		if (strncasecmp(value.ptr + i, token, token_len) == 0)
			return true;
	}
	return false;
}

/* The line ends at '\n'; a '\r' before it is not part of the line. */
static strview line_without_terminator(const char *start, const char *newline)
{
	if (newline > start && newline[-1] == '\r')
		newline--;
	return strview_make(start, (size_t)(newline - start));
}

static strview trim_blanks(strview s)
{
	while (s.len != 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t'))
		s = strview_drop_front(s, 1);
	while (s.len != 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t'))
		s = strview_drop_back(s, 1);
	return s;
}

/* "METHOD SP target SP HTTP/x.y" */
static bool parse_request_line(strview line, http_request *request)
{
	const char *first_space = memchr(line.ptr, ' ', line.len);

	if (first_space == NULL)
		return false;
	strview method = strview_make(line.ptr, (size_t)(first_space - line.ptr));
	strview rest = strview_make(first_space + 1, line.len - method.len - 1);
	const char *second_space = memchr(rest.ptr, ' ', rest.len);

	if (second_space == NULL)
		return false;
	request->method = strview_equals(method, STRVIEW("GET")) ? HTTP_METHOD_GET
	                : strview_equals(method, STRVIEW("POST")) ? HTTP_METHOD_POST
	                : HTTP_METHOD_OTHER;

	strview target = strview_make(rest.ptr, (size_t)(second_space - rest.ptr));
	const char *query = memchr(target.ptr, '?', target.len);

	request->path = query != NULL ? strview_make(target.ptr, (size_t)(query - target.ptr)) : target;
	strview version = strview_make(second_space + 1, rest.len - target.len - 1);

	bool http10 = strview_equals(version, STRVIEW("HTTP/1.0"));

	if (!http10 && !strview_equals(version, STRVIEW("HTTP/1.1")))
		return false;
	request->close_after = http10; /* until a Connection header says otherwise */
	return true;
}

static http_parse_result parse_content_length(strview value, size_t *length)
{
	size_t total = 0;

	if (value.len == 0)
		return HTTP_MALFORMED;
	for (size_t i = 0; i < value.len; i++) {
		if (value.ptr[i] < '0' || value.ptr[i] > '9')
			return HTTP_MALFORMED;
		total = total * 10 + (size_t)(value.ptr[i] - '0');
		if (total > HTTP_MAX_REQUEST_BYTES)
			return HTTP_TOO_LARGE;
	}
	*length = total;
	return HTTP_COMPLETE;
}

/* Applies one header line. Returns HTTP_COMPLETE to mean "fine, keep going". */
static http_parse_result apply_header(strview name, strview value, http_request *request, header_scan *scan)
{
	if (NAME_IS(name, "authorization")) {
		request->authorization = value;
	} else if (NAME_IS(name, "content-length")) {
		return parse_content_length(value, &scan->content_length);
	} else if (NAME_IS(name, "connection")) {
		if (contains_token_ignoring_case(value, "close", 5))
			request->close_after = true;
		else if (contains_token_ignoring_case(value, "keep-alive", 10))
			request->close_after = false;
	} else if (NAME_IS(name, "expect")) {
		request->expects_continue = contains_token_ignoring_case(value, "100-continue", 12);
	} else if (NAME_IS(name, "transfer-encoding")) {
		scan->chunked = true;
	}
	return HTTP_COMPLETE;
}

http_parse_result http_parse_request(strview input, http_request *request)
{
	const char *pos = input.ptr, *end = input.ptr + input.len;
	header_scan scan = { 0, false };

	memset(request, 0, sizeof *request);

	const char *newline = memchr(pos, '\n', input.len);

	if (newline == NULL)
		return input.len > HTTP_MAX_HEADER_BYTES ? HTTP_TOO_LARGE : HTTP_NEED_MORE;
	if (!parse_request_line(line_without_terminator(pos, newline), request))
		return HTTP_MALFORMED;
	pos = newline + 1;

	for (;;) {
		newline = memchr(pos, '\n', (size_t)(end - pos));
		if (newline == NULL)
			return input.len > HTTP_MAX_HEADER_BYTES ? HTTP_TOO_LARGE : HTTP_NEED_MORE;
		if ((size_t)(newline - input.ptr) > HTTP_MAX_HEADER_BYTES)
			return HTTP_TOO_LARGE;

		strview line = line_without_terminator(pos, newline);

		pos = newline + 1;
		if (line.len == 0)
			break; /* blank line: end of headers */

		const char *colon = memchr(line.ptr, ':', line.len);

		if (colon == NULL)
			return HTTP_MALFORMED;

		strview name = strview_make(line.ptr, (size_t)(colon - line.ptr));
		strview value = trim_blanks(strview_make(colon + 1, line.len - name.len - 1));
		http_parse_result result = apply_header(name, value, request, &scan);

		if (result != HTTP_COMPLETE)
			return result;
	}
	if (scan.chunked)
		return HTTP_MALFORMED; /* the API never needs chunked uploads */

	request->headers_complete = true;
	size_t header_len = (size_t)(pos - input.ptr);

	if (input.len - header_len < scan.content_length)
		return header_len + scan.content_length > HTTP_MAX_REQUEST_BYTES ? HTTP_TOO_LARGE : HTTP_NEED_MORE;
	request->body = strview_make(pos, scan.content_length);
	request->total_len = header_len + scan.content_length;
	return HTTP_COMPLETE;
}
