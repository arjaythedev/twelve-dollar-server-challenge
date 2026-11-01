// JSON output and a validating single-pass parser for the two documents we read:
// the POST /posts body and the JWT header/payload.

@inline(__always)
func hexDigit(_ v: UInt8) -> UInt8 { v < 10 ? v &+ 48 : v &+ 87 }

/// True when any of the 8 bytes in `w` may be `"`, `\` or below 0x20. False positives are possible
/// (the slow path then checks byte by byte); false negatives are not.
@inline(__always)
func mayNeedEscape(_ w: UInt64) -> Bool {
    let ones: UInt64 = 0x0101_0101_0101_0101
    let high: UInt64 = 0x8080_8080_8080_8080
    let quote = w ^ (ones &* 0x22)
    let slash = w ^ (ones &* 0x5C)
    let hasQuote = (quote &- ones) & ~quote
    let hasSlash = (slash &- ones) & ~slash
    let hasControl = (w &- ones &* 0x20) & ~w
    return (hasQuote | hasSlash | hasControl) & high != 0
}

extension OutBuffer {
    /// Writes `v` as a JSON string literal. Escapes only what JSON requires: `"`, `\` and bytes below 0x20.
    /// Reserves the worst case once, then writes without bounds checks; clean 8-byte words copy whole.
    public mutating func appendJSONString(_ v: ByteView) {
        let n = v.count
        reserve(n &* 6 &+ 2)
        var o = base + count
        o.pointee = 0x22
        o += 1
        if let p = v.base {
            var i = 0
            while i < n {
                if i &+ 8 <= n {
                    let w = UnsafeRawPointer(p + i).loadUnaligned(as: UInt64.self)
                    if !mayNeedEscape(w) {
                        UnsafeMutableRawPointer(o).storeBytes(of: w, as: UInt64.self)
                        o += 8
                        i &+= 8
                        continue
                    }
                }
                let b = p[i]
                i &+= 1
                if b >= 0x20 && b != 0x22 && b != 0x5C {
                    o.pointee = b
                    o += 1
                    continue
                }
                o[0] = 0x5C
                switch b {
                case 0x22: o[1] = 0x22
                case 0x5C: o[1] = 0x5C
                case 0x0A: o[1] = UInt8(ascii: "n")
                case 0x0D: o[1] = UInt8(ascii: "r")
                case 0x09: o[1] = UInt8(ascii: "t")
                case 0x08: o[1] = UInt8(ascii: "b")
                case 0x0C: o[1] = UInt8(ascii: "f")
                default:
                    o[1] = UInt8(ascii: "u")
                    o[2] = 0x30
                    o[3] = 0x30
                    o[4] = hexDigit(b >> 4)
                    o[5] = hexDigit(b & 0x0F)
                    o += 4
                }
                o += 2
            }
        }
        o.pointee = 0x22
        o += 1
        count = base.distance(to: o)
    }
}

public enum JSONKind: UInt8 {
    case string, number, object, array, trueValue, falseValue, null
}

/// One top-level member of an object. Ranges point into the parsed document.
/// For strings, `value` excludes the quotes and is still escaped when `escaped` is true.
public struct JSONMember {
    public var key: ByteView
    public var keyEscaped: Bool
    public var kind: JSONKind
    public var value: ByteView
    public var escaped: Bool
}

/// Validating recursive-descent parser over raw bytes (RFC 8259). No allocation.
struct JSONParser {
    let p: UnsafePointer<UInt8>
    let end: Int
    var i = 0

    init(_ v: ByteView) {
        // A zero-length document has no base pointer; point at a static empty string instead.
        p = v.base ?? ("" as StaticString).utf8Start
        end = v.count
    }

    @inline(__always)
    mutating func skipWhitespace() {
        while i < end {
            let b = p[i]
            if b == 0x20 || b == 0x0A || b == 0x0D || b == 0x09 { i &+= 1 } else { break }
        }
    }

    /// Parses a string starting at the opening quote. Returns the content range and whether it has escapes.
    mutating func string(_ escaped: inout Bool) -> ByteView? {
        guard i < end, p[i] == 0x22 else { return nil }
        i &+= 1
        let start = i
        escaped = false
        while i < end {
            let b = p[i]
            if b == 0x22 {
                let v = ByteView(base: p + start, count: i &- start)
                i &+= 1
                return v
            }
            if b < 0x20 { return nil }
            if b == 0x5C {
                escaped = true
                i &+= 1
                guard i < end else { return nil }
                switch p[i] {
                case 0x22, 0x5C, 0x2F, 0x62, 0x66, 0x6E, 0x72, 0x74:
                    i &+= 1
                case 0x75:
                    guard i &+ 4 < end else { return nil }
                    for k in 1...4 where hexValue(p[i &+ k]) < 0 { return nil }
                    i &+= 5
                default:
                    return nil
                }
                continue
            }
            i &+= 1
        }
        return nil
    }

