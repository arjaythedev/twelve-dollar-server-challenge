"""Focused checks beyond the official suite: JWT edge cases and committed state."""

import asyncio
import base64
import hashlib
import hmac
import json
import sqlite3
import tempfile
import time
import unittest
from contextlib import closing
from dataclasses import dataclass
from pathlib import Path
from unittest.mock import patch

from api.app import Application
from api.auth import AuthenticationError, authenticate
from api.codec import JSONValue, decode_json

ROOT = Path(__file__).resolve().parents[2]
SECRET = b"test-secret"


def sign(claims: JSONValue, algorithm: str = "HS256") -> str:
    def segment(value: JSONValue) -> bytes:
        return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).rstrip(
            b"="
        )

    content = segment({"alg": algorithm}) + b"." + segment(claims)
    signature = base64.urlsafe_b64encode(hmac.new(SECRET, content, hashlib.sha256).digest()).rstrip(
        b"="
    )
    return "Bearer " + (content + b"." + signature).decode("ascii")


def claims(**overrides: JSONValue) -> dict[str, JSONValue]:
    return {
        "sub": "1",
        "username": "token-user",
        "exp": time.time() + 3600,
        **overrides,
    }


class AuthenticationTests(unittest.TestCase):
    def test_expiration_is_required_numeric_finite_and_not_boolean(self) -> None:
        missing = claims()
        del missing["exp"]
        for value in [
            missing,
            *[claims(exp=exp) for exp in (None, True, "9999999999", 0, float("nan"), float("inf"))],
        ]:
            with (
                self.subTest(claims=value),
                self.assertRaisesRegex(AuthenticationError, "invalid or expired token"),
            ):
                authenticate(sign(value), SECRET)

    def test_expiration_is_checked_again_for_the_same_token(self) -> None:
        token = sign(claims(exp=1000))
        with patch("api.auth.time.time", return_value=999):
            self.assertEqual(authenticate(token, SECRET).user_id, 1)
        with patch("api.auth.time.time", return_value=1000):
            with self.assertRaisesRegex(AuthenticationError, "invalid or expired token"):
                authenticate(token, SECRET)

    def test_signature_and_algorithm_are_verified(self) -> None:
        token = sign(claims())
        corrupted = token.rsplit(".", 1)[0] + "." + "A" * 43
        for header in (
            corrupted,
            sign(claims(), algorithm="HS512"),
            "Bearer a.b.c",
            "Bearer \N{SNOWMAN}",
        ):
            with (
                self.subTest(header=header),
                self.assertRaisesRegex(AuthenticationError, "invalid or expired token"),
            ):
                authenticate(header, SECRET)

    def test_payload_validation_and_verbatim_username(self) -> None:
        for subject in (1, True, "0", "-1", "1.5", "", "abc"):
            with (
                self.subTest(subject=subject),
                self.assertRaisesRegex(AuthenticationError, "invalid token payload"),
            ):
                authenticate(sign(claims(sub=subject)), SECRET)
        for username in (None, 1, True):
            with (
                self.subTest(username=username),
                self.assertRaisesRegex(AuthenticationError, "invalid token payload"),
            ):
                authenticate(sign(claims(username=username)), SECRET)
        identity = authenticate(sign(claims(username='name "quoted" \N{SNOWMAN}')), SECRET)
        self.assertEqual(identity.username, 'name "quoted" \N{SNOWMAN}')


def as_object(value: JSONValue) -> dict[str, JSONValue]:
    if not isinstance(value, dict):
        raise AssertionError("expected a JSON object")
    return value


def as_array(value: JSONValue) -> list[JSONValue]:
    if not isinstance(value, list):
        raise AssertionError("expected a JSON array")
    return value


def as_int(value: JSONValue) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise AssertionError("expected an integer")
    return value


@dataclass(slots=True)
class Scope:
    method: str
    path: str
    headers: dict[str, str]


class Protocol:
    def __init__(self, body: bytes) -> None:
        self.body = body
        self.reads = 0
        self.response: tuple[int, dict[str, str], bytes] | None = None

    async def __call__(self) -> bytes:
        self.reads += 1
        return self.body

    def response_bytes(self, status: int, headers: list[tuple[str, str]], body: bytes) -> None:
        if self.response is not None:
            raise AssertionError("multiple responses")
        self.response = status, dict(headers), body


class ApplicationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "feed.db"
        with closing(sqlite3.connect(self.path)) as connection, connection:
            connection.executescript((ROOT / "schema.sql").read_text(encoding="utf-8"))
            connection.execute("INSERT INTO users(id, username) VALUES(1, 'db-user')")
            connection.executemany(
                "INSERT INTO posts(id, user_id, body, created_at) VALUES(?, 1, ?, ?)",
                [
                    (1, "old", "2024-01-01T00:00:00.000Z"),
                    (2, "tie-low", "2025-01-01T00:00:00.000Z"),
                    (3, "tie-high", "2025-01-01T00:00:00.000Z"),
                ],
            )
        self.application = Application(str(self.path), SECRET)
        self.addCleanup(self.close_database)

    def close_database(self) -> None:
        self.application.close()

    def request(
        self, method: str, path: str, body: bytes = b"", auth: str | None = None
    ) -> tuple[int, dict[str, JSONValue], Protocol]:
        protocol = Protocol(body)
        scope = Scope(method=method, path=path, headers={"authorization": auth} if auth else {})
        asyncio.run(self.application(scope, protocol))
        response = protocol.response
        assert response is not None
        status, headers, raw = response
        self.assertEqual(headers["content-type"], "application/json")
        return status, as_object(decode_json(raw)), protocol

    def test_feed_order_and_fresh_like_counts(self) -> None:
        status, body, _ = self.request("GET", "/feed")
        self.assertEqual(status, 200)
        posts = [as_object(post) for post in as_array(body["posts"])]
        self.assertEqual([post["id"] for post in posts], [3, 2, 1])
        self.assertEqual(list(posts[0]), ["id", "body", "created_at", "author", "like_count"])
        with closing(sqlite3.connect(self.path)) as observer, observer:
            observer.execute("INSERT INTO likes(user_id, post_id) VALUES(1, 3)")
        refreshed = as_array(self.request("GET", "/feed")[1]["posts"])
        self.assertEqual(as_object(refreshed[0])["like_count"], 1)

    def test_auth_precedes_id_validation_and_body_read(self) -> None:
        status, body, protocol = self.request("POST", "/posts/abc/like", body=b"not JSON")
        self.assertEqual((status, body), (401, {"error": "missing bearer token"}))
        self.assertEqual(protocol.reads, 0)
        self.assertEqual(
            self.request("POST", "/posts/abc/like", auth=sign(claims()))[:2],
            (400, {"error": "invalid post id"}),
        )
        status, _, protocol = self.request("POST", "/posts", body=b"not JSON")
        self.assertEqual(status, 401)
        self.assertEqual(protocol.reads, 0)

    def test_post_and_like_are_committed_before_response(self) -> None:
        status, body, _ = self.request(
            "POST", "/posts", b'{"body":"  committed  "}', sign(claims())
        )
        self.assertEqual(status, 201)
        post = as_object(body["post"])
        post_id = as_int(post["id"])
        self.assertEqual(post["author"], "token-user")
        with closing(sqlite3.connect(self.path)) as observer, observer:
            self.assertEqual(
                observer.execute("SELECT body FROM posts WHERE id=?", (post_id,)).fetchone(),
                ("committed",),
            )
            for expected in (201, 200):
                status, result, _ = self.request(
                    "POST", f"/posts/{post_id}/like", auth=sign(claims())
                )
                self.assertEqual(status, expected)
                self.assertEqual(result["already_liked"], expected == 200)
            self.assertEqual(
                observer.execute(
                    "SELECT count(*) FROM likes WHERE post_id=?", (post_id,)
                ).fetchone(),
                (1,),
            )

    def test_invalid_json_and_non_object_bodies(self) -> None:
        for raw in (b'{"body":NaN}', b'{"body":Infinity}', b"\xff", b"{bad"):
            with self.subTest(raw=raw):
                self.assertEqual(
                    self.request("POST", "/posts", raw, sign(claims()))[:2],
                    (400, {"error": "malformed JSON body"}),
                )
        for raw in (b"null", b"[]", b"1", b'{"body":true}'):
            with self.subTest(raw=raw):
                self.assertEqual(
                    self.request("POST", "/posts", raw, sign(claims()))[:2],
                    (400, {"error": "body is required"}),
                )

    def test_unicode_whitespace_and_character_limit(self) -> None:
        text = "\ufeff\u00a0" + "\N{GRINNING FACE}" * 500 + "\u3000"
        status, body, _ = self.request(
            "POST", "/posts", json.dumps({"body": text}).encode(), sign(claims())
        )
        self.assertEqual(status, 201)
        self.assertEqual(as_object(body["post"])["body"], "\N{GRINNING FACE}" * 500)
        status, body, _ = self.request(
            "POST",
            "/posts",
            json.dumps({"body": "\N{GRINNING FACE}" * 501}).encode(),
            sign(claims()),
        )
        self.assertEqual((status, body), (400, {"error": "body must be at most 500 characters"}))

    def test_failed_write_rolls_back_and_connection_remains_usable(self) -> None:
        database = self.application.database
        with self.assertRaises(sqlite3.IntegrityError):
            database.create(999, "missing", "must not land")
        with closing(sqlite3.connect(self.path)) as observer, observer:
            self.assertEqual(observer.execute("SELECT count(*) FROM posts").fetchone(), (3,))
        self.assertEqual(
            self.request("POST", "/posts", b'{"body":"after failure"}', sign(claims()))[0],
            201,
        )

    def test_health_degrades_without_creating_missing_database(self) -> None:
        missing = self.path.parent / "missing.db"
        self.application = Application(str(missing), SECRET)
        status, body, _ = self.request("GET", "/health")
        self.assertEqual(status, 503)
        self.assertEqual((body["status"], body["db"]), ("degraded", "unreachable"))
        self.assertFalse(missing.exists())


if __name__ == "__main__":
    unittest.main()
