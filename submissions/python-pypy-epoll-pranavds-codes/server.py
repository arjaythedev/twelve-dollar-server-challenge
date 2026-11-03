"""The $12 server challenge API on PyPy: one thread, a raw epoll loop, SQLite inline, SQLite-built JSON.

No framework and no third-party packages. PyPy's JIT compiles the HTTP parsing and request handling,
leaving SQLite and the kernel's TCP stack as most of the CPU. Also runs on CPython (with orjson), slower.
"""
import binascii
import errno
import gc
import hmac
import itertools
import os
import resource
import select
import socket
import sqlite3
import time

try:
    import orjson
    loads, dumps, JSONError = orjson.loads, orjson.dumps, orjson.JSONDecodeError
except ImportError:  # PyPy
    import json
    JSONError = ValueError
    _json_loads = json.loads

    def _reject(_):
        raise ValueError("non-finite number")

    def _finite(text):
        number = float(text)
        if number - number != 0:
            raise ValueError("non-finite number")
        return number

    _strict = json.JSONDecoder(parse_constant=_reject, parse_float=_finite).decode

    def loads(raw):
        """json.loads with orjson's strictness: UTF-8 only (json.loads(bytes) would accept encoded
        surrogates), no lone-surrogate escapes, no NaN/Infinity/overflowing numbers."""
        text = raw.decode("utf-8")
        value = _json_loads(text)
        if type(value) is dict:
            for item in value.values():
                kind = type(item)
                if kind is str:
                    if not item.isascii():
                        item.encode("utf-8")
                elif kind is float:
                    if item - item != 0:
                        raise ValueError("non-finite number")
                elif kind is dict or kind is list:
                    return _strict(text)  # rare: validate nested values the slow way
        elif type(value) is list:
            return _strict(text)
        return value

    def dumps(obj):
        return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode()

SECRET = os.environ["JWT_SECRET"].encode()
START = time.monotonic()
MAX_ID = 2**63 - 1
WHITESPACE = "\t\n\v\f\r                  　﻿"
HS256_HEADERS = {b"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"}  # base64url of {"alg":"HS256","typ":"JWT"}

# --- database -------------------------------------------------------------------------------
db = sqlite3.connect(os.environ["SQLITE_PATH"], isolation_level=None, check_same_thread=False, cached_statements=16)
for pragma in ("journal_mode=WAL", "synchronous=NORMAL", "foreign_keys=ON", "locking_mode=EXCLUSIVE", "mmap_size=1073741824",
               "cache_size=-32768", "temp_store=MEMORY", "busy_timeout=5000"):
    db.execute("PRAGMA " + pragma)
cur = db.cursor()
execute = cur.execute
POST_JSON = ("json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,"
             "'like_count',(SELECT count(*) FROM likes WHERE post_id=p.id))")
FEED_SQL = ("SELECT CAST('{\"posts\":[' || coalesce(group_concat(" + POST_JSON + ", ','), '') || ']}' AS BLOB) "
            "FROM (SELECT id, user_id, body, created_at FROM posts ORDER BY created_at DESC, id DESC LIMIT 20) p "
            "JOIN users u ON u.id = p.user_id")
POST_SQL = "SELECT CAST(json_object('post'," + POST_JSON + ") AS BLOB) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?"
CREATE_SQL = ("INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING CAST(json_object('post',json_object("
              "'id',id,'body',body,'created_at',created_at,'author',?,'like_count',0)) AS BLOB)")
LIKE_SQL = ("INSERT INTO likes(user_id,post_id) SELECT ?,id FROM posts WHERE id=? "
            "ON CONFLICT(user_id,post_id) DO NOTHING RETURNING post_id")
EXISTS_SQL = "SELECT 1 FROM posts WHERE id=?"

# --- responses ------------------------------------------------------------------------------
REASONS = {200: b"OK", 201: b"Created", 400: b"Bad Request", 401: b"Unauthorized", 404: b"Not Found",
           413: b"Payload Too Large", 431: b"Request Header Fields Too Large", 500: b"Internal Server Error",
           503: b"Service Unavailable"}
PREFIX = {}        # (status, close) -> b"HTTP/1.1 ... Content-Length: "
date_second = 0


def refresh_prefixes(now):
    global date_second
    date_second = int(now)
    date = time.strftime("%a, %d %b %Y %H:%M:%S GMT", time.gmtime(now)).encode()
    for status, reason in REASONS.items():
        head = b"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nDate: %s\r\n" % (status, reason, date)
        PREFIX[status, False] = head + b"Content-Length: "
        PREFIX[status, True] = head + b"Connection: close\r\nContent-Length: "


