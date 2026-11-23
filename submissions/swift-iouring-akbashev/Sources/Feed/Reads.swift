// The two read queries (and a post's existence, for a like) straight off SQLite's B-trees, as SQLite's own plan
// walks them but without its SQL engine. When the schema isn't the one these reads assume, or a row isn't in the form
// they expect, SQL answers instead.
import BTree
import CSQLite
import Syscalls

/// The read queries over four cursors: the posts and users tables (by rowid), posts_created_at_id_idx (newest first)
/// and likes_post_id_idx. The cursors open with the pass's first read and close before its commit.
nonisolated struct Reads: ~Copyable, ~Sendable {
    enum Answer {
        case done
        /// No such post.
        case missing
        /// Not in the form these reads expect (a post without its user, a record that spills): SQL answers.
        case unexpected
        case failed
    }

    /// The schema is the one these reads assume (else SQL answers every read).
    let available: Bool
    private var posts: Cursor
    private var users: Cursor
    private var newest: Cursor
    private var likes: Cursor
    private var isOpen = false
    /// The feed rows' texts, copied as each cursor moves on.
    private var text = ByteBuffer(capacity: 16 * 1024)

    /// A feed row: its post, author and likes, and where its texts are in `text`.
    private struct Row {
        var id: Int64 = 0
        var userID: Int64 = 0
        var likes: UInt32 = 0
        var body = 0..<0
        var created = 0..<0
        var author = 0..<0
    }

    /// Like counts seek with the one-column key (`checkOneColumnSeeks`).
    var oneColumnSeeks: Bool { likes.oneColumnSeeks }

    init(_ db: OpaquePointer, enabled: Bool) throws(StoreError) {
        let roots = enabled && Self.schemaMatches(db)
            ? (Self.root(db, "posts"), Self.root(db, "users"), Self.root(db, "posts_created_at_id_idx"), Self.root(db, "likes_post_id_idx"))
            : (0, 0, 0, 0)
        available = roots.0 != 0 && roots.1 != 0 && roots.2 != 0 && roots.3 != 0
        guard let posts = Cursor(db, root: roots.0), let users = Cursor(db, root: roots.1),
              let newest = Cursor(db, root: roots.2, keyColumns: 2),
              let likes = Cursor(db, root: roots.3, keyColumns: 1) else { throw StoreError(description: "btree cursors: out of memory") }
        self.posts = posts
        self.users = users
        self.newest = newest
        self.likes = likes
    }

    /// Frees the cursors, before the connection closes.
    mutating func release() {
        close()
        posts.release()
        users.release()
        newest.release()
        likes.release()
    }

    /// Before the pass's transaction ends.
    mutating func close() {
        guard isOpen else { return }
        posts.close()
        users.close()
        newest.close()
        likes.close()
        isOpen = false
    }

    private mutating func openIfNeeded() -> Bool {
        if isOpen { return true }
        guard posts.open(), users.open(), newest.open(), likes.open() else {
            posts.close()
            users.close()
            newest.close()
            likes.close()
            return false
        }
        isOpen = true
        return true
    }

    // ---------- the queries ----------

    /// `{"posts":[...]}`: the newest (created_at, id) entries off the index; then the posts in ascending id order, so each
    /// seek from the one before is a step along the leaf; their authors; their likes in one walk of the likes index;
    /// then the response in one reservation. The rows' texts are copied aside (`text`) as each cursor moves on.
    mutating func feed(into body: inout ByteBuffer) -> Answer {
        guard openIfNeeded() else { return .failed }
        text.removeAll()
        var rows = InlineArray<20, Row>(repeating: Row())
        var n = 0
        var move = newest.first()
        while move == .entry && n < rows.count {
            do {
                // The entry is borrowed from the cursor until the end of this block, before the cursor moves on.
                let entry = newest.record
                guard let columns: InlineArray<2, Record.Column> = Record.columns(entry), columns[0].isText,
                      let id = Record.integer(entry, columns[1]) else { return .unexpected }
                rows[n].id = id
                let from = text.count
                text.append(entry.extracting(columns[0].range))
                rows[n].created = from..<text.count
            }
            n += 1
            move = newest.next()
        }
        if move == .failed { return .failed }

        // Newest first is normally descending ids, so ascending is the reverse; otherwise sorted.
        var order = InlineArray<20, Int>(repeating: 0)
        for k in 0..<n { order[k] = n - 1 - k }
        var ascending = true
        for k in 0..<max(n - 1, 0) where rows[order[k]].id >= rows[order[k + 1]].id { ascending = false }
        if !ascending {
            for k in 1..<max(n, 1) {
                let i = order[k]
                var j = k
                while j > 0 && rows[order[j - 1]].id > rows[i].id {
                    order[j] = order[j - 1]
                    j -= 1
                }
                order[j] = i
            }
        }
        for k in 0..<n {
            let i = order[k]
            switch posts.seek(rows[i].id) {
            case .entry: break
            case .none: return .unexpected // the index and the table disagree
            case .failed: return .failed
            }
            let row = posts.record
            guard let columns: InlineArray<3, Record.Column> = Record.columns(row),
                  let userID = Record.integer(row, columns[1]), columns[2].isText else { return .unexpected }
            rows[i].userID = userID
            let from = text.count
            text.append(row.extracting(columns[2].range))
            rows[i].body = from..<text.count
        }
        // A post without its user is no row of the join: SQL answers.
        for i in 0..<n {
            switch users.seek(rows[i].userID) {
            case .entry: break
            case .none: return .unexpected
            case .failed: return .failed
            }
            let author = users.record
            guard let columns: InlineArray<2, Record.Column> = Record.columns(author), columns[1].isText else { return .unexpected }
            let from = text.count
            text.append(author.extracting(columns[1].range))
            rows[i].author = from..<text.count
        }

        // Their likes in one walk when their ids lie close together (new posts' ids follow one another).
        var lo = Int64.max, hi = Int64.min
        for i in 0..<n {
            lo = min(lo, rows[i].id)
            hi = max(hi, rows[i].id)
        }
        if n > 0 && hi - lo < 513 {
            var counts = InlineArray<513, UInt32>(repeating: 0)
            var span = counts.mutableSpan
            switch likes.count(lo, hi, into: &span) {
            case .done: break
            case .unexpected: return .unexpected
            case .failed: return .failed
            }
            for i in 0..<n { rows[i].likes = counts[Int(rows[i].id - lo)] }
        } else {
            for i in 0..<n {
                var one = InlineArray<1, UInt32>(repeating: 0)
                var span = one.mutableSpan
                switch likes.count(rows[i].id, rows[i].id, into: &span) {
                case .done: rows[i].likes = one[0]
                case .unexpected: return .unexpected
                case .failed: return .failed
                }
            }
        }

        // Each row's fixed part and two integers take under 112 bytes, and an escaped byte at most 6; an integer is
        // written with 32 bytes of room.
        let bound = 12 + 112 * n + 6 * text.count + 32
        let t = text.span
        body.append(maximum: bound) { out in
            out.append("{\"posts\":[")
            for i in 0..<n {
                if i > 0 { out.append(0x2C) }
                out.append("{\"id\":")
                out.appendDecimal(rows[i].id)
                out.append(",\"body\":\"")
                out.appendJSONStringContents(t.extracting(rows[i].body))
                out.append("\",\"created_at\":\"")
                out.appendJSONStringContents(t.extracting(rows[i].created))
                out.append("\",\"author\":\"")
                out.appendJSONStringContents(t.extracting(rows[i].author))
                out.append("\",\"like_count\":")
                out.appendDecimal(Int64(rows[i].likes))
                out.append(0x7D)
            }
            out.append("]}")
        }
        return .done
    }

    /// Turns the likes cursor's one-column seeks on if they count what the two-column key does, over the newest posts
    /// and a spread of others (so `Layout`'s offsets are SQLite's). Run once, inside a transaction; returns what didn't
    /// match, if anything.
    mutating func checkOneColumnSeeks() -> String? {
        guard available, openIfNeeded() else { return "no cursors" }
        defer { close() }
        var bases: [Int64] = [1, 2, 1_000, 123_457, 250_000, 499_990]
        if newest.first() == .entry {
            let entry = newest.record
            if let columns: InlineArray<2, Record.Column> = Record.columns(entry), let id = Record.integer(entry, columns[1]) {
                bases += [id - 63, id - 300]
            }
        }
        for base in bases where base > 0 {
            var expected = InlineArray<64, UInt32>(repeating: 0)
            var found = InlineArray<64, UInt32>(repeating: 0)
            likes.oneColumnSeeks = false
            var expectedSpan = expected.mutableSpan
            guard likes.count(base, base + 63, into: &expectedSpan) == .done else { return "counting from \(base)" }
            likes.oneColumnSeeks = true
            var foundSpan = found.mutableSpan
            let result = likes.count(base, base + 63, into: &foundSpan)
            likes.oneColumnSeeks = false
            guard result == .done else { return "counting from \(base) with the one-column key: \(result)" }
            for i in 0..<64 where expected[i] != found[i] {
                return "post \(base + Int64(i)): \(found[i]) likes with the one-column key, \(expected[i]) with the two-column one"
            }
        }
        likes.oneColumnSeeks = true
        return nil
    }

    /// `{"post":{...}}`.
    mutating func post(_ id: Int64, into body: inout ByteBuffer) -> Answer {
        guard openIfNeeded() else { return .failed }
        var one = InlineArray<1, UInt32>(repeating: 0)
        var span = one.mutableSpan
        switch likes.count(id, id, into: &span) {
        case .done: break
        case .unexpected: return .unexpected
        case .failed: return .failed
        }
        body.append("{\"post\":")
        let answer = writePost(id, likes: one[0], into: &body)
        if answer == .done { body.append("}") }
        return answer
    }

    /// The post id of a posts_created_at_id_idx entry: (created_at, id).
    private static func indexedID(_ entry: Span<UInt8>) -> Int64? {
        guard let columns: InlineArray<2, Record.Column> = Record.columns(entry) else { return nil }
        return Record.integer(entry, columns[1])
    }

    /// Whether post `id` exists (for a like).
    mutating func postExists(_ id: Int64) -> Answer {
        guard openIfNeeded() else { return .failed }
        switch posts.seek(id) {
        case .entry: return .done
        case .none: return .missing
        case .failed: return .failed
        }
    }

    /// A post object: posts row `id` (id, user_id, body, created_at), then its author's username.
    private mutating func writePost(_ id: Int64, likes likeCount: UInt32, into body: inout ByteBuffer) -> Answer {
        switch posts.seek(id) {
        case .entry: break
        case .none: return .missing
        case .failed: return .failed
        }
        let row = posts.record
        guard let post: InlineArray<4, Record.Column> = Record.columns(row),
              let userID = Record.integer(row, post[1]), post[2].isText, post[3].isText else { return .unexpected }
        body.append("{\"id\":")
        JSON.writeInt(&body, id)
        body.append(",\"body\":")
        JSON.writeString(&body, row.extracting(post[2].range))
        body.append(",\"created_at\":")
        JSON.writeString(&body, row.extracting(post[3].range))

        // The post's columns are written, so the users cursor may move: a post without its user is no row of the join.
        switch users.seek(userID) {
        case .entry: break
        case .none: return .unexpected
        case .failed: return .failed
        }
        let author = users.record
        guard let user: InlineArray<2, Record.Column> = Record.columns(author), user[1].isText else { return .unexpected }
        body.append(",\"author\":")
        JSON.writeString(&body, author.extracting(user[1].range))
        body.append(",\"like_count\":")
        JSON.writeUInt(&body, UInt64(likeCount))
        body.append("}")
        return .done
    }

    // ---------- the schema these reads assume ----------

    /// The columns the decoders rely on, in order, with rowid tables whose `id` is the rowid, the two indexes' keys,
    /// and UTF-8 text.
    private static func schemaMatches(_ db: OpaquePointer) -> Bool {
        let checks: [(String, String)] = [
            ("SELECT name || ' ' || type || ' ' || pk FROM pragma_table_info('posts')",
             "id INTEGER 1,user_id INTEGER 0,body TEXT 0,created_at TEXT 0"),
            ("SELECT name || ' ' || type || ' ' || pk FROM pragma_table_info('users')",
             "id INTEGER 1,username TEXT 0,created_at TEXT 0"),
            ("SELECT count(*) FROM pragma_index_list('posts') WHERE origin = 'pk'", "0"),
            ("SELECT count(*) FROM pragma_index_list('users') WHERE origin = 'pk'", "0"),
            ("SELECT name || ' ' || desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "created_at 1,id 1"),
            ("SELECT name || ' ' || desc FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "post_id 0"),
            // The record decoder reads text as UTF-8. A text cast to a BLOB is its bytes in the database's encoding:
            // two characters are 2 bytes only in UTF-8 (`PRAGMA encoding` isn't compiled in: SQLITE_OMIT_UTF16).
            ("SELECT length(CAST('ab' AS BLOB))", "2"),
        ]
        for (sql, expected) in checks where rows(db, sql) != expected {
            Sys.logLine("btree reads off, SQL answers: \(sql) gave \(rows(db, sql) ?? "an error")")
            return false
        }
        return true
    }

    /// The first column of every row of `sql`, joined with commas.
    private static func rows(_ db: OpaquePointer, _ sql: String) -> String? {
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK, let stmt else { return nil }
        defer { sqlite3_finalize(stmt) }
        var values: [String] = []
        var rc = sqlite3_step(stmt)
        while rc == SQLITE_ROW {
            values.append(sqlite3_column_text(stmt, 0).map { String(cString: $0) } ?? "")
            rc = sqlite3_step(stmt)
        }
        return rc == SQLITE_DONE ? values.joined(separator: ",") : nil
    }

    private static func root(_ db: OpaquePointer, _ name: String) -> UInt32 {
        guard let text = rows(db, "SELECT rootpage FROM sqlite_schema WHERE name = '\(name)'"), let page = UInt32(text) else { return 0 }
        return page
    }
}
