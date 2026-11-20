// Reads through SQLite's B-tree cursors (see Sources/CShim/shim.c) and writes through SQL. Every
// response is built from rows read while the request is served, inside the batch's transaction;
// nothing is kept between requests except prepared statements and cursors.
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif
import CShim

struct DbError: Error {}

private let posts = 0, users = 1, feedIdx = 2, likesIdx = 3
private let feedLen = 20
/// Count likes with one index range walk when the feed's ids span at most this many ids.
private let rangeSpan: Int64 = 512

typealias Cur = UnsafeMutableRawPointer

/// One column of a record: serial type and bytes.
struct Col {
    var t: UInt64 = 0
    var p: UnsafePointer<UInt8> = UnsafePointer(emptyBytes)
    var n = 0

    @inline(__always) var slice: Slice { Slice(p: p, n: n) }
}

/// The current row of a cursor, as record bytes.
@inline(__always)
private func payload(_ c: Cur) throws -> Slice {
    var n: UInt32 = 0
    guard let p = td_payload(c, &n) else { throw DbError() }
    return Slice(p: p, n: Int(n))
}

@inline(__always)
private func varint(_ p: UnsafePointer<UInt8>, _ lim: Int, _ i: inout Int) throws -> UInt64 {
    // One-byte varints (every serial type below 64 bytes of text) take the short path.
    if i < lim {
        let b = p[i]
        if b < 0x80 {
            i += 1
            return UInt64(b)
        }
    }
    return try varintLong(p, lim, &i)
}

@inline(never)
private func varintLong(_ p: UnsafePointer<UInt8>, _ lim: Int, _ i: inout Int) throws -> UInt64 {
    var v: UInt64 = 0
    for k in 0..<9 {
        if i >= lim { throw DbError() }
        let b = p[i]
        i += 1
        if k == 8 { return v << 8 | UInt64(b) }
        v = v << 7 | UInt64(b & 0x7f)
        if b & 0x80 == 0 { return v }
    }
    return v
}

@inline(__always)
private func serialLen(_ t: UInt64) -> Int {
    switch t {
    case 0, 8, 9, 10, 11: return 0
    case 1: return 1
    case 2: return 2
    case 3: return 3
    case 4: return 4
    case 5: return 6
    case 6, 7: return 8
    default: return Int((t - 12) / 2)
    }
}

/// Column `c` as an integer (serial types 1-6, 8, 9).
@inline(__always)
private func intOf(_ c: Col) throws -> Int64 {
    switch c.t {
    case 8: return 0
    case 9: return 1
    case 1...6:
        let n = serialLen(c.t)
        var v = Int64(Int8(bitPattern: c.p[0]))
        var k = 1
        while k < n {
            v = v << 8 | Int64(c.p[k])
            k += 1
        }
        return v
    default: throw DbError()
    }
}

/// The first `n` columns of a record into `out`.
@inline(__always)
private func columns(_ rec: Slice, _ n: Int, _ out: UnsafeMutablePointer<Col>) throws {
    var i = 0
    let hsize = Int(truncatingIfNeeded: try varint(rec.p, rec.n, &i))
    if hsize < 0 || hsize > rec.n { throw DbError() }
    var body = hsize
    for k in 0..<n {
        if i >= hsize { throw DbError() }
        let t = try varint(rec.p, hsize, &i)
        let len = serialLen(t)
        if body + len > rec.n { throw DbError() }
        out[k] = Col(t: t, p: rec.p + body, n: len)
        body += len
    }
}

@inline(__always)
private func text(_ c: Col) throws -> Slice {
    if c.t >= 13 && c.t & 1 == 1 { return c.slice }
    throw DbError()
}

/// Likes of one post.
private func countLikes(_ c: Cur, _ id: Int64, _ cols: UnsafeMutablePointer<Col>) throws -> Int64 {
    var n: UInt32 = 0
    switch td_count_likes(c, id, id, &n) {
    case 0: return Int64(n)
    case 1: return try countLikesSlow(c, id, cols)
    default: throw DbError()
    }
}

/// Likes of one post, decoding each entry here: for cells td_count_likes doesn't expect.
private func countLikesSlow(_ c: Cur, _ id: Int64, _ cols: UnsafeMutablePointer<Col>) throws -> Int64 {
    var r = td_seek_ge(c, id)
    var n: Int64 = 0
    while r == 1 {
        try columns(try payload(c), 1, cols)
        if try intOf(cols[0]) != id { break }
        n += 1
        r = td_next(c)
    }
    if r < 0 { throw DbError() }
    return n
}

private struct Row {
    var id: Int64 = 0
    var userId: Int64 = 0
    var body = (0, 0)
    var created = (0, 0)
    var author = (0, 0)
    var likes: Int64 = 0
}

enum PostLookup {
    case found
    case missing
}

