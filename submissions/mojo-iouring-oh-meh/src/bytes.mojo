# Raw byte helpers. Everything on the request path works on Bytes: a plain
# pointer into memory that is either C-owned (provided buffers, SQLite pages)
# or leaked once at startup and never freed.
from std.ffi import c_int, external_call
from std.memory import alloc, Layout

comptime Bytes = Pointer[UInt8, MutUntrackedOrigin]


@always_inline
def balloc(n: Int) -> Bytes:
    var a = alloc(Layout[UInt8](count=n))
    return Bytes(unsafe_from_address=Int(a^.unsafe_leak()))


@always_inline
def bzero(p: Bytes, n: Int):
    _ = external_call["memset", Int](p, c_int(0), n)


@always_inline
def bcopy(dst: Bytes, src: Bytes, n: Int):
    _ = external_call["memcpy", Int](dst, src, n)


@always_inline
def bat(addr: Int) -> Bytes:
    return Bytes(unsafe_from_address=addr)


def alloc_u8(n: Int) -> Pointer[UInt8, MutUntrackedOrigin]:
    var p = balloc(n)
    bzero(p, n)
    return p


def alloc_u32(n: Int) -> Pointer[UInt32, MutUntrackedOrigin]:
    var a = alloc(Layout[UInt32](count=n))
    var p = Pointer[UInt32, MutUntrackedOrigin](unsafe_from_address=Int(a^.unsafe_leak()))
    _ = external_call["memset", Int](p, c_int(0), n * 4)
    return p


def alloc_i64(n: Int) -> Pointer[Int64, MutUntrackedOrigin]:
    var a = alloc(Layout[Int64](count=n))
    var p = Pointer[Int64, MutUntrackedOrigin](unsafe_from_address=Int(a^.unsafe_leak()))
    _ = external_call["memset", Int](p, c_int(0), n * 8)
    return p


def alloc_int(n: Int) -> Pointer[Int, MutUntrackedOrigin]:
    var a = alloc(Layout[Int](count=n))
    var p = Pointer[Int, MutUntrackedOrigin](unsafe_from_address=Int(a^.unsafe_leak()))
    _ = external_call["memset", Int](p, c_int(0), n * 8)
    return p


@always_inline
def beq(a: Bytes, b: Bytes, n: Int) -> Bool:
    return external_call["memcmp", c_int](a, b, n) == 0


# A string literal's bytes, for comparisons and constant output.
@always_inline
def lit(s: StringSpan[ImmStaticOrigin]) -> Bytes:
    return Bytes(unsafe_from_address=Int(s.unsafe_ptr()))


@always_inline
def eqlit(p: Bytes, n: Int, s: StringSpan[ImmStaticOrigin]) -> Bool:
    if n != s.byte_length():
        return False
    return beq(p, lit(s), n)


@always_inline
def lower(b: UInt8) -> UInt8:
    if b >= 0x41 and b <= 0x5A:
        return b + 0x20
    return b


# Case-insensitive compare of p[0:n] with an all-lowercase literal.
def eqlit_ci(p: Bytes, n: Int, s: StringSpan[ImmStaticOrigin]) -> Bool:
    if n != s.byte_length():
        return False
    var q = lit(s)
    for i in range(n):
        if lower(p[unsafe_offset=i]) != q[unsafe_offset=i]:
            return False
    return True


# Index of the first CRLF at or after `start`, or -1.
def find_crlf(p: Bytes, n: Int, start: Int) -> Int:
    var i = start
    while i + 1 < n:
        if p[unsafe_offset=i] == 13 and p[unsafe_offset=i + 1] == 10:
            return i
        i += 1
    return -1


# Parse a positive decimal integer with no sign, no leading zero, at most 18 digits.
# Returns -1 when the bytes aren't exactly that.
def parse_pos_int(p: Bytes, n: Int) -> Int64:
    if n == 0 or n > 18:
        return -1
    if p[unsafe_offset=0] == 0x30:
        return -1
    var v: Int64 = 0
    for i in range(n):
        var c = p[unsafe_offset=i]
        if c < 0x30 or c > 0x39:
            return -1
        v = v * 10 + Int64(c - 0x30)
    return v


