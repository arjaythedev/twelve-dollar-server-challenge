// Reads through SQLite's B-tree cursors (Sources/CSQLite/db.c), writes through SQL.
//
// Every response is built from rows read while the request is served, inside the pass's transaction.
// Nothing survives between requests except prepared statements, cursors and the root page numbers.
// The record format is SQLite's: a varint header size, one varint serial type per column, then the
// column bytes (https://sqlite.org/fileformat.html#record_format).

import CSQLite

/// One column of a record: SQLite serial type and the byte range of its value.
struct Field {
    var type: UInt64 = 0
    var start: UnsafePointer<UInt8>? = nil
    var count: Int = 0

    var isText: Bool { type >= 13 && type & 1 == 1 }

    var bytes: ByteView { ByteView(base: start, count: count) }

    /// The value of an integer column (serial types 1 to 6, 8 and 9).
    var integer: Int64? {
        switch type {
        case 8: return 0
        case 9: return 1
        case 1...6:
            let p = start.unsafelyUnwrapped
            var v = Int64(Int8(bitPattern: p[0]))
            for i in 1..<count { v = v << 8 | Int64(p[i]) }
            return v
        default: return nil
        }
    }
}

@inline(__always)
func serialSize(_ t: UInt64) -> Int {
    switch t {
    case 0, 8, 9, 10, 11: return 0
    case 1, 2, 3, 4: return Int(t)
    case 5: return 6
    case 6, 7: return 8
    default: return Int((t &- 12) >> 1)
    }
}

/// Reads one varint at `i`, moving `i` past it. Nil if it runs past `end`.
@inline(__always)
func readVarint(_ p: UnsafePointer<UInt8>, _ i: inout Int, _ end: Int) -> UInt64? {
    guard i < end else { return nil }
    let b = p[i]
    if b < 0x80 {
        i &+= 1
        return UInt64(b)
    }
    return readLongVarint(p, &i, end)
}

@inline(never)
func readLongVarint(_ p: UnsafePointer<UInt8>, _ i: inout Int, _ end: Int) -> UInt64? {
    var v: UInt64 = 0
    for k in 0..<9 {
        guard i < end else { return nil }
        let b = p[i]
        i &+= 1
        if k == 8 { return v << 8 | UInt64(b) }
        v = v << 7 | UInt64(b & 0x7F)
        if b & 0x80 == 0 { return v }
    }
    return v
}

/// Decodes the first `n` columns of the cursor's current record into `fields`. False if the record
/// is shorter or malformed.
@inline(__always)
func decodeRecord(_ cursor: Int32, _ fields: UnsafeMutablePointer<Field>, _ n: Int) -> Bool {
    var length: UInt32 = 0
    guard let p = fd_payload(cursor, &length) else { return false }
    let end = Int(length)
    var i = 0
    guard let header = readVarint(p, &i, end), Int(header) <= end else { return false }
    let headerEnd = Int(header)
    var body = headerEnd
    for k in 0..<n {
        guard let t = readVarint(p, &i, headerEnd) else { return false }
        let size = serialSize(t)
        guard body &+ size <= end else { return false }
        fields[k] = Field(type: t, start: p + body, count: size)
        body &+= size
    }
    return true
}

let feedLength = 20
/// Count the feed's likes with one index walk when its ids span at most this many.
let likeRangeSpan: Int64 = 512

public final class Database {
    var inTransaction = false
    let fields: UnsafeMutablePointer<Field>
    // The feed's rows, in feed order. Text is copied into `text` so each cursor can move on.
    let ids: UnsafeMutablePointer<Int64>
    let userIDs: UnsafeMutablePointer<Int64>
    let likes: UnsafeMutablePointer<Int64>
    let order: UnsafeMutablePointer<Int>
    let spans: UnsafeMutablePointer<Int> // per row: created, body, author as (start, end)
    let counts: UnsafeMutablePointer<UInt32>
    var text = OutBuffer(capacity: 16384)

    /// Opens the database. On failure, `error` holds the reason.
    public init?(path: UnsafePointer<CChar>, error: inout OutBuffer) {
        let message = UnsafeMutablePointer<CChar>.allocate(capacity: 256)
        defer { message.deallocate() }
        message[0] = 0
        if fd_open(path, message, 256) != 0 {
            appendCString(message, &error)
            return nil
        }
        fields = .allocate(capacity: 8)
        ids = .allocate(capacity: feedLength)
        userIDs = .allocate(capacity: feedLength)
        likes = .allocate(capacity: feedLength)
        order = .allocate(capacity: feedLength)
        spans = .allocate(capacity: feedLength &* 6)
        counts = .allocate(capacity: Int(likeRangeSpan) &+ 1)
    }

    // MARK: Transactions

    /// Joins the pass's transaction, starting it if needed. Every request that touches the database
    /// calls this, so one that SQLite rolled back mid-pass is never written to in autocommit mode.
    @inline(__always)
    func begin() -> Bool {
        inTransaction = true // commit() must run even if this fails, to report the lost pass
        return fd_begin() == 0
    }

