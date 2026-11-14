-- HTTP/1.1 request parsing, routes, JWT checks, SQLite record decoding and the JSON responses.
-- handle() reads one request from a byte buffer and writes one full response into M.resp.
local ffi = require("ffi")
local bit = require("bit")
local C = require("cdefs")
local json = require("json")
local sha = require("sha256")
local band, bor, rshift, lshift = bit.band, bit.bor, bit.rshift, bit.lshift
local escape, uint = json.escape, json.uint

local M = {}

local shim                          -- libtdshim.so
local C_POSTS, C_USERS, C_FEED = 0, 1, 2

local RESP_CAP = 512 * 1024
local resp = ffi.new("uint8_t[?]", RESP_CAP)
M.resp = resp
M.rlen = 0            -- length of the response handle() wrote
M.close = false       -- close the connection after this response
M.now = 0             -- unix time, set by the event loop once per pass
M.started = 0
M.in_txn = false      -- a pass's transaction is open
M.wrote = false       -- this pass wrote to the database
M.db_error = false
M.hold = false        -- hold write requests (the event loop sets this around a checkpoint)

-- stats
M.n_feed, M.n_post, M.n_like, M.n_create, M.n_other = 0, 0, 0, 0, 0

local MAX_REQ = 64 * 1024           -- head + body; larger requests get a 400 and the connection closed
local scratch = ffi.new("uint8_t[?]", MAX_REQ + 8)
local scratch2 = ffi.new("uint8_t[?]", MAX_REQ + 8)
-- Fixed buffers: the request being parsed, and the SQLite records the shim copies out. The hot
-- path indexes these with plain numbers and never makes a pointer, so it allocates nothing and
-- LuaJIT's garbage collector has nothing to do.
local REQ_CAP = 128 * 1024
local req = ffi.new("uint8_t[?]", REQ_CAP + 8)
M.req = req
M.REQ_CAP = REQ_CAP
local REC_CAP = 8192
local rec = ffi.new("uint8_t[?]", REC_CAP + 8)      -- a posts record
local urec = ffi.new("uint8_t[?]", REC_CAP + 8)     -- a users record
local irec = ffi.new("uint8_t[?]", REC_CAP + 8)     -- an index entry
local idout = ffi.new("int64_t[1]")
local created = ffi.new("uint8_t[40]")
local TOK_CAP = 4096
local tok = ffi.new("uint8_t[?]", TOK_CAP + 8)                -- the bearer token being checked
local namebuf = ffi.new("uint8_t[?]", TOK_CAP + 8)            -- its username claim, decoded
local digest = ffi.new("uint8_t[32]")
local sigb64 = ffi.new("uint8_t[44]")

---------------------------------------------------------------------------------------------------
-- fixed responses

