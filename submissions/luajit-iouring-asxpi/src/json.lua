-- A validating JSON reader over raw bytes that allocates nothing: it checks the whole document and
-- remembers where the values of a few wanted top-level keys are. Plus the string decoder and the
-- output escaper. The input must be followed by a NUL byte (p[n] == 0), which ends every scan.
local ffi = require("ffi")
local bit = require("bit")
require("cdefs")
local band, bor, rshift, lshift = bit.band, bit.bor, bit.rshift, bit.lshift

local M = {}

M.T_STRING, M.T_NUMBER, M.T_OBJECT, M.T_ARRAY, M.T_TRUE, M.T_FALSE, M.T_NULL = 1, 2, 3, 4, 5, 6, 7
local T_STRING, T_NUMBER, T_OBJECT, T_ARRAY, T_TRUE, T_FALSE, T_NULL = 1, 2, 3, 4, 5, 6, 7

local MAXW = 4
local vtype = ffi.new("int32_t[?]", MAXW)   -- type of each wanted key's value, 0 = absent
local vstart = ffi.new("int32_t[?]", MAXW)  -- strings: first byte after the quote; numbers: first byte
local vend = ffi.new("int32_t[?]", MAXW)    -- one past the last byte (strings: the closing quote)
local vesc = ffi.new("uint8_t[?]", MAXW)    -- string has backslash escapes
M.vtype, M.vstart, M.vend, M.vesc = vtype, vstart, vend, vesc

local P                 -- current input
local s_esc = false     -- last string scanned had escapes
local v_type = 0        -- type of the last value scanned
local keybuf = ffi.new("uint8_t[64]")

-- Compact JSON has no whitespace, so the loop sits in a function of its own: a loop that is
-- entered on every call and left at once never becomes a trace, the function around it gets
-- blacklisted, and then no trace that calls it can be compiled either.
local function ws_more(i)
  while true do
    local c = P[i]
    if c == 32 or c == 10 or c == 13 or c == 9 then i = i + 1 else return i end
  end
end
local function ws(i)
  local c = P[i]
  if c ~= 32 and c ~= 10 and c ~= 13 and c ~= 9 then return i end
  return ws_more(i + 1)
end

local function hex(c)
  if c >= 48 and c <= 57 then return c - 48 end
  if c >= 97 and c <= 102 then return c - 87 end
  if c >= 65 and c <= 70 then return c - 55 end
  return -1
end

-- the rest of a string from its first backslash on; returns the index of the closing quote, or -1
local function str_more(i)
  while true do
    local c = P[i]
    if c == 34 then return i end
    if c < 32 then return -1 end
    if c == 92 then
      s_esc = true
      c = P[i + 1]
      if c == 117 then
        if hex(P[i + 2]) < 0 or hex(P[i + 3]) < 0 or hex(P[i + 4]) < 0 or hex(P[i + 5]) < 0 then return -1 end
        i = i + 6
      elseif c == 34 or c == 92 or c == 47 or c == 98 or c == 102 or c == 110 or c == 114 or c == 116 then
        i = i + 2
      else
        return -1
      end
    else
      i = i + 1
    end
  end
end

-- i is just past the opening quote; returns the index of the closing quote, or -1.
-- The run up to the quote, a backslash or a control byte is found by the shim's scan: the keys
-- and values here are a few bytes long, and a loop that short rarely completes a trace.
local shim, PN
local function str(i)
  s_esc = false
  local k = i + shim.td_json_clean(P + i, PN - i)
  local c = P[k]                      -- P[PN] is the terminating zero
  if c == 34 then return k end
  if c ~= 92 then return -1 end
  return str_more(k)
end

