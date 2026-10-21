#include "check.h"
#include "http_response.h"
#include "writer.h"

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
