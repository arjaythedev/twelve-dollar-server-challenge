#include <string.h>

#include "check.h"
#include "http_response.h"
#include "writer.h"


/* The obvious byte-at-a-time escaper, to compare the fast scan against. */
static size_t reference_escape(const unsigned char *in, size_t len, char *out)
{
	static const char hex[] = "0123456789abcdef";
	size_t n = 0;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = in[i];

		switch (c) {
		case '"':  out[n++] = '\\'; out[n++] = '"'; break;
		case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
		case '\n': out[n++] = '\\'; out[n++] = 'n'; break;
		case '\r': out[n++] = '\\'; out[n++] = 'r'; break;
		case '\t': out[n++] = '\\'; out[n++] = 't'; break;
		case '\b': out[n++] = '\\'; out[n++] = 'b'; break;
		case '\f': out[n++] = '\\'; out[n++] = 'f'; break;
		default:
			if (c < 0x20) {
				memcpy(out + n, "\\u00", 4);
				out[n + 4] = hex[c >> 4];
				out[n + 5] = hex[c & 15];
				n += 6;
			} else {
				out[n++] = (char)c;
			}
		}
	}
	return n;
}

static void check_escape_against_reference(void)
{
	static const unsigned char specials[] = { '"', '\\', '\n', 0x00, 0x1f, 0x20, 0x21, 0x7f, 0x80, 0xc3, 0xff };
	unsigned char input[96];
	char expected[96 * 6], actual[96 * 6];

	for (size_t len = 0; len <= 70; len++) {
		for (size_t at = 0; at < len; at++) {
			for (size_t k = 0; k < sizeof specials; k++) {
				memset(input, 'a', sizeof input);
				input[at] = specials[k];
				if (at + 5 < len)
					input[at + 5] = specials[(k + 3) % sizeof specials];
				size_t want = reference_escape(input, len, expected);
				writer w = writer_make(actual, sizeof actual);

				writer_json_escaped(&w, strview_make((const char *)input, len));
				CHECK(!w.overflowed && (size_t)(w.cursor - actual) == want && memcmp(actual, expected, want) == 0);
			}
		}
	}
}

int main(void)
{
	char buffer[64];
	writer w = writer_make(buffer, sizeof buffer);

	writer_literal(&w, "n=");
	writer_u64(&w, 0);
	writer_literal(&w, ",");
	writer_u64(&w, 1234567890123ULL);
	CHECK(!w.overflowed && (size_t)(w.cursor - buffer) == 17 && memcmp(buffer, "n=0,1234567890123", 17) == 0);

	/* escaping */
	w = writer_make(buffer, sizeof buffer);
	writer_json_escaped(&w, STRVIEW("a\"b\\c\n\t\x01 caf\xC3\xA9 </>"));
	CHECK(!w.overflowed);
	CHECK((size_t)(w.cursor - buffer) == strlen("a\\\"b\\\\c\\n\\t\\u0001 caf\xC3\xA9 </>") &&
	      memcmp(buffer, "a\\\"b\\\\c\\n\\t\\u0001 caf\xC3\xA9 </>", (size_t)(w.cursor - buffer)) == 0);

	check_escape_against_reference();

	/* overflow is sticky and writes nothing partial */
	{
		char tiny[4];
		writer t = writer_make(tiny, sizeof tiny);

		writer_literal(&t, "abc");
		writer_literal(&t, "de");
		CHECK(t.overflowed && (size_t)(t.cursor - tiny) == 3);
	}

	/* a response: headers in front of the body, Content-Length matches */
	{
		char out[512];
		http_response response;

		http_response_begin(&response, out, sizeof out);
		writer_literal(&response.body, "{\"a\":1}");
		strview message = http_response_finish(&response, HTTP_CREATED, false);
		const char *expected = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}";
		CHECK(message.len == strlen(expected) && memcmp(message.ptr, expected, message.len) == 0);

		message = http_response_finish(&response, HTTP_OK, true);
		expected = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 7\r\nConnection: close\r\n\r\n{\"a\":1}";
		CHECK(message.len == strlen(expected) && memcmp(message.ptr, expected, message.len) == 0);
	}

	/* an overflowed body yields an empty message */
	{
		char out[200];
		http_response response;

		http_response_begin(&response, out, sizeof out);
		for (int i = 0; i < 10; i++)
			writer_literal(&response.body, "0123456789");
		CHECK(http_response_finish(&response, HTTP_OK, false).len == 0);
	}

	/* the prebuilt errors */
	http_response_init_errors();
	{
		strview e = http_response_error(API_ERROR_POST_NOT_FOUND, false);
		const char *expected = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 26\r\n\r\n{\"error\":\"post not found\"}";
		CHECK(e.len == strlen(expected) && memcmp(e.ptr, expected, e.len) == 0);
		e = http_response_error(API_ERROR_BODY_TOO_LONG, true);
		CHECK(e.len != 0 && strstr(e.ptr, "Connection: close") != NULL);
	}
	TEST_MAIN_END();
}