    mutating func number() -> Bool {
        if i < end && p[i] == UInt8(ascii: "-") { i &+= 1 }
        guard i < end else { return false }
        if p[i] == UInt8(ascii: "0") {
            i &+= 1
        } else if isDigit(p[i]) {
            while i < end && isDigit(p[i]) { i &+= 1 }
        } else {
            return false
        }
        if i < end && p[i] == UInt8(ascii: ".") {
            i &+= 1
            guard i < end && isDigit(p[i]) else { return false }
            while i < end && isDigit(p[i]) { i &+= 1 }
        }
        if i < end && (p[i] == UInt8(ascii: "e") || p[i] == UInt8(ascii: "E")) {
            i &+= 1
            if i < end && (p[i] == UInt8(ascii: "+") || p[i] == UInt8(ascii: "-")) { i &+= 1 }
            guard i < end && isDigit(p[i]) else { return false }
            while i < end && isDigit(p[i]) { i &+= 1 }
        }
        return true
    }

    mutating func literal(_ s: StaticString) -> Bool {
        let n = s.utf8CodeUnitCount
        guard end &- i >= n, memeq(p + i, s.utf8Start, n) else { return false }
        i &+= n
        return true
    }

    /// Validates any value. `depth` bounds nesting so hostile input can't overflow the stack.
    mutating func value(_ depth: Int, _ kind: inout JSONKind) -> Bool {
        skipWhitespace()
        guard i < end, depth < 256 else { return false }
        switch p[i] {
        case 0x22:
            kind = .string
            var esc = false
            return string(&esc) != nil
        case 0x7B:
            kind = .object
            return object(depth) { _ in }
        case 0x5B:
            kind = .array
            i &+= 1
            skipWhitespace()
            if i < end && p[i] == 0x5D { i &+= 1; return true }
            while true {
                var k = JSONKind.null
                guard value(depth &+ 1, &k) else { return false }
                skipWhitespace()
                guard i < end else { return false }
                if p[i] == 0x2C { i &+= 1; continue }
                if p[i] == 0x5D { i &+= 1; return true }
                return false
            }
        case UInt8(ascii: "t"):
            kind = .trueValue
            return literal("true")
        case UInt8(ascii: "f"):
            kind = .falseValue
            return literal("false")
        case UInt8(ascii: "n"):
            kind = .null
            return literal("null")
        default:
            kind = .number
            return number()
        }
    }

    /// Parses an object starting at `{`, calling `member` for each direct member.
    mutating func object(_ depth: Int, _ member: (JSONMember) -> Void) -> Bool {
        guard i < end, p[i] == 0x7B else { return false }
        i &+= 1
        skipWhitespace()
        if i < end && p[i] == 0x7D { i &+= 1; return true }
        while true {
            skipWhitespace()
            var keyEscaped = false
            guard let key = string(&keyEscaped) else { return false }
            skipWhitespace()
            guard i < end, p[i] == 0x3A else { return false }
            i &+= 1
            skipWhitespace()
            let valueStart = i
            var kind = JSONKind.null
            var escaped = false
            var valueView: ByteView
            if i < end && p[i] == 0x22 {
                kind = .string
                guard let s = string(&escaped) else { return false }
                valueView = s
            } else {
                guard value(depth &+ 1, &kind) else { return false }
                valueView = ByteView(base: p + valueStart, count: i &- valueStart)
            }
            member(JSONMember(key: key, keyEscaped: keyEscaped, kind: kind, value: valueView, escaped: escaped))
            skipWhitespace()
            guard i < end else { return false }
            if p[i] == 0x2C { i &+= 1; continue }
            if p[i] == 0x7D { i &+= 1; return true }
            return false
        }
    }
}

public enum JSONDocument {
    case malformed
    case notAnObject
    case object
}

/// Validates the whole document and reports each top-level member when it is an object.
public func scanJSONObject(_ doc: ByteView, _ member: (JSONMember) -> Void) -> JSONDocument {
    var parser = JSONParser(doc)
    parser.skipWhitespace()
    var result = JSONDocument.object
    if parser.i < parser.end && parser.p[parser.i] == 0x7B {
        guard parser.object(0, member) else { return .malformed }
    } else {
        var kind = JSONKind.null
        guard parser.value(0, &kind) else { return .malformed }
        result = .notAnObject
    }
    parser.skipWhitespace()
    return parser.i == parser.end ? result : .malformed
}

