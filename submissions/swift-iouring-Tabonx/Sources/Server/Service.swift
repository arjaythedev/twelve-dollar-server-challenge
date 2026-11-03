// Routing and the five endpoints. The server hands in a parsed request and gets back the JSON body,
// or calls `appendHTTPResponse` for the full HTTP/1.1 response bytes.

import CShim

public enum Method: UInt8 {
    case get, post, other
}

/// A parsed request. Every view points into the receive buffer.
public struct Request {
    public var method: Method
    /// Path without the query string.
    public var path: ByteView
    public var authorization: ByteView?
    public var body: ByteView
    public var keepAlive: Bool

    public init(method: Method, path: ByteView, authorization: ByteView?, body: ByteView, keepAlive: Bool = true) {
        self.method = method
        self.path = path
        self.authorization = authorization
        self.body = body
        self.keepAlive = keepAlive
    }
}

/// The application. Not thread-safe: SQLite is single-threaded and the JWT verifier owns scratch buffers.
public final class FeedService {
    let db: Database
    var jwt: JWTVerifier
    var bodyScratch: OutBuffer
    let startedAt: Int64

    public init?(sqlitePath: UnsafePointer<CChar>, secret: ByteView, error: inout OutBuffer) {
        guard let db = Database(path: sqlitePath, error: &error) else { return nil }
        self.db = db
        jwt = JWTVerifier(secret: secret)
        bodyScratch = OutBuffer(capacity: 2048)
        startedAt = sv_monotonic_s()
    }

    // MARK: Batches

    /// Starts sharing one transaction across the following requests. Their responses must not be sent
    /// until `endBatch` returns true.
    public func beginBatch() -> Bool { db.beginTransaction() }

    /// Commits the batch. False means it was rolled back and every response in it must become a 500.
    public func endBatch() -> Bool { db.commitTransaction() }

    /// Whether a WAL checkpoint is due. Read without the lock as a hint; run it under the lock.
    public var checkpointDue: Bool { db.checkpointDue }

    public func checkpointIfDue() { db.checkpointIfDue() }

    // MARK: Requests

    /// Appends the JSON body for `r` to `out` and returns the status code.
    public func respond(_ r: Request, body out: inout OutBuffer) -> Int {
        let path = r.path
        let mark = out.count
        let status: Int
        if path.equals("/feed") {
            status = r.method == .get ? feed(&out) : notFound(&out)
        } else if path.hasPrefix("/posts/") {
            let rest = path.slice(7, path.count)
            if r.method == .post, rest.count > 5, rest.slice(rest.count &- 5, rest.count).equals("/like") {
                status = like(r, rest.slice(0, rest.count &- 5), &out)
            } else if r.method == .get, rest.count > 0, !contains(rest, UInt8(ascii: "/")) {
                status = getPost(rest, &out)
            } else {
                status = notFound(&out)
            }
        } else if path.equals("/posts") {
            status = r.method == .post ? createPost(r, &out) : notFound(&out)
        } else if path.equals("/health") {
            status = r.method == .get ? health(&out) : notFound(&out)
        } else {
            status = notFound(&out)
        }
        if status == 500 {
            out.truncate(mark)
            out.append("{\"error\":\"internal server error\"}")
        }
        return status
    }

    func notFound(_ out: inout OutBuffer) -> Int {
        out.append("{\"error\":\"not found\"}")
        return 404
    }

    func health(_ out: inout OutBuffer) -> Int {
        if db.healthCheck() {
            out.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
            out.appendInt(sv_monotonic_s() &- startedAt)
            out.append(UInt8(ascii: "}"))
            return 200
        }
        bodyScratch.truncate(0)
        db.appendError(&bodyScratch)
        out.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
        out.appendJSONString(bodyScratch.view)
        out.append(UInt8(ascii: "}"))
        return 503
    }

    func feed(_ out: inout OutBuffer) -> Int {
        db.writeFeed(&out) ? 200 : 500
    }

    func getPost(_ idText: ByteView, _ out: inout OutBuffer) -> Int {
        guard let id = parsePositiveInt(idText) else {
            out.append("{\"error\":\"invalid post id\"}")
            return 400
        }
        switch db.writePost(id, &out) {
        case .found: return 200
        case .notFound:
            out.append("{\"error\":\"post not found\"}")
            return 404
        case .failed: return 500
        }
    }

    /// Writes the 401 body for a failed check, or returns the user.
    func authenticate(_ r: Request, _ out: inout OutBuffer) -> (id: Int64, username: ByteView)? {
        switch jwt.verify(authorization: r.authorization, nowMs: sv_now_ms()) {
        case .missingBearer:
            out.append("{\"error\":\"missing bearer token\"}")
        case .invalidToken:
            out.append("{\"error\":\"invalid or expired token\"}")
        case .invalidPayload:
            out.append("{\"error\":\"invalid token payload\"}")
        case .user(let id, let username):
            return (id, username)
        }
        return nil
    }