local function full(status, body)
  local s = "HTTP/1.1 " .. status .. "\r\nContent-Type: application/json\r\nContent-Length: " .. #body .. "\r\n\r\n" .. body
  local b = ffi.new("uint8_t[?]", #s)
  ffi.copy(b, s, #s)
  return { b = b, n = #s }
end
local function err(status, msg) return full(status, '{"error":"' .. msg .. '"}') end

local R_NOT_FOUND = err("404 Not Found", "not found")
local R_BAD_ID = err("400 Bad Request", "invalid post id")
local R_POST_NOT_FOUND = err("404 Not Found", "post not found")
local R_NO_TOKEN = err("401 Unauthorized", "missing bearer token")
local R_BAD_TOKEN = err("401 Unauthorized", "invalid or expired token")
local R_BAD_PAYLOAD = err("401 Unauthorized", "invalid token payload")
local R_BAD_JSON = err("400 Bad Request", "malformed JSON body")
local R_BODY_REQUIRED = err("400 Bad Request", "body is required")
local R_BODY_TOO_LONG = err("400 Bad Request", "body must be at most 500 characters")
local R_INTERNAL = err("500 Internal Server Error", "internal server error")
local R_BAD_REQUEST = err("400 Bad Request", "bad request")
M.R_INTERNAL = R_INTERNAL

local function fixed(r)
  ffi.copy(resp, r.b, r.n)
  M.rlen = r.n
end

-- A response with a computed body: the head is written with room for a 4-digit Content-Length,
-- the body right after it, and the body is shifted by a byte or two if the length needs fewer or
-- more digits.
local function head(status)
  local s = "HTTP/1.1 " .. status .. "\r\nContent-Type: application/json\r\nContent-Length: "
  local b = ffi.new("uint8_t[?]", #s)
  ffi.copy(b, s, #s)
  return { b = b, n = #s }
end
local H_200, H_201 = head("200 OK"), head("201 Created")
local HN = H_200.n
assert(H_201.n + 0 == H_201.n)

local function begin_resp(h)
  ffi.copy(resp, h.b, h.n)
  return h.n + 8                    -- 4 digits + CRLF CRLF
end

local function end_resp(h, o)
  local start = h.n + 8
  local len = o - start
  local d = (len < 10 and 1) or (len < 100 and 2) or (len < 1000 and 3) or (len < 10000 and 4)
    or (len < 100000 and 5) or 6
  if d ~= 4 then C.memmove(resp + start + (d - 4), resp + start, len) end
  local e = uint(resp, h.n, len)
  resp[e] = 13; resp[e + 1] = 10; resp[e + 2] = 13; resp[e + 3] = 10
  M.rlen = e + 4 + len
end

local function lit(o, s)
  ffi.copy(resp + o, s, #s)
  return o + #s
end

---------------------------------------------------------------------------------------------------
-- SQLite record decoding (the bytes come from the B-tree cursor; see shim.c)

-- reads a varint at p[i]; returns value, next index
local function varint(p, i)
  local b = p[i]
  if b < 128 then return b, i + 1 end
  local v = band(b, 127)
  for k = 1, 7 do
    b = p[i + k]
    v = v * 128 + band(b, 127)
    if b < 128 then return v, i + k + 1 end
  end
  return v * 256 + p[i + 8], i + 9
end

-- big-endian two's complement integer of serial type t at p[i]
local function intval(p, i, t)
  if t == 1 then local v = p[i]; return v < 128 and v or v - 256 end
  if t == 2 then local v = p[i] * 256 + p[i + 1]; return v < 32768 and v or v - 65536 end
  if t == 3 then local v = p[i] * 65536 + p[i + 1] * 256 + p[i + 2]; return v < 8388608 and v or v - 16777216 end
  if t == 4 then
    return bor(lshift(p[i], 24), lshift(p[i + 1], 16), lshift(p[i + 2], 8), p[i + 3])
  end
  if t == 5 then
    local hi = p[i] * 256 + p[i + 1]
    if hi >= 32768 then hi = hi - 65536 end
    return hi * 4294967296 + p[i + 2] * 16777216 + p[i + 3] * 65536 + p[i + 4] * 256 + p[i + 5]
  end
  if t == 6 then
    local hi = bor(lshift(p[i], 24), lshift(p[i + 1], 16), lshift(p[i + 2], 8), p[i + 3])
    return hi * 4294967296 + p[i + 4] * 16777216 + p[i + 5] * 65536 + p[i + 6] * 256 + p[i + 7]
  end
  if t == 8 then return 0 end
  if t == 9 then return 1 end
  return nil
end

local function intsize(t)
  if t <= 4 then return t end
  if t == 5 then return 6 end
  if t == 6 then return 8 end
  return 0
end

-- Appends the post object for the posts record in `rec` (row `id`), or returns nil if its author row,
-- which the reference query's JOIN needs, doesn't exist. Returns false on a database error.
-- `likes` is the like count if the caller already has it, else -1. `uc` is the users cursor to
-- look the author up with (0 .. 19, see td_user_row in shim.c), or -1 for the shared one.
local function post_emit(o, id, likes, uc)
  local p = rec
  -- posts record: (id = NULL, the rowid), user_id, body, created_at
  local hdr, i = varint(p, 0)
  local t_id, t_uid, t_body, t_created
  t_id, i = varint(p, i)
  t_uid, i = varint(p, i)
  t_body, i = varint(p, i)
  t_created, i = varint(p, i)
  if t_body < 13 or band(t_body, 1) == 0 or t_created < 13 or band(t_created, 1) == 0 then return false end
  local d = hdr + intsize(t_id)
  local uid = intval(p, d, t_uid)
  if not uid then return false end
  d = d + intsize(t_uid)
  local nbody = rshift(t_body - 13, 1)
  local ncreated = rshift(t_created - 13, 1)

  o = lit(o, '{"id":')
  o = uint(resp, o, id)
  o = lit(o, ',"body":"')
  o = escape(resp, o, rec, d, nbody)
  o = lit(o, '","created_at":"')
  o = escape(resp, o, rec, d + nbody, ncreated)
  o = lit(o, '","author":"')

  local r
  if uc >= 0 then r = shim.td_user_row(uc, uid, urec, REC_CAP) else r = shim.td_row(C_USERS, uid, urec, REC_CAP) end
  if r <= 0 then
    if r < 0 then return false end
    return nil
  end
  -- users record: (id = NULL), username, created_at
  p = urec
  hdr, i = varint(p, 0)
  local t_name
  t_id, i = varint(p, i)
  t_name, i = varint(p, i)
  if t_name < 13 or band(t_name, 1) == 0 then return false end
  o = escape(resp, o, urec, hdr + intsize(t_id), rshift(t_name - 13, 1))
  o = lit(o, '","like_count":')

  if likes < 0 then
    likes = shim.td_count_likes(id)
    if likes < 0 then return false end
  end
  o = uint(resp, o, likes)
  resp[o] = 125
  return o + 1
end

-- the same for row `id`, looked up by rowid; nil if there is no such post
local function post_object(o, id)
  local r = shim.td_row(C_POSTS, id, rec, REC_CAP)
  if r <= 0 then
    if r < 0 then return false end
    return nil
  end
  return post_emit(o, id, -1, -1)
end

local function ensure_txn()
  if M.in_txn then return true end
  if shim.td_begin() ~= 0 then
    io.stderr:write("begin failed: ", ffi.string(shim.td_errmsg()), "\n")
    return false
  end
  M.in_txn = true
  return true
end

---------------------------------------------------------------------------------------------------
-- handlers

-- The feed with a tree descent per post, author and like count: right for any data.
local function get_feed_slow()
  local o = begin_resp(H_200)
  o = lit(o, '{"posts":[')
  local n = 0
  -- posts_created_at_id_idx is (created_at DESC, id DESC): its first 20 entries are the feed.
  local r = shim.td_first(C_FEED, irec, REC_CAP)
  while r > 0 and n < 20 do
    -- index record: (created_at, id, rowid)
    local p = irec
    local hdr, i = varint(p, 0)
    local t_created, t_pid
    t_created, i = varint(p, i)
    t_pid, i = varint(p, i)
    if t_created < 13 then return fixed(R_INTERNAL) end
    local id = intval(p, hdr + rshift(t_created - 13, 1), t_pid)
    if not id then return fixed(R_INTERNAL) end
    if n > 0 then resp[o] = 44; o = o + 1 end
    local e = post_object(o, id)
    if e then
      o = e; n = n + 1
    elseif e == false then
      return fixed(R_INTERNAL)
    elseif n > 0 then
      o = o - 1                       -- no author row: the JOIN drops this post
    end
    r = shim.td_next(C_FEED, irec, REC_CAP)
  end
  if r < 0 then return fixed(R_INTERNAL) end
  resp[o] = 93; resp[o + 1] = 125
  end_resp(H_200, o + 2)
end

local FEED_SPAN = 256
local feed_ids = ffi.new("double[20]")
local feed_likes = ffi.new("int32_t[?]", FEED_SPAN + 1)
local C_LIKES = 3

-- The feed when its 20 ids fall in a narrow, descending range (the newest posts normally have the
-- highest ids): three range scans instead of a descent per row. One walk of the feed index for
-- the ids, one walk of likes_post_id_idx over [lowest id, highest id] for every like count, and
-- one walk of posts backwards from the highest id. Anything unexpected hands over to the plain
-- plan above, so the answer never depends on the ids being dense or in timestamp order.
local FEED_PLAIN = os.getenv("TD_FEED_PLAIN") == "1"     -- testing: always use the plain plan

local function get_feed()
  if not ensure_txn() then return fixed(R_INTERNAL) end
  if FEED_PLAIN then return get_feed_slow() end
  local n = 0
  local r = shim.td_first(C_FEED, irec, REC_CAP)
  while r > 0 do
    -- index record: (created_at, id, rowid)
    local p = irec
    local hdr, i = varint(p, 0)
    local t_created, t_pid
    t_created, i = varint(p, i)
    t_pid, i = varint(p, i)
    if t_created < 13 then return fixed(R_INTERNAL) end
    local id = intval(p, hdr + rshift(t_created - 13, 1), t_pid)
    if not id then return fixed(R_INTERNAL) end
    if n > 0 and id >= feed_ids[n - 1] then return get_feed_slow() end
    feed_ids[n] = id
    n = n + 1
    if n == 20 then break end
    r = shim.td_next(C_FEED, irec, REC_CAP)
  end
  if r < 0 then return fixed(R_INTERNAL) end
  if n == 0 then return get_feed_slow() end
  local hi, lo = feed_ids[0], feed_ids[n - 1]
  local span = hi - lo
  if span > FEED_SPAN then return get_feed_slow() end

  -- like counts of every post in [lo, hi]
  for k = 0, span do feed_likes[k] = 0 end
  r = shim.td_likes_seek(lo, irec, REC_CAP)
  while r > 0 do
    -- index record: (post_id, rowid)
    local p = irec
    local hdr, i = varint(p, 0)
    local t_pid = varint(p, i)
    local pid = intval(p, hdr, t_pid)
    if not pid then return fixed(R_INTERNAL) end
    if pid > hi then break end
    feed_likes[pid - lo] = feed_likes[pid - lo] + 1
    r = shim.td_next(C_LIKES, irec, REC_CAP)
  end
  if r < 0 then return fixed(R_INTERNAL) end

  -- the rows, newest first. Every feed post has a posts cursor and a users cursor of its own
  -- (its id modulo 20; see td_user_row in shim.c), which usually still sit on its rows from the
  -- feed request before. Two posts that share a slot just move the cursors back and forth.
  local o = begin_resp(H_200)
  o = lit(o, '{"posts":[')
  for k = 0, n - 1 do
    local id = feed_ids[k]
    if k > 0 then resp[o] = 44; o = o + 1 end
    local slot = id % 20
    r = shim.td_post_row(slot, id, rec, REC_CAP)
    if r <= 0 then
      if r < 0 then return fixed(R_INTERNAL) end
      return get_feed_slow()
    end
    local e = post_emit(o, id, feed_likes[id - lo], slot)
    if not e then
      if e == false then return fixed(R_INTERNAL) end
      return get_feed_slow()
    end
    o = e
  end
  resp[o] = 93; resp[o + 1] = 125
  end_resp(H_200, o + 2)
end

local function get_post(id)
  if not ensure_txn() then return fixed(R_INTERNAL) end
  local o = begin_resp(H_200)
  o = lit(o, '{"post":')
  local e = post_object(o, id)
  if not e then
    if e == false then return fixed(R_INTERNAL) end
    return fixed(R_POST_NOT_FOUND)
  end
  resp[e] = 125
  end_resp(H_200, e + 1)
end

local function like_post(uid, id)
  if not ensure_txn() then return fixed(R_INTERNAL) end
  local r = shim.td_exists(C_POSTS, id)
  if r < 0 then return fixed(R_INTERNAL) end
  if r == 0 then return fixed(R_POST_NOT_FOUND) end
  r = shim.td_insert_like(uid, id)
  if r < 0 then M.db_error = true; return fixed(R_INTERNAL) end
  M.wrote = true
  local h = (r == 1) and H_201 or H_200
  local o = begin_resp(h)
  o = lit(o, (r == 1) and '{"liked":true,"already_liked":false,"post_id":' or '{"liked":true,"already_liked":true,"post_id":')
  o = uint(resp, o, id)
  resp[o] = 125
  end_resp(h, o + 1)
end

-- JavaScript's String.prototype.trim() whitespace, on UTF-8 bytes: returns the length of the
-- whitespace character at p[i] (0 if it isn't one)
local function wslen(p, i, n)
  local c = p[i]
  if c == 32 or (c >= 9 and c <= 13) then return 1 end
  if c < 0xC2 then return 0 end
  if c == 0xC2 then return (i + 1 < n and p[i + 1] == 0xA0) and 2 or 0 end
  if i + 2 >= n then return 0 end
  local c1, c2 = p[i + 1], p[i + 2]
  if c == 0xE1 then return (c1 == 0x9A and c2 == 0x80) and 3 or 0 end
  if c == 0xE2 then
    if c1 == 0x80 and ((c2 >= 0x80 and c2 <= 0x8A) or c2 == 0xA8 or c2 == 0xA9 or c2 == 0xAF) then return 3 end
    if c1 == 0x81 and c2 == 0x9F then return 3 end
    return 0
  end
  if c == 0xE3 then return (c1 == 0x80 and c2 == 0x80) and 3 or 0 end
  if c == 0xEF then return (c1 == 0xBB and c2 == 0xBF) and 3 or 0 end
  return 0
end

local KEYS_BODY = { "body" }

-- req[bs .. bs+n) is the request body, namebuf[0 .. nname) the token's username
local function create_post(uid, nname, bs, n)
  ffi.copy(scratch, req + bs, n)
  scratch[n] = 0
  local t = json.scan(scratch, n, KEYS_BODY, 1)
  if t == 0 then return fixed(R_BAD_JSON) end
  if t ~= json.T_OBJECT or json.vtype[0] ~= json.T_STRING then return fixed(R_BODY_REQUIRED) end
  local s, e = json.vstart[0], json.vend[0]
  -- b[0 .. len) is the decoded string: always in scratch2
  local b, len = scratch2, e - s
  if json.vesc[0] == 1 then
    len = json.decode(scratch, s, e, scratch2)
  else
    ffi.copy(scratch2, scratch + s, len)
  end
  -- trim
  local i = 0
  while i < len do
    local w = wslen(b, i, len)
    if w == 0 then break end
    i = i + w
  end
  local j = len
  while j > i do
    local k = j - 1
    while k > i and band(b[k], 0xC0) == 0x80 do k = k - 1 end   -- back to the start of the character
    if wslen(b, k, j) ~= j - k then break end
    j = k
  end
  if j <= i then return fixed(R_BODY_REQUIRED) end
  -- length in characters (code points), the unit of the schema's CHECK (length(body) ...)
  local chars = 0
  for k = i, j - 1 do
    if band(b[k], 0xC0) ~= 0x80 then chars = chars + 1 end
  end
  if chars > 500 then return fixed(R_BODY_TOO_LONG) end

  if not ensure_txn() then return fixed(R_INTERNAL) end
  if shim.td_insert_post(uid, b + i, j - i, idout, created, 40) ~= 0 then
    M.db_error = true
    return fixed(R_INTERNAL)
  end
  M.wrote = true
  local o = begin_resp(H_201)
  o = lit(o, '{"post":{"id":')
  o = uint(resp, o, tonumber(idout[0]))
  o = lit(o, ',"body":"')
  o = escape(resp, o, b, i, j - i)
  o = lit(o, '","created_at":"')
  local nc = 0
  while nc < 40 and created[nc] ~= 0 do nc = nc + 1 end
  o = escape(resp, o, created, 0, nc)
  o = lit(o, '","author":"')
  o = escape(resp, o, namebuf, 0, nname)
  o = lit(o, '","like_count":0}}')
  end_resp(H_201, o)
end

local function get_health()
  if shim.td_health() ~= 0 then
    local msg = ffi.string(shim.td_errmsg())
    local body = '{"status":"degraded","db":"unreachable","error":"' .. msg:gsub('[%c"\\]', "?") .. '"}'
    local r = full("503 Service Unavailable", body)
    return fixed(r)
  end
  local o = begin_resp(H_200)
  o = lit(o, '{"status":"ok","db":"ok","uptime_s":')
  o = uint(resp, o, math.max(0, M.now - M.started))
  resp[o] = 125
  end_resp(H_200, o + 1)
end

---------------------------------------------------------------------------------------------------
-- JWT (HS256)

local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
local b64enc = ffi.new("uint8_t[64]")
ffi.copy(b64enc, B64, 64)
local b64dec = ffi.new("int8_t[256]")
for i = 0, 255 do b64dec[i] = -1 end
for i = 0, 63 do b64dec[B64:byte(i + 1)] = i end

-- base64url without padding: decodes p[s .. s+n) into dst, returns the length or -1
local function b64url_decode(p, s, n, dst)
  local rem = n % 4
  if rem == 1 then return -1 end
  local o, i = 0, s
  local fullq = s + n - rem
  while i < fullq do
    local a, b, c, d = b64dec[p[i]], b64dec[p[i + 1]], b64dec[p[i + 2]], b64dec[p[i + 3]]
    if bor(a, b, c, d) < 0 then return -1 end
    dst[o] = bor(lshift(a, 2), rshift(b, 4))
    dst[o + 1] = band(bor(lshift(b, 4), rshift(c, 2)), 255)
    dst[o + 2] = band(bor(lshift(c, 6), d), 255)
    o = o + 3; i = i + 4
  end
  if rem >= 2 then
    local a, b = b64dec[p[i]], b64dec[p[i + 1]]
    if bor(a, b) < 0 then return -1 end
    dst[o] = bor(lshift(a, 2), rshift(b, 4)); o = o + 1
    if rem == 3 then
      local c = b64dec[p[i + 2]]
      if c < 0 then return -1 end
      dst[o] = band(bor(lshift(b, 4), rshift(c, 2)), 255); o = o + 1
    end
  end
  return o
end

local function b64url_encode32(src, dst)       -- 32 bytes -> 43 characters
  local o = 0
  for i = 0, 27, 3 do
    local a, b, c = src[i], src[i + 1], src[i + 2]
    dst[o] = b64enc[rshift(a, 2)]
    dst[o + 1] = b64enc[bor(lshift(band(a, 3), 4), rshift(b, 4))]
    dst[o + 2] = b64enc[bor(lshift(band(b, 15), 2), rshift(c, 6))]
    dst[o + 3] = b64enc[band(c, 63)]
    o = o + 4
  end
  local a, b = src[30], src[31]
  dst[o] = b64enc[rshift(a, 2)]
  dst[o + 1] = b64enc[bor(lshift(band(a, 3), 4), rshift(b, 4))]
  dst[o + 2] = b64enc[lshift(band(b, 15), 2)]
end

local HS256_HEADER = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"     -- {"alg":"HS256","typ":"JWT"}
local KEYS_HEADER = { "alg" }
local KEYS_CLAIMS = { "exp", "sub", "username" }
local auth_uid, auth_nname = 0, 0

-- Verifies the token tok[0 .. n). Returns nil when it is valid (auth_uid, namebuf and auth_nname
-- are set), else the error response. Every token's signature, alg and exp are checked, every time.
local function check_token(n)
  local t = tok
  local d1, d2 = -1, -1
  for i = 0, n - 1 do
    if t[i] == 46 then
      if d1 < 0 then d1 = i elseif d2 < 0 then d2 = i else return R_BAD_TOKEN end
    end
  end
  if d2 < 0 or d1 == 0 then return R_BAD_TOKEN end
  -- header: HS256 only
  if not (d1 == #HS256_HEADER and C.memcmp(t, HS256_HEADER, d1) == 0) then
    local hn = b64url_decode(t, 0, d1, scratch)
    if hn < 0 then return R_BAD_TOKEN end
    scratch[hn] = 0
    if json.scan(scratch, hn, KEYS_HEADER, 1) ~= json.T_OBJECT or json.vtype[0] ~= json.T_STRING then return R_BAD_TOKEN end
    local s, e = json.vstart[0], json.vend[0]
    if json.vesc[0] == 1 or e - s ~= 5 or C.memcmp(scratch + s, "HS256", 5) ~= 0 then return R_BAD_TOKEN end
  end
  -- signature over "header.payload", compared in constant time against its canonical spelling
  if n - d2 - 1 ~= 43 then return R_BAD_TOKEN end
  sha.hmac(t, 0, d2, digest)
  b64url_encode32(digest, sigb64)
  local diff = 0
  for i = 0, 42 do diff = bor(diff, bit.bxor(sigb64[i], t[d2 + 1 + i])) end
  if diff ~= 0 then return R_BAD_TOKEN end
  -- claims
  local pl = b64url_decode(t, d1 + 1, d2 - d1 - 1, scratch)
  if pl < 0 then return R_BAD_TOKEN end
  scratch[pl] = 0
  if json.scan(scratch, pl, KEYS_CLAIMS, 3) ~= json.T_OBJECT then return R_BAD_TOKEN end
  if json.vtype[0] ~= json.T_NUMBER then return R_BAD_TOKEN end           -- exp is required
  local exp = 0
  for i = json.vstart[0], json.vend[0] - 1 do
    local c = scratch[i]
    if c < 48 or c > 57 then                          -- a fraction, exponent or sign: the slow way
      exp = tonumber(ffi.string(scratch + json.vstart[0], json.vend[0] - json.vstart[0]))
      break
    end
    exp = exp * 10 + (c - 48)
  end
  if not exp or M.now >= exp then return R_BAD_TOKEN end
  -- sub: a positive integer as a string
  if json.vtype[1] ~= json.T_STRING or json.vesc[1] == 1 then return R_BAD_PAYLOAD end
  local s, e = json.vstart[1], json.vend[1]
  if e - s < 1 or e - s > 15 then return R_BAD_PAYLOAD end
  local uid = 0
  for i = s, e - 1 do
    local c = scratch[i]
    if c < 48 or c > 57 then return R_BAD_PAYLOAD end
    uid = uid * 10 + (c - 48)
  end
  if uid < 1 then return R_BAD_PAYLOAD end
  if json.vtype[2] ~= json.T_STRING then return R_BAD_PAYLOAD end
  s, e = json.vstart[2], json.vend[2]
  if json.vesc[2] == 1 then
    auth_nname = json.decode(scratch, s, e, namebuf)
  else
    ffi.copy(namebuf, scratch + s, e - s)
    auth_nname = e - s
  end
  auth_uid = uid
  return nil
end

---------------------------------------------------------------------------------------------------
-- HTTP

-- parses a post id at p[s .. e): digits only, positive. Returns the id, 0 if invalid.
local function post_id(p, s, e)
  if e <= s then return 0 end
  local v = 0
  for i = s, e - 1 do
    local c = p[i]
    if c < 48 or c > 57 then return 0 end
    v = v * 10 + (c - 48)
  end
  if e - s > 15 then return 9007199254740991 end      -- beyond 2^53: no such post
  return v
end

local function lower(c)
  if c >= 65 and c <= 90 then return c + 32 end
  return c
end

-- case-insensitive: does the header name at p[i] equal `name` (lower case) followed by ':'?
local function is_header(p, i, e, name)
  local n = #name
  if i + n >= e or p[i + n] ~= 58 then return false end
  for k = 1, n - 1 do
    if lower(p[i + k]) ~= name:byte(k + 1) then return false end
  end
  return true
end

-- Reads one request from req[0 .. n). Returns the number of bytes it used, 0 if the request is not
-- complete yet, -1 if it can't be parsed (a 400 is in M.resp and the connection must close), or
-- -2 if it is a write and writes are on hold (nothing was done; ask again later).
function M.handle(n)
  local p = req
  M.close = false
  -- end of the head
  local he = -1
  do
    local i = 0
    local lim = n - 3
    while i < lim do
      local q = C.memchr(p + i, 13, lim - i)
      if q == nil then break end
      i = ffi.cast("const uint8_t *", q) - p
      if p[i + 1] == 10 and p[i + 2] == 13 and p[i + 3] == 10 then he = i; break end
      i = i + 1
    end
  end
  if he < 0 then
    if n > 16384 then fixed(R_BAD_REQUEST); M.close = true; return -1 end
    return 0
  end

  -- request line: METHOD SP target SP HTTP/1.x
  local is_get
  local ps                                   -- start of the path
  if p[0] == 71 and p[1] == 69 and p[2] == 84 and p[3] == 32 then
    is_get = true; ps = 4
  elseif p[0] == 80 and p[1] == 79 and p[2] == 83 and p[3] == 84 and p[4] == 32 then
    is_get = false; ps = 5
  else
    is_get = nil
    ps = 0
    while ps < he and p[ps] ~= 32 do ps = ps + 1 end
    ps = ps + 1
  end
  local le = ps                              -- end of the path (a query string is ignored)
  while true do
    local c = p[le]
    if c == 32 or c == 13 or c == 63 then break end
    le = le + 1
  end
  local pe = le
  while p[le] ~= 32 and p[le] ~= 13 do le = le + 1 end
  if p[le] ~= 32 or pe == ps or p[ps] ~= 47 then fixed(R_BAD_REQUEST); M.close = true; return -1 end
  -- le is the space before HTTP/1.x; the first header line starts after the CRLF
  local i = le
  while p[i] ~= 13 do i = i + 1 end
  i = i + 2

  -- headers: only Content-Length, Authorization, Connection and Transfer-Encoding matter
  local clen = 0
  local as, ae = -1, -1                      -- Authorization value
  local hend = he + 2
  while i < hend do
    local c = lower(p[i])
    local q = C.memchr(p + i, 13, hend - i)
    local e = ffi.cast("const uint8_t *", q) - p
    if c == 99 then
      if is_header(p, i, e, "content-length") then
        local k = i + 15
        while k < e and (p[k] == 32 or p[k] == 9) do k = k + 1 end
        if k == e then fixed(R_BAD_REQUEST); M.close = true; return -1 end
        clen = 0
        while k < e do
          local d = p[k]
          if d < 48 or d > 57 then
            while k < e and (p[k] == 32 or p[k] == 9) do k = k + 1 end
            if k < e then fixed(R_BAD_REQUEST); M.close = true; return -1 end
            break
          end
          clen = clen * 10 + (d - 48)
          if clen > MAX_REQ then fixed(R_BAD_REQUEST); M.close = true; return -1 end
          k = k + 1
        end
      elseif is_header(p, i, e, "connection") then
        local k = i + 11
        while k < e and (p[k] == 32 or p[k] == 9) do k = k + 1 end
        if e - k == 5 and lower(p[k]) == 99 and lower(p[k + 1]) == 108 then M.close = true end
      end
    elseif c == 97 then
      if is_header(p, i, e, "authorization") then
        as = i + 14
        while as < e and (p[as] == 32 or p[as] == 9) do as = as + 1 end
        ae = e
        while ae > as and (p[ae - 1] == 32 or p[ae - 1] == 9) do ae = ae - 1 end
      end
    elseif c == 116 then
      if is_header(p, i, e, "transfer-encoding") then fixed(R_BAD_REQUEST); M.close = true; return -1 end
    end
    i = e + 2
  end

  local bs = he + 4                          -- body
  local total = bs + clen
  if total > MAX_REQ then fixed(R_BAD_REQUEST); M.close = true; return -1 end
  if total > n then return 0 end

  -- routes
  local plen = pe - ps
  if is_get then
    if plen == 5 and C.memcmp(p + ps, "/feed", 5) == 0 then
      M.n_feed = M.n_feed + 1
      get_feed()
    elseif plen > 7 and C.memcmp(p + ps, "/posts/", 7) == 0 then
      M.n_post = M.n_post + 1
      local id = post_id(p, ps + 7, pe)
      if id == 0 then fixed(R_BAD_ID) else get_post(id) end
    elseif plen == 7 and C.memcmp(p + ps, "/health", 7) == 0 then
      get_health()
    else
      M.n_other = M.n_other + 1
      fixed(R_NOT_FOUND)
    end
  elseif is_get == false then
    local is_create = plen == 6 and C.memcmp(p + ps, "/posts", 6) == 0
    local is_like = (not is_create) and plen > 12 and C.memcmp(p + ps, "/posts/", 7) == 0
      and C.memcmp(p + pe - 5, "/like", 5) == 0
    if is_like then
      for k = ps + 7, pe - 6 do
        if p[k] == 47 then is_like = false; break end
      end
    end
    if M.hold and (is_create or is_like) then
      return -2                              -- writes are held while a checkpoint finishes
    elseif not (is_create or is_like) then
      M.n_other = M.n_other + 1
      fixed(R_NOT_FOUND)
    elseif as < 0 or ae - as < 7 or C.memcmp(p + as, "Bearer ", 7) ~= 0 then
      fixed(R_NO_TOKEN)                      -- auth is checked before the id
    else
      local ts = as + 7
      while ts < ae and p[ts] == 32 do ts = ts + 1 end
      local tn = ae - ts
      local bad = R_BAD_TOKEN
      if tn <= TOK_CAP then
        ffi.copy(tok, req + ts, tn)
        bad = check_token(tn)
      end
      if bad then
        fixed(bad)
      elseif is_create then
        M.n_create = M.n_create + 1
        create_post(auth_uid, auth_nname, bs, clen)
      else
        M.n_like = M.n_like + 1
        local id = post_id(p, ps + 7, pe - 5)
        if id == 0 then fixed(R_BAD_ID) else like_post(auth_uid, id) end
      end
    end
  else
    M.n_other = M.n_other + 1
    fixed(R_NOT_FOUND)
  end
  return total
end

function M.init(lib, secret)
  shim = lib
  json.init(lib)
  sha.set_key(secret)
end

return M
