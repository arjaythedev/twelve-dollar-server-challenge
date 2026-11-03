# A zero-allocation JSON validator that also picks out a few top-level fields.
# It validates the whole document (so malformed bodies are rejected exactly), and for
# up to three wanted keys records where their value sits in the input.
module Json
  KIND_NONE   = 0
  KIND_STRING = 1 # start/len cover the bytes between the quotes (still escaped)
  KIND_NUMBER = 2
  KIND_OTHER  = 3

  struct Field
    property kind : Int32 = KIND_NONE
    property start : Int32 = 0
    property len : Int32 = 0
    property escaped : Bool = false
  end

  MALFORMED  = -1
  NOT_OBJECT =  0
  OK         =  1

  struct Parser
    @p : UInt8*
    @n : Int32
    @i : Int32 = 0
    @esc : Bool = false

    def initialize(@p, @n)
    end

    @[AlwaysInline]
    def ws : Nil
      while @i < @n
        c = @p[@i]
        break unless c == 0x20 || c == 0x0A || c == 0x0D || c == 0x09
        @i &+= 1
      end
    end

    # Returns the value kind, or -1 if malformed.
    def value(depth : Int32) : Int32
      return -1 if @i >= @n || depth > 64
      c = @p[@i]
      case c
      when '"'.ord
        @i &+= 1
        string ? KIND_STRING : -1
      when '{'.ord
        object(depth) ? KIND_OTHER : -1
      when '['.ord
        @i &+= 1
        ws
        if @i < @n && @p[@i] == ']'.ord
          @i &+= 1
          return KIND_OTHER
        end
        loop do
          ws
          return -1 if value(depth &+ 1) < 0
          ws
          return -1 if @i >= @n
          if @p[@i] == ','.ord
            @i &+= 1
          elsif @p[@i] == ']'.ord
            @i &+= 1
            return KIND_OTHER
          else
            return -1
          end
        end
      when 't'.ord then lit("true") ? KIND_OTHER : -1
      when 'f'.ord then lit("false") ? KIND_OTHER : -1
      when 'n'.ord then lit("null") ? KIND_OTHER : -1
      else
        number ? KIND_NUMBER : -1
      end
    end

    def object(depth : Int32) : Bool
      @i &+= 1
      ws
      if @i < @n && @p[@i] == '}'.ord
        @i &+= 1
        return true
      end
      loop do
        ws
        return false if @i >= @n || @p[@i] != '"'.ord
        @i &+= 1
        return false unless string
        ws
        return false if @i >= @n || @p[@i] != ':'.ord
        @i &+= 1
        ws
        return false if value(depth &+ 1) < 0
        ws
        return false if @i >= @n
        if @p[@i] == ','.ord
          @i &+= 1
        elsif @p[@i] == '}'.ord
          @i &+= 1
          return true
        else
          return false
        end
      end
    end

    # Top-level object: records the fields named k0..k2.
    def top(k0 : String, k1 : String, k2 : String, f : Field*) : Int32
      ws
      return MALFORMED if @i >= @n
      if @p[@i] != '{'.ord
        return MALFORMED if value(0) < 0
        ws
        return @i == @n ? NOT_OBJECT : MALFORMED
      end
      @i &+= 1
      ws
      if @i < @n && @p[@i] == '}'.ord
        @i &+= 1
      else
        loop do
          ws
          return MALFORMED if @i >= @n || @p[@i] != '"'.ord
          @i &+= 1
          ks = @i
          return MALFORMED unless string
          kl = @i &- 1 &- ks
          ws
          return MALFORMED if @i >= @n || @p[@i] != ':'.ord
          @i &+= 1
          ws
          vs = @i
          @esc = false
          kind = value(1)
          return MALFORMED if kind < 0
          slot = -1
          slot = 0 if key_is(ks, kl, k0)
          slot = 1 if key_is(ks, kl, k1)
          slot = 2 if key_is(ks, kl, k2)
          if slot >= 0
            fl = Field.new
            fl.kind = kind
            if kind == KIND_STRING
              fl.start = vs &+ 1
              fl.len = @i &- vs &- 2
              fl.escaped = @esc
            else
              fl.start = vs
              fl.len = @i &- vs
            end
            f[slot] = fl
          end
          ws
          return MALFORMED if @i >= @n
          if @p[@i] == ','.ord
            @i &+= 1
          elsif @p[@i] == '}'.ord
            @i &+= 1
            break
          else
            return MALFORMED
          end
        end
      end
      ws
      @i == @n ? OK : MALFORMED
    end

    @[AlwaysInline]
    private def key_is(s : Int32, l : Int32, k : String) : Bool
      l == k.bytesize && l > 0 && (@p + s).memcmp(k.to_unsafe, l) == 0
    end

    private def lit(s : String) : Bool
      l = s.bytesize
      return false if @i &+ l > @n || (@p + @i).memcmp(s.to_unsafe, l) != 0
      @i &+= l
      true
    end

    private def digits : Int32
      s = @i
      while @i < @n && @p[@i] >= 0x30 && @p[@i] <= 0x39
        @i &+= 1
      end
      @i &- s
    end

    private def number : Bool
      @i &+= 1 if @i < @n && @p[@i] == '-'.ord
      return false if @i >= @n
      if @p[@i] == '0'.ord
        @i &+= 1
      else
        return false if digits == 0
      end
      if @i < @n && @p[@i] == '.'.ord
        @i &+= 1
        return false if digits == 0
      end
      if @i < @n && (@p[@i] == 'e'.ord || @p[@i] == 'E'.ord)
        @i &+= 1
        @i &+= 1 if @i < @n && (@p[@i] == '+'.ord || @p[@i] == '-'.ord)
        return false if digits == 0
      end
      true
    end

    # After the opening quote; leaves @i after the closing quote.
    private def string : Bool
      while @i < @n
        c = @p[@i]
        if c == '"'.ord
          @i &+= 1
          return true
        elsif c == '\\'.ord
          @esc = true
          @i &+= 1
          return false if @i >= @n
          e = @p[@i]
          if e == 'u'.ord
            return false if @i &+ 4 >= @n
            4.times do |k|
              return false if Json.hexval(@p[@i &+ 1 &+ k]) < 0
            end
            @i &+= 5
          elsif e == '"'.ord || e == '\\'.ord || e == '/'.ord || e == 'b'.ord || e == 'f'.ord ||
                e == 'n'.ord || e == 'r'.ord || e == 't'.ord
            @i &+= 1
          else
            return false
          end
        elsif c < 0x20
          return false
        else
          @i &+= 1
        end
      end
      false
    end
  end

  @[AlwaysInline]
  def self.hexval(c : UInt8) : Int32
    return c.to_i32 &- 48 if c >= 0x30 && c <= 0x39
    return c.to_i32 &- 87 if c >= 0x61 && c <= 0x66
    return c.to_i32 &- 55 if c >= 0x41 && c <= 0x46
    -1
  end

  # Unescapes an already validated JSON string body into dst (dst needs n bytes). Returns length.
  def self.unescape(p : UInt8*, n : Int32, dst : UInt8*) : Int32
    i = 0
    o = 0
    while i < n
      c = p[i]
      if c != '\\'.ord
        dst[o] = c
        o &+= 1
        i &+= 1
        next
      end
      e = p[i &+ 1]
      i &+= 2
      case e
      when 'n'.ord then dst[o] = 0x0A; o &+= 1
      when 't'.ord then dst[o] = 0x09; o &+= 1
      when 'r'.ord then dst[o] = 0x0D; o &+= 1
      when 'b'.ord then dst[o] = 0x08; o &+= 1
      when 'f'.ord then dst[o] = 0x0C; o &+= 1
      when 'u'.ord
        cp = hex4(p + i)
        i &+= 4
        if cp >= 0xD800 && cp < 0xDC00 && i &+ 6 <= n && p[i] == '\\'.ord && p[i &+ 1] == 'u'.ord
          lo = hex4(p + (i &+ 2))
          if lo >= 0xDC00 && lo < 0xE000
            cp = 0x10000 &+ ((cp &- 0xD800) << 10) &+ (lo &- 0xDC00)
            i &+= 6
          end
        end
        o = utf8(dst, o, cp)
      else
        dst[o] = e
        o &+= 1
      end
    end
    o
  end

  private def self.hex4(p : UInt8*) : Int32
    (hexval(p[0]) << 12) | (hexval(p[1]) << 8) | (hexval(p[2]) << 4) | hexval(p[3])
  end

  private def self.utf8(d : UInt8*, o : Int32, cp : Int32) : Int32
    if cp < 0x80
      d[o] = cp.to_u8!
      o &+ 1
    elsif cp < 0x800
      d[o] = (0xC0 | (cp >> 6)).to_u8!
      d[o &+ 1] = (0x80 | (cp & 0x3F)).to_u8!
      o &+ 2
    elsif cp < 0x10000
      d[o] = (0xE0 | (cp >> 12)).to_u8!
      d[o &+ 1] = (0x80 | ((cp >> 6) & 0x3F)).to_u8!
      d[o &+ 2] = (0x80 | (cp & 0x3F)).to_u8!
      o &+ 3
    else
      d[o] = (0xF0 | (cp >> 18)).to_u8!
      d[o &+ 1] = (0x80 | ((cp >> 12) & 0x3F)).to_u8!
      d[o &+ 2] = (0x80 | ((cp >> 6) & 0x3F)).to_u8!
      d[o &+ 3] = (0x80 | (cp & 0x3F)).to_u8!
      o &+ 4
    end
  end
end
