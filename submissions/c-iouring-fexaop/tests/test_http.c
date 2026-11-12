#include <stdio.h>
#include <string.h>

#include "../src/http.h"

static int failures;

static void check(int cond, const char *label)
{
    if (cond) {
        printf("PASS %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures++;
    }
}

int main(void)
{
    struct http_request req;
    size_t used;
    enum http_result rr;

    /* A plain request: no auth, empty body, consumed is exactly the request length. */
    static const char r_feed[] = "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n";
    rr = http_parse(r_feed, sizeof(r_feed) - 1, &req, &used);
    check(rr == HTTP_DONE, "plain GET is DONE");
    check(req.method_len == 3 && memcmp(req.method, "GET", 3) == 0, "GET method");
    check(req.path_len == 5 && memcmp(req.path, "/feed", 5) == 0, "path is /feed");
    check(req.has_auth == 0, "no Authorization header");
    check(req.body_len == 0, "empty body");
    check(used == sizeof(r_feed) - 1, "consumed equals request length");

    /* The query string is stripped from the path. */
    static const char r_query[] = "GET /posts/12?x=1 HTTP/1.1\r\n\r\n";
    rr = http_parse(r_query, sizeof(r_query) - 1, &req, &used);
    check(rr == HTTP_DONE, "request with query is DONE");
    check(req.path_len == 9 && memcmp(req.path, "/posts/12", 9) == 0, "path drops query string");

    /* Authorization (with surrounding whitespace trimmed) and a body. */
    static const char r_post[] =
        "POST /posts HTTP/1.1\r\n"
        "Authorization: \t Bearer abc  \r\n"
        "Content-Length: 7\r\n"
        "\r\n"
        "{\"a\":1}";
    rr = http_parse(r_post, sizeof(r_post) - 1, &req, &used);
    check(rr == HTTP_DONE, "POST with auth and body is DONE");
    check(req.has_auth == 1 && req.auth_len == 10 && memcmp(req.auth, "Bearer abc", 10) == 0,
          "Authorization value is trimmed");
    check(req.body_len == 7 && memcmp(req.body, "{\"a\":1}", 7) == 0, "body pointer and length");
    check(used == sizeof(r_post) - 1, "POST consumed equals request length");

    /* Every proper prefix of a valid request is PARTIAL; the full request is DONE. */
    {
        int all_partial = 1;
        size_t i;

        for (i = 0; i < sizeof(r_feed) - 1; i++) {
            rr = http_parse(r_feed, i, &req, &used);
            if (rr != HTTP_PARTIAL) {
                printf("FAIL split at %zu returned %d\n", i, (int)rr);
                all_partial = 0;
                failures++;
                break;
            }
        }
        if (all_partial)
            printf("PASS every split prefix (0..%zu) is PARTIAL\n", sizeof(r_feed) - 2);
        else
            printf("FAIL split prefixes are not all PARTIAL\n");
        rr = http_parse(r_feed, sizeof(r_feed) - 1, &req, &used);
        check(rr == HTTP_DONE, "complete request after split is DONE");
    }

    /* Pipelined requests: only the first is parsed. */
    static const char r_two[] =
        "GET /feed HTTP/1.1\r\n\r\n"
        "GET /posts/1 HTTP/1.1\r\n\r\n";
    rr = http_parse(r_two, sizeof(r_two) - 1, &req, &used);
    check(rr == HTTP_DONE, "pipelined requests are DONE");
    check(used == sizeof("GET /feed HTTP/1.1\r\n\r\n") - 1, "only the first pipelined request is consumed");
    check(req.path_len == 5 && memcmp(req.path, "/feed", 5) == 0, "pipelined first path is /feed");

    /* Header names are case-insensitive. */
    static const char r_lower[] =
        "POST /posts HTTP/1.1\r\n"
        "authorization: Bearer abc\r\n"
        "content-length: 3\r\n"
        "\r\n"
        "xyz";
    rr = http_parse(r_lower, sizeof(r_lower) - 1, &req, &used);
    check(rr == HTTP_DONE, "lowercase headers are DONE");
    check(req.has_auth == 1 && req.auth_len == 10 && memcmp(req.auth, "Bearer abc", 10) == 0,
          "lowercase authorization parsed");
    check(req.body_len == 3 && memcmp(req.body, "xyz", 3) == 0, "lowercase content-length parsed");

    /* Content-Length: zero is a valid empty body. */
    static const char r_zero[] = "POST /posts HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    rr = http_parse(r_zero, sizeof(r_zero) - 1, &req, &used);
    check(rr == HTTP_DONE && req.body_len == 0, "Content-Length 0 is DONE with empty body");

    /* Bad Content-Length values. */
    {
        static const char *const bad[] = {"-1", "abc", "10000000000", "", "+5", "1 2"};
        size_t i;
        int ok = 1;

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            char b[256];
            int n = snprintf(b, sizeof(b), "POST /posts HTTP/1.1\r\nContent-Length: %s\r\n\r\n", bad[i]);

            rr = http_parse(b, (size_t)n, &req, &used);
            if (rr != HTTP_BAD) {
                printf("FAIL Content-Length '%s' returned %d\n", bad[i], (int)rr);
                ok = 0;
                failures++;
            }
        }
        if (ok)
            printf("PASS bad Content-Length values are BAD\n");
    }

    /* Transfer-Encoding is never supported. */
    static const char r_te[] = "POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    rr = http_parse(r_te, sizeof(r_te) - 1, &req, &used);
    check(rr == HTTP_BAD, "Transfer-Encoding: chunked is BAD");
    static const char r_te_lower[] = "POST /posts HTTP/1.1\r\ntransfer-encoding: gzip\r\n\r\n";
    rr = http_parse(r_te_lower, sizeof(r_te_lower) - 1, &req, &used);
    check(rr == HTTP_BAD, "lowercase transfer-encoding is BAD");

    /* Malformed request lines. */
    static const char r_no_version[] = "GET /feed\r\n\r\n";
    rr = http_parse(r_no_version, sizeof(r_no_version) - 1, &req, &used);
    check(rr == HTTP_BAD, "missing HTTP version is BAD");
    static const char r_no_version2[] = "GET /feed \r\n\r\n";
    rr = http_parse(r_no_version2, sizeof(r_no_version2) - 1, &req, &used);
    check(rr == HTTP_BAD, "empty HTTP version is BAD");
    static const char r_bad_version[] = "GET /feed HTTP/2.0\r\n\r\n";
    rr = http_parse(r_bad_version, sizeof(r_bad_version) - 1, &req, &used);
    check(rr == HTTP_BAD, "unsupported HTTP version is BAD");
    static const char r_no_method[] = " /feed HTTP/1.1\r\n\r\n";
    rr = http_parse(r_no_method, sizeof(r_no_method) - 1, &req, &used);
    check(rr == HTTP_BAD, "empty method is BAD");
    static const char r_no_target[] = "GET  HTTP/1.1\r\n\r\n";
    rr = http_parse(r_no_target, sizeof(r_no_target) - 1, &req, &used);
    check(rr == HTTP_BAD, "empty target is BAD");

    /* An incomplete body is PARTIAL. */
    static const char r_partial_body[] =
        "POST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\n12345";
    rr = http_parse(r_partial_body, sizeof(r_partial_body) - 1, &req, &used);
    check(rr == HTTP_PARTIAL, "incomplete body is PARTIAL");

    /* Bare LF terminators are tolerated. */
    static const char r_lf[] = "GET /feed HTTP/1.0\nHost: x\n\n";
    rr = http_parse(r_lf, sizeof(r_lf) - 1, &req, &used);
    check(rr == HTTP_DONE, "bare LF request is DONE");

    /* Head over the 16 KiB limit is BAD. */
    {
        char big[20000];
        size_t n = 0;

        n += (size_t)snprintf(big + n, sizeof(big) - n, "GET /feed HTTP/1.1\r\n");
        while (n < 17000 && n + 32 < sizeof(big))
            n += (size_t)snprintf(big + n, sizeof(big) - n, "X-Pad: padding\r\n");
        rr = http_parse(big, n, &req, &used);
        check(rr == HTTP_BAD, "unterminated head over 16 KiB is BAD");

        n += (size_t)snprintf(big + n, sizeof(big) - n, "\r\n");
        rr = http_parse(big, n, &req, &used);
        check(rr == HTTP_BAD, "terminated head over 16 KiB is BAD");
    }

    if (failures != 0) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
