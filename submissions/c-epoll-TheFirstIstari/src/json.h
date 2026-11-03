#pragma once
// Strict JSON validation + `body` extraction + writer escaping helpers.
#include <stddef.h>

enum { JSON_OK = 0, JSON_MALFORMED = 1, JSON_NO_BODY = 2 };

// Validate the whole document (depth-capped). If valid and the top-level
// object has a "body" string field, unescape it into out (cap bytes).
// Returns JSON_OK (body_len set), JSON_NO_BODY (valid, no string body),
// or JSON_MALFORMED.
int json_extract_body(const char *p, size_t n, char *out, size_t cap, size_t *body_len);

// Decode a JSON string (val includes quotes) into out. Returns bytes written,
// or -1 on invalid escapes. Shared by the JWT claim decoder.
int json_unescape(const char *val, size_t vlen, char *out, size_t cap);

// Escape s[0..n) as JSON string content (no quotes). Returns bytes written,
// or -1 if it does not fit. Assumes valid UTF-8 (passes through >= 0x20,
// escapes '"' '\\' and control chars as \u00XX).
int json_escape(const char *s, size_t n, char *out, size_t cap);

// Count Unicode scalar values; validates UTF-8. Returns 0 ok (*chars set),
// -1 invalid.
int utf8_count_chars(const char *s, size_t n, size_t *chars);
