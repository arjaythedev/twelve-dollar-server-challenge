// Byte slices and the response writer. Everything on the request path works on raw pointers:
// no arrays that reallocate, no String, no Foundation.
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif
import CShim

/// A borrowed run of bytes. Valid only while the memory it points into is.
struct Slice {
    var p: UnsafePointer<UInt8>
    var n: Int

    @inline(__always) subscript(i: Int) -> UInt8 { p[i] }
    @inline(__always) var isEmpty: Bool { n == 0 }

    @inline(__always) func sub(_ a: Int, _ b: Int) -> Slice { Slice(p: p + a, n: b - a) }
    @inline(__always) func from(_ a: Int) -> Slice { Slice(p: p + a, n: n - a) }

    @inline(__always) func equals(_ s: StaticString) -> Bool {
        n == s.utf8CodeUnitCount && memcmp(p, s.utf8Start, n) == 0
    }

    @inline(__always) func hasPrefix(_ s: StaticString) -> Bool {
        n >= s.utf8CodeUnitCount && memcmp(p, s.utf8Start, s.utf8CodeUnitCount) == 0
    }

    /// Index of the first `b`, or -1.
    @inline(__always) func find(_ b: UInt8) -> Int {
        guard n > 0, let r = memchr(p, Int32(b), n) else { return -1 }
        return UnsafeRawPointer(r) - UnsafeRawPointer(p)
    }

    @inline(__always) func contains(_ b: UInt8) -> Bool { find(b) >= 0 }

    /// ASCII case-insensitive comparison against an already lower-case literal.
    @inline(__always) func equalsIgnoreCase(_ lower: StaticString) -> Bool {
        let m = lower.utf8CodeUnitCount
        if n != m { return false }
        let l = lower.utf8Start
        var i = 0
        while i < m {
            var c = p[i]
            if c >= 0x41 && c <= 0x5a { c |= 0x20 }
            if c != l[i] { return false }
            i += 1
        }
        return true
    }
}

let emptyBytes: UnsafeMutablePointer<UInt8> = {
    let p = UnsafeMutablePointer<UInt8>.allocate(capacity: 1)
    p.pointee = 0
    return p
}()

let emptySlice = Slice(p: UnsafePointer(emptyBytes), n: 0)

/// A positive decimal integer with no sign, spaces or leading zeros that fits in an i64.
func parsePositive(_ s: Slice) -> Int64? {
    if s.n == 0 || s.n > 18 || s[0] == 0x30 { return nil }
    var v: Int64 = 0
    for i in 0..<s.n {
        let d = s[i] &- 0x30
        if d > 9 { return nil }
        v = v &* 10 &+ Int64(d)
    }
    return v
}

private let digits2: StaticString = "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899"

/// Writes `v` in decimal at `p`; returns the bytes written (at most 20).
@inline(__always)
func writeInt(_ p: UnsafeMutablePointer<UInt8>, _ v: Int64) -> Int {
    var n = v.magnitude
    var d = 1
    var lim: UInt64 = 10
    while n >= lim && d < 20 {
        d += 1
        lim = lim &* 10
    }
    let neg = v < 0 ? 1 : 0
    let total = d + neg
    if neg == 1 { p[0] = 0x2d }
    let t = digits2.utf8Start
    var i = total
    while n >= 100 {
        let r = Int(n % 100) * 2
        n /= 100
        i -= 2
        p[i] = t[r]
        p[i + 1] = t[r + 1]
    }
    if n >= 10 {
        let r = Int(n) * 2
        i -= 2
        p[i] = t[r]
        p[i + 1] = t[r + 1]
    } else {
        i -= 1
        p[i] = 0x30 &+ UInt8(truncatingIfNeeded: n)
    }
    return total
}

/// The response buffer of one connection (a C `lp_ob`). Every append reserves first.
struct Out {
    let o: UnsafeMutablePointer<lp_ob>

    @inline(__always) var len: Int { Int(o.pointee.len) }

    @inline(__always) func reserve(_ n: Int) {
        if Int(o.pointee.cap) - Int(o.pointee.len) < n { lp_ob_grow(o, UInt32(truncatingIfNeeded: n)) }
    }

    @inline(__always) func truncate(_ n: Int) { o.pointee.len = UInt32(truncatingIfNeeded: n) }

    @inline(__always) func lit(_ s: StaticString) {
        let n = s.utf8CodeUnitCount
        reserve(n)
        memcpy(o.pointee.p + Int(o.pointee.len), s.utf8Start, n)
        o.pointee.len += UInt32(truncatingIfNeeded: n)
    }

    @inline(__always) func byte(_ b: UInt8) {
        reserve(1)
        o.pointee.p[Int(o.pointee.len)] = b
        o.pointee.len += 1
    }

    @inline(__always) func bytes(_ s: Slice) {
        reserve(s.n)
        memcpy(o.pointee.p + Int(o.pointee.len), s.p, s.n)
        o.pointee.len += UInt32(truncatingIfNeeded: s.n)
    }

    @inline(__always) func int(_ v: Int64) {
        reserve(20)
        o.pointee.len += UInt32(truncatingIfNeeded: writeInt(o.pointee.p + Int(o.pointee.len), v))
    }

    /// `s` as a JSON string body (no quotes).
    @inline(__always) func esc(_ s: Slice) {
        reserve(6 * s.n)
        o.pointee.len += UInt32(truncatingIfNeeded: td_esc(s.p, s.n, o.pointee.p + Int(o.pointee.len)))
    }

    /// Reserves `bound` bytes and returns a cursor for writing at most that many without further
    /// checks; `commit` keeps what was written.
    @inline(__always) func cursor(bound: Int) -> Cursor {
        reserve(bound)
        let base = o.pointee.p + Int(o.pointee.len)
        return Cursor(p: base, base: base, end: base + bound)
    }

    @inline(__always) func commit(_ c: Cursor) {
        if c.p > c.end { fatalError("cursor bound exceeded") }
        o.pointee.len += UInt32(truncatingIfNeeded: c.p - c.base)
    }
}

/// Writes straight into reserved spare capacity: fixed pieces are plain copies, integers are
/// written in place, strings go through the C escaper.
struct Cursor {
    var p: UnsafeMutablePointer<UInt8>
    let base: UnsafeMutablePointer<UInt8>
    let end: UnsafeMutablePointer<UInt8>

    @inline(__always) mutating func lit(_ s: StaticString) {
        let n = s.utf8CodeUnitCount
        memcpy(p, s.utf8Start, n)
        p += n
    }

    @inline(__always) mutating func byte(_ b: UInt8) {
        p.pointee = b
        p += 1
    }

    @inline(__always) mutating func int(_ v: Int64) { p += writeInt(p, v) }

    @inline(__always) mutating func esc(_ s: Slice) { p += td_esc(s.p, s.n, p) }
}
