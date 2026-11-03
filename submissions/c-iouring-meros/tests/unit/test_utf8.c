#include "check.h"
#include "utf8.h"

static strview trim(const char *s)
{
	return utf8_trim_js_whitespace(strview_make(s, strlen(s)));
}

int main(void)
{
	CHECK_VIEW(trim("  hello \t\n"), "hello");
	CHECK_VIEW(trim("hello"), "hello");
	CHECK(trim("").len == 0);
	CHECK(trim(" \t\r\n\v\f ").len == 0);
	CHECK_VIEW(trim("a b"), "a b");

	/* Unicode whitespace that String.prototype.trim removes */
	CHECK_VIEW(trim("\xC2\xA0x\xC2\xA0"), "x");                    /* U+00A0 */
	CHECK_VIEW(trim("\xE2\x80\x83x\xE2\x80\xA8"), "x");            /* U+2003, U+2028 */
	CHECK_VIEW(trim("\xE3\x80\x80x\xEF\xBB\xBF"), "x");            /* U+3000, U+FEFF */
	CHECK_VIEW(trim("\xE1\x9A\x80x\xE2\x81\x9F"), "x");            /* U+1680, U+205F */

	/* Text that merely ends in similar bytes stays intact. */
	CHECK_VIEW(trim("caf\xC3\xA9"), "caf\xC3\xA9");
	CHECK_VIEW(trim("\xE2\x9C\x93"), "\xE2\x9C\x93");              /* U+2713 check mark */
	CHECK_VIEW(trim("\xE2\x80\x8Bz"), "\xE2\x80\x8Bz");            /* U+200B is not whitespace */

	CHECK(utf8_codepoint_count(STRVIEW("")) == 0);
	CHECK(utf8_codepoint_count(STRVIEW("abc")) == 3);
	CHECK(utf8_codepoint_count(STRVIEW("caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x98\x80")) == 8);

	char buffer[4];
	CHECK(utf8_encode('A', buffer) == 1 && buffer[0] == 'A');
	CHECK(utf8_encode(0xE9, buffer) == 2 && memcmp(buffer, "\xC3\xA9", 2) == 0);
	CHECK(utf8_encode(0x2713, buffer) == 3 && memcmp(buffer, "\xE2\x9C\x93", 3) == 0);
	CHECK(utf8_encode(0x1F600, buffer) == 4 && memcmp(buffer, "\xF0\x9F\x98\x80", 4) == 0);
	TEST_MAIN_END();
}
