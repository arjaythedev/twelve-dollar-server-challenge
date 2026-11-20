// HS256 JWT verification. Every request's signature is checked (rule 5); only the HMAC key's
// padded blocks are hashed once at startup (td_hmac_init).
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif
import CShim

enum AuthError {
    case missing
    case invalid
    case payload
}

/// Longest decoded token part: a request head is at most 16 KB.
private let scratchCap = 16 * 1024 + 64

private let b64Table: UnsafeMutablePointer<UInt8> = {
    let t = UnsafeMutablePointer<UInt8>.allocate(capacity: 256)
    t.initialize(repeating: 0xff, count: 256)
    let a = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".utf8)
    for (i, c) in a.enumerated() { t[Int(c)] = UInt8(i) }
    return t
}()

/// Unpadded base64url into `out`; returns the decoded length, or -1 on any invalid character
/// or length. The unused low bits of the last character must be zero, so exactly one spelling
/// of a signature verifies.
private func b64url(_ s: Slice, _ out: UnsafeMutablePointer<UInt8>) -> Int {
    if s.n % 4 == 1 { return -1 }
    let t = b64Table
    var o = 0
    var i = 0
    while i + 4 <= s.n {
        let a = t[Int(s[i])], b = t[Int(s[i + 1])], d = t[Int(s[i + 2])], e = t[Int(s[i + 3])]
        if (a | b | d | e) & 0xc0 != 0 { return -1 }
        let v = UInt32(a) << 18 | UInt32(b) << 12 | UInt32(d) << 6 | UInt32(e)
        out[o] = UInt8(truncatingIfNeeded: v >> 16)
        out[o + 1] = UInt8(truncatingIfNeeded: v >> 8)
        out[o + 2] = UInt8(truncatingIfNeeded: v)
        o += 3
        i += 4
    }
    let r = s.n - i
    if r > 0 {
        var v: UInt32 = 0
        for k in 0..<r {
            let x = t[Int(s[i + k])]
            if x & 0xc0 != 0 { return -1 }
            v |= UInt32(x) << UInt32(18 - 6 * k)
        }
        let unused = r == 2 ? v & 0xffff : v & 0xff
        if r < 2 || unused != 0 { return -1 }
        out[o] = UInt8(truncatingIfNeeded: v >> 16)
        o += 1
        if r == 3 {
            out[o] = UInt8(truncatingIfNeeded: v >> 8)
            o += 1
        }
    }
    return o
}

final class Verifier {
    private let scratch = UnsafeMutablePointer<UInt8>.allocate(capacity: scratchCap)
    private let want = UnsafeMutablePointer<UInt8>.allocate(capacity: 32)
    /// The verified token's claims (valid after `verify` returns nil).
    var userId: Int64 = 0
    let username = UnsafeMutablePointer<UInt8>.allocate(capacity: scratchCap)
    var usernameLen = 0

    init(secret: Slice) {
        td_hmac_init(secret.p, secret.n)
    }

    /// `header` is the Authorization header value, if any. `now` is Unix seconds.
    /// Returns nil when the token is good.
    func verify(hasAuth: Bool, header: Slice, now: Double) -> AuthError? {
        guard hasAuth, header.hasPrefix("Bearer ") else { return .missing }
        let token = header.from(7)
        let d1 = token.find(0x2e)
        if d1 < 0 { return .invalid }
        let rest1 = token.from(d1 + 1)
        let d2 = rest1.find(0x2e)
        if d2 < 0 { return .invalid }
        let sig = rest1.from(d2 + 1)
        if sig.contains(0x2e) { return .invalid }
        let h = Slice(p: token.p, n: d1)
        let p = Slice(p: rest1.p, n: d2)

        let buf = scratch
        var n = b64url(sig, buf)
        if n != 32 { return .invalid }
        td_hmac(token.p, d1 + 1 + d2, want)
        var diff: UInt8 = 0
        for i in 0..<32 { diff |= want[i] ^ buf[i] }
        if diff != 0 { return .invalid }

        // Header: must say HS256.
        n = b64url(h, buf)
        if n < 0 { return .invalid }
        var algOK = false
        let isObj: Bool
        do {
            isObj = try parseObject(Slice(p: UnsafePointer(buf), n: n)) { k, v in
                if k.equals("alg"), case .str(let s, let e) = v {
                    if !e {
                        algOK = s.equals("HS256")
                    } else {
                        var t = [UInt8]()
                        unescape(s, into: &t)
                        algOK = t == [0x48, 0x53, 0x32, 0x35, 0x36]
                    }
                } else if k.equals("alg") {
                    algOK = false
                }
            }
        } catch { return .invalid }
        if !isObj || !algOK { return .invalid }

        // Payload.
        n = b64url(p, buf)
        if n < 0 { return .invalid }
        var exp: Double?
        var sub: (Slice, Bool)?
        var name: (Slice, Bool)?
        do {
            let ok = try parseObject(Slice(p: UnsafePointer(buf), n: n)) { k, v in
                if k.equals("exp") {
                    if case .num(let x) = v { exp = x } else { exp = nil }
                } else if k.equals("sub") {
                    if case .str(let s, let e) = v { sub = (s, e) } else { sub = nil }
                } else if k.equals("username") {
                    if case .str(let s, let e) = v { name = (s, e) } else { name = nil }
                }
            }
            if !ok { return .invalid }
        } catch { return .invalid }
        guard let e = exp, now < e else { return .invalid }

        guard let (s, esc) = sub else { return .payload }
        if !esc {
            guard let id = parsePositive(s) else { return .payload }
            userId = id
        } else {
            var t = [UInt8]()
            unescape(s, into: &t)
            let id = t.withUnsafeBufferPointer { b in
                parsePositive(Slice(p: b.baseAddress ?? UnsafePointer(emptyBytes), n: b.count))
            }
            guard let id else { return .payload }
            userId = id
        }
        guard let (nm, nesc) = name else { return .payload }
        if !nesc {
            memcpy(username, nm.p, nm.n)
            usernameLen = nm.n
        } else {
            var t = [UInt8]()
            unescape(nm, into: &t)
            t.withUnsafeBufferPointer { b in
                if b.count > 0 { memcpy(username, b.baseAddress!, b.count) }
            }
            usernameLen = t.count
        }
        return nil
    }
}
