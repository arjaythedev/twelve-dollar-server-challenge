import json
import unittest

from adapters.asgi_http import Dispatcher, Handlers
from domain.railway import TimedRunner
from domain.workflows import FeedApi
from tests.fixtures import FakeStore, FakeVerifier, FixedClock, bearer

GOOD = bearer({"sub": "1", "username": "alice"}).encode()


class FakeBudget:
    strained = False


class Dispatch(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        api = FeedApi(FakeStore({5: b'{"post":5}'}), FakeVerifier(), FixedClock(), decode_json=json.loads)
        self.other = []

        async def other(scope, receive, send):
            self.other.append(scope["type"])

        self.budget = FakeBudget()
        self.dispatcher = Dispatcher(Handlers(api), self.budget, other)

    async def request(self, method, path, body=b"", headers=()):
        sent = []

        async def receive():
            return {"type": "http.request", "body": body, "more_body": False}

        async def send(message):
            sent.append(message)

        scope = {"type": "http", "method": method, "path": path, "headers": list(headers)}
        await self.dispatcher(scope, receive, send)
        start, response = sent
        return start["status"], response["body"], dict(start["headers"])

    async def test_routes(self):
        self.assertEqual((await self.request("GET", "/feed"))[:2], (200, b'{"posts":[]}'))
        self.assertEqual((await self.request("GET", "/posts/5"))[:2], (200, b'{"post":5}'))
        self.assertEqual((await self.request("POST", "/posts/5/like", headers=[(b"authorization", GOOD)]))[:2],
                         (201, b'{"liked":true,"already_liked":false,"post_id":5}'))
        self.assertEqual((await self.request("POST", "/posts", b'{"body":"hi"}', [(b"authorization", GOOD)]))[:2],
                         (201, b'{"post":"new"}'))
        self.assertEqual((await self.request("GET", "/health"))[0], 200)

    async def test_post_id_reaches_the_handler_unvalidated(self):
        self.assertEqual((await self.request("GET", "/posts/abc"))[:2], (400, b'{"error":"invalid post id"}'))
        self.assertEqual((await self.request("GET", "/posts/6"))[:2], (404, b'{"error":"post not found"}'))
        self.assertEqual((await self.request("POST", "/posts/abc/like"))[:2],
                         (401, b'{"error":"missing bearer token"}'))

    async def test_unknown_paths(self):
        for path in ("/nope", "/posts/", "/posts/5/", "/posts/5/like/", "/posts//like", "/posts/1/2", "/feed/"):
            self.assertEqual((await self.request("GET", path))[:2], (404, b'{"error":"not found"}'), path)

    async def test_wrong_method(self):
        for method, path in (("POST", "/feed"), ("GET", "/posts"), ("GET", "/posts/5/like"), ("DELETE", "/posts/5")):
            self.assertEqual((await self.request(method, path))[0], 405, (method, path))

    async def test_headers_and_close_under_strain(self):
        _, body, headers = await self.request("GET", "/feed")
        self.assertEqual(headers, {b"content-type": b"application/json", b"content-length": b"%d" % len(body)})
        self.budget.strained = True
        self.assertEqual((await self.request("GET", "/feed"))[2][b"connection"], b"close")

    async def test_timed_requests_record_every_hop(self):
        recorded = []

        class Sink:
            def record(self, endpoint, spans):
                recorded.append((endpoint, [span.name for span in spans]))

        clock = FixedClock()
        api = FeedApi(FakeStore({5: b'{"post":5}'}), FakeVerifier(), clock, decode_json=json.loads,
                      run=TimedRunner(clock))
        self.dispatcher = Dispatcher(Handlers(api), self.budget, None, Sink(), clock)
        self.assertEqual((await self.request("GET", "/posts/5"))[:2], (200, b'{"post":5}'))
        await self.request("GET", "/nope")
        self.assertEqual(recorded, [
            ("GET /posts/:id", ["route", "parse id", "read post", "adapter glue", "send", "request"]),
            ("not found", ["route", "adapter glue", "send", "request"]),
        ])

    async def test_lifespan_goes_to_the_router(self):
        await self.dispatcher({"type": "lifespan"}, None, None)
        self.assertEqual(self.other, ["lifespan"])


if __name__ == "__main__":
    unittest.main()
