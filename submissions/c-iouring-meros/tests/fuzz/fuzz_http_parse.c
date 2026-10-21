/* libFuzzer: the HTTP parser must never crash, and every view it returns must lie inside its input. */
#include <stdint.h>
#include <stdlib.h>

#include "http_parse.h"

static int inside(const char *begin, size_t length, const char *outer, size_t outer_length)
{
	return length == 0 || (begin >= outer && begin + length <= outer + outer_length);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *input = (const char *)data;
	http_request request;
	http_parse_result result = http_parse_request(strview_make(input, size), &request);

	if (result == HTTP_COMPLETE) {
		if (request.total_len == 0 || request.total_len > size)
			abort();
		if (!inside(request.path.ptr, request.path.len, input, size) ||
		    !inside(request.authorization.ptr, request.authorization.len, input, size) ||
		    !inside(request.body.ptr, request.body.len, input, size))
			abort();
		if (request.body.len > request.total_len)
			abort();
	}
	return 0;
}
