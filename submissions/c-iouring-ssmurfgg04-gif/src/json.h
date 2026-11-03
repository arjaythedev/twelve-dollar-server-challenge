/* json.h - just enough JSON: a validating scanner for named fields and a writer */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum { JT_NONE = 0, JT_STRING, JT_NUMBER, JT_OTHER } jt_t;

typedef struct {
    const char *name;   /* field to look for */
    const char *v, *ve; /* raw span of the value (strings: undecoded) */
    jt_t type;
} jfield_t;

/* Parses one JSON document from [s, s+n), ignoring surrounding whitespace.
 * Returns the root type (JT_NONE if invalid). When the root is an object,
 * fills in every jfield whose name matched (first occurrence wins; later
 * duplicates do not overwrite). Returns 1 on a valid document, 0 otherwise. */
int json_fields(const char *s, size_t n, jfield_t *f, int nf);

/* Decodes a JSON string span into dst (no NUL). Returns decoded length,
 * or (size_t)-1 on invalid escapes (caller sized dst with room for the
 * worst case: same as the raw span). */
size_t j_unescape(const char *src, const char *se, char *dst);

/* Escapes [s, s+n) into o as a JSON string body (no quotes): escapes
 * ", \ and C0 controls; everything else (incl. UTF-8) passes through.
 * Returns the end pointer. dst must have room for 6*n + 1. */
char *j_escape(char *o, const unsigned char *s, size_t n);
