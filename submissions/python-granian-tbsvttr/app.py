"""A compact Granian/RSGI API with inline SQLite and database-generated JSON."""
import base64
import gc
import hmac
import os
import re
import sqlite3
import time

import orjson
from pressure import connection_pressure

SECRET = os.environ["JWT_SECRET"].encode()
START = time.monotonic()
MAX_ID = 2**63 - 1
B64URL = re.compile(rb"[A-Za-z0-9_-]+")
POST_PATH = re.compile(r"/posts/([^/]+)(/like)?")
HEADERS = [("content-type", "application/json")]
WHITESPACE = "\u0009\u000a\u000b\u000c\u000d\u0020\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005"
WHITESPACE += "\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff"
db = sqlite3.connect(os.environ["SQLITE_PATH"], isolation_level=None, cached_statements=16)
db.executescript("""PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;
                   PRAGMA locking_mode=EXCLUSIVE; PRAGMA cache_size=500; PRAGMA mmap_size=536870912;""")
cursor = db.cursor()
POST_JSON = """json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,
                          'like_count',(SELECT count(*) FROM likes WHERE post_id=p.id))"""
FROM_POSTS = "FROM posts p JOIN users u ON u.id=p.user_id"
FEED_SQL = f"""SELECT CAST('{{"posts":[' ||
               coalesce(group_concat(item ORDER BY created_at DESC,id DESC), '') || ']}}' AS BLOB)
               FROM (SELECT p.id,p.created_at,{POST_JSON} AS item {FROM_POSTS}
                     ORDER BY p.created_at DESC,p.id DESC LIMIT 20)"""
POST_SQL = f"SELECT CAST(json_object('post',{POST_JSON}) AS BLOB) {FROM_POSTS} WHERE p.id=?"
CREATE_SQL = """INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING CAST(json_object('post',
                json_object('id',id,'body',body,'created_at',created_at,'author',?,'like_count',0)) AS BLOB)"""
LIKE_SQL = """INSERT INTO likes(user_id,post_id) SELECT ?,id FROM posts WHERE id=?
              ON CONFLICT(user_id,post_id) DO NOTHING RETURNING post_id"""


class APIError(Exception):
    pass


def query(sql, *parameters):
    # Exhaust RETURNING before replying, committing every autocommit write.
    return cursor.execute(sql, parameters).fetchall()


def positive_id(value):
    if isinstance(value, str) and value.isascii() and value.isdecimal():
        value = value.lstrip("0") or "0"
        return (int(value) or None) if len(value) <= 19 else MAX_ID + 1
    return None


def decode(segment):
    if not B64URL.fullmatch(segment):
        raise ValueError("invalid base64url")
    return base64.urlsafe_b64decode(segment + b"=" * (-len(segment) % 4))


def authenticate(scope):
    header = scope.headers.get("authorization", "")
    if not header.startswith("Bearer "):
        raise APIError(401, "missing bearer token")
    try:
        head64, payload64, signature = header[7:].encode("ascii").split(b".")
        head = orjson.loads(decode(head64))
        if not isinstance(head, dict) or head.get("alg") != "HS256":
            raise ValueError("algorithm")
        digest = hmac.digest(SECRET, head64 + b"." + payload64, "sha256")
        if not hmac.compare_digest(base64.urlsafe_b64encode(digest).rstrip(b"="), signature):
            raise ValueError("signature")
        payload = orjson.loads(decode(payload64))
        if not isinstance(payload, dict):
            raise ValueError("payload")
        now, exp, nbf = time.time(), payload.get("exp"), payload.get("nbf", 0)
        if type(exp) not in (int, float) or now >= exp or type(nbf) not in (int, float) or now < nbf:
            raise ValueError("dates")
    except (ValueError, UnicodeError):
        raise APIError(401, "invalid or expired token") from None
    user_id, username = positive_id(payload.get("sub")), payload.get("username")
    if user_id is None or user_id > MAX_ID or not isinstance(username, str):
        raise APIError(401, "invalid token payload")
    return user_id, username


async def handle(scope, protocol):
    # Granian already percent-decodes scope.path and excludes the query string.
    method, path = scope.method, scope.path
    if method == "GET" and path == "/health":
        try:
            query("SELECT 1")
        except sqlite3.Error as error:
            return 503, {"status": "degraded", "db": "unreachable", "error": str(error)}
        return 200, {"status": "ok", "db": "ok", "uptime_s": int(time.monotonic() - START)}
    if method == "GET" and path == "/feed":
        return 200, query(FEED_SQL)[0][0]
    if method == "POST" and path == "/posts":
        user_id, username = authenticate(scope)
        try:
            data = orjson.loads(await protocol())
        except orjson.JSONDecodeError:
            raise APIError(400, "malformed JSON body") from None
        body = data.get("body") if isinstance(data, dict) else None
        if not isinstance(body, str) or not (body := body.strip(WHITESPACE)):
            raise APIError(400, "body is required")
        if len(body) > 500:
            raise APIError(400, "body must be at most 500 characters")
        return 201, query(CREATE_SQL, user_id, body, username)[0][0]
    match = POST_PATH.fullmatch(path)
    if match and ((method == "GET" and not match[2]) or (method == "POST" and match[2])):
        if method == "POST":
            user_id, _ = authenticate(scope)  # Authentication precedes id validation.
        post_id = positive_id(match[1])
        if post_id is None:
            raise APIError(400, "invalid post id")
        if post_id > MAX_ID:
            raise APIError(404, "post not found")
        if method == "GET":
            rows = query(POST_SQL, post_id)
            if not rows:
                raise APIError(404, "post not found")
            return 200, rows[0][0]
        added = query(LIKE_SQL, user_id, post_id)
        if not added and not query("SELECT 1 FROM posts WHERE id=?", post_id):
            raise APIError(404, "post not found")
        return (201 if added else 200), {"liked": True, "already_liked": not added, "post_id": post_id}
    raise APIError(404, "not found")


async def app(scope, protocol):
    try:
        status, body = await handle(scope, protocol)
        body = body if isinstance(body, bytes) else orjson.dumps(body)
    except APIError as error:
        status, message = error.args
        body = orjson.dumps({"error": message})
    except Exception:
        status, body = 500, orjson.dumps({"error": "internal server error"})
    headers = HEADERS + [("connection", "close")] if connection_pressure() else HEADERS
    protocol.response_bytes(status, headers, body)


gc.collect()
gc.freeze()
gc.set_threshold(50_000, 20, 20)
