/* Base64url without padding (RFC 4648 section 5), strict decoder. */
#ifndef BASE64URL_H
#define BASE64URL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "strview.h"

/*
 * Decodes `encoded` into `out`. Fails on a character outside the alphabet (including '=', '+', '/'),
 * on an impossible length, and when the result does not fit in `capacity`.
 */
bool base64url_decode(strview encoded, uint8_t *out, size_t capacity, size_t *decoded_len);

#endif
