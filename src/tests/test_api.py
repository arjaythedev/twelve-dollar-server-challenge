"""HTTP regressions for authentication, fresh reads, JSON and committed writes"""

import base64
import hashlib
import hmac
import http.client
import json
import os
import random
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
from contextlib import ExitStack, closing
from pathlib import Path
from typing import ClassVar

from devtools import images
from devtools.reports import JSONValue, decode_json, integer, mapping
from devtools.reports import text as json_text
from devtools.runtime import ROOT, command, server

SECRET = b"test-secret"


def sign(payload: JSONValue, algorithm: str = "HS256") -> str:
    def segment(value: JSONValue) -> bytes:
        encoded = json.dumps(value, separators=(",", ":")).encode()
        return base64.urlsafe_b64encode(encoded).rstrip(b"=")

    content = segment({"alg": algorithm}) + b"." + segment(payload)
    digest = hmac.new(SECRET, content, hashlib.sha256).digest()
    signature = base64.urlsafe_b64encode(digest).rstrip(b"=")
    return "Bearer " + (content + b"." + signature).decode("ascii")


def claims(**overrides: JSONValue) -> dict[str, JSONValue]:
    return {"sub": "1", "username": "token-user", "exp": time.time() + 3600, **overrides}


def array(value: JSONValue) -> list[JSONValue]:
    if not isinstance(value, list):
        raise AssertionError("expected a JSON array")
    return value


def literal(value: str) -> str:
    return f"CAST(X'{value.encode().hex()}' AS TEXT)"


