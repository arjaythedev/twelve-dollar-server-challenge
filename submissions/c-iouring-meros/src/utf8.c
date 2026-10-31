#include "utf8.h"

size_t utf8_encode(uint32_t codepoint, char out[4])
{
	if (codepoint < 0x80) {
		out[0] = (char)codepoint;
		return 1;
	}
	if (codepoint < 0x800) {
		out[0] = (char)(0xC0 | codepoint >> 6);
		out[1] = (char)(0x80 | (codepoint & 0x3F));
		return 2;
	}
	if (codepoint < 0x10000) {
		out[0] = (char)(0xE0 | codepoint >> 12);
		out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
		out[2] = (char)(0x80 | (codepoint & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | codepoint >> 18);
	out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
	out[3] = (char)(0x80 | (codepoint & 0x3F));
	return 4;
}

size_t utf8_codepoint_count(strview text)
{
	size_t count = 0;

	for (size_t i = 0; i < text.len; i++)
		count += ((unsigned char)text.ptr[i] & 0xC0) != 0x80; /* skip continuation bytes */
	return count;
}

/*
 * Length in bytes of the whitespace character that begins `s` (0 if none). The set is the
 * ECMAScript WhiteSpace and LineTerminator productions.
 */
static size_t whitespace_length(const unsigned char *s, size_t available)
{
	if (s[0] == ' ' || (s[0] >= '\t' && s[0] <= '\r'))
		return 1;
	if (available >= 2 && s[0] == 0xC2 && s[1] == 0xA0) /* U+00A0 */
		return 2;
	if (available < 3)
		return 0;
	if (s[0] == 0xE1 && s[1] == 0x9A && s[2] == 0x80) /* U+1680 */
		return 3;
	if (s[0] == 0xE2 && s[1] == 0x80 &&
	    ((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xA8 || s[2] == 0xA9 || s[2] == 0xAF)) /* U+2000..200A, 2028, 2029, 202F */
		return 3;
	if (s[0] == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) /* U+205F */
		return 3;
	if (s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80) /* U+3000 */
		return 3;
	if (s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) /* U+FEFF */
		return 3;
	return 0;
}

/* Length of the whitespace character that ends at s[len] (0 if none). */
static size_t whitespace_length_before_end(const unsigned char *s, size_t len)
{
	for (size_t width = 1; width <= 3 && width <= len; width++) {
		if (whitespace_length(s + len - width, width) == width)
			return width;
	}
	return 0;
}

strview utf8_trim_js_whitespace(strview text)
{
	const unsigned char *bytes = (const unsigned char *)text.ptr;
	size_t start = 0, end = text.len, width;

	while (start < end && (width = whitespace_length(bytes + start, end - start)) != 0)
		start += width;
	while (end > start && (width = whitespace_length_before_end(bytes + start, end - start)) != 0)
		end -= width;
	return strview_make(text.ptr + start, end - start);
}
