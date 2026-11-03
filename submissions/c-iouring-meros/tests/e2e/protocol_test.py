"""Protocol-level tests that the challenge's curl-based suite cannot express.

They start the server on a fresh copy of the seed database and talk to it over raw sockets:
pipelining, partial requests, slow readers, group commit and durability after SIGKILL.

    SERVER_BIN=bin/server SEED_DB=seed/feed.db python3 -I tests/e2e/protocol_test.py
"""
import base64
import hashlib
import hmac
import json
import os
import resource
import shutil
import signal
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest

SECRET = b"twelve-dollar-challenge"
USER_ID, USERNAME = 1, "golden_ember_1"


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")


def make_token(user_id=USER_ID, username=USERNAME):
    now = int(time.time())
    header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
    payload = b64url(json.dumps({"sub": str(user_id), "username": username, "iat": now, "exp": now + 3600}).encode())
    signature = b64url(hmac.new(SECRET, header + b"." + payload, hashlib.sha256).digest())
    return (header + b"." + payload + b"." + signature).decode()


class Response:
    def __init__(self, status, headers, body):
        self.status, self.headers, self.body = status, headers, body

    def json(self):
        return json.loads(self.body)


def read_response(stream):
    """Reads one response from a buffered socket file; returns None at end of stream."""
    status_line = stream.readline()
    if not status_line:
        return None
    status = int(status_line.split()[1])
    headers = {}
    while True:
        line = stream.readline().rstrip(b"\r\n")
        if not line:
            break
        name, _, value = line.partition(b":")
        headers[name.strip().lower().decode()] = value.strip().decode()
    body = stream.read(int(headers.get("content-length", "0")))
    return Response(status, headers, body)


def request_bytes(method, path, body=None, token=None, extra_headers=""):
    head = f"{method} {path} HTTP/1.1\r\nHost: test\r\n{extra_headers}"
    if token:
        head += f"Authorization: Bearer {token}\r\n"
    if body is not None:
        head += f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n"
    return head.encode() + b"\r\n" + (body or b"")


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.stream = self.sock.makefile("rb")

    def send(self, data):
        self.sock.sendall(data)

    def read(self):
        return read_response(self.stream)

    def call(self, method, path, body=None, token=None):
        self.send(request_bytes(method, path, body, token))
        return self.read()

    def close(self):
        self.stream.close()
        self.sock.close()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ServerTestCase(unittest.TestCase):
    """Every test gets its own server and its own copy of the database."""

    descriptor_limit = None  # set to start the server with a small RLIMIT_NOFILE

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="e2e-")
        self.db_path = os.path.join(self.dir, "feed.db")
        shutil.copyfile(os.environ["SEED_DB"], self.db_path)
        self.port = free_port()
        env = dict(os.environ, SQLITE_PATH=self.db_path, JWT_SECRET=SECRET.decode(), HOST="127.0.0.1", PORT=str(self.port))
        self.log = open(os.path.join(self.dir, "server.log"), "wb")
        limit = self.descriptor_limit
        self.server = subprocess.Popen(
            [os.environ["SERVER_BIN"]], env=env, stdout=self.log, stderr=self.log,
            preexec_fn=(lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit))) if limit else None,
        )
        self.addCleanup(self.stop_server)
        self.wait_until_healthy()
        self.token = make_token()
        with open(os.path.join(self.dir, "server.log"), "rb") as f:
            self.backend = "io_uring" if b"server: io_uring" in f.read() else "epoll"

    def wait_until_healthy(self):
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                client = Client(self.port)
                try:
                    if client.call("GET", "/health").status == 200:
                        return
                finally:
                    client.close()
            except OSError:
                time.sleep(0.1)
        self.fail("server did not become healthy")

    def stop_server(self):
        if self.server.poll() is None:
            self.server.send_signal(signal.SIGTERM)
            self.server.wait(timeout=10)
        self.log.close()
        if self.server.returncode not in (0, -signal.SIGTERM, -signal.SIGKILL):
            with open(os.path.join(self.dir, "server.log"), "rb") as f:
                self.fail(f"server exited with {self.server.returncode}:\n{f.read().decode(errors='replace')}")
        shutil.rmtree(self.dir, ignore_errors=True)

    def client(self):
        c = Client(self.port)
        self.addCleanup(c.close)
        return c


