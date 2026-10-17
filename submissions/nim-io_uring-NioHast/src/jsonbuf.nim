## Byte-level JSON writing and minimal request-body parsing.
##
## parseBody uses std/json for correctness; the serializer below writes the
## bytes directly into the caller's buffer so a hot response never builds an
## intermediate per-element string.
import std/json
import model

const HexDigits = "0123456789abcdef"

## Characters that a JSON string must escape. Everything outside this set is
## copied in one run, so ordinary text costs one append per run instead of one
## per byte.
const Escaped = block:
  var s: set[char]
  s.incl '"'
  s.incl '\\'
  for i in 0 ..< 0x20:
    s.incl char(i)
  s

proc addChars(buf: var string, src: openArray[char]) {.inline.} =
  ## Zero-allocation bulk append: grow once and copy the run in.
  if src.len == 0:
    return
  let base = buf.len
  buf.setLen(base + src.len)
  copyMem(addr buf[base], unsafeAddr src[0], src.len)

proc addEscaped*(buf: var string, s: string) =
  ## Append `s` as the *contents* of a JSON string (no surrounding quotes).
  ## Unescaped runs are appended in a single bulk copy; the escape sequences are
  ## byte-for-byte the ones the previous per-character writer produced.
  var runStart = 0
  var i = 0
  while i < s.len:
    let c = s[i]
    if c in Escaped:
      if i > runStart:
        buf.addChars s.toOpenArray(runStart, i - 1)
      case c
      of '"':  buf.add "\\\""
      of '\\': buf.add "\\\\"
      of '\b': buf.add "\\b"
      of '\f': buf.add "\\f"
      of '\n': buf.add "\\n"
      of '\r': buf.add "\\r"
      of '\t': buf.add "\\t"
      else:
        let o = c.ord
        buf.add "\\u00"
        buf.add HexDigits[(o shr 4) and 0xF]
        buf.add HexDigits[o and 0xF]
      runStart = i + 1
    inc i
  if s.len > runStart:
    buf.addChars s.toOpenArray(runStart, s.len - 1)

proc addJStr*(buf: var string, s: string) =
  buf.add '"'
  buf.addEscaped s
  buf.add '"'

proc addJInt*(buf: var string, v: int64) =
  ## Append the decimal form of `v` by writing the digits straight into `buf`;
  ## no `$v` temp string. The magnitude is taken through uint64 so int64.low
  ## (whose negation overflows int64) still prints correctly.
  var tmp: array[24, char]
  var i = tmp.len
  let neg = v < 0
  var mag: uint64 =
    if neg: uint64(-(v + 1)) + 1'u64
    else:   uint64(v)
  if mag == 0:
    dec i
    tmp[i] = '0'
  else:
    while mag > 0:
      dec i
      tmp[i] = char(ord('0') + int(mag mod 10))
      mag = mag div 10
  if neg:
    dec i
    tmp[i] = '-'
  buf.addChars tmp.toOpenArray(i, tmp.len - 1)

proc writePost*(buf: var string, p: Post) =
  buf.add "{\"id\":"
  buf.addJInt p.id
  buf.add ",\"body\":"
  buf.addJStr p.body
  buf.add ",\"created_at\":"
  buf.addJStr p.createdAt
  buf.add ",\"author\":"
  buf.addJStr p.author
  buf.add ",\"like_count\":"
  buf.addJInt p.likeCount
  buf.add "}"

proc writeFeed*(buf: var string, posts: openArray[Post]) =
  buf.add "{\"posts\":["
  for i in 0 ..< posts.len:
    if i > 0: buf.add ','
    buf.writePost posts[i]
  buf.add "]}"

type BodyKind* = enum
  bkOk, bkMalformed, bkRequired

proc parseBody*(raw: string): tuple[kind: BodyKind, body: string] =
  ## bkMalformed -> invalid JSON; bkRequired -> no usable string "body"; bkOk -> decoded body (raw, untrimmed).
  var node: JsonNode
  try:
    node = parseJson(raw)
  except CatchableError:
    return (bkMalformed, "")
  if node.kind != JObject or not node.hasKey("body"):
    return (bkRequired, "")
  let b = node["body"]
  if b.kind != JString:
    return (bkRequired, "")
  (bkOk, b.getStr)
