"""Five API routes with fresh SQLite reads and JSON bytes ready for Granian."""

import logging
import os
import re
import sqlite3
import time

from .auth import AuthenticationError, authenticate
from .codec import decode_json, encode_json
from .db import Database
from .http import Request, ResponseProtocol
from .models import Response

JSON_HEADERS = [("content-type", "application/json")]
POST_ROUTE = re.compile(r"/posts/([^/]+)(/like)?\Z")
TRIM_WHITESPACE = " \t\n\v\f\r\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff"


class APIError(Exception):
    def __init__(self, status: int, message: str) -> None:
        self.status = status
        self.message = message


def positive_id(text: str) -> int:
    if re.fullmatch(r"[0-9]+", text) is None:
        raise APIError(400, "invalid post id")
    try:
        value = int(text)
    except ValueError:
        raise APIError(400, "invalid post id") from None
    if value <= 0:
        raise APIError(400, "invalid post id")
    if value > 2**63 - 1:
        raise APIError(404, "post not found")
    return value


class Application:
    def __init__(self, database_path: str, secret: bytes) -> None:
        self.database_path = database_path
        self.secret = secret
        self.started = time.monotonic()
        self._database: Database | None = None

    @property
    def database(self) -> Database:
        # Granian imports the app before serving; open SQLite on the thread that handles requests.
        if self._database is None:
            self._database = Database(self.database_path)
        return self._database

    def close(self) -> None:
        if self._database is not None:
            self._database.close()
            self._database = None

    async def handle(self, scope: Request, protocol: ResponseProtocol) -> tuple[int, Response]:
        method, path = scope.method, scope.path
        if method == "GET" and path == "/health":
            try:
                self.database.health()
            except sqlite3.Error as error:
                return 503, {
                    "status": "degraded",
                    "db": "unreachable",
                    "error": str(error),
                }
            return 200, {
                "status": "ok",
                "db": "ok",
                "uptime_s": int(time.monotonic() - self.started),
            }
        if method == "GET" and path == "/feed":
            return 200, self.database.feed()
        if method == "POST" and path == "/posts":
            identity = authenticate(scope.headers.get("authorization", ""), self.secret)
            try:
                payload = decode_json(await protocol())
            except ValueError, UnicodeError:
                raise APIError(400, "malformed JSON body") from None
            body = payload.get("body") if isinstance(payload, dict) else None
            if not isinstance(body, str) or not (body := body.strip(TRIM_WHITESPACE)):
                raise APIError(400, "body is required")
            if len(body) > 500:
                raise APIError(400, "body must be at most 500 characters")
            return 201, {"post": self.database.create(identity.user_id, identity.username, body)}
        route = POST_ROUTE.fullmatch(path)
        if route is not None and method == "GET" and route[2] is None:
            post_id = positive_id(route[1])
            post = self.database.post(post_id)
            if post is None:
                raise APIError(404, "post not found")
            return 200, post
        if route is not None and method == "POST" and route[2] is not None:
            identity = authenticate(scope.headers.get("authorization", ""), self.secret)
            post_id = positive_id(route[1])
            created = self.database.like(identity.user_id, post_id)
            if created is None:
                raise APIError(404, "post not found")
            return (201 if created else 200), {
                "liked": True,
                "already_liked": not created,
                "post_id": post_id,
            }
        raise APIError(404, "not found")

    async def __call__(self, scope: Request, protocol: ResponseProtocol) -> None:
        try:
            status, body = await self.handle(scope, protocol)
            encoded = body if isinstance(body, bytes) else encode_json(body)
        except AuthenticationError as error:
            status, encoded = 401, encode_json({"error": str(error)})
        except APIError as error:
            status, encoded = error.status, encode_json({"error": error.message})
        except Exception:
            logging.exception("request failed")
            status, encoded = 500, encode_json({"error": "internal server error"})
        protocol.response_bytes(status, JSON_HEADERS, encoded)


def create_application() -> Application:
    return Application(os.environ["SQLITE_PATH"], os.environ["JWT_SECRET"].encode("utf-8"))
