# A growable byte buffer on raw malloc. The GC is compiled out (-Dgc_none), so every
# per-request byte lives in buffers like this one that are allocated once and reused.
class Buf
  getter ptr : UInt8*
  property len : Int32
  getter cap : Int32

  def initialize(@cap : Int32)
    @ptr = LibC.malloc(@cap).as(UInt8*)
    @len = 0
  end

  @[AlwaysInline]
  def clear : Nil
    @len = 0
  end

  @[AlwaysInline]
  def reserve(n : Int32) : UInt8*
    need = @len &+ n
    grow(need) if need > @cap
    @ptr + @len
  end

  @[NoInline]
  private def grow(need : Int32) : Nil
    c = @cap
    while c < need
      c = c &* 2
    end
    @ptr = LibC.realloc(@ptr, c).as(UInt8*)
    @cap = c
  end

  @[AlwaysInline]
  def tail : UInt8*
    @ptr + @len
  end

  @[AlwaysInline]
  def <<(b : UInt8) : self
    reserve(1).value = b
    @len &+= 1
    self
  end

  @[AlwaysInline]
  def write(p : UInt8*, n : Int32) : self
    reserve(n).copy_from(p, n)
    @len &+= n
    self
  end

  # String literals are static data, so this is a plain memcpy.
  @[AlwaysInline]
  def <<(s : String) : self
    write(s.to_unsafe, s.bytesize)
  end

  DIGITS2 = "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899"

  def <<(v : Int64) : self
    if v < 0
      self << '-'.ord.to_u8
      v = 0_i64 &- v
    end
    u = v.to_u64!
    tmp = uninitialized UInt8[20]
    i = 20
    d = DIGITS2.to_unsafe
    while u >= 100
      r = (u % 100).to_i32! &* 2
      u //= 100
      i &-= 2
      pointerof(tmp).as(UInt8*)[i] = d[r]
      pointerof(tmp).as(UInt8*)[i &+ 1] = d[r &+ 1]
    end
    if u >= 10
      r = u.to_i32! &* 2
      i &-= 2
      pointerof(tmp).as(UInt8*)[i] = d[r]
      pointerof(tmp).as(UInt8*)[i &+ 1] = d[r &+ 1]
    else
      i &-= 1
      pointerof(tmp).as(UInt8*)[i] = 48_u8 &+ u.to_u8!
    end
    write(pointerof(tmp).as(UInt8*) + i, 20 &- i)
  end

  def <<(v : Int32) : self
    self << v.to_i64
  end

  HEX = "0123456789abcdef"
  L01 = 0x0101010101010101_u64
  L80 = 0x8080808080808080_u64

  # Escapes a UTF-8 string into a JSON string body (no quotes). 8 bytes at a time (SWAR):
  # a word is copied unchanged unless it holds a control byte, '"' or '\'.
  def json_escape(p : UInt8*, n : Int32) : self
    dst = reserve(n &* 6)
    o = 0
    i = 0
    q = L01 &* 0x22
    bs = L01 &* 0x5C
    while i &+ 8 <= n
      w = (p + i).as(UInt64*).value
      x1 = w ^ q
      x2 = w ^ bs
      hit = ((w &- L01 &* 0x20) & ~w) | ((x1 &- L01) & ~x1) | ((x2 &- L01) & ~x2)
      if hit & L80 == 0
        (dst + o).as(UInt64*).value = w
        o &+= 8
        i &+= 8
      else
        8.times do
          o = esc1(dst, o, p[i])
          i &+= 1
        end
      end
    end
    while i < n
      o = esc1(dst, o, p[i])
      i &+= 1
    end
    @len &+= o
    self
  end

  @[AlwaysInline]
  private def esc1(dst : UInt8*, o : Int32, c : UInt8) : Int32
    if c >= 0x20 && c != 0x22 && c != 0x5C
      dst[o] = c
      return o &+ 1
    end
    dst[o] = 0x5C
    case c
    when 0x22 then dst[o &+ 1] = 0x22; o &+ 2
    when 0x5C then dst[o &+ 1] = 0x5C; o &+ 2
    when 0x0A then dst[o &+ 1] = 'n'.ord.to_u8; o &+ 2
    when 0x0D then dst[o &+ 1] = 'r'.ord.to_u8; o &+ 2
    when 0x09 then dst[o &+ 1] = 't'.ord.to_u8; o &+ 2
    else
      dst[o &+ 1] = 'u'.ord.to_u8
      dst[o &+ 2] = 48_u8
      dst[o &+ 3] = 48_u8
      dst[o &+ 4] = HEX.to_unsafe[c >> 4]
      dst[o &+ 5] = HEX.to_unsafe[c & 15]
      o &+ 6
    end
  end
end
