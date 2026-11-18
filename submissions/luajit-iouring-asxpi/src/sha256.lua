-- SHA-256 and HMAC-SHA256 (HS256) in plain LuaJIT: 32-bit ops from the bit library on FFI arrays.
local ffi = require("ffi")
local bit = require("bit")
local band, bor, bxor, bnot = bit.band, bit.bor, bit.bxor, bit.bnot
local ror, rshift, lshift, tobit = bit.ror, bit.rshift, bit.lshift, bit.tobit

local K = ffi.new("int32_t[64]")
do
  local k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  }
  for i = 0, 63 do K[i] = tobit(k[i + 1]) end
end

local IV = ffi.new("int32_t[8]")
do
  local iv = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }
  for i = 0, 7 do IV[i] = tobit(iv[i + 1]) end
end

local W = ffi.new("int32_t[64]")

-- One 64-byte block at p[off] into state H.
--
-- The 64 rounds are a loop of 8 iterations with 8 rounds written out in each. After 8 rounds the
-- eight working variables are back under their own names, so the usual h = g; g = f; ... shuffle
-- of every round disappears, and the trace of the loop body stays well inside LuaJIT's default
-- limits (all 64 rounds written out are 2.5 times faster too, but only with the trace-length
-- limits raised for the whole process). The body is generated: it is the same two lines eight
-- times with the variables rotated.
local compress
do
  local t = {
    "local bit, K, W = ...",
    "local band, bor, bxor, bnot, ror, rshift, lshift, tobit = bit.band, bit.bor, bit.bxor, bit.bnot, bit.ror, bit.rshift, bit.lshift, bit.tobit",
    "return function(H, p, off)",
    "  for i = 0, 15 do",
    "    local j = off + i * 4",
    "    W[i] = bor(lshift(p[j], 24), lshift(p[j + 1], 16), lshift(p[j + 2], 8), p[j + 3])",
    "  end",
    "  for i = 16, 63 do",
    "    local x, y = W[i - 15], W[i - 2]",
    "    W[i] = tobit(W[i - 16] + bxor(ror(x, 7), ror(x, 18), rshift(x, 3)) + W[i - 7] + bxor(ror(y, 17), ror(y, 19), rshift(y, 10)))",
    "  end",
    "  local a, b, c, d, e, f, g, h = H[0], H[1], H[2], H[3], H[4], H[5], H[6], H[7]",
    "  for i = 0, 56, 8 do",
    "    local t1, t2",
  }
  local v = { "a", "b", "c", "d", "e", "f", "g", "h" }
  for r = 0, 7 do
    local A, B, C, D, E, F, G, Hh = v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]
    t[#t + 1] = ("    t1 = %s + bxor(ror(%s, 6), ror(%s, 11), ror(%s, 25)) + bxor(band(%s, %s), band(bnot(%s), %s)) + K[i + %d] + W[i + %d]")
      :format(Hh, E, E, E, E, F, E, G, r, r)
    t[#t + 1] = ("    t2 = bxor(ror(%s, 2), ror(%s, 13), ror(%s, 22)) + bxor(band(%s, %s), band(%s, %s), band(%s, %s))")
      :format(A, A, A, A, B, A, C, B, C)
    t[#t + 1] = ("    %s = tobit(%s + t1)"):format(D, D)
    t[#t + 1] = ("    %s = tobit(t1 + t2)"):format(Hh)
    v = { Hh, A, B, C, D, E, F, G }      -- the new a is in h's variable, the new e in d's
  end
  assert(v[1] == "a" and v[8] == "h")
  t[#t + 1] = "  end"
  for i = 0, 7 do t[#t + 1] = ("  H[%d] = tobit(H[%d] + %s)"):format(i, i, v[i + 1]) end
  t[#t + 1] = "end"
  compress = assert(loadstring(table.concat(t, "\n"), "=sha256.compress"))(bit, K, W)
end

local tailbuf = ffi.new("uint8_t[128]")

-- finishes a hash: `done` bytes were already compressed into H, then p[off .. off+n) follows
local function finish(H, p, off, n, done)
  local total = done + n
  while n >= 64 do
    compress(H, p, off); off = off + 64; n = n - 64
  end
  ffi.fill(tailbuf, 128)
  if n > 0 then ffi.copy(tailbuf, p + off, n) end
  tailbuf[n] = 0x80
  local blocks = (n < 56) and 64 or 128
  local bits = total * 8                      -- < 2^32 for anything we hash
  tailbuf[blocks - 4] = band(rshift(bits, 24), 255)
  tailbuf[blocks - 3] = band(rshift(bits, 16), 255)
  tailbuf[blocks - 2] = band(rshift(bits, 8), 255)
  tailbuf[blocks - 1] = band(bits, 255)
  compress(H, tailbuf, 0)
  if blocks == 128 then compress(H, tailbuf, 64) end
end

local function put_digest(H, out)
  for i = 0, 7 do
    local v = H[i]
    out[i * 4] = band(rshift(v, 24), 255)
    out[i * 4 + 1] = band(rshift(v, 16), 255)
    out[i * 4 + 2] = band(rshift(v, 8), 255)
    out[i * 4 + 3] = band(v, 255)
  end
end

local M = {}

local Hi, Ho = ffi.new("int32_t[8]"), ffi.new("int32_t[8]")   -- states after the ipad / opad block
local Hs = ffi.new("int32_t[8]")
local inner = ffi.new("uint8_t[32]")

-- The key's inner and outer padded blocks are hashed once, here.
function M.set_key(key)
  local kb = ffi.new("uint8_t[64]")
  if #key > 64 then
    ffi.copy(Hs, IV, 32)
    local kp = ffi.new("uint8_t[?]", #key, key)
    finish(Hs, kp, 0, #key, 0)
    put_digest(Hs, kb)
  else
    ffi.copy(kb, key, #key)
  end
  local blk = ffi.new("uint8_t[64]")
  for i = 0, 63 do blk[i] = bxor(kb[i], 0x36) end
  ffi.copy(Hi, IV, 32); compress(Hi, blk, 0)
  for i = 0, 63 do blk[i] = bxor(kb[i], 0x5c) end
  ffi.copy(Ho, IV, 32); compress(Ho, blk, 0)
end

-- HMAC-SHA256 of p[off .. off+n) into out[0..31]
function M.hmac(p, off, n, out)
  ffi.copy(Hs, Hi, 32)
  finish(Hs, p, off, n, 64)
  put_digest(Hs, inner)
  ffi.copy(Hs, Ho, 32)
  finish(Hs, inner, 0, 32, 64)
  put_digest(Hs, out)
end

return M
