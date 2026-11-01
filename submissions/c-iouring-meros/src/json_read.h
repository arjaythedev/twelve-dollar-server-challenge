/*
 * json_read: validates a JSON document and extracts chosen members of its top-level object.
 *
 * The whole document is validated (RFC 8259 grammar, nesting depth <= 32), but only the fields
 * the caller asks for are decoded. No allocation.
 */
#ifndef JSON_READ_H
#define JSON_READ_H

#include <stdbool.h>
#include <stddef.h>

#include "strview.h"

typedef enum json_kind {
	JSON_ABSENT,  /* the key does not occur */
	JSON_STRING,
	JSON_NUMBER,
	JSON_OTHER,   /* true, false, null, array or object */
} json_kind;

/* One member to look for. Fill the "request" part; json_read_object fills the "result" part. */
typedef struct json_field {
	/* request */
	const char *key;
	size_t key_len;
	char *text;        /* where to decode a string value as UTF-8; NULL to discard the text */
	size_t text_cap;
	/* result: describes the last occurrence of the key */
	json_kind kind;
	size_t text_len;
	bool truncated;    /* the string did not fit in text_cap: text and text_len are incomplete, do not use them */
	double number;
} json_field;

#define JSON_FIELD(KEY, BUFFER, CAPACITY) \
	{ .key = (KEY), .key_len = sizeof(KEY) - 1, .text = (BUFFER), .text_cap = (CAPACITY) }

typedef enum json_document {
	JSON_DOC_INVALID,
	JSON_DOC_OBJECT,
	JSON_DOC_NOT_OBJECT, /* valid JSON, but a scalar or an array */
} json_document;

json_document json_read_object(strview document, json_field *fields, size_t field_count);

#endif