    /// Ends the pass's transaction. False if the commit failed and the pass was rolled back.
    public func commit() -> Bool {
        guard inTransaction else { return true }
        inTransaction = false
        return fd_commit() == 0
    }

    /// Between passes, after the commit: drives the checkpoint thread.
    public func tick() { fd_tick() }

    /// Writes must wait: the checkpoint thread is copying the end of the log.
    public var writesHeld: Bool { fd_writes_held() != 0 }

    // MARK: Queries

    func health(_ error: inout OutBuffer) -> Bool {
        guard begin() else {
            error.append("cannot begin transaction")
            return false
        }
        let message = UnsafeMutablePointer<CChar>.allocate(capacity: 256)
        defer { message.deallocate() }
        if fd_health(message, 256) != 0 {
            appendCString(message, &error)
            return false
        }
        return true
    }

    enum Lookup { case found, notFound, failed }

    /// Writes `{"post":{...}}` for one id.
    func writePost(_ id: Int64, _ out: inout OutBuffer) -> Lookup {
        guard begin() else { return .failed }
        switch fd_seek(Int32(FD_POSTS), id) {
        case 1: break
        case 0: return .notFound
        default: return .failed
        }
        guard decodeRecord(Int32(FD_POSTS), fields, 4), let userID = fields[1].integer,
              fields[2].isText, fields[3].isText else { return .failed }
        let mark = out.count
        out.append("{\"post\":{\"id\":")
        out.appendInt(id)
        out.append(",\"body\":")
        out.appendJSONString(fields[2].bytes)
        out.append(",\"created_at\":")
        out.appendJSONString(fields[3].bytes)
        out.append(",\"author\":")
        // The post's bytes are written, so the users cursor may move.
        switch fd_seek(Int32(FD_USERS), userID) {
        case 1: break
        case 0:
            out.truncate(mark) // the JOIN finds no row
            return .notFound
        default: return .failed
        }
        guard decodeRecord(Int32(FD_USERS), fields, 2), fields[1].isText else { return .failed }
        out.appendJSONString(fields[1].bytes)
        var count: UInt32 = 0
        guard fd_count_likes(id, id, &count) == 0 else { return .failed }
        out.append(",\"like_count\":")
        out.appendInt(Int64(count))
        out.append("}}")
        return .found
    }

    /// Writes `{"posts":[...]}`: the 20 newest by (created_at DESC, id DESC), read off that index.
    func writeFeed(_ out: inout OutBuffer) -> Bool {
        guard begin() else { return false }
        switch feedRows() {
        case .some(true):
            writeFeedJSON(&out)
            return true
        case .some(false):
            return writeFeedSQL(&out) // data the B-tree plan doesn't expect: SQL answers
        case .none:
            return false
        }
    }

    @inline(__always)
    func copyText(_ f: Field, _ row: Int, _ which: Int) {
        spans[row &* 6 &+ which &* 2] = text.count
        text.append(f.bytes)
        spans[row &* 6 &+ which &* 2 &+ 1] = text.count
    }

    @inline(__always)
    func span(_ row: Int, _ which: Int) -> ByteView {
        let s = spans[row &* 6 &+ which &* 2]
        return ByteView(base: UnsafePointer(text.base + s), count: spans[row &* 6 &+ which &* 2 &+ 1] &- s)
    }

    /// Fills the feed rows. True on success, false if the reference query should answer, nil on error.
    func feedRows() -> Bool? {
        text.truncate(0)
        let index = Int32(FD_FEED_IDX)
        var n = 0
        // 1. The index, newest first: (created_at, id, rowid).
        var r = fd_first(index)
        while r == 1 && n < feedLength {
            guard decodeRecord(index, fields, 2), fields[0].isText, let id = fields[1].integer else { return false }
            ids[n] = id
            copyText(fields[0], n, 0)
            n &+= 1
            r = fd_next(index)
        }
        if r < 0 { return nil }
        if n == 0 {
            feedCount = 0
            return true
        }
        // 2. The posts in ascending id order: consecutive ids are the next cell of the same leaf.
        // Newest first is normally descending id, so ascending is the reverse.
        var sorted = true
        for k in 0..<n {
            order[k] = n &- 1 &- k
            if k > 0 && ids[order[k]] <= ids[order[k &- 1]] { sorted = false }
        }
        if !sorted {
            for k in 1..<n {
                let v = order[k]
                var j = k
                while j > 0 && ids[order[j &- 1]] > ids[v] {
                    order[j] = order[j &- 1]
                    j &-= 1
                }
                order[j] = v
            }
        }
        let posts = Int32(FD_POSTS)
        for k in 0..<n {
            let row = order[k]
            let found = fd_seek(posts, ids[row])
            if found < 0 { return nil }
            if found == 0 { return false } // index and table disagree
            guard decodeRecord(posts, fields, 3), let userID = fields[1].integer, fields[2].isText else { return false }
            userIDs[row] = userID
            copyText(fields[2], row, 1)
        }
        // 3. Authors.
        let users = Int32(FD_USERS)
        for row in 0..<n {
            let found = fd_seek(users, userIDs[row])
            if found < 0 { return nil }
            if found == 0 { return false } // a post without a user: the JOIN drops it
            guard decodeRecord(users, fields, 2), fields[1].isText else { return false }
            copyText(fields[1], row, 2)
        }
        // 4. Like counts: one walk over likes_post_id_idx when the ids are close together.
        let lo = ids[order[0]]
        let hi = ids[order[n &- 1]]
        if hi &- lo <= likeRangeSpan {
            let width = Int(hi &- lo) &+ 1
            counts.initialize(repeating: 0, count: width)
            if fd_count_likes(lo, hi, counts) != 0 { return nil }
            for row in 0..<n { likes[row] = Int64(counts[Int(ids[row] &- lo)]) }
        } else {
            for row in 0..<n {
                var c: UInt32 = 0
                if fd_count_likes(ids[row], ids[row], &c) != 0 { return nil }
                likes[row] = Int64(c)
            }
        }
        feedCount = n
        return true
    }

