"""The $12 server challenge API: PyRustAPI (Native Rust Tokio/Hyper Core) on SQLite.
Tuned for maximum async event-loop throughput with zero spawn_blocking thread hops.
"""
import base64
import gc
import hashlib
import hmac
import os
import re
import sqlite3
import time

import orjson
from rustapi import FastAPI, Request
from rustapi.responses import Response

SQLITE_PATH = os.environ["SQLITE_PATH"]
JWT_SECRET = os.environ["JWT_SECRET"].encode("utf-8")
START = time.monotonic()
MAX_ID = 2**63 - 1  # SQLite's INTEGER range

# --- database ---------------------------------------------------------------------------------

# Single shared connection running inline on the async loop (exact match to FastAPI design)
db = sqlite3.connect(SQLITE_PATH, isolation_level=None, check_same_thread=False, cached_statements=64)
db.execute("PRAGMA journal_mode = WAL")
db.execute("PRAGMA synchronous = NORMAL")  # rule 6: WAL + NORMAL
db.execute("PRAGMA busy_timeout = 5000")
db.execute("PRAGMA mmap_size = 1073741824")  # 1 GiB mmap
db.execute("PRAGMA cache_size = -65536")     # 64 MiB
db.execute("PRAGMA temp_store = MEMORY")

# Pre-compiled high performance queries with SQLite-native JSON formatting
JSON_FEED_SQL = """
SELECT json_object('posts', json_group_array(
  json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', p.username, 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))
)) FROM (
  SELECT p.id, p.body, p.created_at, u.username
  FROM posts p JOIN users u ON u.id = p.user_id
  ORDER BY p.created_at DESC, p.id DESC LIMIT 20
) p
"""

JSON_POST_SQL = """
SELECT json_object('post', json_object(
  'id', p.id,
  'body', p.body,
  'created_at', p.created_at,
  'author', u.username,
  'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
))
FROM posts p JOIN users u ON u.id = p.user_id
WHERE p.id = :post_id
"""

INSERT_POST_SQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
INSERT_LIKE_SQL = """
INSERT INTO likes (user_id, post_id)
SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
ON CONFLICT (user_id, post_id) DO NOTHING
"""
POST_EXISTS_SQL = "SELECT 1 FROM posts WHERE id = ?"

# --- responses & constants -------------------------------------------------------------------

JSON_HEADER = {"Content-Type": "application/json"}

# Static pre-allocated Responses for 0-allocation error handling
ERR_404_NOT_FOUND = Response(b'{"error":"not found"}', status_code=404, headers=JSON_HEADER)
ERR_404_POST_NOT_FOUND = Response(b'{"error":"post not found"}', status_code=404, headers=JSON_HEADER)
ERR_400_INVALID_ID = Response(b'{"error":"invalid post id"}', status_code=400, headers=JSON_HEADER)
ERR_401_MISSING_TOKEN = Response(b'{"error":"missing bearer token"}', status_code=401, headers=JSON_HEADER)
ERR_401_INVALID_TOKEN = Response(b'{"error":"invalid or expired token"}', status_code=401, headers=JSON_HEADER)
ERR_401_INVALID_PAYLOAD = Response(b'{"error":"invalid token payload"}', status_code=401, headers=JSON_HEADER)
ERR_400_MALFORMED_JSON = Response(b'{"error":"malformed JSON body"}', status_code=400, headers=JSON_HEADER)
ERR_400_BODY_REQUIRED = Response(b'{"error":"body is required"}', status_code=400, headers=JSON_HEADER)
ERR_400_BODY_TOO_LONG = Response(b'{"error":"body must be at most 500 characters"}', status_code=400, headers=JSON_HEADER)

def parse_id(raw: str):
    """A positive integer in plain decimal digits, else None."""
    if not (isinstance(raw, str) and raw.isascii() and raw.isdigit()):
        return None
    n = int(raw)
    return n if n > 0 else None

# --- auth -------------------------------------------------------------------------------------

B64URL = re.compile(r"^[A-Za-z0-9_-]*$")

def b64url_decode(s: str) -> bytes:
    if not B64URL.fullmatch(s):
        raise ValueError("not base64url")
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))

