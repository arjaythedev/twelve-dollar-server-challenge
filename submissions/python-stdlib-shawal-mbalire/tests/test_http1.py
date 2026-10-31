import unittest

from adapters.http1 import (MAX_BODY, MAX_HEAD, NEED_BODY_AFTER_CONTINUE, NEED_MORE, BadRequest, Request,
                            ResponseHeads, parse_request)

GET = b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\n"


def post(body: bytes, *headers: bytes) -> bytes:
    head = b"POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n" % len(body)
    return head + b"".join(header + b"\r\n" for header in headers) + b"\r\n" + body


class ParseRequest(unittest.TestCase):
    def test_get(self):
        self.assertEqual(parse_request(GET, 0), Request(b"GET", b"/feed", None, b"", True, len(GET)))

    def test_query_string_is_not_part_of_the_path(self):
        self.assertEqual(parse_request(b"GET /posts/5?x=1 HTTP/1.1\r\n\r\n", 0).path, b"/posts/5")

    def test_body_and_authorization_keep_their_case(self):
        raw = post(b'{"body":"hi"}', b"AUTHORIZATION:  Bearer AbC.dEf.GhI ")
        request = parse_request(raw, 0)
        self.assertEqual((request.body, request.authorization, request.end), (b'{"body":"hi"}', b"Bearer AbC.dEf.GhI", len(raw)))

    def test_incomplete(self):
        for cut in (5, len(GET) - 1):
            self.assertIs(parse_request(GET[:cut], 0), NEED_MORE)
        self.assertIs(parse_request(post(b"12345")[:-1], 0), NEED_MORE)

    def test_expect_continue_waits_for_the_body(self):
        raw = post(b"12345", b"Expect: 100-continue")
        self.assertIs(parse_request(raw[:-5], 0), NEED_BODY_AFTER_CONTINUE)
        self.assertEqual(parse_request(raw, 0).body, b"12345")

    def test_pipelined_requests(self):
        raw = GET + post(b"{}") + GET
        first = parse_request(raw, 0)
        second = parse_request(raw, first.end)
        third = parse_request(raw, second.end)
        self.assertEqual((first.method, second.method, second.body, third.end), (b"GET", b"POST", b"{}", len(raw)))

    def test_keep_alive(self):
        cases = {
            b"GET / HTTP/1.1\r\n\r\n": True,
            b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n": False,
            b"GET / HTTP/1.1\r\nconnection: Keep-Alive, Close\r\n\r\n": False,
            b"GET / HTTP/1.0\r\n\r\n": False,
            b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n": True,
        }
        for raw, keep_alive in cases.items():
            with self.subTest(raw=raw):
                self.assertIs(parse_request(raw, 0).keep_alive, keep_alive)

    def test_chunked(self):
        raw = (b"POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
               b"4\r\n{\"bo\r\n9;x=y\r\ndy\":\"hi\"}\r\n0\r\n\r\n")
        request = parse_request(raw, 0)
        self.assertEqual((request.body, request.end), (b'{"body":"hi"}', len(raw)))
        self.assertIs(parse_request(raw[:-2], 0), NEED_MORE)

    def test_leading_empty_lines_are_ignored(self):
        self.assertEqual(parse_request(b"\r\n" + GET, 0).end, len(GET) + 2)

    def test_bad_requests(self):
        cases = {
            b"GARBAGE\r\n\r\n": 400,
            b"GET /\r\n\r\n": 400,
            b"GET / HTTP/2.0\r\n\r\n": 400,
            b"GET  / HTTP/1.1\r\n\r\n": 400,
            b"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n": 400,
            b"POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n": 400,
            b"POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n": 400,
            b"POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n": 400,
            b"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0x1\r\n": 400,
            b"POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % (MAX_BODY + 1): 413,
            b"GET / HTTP/1.1\r\n" + b"X: " + b"a" * MAX_HEAD: 431,
        }
        for raw, status in cases.items():
            with self.subTest(raw=raw[:60]):
                with self.assertRaises(BadRequest) as caught:
                    parse_request(raw, 0)
                self.assertEqual(caught.exception.status, status)


class Heads(unittest.TestCase):
    def test_frame(self):
        heads = ResponseHeads(now=0)
        self.assertEqual(
            heads.frame(201, b"{}", close=False),
            b"HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
            b"Content-Length: 2\r\n\r\n{}",
        )
        self.assertIn(b"\r\nConnection: close\r\nContent-Length: 2\r\n\r\n{}", heads.frame(200, b"{}", close=True))

    def test_refresh_changes_the_date_once_a_second(self):
        heads = ResponseHeads(now=0)
        heads.refresh(0.9)
        self.assertIn(b"00:00:00 GMT", heads.frame(200, b"", False))
        heads.refresh(61.2)
        self.assertIn(b"00:01:01 GMT", heads.frame(200, b"", False))


if __name__ == "__main__":
    unittest.main()
