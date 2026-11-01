"""HTTP regressions for authentication, fresh reads, JSON and committed writes."""

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

from dev import ROOT, JSONValue, command, decode_json, integer, mapping, server

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
        self.container, self.port = self.stack.enter_context(
            server(self.output, database=self.database, secret=SECRET.decode())
        )
        self.connection = self.stack.enter_context(
            closing(http.client.HTTPConnection("127.0.0.1", self.port, timeout=5))
        )

    def request(
        self, method: str, path: str, body: bytes = b"", auth: str | None = None
    ) -> tuple[int, dict[str, JSONValue], bytes]:
        self.connection.request(method, path, body, {"authorization": auth} if auth else {})
        response = self.connection.getresponse()
        raw = response.read()
        self.assertEqual(response.getheader("content-type"), "application/json")
        return response.status, mapping(decode_json(raw)), raw

    def sql(self, statement: str) -> list[JSONValue]:
        result = command(
            "docker", "exec", self.container, "sqlite3", "-json", "/data/feed.db", statement
        )
        return array(decode_json(result.encode())) if result else []

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
        self.sql("INSERT INTO likes(user_id,post_id) VALUES(1,3)")
        refreshed = array(self.request("GET", "/feed")[1]["posts"])
        self.assertEqual(mapping(refreshed[0])["like_count"], 1)
        self.assertEqual(mapping(self.request("GET", "/posts/3")[1]["post"])["like_count"], 1)

    def test_empty_feed_and_nonmonotonic_timestamps(self) -> None:
        self.sql("DELETE FROM posts")
        self.assertEqual(self.request("GET", "/feed")[:2], (200, {"posts": []}))
        rows = ",".join(
            f"({i},1,'body','2026-01-{31 - i // 2:02}T00:00:00.000Z')" for i in range(1, 32)
        )
        self.sql(f"INSERT INTO posts(id,user_id,body,created_at) VALUES {rows}")
        posts = array(self.request("GET", "/feed")[1]["posts"])
        self.assertEqual(
            [mapping(post)["id"] for post in posts],
            [1, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 17, 16, 19, 18, 21],
        )

    def test_special_characters_and_surrogate_username(self) -> None:
        text = 'prefix\0"\\\n\t <>&/ café \N{GRINNING FACE}'
        self.sql(f"UPDATE users SET username={literal(text)} WHERE id=1")
        status, body, _ = self.request(
            "POST", "/posts", json.dumps({"body": text}).encode(), sign(claims(username=text))
        )
        self.assertEqual(status, 201)
        post = mapping(body["post"])
        self.assertEqual((post["body"], post["author"]), (text, text))
        self.assertEqual(mapping(self.request("GET", f"/posts/{post['id']}")[1]["post"]), post)
        self.assertEqual(mapping(array(self.request("GET", "/feed")[1]["posts"])[0]), post)
        status, body, _ = self.request(
            "POST", "/posts", b'{"body":"valid"}', sign(claims(username="\ud800"))
        )
        self.assertEqual(status, 201)
        self.assertEqual(mapping(body["post"])["author"], "\ud800")

    def test_writes_are_committed_before_response(self) -> None:
        status, body, _ = self.request(
            "POST", "/posts", b'{"body":"  committed  "}', sign(claims())
        )
        self.assertEqual(status, 201)
        post = mapping(body["post"])
        post_id = integer(post, "id")
        self.assertEqual(post["author"], "token-user")
        self.assertEqual(
            self.sql(f"SELECT body FROM posts WHERE id={post_id}"), [{"body": "committed"}]
        )
        for expected in (201, 200):
            status, result, _ = self.request("POST", f"/posts/{post_id}/like", auth=sign(claims()))
            self.assertEqual(status, expected)
            self.assertEqual(result["already_liked"], expected == 200)
            self.assertEqual(
                self.sql(f"SELECT count(*) AS n FROM likes WHERE post_id={post_id}"), [{"n": 1}]
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
        self.assertEqual(self.sql("SELECT count(*) AS n FROM posts"), [{"n": 3}])
        self.assertEqual(
            self.request("POST", "/posts", b'{"body":"after failure"}', sign(claims()))[0], 201
        )

    def test_health_degrades_without_creating_database(self) -> None:
        missing = self.output / "missing.db"
        with server(self.output, database=missing, secret=SECRET.decode()) as (container, port):
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
        self.sql(f"UPDATE posts SET id={largest} WHERE id=3")
        self.assertEqual(
            mapping(self.request("GET", f"/posts/{largest}")[1]["post"])["id"], largest
        )
        for value, expected in (("0", 400), ("-1", 400), ("abc", 400), (str(2**63), 404)):
            self.assertEqual(self.request("GET", f"/posts/{value}")[0], expected)

    def test_json_escaping_matches_stdlib(self) -> None:
        rng = random.Random(42)
        alphabet = [chr(i) for i in range(128)] + ["é", "\u2028", "\N{GRINNING FACE}"]
        strings = ["a" * n + '"\\\0\n' + "b" * (35 - n) for n in range(36)]
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