# Parse a non-negative decimal integer (leading zeros allowed), -1 if not digits.
def parse_uint(p: Bytes, n: Int) -> Int64:
    if n == 0 or n > 18:
        return -1
    var v: Int64 = 0
    for i in range(n):
        var c = p[unsafe_offset=i]
        if c < 0x30 or c > 0x39:
            return -1
        v = v * 10 + Int64(c - 0x30)
    return v


@always_inline
def is_ws(b: UInt8) -> Bool:
    return b == 0x20 or b == 0x09 or b == 0x0A or b == 0x0D or b == 0x0B or b == 0x0C


# Sequential writer over a Bytes region.
struct Out:
    var p: Bytes
    var n: Int
    var cap: Int

    def __init__(out self, p: Bytes, cap: Int):
        self.p = p
        self.n = 0
        self.cap = cap

    @always_inline
    def put(mut self, b: UInt8):
        self.p[unsafe_offset=self.n] = b
        self.n += 1

    @always_inline
    def put_bytes(mut self, src: Bytes, n: Int):
        bcopy(self.p.unsafe_offset(self.n), src, n)
        self.n += n

    @always_inline
    def put_lit(mut self, s: StringSpan[ImmStaticOrigin]):
        self.put_bytes(lit(s), s.byte_length())

    def put_int(mut self, v: Int64):
        if v < 0:
            self.put(0x2D)
            self.put_int(-v)
            return
        if v >= 10:
            self.put_int(v // 10)
        self.put(UInt8(0x30 + (v % 10)))

    # JSON string body (without quotes): escapes " \ and control characters only.
    # Runs of clean bytes are found 8 at a time (SWAR) and copied with memcpy.
    def put_json(mut self, src: Bytes, n: Int):
        var i = 0
        while i < n:
            # find the end of the clean run starting at i
            var j = i
            while j + 8 <= n:
                var w = src.unsafe_offset(j).unsafe_bitcast[UInt64]()[unsafe_offset=0]
                # bytes < 0x20, == '"' (0x22) or == '\\' (0x5C) set the high bit of their lane
                var lt20 = (w - 0x2020202020202020) & ~w
                var q = w ^ 0x2222222222222222
                var isq = (q - 0x0101010101010101) & ~q
                var b = w ^ 0x5C5C5C5C5C5C5C5C
                var isb = (b - 0x0101010101010101) & ~b
                if ((lt20 | isq | isb) & 0x8080808080808080) != 0:
                    break
                j += 8
            while j < n:
                var c = src[unsafe_offset=j]
                if c < 0x20 or c == 0x22 or c == 0x5C:
                    break
                j += 1
            if j > i:
                self.put_bytes(src.unsafe_offset(i), j - i)
                i = j
            if i >= n:
                break
            var c = src[unsafe_offset=i]
            if c == 0x22:
                self.put(0x5C)
                self.put(0x22)
            elif c == 0x5C:
                self.put(0x5C)
                self.put(0x5C)
            elif c == 0x0A:
                self.put(0x5C)
                self.put(0x6E)
            elif c == 0x0D:
                self.put(0x5C)
                self.put(0x72)
            elif c == 0x09:
                self.put(0x5C)
                self.put(0x74)
            elif c == 0x08:
                self.put(0x5C)
                self.put(0x62)
            elif c == 0x0C:
                self.put(0x5C)
                self.put(0x66)
            else:
                self.put_lit("\\u00")
                var hi = c >> 4
                var lo = c & 0xF
                self.put(UInt8(0x30 + hi) if hi < 10 else UInt8(0x57 + hi))
                self.put(UInt8(0x30 + lo) if lo < 10 else UInt8(0x57 + lo))
            i += 1


# Copy a String's bytes into leaked memory (NUL-terminated). For data that must
# outlive the String, since taking a raw address does not keep the String alive.
def leak_string(s: String) -> Bytes:
    var n = s.byte_length()
    var p = balloc(n + 1)
    bcopy(p, Bytes(unsafe_from_address=Int(s.unsafe_ptr())), n)
    p[unsafe_offset=n] = 0
    return p
