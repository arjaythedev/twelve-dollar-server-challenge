# SQLite record format: header of varint serial types, then the values.
# https://www.sqlite.org/fileformat2.html#record_format
from bytes import Bytes

comptime MAX_COLS = 8


@always_inline
def varint(p: Bytes, mut i: Int) -> UInt64:
    var v: UInt64 = 0
    for _k in range(8):
        var b = UInt64(p[unsafe_offset=i])
        i += 1
        v = (v << UInt64(7)) | (b & 0x7F)
        if b < 0x80:
            return v
    v = (v << UInt64(8)) | UInt64(p[unsafe_offset=i])
    i += 1
    return v


@always_inline
def serial_len(t: UInt64) -> Int:
    if t >= 12:
        return Int((t - 12) >> 1)
    if t == 0 or t == 8 or t == 9 or t == 10 or t == 11:
        return 0
    if t <= 4:
        return Int(t)
    if t == 5:
        return 6
    return 8


@always_inline
def read_int(p: Bytes, off: Int, t: UInt64) -> Int64:
    if t == 8:
        return 0
    if t == 9:
        return 1
    var n = serial_len(t)
    var v: Int64 = 0
    for k in range(n):
        v = (v << Int64(8)) | Int64(p[unsafe_offset=off + k])
    var bits = n * 8
    if bits < 64 and ((v >> Int64(bits - 1)) & 1) == 1:
        v -= Int64(1) << Int64(bits)
    return v


# Decoded column positions of one record.
struct Rec:
    var p: Bytes
    var ncols: Int
    var t0: UInt64
    var t1: UInt64
    var t2: UInt64
    var t3: UInt64
    var o0: Int
    var o1: Int
    var o2: Int
    var o3: Int

    def __init__(out self, p: Bytes):
        self.p = p
        self.ncols = 0
        self.t0 = 0
        self.t1 = 0
        self.t2 = 0
        self.t3 = 0
        self.o0 = 0
        self.o1 = 0
        self.o2 = 0
        self.o3 = 0

    # Decode up to 4 columns of the record at p.
    def decode(mut self, p: Bytes):
        self.p = p
        var i = 0
        var hdr = Int(varint(p, i))
        var off = hdr
        var n = 0
        while i < hdr and n < 4:
            var t = varint(p, i)
            if n == 0:
                self.t0 = t
                self.o0 = off
            elif n == 1:
                self.t1 = t
                self.o1 = off
            elif n == 2:
                self.t2 = t
                self.o2 = off
            else:
                self.t3 = t
                self.o3 = off
            off += serial_len(t)
            n += 1
        self.ncols = n

    @always_inline
    def int_at(self, col: Int) -> Int64:
        if col == 0:
            return read_int(self.p, self.o0, self.t0)
        if col == 1:
            return read_int(self.p, self.o1, self.t1)
        if col == 2:
            return read_int(self.p, self.o2, self.t2)
        return read_int(self.p, self.o3, self.t3)

    @always_inline
    def ptr_at(self, col: Int) -> Bytes:
        if col == 0:
            return self.p.unsafe_offset(self.o0)
        if col == 1:
            return self.p.unsafe_offset(self.o1)
        if col == 2:
            return self.p.unsafe_offset(self.o2)
        return self.p.unsafe_offset(self.o3)

    @always_inline
    def len_at(self, col: Int) -> Int:
        if col == 0:
            return serial_len(self.t0)
        if col == 1:
            return serial_len(self.t1)
        if col == 2:
            return serial_len(self.t2)
        return serial_len(self.t3)
