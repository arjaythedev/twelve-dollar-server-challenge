// HTTP/1.1 request framing. Only the parts the API uses are looked at: the request line,
// Authorization and Content-Length. One pass over the lines finds them and the end of the head.

struct Request {
    var method = emptySlice
    var path = emptySlice
    var hasAuth = false
    var auth = emptySlice
    var body = emptySlice
}

enum Parsed {
    /// A whole request and the bytes it took.
    case done(Int)
    case partial
    case bad
}

private let maxHead = 16 * 1024
private let maxBody = 1024 * 1024

@inline(__always)
private func trim(_ s: Slice) -> Slice {
    var a = 0
    var b = s.n
    while a < b && (s[a] == 0x20 || s[a] == 0x09) { a += 1 }
    while b > a && (s[b - 1] == 0x20 || s[b - 1] == 0x09) { b -= 1 }
    return s.sub(a, b)
}

/// `line` without a trailing CR.
@inline(__always)
private func chomp(_ s: Slice) -> Slice {
    s.n > 0 && s[s.n - 1] == 0x0d ? Slice(p: s.p, n: s.n - 1) : s
}

func parseRequest(_ buf: Slice, _ req: inout Request) -> Parsed {
    // Request line.
    let nl = buf.find(0x0a)
    if nl < 0 { return buf.n > maxHead ? .bad : .partial }
    let line = chomp(Slice(p: buf.p, n: nl))
    let sp1 = line.find(0x20)
    if sp1 < 0 { return .bad }
    let method = Slice(p: line.p, n: sp1)
    let rest = line.from(sp1 + 1)
    let sp2 = rest.find(0x20)
    if sp2 < 0 { return .bad }
    let target = Slice(p: rest.p, n: sp2)
    if !rest.from(sp2 + 1).hasPrefix("HTTP/1.") || method.n == 0 || target.n == 0 { return .bad }
    let q = target.find(0x3f)
    let path = q >= 0 ? Slice(p: target.p, n: q) : target
    // Headers, up to the empty line.
    var hasAuth = false
    var auth = emptySlice
    var len = 0
    var pos = nl + 1
    while true {
        let n = buf.from(pos).find(0x0a)
        if n < 0 { return buf.n > maxHead ? .bad : .partial }
        let l = chomp(Slice(p: buf.p + pos, n: n))
        pos += n + 1
        if l.n == 0 { break }
        let colon = l.find(0x3a)
        if colon < 0 { continue }
        let name = Slice(p: l.p, n: colon)
        switch name.n {
        case 13 where name.equalsIgnoreCase("authorization"):
            hasAuth = true
            auth = trim(l.from(colon + 1))
        case 14 where name.equalsIgnoreCase("content-length"):
            let value = trim(l.from(colon + 1))
            if value.n == 0 || value.n > 9 { return .bad }
            var v = 0
            for i in 0..<value.n {
                let d = value[i] &- 0x30
                if d > 9 { return .bad }
                v = v * 10 + Int(d)
            }
            if v > maxBody { return .bad }
            len = v
        case 17 where name.equalsIgnoreCase("transfer-encoding"):
            return .bad
        default: break
        }
    }
    if buf.n < pos + len { return .partial }
    req = Request(method: method, path: path, hasAuth: hasAuth, auth: auth, body: Slice(p: buf.p + pos, n: len))
    return .done(pos + len)
}
