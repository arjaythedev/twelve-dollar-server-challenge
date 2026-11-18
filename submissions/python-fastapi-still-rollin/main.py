import os
import time
import json
import sqlite3

from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse
import jwt


# --------------------------------------------------
# Configuration
# --------------------------------------------------

DB_PATH = os.environ.get("SQLITE_PATH", "../../seed/feed.db")
JWT_SECRET = os.environ.get(
    "JWT_SECRET",
    "twelve-dollar-challenge"
)

START_TIME = time.monotonic()

app = FastAPI()


# --------------------------------------------------
# Database connection
#
# One vCPU, one process, one connection: every request runs its SQL inline
# on the event loop, so there's never lock contention between requests, and
# we skip the cost of opening/configuring a new connection every time.
# --------------------------------------------------

conn = sqlite3.connect(DB_PATH, check_same_thread=False)
conn.row_factory = sqlite3.Row
conn.execute("PRAGMA journal_mode = WAL")
conn.execute("PRAGMA synchronous = NORMAL")
conn.execute("PRAGMA mmap_size = 268435456")
conn.execute("PRAGMA cache_size = -65536")
conn.execute("PRAGMA temp_store = MEMORY")


# --------------------------------------------------
# Error handling
# --------------------------------------------------

@app.exception_handler(HTTPException)
async def http_error_handler(request, exc):
    return JSONResponse(
        status_code=exc.status_code,
        content={"error": exc.detail}
    )


@app.exception_handler(Exception)
async def internal_error_handler(request, exc):
    return JSONResponse(
        status_code=500,
        content={"error": "internal server error"}
    )


# --------------------------------------------------
# Authentication
# --------------------------------------------------

def authenticate(request: Request):
    header = request.headers.get("Authorization")

    if not header or not header.startswith("Bearer "):
        raise HTTPException(
            status_code=401,
            detail="missing bearer token"
        )

    token = header[7:]

    try:
        payload = jwt.decode(
            token,
            JWT_SECRET,
            algorithms=["HS256"],
            options={"require": ["exp"]}
        )
    except jwt.InvalidTokenError:
        raise HTTPException(
            status_code=401,
            detail="invalid or expired token"
        )

    user_id = payload.get("sub")
    username = payload.get("username")

    if (
        not isinstance(user_id, str)
        or not user_id.isascii()
        or not user_id.isdigit()
        or int(user_id) <= 0
        or not isinstance(username, str)
    ):
        raise HTTPException(
            status_code=401,
            detail="invalid token payload"
        )

    return int(user_id), username


# --------------------------------------------------
# Helpers
# --------------------------------------------------

def parse_post_id(post_id: str) -> int:
    if not post_id.isascii() or not post_id.isdigit():
        raise HTTPException(400, "invalid post id")

    value = int(post_id)

    if value <= 0:
        raise HTTPException(400, "invalid post id")

    return value


def fetch_post(post_id):
    return conn.execute("""
        SELECT
            p.id,
            p.body,
            p.created_at,
            u.username AS author,
            (
                SELECT COUNT(*)
                FROM likes l
                WHERE l.post_id = p.id
            ) AS like_count
        FROM posts p
        JOIN users u ON u.id = p.user_id
        WHERE p.id = ?
    """, (post_id,)).fetchone()


def post_to_dict(row):
    return {
        "id": row["id"],
        "body": row["body"],
        "created_at": row["created_at"],
        "author": row["author"],
        "like_count": row["like_count"]
    }


# --------------------------------------------------
# 1. Health check
# --------------------------------------------------

@app.get("/health")
async def health():
    try:
        conn.execute("SELECT 1").fetchone()

        return {
            "status": "ok",
            "db": "ok",
            "uptime_s": int(time.monotonic() - START_TIME)
        }

    except sqlite3.Error as exc:
        return JSONResponse(
            status_code=503,
            content={
                "status": "degraded",
                "db": "unreachable",
                "error": str(exc)
            }
        )


# --------------------------------------------------
# 2. Get feed
# --------------------------------------------------

@app.get("/feed")
async def get_feed():
    rows = conn.execute("""
        SELECT
            p.id,
            p.body,
            p.created_at,
            u.username AS author,
            (
                SELECT COUNT(*)
                FROM likes l
                WHERE l.post_id = p.id
            ) AS like_count
        FROM posts p
        JOIN users u ON u.id = p.user_id
        ORDER BY p.created_at DESC, p.id DESC
        LIMIT 20
    """).fetchall()

    return {
        "posts": [post_to_dict(row) for row in rows]
    }


# --------------------------------------------------
# 3. Get individual post
# --------------------------------------------------

@app.get("/posts/{post_id}")
async def get_post(post_id: str):
    post_id = parse_post_id(post_id)

    row = fetch_post(post_id)

    if row is None:
        raise HTTPException(404, "post not found")

    return {"post": post_to_dict(row)}


# --------------------------------------------------
# 4. Create post
# --------------------------------------------------

@app.post("/posts")
async def create_post(request: Request):
    # Authentication happens before body validation.
    user_id, username = authenticate(request)

    try:
        data = await request.json()
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise HTTPException(400, "malformed JSON body")

    if not isinstance(data, dict):
        raise HTTPException(400, "body is required")

    body = data.get("body")

    if not isinstance(body, str):
        raise HTTPException(400, "body is required")

    body = body.strip()

    if not body:
        raise HTTPException(400, "body is required")

    if len(body) > 500:
        raise HTTPException(
            400,
            "body must be at most 500 characters"
        )

    cursor = conn.execute("""
        INSERT INTO posts (user_id, body)
        VALUES (?, ?)
    """, (user_id, body))

    post_id = cursor.lastrowid

    created = conn.execute(
        "SELECT created_at FROM posts WHERE id = ?",
        (post_id,)
    ).fetchone()

    conn.commit()

    return JSONResponse(
        status_code=201,
        content={
            "post": {
                "id": post_id,
                "body": body,
                "created_at": created["created_at"],
                "author": username,
                "like_count": 0
            }
        }
    )


# --------------------------------------------------
# 5. Like post
# --------------------------------------------------

@app.post("/posts/{post_id}/like")
async def like_post(post_id: str, request: Request):
    # Important: authenticate BEFORE validating post_id.
    user_id, username = authenticate(request)

    post_id = parse_post_id(post_id)

    post = conn.execute(
        "SELECT id FROM posts WHERE id = ?",
        (post_id,)
    ).fetchone()

    if post is None:
        raise HTTPException(404, "post not found")

    cursor = conn.execute("""
        INSERT INTO likes (user_id, post_id)
        VALUES (?, ?)
        ON CONFLICT (user_id, post_id) DO NOTHING
    """, (user_id, post_id))

    already_liked = cursor.rowcount == 0

    conn.commit()

    return JSONResponse(
        status_code=200 if already_liked else 201,
        content={
            "liked": True,
            "already_liked": already_liked,
            "post_id": post_id
        }
    )


# --------------------------------------------------
# Unknown routes
# --------------------------------------------------

@app.api_route(
    "/{path:path}",
    methods=["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"]
)
def not_found(path: str):
    raise HTTPException(404, "not found")
