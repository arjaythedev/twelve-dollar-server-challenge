#include "writer.h"

/* Escape-table membership, indexed by byte. Everything else, including UTF-8, passes through. */
static bool needs_json_escape(unsigned char c)
{
	return c < 0x20 || c == '"' || c == '\\';
}

void writer_u64(writer *w, uint64_t value)
{
	char digits[20];
	size_t count = 0;

	do {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	} while (value != 0);

	if (writer_remaining(w) < count) {
		w->overflowed = true;
		return;
	}
	while (count != 0)
		*w->cursor++ = digits[--count];
}

static void write_json_escape_sequence(writer *w, unsigned char c)
{
	static const char hex[] = "0123456789abcdef";

	switch (c) {
	case '"':  writer_literal(w, "\\\""); break;
	case '\\': writer_literal(w, "\\\\"); break;
	case '\n': writer_literal(w, "\\n"); break;
	case '\r': writer_literal(w, "\\r"); break;
	case '\t': writer_literal(w, "\\t"); break;
	case '\b': writer_literal(w, "\\b"); break;
	case '\f': writer_literal(w, "\\f"); break;
	default: {
		const char unicode[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };

		writer_bytes(w, unicode, sizeof unicode);
	}
	}
}

void writer_json_escaped(writer *w, strview text)
{
	size_t pos = 0;

	while (pos < text.len) {
		size_t run_end = pos;

		while (run_end < text.len && !needs_json_escape((unsigned char)text.ptr[run_end]))
			run_end++;
		writer_bytes(w, text.ptr + pos, run_end - pos);
		if (run_end == text.len)
			return;
		write_json_escape_sequence(w, (unsigned char)text.ptr[run_end]);
		pos = run_end + 1;
	}
}
