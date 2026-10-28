/* strview: a non-owning pointer + length pair. Nothing here allocates or NUL-terminates. */
#ifndef STRVIEW_H
#define STRVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct strview {
	const char *ptr;
	size_t len;
} strview;

/* A view of a string literal. The "" prefix makes the macro reject non-literals at compile time. */
#define STRVIEW(literal) ((strview){ "" literal, sizeof(literal) - 1 })

static inline strview strview_make(const char *ptr, size_t len)
{
	return (strview){ ptr, len };
}

static inline bool strview_equals(strview a, strview b)
{
	return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static inline bool strview_starts_with(strview s, strview prefix)
{
	return s.len >= prefix.len && memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

static inline bool strview_ends_with(strview s, strview suffix)
{
	return s.len >= suffix.len && memcmp(s.ptr + s.len - suffix.len, suffix.ptr, suffix.len) == 0;
}

/* The view without its first `n` bytes (n <= len) and without its last `n` bytes. */
static inline strview strview_drop_front(strview s, size_t n)
{
	return strview_make(s.ptr + n, s.len - n);
}

static inline strview strview_drop_back(strview s, size_t n)
{
	return strview_make(s.ptr, s.len - n);
}

#endif
