#include "base64url.h"

#define INVALID (-1)

static int sextet_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '-')
		return 62;
	if (c == '_')
		return 63;
	return INVALID;
}

bool base64url_decode(strview encoded, uint8_t *out, size_t capacity, size_t *decoded_len)
{
	uint32_t accumulator = 0;
	unsigned bit_count = 0;
	size_t written = 0;

	if (encoded.len % 4 == 1)
		return false; /* six bits cannot make a byte */
	for (size_t i = 0; i < encoded.len; i++) {
		int value = sextet_value(encoded.ptr[i]);

		if (value == INVALID)
			return false;
		accumulator = accumulator << 6 | (uint32_t)value;
		bit_count += 6;
		if (bit_count >= 8) {
			bit_count -= 8;
			if (written == capacity)
				return false;
			out[written++] = (uint8_t)(accumulator >> bit_count);
		}
	}
	*decoded_len = written;
	return true;
}