refresh_prefixes(time.time())


def err(status, message):
    return status, b'{"error":"' + message + b'"}'


E_NOT_FOUND = err(404, b"not found")
E_BAD_ID = err(400, b"invalid post id")
E_NO_POST = err(404, b"post not found")
E_NO_TOKEN = err(401, b"missing bearer token")
E_BAD_TOKEN = err(401, b"invalid or expired token")
E_BAD_PAYLOAD = err(401, b"invalid token payload")
E_BAD_JSON = err(400, b"malformed JSON body")
E_NO_BODY = err(400, b"body is required")
E_LONG_BODY = err(400, b"body must be at most 500 characters")
E_INTERNAL = err(500, b"internal server error")

# --- auth -----------------------------------------------------------------------------------
B64_OK = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
TO_STD = bytes.maketrans(b"-_", b"+/")
a2b = binascii.a2b_base64
b2a = binascii.b2a_base64
compare = hmac.compare_digest

try:  # PyPy: one-shot OpenSSL HMAC() through cffi, into a reused buffer. PyPy's hmac module costs
    # ~11 us per call and its OpenSSL contexts are freed only at the next major GC (~2 KB each).
    import cffi
    _hffi = cffi.FFI()
    _hffi.cdef("""typedef struct evp_md_st EVP_MD; const EVP_MD *EVP_sha256(void);
                  unsigned char *HMAC(const EVP_MD *evp_md, const void *key, int key_len,
                                      const unsigned char *d, size_t n, unsigned char *md, unsigned int *md_len);""")
    _crypto = _hffi.dlopen("libcrypto.so.3")
    _SHA256 = _crypto.EVP_sha256()
    _mac_out = _hffi.new("unsigned char[32]")
    _mac_len = _hffi.new("unsigned int *")
    _HMAC = _crypto.HMAC
    _buffer = _hffi.buffer

    def hmac_sha256(msg):
        _HMAC(_SHA256, SECRET, len(SECRET), msg, len(msg), _mac_out, _mac_len)
        return _buffer(_mac_out, 32)[:]
except (ImportError, OSError):
    def hmac_sha256(msg):
        return hmac.digest(SECRET, msg, "sha256")


def b64d(seg):
    if not seg or not B64_OK.issuperset(seg):
        raise ValueError
    return a2b(seg.translate(TO_STD) + b"=" * (-len(seg) % 4))


def authenticate(value):
    """value: raw Authorization header bytes (or None). Returns (user_id, username) or an error tuple."""
    if value is None or not value.startswith(b"Bearer "):
        return None, E_NO_TOKEN
    token = value[7:]
    try:
        head64, payload64, sig64 = token.split(b".")
        if head64 not in HS256_HEADERS:
            head = loads(b64d(head64))
            if type(head) is not dict or head.get("alg") != "HS256":
                return None, E_BAD_TOKEN
        mac = b2a(hmac_sha256(token[:len(head64) + 1 + len(payload64)]), newline=False)
        if not compare(mac[:-1].translate(FROM_STD), sig64):
            return None, E_BAD_TOKEN
        payload = loads(b64d(payload64))
        if type(payload) is not dict:
            return None, E_BAD_TOKEN
        now = time.time()
        # Like PyJWT/jsonwebtoken: exp and nbf are optional, but when present they must be numbers.
        exp = payload.get("exp", MISSING)
        if exp is not MISSING and (type(exp) not in (int, float) or now >= exp):
            return None, E_BAD_TOKEN
        nbf = payload.get("nbf", MISSING)
        if nbf is not MISSING and (type(nbf) not in (int, float) or now < nbf):
            return None, E_BAD_TOKEN
    except (ValueError, UnicodeError, binascii.Error):
        return None, E_BAD_TOKEN
    sub = payload.get("sub")
    username = payload.get("username")
    if type(sub) is not str or not (sub.isascii() and sub.isdigit()) or type(username) is not str:
        return None, E_BAD_PAYLOAD
    uid = int(sub)
    if uid <= 0 or uid > MAX_ID:
        return None, E_BAD_PAYLOAD
    return uid, username


FROM_STD = bytes.maketrans(b"+/", b"-_")
MISSING = object()


def parse_id(raw):
    """b'123' -> 123; None if not a positive decimal integer; MAX_ID+1 if too large to exist."""
    if not raw.isdigit():  # bytes.isdigit is ASCII-only
        return None
    n = int(raw)
    if n <= 0:
        return None
    return n if n <= MAX_ID else MAX_ID + 1


