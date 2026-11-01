/* Minimal JSON: a validating scanner that extracts top-level string members,
 * and an escaper for output. Enough for the two documents this server reads
 * (the post body and the JWT payload) and nothing more. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdint.h>

enum json_status {
    JSON_OK = 0,
    JSON_MALFORMED,  /* not valid JSON */
    JSON_NOT_OBJECT, /* valid JSON but the top level is not an object */
};

/* A member of the top-level object the caller wants. The value is unescaped
 * into `buf` (UTF-8). `kind` reports what was found. */
enum json_kind { JSON_MISSING = 0, JSON_STRING, JSON_NUMBER, JSON_OTHER };

struct json_field {
    const char *name;
    char *buf;  /* output for strings; numbers are stored as text too */
    size_t cap; /* capacity of buf; strings longer than this set truncated */
    size_t len; /* output length */
    enum json_kind kind;
    int truncated; /* the string did not fit in buf */
};

/* Validates the whole document (depth-limited) and fills the requested
 * fields from the top-level object. Duplicate keys: the last one wins. */
enum json_status json_parse_object(const char *doc, size_t len, struct json_field *fields, size_t nfields);

/* Writes a JSON string literal (with quotes) escaping '"', '\\' and control
 * characters. Returns the bytes written, or 0 if it does not fit. */
size_t json_write_string(char *out, size_t cap, const char *s, size_t len);

/* Worst-case escaped size including the quotes. */
static inline size_t json_string_max(size_t len) {
    return len * 6 + 2;
}

#endif
