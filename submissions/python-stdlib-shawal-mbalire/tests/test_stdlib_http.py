import asyncio
import unittest

from adapters.http1 import Request
from adapters.json_codec import decode_json
from adapters.stdlib_http import Dispatcher, Handlers, HttpConnection, HttpServer
from domain.models import LikeOutcome
from domain.railway import TimedRunner
from domain.workflows import FeedApi
from tests.fixtures import FakeStore, FakeVerifier, FixedClock, bearer

GOOD = bearer({"sub": "1", "username": "alice"}).encode()


class FakeBudget:
    strained = False


class SlowStore(FakeStore):
    """Likes wait for `commit` to be released, like a write waiting for its group commit."""

    def __init__(self, posts):
        super().__init__(posts)
        self.commit: asyncio.Future | None = None

    async def like(self, user_id: int, post_id: int) -> LikeOutcome:
        outcome = await super().like(user_id, post_id)
        self.commit = asyncio.get_running_loop().create_future()
        await self.commit
        return outcome


class FakeTransport:
    def __init__(self):
        self.written = b""
        self.closed = False

    def write(self, data: bytes) -> None:
        self.written += data

    def close(self) -> None:
        self.closed = True

    def abort(self) -> None:
        self.closed = True


def dispatcher_for(store, **timing) -> Dispatcher:
    api = FeedApi(store, FakeVerifier(), timing.get("clock", FixedClock()), decode_json=decode_json,
                  **({"run": TimedRunner(timing["clock"])} if timing else {}))
    return Dispatcher(Handlers(api), timing.get("spans"), timing.get("clock"))


async def respond(dispatcher: Dispatcher, method: bytes, path: bytes, body: bytes = b"", authorization=None):
    outcome = dispatcher.dispatch(Request(method, path, authorization, body, True, 0))
    if type(outcome[0]) is int:
        return outcome
    coroutine, finish = outcome
    return finish(await coroutine)