final class Db {
    private let cp: Cur, cu: Cur, ci: Cur, cl: Cur
    private let cols = UnsafeMutablePointer<Col>.allocate(capacity: 4)
    private let rows = UnsafeMutablePointer<Row>.allocate(capacity: feedLen)
    private var rowCount = 0
    private let order = UnsafeMutablePointer<Int>.allocate(capacity: feedLen)
    private let counts = UnsafeMutablePointer<UInt32>.allocate(capacity: Int(rangeSpan) + 1)
    /// Copies of the feed rows' text, so each cursor can move on.
    private var textBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: 16384)
    private var textCap = 16384
    private var textLen = 0

    init(path: String) throws {
        var err = [CChar](repeating: 0, count: 256)
        if td_open(path, &err, 256) != 0 { throw OpenError(message: String(cString: err)) }
        cp = td_cursor(Int32(posts))!
        cu = td_cursor(Int32(users))!
        ci = td_cursor(Int32(feedIdx))!
        cl = td_cursor(Int32(likesIdx))!
        cols.initialize(repeating: Col(), count: 4)
        rows.initialize(repeating: Row(), count: feedLen)
    }

    struct OpenError: Error { let message: String }

    /// Joins the batch's transaction, starting it if needed. Called by every request that
    /// touches the database, so one that SQLite rolled back mid-pass is never written to.
    @inline(__always)
    private func begin() throws {
        if td_begin() != 0 { throw DbError() }
    }

    /// Writes must wait: the checkpoint thread is copying the log's last frames.
    @inline(__always) var writesHeld: Bool { td_writes_held() != 0 }

    /// Returns nil when the database answers `SELECT 1`, else the error message.
    func health() -> String? {
        do { try begin() } catch { return "cannot begin transaction" }
        var err = [CChar](repeating: 0, count: 256)
        if td_health(&err, 256) != 0 { return String(cString: err) }
        return nil
    }

    @inline(__always)
    private func addText(_ s: Slice) -> (Int, Int) {
        if textLen + s.n > textCap {
            var nc = textCap * 2
            while nc < textLen + s.n { nc *= 2 }
            let nb = UnsafeMutablePointer<UInt8>.allocate(capacity: nc)
            memcpy(nb, textBuf, textLen)
            textBuf.deallocate()
            textBuf = nb
            textCap = nc
        }
        let start = textLen
        memcpy(textBuf + textLen, s.p, s.n)
        textLen += s.n
        return (start, textLen)
    }

    @inline(__always)
    private func tslice(_ r: (Int, Int)) -> Slice {
        Slice(p: UnsafePointer(textBuf + r.0), n: r.1 - r.0)
    }

    /// Appends the username of `userId` to `out`, escaped. false if there is no such user.
    private func author(_ userId: Int64, _ out: Out) throws -> Bool {
        switch td_seek(cu, userId) {
        case 1: break
        case 0: return false
        default: throw DbError()
        }
        try columns(try payload(cu), 2, cols)
        out.esc(try text(cols[1]))
        return true
    }

    /// `{"post":{...}}` for GET /posts/:id.
    func post(_ id: Int64, _ out: Out) throws -> PostLookup {
        try begin()
        switch td_seek(cp, id) {
        case 1: break
        case 0: return .missing
        default: throw DbError()
        }
        try columns(try payload(cp), 4, cols)
        let userId = try intOf(cols[1])
        let mark = out.len
        out.lit("{\"post\":{\"id\":")
        out.int(id)
        out.lit(",\"body\":\"")
        out.esc(try text(cols[2]))
        out.lit("\",\"created_at\":\"")
        out.esc(try text(cols[3]))
        out.lit("\",\"author\":\"")
        // The post's cell is no longer needed, so the users cursor may move freely now.
        if !(try author(userId, out)) {
            out.truncate(mark)  // the JOIN finds no row
            return .missing
        }
        out.lit("\",\"like_count\":")
        out.int(try countLikes(cl, id, cols))
        out.lit("}}")
        return .found
    }

    /// Whether post `id` exists (for likes).
    func postExists(_ id: Int64) throws -> Bool {
        try begin()
        switch td_seek(cp, id) {
        case 1: return true
        case 0: return false
        default: throw DbError()
        }
    }

    /// `{"posts":[...]}`: the 20 newest by (created_at DESC, id DESC), read off that index.
    func feed(_ out: Out) throws {
        try begin()
        if !(try feedRows()) {
            try feedSQL(out)  // data the B-tree plan doesn't expect: let SQL answer
            return
        }
        feedJSON(out)
    }

    /// Fills `rows`. false if the plain query should answer instead.
    private func feedRows() throws -> Bool {
        rowCount = 0
        textLen = 0
        // 1. The index: (created_at, id) newest first.
        var r = td_first(ci)
        while r == 1 && rowCount < feedLen {
            try columns(try payload(ci), 3, cols)
            var row = Row()
            row.created = addText(try text(cols[0]))
            row.id = try intOf(cols[1])
            rows[rowCount] = row
            rowCount += 1
            r = td_next(ci)
        }
        if r < 0 { throw DbError() }
        // 2. The posts, in ascending id order: consecutive ids are one step along the leaf.
        // Newest-first is normally descending id, so ascending is the reverse.
        for k in 0..<rowCount { order[k] = rowCount - 1 - k }
        var sorted = true
        if rowCount > 1 {
            for k in 0..<(rowCount - 1) where rows[order[k]].id >= rows[order[k + 1]].id {
                sorted = false
                break
            }
        }
        if !sorted {
            // Insertion sort: at most 20 entries.
            var a = 1
            while a < rowCount {
                let v = order[a]
                var b = a
                while b > 0 && rows[order[b - 1]].id > rows[v].id {
                    order[b] = order[b - 1]
                    b -= 1
                }
                order[b] = v
                a += 1
            }
        }
        for k in 0..<rowCount {
            let i = order[k]
            if td_seek(cp, rows[i].id) != 1 { return false }  // index and table disagree
            try columns(try payload(cp), 3, cols)
            rows[i].userId = try intOf(cols[1])
            rows[i].body = addText(try text(cols[2]))
        }
        // 3. Authors.
        for i in 0..<rowCount {
            switch td_seek(cu, rows[i].userId) {
            case 1: break
            case 0: return false  // a post without a user: the JOIN drops it
            default: throw DbError()
            }
            try columns(try payload(cu), 2, cols)
            rows[i].author = addText(try text(cols[1]))
        }
        // 4. Like counts: one walk over likes_post_id_idx when the ids are close together.
        if rowCount > 0 {
            let lo = rows[order[0]].id, hi = rows[order[rowCount - 1]].id
            if hi - lo <= rangeSpan {
                memset(counts, 0, (Int(hi - lo) + 1) * MemoryLayout<UInt32>.size)
                switch td_count_likes(cl, lo, hi, counts) {
                case 0:
                    for i in 0..<rowCount { rows[i].likes = Int64(counts[Int(rows[i].id - lo)]) }
                case 1:
                    for i in 0..<rowCount { rows[i].likes = try countLikesSlow(cl, rows[i].id, cols) }
                default: throw DbError()
                }
            } else {
                for i in 0..<rowCount { rows[i].likes = try countLikes(cl, rows[i].id, cols) }
            }
        }
        return true
    }

    /// Written in one reservation: every row's strings live in `textBuf`, so 6x its length
    /// bounds their escaped form.
    private func feedJSON(_ out: Out) {
        let rowTemplate: StaticString = ",{\"id\":,\"body\":\"\",\"created_at\":\"\",\"author\":\"\",\"like_count\":}"
        let bound = 12 + rowCount * (rowTemplate.utf8CodeUnitCount + 40) + 6 * textLen
        var c = out.cursor(bound: bound)
        c.lit("{\"posts\":[")
        for k in 0..<rowCount {
            let row = rows[k]
            if k > 0 { c.byte(0x2c) }
            c.lit("{\"id\":")
            c.int(row.id)
            c.lit(",\"body\":\"")
            c.esc(tslice(row.body))
            c.lit("\",\"created_at\":\"")
            c.esc(tslice(row.created))
            c.lit("\",\"author\":\"")
            c.esc(tslice(row.author))
            c.lit("\",\"like_count\":")
            c.int(row.likes)
            c.byte(0x7d)
        }
        c.lit("]}")
        out.commit(c)
    }

    /// The reference query, for data the B-tree plan doesn't expect.
    private func feedSQL(_ out: Out) throws {
        out.lit("{\"posts\":[")
        var first = true
        while true {
            var id: Int64 = 0, likes: Int64 = 0
            var b: UnsafePointer<UInt8>?, c: UnsafePointer<UInt8>?, a: UnsafePointer<UInt8>?
            var bl: Int32 = 0, cl: Int32 = 0, al: Int32 = 0
            let r = td_feed_sql_step(&id, &b, &bl, &c, &cl, &a, &al, &likes)
            if r == 0 { break }
            if r < 0 { throw DbError() }
            func s(_ p: UnsafePointer<UInt8>?, _ n: Int32) -> Slice {
                p == nil ? emptySlice : Slice(p: p!, n: Int(n))
            }
            if !first { out.byte(0x2c) }
            first = false
            out.lit("{\"id\":")
            out.int(id)
            out.lit(",\"body\":\"")
            out.esc(s(b, bl))
            out.lit("\",\"created_at\":\"")
            out.esc(s(c, cl))
            out.lit("\",\"author\":\"")
            out.esc(s(a, al))
            out.lit("\",\"like_count\":")
            out.int(likes)
            out.byte(0x7d)
        }
        out.lit("]}")
    }

    /// INSERT a post. Returns the new id and the stored created_at (in `created`, `n` bytes).
    func insertPost(_ userId: Int64, _ body: Slice, _ created: UnsafeMutablePointer<UInt8>) throws -> (Int64, Int) {
        try begin()
        var id: Int64 = 0
        var n: Int32 = 0
        if td_insert_post(userId, body.p, Int32(body.n), &id, created, &n) != 0 { throw DbError() }
        return (id, Int(n))
    }

    /// true if the like is new.
    func insertLike(_ userId: Int64, _ postId: Int64) throws -> Bool {
        try begin()
        switch td_insert_like(userId, postId) {
        case 1: return true
        case 0: return false
        default: throw DbError()
        }
    }
}
