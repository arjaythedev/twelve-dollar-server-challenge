/*
 * writer: a bounds-checked cursor over a caller-owned buffer.
 *
 * Every write checks the remaining space. A write that does not fit sets `overflowed` and
 * writes nothing, so callers can build a whole response and test the flag once at the end.
 */
#ifndef WRITER_H
#define WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "strview.h"

typedef struct writer {
	char *cursor;
	char *end;
	bool overflowed;
} writer;

static inline writer writer_make(char *buffer, size_t capacity)
{
	return (writer){ buffer, buffer + capacity, false };
}

static inline size_t writer_remaining(const writer *w)
{
	return (size_t)(w->end - w->cursor);
}

static inline void writer_bytes(writer *w, const char *data, size_t len)
{
	if (writer_remaining(w) < len) {
		w->overflowed = true;
		return;
	}
	memcpy(w->cursor, data, len);
	w->cursor += len;
}

static inline void writer_view(writer *w, strview s)
{
	writer_bytes(w, s.ptr, s.len);
}

#define writer_literal(w, literal) writer_bytes((w), "" literal, sizeof(literal) - 1)

void writer_u64(writer *w, uint64_t value);

/* Appends the text as the inside of a JSON string: escapes '"', '\' and control characters. */
void writer_json_escaped(writer *w, strview text);

#endif
