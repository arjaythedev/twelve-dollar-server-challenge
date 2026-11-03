# A small JSON scanner, used two ways:
#   - JsonScan: find a top-level key in an object and inspect its value
#     (JWT header/payload fields).
#   - validate_body(): validate a whole document and extract the top-level
#     "body" member as an unescaped UTF-8 string (POST /posts).
from bytes import Bytes, eqlit, is_ws, is_digit

comptime MAX_DEPTH = 64


@always_inline
def hexval(c: UInt8) -> Int:
    if c >= 0x30 and c <= 0x39:
        return Int(c - 0x30)
    if c >= 0x61 and c <= 0x66:
        return Int(c - 0x61) + 10
    if c >= 0x41 and c <= 0x46:
        return Int(c - 0x41) + 10
    return -1


# Four hex digits at p[at:at+4] as an int, or -1.
@always_inline
def hex4(p: Bytes, at: Int) -> Int:
    var cp = 0
    for k in range(4):
        var h = hexval(p[unsafe_offset=at + k])
        if h < 0:
            return -1
        cp = (cp << 4) | h
    return cp


# Skip a JSON string starting at p[i] == '"'. Returns the index after the closing
# quote, or -1 if invalid. If out is not null, writes the unescaped bytes to out
# (up to cap) and stores the count in out_len.
def scan_string(p: Bytes, n: Int, start: Int, dst: Bytes, cap: Int, mut out_len: Int) -> Int:
    var i = start + 1
    var o = 0
    var write = cap > 0
    while True:
        if i >= n:
            return -1
        var c = p[unsafe_offset=i]
        if c == 0x22:
            out_len = o
            return i + 1
        if c < 0x20:
            return -1
        if c == 0x5C:
            i += 1
            if i >= n:
                return -1
            var e = p[unsafe_offset=i]
            var b: UInt8
            if e == 0x22 or e == 0x5C or e == 0x2F:
                b = e
            elif e == 0x62:
                b = 0x08
            elif e == 0x66:
                b = 0x0C
            elif e == 0x6E:
                b = 0x0A
            elif e == 0x72:
                b = 0x0D
            elif e == 0x74:
                b = 0x09
            elif e == 0x75:
                if i + 4 >= n:
                    return -1
                var cp = hex4(p, i + 1)
                if cp < 0:
                    return -1
                i += 4
                if cp >= 0xD800 and cp <= 0xDBFF:
                    # high surrogate: needs \uDC00-\uDFFF next
                    if i + 6 >= n or p[unsafe_offset=i + 1] != 0x5C or p[unsafe_offset=i + 2] != 0x75:
                        return -1
                    var lo = hex4(p, i + 3)
                    if lo < 0xDC00 or lo > 0xDFFF:
                        return -1
                    i += 6
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                elif cp >= 0xDC00 and cp <= 0xDFFF:
                    return -1
                if write:
                    if o + 4 > cap:
                        return -1
                    if cp < 0x80:
                        dst[unsafe_offset=o] = UInt8(cp)
                        o += 1
                    elif cp < 0x800:
                        dst[unsafe_offset=o] = UInt8(0xC0 | (cp >> 6))
                        dst[unsafe_offset=o + 1] = UInt8(0x80 | (cp & 0x3F))
                        o += 2
                    elif cp < 0x10000:
                        dst[unsafe_offset=o] = UInt8(0xE0 | (cp >> 12))
                        dst[unsafe_offset=o + 1] = UInt8(0x80 | ((cp >> 6) & 0x3F))
                        dst[unsafe_offset=o + 2] = UInt8(0x80 | (cp & 0x3F))
                        o += 3
                    else:
                        dst[unsafe_offset=o] = UInt8(0xF0 | (cp >> 18))
                        dst[unsafe_offset=o + 1] = UInt8(0x80 | ((cp >> 12) & 0x3F))
                        dst[unsafe_offset=o + 2] = UInt8(0x80 | ((cp >> 6) & 0x3F))
                        dst[unsafe_offset=o + 3] = UInt8(0x80 | (cp & 0x3F))
                        o += 4
                i += 1
                continue
            else:
                return -1
            if write:
                if o + 1 > cap:
                    return -1
                dst[unsafe_offset=o] = b
                o += 1
            i += 1
            continue
        if write:
            if o + 1 > cap:
                return -1
            dst[unsafe_offset=o] = c
            o += 1
        i += 1