@unittest.skipUnless(os.environ.get("TDS_HTTP_TESTS") == "1", "run dev.py check --docker")
class HTTPTests(unittest.TestCase):
    image: ClassVar[images.ImageIdentity]

    @classmethod
    def setUpClass(cls) -> None:
        cls.image = images.inspect(
            os.environ.get("TDS_HTTP_TEST_IMAGE") or images.IMAGE, "/app/server"
        )

    def setUp(self) -> None:
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        directory = self.stack.enter_context(tempfile.TemporaryDirectory())
        self.output = Path(directory)
        self.database = self.output / "fixture.db"
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.executescript((ROOT / "schema.sql").read_text(encoding="utf-8"))
            connection.execute("INSERT INTO users(id,username) VALUES(1,'db-user')")
            connection.executemany(
                "INSERT INTO posts(id,user_id,body,created_at) VALUES(?,1,?,?)",
                [
                    (1, "old", "2024-01-01T00:00:00.000Z"),
                    (2, "tie-low", "2025-01-01T00:00:00.000Z"),
                    (3, "tie-high", "2025-01-01T00:00:00.000Z"),
                ],
            )
        self._endpoint: tuple[str, int] | None = None
        self._connection: http.client.HTTPConnection | None = None
        self._stopped = False

    def start(self) -> tuple[str, int]:
        if self._stopped:
            raise AssertionError("the server cannot restart after persistence inspection")
        if self._endpoint is None:
            self._endpoint = self.stack.enter_context(
                server(
                    self.output,
                    database=self.database,
                    secret=SECRET.decode(),
                    image=self.image,
                )
            )
        return self._endpoint

    @property
    def container(self) -> str:
        return self.start()[0]

    @property
    def port(self) -> int:
        return self.start()[1]

    @property
    def connection(self) -> http.client.HTTPConnection:
        self.start()
        if self._connection is None:
            self._connection = self.stack.enter_context(
                closing(http.client.HTTPConnection("127.0.0.1", self.port, timeout=5))
            )
        return self._connection

    def request(
        self, method: str, path: str, body: bytes = b"", auth: str | None = None
    ) -> tuple[int, dict[str, JSONValue], bytes]:
        self.connection.request(method, path, body, {"authorization": auth} if auth else {})
        response = self.connection.getresponse()
        raw = response.read()
        self.assertEqual(response.getheader("content-type"), "application/json")
        return response.status, mapping(decode_json(raw)), raw

    def fixture_sql(self, statement: str) -> None:
        if self._endpoint is not None:
            raise AssertionError("fixture SQL must precede the first HTTP request")
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.executescript(statement)

    def fixture_rows(self, statement: str) -> list[JSONValue]:
        if self._endpoint is not None:
            raise AssertionError("fixture references must precede the first HTTP request")
        with closing(sqlite3.connect(self.database)) as connection:
            connection.row_factory = sqlite3.Row
            rows = [dict(row) for row in connection.execute(statement)]
        return array(decode_json(json.dumps(rows).encode()))

    def persisted_rows(self, statement: str) -> list[JSONValue]:
        if self._endpoint is None:
            raise AssertionError("persistence inspection requires a completed HTTP request")
        container, _ = self._endpoint
        if not self._stopped:
            if self._connection is not None:
                self._connection.close()
            command("docker", "stop", "--time=10", container)
            self._stopped = True
            self.assertEqual(
                command("docker", "inspect", "--format", "{{.State.ExitCode}}", container), "0"
            )
        snapshot = self.output / "persisted"
        if not snapshot.is_dir():
            snapshot.mkdir()
            # Copy WAL too, and allow a transient WAL index without changing the stopped volume
            command("docker", "cp", f"{container}:/data/.", str(snapshot))
        with closing(sqlite3.connect(snapshot / "feed.db")) as connection:
            connection.row_factory = sqlite3.Row
            rows = [dict(row) for row in connection.execute(statement)]
        return array(decode_json(json.dumps(rows).encode()))

    def test_concurrent_write_results_and_constraint_isolation(self) -> None:
        with ExitStack() as connections:
            pending: list[tuple[http.client.HTTPConnection, int, str]] = []
            for index in range(24):
                connection = connections.enter_context(
                    closing(http.client.HTTPConnection("127.0.0.1", self.port, timeout=5))
                )
                connection.connect()
                text = f"batch-{index} café \N{GRINNING FACE}"
                pending.append((connection, index, text))
            for connection, index, text in pending:
                body = b"{" if index == 0 else json.dumps({"body": text}).encode()
                user_id = "999999" if index == 1 else "1"
                connection.request(
                    "POST", "/posts", body, {"authorization": sign(claims(sub=user_id))}
                )
            created: list[int] = []
            for connection, index, text in pending:
                response = connection.getresponse()
                result = mapping(decode_json(response.read()))
                self.assertEqual(response.status, 400 if index == 0 else 500 if index == 1 else 201)
                if index > 1:
                    post = mapping(result["post"])
                    self.assertEqual(post["body"], text)
                    created.append(integer(post, "id"))
            self.assertEqual(len(set(created)), 22)
            self.assertEqual(
                integer(mapping(self.persisted_rows("SELECT count(*) AS n FROM posts")[0]), "n"), 25
            )

    def test_pipelined_like_read_and_duplicate_keep_order(self) -> None:
        authorization = sign(claims())
        write = (
            "POST /posts/1/like HTTP/1.1\r\nHost: localhost\r\n"
            f"Authorization: {authorization}\r\nContent-Length: 0\r\n\r\n"
        )
        read = "GET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        with closing(socket.create_connection(("127.0.0.1", self.port), timeout=5)) as connection:
            connection.sendall((write + read + write).encode())
            with connection.makefile("rb") as stream:
                for expected_status, kind in ((201, "like"), (200, "post"), (200, "duplicate")):
                    self.assertTrue(
                        stream.readline().startswith(f"HTTP/1.1 {expected_status} ".encode())
                    )
                    headers = http.client.parse_headers(stream)
                    result = mapping(decode_json(stream.read(int(headers["content-length"]))))
                    if kind == "post":
                        self.assertEqual(mapping(result["post"])["like_count"], 1)
                    else:
                        self.assertEqual(result["already_liked"], kind == "duplicate")

    def test_required_expiration_and_payload(self) -> None:
        missing = claims()
        del missing["exp"]
        invalid = [
            missing,
            *[
                claims(exp=value)
                for value in (None, True, "9999999999", 0, float("nan"), float("inf"))
            ],
        ]
        for value in invalid:
            with self.subTest(claims=value):
                self.assertEqual(
                    self.request("POST", "/posts/3/like", auth=sign(value))[:2],
                    (401, {"error": "invalid or expired token"}),
                )
        for subject in (1, True, "0", "-1", "1.5", "", "abc", "\u0661"):
            with self.subTest(subject=subject):
                self.assertEqual(
                    self.request("POST", "/posts/3/like", auth=sign(claims(sub=subject)))[:2],
                    (401, {"error": "invalid token payload"}),
                )
        for username in (None, 1, True):
            self.assertEqual(
                self.request("POST", "/posts/3/like", auth=sign(claims(username=username)))[:2],
                (401, {"error": "invalid token payload"}),
            )

    def test_signature_algorithm_and_expiration_are_checked_again(self) -> None:
        token = sign(claims())
        corrupted = token.rsplit(".", 1)[0] + "." + "A" * 43
        for header in (corrupted, sign(claims(), algorithm="HS512"), "Bearer a.b.c", "Bearer \xff"):
            self.assertEqual(
                self.request("POST", "/posts/3/like", auth=header)[:2],
                (401, {"error": "invalid or expired token"}),
            )
        expiration = time.time() + 2
        expiring = sign(claims(exp=expiration))
        self.assertEqual(self.request("POST", "/posts/3/like", auth=expiring)[0], 201)
        time.sleep(max(0, expiration - time.time()) + 0.1)
        self.assertEqual(self.request("POST", "/posts/3/like", auth=expiring)[0], 401)

    def test_auth_precedes_id_and_body_validation(self) -> None:
        for route in ("/posts", "/posts/abc/like"):
            self.assertEqual(
                self.request("POST", route, b"not JSON")[:2],
                (401, {"error": "missing bearer token"}),
            )
        self.assertEqual(
            self.request("POST", "/posts/abc/like", auth=sign(claims()))[:2],
            (400, {"error": "invalid post id"}),
        )
        self.assertEqual(
            self.request("POST", "/posts/99999/like", auth=sign(claims()))[:2],
            (404, {"error": "post not found"}),
        )
        self.assertEqual(self.request("POST", "/posts/3/like", auth=sign(claims()))[0], 201)
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as connection:
            connection.sendall(
                b"POST /posts HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n"
            )
            self.assertIn(b" 401 ", connection.recv(4096).split(b"\r\n", 1)[0])

    def test_feed_order_and_fresh_like_counts(self) -> None:
        status, body, _ = self.request("GET", "/feed")
        self.assertEqual(status, 200)
        posts = [mapping(value) for value in array(body["posts"])]
        self.assertEqual([post["id"] for post in posts], [3, 2, 1])
        self.assertEqual(list(posts[0]), ["id", "body", "created_at", "author", "like_count"])
        self.assertEqual(self.request("POST", "/posts/3/like", auth=sign(claims()))[0], 201)
        refreshed = array(self.request("GET", "/feed")[1]["posts"])
        self.assertEqual(mapping(refreshed[0])["like_count"], 1)
        self.assertEqual(mapping(self.request("GET", "/posts/3")[1]["post"])["like_count"], 1)

    def test_empty_feed(self) -> None:
        self.fixture_sql("DELETE FROM posts")
        self.assertEqual(self.request("GET", "/feed")[:2], (200, {"posts": []}))

    def test_nonmonotonic_timestamps(self) -> None:
        rows = ",".join(
            f"({i},1,'body','2026-01-{31 - i // 2:02}T00:00:00.000Z')" for i in range(1, 32)
        )
        self.fixture_sql(
            f"DELETE FROM posts; INSERT INTO posts(id,user_id,body,created_at) VALUES {rows}"
        )
        posts = array(self.request("GET", "/feed")[1]["posts"])
        self.assertEqual(
            [mapping(post)["id"] for post in posts],
            [1, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 17, 16, 19, 18, 21],
        )

    def test_feed_batch_reorders_rows_and_deduplicates_authors(self) -> None:
        self.fixture_sql(
            "DELETE FROM likes; DELETE FROM posts;"
            "INSERT INTO users(id,username) VALUES(-7,'negative'),"
            "(2,replace(hex(zeroblob(5000)),'0','x') || char(0) || ' café'),"
            "(900001,'distant');"
        )
        ids = [-9223372036854775808, -5000000, -17, -1, 0, *range(2, 28)]
        ids += [1000000, 9223372036854775807]
        authors = [-7, 1, 2, 900001]
        rows: list[str] = []
        for index, post_id in enumerate(ids):
            timestamp = f"2026-02-{(7 * index) % 28 + 1:02}T00:00:00.000Z"
            if post_id == ids[0]:
                timestamp = "2089-01-01T00:00:00.000Z"
            if post_id == ids[-1]:
                timestamp = "2090-01-01T00:00:00.000Z"
            rows.append(f"({post_id},{authors[index % len(authors)]},'post-{index}','{timestamp}')")
        rows.extend(
            f"({post_id},1,'skipped','2000-01-01T00:00:00.000Z')" for post_id in range(10000, 10100)
        )
        self.fixture_sql(
            "INSERT INTO posts(id,user_id,body,created_at) VALUES "
            + ",".join(rows)
            + "; INSERT INTO likes(user_id,post_id) SELECT 1,id FROM posts WHERE id<0;"
        )
        reference = (
            "SELECT p.id,p.body,p.created_at,hex(u.username) AS author,"
            "(SELECT count(*) FROM likes WHERE post_id=p.id) AS like_count "
            "FROM posts p JOIN users u ON u.id=p.user_id "
            "ORDER BY p.created_at DESC,p.id DESC LIMIT 20"
        )

        def reference_rows() -> list[dict[str, JSONValue]]:
            # Preserve author bytes exactly, including embedded NULs
            rows = [mapping(row) for row in self.fixture_rows(reference)]
            return [
                {**row, "author": bytes.fromhex(json_text(row, "author")).decode("utf-8")}
                for row in rows
            ]

        expected = reference_rows()
        self.assertEqual(array(self.request("GET", "/feed")[1]["posts"]), expected)

    def test_feed_batch_applies_inner_join_before_limit(self) -> None:
        rows = ",".join(
            f"({index * 100000 - 3000000},{999 if index > 25 else 1},'body',"
            f"'2026-01-01T00:00:{index:02}.000Z')"
            for index in range(1, 51)
        )
        self.fixture_sql(
            "PRAGMA foreign_keys=OFF; DELETE FROM likes; DELETE FROM posts;"
            f"INSERT INTO posts(id,user_id,body,created_at) VALUES {rows};"
        )
        reference = (
            "SELECT p.id,p.body,p.created_at,u.username AS author,0 AS like_count "
            "FROM posts p JOIN users u ON u.id=p.user_id "
            "ORDER BY p.created_at DESC,p.id DESC LIMIT 20"
        )
        expected = self.fixture_rows(reference)
        self.assertEqual(array(self.request("GET", "/feed")[1]["posts"]), expected)

    def test_special_characters_and_surrogate_username(self) -> None:
        text = 'prefix\0"\\\n\t <>&/ café \N{GRINNING FACE}'
        self.fixture_sql(f"UPDATE users SET username={literal(text)} WHERE id=1")
        status, body, _ = self.request(
            "POST", "/posts", json.dumps({"body": text}).encode(), sign(claims(username=text))
        )
        self.assertEqual(status, 201)
        post = mapping(body["post"])
        self.assertEqual((post["body"], post["author"]), (text, text))
        self.assertEqual(mapping(self.request("GET", f"/posts/{post['id']}")[1]["post"]), post)
        self.assertEqual(mapping(array(self.request("GET", "/feed")[1]["posts"])[0]), post)
        for offset in (0, 15, 16, 31, 32):
            username = "a" * offset + "\ud800" + "b" * 17 + "\udfff" + "c" * 17
            status, body, raw = self.request(
                "POST", "/posts", b'{"body":"valid"}', sign(claims(username=username))
            )
            self.assertEqual(status, 201)
            self.assertEqual(mapping(body["post"])["author"], username)
            self.assertEqual(
                raw, json.dumps(body, ensure_ascii=True, separators=(",", ":")).encode()
            )

    def test_writes_are_committed_before_response(self) -> None:
        status, body, _ = self.request(
            "POST", "/posts", b'{"body":"  committed  "}', sign(claims())
        )
        self.assertEqual(status, 201)
        post = mapping(body["post"])
        post_id = integer(post, "id")
        self.assertEqual(post["author"], "token-user")
        self.assertEqual(
            mapping(self.request("GET", f"/posts/{post_id}")[1]["post"])["body"], "committed"
        )
        for expected in (201, 200):
            status, result, _ = self.request("POST", f"/posts/{post_id}/like", auth=sign(claims()))
            self.assertEqual(status, expected)
            self.assertEqual(result["already_liked"], expected == 200)
            self.assertEqual(
                mapping(self.request("GET", f"/posts/{post_id}")[1]["post"])["like_count"], 1
            )
        self.assertEqual(
            self.persisted_rows(
                f"SELECT body,(SELECT count(*) FROM likes WHERE post_id={post_id}) AS n "
                f"FROM posts WHERE id={post_id}"
            ),
            [{"body": "committed", "n": 1}],
        )

    def test_invalid_json_and_duplicate_keys(self) -> None:
        for raw in (
            b'{"body":NaN}',
            b'{"body":Infinity}',
            b"\xff",
            b"{bad",
            b'{"body":"\xed\xa0\x80"}',
        ):
            with self.subTest(raw=raw):
                self.assertEqual(
                    self.request("POST", "/posts", raw, sign(claims()))[:2],
                    (400, {"error": "malformed JSON body"}),
                )
        for raw in (b"null", b"[]", b"1", b'{"body":true}'):
            self.assertEqual(
                self.request("POST", "/posts", raw, sign(claims()))[:2],
                (400, {"error": "body is required"}),
            )
        status, result, _ = self.request(
            "POST", "/posts", b'{"body":"old","body":"last"}', sign(claims())
        )
        self.assertEqual(status, 201)
        self.assertEqual(mapping(result["post"])["body"], "last")

    def test_unicode_whitespace_and_character_limit(self) -> None:
        text = "\ufeff\u00a0" + "\N{GRINNING FACE}" * 500 + "\u3000"
        status, body, _ = self.request(
            "POST", "/posts", json.dumps({"body": text}).encode(), sign(claims())
        )
        self.assertEqual(status, 201)
        self.assertEqual(mapping(body["post"])["body"], "\N{GRINNING FACE}" * 500)
        raw = json.dumps({"body": "\N{GRINNING FACE}" * 501}).encode()
        self.assertEqual(
            self.request("POST", "/posts", raw, sign(claims()))[:2],
            (400, {"error": "body must be at most 500 characters"}),
        )

    def test_failed_write_rolls_back_and_connection_remains_usable(self) -> None:
        self.assertEqual(
            self.request("POST", "/posts", b'{"body":"must not land"}', sign(claims(sub="999")))[0],
            500,
        )
        self.assertEqual(len(array(self.request("GET", "/feed")[1]["posts"])), 3)
        self.assertEqual(
            self.request("POST", "/posts", b'{"body":"after failure"}', sign(claims()))[0], 201
        )
        self.assertEqual(self.persisted_rows("SELECT count(*) AS n FROM posts"), [{"n": 4}])

    def test_external_sqlite_is_busy_until_graceful_stop(self) -> None:
        self.assertEqual(self.request("GET", "/feed")[0], 200)
        with self.assertRaises(subprocess.CalledProcessError) as result:
            command(
                "docker",
                "exec",
                self.container,
                "sqlite3",
                "-readonly",
                "/data/feed.db",
                "SELECT count(*) FROM posts",
            )
        self.assertEqual(result.exception.returncode, sqlite3.SQLITE_BUSY)
        self.assertIn("database is locked", str(result.exception.stderr))
        self.assertEqual(self.request("GET", "/health")[0], 200)
        self.assertEqual(self.request("POST", "/posts/3/like", auth=sign(claims()))[0], 201)
        self.assertEqual(mapping(self.request("GET", "/posts/3")[1]["post"])["like_count"], 1)
        self.assertEqual(self.persisted_rows("SELECT count(*) AS n FROM likes"), [{"n": 1}])

    def test_health_degrades_without_creating_database(self) -> None:
        missing = self.output / "missing.db"
        with server(self.output, image=self.image, database=missing, secret=SECRET.decode()) as (
            container,
            port,
        ):
            with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as connection:
                connection.request("GET", "/health")
                response = connection.getresponse()
                body = mapping(decode_json(response.read()))
                self.assertEqual(response.status, 503)
                self.assertEqual((body["status"], body["db"]), ("degraded", "unreachable"))
            with self.assertRaises(subprocess.CalledProcessError) as result:
                command("docker", "exec", container, "test", "-e", "/data/feed.db")
            self.assertEqual(result.exception.returncode, 1)

    def test_integer_boundaries_and_invalid_ids(self) -> None:
        largest = 2**63 - 1
        self.fixture_sql(f"UPDATE posts SET id={largest} WHERE id=3")
        self.assertEqual(
            mapping(self.request("GET", f"/posts/{largest}")[1]["post"])["id"], largest
        )
        for value, expected in (("0", 400), ("-1", 400), ("abc", 400), (str(2**63), 404)):
            self.assertEqual(self.request("GET", f"/posts/{value}")[0], expected)
        idle_socket = self.connection.sock
        for path, body in (("/posts", '{"body":"valid"}'), ("/posts/1/like", "")):
            with (
                self.subTest(path=path),
                closing(
                    socket.create_connection(("127.0.0.1", self.port), timeout=5)
                ) as connection,
            ):
                auth = sign(claims(sub=str(2**63)))
                request = (
                    f"POST {path} HTTP/1.1\r\nHost: local\r\nAuthorization: {auth}\r\n"
                    f"Content-Length: {len(body)}\r\n\r\n{body}"
                    "GET /health HTTP/1.1\r\nHost: local\r\n\r\n"
                )
                connection.sendall(request.encode())
                with connection.makefile("rb") as stream:
                    self.assertTrue(stream.readline().startswith(b"HTTP/1.1 500 "))
                    headers = http.client.parse_headers(stream)
                    self.assertEqual(headers["connection"], "close")
                    self.assertEqual(
                        mapping(decode_json(stream.read(int(headers["content-length"])))),
                        {"error": "internal server error"},
                    )
                    self.assertEqual(stream.read(1), b"")
            self.assertEqual(self.request("GET", "/health")[0], 200)
            self.assertIs(self.connection.sock, idle_socket)

    def test_json_escaping_matches_stdlib(self) -> None:
        rng = random.Random(42)
        alphabet = [chr(i) for i in range(128)] + ["é", "\u2028", "\N{GRINNING FACE}"]
        strings = ["a" * n + '"\\\0\n' + "b" * (35 - n) for n in range(36)]
        strings += [
            "a" * n + marker + "b" * 33
            for n in (0, 14, 15, 16, 30, 31, 32)
            for marker in ("", "\ud000", "\ud7ff")
        ]
        strings += ["".join(rng.choices(alphabet, k=rng.randrange(499))) for _ in range(1000)]
        for text in strings:
            value = "x" + text + "x"
            status, result, raw = self.request(
                "POST", "/posts", json.dumps({"body": value}).encode(), sign(claims(username=value))
            )
            self.assertEqual(status, 201)
            self.assertEqual(mapping(result["post"])["body"], value)
            self.assertEqual(mapping(result["post"])["author"], value)
            self.assertEqual(
                raw, json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()
            )
