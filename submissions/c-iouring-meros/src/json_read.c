#include "json_read.h"

#include <stdint.h>
#include <stdlib.h>

#include "utf8.h"

enum { MAX_DEPTH = 32, MAX_KEY_LEN = 31, NUMBER_BUFFER = 40 };

typedef struct cursor {
	const char *pos;
	const char *end;
} cursor;

/* Where decoded string text goes. A NULL destination validates without storing. */
typedef struct text_sink {
	char *dest;
	size_t cap;
	size_t len;
	bool truncated;
} text_sink;

static void sink_put(text_sink *sink, const char *data, size_t len)
{
	if (sink->dest == NULL)
		return;
	if (sink->len + len > sink->cap) {
		sink->truncated = true;
		return;
	}
	memcpy(sink->dest + sink->len, data, len);
	sink->len += len;
}

static void skip_whitespace(cursor *c)
{
	while (c->pos < c->end && (*c->pos == ' ' || *c->pos == '\t' || *c->pos == '\n' || *c->pos == '\r'))
		c->pos++;
}

static bool at_end(const cursor *c)
{
	return c->pos == c->end;
}

static bool consume(cursor *c, char expected)
{
	if (at_end(c) || *c->pos != expected)
		return false;
	c->pos++;
	return true;
}

static bool consume_literal(cursor *c, const char *word, size_t len)
{
	if ((size_t)(c->end - c->pos) < len || memcmp(c->pos, word, len) != 0)
		return false;
	c->pos += len;
	return true;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Reads "XXXX" after "\u". Returns the 16-bit value, or -1. */
static int read_hex4(cursor *c)
{
	int value = 0;

	if (c->end - c->pos < 4)
		return -1;
	for (int i = 0; i < 4; i++) {
		int digit = hex_digit(c->pos[i]);

		if (digit < 0)
			return -1;
		value = value << 4 | digit;
	}
	c->pos += 4;
	return value;
}

static bool is_high_surrogate(int unit) { return unit >= 0xD800 && unit < 0xDC00; }
static bool is_low_surrogate(int unit) { return unit >= 0xDC00 && unit < 0xE000; }

/* Reads the escape after "\u" (a surrogate pair takes two) and stores its UTF-8 form. */
static bool read_unicode_escape(cursor *c, text_sink *sink)
{
	const uint32_t replacement = 0xFFFD; /* lone surrogates have no UTF-8 form */
	int unit = read_hex4(c);
	uint32_t codepoint = (uint32_t)unit;
	char encoded[4];

	if (unit < 0)
		return false;
	if (is_high_surrogate(unit)) {
		cursor lookahead = *c;
		int low;

		if (consume(&lookahead, '\\') && consume(&lookahead, 'u') &&
		    (low = read_hex4(&lookahead)) >= 0 && is_low_surrogate(low)) {
			codepoint = 0x10000 + (((uint32_t)unit - 0xD800) << 10) + ((uint32_t)low - 0xDC00);
			*c = lookahead;
		} else {
			codepoint = replacement;
		}
	} else if (is_low_surrogate(unit)) {
		codepoint = replacement;
	}
	sink_put(sink, encoded, utf8_encode(codepoint, encoded));
	return true;
}

/* Reads a string whose opening quote is at c->pos. */
static bool read_string(cursor *c, text_sink *sink)
{
	c->pos++;
	for (;;) {
		const char *run = c->pos;

		while (c->pos < c->end && *c->pos != '"' && *c->pos != '\\' && (unsigned char)*c->pos >= 0x20)
			c->pos++;
		if (at_end(c))
			return false;
		sink_put(sink, run, (size_t)(c->pos - run));

		char c0 = *c->pos++;

		if (c0 == '"')
			return true;
		if (c0 != '\\')
			return false; /* raw control character */
		if (at_end(c))
			return false;

		char escape = *c->pos++, decoded;

		switch (escape) {
		case '"': case '\\': case '/': decoded = escape; break;
		case 'b': decoded = '\b'; break;
		case 'f': decoded = '\f'; break;
		case 'n': decoded = '\n'; break;
		case 'r': decoded = '\r'; break;
		case 't': decoded = '\t'; break;
		case 'u':
			if (!read_unicode_escape(c, sink))
				return false;
			continue;
		default:
			return false;
		}
		sink_put(sink, &decoded, 1);
	}
}

static bool skip_digits(cursor *c)
{
	const char *start = c->pos;

	while (c->pos < c->end && *c->pos >= '0' && *c->pos <= '9')
		c->pos++;
	return c->pos != start;
}

static bool read_number(cursor *c, double *value)
{
	const char *start = c->pos;

	consume(c, '-');
	if (at_end(c))
		return false;
	if (*c->pos == '0')
		c->pos++;
	else if (!skip_digits(c))
		return false;
	if (consume(c, '.') && !skip_digits(c))
		return false;
	if (!at_end(c) && (*c->pos == 'e' || *c->pos == 'E')) {
		c->pos++;
		if (!at_end(c) && (*c->pos == '+' || *c->pos == '-'))
			c->pos++;
		if (!skip_digits(c))
			return false;
	}
	if (value != NULL) {
		char text[NUMBER_BUFFER];
		size_t len = (size_t)(c->pos - start);

		if (len >= sizeof text)
			len = sizeof text - 1;
		memcpy(text, start, len);
		text[len] = '\0';
		*value = strtod(text, NULL);
	}
	return true;
}

static bool skip_value(cursor *c, int depth);

static bool skip_discarded_string(cursor *c)
{
	text_sink discard = { 0 };

	return read_string(c, &discard);
}

/* After the opening bracket: elements or members separated by commas, up to `close`. */
static bool skip_container(cursor *c, int depth, char close, bool has_keys)
{
	skip_whitespace(c);
	if (consume(c, close))
		return true;
	for (;;) {
		if (has_keys) {
			skip_whitespace(c);
			if (at_end(c) || *c->pos != '"' || !skip_discarded_string(c))
				return false;
			skip_whitespace(c);
			if (!consume(c, ':'))
				return false;
		}
		if (!skip_value(c, depth + 1))
			return false;
		skip_whitespace(c);
		if (consume(c, close))
			return true;
		if (!consume(c, ','))
			return false;
	}
}

static bool skip_value(cursor *c, int depth)
{
	if (depth > MAX_DEPTH)
		return false;
	skip_whitespace(c);
	if (at_end(c))
		return false;
	switch (*c->pos) {
	case '"': return skip_discarded_string(c);
	case 't': return consume_literal(c, "true", 4);
	case 'f': return consume_literal(c, "false", 5);
	case 'n': return consume_literal(c, "null", 4);
	case '[': c->pos++; return skip_container(c, depth, ']', false);
	case '{': c->pos++; return skip_container(c, depth, '}', true);
	default:  return read_number(c, NULL);
	}
}

static json_field *find_field(json_field *fields, size_t count, const char *key, size_t key_len)
{
	for (size_t i = 0; i < count; i++) {
		if (fields[i].key_len == key_len && memcmp(fields[i].key, key, key_len) == 0)
			return &fields[i];
	}
	return NULL;
}

/* Reads the value at the cursor into `field` (or skips it when field is NULL). */
static bool read_member_value(cursor *c, json_field *field)
{
	if (field != NULL && *c->pos == '"') {
		text_sink sink = { field->text, field->text_cap, 0, false };

		if (!read_string(c, &sink))
			return false;
		field->kind = JSON_STRING;
		field->text_len = sink.len;
		field->truncated = sink.truncated;
		return true;
	}
	if (field != NULL && (*c->pos == '-' || (*c->pos >= '0' && *c->pos <= '9'))) {
		field->kind = JSON_NUMBER;
		return read_number(c, &field->number);
	}
	if (field != NULL)
		field->kind = JSON_OTHER;
	return skip_value(c, 1);
}

static bool read_members(cursor *c, json_field *fields, size_t field_count)
{
	for (;;) {
		char key[MAX_KEY_LEN];
		text_sink key_sink = { key, sizeof key, 0, false };

		skip_whitespace(c);
		if (at_end(c) || *c->pos != '"' || !read_string(c, &key_sink))
			return false;
		skip_whitespace(c);
		if (!consume(c, ':'))
			return false;
		skip_whitespace(c);
		if (at_end(c))
			return false;

		/* A key longer than the buffer cannot equal any requested key. */
		json_field *field = key_sink.truncated ? NULL : find_field(fields, field_count, key, key_sink.len);

		if (!read_member_value(c, field))
			return false;
		skip_whitespace(c);
		if (consume(c, '}'))
			return true;
		if (!consume(c, ','))
			return false;
	}
}

json_document json_read_object(strview document, json_field *fields, size_t field_count)
{
	cursor c = { document.ptr, document.ptr + document.len };

	for (size_t i = 0; i < field_count; i++) {
		fields[i].kind = JSON_ABSENT;
		fields[i].text_len = 0;
		fields[i].truncated = false;
		fields[i].number = 0;
	}

	skip_whitespace(&c);
	if (at_end(&c))
		return JSON_DOC_INVALID;
	if (*c.pos != '{') {
		if (!skip_value(&c, 0))
			return JSON_DOC_INVALID;
		skip_whitespace(&c);
		return at_end(&c) ? JSON_DOC_NOT_OBJECT : JSON_DOC_INVALID;
	}
	c.pos++;
	skip_whitespace(&c);
	if (!consume(&c, '}') && !read_members(&c, fields, field_count))
		return JSON_DOC_INVALID;
	skip_whitespace(&c);
	return at_end(&c) ? JSON_DOC_OBJECT : JSON_DOC_INVALID;
}