local function num(i)
  if P[i] == 45 then i = i + 1 end
  local c = P[i]
  if c == 48 then
    i = i + 1
  elseif c >= 49 and c <= 57 then
    repeat i = i + 1; c = P[i] until c < 48 or c > 57
  else
    return -1
  end
  if P[i] == 46 then
    i = i + 1; c = P[i]
    if c < 48 or c > 57 then return -1 end
    repeat i = i + 1; c = P[i] until c < 48 or c > 57
  end
  c = P[i]
  if c == 101 or c == 69 then
    i = i + 1; c = P[i]
    if c == 43 or c == 45 then i = i + 1; c = P[i] end
    if c < 48 or c > 57 then return -1 end
    repeat i = i + 1; c = P[i] until c < 48 or c > 57
  end
  return i
end

local value

local function lit(i, a, b, c, d)
  if P[i + 1] == a and P[i + 2] == b and P[i + 3] == c and (d == 0 or P[i + 4] == d) then
    return i + (d == 0 and 4 or 5)
  end
  return -1
end

-- returns the index after the value, or -1; sets v_type
value = function(i, depth)
  local c = P[i]
  if c == 34 then
    local e = str(i + 1)
    if e < 0 then return -1 end
    v_type = T_STRING
    return e + 1
  elseif c == 123 then
    if depth > 64 then return -1 end
    i = ws(i + 1)
    if P[i] == 125 then v_type = T_OBJECT; return i + 1 end
    while true do
      if P[i] ~= 34 then return -1 end
      i = str(i + 1)
      if i < 0 then return -1 end
      i = ws(i + 1)
      if P[i] ~= 58 then return -1 end
      i = value(ws(i + 1), depth + 1)
      if i < 0 then return -1 end
      i = ws(i)
      c = P[i]
      if c == 44 then i = ws(i + 1)
      elseif c == 125 then v_type = T_OBJECT; return i + 1
      else return -1 end
    end
  elseif c == 91 then
    if depth > 64 then return -1 end
    i = ws(i + 1)
    if P[i] == 93 then v_type = T_ARRAY; return i + 1 end
    while true do
      i = value(i, depth + 1)
      if i < 0 then return -1 end
      i = ws(i)
      c = P[i]
      if c == 44 then i = ws(i + 1)
      elseif c == 93 then v_type = T_ARRAY; return i + 1
      else return -1 end
    end
  elseif c == 116 then
    v_type = T_TRUE; return lit(i, 114, 117, 101, 0)
  elseif c == 102 then
    v_type = T_FALSE; return lit(i, 97, 108, 115, 101)
  elseif c == 110 then
    v_type = T_NULL; return lit(i, 117, 108, 108, 0)
  else
    v_type = T_NUMBER
    return num(i)
  end
end

local function put_utf8(dst, n, cp)
  if cp < 0x80 then
    dst[n] = cp; return n + 1
  elseif cp < 0x800 then
    dst[n] = bor(0xC0, rshift(cp, 6)); dst[n + 1] = bor(0x80, band(cp, 63)); return n + 2
  elseif cp < 0x10000 then
    dst[n] = bor(0xE0, rshift(cp, 12)); dst[n + 1] = bor(0x80, band(rshift(cp, 6), 63))
    dst[n + 2] = bor(0x80, band(cp, 63)); return n + 3
  end
  dst[n] = bor(0xF0, rshift(cp, 18)); dst[n + 1] = bor(0x80, band(rshift(cp, 12), 63))
  dst[n + 2] = bor(0x80, band(rshift(cp, 6), 63)); dst[n + 3] = bor(0x80, band(cp, 63)); return n + 4
end

local function hex4(p, i)
  return bor(lshift(hex(p[i]), 12), lshift(hex(p[i + 1]), 8), lshift(hex(p[i + 2]), 4), hex(p[i + 3]))
end

