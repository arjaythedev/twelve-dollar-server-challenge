// A validating JSON reader for request bodies and JWT parts. It checks the whole document and
// reports the top-level object's string and number members; everything else is validated and
// skipped without allocating (only keys and strings that hold escapes are decoded).
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif
import CShim

struct JSONError: Error {}

enum Val {
    /// A string's raw bytes between the quotes, and whether they contain escapes.
    case str(Slice, Bool)
    case num(Double)
    case other
}

private let maxDepth = 64

private struct Parser {
    let s: UnsafePointer<UInt8>
    let n: Int
    var i = 0

    @inline(__always) mutating func ws() {
        while i < n {
            let c = s[i]
            if c == 0x20 || c == 0x0a || c == 0x0d || c == 0x09 { i += 1 } else { break }
        }
    }

    @inline(__always) func peek() -> Int { i < n ? Int(s[i]) : -1 }

    mutating func lit(_ w: StaticString) throws {
        let m = w.utf8CodeUnitCount
        if n - i >= m && memcmp(s + i, w.utf8Start, m) == 0 { i += m } else { throw JSONError() }
    }

    @inline(__always) static func isHex(_ c: UInt8) -> Bool {
        (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66)
    }

    /// After the opening quote. Returns the raw contents and whether they hold escapes.
    mutating func string() throws -> (Slice, Bool) {
        let start = i
        var esc = false
        while true {
            if i >= n { throw JSONError() }
            let c = s[i]
            if c == 0x22 {
                let raw = Slice(p: s + start, n: i - start)
                i += 1
                return (raw, esc)
            } else if c == 0x5c {
                esc = true
                if i + 1 >= n { throw JSONError() }
                switch s[i + 1] {
                case 0x22, 0x5c, 0x2f, 0x62, 0x66, 0x6e, 0x72, 0x74: i += 2
                case 0x75:
                    if i + 6 > n { throw JSONError() }
                    for k in 2..<6 where !Parser.isHex(s[i + k]) { throw JSONError() }
                    i += 6
                default: throw JSONError()
                }
            } else if c < 0x20 {
                throw JSONError()
            } else {
                i += 1
            }
        }
    }

    @inline(__always) func isDigit(_ c: Int) -> Bool { c >= 0x30 && c <= 0x39 }

    mutating func number() throws -> Double {
        let start = i
        if peek() == 0x2d { i += 1 }
        let c = peek()
        if c == 0x30 {
            i += 1
        } else if c >= 0x31 && c <= 0x39 {
            while isDigit(peek()) { i += 1 }
        } else {
            throw JSONError()
        }
        if peek() == 0x2e {
            i += 1
            if !isDigit(peek()) { throw JSONError() }
            while isDigit(peek()) { i += 1 }
        }
        if peek() == 0x65 || peek() == 0x45 {
            i += 1
            if peek() == 0x2b || peek() == 0x2d { i += 1 }
            if !isDigit(peek()) { throw JSONError() }
            while isDigit(peek()) { i += 1 }
        }
        return toDouble(Slice(p: s + start, n: i - start))
    }

    mutating func value(_ depth: Int) throws -> Val {
        if depth > maxDepth { throw JSONError() }
        ws()
        switch peek() {
        case -1: throw JSONError()
        case 0x22:
            i += 1
            let (r, e) = try string()
            return .str(r, e)
        case 0x7b:
            try object(depth) { _, _ in }
            return .other
        case 0x5b:
            i += 1
            ws()
            if peek() == 0x5d {
                i += 1
                return .other
            }
            while true {
                _ = try value(depth + 1)
                ws()
                let c = peek()
                if c == 0x2c {
                    i += 1
                } else if c == 0x5d {
                    i += 1
                    return .other
                } else {
                    throw JSONError()
                }
            }
        case 0x74:
            try lit("true")
            return .other
        case 0x66:
            try lit("false")
            return .other
        case 0x6e:
            try lit("null")
            return .other
        default: return .num(try number())
        }
    }

    mutating func object(_ depth: Int, _ f: (Slice, Val) -> Void) throws {
        i += 1  // '{'
        ws()
        if peek() == 0x7d {
            i += 1
            return
        }
        while true {
            ws()
            if peek() != 0x22 { throw JSONError() }
            i += 1
            let (k, kesc) = try string()
            ws()
            if peek() != 0x3a { throw JSONError() }
            i += 1
            let v = try value(depth + 1)
            if kesc {
                var name = [UInt8]()
                unescape(k, into: &name)
                name.withUnsafeBufferPointer { b in
                    f(Slice(p: b.baseAddress ?? UnsafePointer(emptyBytes), n: b.count), v)
                }
            } else {
                f(k, v)
            }
            ws()
            let c = peek()
            if c == 0x2c {
                i += 1
            } else if c == 0x7d {
                i += 1
                return
            } else {
                throw JSONError()
            }
        }
    }
}

