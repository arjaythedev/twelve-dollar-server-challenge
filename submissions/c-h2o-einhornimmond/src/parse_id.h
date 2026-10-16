#ifndef PARSE_ID_H
#define PARSE_ID_H

#include <stddef.h>
#include <stdint.h>

/* A row id written in decimal digits and nothing else -- a path segment, the `sub` of a token.
 * The text is a slice: @p n bytes, no terminator.
 *
 * > 0 the id, 0 not a positive integer, -1 too long to be an id that exists. */
static inline int64_t parse_id(const char *s, size_t n)
{
    int64_t v = 0;

    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        if (i >= 18) /* 18 digits always fit an int64 */
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

#endif
