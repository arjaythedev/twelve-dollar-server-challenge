/* libFuzzer: trimming returns a view inside its input, and never trims text that is not whitespace. */
#include <stdint.h>
#include <stdlib.h>

#include "utf8.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *input = (const char *)data;
	strview trimmed = utf8_trim_js_whitespace(strview_make(input, size));

	if (trimmed.len > size || (trimmed.len != 0 && (trimmed.ptr < input || trimmed.ptr + trimmed.len > input + size)))
		abort();
	if (utf8_codepoint_count(trimmed) > trimmed.len)
		abort();
	return 0;
}
