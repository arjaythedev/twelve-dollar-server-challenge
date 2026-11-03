# HS256 JWT verification with no allocation. The HMAC key is fixed for the life of the
# process, so the inner and outer SHA-256 states (key ^ ipad, key ^ opad) are computed once
# and copied per token: every verification saves two compression rounds.
module Jwt
  OK          = 0
  MISSING     = 1 # no header or not "Bearer "
  INVALID     = 2 # signature, alg, exp, shape
  BAD_PAYLOAD = 3 # sub / username

  # Heap-allocated: a C struct held directly in a class var lost its last word here.
  @@inner = Pointer(LibSha::ShaCtx).null
  @@outer = Pointer(LibSha::ShaCtx).null
  @@scratch = Pointer(UInt8).null
  @@sig = Pointer(UInt8).null
  @@mac = Pointer(UInt8).null

  B64 = begin
    t = StaticArray(Int8, 256).new(-1_i8)
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".each_byte.each_with_index { |b, i| t[b] = i.to_i8 }
    t
  end

  def self.init(secret : String) : Nil
    key = StaticArray(UInt8, 64).new(0_u8)
    if secret.bytesize > 64
      c = LibSha::ShaCtx.new
      LibSha.sha256_init(pointerof(c))
      LibSha.sha256_update(pointerof(c), secret.to_unsafe, secret.bytesize)
      LibSha.sha256_final(pointerof(key).as(UInt8*), pointerof(c))
    else
      pointerof(key).as(UInt8*).copy_from(secret.to_unsafe, secret.bytesize)
    end
    ipad = StaticArray(UInt8, 64).new { |i| key[i] ^ 0x36_u8 }
    opad = StaticArray(UInt8, 64).new { |i| key[i] ^ 0x5c_u8 }
    @@inner = LibC.malloc(sizeof(LibSha::ShaCtx)).as(LibSha::ShaCtx*)
    @@outer = LibC.malloc(sizeof(LibSha::ShaCtx)).as(LibSha::ShaCtx*)
    LibSha.sha256_init(@@inner)
    LibSha.sha256_update(@@inner, pointerof(ipad).as(UInt8*), 64)
    LibSha.sha256_init(@@outer)
    LibSha.sha256_update(@@outer, pointerof(opad).as(UInt8*), 64)
    @@scratch = LibC.malloc(8192).as(UInt8*)
    @@sig = LibC.malloc(64).as(UInt8*)
    @@mac = LibC.malloc(64).as(UInt8*)
  end

  # Decodes unpadded base64url. Returns the length, or -1.
  def self.b64dec(p : UInt8*, n : Int32, dst : UInt8*) : Int32
    return -1 if n & 3 == 1
    t = B64.to_unsafe
    o = 0
    i = 0
    while i &+ 4 <= n
      a = t[p[i]]; b = t[p[i &+ 1]]; c = t[p[i &+ 2]]; d = t[p[i &+ 3]]
      return -1 if (a | b | c | d) < 0
      v = (a.to_u32 << 18) | (b.to_u32 << 12) | (c.to_u32 << 6) | d.to_u32
      dst[o] = (v >> 16).to_u8!; dst[o &+ 1] = (v >> 8).to_u8!; dst[o &+ 2] = v.to_u8!
      o &+= 3
      i &+= 4
    end
    r = n &- i
    if r == 2
      a = t[p[i]]; b = t[p[i &+ 1]]
      return -1 if (a | b) < 0
      dst[o] = ((a.to_u32 << 2) | (b.to_u32 >> 4)).to_u8!
      o &+= 1
    elsif r == 3
      a = t[p[i]]; b = t[p[i &+ 1]]; c = t[p[i &+ 2]]
      return -1 if (a | b | c) < 0
      v = (a.to_u32 << 18) | (b.to_u32 << 12) | (c.to_u32 << 6)
      dst[o] = (v >> 16).to_u8!; dst[o &+ 1] = (v >> 8).to_u8!
      o &+= 2
    end
    o
  end

  # Result of a successful verification. `name` points into the scratch buffer and stays
  # valid until the next verify call: it is the raw (JSON-escaped) username.
  struct Claims
    property uid : Int64 = 0
    property name : UInt8* = Pointer(UInt8).null
    property name_len : Int32 = 0
  end

  # p/n: the full Authorization header value. now_s: unix seconds.
  def self.verify(p : UInt8*, n : Int32, now_s : Int64, claims : Claims*, prefix : String = "Bearer ") : Int32
    pl = prefix.bytesize
    return MISSING if n < pl || p.memcmp(prefix.to_unsafe, pl) != 0
    p += pl
    n -= pl
    while n > 0 && p[n &- 1] == 0x20
      n &-= 1
    end
    return INVALID if n > 4096
    d1 = LibC.memchr(p, '.'.ord, n).as(UInt8*)
    return INVALID if d1.null?
    hl = (d1 - p).to_i32
    rest = n &- hl &- 1
    d2 = LibC.memchr(d1 + 1, '.'.ord, rest).as(UInt8*)
    return INVALID if d2.null?
    signed_len = (d2 - p).to_i32
    sig_len = n &- signed_len &- 1
    return INVALID if sig_len != 43

    # signature first: cheapest rejection of garbage
    sig = @@sig
    mac = @@mac
    return INVALID if b64dec(d2 + 1, 43, sig) != 32
    c = @@inner.value
    LibSha.sha256_update(pointerof(c), p, signed_len)
    LibSha.sha256_final(mac, pointerof(c))
    c = @@outer.value
    LibSha.sha256_update(pointerof(c), mac, 32)
    LibSha.sha256_final(mac, pointerof(c))
    diff = 0_u8
    32.times { |i| diff |= mac[i] ^ sig[i] }
    return INVALID if diff != 0

    # header: {"alg":"HS256",...}
    s = @@scratch
    hlen = b64dec(p, hl, s)
    return INVALID if hlen < 0
    f = uninitialized Json::Field[3]
    f[0] = Json::Field.new; f[1] = Json::Field.new; f[2] = Json::Field.new
    pr = Json::Parser.new(s, hlen)
    return INVALID if pr.top("alg", "", "", pointerof(f).as(Json::Field*)) != Json::OK
    return INVALID unless f[0].kind == Json::KIND_STRING && f[0].len == 5 && (s + f[0].start).memcmp("HS256".to_unsafe, 5) == 0

    # payload
    plen = b64dec(d1 + 1, signed_len &- hl &- 1, s)
    return INVALID if plen < 0
    f[0] = Json::Field.new; f[1] = Json::Field.new; f[2] = Json::Field.new
    pr = Json::Parser.new(s, plen)
    return INVALID if pr.top("exp", "sub", "username", pointerof(f).as(Json::Field*)) != Json::OK
    e = f[0]
    return INVALID unless e.kind == Json::KIND_NUMBER
    exp = 0_i64
    k = e.start
    neg = s[k] == '-'.ord
    k &+= 1 if neg
    while k < e.start &+ e.len && s[k] >= 0x30 && s[k] <= 0x39
      exp = exp &* 10 &+ (s[k] &- 0x30)
      k &+= 1
      return INVALID if exp > 1_000_000_000_000_i64
    end
    exp = -exp if neg
    return INVALID if exp <= now_s

    sb = f[1]
    return BAD_PAYLOAD unless sb.kind == Json::KIND_STRING && !sb.escaped && sb.len > 0 && sb.len <= 18
    uid = 0_i64
    sb.len.times do |j|
      ch = s[sb.start &+ j]
      return BAD_PAYLOAD unless ch >= 0x30 && ch <= 0x39
      uid = uid &* 10 &+ (ch &- 0x30)
    end
    return BAD_PAYLOAD if uid <= 0
    u = f[2]
    return BAD_PAYLOAD unless u.kind == Json::KIND_STRING
    cl = claims.value
    cl.uid = uid
    cl.name = s + u.start
    cl.name_len = u.len
    claims.value = cl
    OK
  end
end
