// Routing and handlers. Each handler appends one complete HTTP response to `out`.
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif
import CShim

private let maxBodyChars = 500
private let crlf2: StaticString = "\r\n\r\n"

/// Writes the status line and headers, leaving room for a 4-digit Content-Length that is fixed
/// up by `finish`. Returns where the body starts.
@inline(__always)
private func start(_ out: Out, _ status: Int) -> Int {
    switch status {
    case 200: out.lit("HTTP/1.1 200 OK\r\n")
    case 201: out.lit("HTTP/1.1 201 Created\r\n")
    case 400: out.lit("HTTP/1.1 400 Bad Request\r\n")
    case 401: out.lit("HTTP/1.1 401 Unauthorized\r\n")
    case 404: out.lit("HTTP/1.1 404 Not Found\r\n")
    case 503: out.lit("HTTP/1.1 503 Service Unavailable\r\n")
    default: out.lit("HTTP/1.1 500 Internal Server Error\r\n")
    }
    out.lit("Content-Type: application/json\r\nContent-Length: 0000\r\n\r\n")
    return out.len
}

/// Fills in Content-Length for the body that started at `bodyStart`.
@inline(__always)
private func finish(_ out: Out, _ bodyStart: Int) {
    let n = out.len - bodyStart
    let slot = bodyStart - 8  // "0000\r\n\r\n"
    var d = 1
    var t = n
    while t >= 10 {
        t /= 10
        d += 1
    }
    if d != 4 {
        // Shift the body so the header has exactly the digits it needs.
        let delta = d - 4
        if delta > 0 { out.reserve(delta) }
        let p = out.o.pointee.p!
        memmove(p + bodyStart + delta, p + bodyStart, n)
        _ = writeInt(p + slot, Int64(n))
        memcpy(p + slot + d, crlf2.utf8Start, 4)
        out.truncate(bodyStart + delta + n)
    } else {
        _ = writeInt(out.o.pointee.p! + slot, Int64(n))
    }
}

func writeError(_ out: Out, _ status: Int, _ msg: StaticString) {
    let b = start(out, status)
    out.lit("{\"error\":\"")
    out.lit(msg)
    out.lit("\"}")
    finish(out, b)
}

private func nowSecs() -> Double {
    var ts = timespec()
    clock_gettime(CLOCK_REALTIME, &ts)
    return Double(ts.tv_sec) + Double(ts.tv_nsec) / 1e9
}

/// JavaScript's String.prototype.trim whitespace: Unicode White_Space without NEL, plus BOM.
@inline(__always)
private func isJsSpace(_ c: UInt32) -> Bool {
    switch c {
    case 0x09...0x0d, 0x20, 0xa0, 0x1680, 0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
        return true
    default: return false
    }
}

/// The scalar starting at `s[i]` (valid UTF-8) and its length.
@inline(__always)
private func scalar(_ s: Slice, _ i: Int) -> (UInt32, Int) {
    let b = UInt32(s[i])
    if b < 0x80 { return (b, 1) }
    if b < 0xe0 { return ((b & 0x1f) << 6 | UInt32(s[i + 1] & 0x3f), 2) }
    if b < 0xf0 { return ((b & 0x0f) << 12 | UInt32(s[i + 1] & 0x3f) << 6 | UInt32(s[i + 2] & 0x3f), 3) }
    return ((b & 0x07) << 18 | UInt32(s[i + 1] & 0x3f) << 12 | UInt32(s[i + 2] & 0x3f) << 6 | UInt32(s[i + 3] & 0x3f), 4)
}

private func jsTrim(_ s: Slice) -> Slice {
    var a = 0
    var b = s.n
    while a < b {
        let (cp, len) = scalar(s, a)
        if !isJsSpace(cp) { break }
        a += len
    }
    while b > a {
        var k = b - 1
        while k > a && s[k] & 0xc0 == 0x80 { k -= 1 }
        if !isJsSpace(scalar(s, k).0) { break }
        b = k
    }
    return s.sub(a, b)
}

/// Unicode scalars in valid UTF-8.
private func scalarCount(_ s: Slice) -> Int {
    var n = 0
    for i in 0..<s.n where s[i] & 0xc0 != 0x80 { n += 1 }
    return n
}

