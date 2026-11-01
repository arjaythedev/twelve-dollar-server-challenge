# HTTP/1.1 request framing: one pass over the header block, picking out only the headers
# the API uses. Nothing is copied: every field points into the receive buffer.
module Http
  GET   = 1
  POST  = 2
  OTHER = 3

  NEED_MORE = 0
  BAD       = -1

  MAX_HEADER = 16384
  MAX_BODY   = 1 << 20

  struct Req
    property method : Int32 = 0
    property path : UInt8* = Pointer(UInt8).null
    property path_len : Int32 = 0
    property auth : UInt8* = Pointer(UInt8).null
    property auth_len : Int32 = -1
    property body : UInt8* = Pointer(UInt8).null
    property body_len : Int32 = 0
    property gzip : Bool = false
    property close : Bool = false
  end

  @[AlwaysInline]
  def self.ieq(p : UInt8*, s : String) : Bool
    l = s.bytesize
    q = s.to_unsafe
    i = 0
    while i < l
      return false if (p[i] | 0x20) != q[i]
      i &+= 1
    end
    true
  end

  @[AlwaysInline]
  private def self.value(p : UInt8*, e : UInt8*) : {UInt8*, Int32}
    while p < e && (p.value == 0x20 || p.value == 0x09)
      p += 1
    end
    while e > p && (e[-1] == 0x20 || e[-1] == 0x09)
      e -= 1
    end
    {p, (e - p).to_i32}
  end

  # Parses one request at p[0, n). Returns bytes consumed, NEED_MORE or BAD.
  def self.parse(p : UInt8*, n : Int32, r : Req*) : Int32
    he = LibSys.memmem(p, n, "\r\n\r\n".to_unsafe, 4).as(UInt8*)
    if he.null?
      return n > MAX_HEADER ? BAD : NEED_MORE
    end
    hlen = (he - p).to_i32 &+ 4
    req = Req.new
    # request line
    q = p
    e = he
    sp = LibC.memchr(q, 0x20, e - q).as(UInt8*)
    return BAD if sp.null?
    ml = (sp - q).to_i32
    req.method = if ml == 3 && q.memcmp("GET".to_unsafe, 3) == 0
                   GET
                 elsif ml == 4 && q.memcmp("POST".to_unsafe, 4) == 0
                   POST
                 else
                   OTHER
                 end
    q = sp + 1
    le = LibC.memchr(q, 0x0D, e - q).as(UInt8*)
    le = e if le.null?
    sp = LibC.memchr(q, 0x20, le - q).as(UInt8*)
    return BAD if sp.null?
    req.path = q
    qm = LibC.memchr(q, '?'.ord, sp - q).as(UInt8*)
    req.path_len = ((qm.null? ? sp : qm) - q).to_i32
    req.close = (le - sp) == 9 && (sp + 1).memcmp("HTTP/1.0".to_unsafe, 8) == 0
    cl = 0
    # headers
    q = le
    while q < e
      q += 2 # \r\n
      le = LibC.memchr(q, 0x0D, e - q).as(UInt8*)
      le = e if le.null?
      len = le - q
      c = q.value | 0x20
      if c == 'c'.ord
        if len > 15 && ieq(q, "content-length:")
          v, vl = value(q + 15, le)
          vl.times do |i|
            d = v[i]
            return BAD unless d >= 0x30 && d <= 0x39
            cl = cl &* 10 &+ (d &- 0x30)
            return BAD if cl > MAX_BODY
          end
        elsif len > 11 && ieq(q, "connection:")
          v, vl = value(q + 11, le)
          if vl == 5 && ieq(v, "close")
            req.close = true
          elsif vl == 10 && ieq(v, "keep-alive")
            req.close = false
          end
        end
      elsif c == 'a'.ord
        if len >= 14 && ieq(q, "authorization:")
          v, vl = value(q + 14, le)
          req.auth = v
          req.auth_len = vl
        elsif len > 16 && ieq(q, "accept-encoding:")
          v, vl = value(q + 16, le)
          req.gzip = !LibSys.memmem(v, vl, "gzip".to_unsafe, 4).null?
        end
      end
      q = le
    end
    total = hlen &+ cl
    return NEED_MORE if total > n
    req.body = p + hlen
    req.body_len = cl
    r.value = req
    total
  end
end