@inline(__always)
func isDigit(_ b: UInt8) -> Bool { b &- 48 < 10 }

@inline(__always)
func hexValue(_ b: UInt8) -> Int {
    switch b {
    case 48...57: return Int(b &- 48)
    case 97...102: return Int(b &- 87)
    case 65...70: return Int(b &- 55)
    default: return -1
    }
}

/// Decodes the escaped content of a valid JSON string into `out` as UTF-8.
/// Lone surrogates become U+FFFD.
public func unescapeJSONString(_ v: ByteView, into out: inout OutBuffer) {
    guard let p = v.base else { return }
    let n = v.count
    out.reserve(n)
    var i = 0
    while i < n {
        let b = p[i]
        if b != 0x5C {
            out.append(b)
            i &+= 1
            continue
        }
        let e = p[i &+ 1]
        i &+= 2
        switch e {
        case 0x62: out.append(0x08)
        case 0x66: out.append(0x0C)
        case 0x6E: out.append(0x0A)
        case 0x72: out.append(0x0D)
        case 0x74: out.append(0x09)
        case 0x75:
            var cp = readHex4(p + i)
            i &+= 4
            if cp >= 0xD800 && cp < 0xDC00 {
                if i &+ 6 <= n && p[i] == 0x5C && p[i &+ 1] == 0x75 {
                    let lo = readHex4(p + (i &+ 2))
                    if lo >= 0xDC00 && lo < 0xE000 {
                        cp = 0x10000 &+ ((cp &- 0xD800) << 10) &+ (lo &- 0xDC00)
                        i &+= 6
                    } else {
                        cp = 0xFFFD
                    }
                } else {
                    cp = 0xFFFD
                }
            } else if cp >= 0xDC00 && cp < 0xE000 {
                cp = 0xFFFD
            }
            appendUTF8(cp, &out)
        default:
            out.append(e)  // " \ /
        }
    }
}

@inline(__always)
func readHex4(_ p: UnsafePointer<UInt8>) -> UInt32 {
    UInt32(hexValue(p[0]) << 12 | hexValue(p[1]) << 8 | hexValue(p[2]) << 4 | hexValue(p[3]))
}

func appendUTF8(_ cp: UInt32, _ out: inout OutBuffer) {
    if cp < 0x80 {
        out.append(UInt8(cp))
    } else if cp < 0x800 {
        out.append(UInt8(0xC0 | (cp >> 6)))
        out.append(UInt8(0x80 | (cp & 0x3F)))
    } else if cp < 0x10000 {
        out.append(UInt8(0xE0 | (cp >> 12)))
        out.append(UInt8(0x80 | ((cp >> 6) & 0x3F)))
        out.append(UInt8(0x80 | (cp & 0x3F)))
    } else {
        out.append(UInt8(0xF0 | (cp >> 18)))
        out.append(UInt8(0x80 | ((cp >> 12) & 0x3F)))
        out.append(UInt8(0x80 | ((cp >> 6) & 0x3F)))
        out.append(UInt8(0x80 | (cp & 0x3F)))
    }
}

/// Parses a JSON number token into a Double. The token was validated by the parser.
func parseJSONNumber(_ v: ByteView) -> Double {
    var i = 0
    var negative = false
    if i < v.count && v[i] == UInt8(ascii: "-") { negative = true; i &+= 1 }
    var value = 0.0
    while i < v.count && isDigit(v[i]) {
        value = value * 10 + Double(v[i] &- 48)
        i &+= 1
    }
    if i < v.count && v[i] == UInt8(ascii: ".") {
        i &+= 1
        var scale = 0.1
        while i < v.count && isDigit(v[i]) {
            value += Double(v[i] &- 48) * scale
            scale *= 0.1
            i &+= 1
        }
    }
    if i < v.count && (v[i] == UInt8(ascii: "e") || v[i] == UInt8(ascii: "E")) {
        i &+= 1
        var expNegative = false
        if i < v.count && (v[i] == UInt8(ascii: "+") || v[i] == UInt8(ascii: "-")) {
            expNegative = v[i] == UInt8(ascii: "-")
            i &+= 1
        }
        var e = 0
        while i < v.count && isDigit(v[i]) {
            e = min(e &* 10 &+ Int(v[i] &- 48), 400)
            i &+= 1
        }
        while e > 0 {
            value = expNegative ? value / 10 : value * 10
            e &-= 1
        }
    }
    return negative ? -value : value
}