# --- handlers -------------------------------------------------------------------------------
def health():
    try:
        execute("SELECT 1").fetchall()
    except Exception as exc:
        return 503, dumps({"status": "degraded", "db": "unreachable", "error": str(exc)})
    return 200, b'{"status":"ok","db":"ok","uptime_s":%d}' % int(time.monotonic() - START)


def create_post(auth, body):
    uid, username = authenticate(auth)
    if uid is None:
        return username
    try:
        data = loads(body)
    except JSONError:
        return E_BAD_JSON
    text = data.get("body") if type(data) is dict else None
    if type(text) is not str:
        return E_NO_BODY
    text = text.strip(WHITESPACE)
    if not text:
        return E_NO_BODY
    if len(text) > 500:
        return E_LONG_BODY
    return 201, execute(CREATE_SQL, (uid, text, username)).fetchall()[0][0]


def like(auth, raw_id):
    uid, error = authenticate(auth)
    if uid is None:
        return error
    pid = parse_id(raw_id)
    if pid is None:
        return E_BAD_ID
    if pid > MAX_ID:
        return E_NO_POST
    if execute(LIKE_SQL, (uid, pid)).fetchall():
        return 201, b'{"liked":true,"already_liked":false,"post_id":%d}' % pid
    if execute(EXISTS_SQL, (pid,)).fetchall():
        return 200, b'{"liked":true,"already_liked":true,"post_id":%d}' % pid
    return E_NO_POST


def get_post(raw_id):
    pid = parse_id(raw_id)
    if pid is None:
        return E_BAD_ID
    if pid > MAX_ID:
        return E_NO_POST
    row = execute(POST_SQL, (pid,)).fetchone()
    return (200, row[0]) if row is not None else E_NO_POST


def route(method, target, auth, body):
    q = target.find(b"?")
    if q >= 0:
        target = target[:q]
    if method == b"GET":
        if target == b"/feed":
            return 200, execute(FEED_SQL).fetchone()[0]
        if target[:7] == b"/posts/":
            rest = target[7:]
            if rest and b"/" not in rest:
                return get_post(rest)
            return E_NOT_FOUND
        if target == b"/health":
            return health()
        return E_NOT_FOUND
    if method == b"POST":
        if target == b"/posts":
            return create_post(auth, body)
        if target[:7] == b"/posts/" and target[-5:] == b"/like":
            rest = target[7:-5]
            if rest and b"/" not in rest:
                return like(auth, rest)
        return E_NOT_FOUND
    return E_NOT_FOUND


# --- server ---------------------------------------------------------------------------------
HOST = os.environ.get("HOST", "127.0.0.1")
PORT = int(os.environ.get("PORT", "3000"))
NOFILE = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
# Keep-alive budget. Above this many open client sockets, responses carry "Connection: close" and the
# socket is closed after the reply, so the number of users is not capped by the descriptor limit
# (LimitNOFILE=65535 direct). An idle connection costs ~4 KB of kernel memory and almost nothing here,
# so the cap is about the other end: 25,000 stays under Linux's default ephemeral port range (28,232)
# on a single-address load generator, and the server closes first, so the client keeps no TIME_WAIT.
BUDGET = max(64, min(NOFILE - 256, 25_000))
MAX_HEAD = 16384
MAX_BODY = 1 << 20

lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
lsock.bind((HOST, PORT))
# Wake accept() only once the request bytes have arrived, so a new connection is read and answered
# straight away (and, when it is being retired, closed without ever touching epoll).
lsock.setsockopt(socket.IPPROTO_TCP, socket.TCP_DEFER_ACCEPT, 2)
lsock.listen(4096)
lsock.setblocking(False)
LFD = lsock.fileno()

try:  # PyPy ships cffi; accept4() hands back an already non-blocking descriptor in one syscall.
    import cffi
    _ffi = cffi.FFI()
    _ffi.cdef("int accept4(int sockfd, void *addr, void *addrlen, int flags);")
    _libc = _ffi.dlopen(None)
    _NULL = _ffi.NULL
    _ACCEPT_FLAGS = socket.SOCK_NONBLOCK | socket.SOCK_CLOEXEC

    def accept_fd():
        """A new non-blocking descriptor; -1 when none is waiting; -2 when out of descriptors."""
        fd = _libc.accept4(LFD, _NULL, _NULL, _ACCEPT_FLAGS)
        if fd >= 0:
            return fd
        return -2 if _ffi.errno in (errno.EMFILE, errno.ENFILE) else -1