class FramingTests(ServerTestCase):
    def test_pipelined_requests_are_answered_in_order(self):
        c = self.client()
        c.send(request_bytes("GET", "/health") + request_bytes("POST", "/posts/1/like", token=self.token)
               + request_bytes("GET", "/posts/1") + request_bytes("GET", "/nope"))
        statuses = [c.read().status for _ in range(4)]
        self.assertEqual(statuses, [200, 201, 200, 404])

    def test_pipelined_write_then_read_sees_the_write(self):
        c = self.client()
        body = json.dumps({"body": "pipelined"}).encode()
        c.send(request_bytes("POST", "/posts", body, self.token) + request_bytes("GET", "/feed"))
        created, feed = c.read(), c.read()
        self.assertEqual(created.status, 201)
        self.assertEqual(feed.json()["posts"][0]["id"], created.json()["post"]["id"])

    def test_request_arriving_one_byte_at_a_time(self):
        c = self.client()
        for byte in request_bytes("GET", "/posts/1"):
            c.send(bytes([byte]))
            time.sleep(0.001)
        self.assertEqual(c.read().status, 200)

    def test_body_arriving_after_the_headers(self):
        c = self.client()
        body = json.dumps({"body": "late body"}).encode()
        full = request_bytes("POST", "/posts", body, self.token)
        split = full.index(b"\r\n\r\n") + 4
        c.send(full[:split])
        time.sleep(0.2)
        c.send(full[split:])
        self.assertEqual(c.read().status, 201)

    def test_expect_continue_gets_an_interim_response(self):
        c = self.client()
        body = json.dumps({"body": "expecting"}).encode()
        full = request_bytes("POST", "/posts", body, self.token, extra_headers="Expect: 100-continue\r\n")
        split = full.index(b"\r\n\r\n") + 4
        c.send(full[:split])
        self.assertEqual(c.read().status, 100)
        c.send(full[split:])
        self.assertEqual(c.read().status, 201)

    def test_connection_close_is_honoured(self):
        c = self.client()
        c.send(request_bytes("GET", "/health", extra_headers="Connection: close\r\n"))
        response = c.read()
        self.assertEqual(response.status, 200)
        self.assertEqual(response.headers["connection"], "close")
        self.assertIsNone(c.read())  # the server closed the connection

    def test_http_1_0_closes_after_the_response(self):
        c = self.client()
        c.send(b"GET /health HTTP/1.0\r\n\r\n")
        self.assertEqual(c.read().status, 200)
        self.assertIsNone(c.read())

    def test_malformed_request_gets_400_and_close(self):
        c = self.client()
        c.send(b"THIS IS NOT HTTP\r\n\r\n")
        self.assertEqual(c.read().status, 400)
        self.assertIsNone(c.read())

    def test_oversized_headers_get_413_and_close(self):
        c = self.client()
        c.send(b"GET /feed HTTP/1.1\r\nX-Pad: " + b"a" * 20000 + b"\r\n\r\n")
        self.assertEqual(c.read().status, 413)
        self.assertIsNone(c.read())

    def test_slow_reader_gets_every_response_in_order(self):
        """200 pipelined feeds are ~1 MB: more than the socket buffers hold, so the server must queue output."""
        c = self.client()
        c.send(request_bytes("GET", "/feed") * 200)
        time.sleep(0.5)  # let the server fill the socket buffers and block
        first = c.read()
        self.assertEqual(len(first.json()["posts"]), 20)
        for _ in range(199):
            self.assertEqual(c.read().status, 200)

    def test_client_that_disconnects_mid_pipeline_does_not_hurt_others(self):
        for _ in range(20):
            rude = Client(self.port)
            rude.send(request_bytes("GET", "/feed") * 50)
            rude.sock.close()
        self.assertEqual(self.client().call("GET", "/health").status, 200)