-- Decodes the (already validated) string body p[s .. e) into dst; returns the length. dst needs
-- room for e - s bytes: decoding never grows a string.
local function decode(p, s, e, dst)
  local n, i = 0, s
  while i < e do
    local c = p[i]
    if c ~= 92 then
      dst[n] = c; n = n + 1; i = i + 1
    else
      c = p[i + 1]
      if c == 117 then
        local cp = hex4(p, i + 2)
        i = i + 6
        if cp >= 0xD800 and cp <= 0xDBFF and i + 5 < e and p[i] == 92 and p[i + 1] == 117 then
          local lo = hex4(p, i + 2)
          if lo >= 0xDC00 and lo <= 0xDFFF then
            cp = 0x10000 + lshift(cp - 0xD800, 10) + (lo - 0xDC00)
            i = i + 6
          end
        end
        if cp >= 0xD800 and cp <= 0xDFFF then cp = 0xFFFD end   -- a lone surrogate, as UTF-8 stores it
        n = put_utf8(dst, n, cp)
      else
        if c == 110 then c = 10 elseif c == 116 then c = 9 elseif c == 114 then c = 13
        elseif c == 98 then c = 8 elseif c == 102 then c = 12 end
        dst[n] = c; n = n + 1; i = i + 2
      end
    end
  end
  return n
end
M.decode = decode

-- Validates the document p[0 .. n) (p[n] must be 0). `keys` is an array of Lua strings; the value
-- of each as a top-level member is recorded in vtype/vstart/vend/vesc (the last duplicate wins).
-- Returns the type of the top-level value, or 0 if the document is malformed.
function M.scan(p, n, keys, nkeys)
  P = p; PN = n
  for k = 0, nkeys - 1 do vtype[k] = 0 end
  local i = ws(0)
  if P[i] ~= 123 then
    i = value(i, 0)
    if i < 0 then return 0 end
    local t = v_type
    if ws(i) ~= n then return 0 end
    return t
  end
  i = ws(i + 1)
  if P[i] == 125 then
    i = i + 1
  else
    while true do
      if P[i] ~= 34 then return 0 end
      local ks = i + 1
      local ke = str(ks)
      if ke < 0 then return 0 end
      local kesc = s_esc
      i = ws(ke + 1)
      if P[i] ~= 58 then return 0 end
      i = ws(i + 1)
      local vs = i
      i = value(i, 1)
      if i < 0 then return 0 end
      local t, ve, esc = v_type, i, s_esc
      -- is this one of the wanted keys?
      local kp, kl = P + ks, ke - ks
      if kesc then
        if kl <= 64 then kl = decode(P, ks, ke, keybuf); kp = keybuf else kl = -1 end
      end
      for k = 0, nkeys - 1 do
        local w = keys[k + 1]
        if #w == kl and ffi.C.memcmp(kp, w, kl) == 0 then
          vtype[k] = t
          if t == T_STRING then
            vstart[k] = vs + 1; vend[k] = ve - 1; vesc[k] = esc and 1 or 0
          else
            vstart[k] = vs; vend[k] = ve
          end
        end
      end
      i = ws(i)
      local c = P[i]
      if c == 44 then i = ws(i + 1)
      elseif c == 125 then i = i + 1; break
      else return 0 end
    end
  end
  if ws(i) ~= n then return 0 end
  return T_OBJECT
end

local HEX = "0123456789abcdef"
local hexb = ffi.new("uint8_t[16]")
ffi.copy(hexb, HEX, 16)

-- Appends src[s .. s+n) to dst at `o` as the inside of a JSON string, escaping what
-- JSON.stringify escapes. Returns the new offset. Worst case 6 output bytes per input byte.
-- Buffers and offsets, never a derived pointer: nothing here allocates.
local function escape_slow(dst, o, src, s, n)
  for i = s, s + n - 1 do
    local c = src[i]
    if c >= 32 and c ~= 34 and c ~= 92 then
      dst[o] = c; o = o + 1
    else
      dst[o] = 92
      if c == 34 or c == 92 then dst[o + 1] = c; o = o + 2
      elseif c == 10 then dst[o + 1] = 110; o = o + 2
      elseif c == 13 then dst[o + 1] = 114; o = o + 2
      elseif c == 9 then dst[o + 1] = 116; o = o + 2
      elseif c == 8 then dst[o + 1] = 98; o = o + 2
      elseif c == 12 then dst[o + 1] = 102; o = o + 2
      else
        dst[o + 1] = 117; dst[o + 2] = 48; dst[o + 3] = 48
        dst[o + 4] = hexb[rshift(c, 4)]; dst[o + 5] = hexb[band(c, 15)]
        o = o + 6
      end
    end
  end
  return o