    var feedCount = 0

    func writeFeedJSON(_ out: inout OutBuffer) {
        out.reserve(text.count &* 6 &+ feedCount &* 128 &+ 16)
        out.append("{\"posts\":[")
        for row in 0..<feedCount {
            if row > 0 { out.append(UInt8(ascii: ",")) }
            out.append("{\"id\":")
            out.appendInt(ids[row])
            out.append(",\"body\":")
            out.appendJSONString(span(row, 1))
            out.append(",\"created_at\":")
            out.appendJSONString(span(row, 0))
            out.append(",\"author\":")
            out.appendJSONString(span(row, 2))
            out.append(",\"like_count\":")
            out.appendInt(likes[row])
            out.append(UInt8(ascii: "}"))
        }
        out.append("]}")
    }

    /// The reference query, for data the B-tree plan doesn't expect.
    func writeFeedSQL(_ out: inout OutBuffer) -> Bool {
        out.append("{\"posts\":[")
        var first = true
        while true {
            var id: Int64 = 0, likeCount: Int64 = 0
            var body: UnsafePointer<UInt8>? = nil, created: UnsafePointer<UInt8>? = nil, author: UnsafePointer<UInt8>? = nil
            var bodyLength: Int32 = 0, createdLength: Int32 = 0, authorLength: Int32 = 0
            let r = fd_feed_sql_step(&id, &body, &bodyLength, &created, &createdLength, &author, &authorLength, &likeCount)
            if r == 0 { break }
            if r < 0 { return false }
            if !first { out.append(UInt8(ascii: ",")) }
            first = false
            out.append("{\"id\":")
            out.appendInt(id)
            out.append(",\"body\":")
            out.appendJSONString(ByteView(base: body, count: Int(bodyLength)))
            out.append(",\"created_at\":")
            out.appendJSONString(ByteView(base: created, count: Int(createdLength)))
            out.append(",\"author\":")
            out.appendJSONString(ByteView(base: author, count: Int(authorLength)))
            out.append(",\"like_count\":")
            out.appendInt(likeCount)
            out.append(UInt8(ascii: "}"))
        }
        out.append("]}")
        return true
    }

    /// Inserts a post and writes the 201 body. `body` is the decoded, trimmed text.
    func createPost(userID: Int64, body: ByteView, author: ByteView, _ out: inout OutBuffer) -> Bool {
        guard begin() else { return false }
        var id: Int64 = 0
        var created = (UInt64(0), UInt64(0), UInt64(0), UInt64(0), UInt64(0), UInt64(0), UInt64(0), UInt64(0))
        var createdLength: Int32 = 0
        let ok = withUnsafeMutableBytes(of: &created) { raw -> Bool in
            let p = raw.baseAddress.unsafelyUnwrapped.assumingMemoryBound(to: UInt8.self)
            guard fd_insert_post(userID, body.base, Int32(body.count), &id, p, &createdLength) == 0 else { return false }
            out.append("{\"post\":{\"id\":")
            out.appendInt(id)
            out.append(",\"body\":")
            out.appendJSONString(body)
            out.append(",\"created_at\":")
            out.appendJSONString(ByteView(base: UnsafePointer(p), count: Int(createdLength)))
            return true
        }
        guard ok else { return false }
        out.append(",\"author\":\"")
        out.append(author)
        out.append("\",\"like_count\":0}}")
        return true
    }

    enum LikeResult { case created, already, missingPost, failed }

    func like(userID: Int64, postID: Int64) -> LikeResult {
        guard begin() else { return .failed }
        switch fd_seek(Int32(FD_POSTS), postID) {
        case 1: break
        case 0: return .missingPost
        default: return .failed
        }
        switch fd_insert_like(userID, postID) {
        case 1: return .created
        case 0: return .already
        default: return .failed
        }
    }
}

func appendCString(_ s: UnsafePointer<CChar>, _ out: inout OutBuffer) {
    var p = UnsafeRawPointer(s).assumingMemoryBound(to: UInt8.self)
    while p.pointee != 0 {
        out.append(p.pointee)
        p += 1
    }
}