except ImportError:  # CPython fallback
    def accept_fd():
        try:
            fd, _ = lsock._accept()
        except OSError as error:
            return -2 if error.errno in (errno.EMFILE, errno.ENFILE) else -1
        os.set_blocking(fd, False)
        return fd

ep = select.epoll(1024)
EPOLLIN = select.EPOLLIN
EPOLLOUT = select.EPOLLOUT
ep.register(LFD, EPOLLIN)

pending_in = {}    # fd -> unprocessed bytes (partial request)
pending_out = {}   # fd -> [unsent bytes, close_after]
open_fds = set()
seen = set()       # fds active in the current idle period
continued = set()  # fds that were sent "100 Continue" for the request in progress
read = os.read
write = os.write
close = os.close


def drop(fd):
    open_fds.discard(fd)
    continued.discard(fd)
    pending_in.pop(fd, None)
    pending_out.pop(fd, None)
    try:
        close(fd)
    except OSError:
        pass


def fixed(status, message):
    body = b'{"error":"' + message + b'"}'
    return PREFIX[status, True] + b"%d\r\n\r\n" % len(body) + body


BAD_REQUEST = fixed(400, b"bad request")          # Date frozen at startup; only sent before closing
HEADER_TOO_LARGE = fixed(431, b"request header fields too large")


def on_data(fd, data):
    """Parse and answer every complete request in data. Returns True if the connection stays open."""
    pos = 0
    n = len(data)
    out = []
    keep = True
    while pos < n:
        he = data.find(b"\r\n\r\n", pos)
        if he < 0:
            if n - pos > MAX_HEAD:
                out.append(HEADER_TOO_LARGE)
                keep = False
            break
        le = data.find(b"\r\n", pos)
        line = data[pos:le]
        sp1 = line.find(b" ")
        sp2 = line.rfind(b" ")
        if sp1 <= 0 or sp2 <= sp1:
            out.append(BAD_REQUEST)
            keep = False
            break
        method = line[:sp1]
        target = line[sp1 + 1:sp2]
        hdr = data[le:he + 2].lower()  # b"\r\nname: value\r\n...\r\n"
        cl = 0
        auth = None
        chunked = False
        if len(hdr) > 2:
            i = hdr.find(b"\r\ncontent-length:")
            t = hdr.find(b"\r\ntransfer-encoding:")
            if i >= 0:
                try:
                    cl = int(hdr[i + 17:hdr.find(b"\r\n", i + 17)])
                except ValueError:
                    cl = -1
                if cl < 0 or cl > MAX_BODY or t >= 0:  # both framings at once: refuse (RFC 9112 6.3)
                    out.append(BAD_REQUEST)
                    keep = False
                    break
            elif t >= 0:
                if not hdr[t + 20:hdr.find(b"\r\n", t + 20)].rstrip().endswith(b"chunked"):
                    out.append(BAD_REQUEST)
                    keep = False
                    break
                chunked = True
            i = hdr.find(b"\r\nauthorization:")
            if i >= 0:
                auth = data[le + i + 16:le + hdr.find(b"\r\n", i + 16)].strip()
            i = hdr.find(b"\r\nconnection:")
            tokens = hdr[i + 13:hdr.find(b"\r\n", i + 13)].replace(b" ", b"").replace(b"\t", b"").split(b",") if i >= 0 else ()
            if line[sp2 + 1:] == b"HTTP/1.1":
                if b"close" in tokens:
                    keep = False
            elif b"keep-alive" not in tokens:
                keep = False
        elif line[sp2 + 1:] != b"HTTP/1.1":
            keep = False
        if chunked:
            done = dechunk(data, he + 4)
            if done is None:
                break  # incomplete
            if done is False:
                out.append(BAD_REQUEST)
                keep = False
                break
            body, end = done
        else:
            end = he + 4 + cl
            if end > n:
                if b"\r\nexpect: 100-continue" in hdr and fd not in continued and not out:
                    continued.add(fd)
                    try:
                        write(fd, b"HTTP/1.1 100 Continue\r\n\r\n")
                    except OSError:
                        pass
                break
            body = data[he + 4:end] if cl else b""
        continued.discard(fd)
        try:
            status, payload = route(method, target, auth, body)
        except Exception:
            status, payload = E_INTERNAL
        if keep and len(open_fds) > BUDGET:
            keep = False
        out.append(PREFIX[status, not keep] + b"%d\r\n\r\n" % len(payload) + payload)
        pos = end
        if not keep:
            break
    if keep and pos < n:
        pending_in[fd] = data[pos:]
    if out:
        resp = out[0] if len(out) == 1 else b"".join(out)
        try:
            sent = write(fd, resp)
        except BlockingIOError:
            sent = 0
        except OSError:
            return False
        if sent < len(resp):
            pending_out[fd] = [resp[sent:], not keep]
            try:
                ep.modify(fd, EPOLLIN | EPOLLOUT)
            except OSError:
                pass  # not registered yet: on_accept registers it with EPOLLOUT
            return True
    return keep


