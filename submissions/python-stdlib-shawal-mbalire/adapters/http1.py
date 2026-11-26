"""HTTP/1.1 framing, without I/O: requests parsed out of a byte buffer, responses framed into bytes.

The parser does what the five endpoints need and what keep-alive clients send: a request line,
headers (only Content-Length, Transfer-Encoding, Authorization, Connection and Expect are read),
and a body framed by Content-Length or chunked. It works on bytes with find/split, like PR #84's.
Header names are matched case-insensitively by lowercasing the header block once; values are read
from the original bytes, so a token keeps its case.
"""

import time
from dataclasses import dataclass

MAX_HEAD = 16_384  # request line + headers
MAX_BODY = 1 << 20  # SPEC leaves bodies over 16 KB out of scope; 1 MiB stops a runaway client

REASONS = {
    100: b"Continue", 200: b"OK", 201: b"Created", 400: b"Bad Request", 401: b"Unauthorized",
    404: b"Not Found", 405: b"Method Not Allowed", 413: b"Content Too Large",
    431: b"Request Header Fields Too Large", 500: b"Internal Server Error", 503: b"Service Unavailable",
}
CONTINUE = b"HTTP/1.1 100 Continue\r\n\r\n"
HEX_DIGITS = b"0123456789abcdefABCDEF"


@dataclass(slots=True, frozen=True)
class Request:
    method: bytes
    path: bytes  # the target without its query string
    authorization: bytes | None
    body: bytes
    keep_alive: bool
    end: int  # where the next request starts in the buffer


class Incomplete:
    """More bytes are needed. `expects_continue`: the client waits for 100 Continue before its body."""

    __slots__ = ("expects_continue",)

    def __init__(self, expects_continue: bool):
        self.expects_continue = expects_continue


NEED_MORE = Incomplete(expects_continue=False)
NEED_BODY_AFTER_CONTINUE = Incomplete(expects_continue=True)


class BadRequest(Exception):
    """The connection can't continue: answer `status` with `message`, then close."""

    def __init__(self, status: int, message: bytes):
        super().__init__(status, message)
        self.status = status
        self.message = message


def parse_request(buffer: bytes, start: int) -> Request | Incomplete:
    """The request that starts at `start`, or NEED_MORE / NEED_BODY_AFTER_CONTINUE. Raises BadRequest."""
    while buffer.startswith(b"\r\n", start):  # RFC 9112 2.2: ignore empty lines before a request
        start += 2
    head_end = buffer.find(b"\r\n\r\n", start)
    if head_end < 0:
        if len(buffer) - start > MAX_HEAD:
            raise BadRequest(431, b"request header fields too large")
        return NEED_MORE
    line_end = buffer.find(b"\r\n", start)
    try:
        method, target, version = buffer[start:line_end].split(b" ")
    except ValueError:
        raise BadRequest(400, b"bad request") from None
    if version == b"HTTP/1.1":
        keep_alive = True
    elif version == b"HTTP/1.0":
        keep_alive = False
    else:
        raise BadRequest(400, b"bad request")
    query = target.find(b"?")
    path = target if query < 0 else target[:query]

    length = 0
    authorization = None
    chunked = False
    if head_end > line_end:  # any headers at all
        fields = buffer[line_end:head_end + 2].lower()  # b"\r\nname: value\r\n...\r\n"
        if (at := fields.find(b"\r\ncontent-length:")) >= 0:
            length = _content_length(fields[at + 17:fields.find(b"\r\n", at + 17)])
            if b"\r\ntransfer-encoding:" in fields:  # both framings: refuse (RFC 9112 6.3)
                raise BadRequest(400, b"bad request")
        elif (at := fields.find(b"\r\ntransfer-encoding:")) >= 0:
            if not fields[at + 20:fields.find(b"\r\n", at + 20)].rstrip().endswith(b"chunked"):
                raise BadRequest(400, b"bad request")
            chunked = True
        if (at := fields.find(b"\r\nauthorization:")) >= 0:
            authorization = buffer[line_end + at + 16:line_end + fields.find(b"\r\n", at + 16)].strip()
        if (at := fields.find(b"\r\nconnection:")) >= 0:
            options = fields[at + 13:fields.find(b"\r\n", at + 13)].replace(b" ", b"").replace(b"\t", b"").split(b",")
            keep_alive = b"close" not in options and (keep_alive or b"keep-alive" in options)
        expects_continue = b"\r\nexpect: 100-continue" in fields
    else:
        expects_continue = False

    body_start = head_end + 4
    if chunked:
        match _dechunk(buffer, body_start):
            case None:
                return NEED_BODY_AFTER_CONTINUE if expects_continue else NEED_MORE
            case (body, end):
                return Request(method, path, authorization, body, keep_alive, end)
    end = body_start + length
    if end > len(buffer):
        return NEED_BODY_AFTER_CONTINUE if expects_continue else NEED_MORE
    body = buffer[body_start:end] if length else b""
    return Request(method, path, authorization, body, keep_alive, end)


def _content_length(value: bytes) -> int:
    value = value.strip()
    if not value.isdigit():  # bytes.isdigit is ASCII-only; no sign, no spaces inside
        raise BadRequest(400, b"bad request")
    length = int(value)
    if length > MAX_BODY:
        raise BadRequest(413, b"request body too large")
    return length


def _dechunk(buffer: bytes, start: int) -> tuple[bytes, int] | None:
    """A chunked body from `start`: (body, end), or None while incomplete. Raises BadRequest."""
    parts = []
    total = 0
    position = start
    while True:
        line_end = buffer.find(b"\r\n", position)
        if line_end < 0:
            return None
        size_text = buffer[position:line_end].split(b";", 1)[0].strip()
        if not size_text or size_text.strip(HEX_DIGITS):  # int(..., 16) would also take "0x", "+", "_"
            raise BadRequest(400, b"bad request")
        size = int(size_text, 16)
        if size == 0:  # trailers, if any, end with an empty line
            trailer_end = buffer.find(b"\r\n\r\n", line_end)
            return None if trailer_end < 0 else (b"".join(parts), trailer_end + 4)
        total += size
        if total > MAX_BODY:
            raise BadRequest(413, b"request body too large")
        chunk_end = line_end + 2 + size
        if chunk_end + 2 > len(buffer):
            return None
        if buffer[chunk_end:chunk_end + 2] != b"\r\n":
            raise BadRequest(400, b"bad request")
        parts.append(buffer[line_end + 2:chunk_end])
        position = chunk_end + 2


class ResponseHeads:
    """Every response's head up to the Content-Length value, per (status, close), with today's Date.

    `refresh` rebuilds them for the current second; the HTTP server calls it once a second, so a
    response costs one dict lookup and two concatenations.
    """

    __slots__ = ("_heads", "second")

    def __init__(self, now: float | None = None):
        self._heads: dict[tuple[int, bool], bytes] = {}
        self.second = -1
        self.refresh(time.time() if now is None else now)

    def refresh(self, now: float) -> None:
        second = int(now)
        if second == self.second:
            return
        date = time.strftime("%a, %d %b %Y %H:%M:%S GMT", time.gmtime(second)).encode()
        heads = {}
        for status, reason in REASONS.items():
            head = b"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nDate: %s\r\n" % (status, reason, date)
            heads[status, False] = head + b"Content-Length: "
            heads[status, True] = head + b"Connection: close\r\nContent-Length: "
        self._heads = heads
        self.second = second

    def frame(self, status: int, body: bytes, close: bool) -> bytes:
        return self._heads[status, close] + b"%d\r\n\r\n" % len(body) + body
