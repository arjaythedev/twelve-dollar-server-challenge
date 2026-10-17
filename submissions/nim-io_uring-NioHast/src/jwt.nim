## HS256 JWT verification (stdlib + nimcrypto only).
##
## Contract (frozen, see INTERFACES.md):
##   jsInvalid    = malformed token / bad signature / expired / alg != HS256
##   jsBadPayload = signature valid but `sub` is not a positive integer string
##                  or `username` is not a JSON string
import std/[json, times, strutils]
import nimcrypto

type
  JwtStatus* = enum jsOk, jsInvalid, jsBadPayload
  JwtInfo* = object
    userId*: int64
    username*: string

proc b64Value(c: char): int {.inline.} =
  ## Value 0..63 for a base64 char, accepting BOTH URL-safe (-, _) and
  ## standard (+, /) alphabets. Returns -1 for anything else.
  case c
  of 'A'..'Z': ord(c) - ord('A')
  of 'a'..'z': ord(c) - ord('a') + 26
  of '0'..'9': ord(c) - ord('0') + 52
  of '+', '-': 62
  of '/', '_': 63
  else: -1

proc b64Decode(s: string; outBytes: var seq[byte]): bool =
  ## Strict base64/base64url decode without requiring padding. Padding '=' is
  ## tolerated and ignored. Leftover bits must be zero (canonical encoding).
  outBytes.setLen(0)
  var acc = 0'u32
  var bits = 0
  for c in s:
    if c == '=' or c == '\n' or c == '\r':
      continue
    let v = b64Value(c)
    if v < 0: return false
    acc = (acc shl 6) or uint32(v)
    bits += 6
    if bits >= 8:
      bits -= 8
      outBytes.add byte((acc shr bits) and 0xFF'u32)
  if bits >= 6: return false
  if bits > 0 and (acc and ((1'u32 shl bits) - 1'u32)) != 0'u32:
    return false
  true

proc bytesToString(b: openArray[byte]): string =
  result = newString(b.len)
  for i in 0 ..< b.len:
    result[i] = char(b[i])

proc constantTimeEq(a, b: openArray[byte]): bool =
  ## XOR-accumulate comparison; no early return inside the loop. Lengths are
  ## public (signature is always 32 bytes) so the length check may short-circuit.
  if a.len != b.len: return false
  var diff = 0'u8
  for i in 0 ..< a.len:
    diff = diff or (a[i] xor b[i])
  diff == 0'u8

proc parseObject(raw: string; node: var JsonNode): bool =
  try:
    node = parseJson(raw)
  except CatchableError:
    return false
  node.kind == JObject

proc parsePositiveInt64(s: string; value: var int64): bool =
  ## `s` must be non-empty ASCII digits representing an integer > 0 that fits
  ## in int64.
  if s.len == 0: return false
  for c in s:
    if c < '0' or c > '9': return false
  try:
    value = parseBiggestInt(s)
  except ValueError:
    return false
  value > 0

proc verifyJwt*(token, secret: string): tuple[status: JwtStatus, info: JwtInfo] =
  result = (jsInvalid, JwtInfo())

  # 1. Exactly three dot-separated segments.
  let firstDot = token.find('.')
  if firstDot < 0: return
  let secondDot = token.find('.', firstDot + 1)
  if secondDot < 0: return
  if token.find('.', secondDot + 1) >= 0: return

  let headerB64 = token[0 ..< firstDot]
  let payloadB64 = token[firstDot + 1 ..< secondDot]
  let sigB64 = token[secondDot + 1 ..< token.len]

  # 2. Header must be JSON with alg == "HS256".
  var headerBytes: seq[byte]
  if not b64Decode(headerB64, headerBytes): return
  var header: JsonNode
  if not parseObject(bytesToString(headerBytes), header): return
  if not header.hasKey("alg"): return
  if header["alg"].kind != JString or header["alg"].getStr != "HS256": return

  # 3. Recompute HMAC-SHA256 over "header.payload" and compare to the decoded
  #    signature in constant time.
  var sigBytes: seq[byte]
  if not b64Decode(sigB64, sigBytes): return
  let signingInput = headerB64 & "." & payloadB64
  let mac = sha256.hmac(secret, signingInput)
  if not constantTimeEq(mac.data, sigBytes): return

  # 4. Payload must be a JSON object with a numeric, non-expired exp.
  var payloadBytes: seq[byte]
  if not b64Decode(payloadB64, payloadBytes): return
  var payload: JsonNode
  if not parseObject(bytesToString(payloadBytes), payload): return
  if not payload.hasKey("exp"): return
  let expNode = payload["exp"]
  var expVal: float
  case expNode.kind
  of JInt: expVal = float(expNode.getBiggestInt)
  of JFloat: expVal = expNode.getFloat
  else: return
  if epochTime() > expVal: return

  # 5. Signature is valid: `sub` must be a positive-integer digit string and
  #    `username` must be a JSON string, otherwise jsBadPayload.
  if not payload.hasKey("sub") or payload["sub"].kind != JString:
    result.status = jsBadPayload
    return
  var userId: int64
  if not parsePositiveInt64(payload["sub"].getStr, userId):
    result.status = jsBadPayload
    return
  if not payload.hasKey("username") or payload["username"].kind != JString:
    result.status = jsBadPayload
    return

  result.status = jsOk
  result.info.userId = userId
  result.info.username = payload["username"].getStr
