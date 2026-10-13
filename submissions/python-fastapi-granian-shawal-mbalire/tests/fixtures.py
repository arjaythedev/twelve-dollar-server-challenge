"""Fakes for the domain's ports, and a tiny database built from the challenge's schema.sql."""

import base64
import hashlib
import hmac
import json
import sqlite3
from pathlib import Path

from domain.errors import StoreUnavailable
from domain.models import LikeOutcome, Principal

SCHEMA = Path(__file__).resolve().parents[3] / "schema.sql"
SECRET = b"test-secret"


class FakeStore:
    def __init__(self, posts: dict[int, bytes] | None = None, up: bool = True):
        self.posts = dict(posts or {})
        self.likes: set[tuple[int, int]] = set()
        self.created: list[tuple[Principal, str]] = []
        self.up = up

    async def feed(self) -> bytes:
        return b'{"posts":[]}'

    async def post(self, post_id: int) -> bytes | None:
        return self.posts.get(post_id)

    async def create_post(self, author: Principal, body: str) -> bytes:
        self.created.append((author, body))
        return b'{"post":"new"}'

    async def like(self, user_id: int, post_id: int) -> LikeOutcome:
        if post_id not in self.posts:
            return LikeOutcome.POST_MISSING
        if (user_id, post_id) in self.likes:
            return LikeOutcome.ALREADY_LIKED
        self.likes.add((user_id, post_id))
        return LikeOutcome.LIKED

    def ping(self) -> None:
        if not self.up:
            raise StoreUnavailable("disk on fire")


class FakeVerifier:
    """Accepts tokens of the form `ok:<json claims>`."""

    def claims(self, token: str) -> dict | None:
        return json.loads(token[3:]) if token.startswith("ok:") else None


class FixedClock:
    def __init__(self, wall: float = 1_000_000.0, monotonic: float = 50.0):
        self.now = wall
        self.mono = monotonic

    def wall(self) -> float:
        return self.now

    def monotonic(self) -> float:
        return self.mono


def bearer(claims: dict) -> str:
    return "Bearer ok:" + json.dumps(claims)


def sign(claims: dict, secret: bytes = SECRET, header: dict | None = None) -> str:
    """A real HS256 JWT."""

    def b64(raw: bytes) -> str:
        return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()

    signing_input = b64(json.dumps(header or {"alg": "HS256", "typ": "JWT"}).encode()) + "." + b64(json.dumps(claims).encode())
    signature = hmac.new(secret, signing_input.encode(), hashlib.sha256).digest()
    return signing_input + "." + b64(signature)


def make_db(path: Path) -> Path:
    """Two users, three posts (post 2 is newest), two likes on post 1."""
    db = sqlite3.connect(path)
    db.executescript(SCHEMA.read_text())
    db.executescript(
        """
        INSERT INTO users (id, username) VALUES (1, 'alice'), (2, 'bob');
        INSERT INTO posts (id, user_id, body, created_at) VALUES
          (1, 1, 'first', '2025-01-01T00:00:00.000Z'),
          (2, 2, 'say "hi"', '2025-01-03T00:00:00.000Z'),
          (3, 1, 'third', '2025-01-02T00:00:00.000Z');
        INSERT INTO likes (user_id, post_id) VALUES (1, 1), (2, 1);
        """
    )
    db.commit()
    db.close()
    return path
