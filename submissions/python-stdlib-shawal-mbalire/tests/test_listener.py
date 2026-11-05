import asyncio
import json
import unittest

from adapters.epoll_http import EpollHttpServer
from tests.fixtures import FakeStore
from tests.test_stdlib_http import GOOD, FakeBudget, SlowStore, dispatcher_for

SERVERS = (EpollHttpServer,)


async def read_response(reader: asyncio.StreamReader) -> tuple[bytes, bytes]:
    head = await reader.readuntil(b"\r\n\r\n")
    length = int(head.split(b"Content-Length: ")[1].split(b"\r\n")[0])
    return head, await reader.readexactly(length)


class EveryServer(unittest.IsolatedAsyncioTestCase):
    """Conversations with the server on a loopback socket (a loop over SERVERS, for any future A/B)."""

    async def serve(self, server_class, store=None, budget=None):
        server = server_class(dispatcher_for(store or FakeStore({5: b'{"post":5}'})), budget or FakeBudget(),
                              "127.0.0.1", 0)
        await server.start()
        self.addAsyncCleanup(server.close)
        return server, server.sockets[0].getsockname()[1]

    async def test_keep_alive_round_trips(self):
        for server_class in SERVERS:
            with self.subTest(server_class.__name__):
                server, port = await self.serve(server_class)
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                for _ in range(3):
                    writer.write(b"GET /posts/5 HTTP/1.1\r\nHost: x\r\n\r\n")
                    head, body = await read_response(reader)
                    self.assertTrue(head.startswith(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"))
                    self.assertEqual(json.loads(body), {"post": 5})
                self.assertEqual(len(server.connections), 1)
                writer.close()
                await writer.wait_closed()
                for _ in range(20):
                    if not server.connections:
                        break
                    await asyncio.sleep(0.01)
                self.assertEqual(len(server.connections), 0)

    async def test_pipelined_requests_and_a_write_waiting_for_its_commit(self):
        for server_class in SERVERS:
            with self.subTest(server_class.__name__):
                store = SlowStore({5: b'{"post":5}'})
                _, port = await self.serve(server_class, store)
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                writer.write(b"POST /posts/5/like HTTP/1.1\r\nAuthorization: " + GOOD + b"\r\n\r\n"
                             b"GET /feed HTTP/1.1\r\n\r\n")
                while store.commit is None:
                    await asyncio.sleep(0.005)
                store.commit.set_result(None)
                self.assertEqual((await read_response(reader))[1], b'{"liked":true,"already_liked":false,"post_id":5}')
                self.assertEqual((await read_response(reader))[1], b'{"posts":[]}')
                writer.close()

    async def test_strained_answers_close_the_connection(self):
        for server_class in SERVERS:
            with self.subTest(server_class.__name__):
                budget = FakeBudget()
                budget.strained = True
                server, port = await self.serve(server_class, budget=budget)
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                writer.write(b"GET /feed HTTP/1.1\r\n\r\n")
                head, body = await read_response(reader)
                self.assertIn(b"Connection: close\r\n", head)
                self.assertEqual(await reader.read(), b"")  # the server closed it
                self.assertEqual(len(server.connections), 0)
                writer.close()

    async def test_large_response_is_written_in_parts(self):
        big = b'{"post":"' + b"x" * 4_000_000 + b'"}'
        for server_class in SERVERS:
            with self.subTest(server_class.__name__):
                _, port = await self.serve(server_class, FakeStore({5: big}))
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                writer.write(b"GET /posts/5 HTTP/1.1\r\n\r\nGET /posts/5 HTTP/1.1\r\nConnection: close\r\n\r\n")
                self.assertEqual((await read_response(reader))[1], big)
                head, body = await read_response(reader)
                self.assertEqual(body, big)
                self.assertIn(b"Connection: close", head)
                self.assertEqual(await reader.read(), b"")
                writer.close()


class AcceptGate(unittest.IsolatedAsyncioTestCase):
    """At the connection cap the listener is not watched; it is again once a connection closes."""

    async def test_stops_and_resumes_accepting(self):
        for server_class in SERVERS:
            with self.subTest(server_class.__name__):
                server = server_class(dispatcher_for(FakeStore()), FakeBudget(), "127.0.0.1", 0)
                server._cap = 1
                await server.start()
                self.addAsyncCleanup(server.close)
                port = server.sockets[0].getsockname()[1]
                first_reader, first_writer = await asyncio.open_connection("127.0.0.1", port)
                first_writer.write(b"GET /feed HTTP/1.1\r\n\r\n")
                await read_response(first_reader)
                second_reader, second_writer = await asyncio.open_connection("127.0.0.1", port)  # queued by the kernel
                second_writer.write(b"GET /feed HTTP/1.1\r\n\r\n")
                with self.assertRaises(TimeoutError):
                    await asyncio.wait_for(second_reader.readuntil(b"\r\n\r\n"), 0.1)
                self.assertFalse(server._accepting)
                first_writer.close()
                _, body = await asyncio.wait_for(read_response(second_reader), 2)
                self.assertEqual(body, b'{"posts":[]}')
                second_writer.close()


if __name__ == "__main__":
    unittest.main()