# Skip a number starting at p[i]; returns index after it or -1.
def scan_number(p: Bytes, n: Int, start: Int) -> Int:
    var i = start
    if i < n and p[unsafe_offset=i] == 0x2D:
        i += 1
    if i >= n:
        return -1
    if p[unsafe_offset=i] == 0x30:
        i += 1
    elif is_digit(p[unsafe_offset=i]):
        while i < n and is_digit(p[unsafe_offset=i]):
            i += 1
    else:
        return -1
    if i < n and p[unsafe_offset=i] == 0x2E:
        i += 1
        var d = 0
        while i < n and is_digit(p[unsafe_offset=i]):
            i += 1
            d += 1
        if d == 0:
            return -1
    if i < n and (p[unsafe_offset=i] == 0x65 or p[unsafe_offset=i] == 0x45):
        i += 1
        if i < n and (p[unsafe_offset=i] == 0x2B or p[unsafe_offset=i] == 0x2D):
            i += 1
        var d = 0
        while i < n and is_digit(p[unsafe_offset=i]):
            i += 1
            d += 1
        if d == 0:
            return -1
    return i


@always_inline
def skip_ws(p: Bytes, n: Int, start: Int) -> Int:
    var i = start
    while i < n:
        var c = p[unsafe_offset=i]
        if c == 0x20 or c == 0x09 or c == 0x0A or c == 0x0D:
            i += 1
        else:
            break
    return i


# Validate any JSON value starting at p[start]; returns index after it or -1.
def scan_value(p: Bytes, n: Int, start: Int, depth: Int) -> Int:
    if depth > MAX_DEPTH:
        return -1
    var i = skip_ws(p, n, start)
    if i >= n:
        return -1
    var c = p[unsafe_offset=i]
    var dummy = 0
    if c == 0x22:
        return scan_string(p, n, i, p, 0, dummy)
    if c == 0x7B:
        i = skip_ws(p, n, i + 1)
        if i < n and p[unsafe_offset=i] == 0x7D:
            return i + 1
        while True:
            i = skip_ws(p, n, i)
            if i >= n or p[unsafe_offset=i] != 0x22:
                return -1
            i = scan_string(p, n, i, p, 0, dummy)
            if i < 0:
                return -1
            i = skip_ws(p, n, i)
            if i >= n or p[unsafe_offset=i] != 0x3A:
                return -1
            i = scan_value(p, n, i + 1, depth + 1)
            if i < 0:
                return -1
            i = skip_ws(p, n, i)
            if i >= n:
                return -1
            if p[unsafe_offset=i] == 0x2C:
                i += 1
                continue
            if p[unsafe_offset=i] == 0x7D:
                return i + 1
            return -1
    if c == 0x5B:
        i = skip_ws(p, n, i + 1)
        if i < n and p[unsafe_offset=i] == 0x5D:
            return i + 1
        while True:
            i = scan_value(p, n, i, depth + 1)
            if i < 0:
                return -1
            i = skip_ws(p, n, i)
            if i >= n:
                return -1
            if p[unsafe_offset=i] == 0x2C:
                i += 1
                continue
            if p[unsafe_offset=i] == 0x5D:
                return i + 1
            return -1
    if c == 0x74:
        if i + 4 <= n and eqlit(p.unsafe_offset(i), 4, "true"):
            return i + 4
        return -1
    if c == 0x66:
        if i + 5 <= n and eqlit(p.unsafe_offset(i), 5, "false"):
            return i + 5
        return -1
    if c == 0x6E:
        if i + 4 <= n and eqlit(p.unsafe_offset(i), 4, "null"):
            return i + 4
        return -1
    return scan_number(p, n, i)


