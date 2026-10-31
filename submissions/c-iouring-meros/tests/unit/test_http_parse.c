#include "check.h"
#include "http_parse.h"

static http_parse_result parse(const char *text, http_request *request)
{
	return http_parse_request(strview_make(text, strlen(text)), request);
}

int main(void)
{
	http_request r;

	/* a GET with headers in mixed case */
	const char *get = "GET /posts/7?x=1 HTTP/1.1\r\nHost: a\r\nAUTHORIZATION:   Bearer tok  \r\n\r\n";
	CHECK(parse(get, &r) == HTTP_COMPLETE);
	CHECK(r.method == HTTP_METHOD_GET);
	CHECK_VIEW(r.path, "/posts/7");
	CHECK_VIEW(r.authorization, "Bearer tok");
	CHECK(!r.close_after && r.total_len == strlen(get) && r.body.len == 0);

	/* a POST with a body; the next request starts after total_len */
	const char *post = "POST /posts HTTP/1.1\r\ncontent-length: 13\r\n\r\n{\"body\":\"x\"}\n";
	CHECK(parse(post, &r) == HTTP_COMPLETE);
	CHECK(r.method == HTTP_METHOD_POST);
	CHECK_VIEW(r.body, "{\"body\":\"x\"}\n");
	CHECK(r.total_len == strlen(post));

	/* pipelining: only the first request is consumed */
	{
		const char *two = "GET /feed HTTP/1.1\r\n\r\nGET /health HTTP/1.1\r\n\r\n";
		CHECK(parse(two, &r) == HTTP_COMPLETE);
		CHECK(r.total_len == strlen("GET /feed HTTP/1.1\r\n\r\n"));
	}

	/* bare LF line endings are tolerated */
	CHECK(parse("GET /feed HTTP/1.1\n\n", &r) == HTTP_COMPLETE);

	/* incomplete input */
	CHECK(parse("GET /fe", &r) == HTTP_NEED_MORE);
	CHECK(parse("GET /feed HTTP/1.1\r\nHost: a\r\n", &r) == HTTP_NEED_MORE && !r.headers_complete);
	CHECK(parse("POST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\n{\"b", &r) == HTTP_NEED_MORE && r.headers_complete);

	/* Expect: 100-continue is reported */
	CHECK(parse("POST /posts HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n", &r) == HTTP_NEED_MORE);
	CHECK(r.expects_continue && r.headers_complete);

	/* connection handling */
	CHECK(parse("GET /feed HTTP/1.1\r\nConnection: close\r\n\r\n", &r) == HTTP_COMPLETE && r.close_after);
	CHECK(parse("GET /feed HTTP/1.0\r\n\r\n", &r) == HTTP_COMPLETE && r.close_after);
	CHECK(parse("GET /feed HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &r) == HTTP_COMPLETE && !r.close_after);

	/* other methods */
	CHECK(parse("DELETE /feed HTTP/1.1\r\n\r\n", &r) == HTTP_COMPLETE && r.method == HTTP_METHOD_OTHER);

	/* errors */
	CHECK(parse("GARBAGE\r\n\r\n", &r) == HTTP_MALFORMED);
	CHECK(parse("THIS IS NOT HTTP\r\n\r\n", &r) == HTTP_MALFORMED);
	CHECK(parse("GET /feed HTTP/2.0\r\n\r\n", &r) == HTTP_MALFORMED);
	CHECK(parse("GET /feed HTTP/1.1\r\nno colon here\r\n\r\n", &r) == HTTP_MALFORMED);
	CHECK(parse("POST /posts HTTP/1.1\r\nContent-Length: abc\r\n\r\n", &r) == HTTP_MALFORMED);
	CHECK(parse("POST /posts HTTP/1.1\r\nContent-Length: 999999\r\n\r\n", &r) == HTTP_TOO_LARGE);
	CHECK(parse("POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == HTTP_MALFORMED);
	{
		char huge[HTTP_MAX_HEADER_BYTES + 100];

		memset(huge, 'a', sizeof huge - 1);
		huge[sizeof huge - 1] = '\0';
		CHECK(parse(huge, &r) == HTTP_TOO_LARGE);
	}
	TEST_MAIN_END();
}
