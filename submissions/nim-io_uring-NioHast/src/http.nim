## http.nim: transport-agnostic HTTP/1.1 parsing and response framing.
##
## The io_uring server loop in `server.nim` uses this module, so the request
## parser and the response bytes are pinned in one place. Nothing here touches a
## socket, an event loop or the database: every routine is pure string-in /
## string-out, and the only policy knob it needs (the inbound body cap) is
## passed in by the caller.
##
## Wire contract
## -------------
## * `addResponse` emits the exact bytes the spec requires, body verbatim:
##     HTTP/1.1 <status> <reason>\r\n
##     Content-Type: application/json\r\n
##     Content-Length: <n>\r\n
##     Connection: <keep-alive|close>\r\n
##     \r\n
##     <body>
## * `parseHead` never reads the body; it extracts the request line and the
##   `Authorization` / `Content-Length` / `Connection` headers and returns the
##   framing verdict (`ok` / `bad`). The caller decides how much body to consume.
##
## Keeping all of this in one module is what makes a byte-exact A/B possible: the
## two event loops differ only in *how* bytes move, never in *what* bytes are
## produced or accepted.

import std/strutils

const
  ## Largest header block (request line + headers) accepted before a request is
  ## treated as malformed. Shared by both transports so a hostile header block
  ## is bounded identically on either path.
  MaxHeader* = 16 * 1024

  ## Server-loop policy shared verbatim by both transports (identical today;
  ## kept here so the two copies can never silently drift).
  ListenBacklog* = 4096
  IdleKeepAlive* = 75.0
  StallTimeout* = 30.0

type
  HeadInfo* = object
    ok*:            bool
    ## Set when the request is syntactically a request but its framing is
    ## non-conforming (bad/conflicting Content-Length, Transfer-Encoding): the
    ## caller answers 400 and closes instead of re-parsing the body as a request.
    bad*:           bool
    httpMethod*:    string
    path*:          string
    auth*:          string
    keepAlive*:     bool
    contentLen*:    int
    sawContentLen*: bool

proc reasonPhrase*(status: int): string =
  ## Canonical reason phrase for the status codes the API can emit.
  case status
  of 200: "OK"
  of 201: "Created"
  of 400: "Bad Request"
  of 401: "Unauthorized"
  of 404: "Not Found"
  of 500: "Internal Server Error"
  of 503: "Service Unavailable"
  else:   "OK"

proc addResponse*(buf: var string; status: int; body: string; keepAlive: bool) =
  ## Append the exact wire bytes required by the spec to `buf`; the body is
  ## appended verbatim. Same bytes as the returned-then-copied form, without the
  ## intermediate whole-response string.
  buf.add "HTTP/1.1 "
  buf.add $status
  buf.add ' '
  buf.add reasonPhrase(status)
  buf.add "\r\nContent-Type: application/json\r\nContent-Length: "
  buf.add $body.len
  buf.add "\r\nConnection: "
  buf.add(if keepAlive: "keep-alive" else: "close")
  buf.add "\r\n\r\n"
  buf.add body

proc findHeaderEnd*(buf: string): int {.inline.} =
  ## Index of the terminating CRLFCRLF, or -1 when the header block is incomplete.
  buf.find("\r\n\r\n")

proc strictContentLen(value: string, outLen: var int, maxBody: int): bool =
  ## Strict Content-Length: one or more ASCII digits, no sign, no whitespace, no
  ## junk, and small enough to fit the caller's body cap (`maxBody`). Rejects
  ## empty, "5 " (stripped to "5" upstream), "+5", "-5", "0x10", "5,5" and
  ## anything exceeding the cap. Not exported: only `parseHead` consumes it.
  if value.len == 0: return false
  var v: int64 = 0
  for c in value:
    if c < '0' or c > '9': return false
    v = v * 10 + int64(ord(c) - ord('0'))
    if v > int64(maxBody): return false   # bounds v well before int64 overflow
  outLen = int(v)
  true

proc parseHead*(buf: string, start, hend, maxBody: int): HeadInfo =
  ## Parse the request line and headers of the request beginning at `start` in
  ## `buf`, whose header block ends at `hend` (index of the terminating
  ## CRLFCRLF). Never reads the body. Taking a start offset lets the caller
  ## advance through a pipelined buffer without front-deleting it per request.
  ## `maxBody` is the caller's inbound body cap, enforced on `Content-Length`.
  result.keepAlive = true
  let lineEnd = buf.find("\r\n", start)
  if lineEnd < 0 or lineEnd > hend:
    return
  let line = buf[start ..< lineEnd]
  let sp1 = line.find(' ')
  if sp1 < 0:
    return
  let sp2 = line.find(' ', sp1 + 1)
  if sp2 < 0:
    return
  result.httpMethod = line[0 ..< sp1]
  var path = line[sp1 + 1 ..< sp2]
  let version = line[sp2 + 1 ..< line.len]
  if version.len == 0:
    return

  # HTTP/1.0 defaults to close; /1.1 defaults to keep-alive.
  if version == "HTTP/1.0":
    result.keepAlive = false

  # Strip query string (keep the fragment-free path only).
  let q = path.find('?')
  if q >= 0:
    path = path[0 ..< q]
  result.path = path

  var i = lineEnd + 2
  while i < hend:
    let e = buf.find("\r\n", i)
    if e < 0 or e > hend:
      break
    let h = buf[i ..< e]
    let colon = h.find(':')
    if colon > 0:
      let name = h[0 ..< colon]
      let value = h[colon + 1 ..< h.len].strip()
      if cmpIgnoreCase(name, "content-length") == 0:
        var cl = 0
        if not strictContentLen(value, cl, maxBody):
          result.bad = true
        elif result.sawContentLen and cl != result.contentLen:
          result.bad = true                 # duplicate with a conflicting value
        else:
          result.sawContentLen = true
          result.contentLen = cl
      elif cmpIgnoreCase(name, "transfer-encoding") == 0:
        result.bad = true                   # chunked/any TE is not supported
      elif cmpIgnoreCase(name, "authorization") == 0:
        result.auth = value
      elif cmpIgnoreCase(name, "connection") == 0:
        let v = value.toLowerAscii
        if v.contains("close"):
          result.keepAlive = false
        elif v.contains("keep-alive"):
          result.keepAlive = true
    i = e + 2

  result.ok = true