# Cursor over one JSON object: find a top-level key and look at its value.
struct JsonScan:
    var p: Bytes
    var n: Int
    var vstart: Int  # index of the value found by find_key
    var vend: Int

    def __init__(out self, p: Bytes, n: Int):
        self.p = p
        self.n = n
        self.vstart = -1
        self.vend = -1

    # True if the document is an object containing key (last occurrence wins).
    def find_key(mut self, key: StringSpan[ImmStaticOrigin]) -> Bool:
        var i = skip_ws(self.p, self.n, 0)
        if i >= self.n or self.p[unsafe_offset=i] != 0x7B:
            return False
        i = skip_ws(self.p, self.n, i + 1)
        if i < self.n and self.p[unsafe_offset=i] == 0x7D:
            return False
        var found = False
        var dummy = 0
        while True:
            i = skip_ws(self.p, self.n, i)
            if i >= self.n or self.p[unsafe_offset=i] != 0x22:
                return False
            var kstart = i + 1
            i = scan_string(self.p, self.n, i, self.p, 0, dummy)
            if i < 0:
                return False
            var klen = i - 1 - kstart
            i = skip_ws(self.p, self.n, i)
            if i >= self.n or self.p[unsafe_offset=i] != 0x3A:
                return False
            var vs = skip_ws(self.p, self.n, i + 1)
            i = scan_value(self.p, self.n, vs, 1)
            if i < 0:
                return False
            if eqlit(self.p.unsafe_offset(kstart), klen, key):
                self.vstart = vs
                self.vend = i
                found = True
            i = skip_ws(self.p, self.n, i)
            if i >= self.n:
                return False
            if self.p[unsafe_offset=i] == 0x2C:
                i += 1
                continue
            if self.p[unsafe_offset=i] == 0x7D:
                # the object must be the whole document
                return found and skip_ws(self.p, self.n, i + 1) == self.n
            return False

    def value_is_string(self) -> Bool:
        return self.vstart >= 0 and self.p[unsafe_offset=self.vstart] == 0x22

    # Raw (still escaped) string contents.
    def string_raw_ptr(self) -> Bytes:
        return self.p.unsafe_offset(self.vstart + 1)

    def string_raw_len(self) -> Int:
        return self.vend - self.vstart - 2

    def string_unescape(self, dst: Bytes, cap: Int) -> Int:
        var olen = 0
        if scan_string(self.p, self.n, self.vstart, dst, cap, olen) < 0:
            return -1
        return olen

    # Integer part of a JSON number value (floors), -1 if not a non-negative number.
    def number_int(self) -> Int64:
        if self.vstart < 0:
            return -1
        var i = self.vstart
        if not is_digit(self.p[unsafe_offset=i]):
            return -1
        var v: Int64 = 0
        var digits = 0
        while i < self.vend and is_digit(self.p[unsafe_offset=i]):
            if digits < 18:
                v = v * 10 + Int64(self.p[unsafe_offset=i] - 0x30)
            digits += 1
            i += 1
        if i < self.vend:
            var e = self.p[unsafe_offset=i]
            if e == 0x65 or e == 0x45:
                return -1  # exponent form: treat as invalid
        return v


comptime BODY_OK = 0
comptime BODY_MALFORMED = 1
comptime BODY_REQUIRED = 2
comptime BODY_TOO_LONG = 3

comptime MAX_BODY_CHARS = 500


# Validate the whole document, extract and trim "body". On BODY_OK, out holds
# the trimmed UTF-8 bytes (in dst) and out_len its length.
def validate_body(p: Bytes, n: Int, dst: Bytes, cap: Int, mut out_len: Int) -> Int:
    var end = scan_value(p, n, 0, 0)
    if end < 0 or skip_ws(p, n, end) != n:
        return BODY_MALFORMED
    var s = JsonScan(p, n)
    if not s.find_key("body") or not s.value_is_string():
        return BODY_REQUIRED
    var raw = s.string_unescape(dst, cap)
    if raw < 0:
        return BODY_MALFORMED
    var a = 0
    var b = raw
    while a < b and is_ws(dst[unsafe_offset=a]):
        a += 1
    while b > a and is_ws(dst[unsafe_offset=b - 1]):
        b -= 1
    if a == b:
        return BODY_REQUIRED
    var chars = 0
    for i in range(a, b):
        if (dst[unsafe_offset=i] & 0xC0) != 0x80:
            chars += 1
    if chars > MAX_BODY_CHARS:
        return BODY_TOO_LONG
    if a > 0:
        for i in range(b - a):
            dst[unsafe_offset=i] = dst[unsafe_offset=a + i]
    out_len = b - a
    return BODY_OK
