require "rayo"
require "./database"

module Feed
  KEY     = Rayo::JWT::Key.new(ENV["JWT_SECRET"])
  CLAIMS  = Bytes.new(16384)
  NAME    = Bytes.new(16384)
  SUBJECT = Bytes.new(16384)
  BODY    = Bytes.new(16384)
  class_property database : Database? = nil

  def self.db
    database.not_nil!
  end

  def self.error(ctx, code, message)
    ctx.status code
    ctx.json error: message
  end

  def self.positive(bytes) : Int64?
    return nil if bytes.empty?
    n = 0_i64
    bytes.each do |c|
      return nil unless 48 <= c <= 57
      digit = c - 48
      return nil if n > (Int64::MAX - digit) // 10
      n = n * 10 + digit
    end
    n > 0 ? n : nil
  end

  def self.id(ctx) : Int64?
    value = positive(ctx.param("id"))
    error(ctx, 400, "invalid post id") unless value
    value
  end

  def self.field(reader, name) : Rayo::JSON::Reader?
    found = nil.as(Rayo::JSON::Reader?)
    return nil unless reader.object?
    reader.object { |key, value| found = value if key.string_equals?(name) }
    found
  end

  def self.auth(ctx) : Tuple(Int64, Bytes)?
    header = ctx.header("Authorization")
    unless header.size >= 7 && header[0, 7] == "Bearer ".to_slice
      error(ctx, 401, "missing bearer token")
      return nil
    end
    unless claims = KEY.verify(header[7..], CLAIMS, now: Time.utc.to_unix)
      error(ctx, 401, "invalid or expired token")
      return nil
    end
    unless field(claims, "exp")
      error(ctx, 401, "invalid or expired token")
      return nil
    end
    sub = field(claims, "sub")
    name = field(claims, "username")
    if sub && sub.string? && name && name.string?
      if uid = positive(sub.string(SUBJECT))
        return {uid, name.string(NAME)}
      end
    end
    error(ctx, 401, "invalid token payload")
    nil
  end

  # Reader already validated UTF-8. Trim Unicode whitespace and count code points
  # directly in the decoded scratch slice, without constructing a String.
  def self.trim(bytes) : Tuple(Bytes, Int32)
    first = -1
    last = 0
    count = 0
    kept = 0
    i = 0
    while i < bytes.size
      start = i
      c = bytes[i].to_u32
      i += 1
      if c >= 0xc0
        extra = c < 0xe0 ? 1 : (c < 0xf0 ? 2 : 3)
        c &= (1_u32 << (6 - extra)) - 1
        extra.times { c = (c << 6) | (bytes[i] & 0x3f); i += 1 }
      end
      if !c.unsafe_chr.whitespace?
        first = start if first < 0
        last = i
        kept = count + 1
      end
      count += 1 if first >= 0
    end
    first < 0 ? {Bytes.empty, 0} : {bytes[first, last - first], kept}
  end
end