def authenticate(request: Request):
    """Returns (user_id, username), or an error Response."""
    header = request.headers.get("authorization") or request.headers.get("Authorization")
    if header is None or not header.startswith("Bearer "):
        return ERR_401_MISSING_TOKEN
    parts = header[7:].split(".")
    if len(parts) != 3:
        return ERR_401_INVALID_TOKEN
    head_b64, payload_b64, sig_b64 = parts
    try:
        head = orjson.loads(b64url_decode(head_b64))
        if not isinstance(head, dict) or head.get("alg") != "HS256":
            raise ValueError("alg")
        digest = hmac.new(JWT_SECRET, f"{head_b64}.{payload_b64}".encode("utf-8"), hashlib.sha256).digest()
        expected = base64.urlsafe_b64encode(digest).rstrip(b"=")
        if not hmac.compare_digest(expected, sig_b64.encode("utf-8")):
            raise ValueError("signature")
        payload = orjson.loads(b64url_decode(payload_b64))
        if not isinstance(payload, dict):
            raise ValueError("payload")
        now = int(time.time())
        exp = payload.get("exp")
        if exp is not None and (not isinstance(exp, (int, float)) or isinstance(exp, bool) or now >= exp):
            raise ValueError("exp")
        nbf = payload.get("nbf")
        if nbf is not None and (not isinstance(nbf, (int, float)) or isinstance(nbf, bool) or now < nbf):
            raise ValueError("nbf")
    except Exception:
        return ERR_401_INVALID_TOKEN
    
    sub = payload.get("sub")
    username = payload.get("username")
    user_id = parse_id(sub) if isinstance(sub, str) else None
    if user_id is None or user_id > MAX_ID or not isinstance(username, str):
        return ERR_401_INVALID_PAYLOAD
    return user_id, username

# JavaScript's String.prototype.trim() set (WhiteSpace + LineTerminator)
WHITESPACE = "\t\n\v\f\r                 　﻿"

# --- app --------------------------------------------------------------------------------------

app = FastAPI(openapi_url=None, docs_url=None, redoc_url=None)

# Native Rust/SQL route tier: the two read-heavy endpoints never enter CPython.
app.connect_db(f"sqlite:///{os.path.abspath(SQLITE_PATH)}")

@app.get("/health")
async def health():
    try:
        db.execute("SELECT 1").fetchone()
    except Exception as exc:
        return Response(
            orjson.dumps({"status": "degraded", "db": "unreachable", "error": str(exc)}),
            status_code=503,
            headers=JSON_HEADER,
        )
    uptime = int(time.monotonic() - START)
    return Response(
        f'{{"status":"ok","db":"ok","uptime_s":{uptime}}}'.encode("utf-8"),
        status_code=200,
        headers=JSON_HEADER,
    )

@app.get("/feed", sql=JSON_FEED_SQL, raw=True)
async def feed():
    """Served by the Rust native SQL tier; SQLite builds the JSON document."""

@app.get(
    "/posts/{post_id}",
    sql=JSON_POST_SQL,
    raw=True,
    param_types={"post_id": "posint"},
    missing_status=404,
    missing_body='{"error":"post not found"}',
    invalid_status=400,
    invalid_body='{"error":"invalid post id"}',
)
async def get_post(post_id: str):
    """Served by the Rust native SQL tier with bound, validated parameters."""

@app.post("/posts")
async def create_post(request: Request):
    user = authenticate(request)
    if isinstance(user, Response):
        return user
    user_id, username = user
    try:
        raw_body = request.body
        if callable(raw_body):
            raw_body = raw_body()
        data = orjson.loads(raw_body)
    except Exception:
        return ERR_400_MALFORMED_JSON
    body = data.get("body") if isinstance(data, dict) else None
    if not isinstance(body, str):
        return ERR_400_BODY_REQUIRED
    body = body.strip(WHITESPACE)
    if not body:
        return ERR_400_BODY_REQUIRED
    if len(body) > 500:
        return ERR_400_BODY_TOO_LONG
    
    rows = db.execute(INSERT_POST_SQL, (user_id, body)).fetchall()
    post_id, created_at = rows[0]
    res_bytes = orjson.dumps({
        "post": {
            "id": post_id,
            "body": body,
            "created_at": created_at,
            "author": username,
            "like_count": 0,
        }
    })
    return Response(res_bytes, status_code=201, headers=JSON_HEADER)

@app.post("/posts/{post_id}/like")
async def like_post(post_id: str, request: Request):
    user = authenticate(request)  # auth before id per SPEC
    if isinstance(user, Response):
        return user
    pid = parse_id(post_id)
    if pid is None:
        return ERR_400_INVALID_ID
    if pid <= MAX_ID:
        cursor = db.execute(INSERT_LIKE_SQL, (user[0], pid))
        if cursor.rowcount == 1:
            return Response(
                f'{{"liked":true,"already_liked":false,"post_id":{pid}}}'.encode("utf-8"),
                status_code=201,
                headers=JSON_HEADER,
            )
        if db.execute(POST_EXISTS_SQL, (pid,)).fetchone() is not None:
            return Response(
                f'{{"liked":true,"already_liked":true,"post_id":{pid}}}'.encode("utf-8"),
                status_code=200,
                headers=JSON_HEADER,
            )
    return ERR_404_POST_NOT_FOUND

@app.get("/{full_path}")
async def catch_all_get(full_path: str):
    return ERR_404_NOT_FOUND

@app.post("/{full_path}")
async def catch_all_post(full_path: str):
    return ERR_404_NOT_FOUND

# Freeze startup memory to minimize cyclic GC overhead during high concurrency
gc.collect()
gc.freeze()
gc.set_threshold(50_000, 20, 20)
