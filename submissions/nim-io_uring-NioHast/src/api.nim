## Transport-agnostic request handling: routing, auth ordering, exact error semantics.
## Implements the contract in SPEC.md / test/test.sh byte-for-byte.
import std/[strutils, times]
import model, db, jwt, jsonbuf

type ApiResponse* = object
  status*: int
  body*: string

const
  ErrNotFound     = """{"error":"not found"}"""
  ErrInvalidId    = """{"error":"invalid post id"}"""
  ErrPostNotFound = """{"error":"post not found"}"""
  ErrMissingBearer = """{"error":"missing bearer token"}"""
  ErrInvalidToken  = """{"error":"invalid or expired token"}"""
  ErrBadPayload    = """{"error":"invalid token payload"}"""
  ErrMalformed     = """{"error":"malformed JSON body"}"""
  ErrBodyRequired  = """{"error":"body is required"}"""
  ErrTooLong       = """{"error":"body must be at most 500 characters"}"""
  ErrInternal      = """{"error":"internal server error"}"""

# --- uptime support (shared across threads) ---
var gStartTime {.global.}: float
proc markStart*() = gStartTime = epochTime()
proc uptimeSeconds*(): int64 =
  let now = epochTime()
  int64(now - gStartTime)

proc jsonRes(status: int, body: string): ApiResponse =
  ApiResponse(status: status, body: body)

proc parsePositiveId(s: string): tuple[ok: bool, id: int64] =
  ## Accept only ASCII digits forming a value >= 1 that fits in int64.
  ##
  ## Overflow guard: int64 holds at most 19 decimal digits, and the check below
  ## bounds `v` against the *next* digit before multiplying, so no intermediate
  ## `v*10 + d` can overflow. The old guard (`v > 922337203685477580`) fired one
  ## step too late: v=922337203685477580 with d=8 or 9 overflowed and raised a
  ## Defect that aborted the single-threaded process (both fixes below: the
  ## `len > 19` cap and the digit-aware bound).
  if s.len == 0 or s.len > 19: return (false, 0'i64)
  var v: int64 = 0
  for c in s:
    if c < '0' or c > '9': return (false, 0'i64)
    let d = int64(ord(c) - ord('0'))
    if v > (9_223_372_036_854_775_807'i64 - d) div 10:
      return (false, 0'i64)  # v*10 + d would overflow int64
    v = v * 10 + d
  if v < 1: return (false, 0'i64)
  (true, v)

type
  AuthKind = enum akMissing, akInvalid, akBadPayload, akOk
  Auth = object
    kind: AuthKind
    userId: int64
    username: string

proc doAuth(authHeader, secret: string): Auth =
  if not authHeader.startsWith("Bearer "):
    return Auth(kind: akMissing)
  let token = authHeader[7 .. ^1]
  let (status, info) = verifyJwt(token, secret)
  case status
  of jsOk:      Auth(kind: akOk, userId: info.userId, username: info.username)
  of jsInvalid: Auth(kind: akInvalid)
  of jsBadPayload: Auth(kind: akBadPayload)

proc authResponse(a: Auth): ApiResponse =
  case a.kind
  of akMissing:    jsonRes(401, ErrMissingBearer)
  of akInvalid:    jsonRes(401, ErrInvalidToken)
  of akBadPayload: jsonRes(401, ErrBadPayload)
  of akOk:         ApiResponse(status: 0, body: "")  # sentinel; never used

proc postEnvelope(p: Post): string =
  result = """{"post":"""
  result.writePost(p)
  result.add '}'

proc handleRequest*(database: Db, jwtSecret, httpMethod, path, authHeader, rawBody: string): ApiResponse =
  if httpMethod == "GET":
    if path == "/health":
      if database.health():
        return jsonRes(200, """{"status":"ok","db":"ok","uptime_s":""" & $uptimeSeconds() & "}")
      else:
        return jsonRes(503, """{"status":"degraded","db":"unreachable","error":"db unreachable"}""")
    if path == "/feed":
      var posts: seq[Post]
      database.feed(posts)
      var b = ""
      b.writeFeed(posts)
      return jsonRes(200, b)
    if path.startsWith("/posts/"):
      let pid = parsePositiveId(path[7 .. ^1])
      if not pid.ok: return jsonRes(400, ErrInvalidId)
      var p: Post
      if database.getPost(pid.id, p):
        return jsonRes(200, postEnvelope(p))
      else:
        return jsonRes(404, ErrPostNotFound)
    return jsonRes(404, ErrNotFound)

  if httpMethod == "POST":
    if path == "/posts":
      let a = doAuth(authHeader, jwtSecret)          # auth BEFORE body validation
      if a.kind != akOk: return authResponse(a)
      let pb = parseBody(rawBody)
      case pb.kind
      of bkMalformed: return jsonRes(400, ErrMalformed)
      of bkRequired:  return jsonRes(400, ErrBodyRequired)
      of bkOk:
        let trimmed = pb.body.strip
        if trimmed.len == 0: return jsonRes(400, ErrBodyRequired)
        if trimmed.len > 500: return jsonRes(400, ErrTooLong)
        var p: Post
        if database.createPost(a.userId, a.username, trimmed, p):
          return jsonRes(201, postEnvelope(p))
        else:
          return jsonRes(500, ErrInternal)
    if path.startsWith("/posts/") and path.endsWith("/like"):
      let a = doAuth(authHeader, jwtSecret)          # auth BEFORE id validation
      if a.kind != akOk: return authResponse(a)
      let mid = path[7 ..< path.len - 5]
      let pid = parsePositiveId(mid)
      if not pid.ok: return jsonRes(400, ErrInvalidId)
      case database.like(a.userId, pid.id)
      of loLiked:
        return jsonRes(201, """{"liked":true,"already_liked":false,"post_id":""" & $pid.id & "}")
      of loAlready:
        return jsonRes(200, """{"liked":true,"already_liked":true,"post_id":""" & $pid.id & "}")
      of loNoPost:
        return jsonRes(404, ErrPostNotFound)
    return jsonRes(404, ErrNotFound)

  jsonRes(404, ErrNotFound)