    func createPost(_ r: Request, _ out: inout OutBuffer) -> Int {
        guard let user = authenticate(r, &out) else { return 401 }

        var bodyValue: ByteView? = nil
        var bodyEscaped = false
        let doc = scanJSONObject(r.body) { m in
            if keyIs(m, "body") {
                bodyValue = m.kind == .string ? m.value : nil
                bodyEscaped = m.escaped
            }
        }
        if doc == .malformed {
            out.append("{\"error\":\"malformed JSON body\"}")
            return 400
        }
        guard doc == .object, var text = bodyValue else {
            out.append("{\"error\":\"body is required\"}")
            return 400
        }
        if bodyEscaped {
            bodyScratch.truncate(0)
            unescapeJSONString(text, into: &bodyScratch)
            text = bodyScratch.view
        }
        text = trimWhitespace(text)
        if text.count == 0 {
            out.append("{\"error\":\"body is required\"}")
            return 400
        }
        if scalarCount(text) > 500 {
            out.append("{\"error\":\"body must be at most 500 characters\"}")
            return 400
        }
        let mark = out.count
        guard db.createPost(userID: user.id, body: text, author: user.username, &out) else {
            out.truncate(mark)
            return 500
        }
        return 201
    }

    func like(_ r: Request, _ idText: ByteView, _ out: inout OutBuffer) -> Int {
        guard let user = authenticate(r, &out) else { return 401 }
        guard let postID = parsePositiveInt(idText) else {
            out.append("{\"error\":\"invalid post id\"}")
            return 400
        }
        let result = db.like(userID: user.id, postID: postID)
        switch result {
        case .created, .already:
            out.append("{\"liked\":true,\"already_liked\":")
            out.append(result == .already ? "true" : "false")
            out.append(",\"post_id\":")
            out.appendInt(postID)
            out.append(UInt8(ascii: "}"))
            return result == .already ? 200 : 201
        case .missingPost:
            out.append("{\"error\":\"post not found\"}")
            return 404
        case .failed:
            return 500
        }
    }
}

@inline(__always)
func contains(_ v: ByteView, _ b: UInt8) -> Bool {
    for i in 0..<v.count where v[i] == b { return true }
    return false
}

// MARK: Text helpers

/// Number of Unicode scalars in valid UTF-8: every byte that isn't a continuation byte.
func scalarCount(_ v: ByteView) -> Int {
    var n = 0
    for i in 0..<v.count where v[i] & 0xC0 != 0x80 { n &+= 1 }
    return n
}

/// JavaScript's String.prototype.trim set: ASCII whitespace, NBSP, the Unicode space separators,
/// line/paragraph separators and the BOM.
@inline(__always)
func isTrimmable(_ cp: UInt32) -> Bool {
    switch cp {
    case 0x09...0x0D, 0x20, 0xA0, 0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
        return true
    default:
        return false
    }
}

/// Decodes the scalar starting at `i`. Returns (scalar, length); invalid bytes decode as (0xFFFD, 1).
func decodeScalar(_ v: ByteView, _ i: Int) -> (UInt32, Int) {
    let b0 = UInt32(v[i])
    if b0 < 0x80 { return (b0, 1) }
    let n = v.count &- i
    if b0 & 0xE0 == 0xC0, n >= 2 { return ((b0 & 0x1F) << 6 | UInt32(v[i &+ 1] & 0x3F), 2) }
    if b0 & 0xF0 == 0xE0, n >= 3 {
        return ((b0 & 0x0F) << 12 | UInt32(v[i &+ 1] & 0x3F) << 6 | UInt32(v[i &+ 2] & 0x3F), 3)
    }
    if b0 & 0xF8 == 0xF0, n >= 4 {
        return ((b0 & 0x07) << 18 | UInt32(v[i &+ 1] & 0x3F) << 12 | UInt32(v[i &+ 2] & 0x3F) << 6
            | UInt32(v[i &+ 3] & 0x3F), 4)
    }
    return (0xFFFD, 1)
}

func trimWhitespace(_ v: ByteView) -> ByteView {
    var start = 0
    var end = v.count
    while start < end {
        let b = v[start]
        if b < 0x80 {
            guard b == 0x20 || (b >= 0x09 && b <= 0x0D) else { break }
            start &+= 1
        } else {
            let (cp, len) = decodeScalar(v, start)
            guard isTrimmable(cp) else { break }
            start &+= len
        }
    }
    while end > start {
        let b = v[end &- 1]
        if b < 0x80 {
            guard b == 0x20 || (b >= 0x09 && b <= 0x0D) else { break }
            end &-= 1
        } else {
            var s = end &- 1
            while s > start && v[s] & 0xC0 == 0x80 { s &-= 1 }
            let (cp, len) = decodeScalar(v, s)
            guard s &+ len == end, isTrimmable(cp) else { break }
            end = s
        }
    }
    return v.slice(start, end)
}