end


-- The fast path. Almost every string needs no escaping, so find the clean prefix with one call
-- into the shim (a byte scan, eight bytes at a time) and copy it in one piece; the loop above
-- takes whatever follows the first byte that needs escaping.
--
-- This was Lua for a while: overlapping 64-bit words in generated straight-line code, branching
-- on the length. It was fast in isolation, but inlined into the feed loop its branches became
-- side traces that LuaJIT could not link back to the loop ("NYI: register coalescing too
-- complex"). It kept retrying, and whether the hot loop ended up compiled was a matter of luck:
-- the same build ran at 22 or at 60-100 us per request. A call into C is a fixed boundary.
function M.init(lib) shim = lib end
function M.escape(dst, o, src, s, n)
  local k = shim.td_json_clean(src + s, n)
  if k > 0 then ffi.copy(dst + o, src + s, k) end
  if k == n then return o + n end
  return escape_slow(dst, o + k, src, s + k, n - k)
end

-- Appends a non-negative integer (below 2^53) in decimal. Returns the new offset.
-- Up to eight digits take straight-line code, two digits at a time from a table: no loop, and a
-- multiplication where a division would be (v * 0.01 floors to v / 100 exactly below 2^32).
local digits = ffi.new("uint8_t[24]")
local D2 = ffi.new("uint8_t[200]")
for i = 0, 99 do D2[2 * i] = 48 + math.floor(i / 10); D2[2 * i + 1] = 48 + i % 10 end
local floor = math.floor
local function uint_slow(dst, o, v)
  local n = 0
  while v > 0 do
    local q = floor(v / 10)
    digits[n] = 48 + (v - q * 10); n = n + 1
    v = q
  end
  for k = n - 1, 0, -1 do dst[o] = digits[k]; o = o + 1 end
  return o
end
function M.uint(dst, o, v)
  if v < 10 then dst[o] = 48 + v; return o + 1 end
  if v < 100 then dst[o] = D2[2 * v]; dst[o + 1] = D2[2 * v + 1]; return o + 2 end
  if v >= 100000000 then return uint_slow(dst, o, v) end
  local lo = v
  if v >= 10000 then
    -- the leading one to four digits, then exactly four more
    local hi = floor(v * 0.0001)
    lo = v - hi * 10000
    if hi < 100 then
      if hi < 10 then dst[o] = 48 + hi; o = o + 1
      else dst[o] = D2[2 * hi]; dst[o + 1] = D2[2 * hi + 1]; o = o + 2 end
    else
      local q = floor(hi * 0.01)
      local r = hi - q * 100
      if q < 10 then dst[o] = 48 + q; o = o + 1
      else dst[o] = D2[2 * q]; dst[o + 1] = D2[2 * q + 1]; o = o + 2 end
      dst[o] = D2[2 * r]; dst[o + 1] = D2[2 * r + 1]; o = o + 2
    end
    local q = floor(lo * 0.01)
    local r = lo - q * 100
    dst[o] = D2[2 * q]; dst[o + 1] = D2[2 * q + 1]; dst[o + 2] = D2[2 * r]; dst[o + 3] = D2[2 * r + 1]
    return o + 4
  end
  -- 100 .. 9999
  local q = floor(lo * 0.01)
  local r = lo - q * 100
  if q < 10 then dst[o] = 48 + q; o = o + 1
  else dst[o] = D2[2 * q]; dst[o + 1] = D2[2 * q + 1]; o = o + 2 end
  dst[o] = D2[2 * r]; dst[o + 1] = D2[2 * r + 1]
  return o + 2
end

return M