/// The text of a number already validated as JSON, as the nearest double.
private func toDouble(_ s: Slice) -> Double {
    if s.n < 64 {
        return withUnsafeTemporaryAllocation(of: CChar.self, capacity: 64) { b in
            for i in 0..<s.n { b[i] = CChar(bitPattern: s[i]) }
            b[s.n] = 0
            return strtod(b.baseAddress!, nil)
        }
    }
    let copy = UnsafeMutablePointer<CChar>.allocate(capacity: s.n + 1)
    defer { copy.deallocate() }
    for i in 0..<s.n { copy[i] = CChar(bitPattern: s[i]) }
    copy[s.n] = 0
    return strtod(copy, nil)
}

/// Validates `s` as one JSON document. If it is an object, `f` sees each top-level member
/// (later duplicates after earlier ones, so the last one wins, as in JSON.parse).
/// Throws for malformed JSON; returns false for valid JSON that isn't an object.
func parseObject(_ s: Slice, _ f: (Slice, Val) -> Void) throws -> Bool {
    if td_utf8_valid(s.p, s.n) == 0 { throw JSONError() }
    var p = Parser(s: s.p, n: s.n)
    p.ws()
    let obj = p.peek() == 0x7b
    if obj { try p.object(0, f) } else { _ = try p.value(0) }
    p.ws()
    if p.i != s.n { throw JSONError() }
    return obj
}

@inline(__always)
private func hex4(_ p: UnsafePointer<UInt8>) -> UInt32 {
    var v: UInt32 = 0
    for k in 0..<4 {
        let c = p[k]
        let d: UInt32
        switch c {
        case 0x30...0x39: d = UInt32(c) - 0x30
        case 0x41...0x46: d = UInt32(c) - 0x41 + 10
        case 0x61...0x66: d = UInt32(c) - 0x61 + 10
        default: d = 0
        }
        v = v << 4 | d
    }
    return v
}

private func putUTF8(_ out: inout [UInt8], _ cp: UInt32) {
    if cp < 0x80 {
        out.append(UInt8(cp))
    } else if cp < 0x800 {
        out.append(UInt8(0xc0 | cp >> 6))
        out.append(UInt8(0x80 | cp & 0x3f))
    } else if cp < 0x10000 {
        out.append(UInt8(0xe0 | cp >> 12))
        out.append(UInt8(0x80 | cp >> 6 & 0x3f))
        out.append(UInt8(0x80 | cp & 0x3f))
    } else {
        out.append(UInt8(0xf0 | cp >> 18))
        out.append(UInt8(0x80 | cp >> 12 & 0x3f))
        out.append(UInt8(0x80 | cp >> 6 & 0x3f))
        out.append(UInt8(0x80 | cp & 0x3f))
    }
}

/// Decodes a validated string's raw contents into `out`. Lone surrogates become U+FFFD.
func unescape(_ raw: Slice, into out: inout [UInt8]) {
    var i = 0
    while i < raw.n {
        let c = raw[i]
        if c != 0x5c {
            var j = i
            while j < raw.n && raw[j] != 0x5c { j += 1 }
            out.append(contentsOf: UnsafeBufferPointer(start: raw.p + i, count: j - i))
            i = j
            continue
        }
        let e = raw[i + 1]
        i += 2
        var ch: UInt32
        switch e {
        case 0x62: ch = 8
        case 0x66: ch = 12
        case 0x6e: ch = 10
        case 0x72: ch = 13
        case 0x74: ch = 9
        case 0x75:
            var u = hex4(raw.p + i)
            i += 4
            if u >= 0xd800 && u < 0xdc00 && i + 6 <= raw.n && raw[i] == 0x5c && raw[i + 1] == 0x75 {
                let lo = hex4(raw.p + i + 2)
                if lo >= 0xdc00 && lo < 0xe000 {
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00)
                    i += 6
                }
            }
            ch = u
        default: ch = UInt32(e)
        }
        if ch >= 0xd800 && ch < 0xe000 { ch = 0xfffd }
        putUTF8(&out, ch)
    }
}
