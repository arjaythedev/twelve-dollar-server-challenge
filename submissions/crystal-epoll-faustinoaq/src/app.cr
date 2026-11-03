# The API: fixed responses, routing, validation. Shared by both I/O backends.
module Resp
  JSON_HDR = "\r\nContent-Type: application/json\r\nContent-Length:       \r\n\r\n"
  CL_PAD   = 7 # digits right-aligned in the spaces before the final CRLFCRLF

  macro fixed(status, body)
    "HTTP/1.1 " + {{status}} + "\r\nContent-Type: application/json\r\nContent-Length: " + {{body}}.bytesize.to_s + "\r\n\r\n" + {{body}}
  end

  NOT_FOUND      = fixed("404 Not Found", %({"error":"not found"}))
  BAD_ID         = fixed("400 Bad Request", %({"error":"invalid post id"}))
  POST_NOT_FOUND = fixed("404 Not Found", %({"error":"post not found"}))
  MISSING_TOKEN  = fixed("401 Unauthorized", %({"error":"missing bearer token"}))
  INVALID_TOKEN  = fixed("401 Unauthorized", %({"error":"invalid or expired token"}))
  BAD_PAYLOAD    = fixed("401 Unauthorized", %({"error":"invalid token payload"}))
  MALFORMED      = fixed("400 Bad Request", %({"error":"malformed JSON body"}))
  BODY_REQUIRED  = fixed("400 Bad Request", %({"error":"body is required"}))
  BODY_TOO_LONG  = fixed("400 Bad Request", %({"error":"body must be at most 500 characters"}))
  INTERNAL       = fixed("500 Internal Server Error", %({"error":"internal server error"}))
  BAD_REQUEST    = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

  # Writes the status line and headers with a blank Content-Length; returns the body start.
  @[AlwaysInline]
  def self.open(o : Buf, status : String) : Int32
    o << "HTTP/1.1 " << status << JSON_HDR
    o.len
  end

  @[AlwaysInline]
  def self.close(o : Buf, body_start : Int32) : Nil
    n = o.len &- body_start
    p = o.ptr + body_start - 4 - 1 # last pad byte
    loop do
      p.value = 48_u8 &+ (n % 10).to_u8!
      n //= 10
      break if n == 0
      p -= 1
    end
  end
end

module Router
  # Parses "/posts/<id>" style ids: a positive integer, else -1.
  def self.parse_id(p : UInt8*, n : Int32) : Int64
    return -1_i64 if n == 0 || n > 18
    v = 0_i64
    n.times do |i|
      c = p[i]
      return -1_i64 unless c >= 0x30 && c <= 0x39
      v = v &* 10 &+ (c &- 0x30)
    end
    v > 0 ? v : -1_i64
  end

  @[AlwaysInline]
  def self.auth(r : Http::Req*, o : Buf) : Bool
    rv = r.value
    if rv.auth_len < 0
      o << Resp::MISSING_TOKEN
      return false
    end
    case Jwt.verify(rv.auth, rv.auth_len, Server.now_s, Server.claims)
    when Jwt::OK then return true
    when Jwt::MISSING then o << Resp::MISSING_TOKEN
    when Jwt::INVALID then o << Resp::INVALID_TOKEN
    else o << Resp::BAD_PAYLOAD
    end
    false
  end

  def self.handle(r : Http::Req*, o : Buf) : Nil
    rv = r.value
    p = rv.path
    n = rv.path_len
    if rv.method == Http::GET
      if n == 5 && p.memcmp("/feed".to_unsafe, 5) == 0
        Db.feed(o)
      elsif n > 7 && p.memcmp("/posts/".to_unsafe, 7) == 0
        id = parse_id(p + 7, n &- 7)
        if id < 0
          if LibC.memchr(p + 7, '/'.ord, n &- 7).null?
            o << Resp::BAD_ID
          else
            o << Resp::NOT_FOUND
          end
        else
          Db.post(o, id)
        end
      elsif n == 7 && p.memcmp("/health".to_unsafe, 7) == 0
        Db.health(o, Server.uptime)
      else
        o << Resp::NOT_FOUND
      end
    elsif rv.method == Http::POST
      if n == 6 && p.memcmp("/posts".to_unsafe, 6) == 0
        create(r, o)
      elsif n > 12 && p.memcmp("/posts/".to_unsafe, 7) == 0 && (p + n - 5).memcmp("/like".to_unsafe, 5) == 0
        return unless auth(r, o)
        id = parse_id(p + 7, n &- 12)
        if id < 0
          o << Resp::BAD_ID
        else
          Db.like(o, Server.claims.value.uid, id)
        end
      else
        o << Resp::NOT_FOUND
      end
    else
      o << Resp::NOT_FOUND
    end
  end

  WS = begin
    t = StaticArray(Bool, 256).new(false)
    {9, 10, 11, 12, 13, 32}.each { |c| t[c] = true }
    t
  end

  def self.create(r : Http::Req*, o : Buf) : Nil
    return unless auth(r, o)
    rv = r.value
    f = uninitialized Json::Field[3]
    f[0] = Json::Field.new; f[1] = Json::Field.new; f[2] = Json::Field.new
    pr = Json::Parser.new(rv.body, rv.body_len)
    st = pr.top("body", "", "", pointerof(f).as(Json::Field*))
    if st == Json::MALFORMED
      o << Resp::MALFORMED
      return
    end
    b = f[0]
    if st != Json::OK || b.kind != Json::KIND_STRING
      o << Resp::BODY_REQUIRED
      return
    end
    s = rv.body + b.start
    l = b.len
    if b.escaped
      l = Json.unescape(s, l, Server.scratch)
      s = Server.scratch
    end
    while l > 0 && WS[s.value]
      s += 1
      l &-= 1
    end
    while l > 0 && WS[s[l &- 1]]
      l &-= 1
    end
    if l == 0
      o << Resp::BODY_REQUIRED
      return
    end
    if l > 500
      chars = 0
      l.times { |i| chars &+= 1 if s[i] & 0xC0 != 0x80 }
      if chars > 500
        o << Resp::BODY_TOO_LONG
        return
      end
    end
    cl = Server.claims.value
    Db.create(o, cl.uid, s, l, cl.name, cl.name_len)
  end
end