final class App {
    let db: Db
    private let jwt: Verifier
    private let started: Int
    private var unescaped = [UInt8]()
    private let created = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)

    init(db: Db, secret: Slice) {
        self.db = db
        jwt = Verifier(secret: secret)
        started = App.monotonic()
        unescaped.reserveCapacity(2048)
    }

    private static func monotonic() -> Int {
        var ts = timespec()
        clock_gettime(CLOCK_MONOTONIC, &ts)
        return Int(ts.tv_sec)
    }

    /// Handles one request; the response is appended to `out`.
    func handle(_ req: Request, _ out: Out) {
        let p = req.path
        if req.method.equals("GET") {
            if p.equals("/feed") {
                let mark = out.len
                let b = start(out, 200)
                do { try db.feed(out) } catch {
                    out.truncate(mark)
                    return writeError(out, 500, "internal server error")
                }
                finish(out, b)
            } else if p.hasPrefix("/posts/"), !p.from(7).contains(0x2f) {
                guard let id = parsePositive(p.from(7)) else {
                    return writeError(out, 400, "invalid post id")
                }
                let mark = out.len
                let b = start(out, 200)
                do {
                    switch try db.post(id, out) {
                    case .found: finish(out, b)
                    case .missing:
                        out.truncate(mark)
                        writeError(out, 404, "post not found")
                    }
                } catch {
                    out.truncate(mark)
                    writeError(out, 500, "internal server error")
                }
            } else if p.equals("/health") {
                health(out)
            } else {
                writeError(out, 404, "not found")
            }
        } else if req.method.equals("POST") {
            if p.equals("/posts") {
                create(req, out)
            } else if p.hasPrefix("/posts/") && p.n >= 12 && p.sub(p.n - 5, p.n).equals("/like") {
                let id = p.sub(7, p.n - 5)
                if id.contains(0x2f) { return writeError(out, 404, "not found") }
                like(req, id, out)
            } else {
                writeError(out, 404, "not found")
            }
        } else {
            writeError(out, 404, "not found")
        }
    }

    private func health(_ out: Out) {
        if var err = db.health() {
            let b = start(out, 503)
            out.lit("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"")
            err.withUTF8 { out.esc(Slice(p: $0.baseAddress ?? UnsafePointer(emptyBytes), n: $0.count)) }
            out.lit("\"}")
            finish(out, b)
        } else {
            let b = start(out, 200)
            out.lit("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
            out.int(Int64(App.monotonic() - started))
            out.byte(0x7d)
            finish(out, b)
        }
    }

    /// true if the token is good; otherwise the error response is already written.
    private func auth(_ req: Request, _ out: Out) -> Bool {
        switch jwt.verify(hasAuth: req.hasAuth, header: req.auth, now: nowSecs()) {
        case nil: return true
        case .missing?: writeError(out, 401, "missing bearer token")
        case .invalid?: writeError(out, 401, "invalid or expired token")
        case .payload?: writeError(out, 401, "invalid token payload")
        }
        return false
    }

    private func create(_ req: Request, _ out: Out) {
        if !auth(req, out) { return }
        let uid = jwt.userId
        var body: (Slice, Bool)?
        let parsed: Bool
        do {
            parsed = try parseObject(req.body) { k, v in
                if k.equals("body") {
                    if case .str(let s, let e) = v { body = (s, e) } else { body = nil }
                }
            }
        } catch { return writeError(out, 400, "malformed JSON body") }
        if !parsed { return writeError(out, 400, "body is required") }
        guard let (raw, esc) = body else { return writeError(out, 400, "body is required") }
        if esc {
            unescaped.removeAll(keepingCapacity: true)
            unescape(raw, into: &unescaped)
            unescaped.withUnsafeBufferPointer { b in
                insert(Slice(p: b.baseAddress ?? UnsafePointer(emptyBytes), n: b.count), uid, out)
            }
        } else {
            insert(raw, uid, out)
        }
    }

    /// Trims, validates and stores a post whose text is valid UTF-8.
    private func insert(_ text: Slice, _ uid: Int64, _ out: Out) {
        let s = jsTrim(text)
        if s.n == 0 { return writeError(out, 400, "body is required") }
        if s.n > maxBodyChars && scalarCount(s) > maxBodyChars {
            return writeError(out, 400, "body must be at most 500 characters")
        }
        do {
            let (id, n) = try db.insertPost(uid, s, created)
            let b = start(out, 201)
            out.lit("{\"post\":{\"id\":")
            out.int(id)
            out.lit(",\"body\":\"")
            out.esc(s)
            out.lit("\",\"created_at\":\"")
            out.esc(Slice(p: UnsafePointer(created), n: n))
            out.lit("\",\"author\":\"")
            out.esc(Slice(p: UnsafePointer(jwt.username), n: jwt.usernameLen))
            out.lit("\",\"like_count\":0}}")
            finish(out, b)
        } catch {
            writeError(out, 500, "internal server error")
        }
    }

    private func like(_ req: Request, _ idText: Slice, _ out: Out) {
        if !auth(req, out) { return }
        let uid = jwt.userId
        guard let id = parsePositive(idText) else { return writeError(out, 400, "invalid post id") }
        do {
            if !(try db.postExists(id)) { return writeError(out, 404, "post not found") }
            let new = try db.insertLike(uid, id)
            let b = start(out, new ? 201 : 200)
            if new {
                out.lit("{\"liked\":true,\"already_liked\":false,\"post_id\":")
            } else {
                out.lit("{\"liked\":true,\"already_liked\":true,\"post_id\":")
            }
            out.int(id)
            out.byte(0x7d)
            finish(out, b)
        } catch {
            writeError(out, 500, "internal server error")
        }
    }
}
