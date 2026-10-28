/* The few UTF-8 operations the API needs. Input is assumed to be valid UTF-8. */
#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>

#include "strview.h"

/* Writes the UTF-8 form of `codepoint` to `out` (room for 4 bytes) and returns its length. */
size_t utf8_encode(uint32_t codepoint, char out[4]);

/* Number of Unicode code points. This is what SQLite's length() counts for TEXT. */
size_t utf8_codepoint_count(strview text);

/* Removes the whitespace JavaScript's String.prototype.trim removes, from both ends. */
strview utf8_trim_js_whitespace(strview text);

#endif
