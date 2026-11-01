#include "base64url.h"
#include "check.h"

static bool decodes_to(const char *encoded, const char *expected)
{
	uint8_t out[64];
	size_t len;

	return base64url_decode(strview_make(encoded, strlen(encoded)), out, sizeof out, &len) &&
	       len == strlen(expected) && memcmp(out, expected, len) == 0;
}

static bool is_rejected(const char *encoded, size_t capacity)
{
	uint8_t out[64];
	size_t len;

	return !base64url_decode(strview_make(encoded, strlen(encoded)), out, capacity, &len);
}

int main(void)
{
	CHECK(decodes_to("", ""));
	CHECK(decodes_to("Zg", "f"));
	CHECK(decodes_to("Zm8", "fo"));
	CHECK(decodes_to("Zm9v", "foo"));
	CHECK(decodes_to("Zm9vYg", "foob"));
	CHECK(decodes_to("Zm9vYmFy", "foobar"));
	CHECK(decodes_to("_-8", "\xff\xef"));        /* '-' and '_' are the url-safe alphabet */

	CHECK(is_rejected("Zm9v=", 64));             /* padding is not allowed */
	CHECK(is_rejected("Zm+v", 64));              /* '+' belongs to plain base64 */
	CHECK(is_rejected("Zm/v", 64));
	CHECK(is_rejected("Z", 64));                 /* 6 bits cannot make a byte */
	CHECK(is_rejected("Zm9vYmFy", 5));           /* does not fit */
	TEST_MAIN_END();
}