def dechunk(data, start):
    """Decode a chunked body starting at start. None: incomplete; False: malformed; else (body, end)."""
    parts = []
    pos = start
    total = 0
    while True:
        le = data.find(b"\r\n", pos)
        if le < 0:
            return None
        try:
            size = int(data[pos:le].split(b";", 1)[0], 16)
        except ValueError:
            return False
        if size == 0:
            te = data.find(b"\r\n\r\n", le)
            if te < 0:
                return None
            return b"".join(parts), te + 4
        total += size
        if total > MAX_BODY:
            return False
        if le + 2 + size + 2 > len(data):
            return None
        parts.append(data[le + 2:le + 2 + size])
        pos = le + 2 + size + 2


def flush(fd):
    entry = pending_out.get(fd)
    if entry is None:
        ep.modify(fd, EPOLLIN)
        return
    buf, close_after = entry
    try:
        sent = write(fd, buf)
    except BlockingIOError:
        return
    except OSError:
        drop(fd)
        return
    if sent < len(buf):
        entry[0] = buf[sent:]
        return
    del pending_out[fd]
    if close_after:
        drop(fd)
    else:
        ep.modify(fd, EPOLLIN)
        if fd in pending_in:
            data = pending_in.pop(fd)
            if not on_data(fd, data):
                if fd not in pending_out:
                    drop(fd)


def on_readable(fd):
    try:
        data = read(fd, 65536)
    except BlockingIOError:
        return
    except OSError:
        drop(fd)
        return
    if not data:
        drop(fd)
        return
    if fd in pending_out:
        # Do not process further requests until the previous response is out.
        buf = pending_in.get(fd, b"") + data
        if len(buf) > MAX_HEAD + MAX_BODY:  # a client that sends but never reads
            drop(fd)
            return
        pending_in[fd] = buf
        return
    prev = pending_in.pop(fd, None)
    if prev is not None:
        data = prev + data
    seen.add(fd)
    if not on_data(fd, data) and fd not in pending_out:
        drop(fd)


def shed(count):
    """Out of descriptors: close some idle keep-alive connections (clients just reconnect)."""
    idle = [fd for fd in itertools.islice(open_fds - seen, count) if fd not in pending_out]
    for fd in idle or [fd for fd in itertools.islice(open_fds, count) if fd not in pending_out]:
        drop(fd)


def on_accept():
    for _ in range(256):
        fd = accept_fd()
        if fd == -1:
            return
        if fd == -2:
            shed(256)
            continue
        open_fds.add(fd)
        try:
            data = read(fd, 65536)
        except BlockingIOError:
            data = None
        except OSError:
            drop(fd)
            continue
        if data is None:
            register(fd, EPOLLIN)
            continue
        if not data:
            drop(fd)
            continue
        seen.add(fd)
        if on_data(fd, data) or fd in pending_out:
            register(fd, EPOLLIN | EPOLLOUT if fd in pending_out else EPOLLIN)
        else:
            drop(fd)


register = ep.register
IDLE_PERIOD = 120.0  # a connection with no request for one to two periods is closed


def serve():
    global seen
    poll = ep.poll
    next_sweep = time.monotonic() + IDLE_PERIOD
    while True:
        for fd, ev in poll(1.0):
            if fd == LFD:
                on_accept()
            elif ev & EPOLLOUT:
                flush(fd)
                if ev & EPOLLIN and fd in open_fds:
                    on_readable(fd)
            else:
                on_readable(fd)
        now = time.time()
        if int(now) != date_second:
            refresh_prefixes(now)
            mono = time.monotonic()
            if mono >= next_sweep:
                for fd in open_fds - seen:
                    if fd not in pending_out:
                        drop(fd)
                seen = set()
                next_sweep = mono + IDLE_PERIOD


gc.collect()
if hasattr(gc, "freeze"):
    gc.freeze()
    gc.set_threshold(100_000, 50, 50)

if __name__ == "__main__":
    serve()
