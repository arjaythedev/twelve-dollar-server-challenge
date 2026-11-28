import os
import time
from typing import Optional

import apsw
import orjson
from auth import check_auth
from db import db as global_db
from db import fetchone, init_db

# Global variables
db: Optional[apsw.Connection] = None
start_time = time.time()

# PR #13: Descriptor guard — track open file descriptors
_DESCRIPTOR_BUDGET = 40000  # PR #13: retire connections at 40,000 open descriptors
_DESCRIPTOR_CHECK_INTERVAL = 0.1  # sample at most once per 100ms
_last_descriptor_check = 0.0
_descriptor_count = 0

# --- SQL queries with JSON generation inside SQLite (PR #13, #41) ---

SQL_FEED = """
SELECT '{"posts":[' ||
    COALESCE(
        group_concat(
            json_object(
                'id', p.id,
                'body', p.body,
                'created_at', p.created_at,
                'author', u.username,
                'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
            ),
            ','
        ),
        ''
    ) || ']}'
FROM (
    SELECT p.id, p.body, p.created_at, p.user_id
    FROM posts p
    ORDER BY p.created_at DESC, p.id DESC
    LIMIT 20
) p
JOIN users u ON u.id = p.user_id
"""

SQL_POST = """
SELECT '{"post":' ||
    json_object(
        'id', p.id,
        'body', p.body,
        'created_at', p.created_at,
        'author', u.username,
        'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
    ) || '}'
FROM posts p
JOIN users u ON u.id = p.user_id
WHERE p.id = ?
"""

SQL_INSERT_POST = """
INSERT INTO posts (user_id, body) VALUES (?, ?)
RETURNING '{"post":' || json_object(
    'id', id,
    'body', body,
    'created_at', created_at,
    'author', (SELECT username FROM users WHERE id = user_id),
    'like_count', 0
) || '}'
"""

SQL_CHECK_POST = "SELECT 1 FROM posts WHERE id = ?"

SQL_LIKE_RETURNING = """
INSERT INTO likes (user_id, post_id) VALUES (?, ?)
ON CONFLICT (user_id, post_id) DO NOTHING
RETURNING 1
"""

SQL_HEALTH = "SELECT 1"


def _should_retire_connection() -> bool:
    """
    PR #13: Descriptor guard.
    Проверяет количество открытых дескрипторов и решает, стоит ли отдать
    Connection: close, чтобы не упереться в LimitNOFILE.
    """
    global _last_descriptor_check, _descriptor_count
    now = time.monotonic()
    if now - _last_descriptor_check < _DESCRIPTOR_CHECK_INTERVAL:
        return _descriptor_count > _DESCRIPTOR_BUDGET

    _last_descriptor_check = now
    try:
        _descriptor_count = len(os.listdir("/proc/self/fd"))
    except Exception:
        _descriptor_count = 0
    return _descriptor_count > _DESCRIPTOR_BUDGET


async def app(scope, receive, send):
    """ASGI entry point."""
    if scope["type"] == "lifespan":
        while True:
            message = await receive()
            if message["type"] == "lifespan.startup":
                sqlite_path = os.environ.get("SQLITE_PATH", "feed.db")
                await init_db(sqlite_path)
                global db
                import db as db_module

                db = db_module.db
                await send({"type": "lifespan.startup.complete"})
            elif message["type"] == "lifespan.shutdown":
                if db:
                    await db.close()
                await send({"type": "lifespan.shutdown.complete"})
                return
        return

    assert scope["type"] == "http"

    # Read request body
    body = b""
    while True:
        msg = await receive()
        if msg["type"] == "http.request":
            body += msg.get("body", b"")
            if not msg.get("more_body", False):
                break
        elif msg["type"] == "http.disconnect":
            return

    path = scope["path"]
    method = scope["method"]

    try:
        status, response_body = await handle_request(method, path, body, scope)
    except Exception:
        status = 500
        response_body = orjson.dumps({"error": "internal server error"})

    # PR #13: Descriptor guard
    extra_headers = []
    if _should_retire_connection():
        extra_headers.append((b"connection", b"close"))

    await send(
        {
            "type": "http.response.start",
            "status": status,
            "headers": [
                (b"content-type", b"application/json"),
                (b"content-length", str(len(response_body)).encode()),
            ]
            + extra_headers,
        }
    )
    await send(
        {
            "type": "http.response.body",
            "body": response_body,
        }
    )