class WriteTests(ServerTestCase):
    def test_concurrent_creates_all_commit_and_survive_sigkill(self):
        """Group commit: each 201 means the row is in the write-ahead log, so it survives a crash."""
        count = 60
        results = [None] * count

        def create(i):
            c = Client(self.port)
            try:
                results[i] = c.call("POST", "/posts", json.dumps({"body": f"durable {i}"}).encode(), self.token)
            finally:
                c.close()

        threads = [threading.Thread(target=create, args=(i,)) for i in range(count)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertTrue(all(r is not None and r.status == 201 for r in results))
        ids = {r.json()["post"]["id"] for r in results}
        self.assertEqual(len(ids), count)

        self.server.send_signal(signal.SIGKILL)  # no checkpoint, no orderly shutdown
        self.server.wait()
        db = sqlite3.connect(self.db_path)
        stored = {row[0] for row in db.execute("SELECT id FROM posts WHERE body LIKE 'durable %'")}
        db.close()
        self.assertEqual(stored, ids)

    def test_concurrent_likes_by_one_user_count_once(self):
        count = 30
        statuses = []
        lock = threading.Lock()

        def like():
            c = Client(self.port)
            try:
                response = c.call("POST", "/posts/5/like", token=self.token)
                with lock:
                    statuses.append(response.status)
            finally:
                c.close()

        before = self.client().call("GET", "/posts/5").json()["post"]["like_count"]
        threads = [threading.Thread(target=like) for _ in range(count)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(statuses.count(201), 1)
        self.assertEqual(statuses.count(200), count - 1)
        after = self.client().call("GET", "/posts/5").json()["post"]["like_count"]
        self.assertEqual(after, before + 1)

    def test_a_write_is_visible_to_the_next_read_on_another_connection(self):
        writer, reader = self.client(), self.client()
        for i in range(25):
            created = writer.call("POST", "/posts", json.dumps({"body": f"visible {i}"}).encode(), self.token)
            self.assertEqual(created.status, 201)
            top = reader.call("GET", "/feed").json()["posts"][0]
            self.assertEqual(top["id"], created.json()["post"]["id"])

    def test_feed_requests_in_one_round_all_get_a_valid_feed(self):
        clients = [self.client() for _ in range(300)]
        for c in clients:
            c.send(request_bytes("GET", "/feed"))
        for c in clients:
            self.assertEqual(len(c.read().json()["posts"]), 20)

    def test_posts_with_special_characters_round_trip(self):
        text = 'quote " backslash \\ slash / tab \t newline \n unicode café ✓ \U0001F600 <&>'
        c = self.client()
        created = c.call("POST", "/posts", json.dumps({"body": text}).encode(), self.token)
        self.assertEqual(created.status, 201)
        post_id = created.json()["post"]["id"]
        self.assertEqual(c.call("GET", f"/posts/{post_id}").json()["post"]["body"], text)
        self.assertEqual(c.call("GET", "/feed").json()["posts"][0]["body"], text)

    def test_body_limit_counts_characters_not_bytes(self):
        c = self.client()
        ok = c.call("POST", "/posts", json.dumps({"body": "é" * 500}).encode(), self.token)
        self.assertEqual(ok.status, 201)
        too_long = c.call("POST", "/posts", json.dumps({"body": "é" * 501}).encode(), self.token)
        self.assertEqual(too_long.status, 400)
        self.assertEqual(too_long.json(), {"error": "body must be at most 500 characters"})


REFERENCE_FEED_SQL = """
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id
 ORDER BY p.created_at DESC, p.id DESC LIMIT 20"""


class FeedTests(ServerTestCase):
    def reference_feed(self):
        """Stops the server without a checkpoint and evaluates the challenge's own feed query on the file."""
        self.server.send_signal(signal.SIGKILL)
        self.server.wait()
        db = sqlite3.connect(self.db_path)
        rows = db.execute(REFERENCE_FEED_SQL).fetchall()
        db.close()
        return [dict(id=r[0], body=r[1], created_at=r[2], author=r[3], like_count=r[4]) for r in rows]

    def test_feed_equals_the_reference_query_after_creates_and_likes(self):
        c = self.client()
        ids = []
        for i in range(30):
            token = make_token(user_id=1 + i % 7, username=f"user_{1 + i % 7}")
            ids.append(c.call("POST", "/posts", json.dumps({"body": f"post {i} \"quoted\" \u00e9"}).encode(), token).json()["post"]["id"])
        for i, post_id in enumerate(ids[-12:]):
            for liker in range(1 + i % 5):
                self.assertIn(c.call("POST", f"/posts/{post_id}/like", token=make_token(user_id=100 + liker)).status, (200, 201))
        feed = c.call("GET", "/feed").json()["posts"]
        self.assertEqual(feed, self.reference_feed())

    def test_a_post_without_an_author_row_is_left_out_like_the_reference_query_does(self):
        """The feed shortcut must notice the missing user row and give the reference answer."""
        c = self.client()
        ghost = c.call("POST", "/posts", json.dumps({"body": "written by nobody"}).encode(), make_token(user_id=999999, username="ghost"))
        self.assertEqual(ghost.status, 201)
        c.call("POST", "/posts", json.dumps({"body": "written by user 1"}).encode(), self.token)
        feed = c.call("GET", "/feed").json()["posts"]
        self.assertNotIn(ghost.json()["post"]["id"], [p["id"] for p in feed])
        self.assertEqual(c.call("GET", f"/posts/{ghost.json()['post']['id']}").status, 404)
        self.assertEqual(feed, self.reference_feed())


class ConnectionTests(ServerTestCase):
    def test_many_idle_connections_stay_cheap(self):
        count = 2000
        clients = [Client(self.port) for _ in range(count)]
        self.addCleanup(lambda: [c.close() for c in clients])
        for c in clients:
            c.send(request_bytes("GET", "/health"))
        for c in clients:
            self.assertEqual(c.read().status, 200)
        with open(f"/proc/{self.server.pid}/status") as status:
            rss_kb = int(next(line for line in status if line.startswith("VmRSS")).split()[1])
        # A 238 MB database is memory-mapped, so RSS includes shared file pages; the cap is generous but finite.
        self.assertLess(rss_kb, 700_000)


class ReviewRegressionTests(ServerTestCase):
    def test_write_responses_announce_connection_close(self):
        """A client that reuses a socket the server is about to close loses a POST it cannot retry."""
        body = json.dumps({"body": "closing"}).encode()
        for method, path, payload in (("POST", "/posts", body), ("POST", "/posts/5/like", None), ("POST", "/posts/999999999/like", None)):
            c = self.client()
            c.send(request_bytes(method, path, payload, self.token, extra_headers="Connection: close\r\n"))
            response = c.read()
            self.assertIn(response.status, (200, 201, 404), path)
            self.assertEqual(response.headers.get("connection"), "close", path)
            self.assertIsNone(c.read(), path)

    def test_ids_that_are_not_numbers_are_400_whatever_their_length(self):
        c = self.client()
        for path in ("/posts/abcdefghijklmnopqrs", "/posts/0000000000000000000", "/posts/0", "/posts/-5", "/posts/1.5"):
            self.assertEqual(c.call("GET", path).status, 400, path)
        self.assertEqual(c.call("GET", "/posts/1234567890123456789").status, 404)
        self.assertEqual(c.call("POST", "/posts/abcdefghijklmnopqrs/like", token=self.token).status, 400)

    def test_a_body_that_is_mostly_whitespace_is_trimmed_not_rejected(self):
        c = self.client()
        created = c.call("POST", "/posts", json.dumps({"body": " " * 9000 + "hi" + "\t" * 100}).encode(), self.token)
        self.assertEqual(created.status, 201)
        self.assertEqual(created.json()["post"]["body"], "hi")

    def test_a_request_split_over_many_reads_is_assembled(self):
        c = self.client()
        body = json.dumps({"body": "x" * 400}).encode()
        full = request_bytes("POST", "/posts", body, self.token)
        for i in range(0, len(full), 100):
            c.send(full[i:i + 100])
            time.sleep(0.01)
        self.assertEqual(c.read().status, 201)


class DescriptorBudgetTests(ServerTestCase):
    descriptor_limit = 200  # 32 descriptors stay in reserve, so the epoll server is "crowded" from 168 connections on

    def test_users_beyond_the_descriptor_budget_are_served_with_connection_close(self):
        if self.backend != "epoll":
            self.skipTest("the io_uring server keeps connections in direct descriptors: see RingSpillTests")
        """With 65,535 descriptors on the benchmark box this is how the server serves more users than descriptors."""
        held = [Client(self.port) for _ in range(190)]
        self.addCleanup(lambda: [c.close() for c in held])
        responses = [c.call("GET", "/health") for c in held]
        self.assertTrue(all(r.status == 200 for r in responses))
        closing = [r for r in responses if r.headers.get("connection") == "close"]
        self.assertGreater(len(closing), 10)    # the connections past the budget
        self.assertLess(len(closing), 60)       # the first ones stay persistent
        for c, r in zip(held, responses):
            if r.headers.get("connection") == "close":
                self.assertIsNone(c.read())     # and the server really closes them, after waiting for the client
        # a user whose connection was closed just connects again
        again = Client(self.port)
        self.addCleanup(again.close)
        self.assertEqual(again.call("GET", "/health").status, 200)


class RingSpillTests(ServerTestCase):
    """The io_uring server holds connections in rings of `descriptor_limit` slots and opens more rings as needed."""
    descriptor_limit = 100  # a ring has 100 slots and stops accepting at 88; rings open at 52 used

    def test_connections_spill_into_further_rings_and_the_last_ones_get_connection_close(self):
        if self.backend != "io_uring":
            self.skipTest("rings exist only in the io_uring server")
        held = [Client(self.port) for _ in range(300)]   # all at once: needs four rings
        self.addCleanup(lambda: [c.close() for c in held])
        first = [c.call("GET", "/health") for c in held]
        self.assertTrue(all(r.status == 200 for r in first))
        self.assertTrue(all(r.headers.get("connection") != "close" for r in first))  # room for all of them
        for c in held[:50]:
            self.assertEqual(c.call("GET", "/health").status, 200)  # still usable: a second request

        more = [Client(self.port) for _ in range(180)]   # 480 in all: past what five rings hold without closing
        self.addCleanup(lambda: [c.close() for c in more])
        answers = [c.call("GET", "/health") for c in more]
        self.assertTrue(all(r.status == 200 for r in answers))
        self.assertGreater(len([r for r in answers if r.headers.get("connection") == "close"]), 5)

    def test_a_burst_that_crosses_a_ring_boundary_is_served_without_resets(self):
        if self.backend != "io_uring":
            self.skipTest("rings exist only in the io_uring server")
        held = [Client(self.port) for _ in range(70)]    # ring 0 is nearly full and ring 1 is open
        for c in held:
            self.assertEqual(c.call("GET", "/health").status, 200)
        burst = [Client(self.port) for _ in range(60)]   # all at once
        self.addCleanup(lambda: [c.close() for c in held + burst])
        for c in burst:
            c.send(request_bytes("GET", "/health"))
        self.assertEqual([c.read().status for c in burst], [200] * 60)


class MemoryPressureTests(ServerTestCase):
    """memwatch reads MemAvailable from a file the test controls (SERVER_TEST_MEMINFO) instead of /proc/meminfo."""

    def setUp(self):
        self.meminfo = tempfile.NamedTemporaryFile("w", suffix=".meminfo", delete=False)
        self.meminfo.close()
        self.addCleanup(os.unlink, self.meminfo.name)
        self.write_available(1_500_000)
        os.environ["SERVER_TEST_MEMINFO"] = self.meminfo.name
        self.addCleanup(os.environ.pop, "SERVER_TEST_MEMINFO", None)
        super().setUp()

    def write_available(self, kb):
        tmp = self.meminfo.name + ".new"
        with open(tmp, "w") as f:   # 2 GB machine: shedding spans 500,000 down to 250,000 kB available
            f.write(f"MemTotal:       2000000 kB\nMemFree:         100000 kB\nMemAvailable:   {kb} kB\n")
        os.replace(tmp, self.meminfo.name)

    def set_available(self, kb):
        self.write_available(kb)
        time.sleep(1.6)             # memwatch reads once a second

    def close_share(self, requests=300):
        closing = 0
        for _ in range(requests):
            c = Client(self.port)
            try:
                closing += c.call("GET", "/health").headers.get("connection") == "close"
            finally:
                c.close()
        return closing / requests

    def test_nothing_is_shed_while_memory_is_not_short(self):
        self.assertEqual(self.close_share(100), 0)
        self.set_available(600_000)   # lower than usual, but still above a quarter of RAM
        self.assertEqual(self.close_share(100), 0)

    def test_shedding_grows_linearly_below_a_quarter_and_stops_when_memory_is_back(self):
        self.set_available(375_000)   # halfway between a quarter and an eighth: half of the responses
        share = self.close_share()
        self.assertGreater(share, 0.35)
        self.assertLess(share, 0.65)
        self.set_available(1_500_000)
        self.assertEqual(self.close_share(100), 0)

    def test_when_critical_every_response_closes_new_connections_wait_and_idle_ones_are_evicted(self):
        idle = [self.client() for _ in range(40)]
        for c in idle:
            self.assertEqual(c.call("GET", "/health").status, 200)
        time.sleep(1.2)               # idle for more than a second
        self.set_available(200_000)
        for c in idle:                # evicted: the server closed them
            c.sock.settimeout(5)
            self.assertEqual(c.sock.recv(1), b"")
        waiting = Client(self.port)   # accepted by the kernel, not by the server: no answer yet
        self.addCleanup(waiting.close)
        waiting.send(request_bytes("GET", "/health"))
        waiting.sock.settimeout(1.5)
        with self.assertRaises(socket.timeout):
            waiting.sock.recv(1)
        self.set_available(1_500_000)  # memory recovers: the server accepts again and answers
        waiting.sock.settimeout(5)
        self.assertTrue(waiting.sock.recv(100).startswith(b"HTTP/1.1 200"))


class DrainTests(ServerTestCase):
    """After "Connection: close" the client closes first, so the server does not collect TIME_WAIT sockets."""

    def test_the_server_waits_for_the_client_to_close(self):
        c = self.client()
        c.send(request_bytes("GET", "/health", extra_headers="Connection: close\r\n"))
        self.assertEqual(c.read().headers["connection"], "close")
        c.sock.settimeout(0.1)
        with self.assertRaises(socket.timeout):   # 100 ms later the server has not closed: it waits
            c.sock.recv(1)
        c.sock.shutdown(socket.SHUT_WR)           # the client closes first
        c.sock.settimeout(2)
        self.assertEqual(c.sock.recv(1), b"")     # and the server closes its side right after
        self.assertEqual(self.client().call("GET", "/health").status, 200)

    def test_the_server_gives_up_waiting_for_a_client_that_never_closes(self):
        c = self.client()
        c.send(request_bytes("GET", "/health", extra_headers="Connection: close\r\n"))
        c.read()
        started = time.time()
        c.sock.settimeout(5)
        self.assertEqual(c.sock.recv(1), b"")     # the server closed by itself
        self.assertLess(time.time() - started, 2)
        self.assertGreater(time.time() - started, 0.1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
