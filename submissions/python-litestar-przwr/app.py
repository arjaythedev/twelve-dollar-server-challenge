"""The $12 Server Challenge API on Litestar (single file, stdlib-only beyond litestar/msgspec)."""
import base64
import hashlib
import hmac
import json
import os
import sqlite3
import threading
import time
from typing import Any

import msgspec
from litestar import Litestar, Request, Response, get, post
from litestar.exceptions import NotFoundException
from litestar.status_codes import HTTP_400_BAD_REQUEST, HTTP_401_UNAUTHORIZED, HTTP_404_NOT_FOUND
from litestar.status_codes import HTTP_500_INTERNAL_SERVER_ERROR, HTTP_503_SERVICE_UNAVAILABLE

SQLITE_PATH = os.environ["SQLITE_PATH"]
JWT_SECRET = os.environ.get("JWT_SECRET", "twelve-dollar-challenge").encode()
START = time.monotonic()

POST_COLS = "p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
FEED_SQL = (
    f"SELECT {POST_COLS} FROM posts p JOIN users u ON u.id = p.user_id"
    " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
)
POST_SQL = f"SELECT {POST_COLS} FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?"
INSERT_POST_SQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
INSERT_LIKE_SQL = "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING"

_local = threading.local()


def db() -> sqlite3.Connection:
    conn = getattr(_local, "conn", None)
    if conn is None:
        conn = sqlite3.connect(SQLITE_PATH, isolation_level=None, check_same_thread=False)
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute("PRAGMA mmap_size=268435456")
        conn.execute("PRAGMA temp_store=MEMORY")
        conn.execute("PRAGMA cache_size=-8192")
        _local.conn = conn
    return conn


class Post(msgspec.Struct, frozen=True):
    id: int
    body: str
    created_at: str
    author: str
    like_count: int


class Feed(msgspec.Struct, frozen=True):
    posts: list[Post]


class Created(msgspec.Struct, frozen=True):
    post: Post


class Liked(msgspec.Struct, frozen=True):
    liked: bool
    already_liked: bool
    post_id: int


class Health(msgspec.Struct, frozen=True):
    status: str
    db: str
    uptime_s: int


class Degraded(msgspec.Struct, frozen=True):
    status: str
    db: str
    error: str


class ApiError(Exception):
    def __init__(self, status: int, message: str):
        self.status = status
        self.message = message


def err(status: int, message: str) -> Response:
    return Response({"error": message}, status_code=status)


def api_error_handler(_: Request, exc: ApiError) -> Response:
    return err(exc.status, exc.message)


def not_found_handler(_: Request, __: NotFoundException) -> Response:
    return err(HTTP_404_NOT_FOUND, "not found")


def _b64url(seg: str) -> bytes:
    return base64.urlsafe_b64decode(seg + "=" * (-len(seg) % 4))


def verify_token(auth_header: str | None) -> tuple[int, str]:
    if not auth_header or not auth_header.startswith("Bearer "):
        raise ApiError(HTTP_401_UNAUTHORIZED, "missing bearer token")
    token = auth_header[7:]
    parts = token.split(".")
    if len(parts) != 3:
        raise ApiError(HTTP_401_UNAUTHORIZED, "invalid or expired token")
    head_b64, payload_b64, sig_b64 = parts
    try:
        header = json.loads(_b64url(head_b64))
        payload = json.loads(_b64url(payload_b64))
        sig = _b64url(sig_b64)
    except (ValueError, json.JSONDecodeError):
        raise ApiError(HTTP_401_UNAUTHORIZED, "invalid or expired token") from None
    if not isinstance(header, dict) or header.get("alg") != "HS256":
        raise ApiError(HTTP_401_UNAUTHORIZED, "invalid or expired token")
    expected = hmac.new(JWT_SECRET, f"{head_b64}.{payload_b64}".encode(), hashlib.sha256).digest()
    if not hmac.compare_digest(sig, expected):
        raise ApiError(HTTP_401_UNAUTHORIZED, "invalid or expired token")
    if not isinstance(payload, dict) or not isinstance(payload.get("exp"), (int, float)):
        raise ApiError(HTTP_401_UNAUTHORIZED, "invalid or expired token") from None
    if payload["exp"] <= time.time():
        raise ApiError(HTTP_401_UNAUTHORIZED, "invalid or expired token")
    sub, username = payload.get("sub"), payload.get("username")
    if not isinstance(sub, str) or not sub.isdigit() or int(sub) <= 0 or not isinstance(username, str):
        raise ApiError(HTTP_401_UNAUTHORIZED, "invalid token payload")
    return int(sub), username


def parse_id(raw: str) -> int:
    if not raw.isdigit() or int(raw) <= 0:
        raise ApiError(HTTP_400_BAD_REQUEST, "invalid post id")
    return int(raw)


@get("/health")
async def health() -> Response:
    try:
        db().execute("SELECT 1").fetchone()
        return Response(Health("ok", "ok", int(time.monotonic() - START)))
    except sqlite3.Error as e:
        return Response(Degraded("degraded", "unreachable", str(e)), status_code=HTTP_503_SERVICE_UNAVAILABLE)


@get("/feed")
async def feed() -> Feed:
    return Feed([Post(*row) for row in db().execute(FEED_SQL).fetchall()])


@get("/posts/{pid:str}")
async def get_post(pid: str) -> Response:
    try:
        post_id = parse_id(pid)
    except ApiError as e:
        return err(e.status, e.message)
    row = db().execute(POST_SQL, (post_id,)).fetchone()
    if row is None:
        return err(HTTP_404_NOT_FOUND, "post not found")
    return Response(Created(Post(*row)))


@post("/posts")
async def create_post(request: Request) -> Response:
    user_id, username = verify_token(request.headers.get("authorization"))
    raw = await request.body()
    try:
        doc: Any = msgspec.json.decode(raw)
    except msgspec.DecodeError:
        return err(HTTP_400_BAD_REQUEST, "malformed JSON body")
    body = doc.get("body") if isinstance(doc, dict) else None
    if not isinstance(body, str) or not (body := body.strip()):
        return err(HTTP_400_BAD_REQUEST, "body is required")
    if len(body) > 500:
        return err(HTTP_400_BAD_REQUEST, "body must be at most 500 characters")
    new_id, created_at = db().execute(INSERT_POST_SQL, (user_id, body)).fetchone()
    return Response(Created(Post(new_id, body, created_at, username, 0)), status_code=201)


@post("/posts/{pid:str}/like")
async def like_post(pid: str, request: Request) -> Response:
    user_id, _ = verify_token(request.headers.get("authorization"))
    post_id = parse_id(pid)
    if db().execute("SELECT 1 FROM posts WHERE id = ?", (post_id,)).fetchone() is None:
        return err(HTTP_404_NOT_FOUND, "post not found")
    inserted = db().execute(INSERT_LIKE_SQL, (user_id, post_id)).rowcount == 1
    return Response(Liked(True, not inserted, post_id), status_code=201 if inserted else 200)


app = Litestar(
    [health, feed, get_post, create_post, like_post],
    exception_handlers={ApiError: api_error_handler, NotFoundException: not_found_handler},
)