async def handle_request(
    method: str, path: str, body: bytes, scope: dict
) -> tuple[int, bytes]:
    global db

    # --- GET /health ---
    if path == "/health" and method == "GET":
        try:
            await db.execute(SQL_HEALTH)
            uptime = int(time.time() - start_time)
            return 200, orjson.dumps({"status": "ok", "db": "ok", "uptime_s": uptime})
        except Exception as e:
            return 503, orjson.dumps(
                {"status": "degraded", "db": "unreachable", "error": str(e)}
            )

    # --- GET /feed ---
    if path == "/feed" and method == "GET":
        cursor = await db.execute(SQL_FEED)
        row = await fetchone(cursor)
        if row:
            # SQLite already returned ready JSON — just serve the bytes
            return 200, row[0].encode()
        return 200, b'{"posts":[]}'

    # --- GET /posts/:id ---
    if path.startswith("/posts/") and method == "GET" and path.count("/") == 2:
        post_id_str = path.split("/")[2]
        try:
            post_id = int(post_id_str)
            if post_id <= 0:
                raise ValueError
        except ValueError:
            return 400, orjson.dumps({"error": "invalid post id"})

        cursor = await db.execute(SQL_POST, (post_id,))
        row = await fetchone(cursor)
        if not row:
            return 404, orjson.dumps({"error": "post not found"})

        return 200, row[0].encode()

    # --- POST /posts/:id/like ---
    if (
        path.startswith("/posts/")
        and method == "POST"
        and path.endswith("/like")
        and path.count("/") == 3
    ):
        user_id, username, auth_error = check_auth(scope)
        if auth_error:
            status = 401
            msg = auth_error
            return status, orjson.dumps({"error": msg})

        post_id_str = path.split("/")[2]
        try:
            post_id = int(post_id_str)
            if post_id <= 0:
                raise ValueError
        except ValueError:
            return 400, orjson.dumps({"error": "invalid post id"})

        # Check post exists
        cursor = await db.execute(SQL_CHECK_POST, (post_id,))
        exists = await fetchone(cursor)
        if not exists:
            return 404, orjson.dumps({"error": "post not found"})

        cursor = await db.execute(SQL_LIKE_RETURNING, (user_id, post_id))
        inserted = await fetchone(cursor)

        if inserted:
            return 201, orjson.dumps(
                {"liked": True, "already_liked": False, "post_id": post_id}
            )
        else:
            return 200, orjson.dumps(
                {"liked": True, "already_liked": True, "post_id": post_id}
            )

    # --- POST /posts ---
    if path == "/posts" and method == "POST":
        user_id, username, auth_error = check_auth(scope)
        if auth_error:
            status = 401
            msg = auth_error
            return status, orjson.dumps({"error": msg})

        try:
            data = orjson.loads(body)
        except Exception:
            return 400, orjson.dumps({"error": "malformed JSON body"})

        if not isinstance(data, dict) or "body" not in data:
            return 400, orjson.dumps({"error": "body is required"})

        post_body = data["body"]
        if not isinstance(post_body, str):
            return 400, orjson.dumps({"error": "body is required"})

        post_body = post_body.strip()
        if not post_body:
            return 400, orjson.dumps({"error": "body is required"})
        if len(post_body) > 500:
            return 400, orjson.dumps({"error": "body must be at most 500 characters"})

        cursor = await db.execute(SQL_INSERT_POST, (user_id, post_body))
        row = await fetchone(cursor)
        return 201, row[0].encode()

    # --- Everything else ---
    return 404, orjson.dumps({"error": "not found"})