class Dispatch(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.dispatcher = dispatcher_for(FakeStore({5: b'{"post":5}'}))

    async def test_routes(self):
        d = self.dispatcher
        self.assertEqual(await respond(d, b"GET", b"/feed"), (200, b'{"posts":[]}'))
        self.assertEqual(await respond(d, b"GET", b"/posts/5"), (200, b'{"post":5}'))
        self.assertEqual(await respond(d, b"POST", b"/posts/5/like", authorization=GOOD),
                         (201, b'{"liked":true,"already_liked":false,"post_id":5}'))
        self.assertEqual(await respond(d, b"POST", b"/posts/5/like", authorization=GOOD),
                         (200, b'{"liked":true,"already_liked":true,"post_id":5}'))
        self.assertEqual(await respond(d, b"POST", b"/posts", b'{"body":"hi"}', GOOD), (201, b'{"post":"new"}'))
        self.assertEqual((await respond(d, b"GET", b"/health"))[0], 200)

    async def test_post_id_reaches_the_workflow_unvalidated(self):
        d = self.dispatcher
        self.assertEqual(await respond(d, b"GET", b"/posts/abc"), (400, b'{"error":"invalid post id"}'))
        self.assertEqual(await respond(d, b"GET", b"/posts/6"), (404, b'{"error":"post not found"}'))
        self.assertEqual(await respond(d, b"POST", b"/posts/abc/like"), (401, b'{"error":"missing bearer token"}'))

    async def test_malformed_and_non_finite_json(self):
        for body in (b"{bad", b'{"body":NaN}', b'{"body":"\\ud800"}', b"\xff"):
            with self.subTest(body=body):
                self.assertEqual(await respond(self.dispatcher, b"POST", b"/posts", body, GOOD),
                                 (400, b'{"error":"malformed JSON body"}'))

    async def test_unknown_paths(self):
        for path in (b"/nope", b"/posts/", b"/posts/5/", b"/posts/5/like/", b"/posts//like", b"/posts/1/2", b"/feed/"):
            self.assertEqual(await respond(self.dispatcher, b"GET", path), (404, b'{"error":"not found"}'), path)

    async def test_wrong_method(self):
        for method, path in ((b"POST", b"/feed"), (b"GET", b"/posts"), (b"GET", b"/posts/5/like"), (b"DELETE", b"/posts/5")):
            self.assertEqual((await respond(self.dispatcher, method, path))[0], 405, (method, path))

    async def test_store_down_is_503(self):
        dispatcher = dispatcher_for(FakeStore(up=False))
        self.assertEqual(await respond(dispatcher, b"GET", b"/health"),
                         (503, b'{"status":"degraded","db":"unreachable","error":"disk on fire"}'))

    async def test_timed_requests_record_every_hop(self):
        recorded = []

        class Sink:
            def record(self, endpoint, spans):
                recorded.append((endpoint, [span.name for span in spans]))

        dispatcher = dispatcher_for(FakeStore({5: b'{"post":5}'}), clock=FixedClock(), spans=Sink())
        self.assertEqual(await respond(dispatcher, b"GET", b"/posts/5"), (200, b'{"post":5}'))
        await respond(dispatcher, b"GET", b"/nope")
        self.assertEqual(recorded, [
            ("GET /posts/:id", ["route", "parse id", "read post", "adapter glue", "request"]),
            ("not found", ["route", "adapter glue", "request"]),
        ])


class Connection(unittest.IsolatedAsyncioTestCase):
    """HttpConnection with a fake transport: framing, keep-alive, ordering, closing."""

    def setUp(self):
        self.store = SlowStore({5: b'{"post":5}'})
        self.budget = FakeBudget()
        self.server = HttpServer(dispatcher_for(self.store), self.budget, "127.0.0.1", 0)
        self.transport = FakeTransport()
        self.connection = HttpConnection(self.server)
        self.connection.connection_made(self.transport)

    def responses(self) -> list[tuple[int, bytes]]:
        out = []
        for part in self.transport.written.split(b"HTTP/1.1 ")[1:]:
            head, _, body = part.partition(b"\r\n\r\n")
            out.append((int(head[:3]), body))
        return out

    async def test_answers_pipelined_requests_in_order(self):
        self.connection.data_received(b"GET /feed HTTP/1.1\r\n\r\nGET /posts/5 HTTP/1.1\r\n\r\nGET /x HTTP/1.1\r\n\r\n")
        self.assertEqual(self.responses(), [(200, b'{"posts":[]}'), (200, b'{"post":5}'), (404, b'{"error":"not found"}')])
        self.assertFalse(self.transport.closed)

    async def test_request_split_across_reads(self):
        raw = b"POST /posts HTTP/1.1\r\nAuthorization: " + GOOD + b"\r\nContent-Length: 13\r\n\r\n{\"body\":\"hi\"}"
        for byte in range(len(raw)):
            self.connection.data_received(raw[byte:byte + 1])
        self.assertEqual(self.responses(), [(201, b'{"post":"new"}')])

    async def test_a_suspended_write_holds_later_requests(self):
        like = b"POST /posts/5/like HTTP/1.1\r\nAuthorization: " + GOOD + b"\r\n\r\n"
        self.connection.data_received(like + b"GET /posts/5 HTTP/1.1\r\n\r\n")
        self.assertEqual(self.transport.written, b"")  # the like waits for its commit; the read waits behind it
        self.connection.data_received(b"GET /feed HTTP/1.1\r\n\r\n")
        self.store.commit.set_result(None)
        await asyncio.sleep(0)  # the future's callbacks run on the next loop pass
        self.assertEqual(self.responses(), [(201, b'{"liked":true,"already_liked":false,"post_id":5}'),
                                            (200, b'{"post":5}'), (200, b'{"posts":[]}')])

    async def test_many_requests_wait_for_the_same_commit(self):
        commit = asyncio.get_running_loop().create_future()

        async def feed():
            await commit  # every read during an open batch awaits the batch's one future
            return b'{"posts":[]}'

        self.store.feed = feed
        connections = [HttpConnection(self.server) for _ in range(3)]
        transports = [FakeTransport() for _ in connections]
        for connection, transport in zip(connections, transports):
            connection.connection_made(transport)
            connection.data_received(b"GET /feed HTTP/1.1\r\n\r\n")
        commit.set_result(None)
        await asyncio.sleep(0)
        for transport in transports:
            self.assertTrue(transport.written.startswith(b"HTTP/1.1 200 OK"), transport.written)

    async def test_failed_commit_is_a_500(self):
        self.connection.data_received(b"POST /posts/5/like HTTP/1.1\r\nAuthorization: " + GOOD + b"\r\n\r\n")
        self.store.commit.set_exception(RuntimeError("disk full"))
        with self.assertLogs("adapters.stdlib_http", "ERROR"):
            await asyncio.sleep(0)
        self.assertEqual(self.responses(), [(500, b'{"error":"internal server error"}')])

    async def test_client_gone_before_its_commit(self):
        self.connection.data_received(b"POST /posts/5/like HTTP/1.1\r\nAuthorization: " + GOOD + b"\r\n\r\n")
        self.connection.connection_lost(None)
        self.store.commit.set_result(None)
        await asyncio.sleep(0)
        self.assertEqual(self.transport.written, b"")
        self.assertEqual(self.store.likes, {(1, 5)})  # the write itself still happened

    async def test_close_when_asked_or_strained(self):
        self.connection.data_received(b"GET /feed HTTP/1.1\r\nConnection: close\r\n\r\nGET /feed HTTP/1.1\r\n\r\n")
        self.assertEqual(len(self.responses()), 1)
        self.assertIn(b"Connection: close", self.transport.written)
        self.assertTrue(self.transport.closed)

        self.setUp()
        self.budget.strained = True
        self.connection.data_received(b"GET /feed HTTP/1.1\r\n\r\n")
        self.assertIn(b"Connection: close", self.transport.written)
        self.assertTrue(self.transport.closed)

    async def test_garbage_is_a_400_and_closes(self):
        self.connection.data_received(b"NONSENSE\r\n\r\n")
        self.assertEqual(self.responses(), [(400, b'{"error":"bad request"}')])
        self.assertTrue(self.transport.closed)

    async def test_continue_is_sent_once(self):
        head = b"POST /posts HTTP/1.1\r\nAuthorization: " + GOOD + b"\r\nExpect: 100-continue\r\nContent-Length: 13\r\n\r\n"
        self.connection.data_received(head)
        self.connection.data_received(b'{"body"')
        self.assertEqual(self.transport.written, b"HTTP/1.1 100 Continue\r\n\r\n")
        self.connection.data_received(b':"hi"}')
        self.assertEqual(self.responses()[-1], (201, b'{"post":"new"}'))

    async def test_paused_writing_buffers_requests(self):
        self.connection.pause_writing()
        self.connection.data_received(b"GET /feed HTTP/1.1\r\n\r\n")
        self.assertEqual(self.transport.written, b"")
        self.connection.resume_writing()
        self.assertEqual(self.responses(), [(200, b'{"posts":[]}')])

    async def test_idle_sweep_closes_after_a_quiet_period(self):
        self.connection.close_if_idle()  # had activity (connect): starts a period
        self.assertFalse(self.transport.closed)
        self.connection.close_if_idle()
        self.assertTrue(self.transport.closed)


if __name__ == "__main__":
    unittest.main()
