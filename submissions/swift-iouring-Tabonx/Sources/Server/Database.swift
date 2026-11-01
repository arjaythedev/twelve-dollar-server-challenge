// SQLite access: one connection, prepared statements, and the feed as range scans.
//
// SQLite is compiled with SQLITE_THREADSAFE=0, so exactly one thread may use a Database at a time.

import CSQLite
import CShim

let SQLITE_OK_: Int32 = 0
let SQLITE_ROW_: Int32 = 100
let SQLITE_DONE_: Int32 = 101

typealias Statement = OpaquePointer

/// The newest posts with their like counts, written as JSON post objects.
/// Statement texts follow SPEC.md; the feed splits the reference query into three range scans.
public final class Database {
    let db: OpaquePointer
    let ping: Statement
    let feedIDs: Statement
    let feedLikes: Statement
    let feedRows: Statement
    let feedReference: Statement
    let post: Statement
    let postExists: Statement
    let insertPost: Statement
    let insertLike: Statement
    let begin: Statement
    let commit: Statement
    let rollback: Statement

    /// WAL size in frames after the last commit, written by SQLite's WAL hook.
    let walFrames: UnsafeMutablePointer<Int32>
    /// WAL size in frames that makes a checkpoint due. A larger WAL makes every page read slower, because
    /// SQLite searches the WAL index first: 1,024 frames cost about 7 % less CPU than 4,096 under load.
    let checkpointFrames: Int32 = 1024
    let ids: UnsafeMutablePointer<Int64>
    let likeCounts: UnsafeMutablePointer<Int64>

    public private(set) var inTransaction = false

    /// Opens the database. On failure, `error` holds SQLite's message.
    public init?(path: UnsafePointer<CChar>, error: inout OutBuffer) {
        var handle: OpaquePointer?
        let rc = sqlite3_open_v2(path, &handle, 0x00000002 /* READWRITE */, nil)
        guard rc == SQLITE_OK_, let opened = handle else {
            Self.appendMessage(handle, &error)
            sqlite3_close_v2(handle)
            return nil
        }
        db = opened

        // Exclusive locking first, so WAL keeps its index in heap memory and reads skip fcntl locks.
        // synchronous=NORMAL is the weakest setting rule 6 allows.
        let pragmas: [StaticString] = [
            "PRAGMA locking_mode=EXCLUSIVE",
            "PRAGMA journal_mode=WAL",
            "PRAGMA synchronous=NORMAL",
            "PRAGMA cache_size=-131072",
            "PRAGMA mmap_size=0",
            "PRAGMA temp_store=MEMORY",
            "PRAGMA wal_autocheckpoint=0",
        ]
        for pragma in pragmas {
            guard sqlite3_exec(opened, UnsafeRawPointer(pragma.utf8Start).assumingMemoryBound(to: CChar.self), nil, nil, nil) == SQLITE_OK_ else {
                Self.appendMessage(opened, &error)
                sqlite3_close_v2(opened)
                return nil
            }
        }

        var failed = false
        func prepare(_ sql: StaticString) -> Statement {
            var s: OpaquePointer?
            let rc = csqlite_prepare(opened, UnsafeRawPointer(sql.utf8Start).assumingMemoryBound(to: CChar.self), &s)
            if rc != SQLITE_OK_ || s == nil {
                if !failed { Self.appendMessage(opened, &error) }
                failed = true
                return OpaquePointer(bitPattern: 1)!
            }
            return s!
        }

        ping = prepare("SELECT 1")
        feedIDs = prepare("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20")
        // Counting the index rows here is about 7 % cheaper per feed than GROUP BY in SQLite.
        feedLikes = prepare("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2")
        feedRows = prepare(
            """
            SELECT p.id, p.body, p.created_at, u.username
              FROM posts p JOIN users u ON u.id = p.user_id
             WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC
            """)
        feedReference = prepare(
            """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
             ORDER BY p.created_at DESC, p.id DESC LIMIT 20
            """)
        // No JOIN here: SQLite 3.54 re-prepares `... JOIN users u ON u.id = p.user_id WHERE p.id = ?1` on
        // every run (SQLITE_STMTSTATUS_REPREPARE counts each step), which tripled the cost of this endpoint.
        // A post whose author row is missing yields NULL and is reported as not found, like the join would.
        post = prepare(
            """
            SELECT p.id, p.body, p.created_at,
                   (SELECT username FROM users WHERE id = p.user_id),
                   (SELECT count(*) FROM likes WHERE post_id = ?1)
              FROM posts p
             WHERE p.id = ?1
            """)
        postExists = prepare("SELECT 1 FROM posts WHERE id = ?1")
        insertPost = prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")
        insertLike = prepare("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING")
        begin = prepare("BEGIN")
        commit = prepare("COMMIT")
        rollback = prepare("ROLLBACK")

        walFrames = .allocate(capacity: 1)
        walFrames.pointee = 0
        ids = .allocate(capacity: 20)
        likeCounts = .allocate(capacity: 256)
        if failed {
            sqlite3_close_v2(opened)
            return nil
        }
        csqlite_track_wal(opened, walFrames)
    }

    static func appendMessage(_ db: OpaquePointer?, _ out: inout OutBuffer) {
        guard let db, let msg = sqlite3_errmsg(db) else {
            out.append("sqlite3_open_v2 failed")
            return
        }
        appendCString(msg, &out)
    }

    func appendError(_ out: inout OutBuffer) { Self.appendMessage(db, &out) }

    // MARK: Transactions

    @inline(__always)
    func run(_ s: Statement) -> Bool {
        let rc = sqlite3_step(s)
        sqlite3_reset(s)
        return rc == SQLITE_DONE_
    }

    /// Opens a transaction that the next `commitTransaction` ends. Used to share one snapshot and one
    /// commit across a batch of requests.
    public func beginTransaction() -> Bool {
        guard !inTransaction else { return true }
        inTransaction = run(begin)
        return inTransaction
    }

    /// Commits the open transaction. On failure everything since `beginTransaction` is rolled back.
    public func commitTransaction() -> Bool {
        guard inTransaction else { return true }
        inTransaction = false
        if run(commit) {
            noteCommit()
            return true
        }
        _ = run(rollback)
        return false
    }

    /// True when the WAL has grown past `checkpointFrames` and a checkpoint should run.
    public private(set) var checkpointDue = false

    @inline(__always)
    func noteCommit() {
        if walFrames.pointee >= checkpointFrames { checkpointDue = true }
    }

    /// Runs a due checkpoint. The worker calls this after it has submitted the batch's sends, so the
    /// checkpoint's I/O never delays responses that are already committed.
    public func checkpointIfDue() {
        guard checkpointDue else { return }
        checkpointDue = false
        walFrames.pointee = 0
        sqlite3_wal_checkpoint_v2(db, nil, 0 /* PASSIVE */, nil, nil)
    }

    // MARK: Queries

    func healthCheck() -> Bool {
        let rc = sqlite3_step(ping)
        sqlite3_reset(ping)
        return rc == SQLITE_ROW_
    }

    /// Writes `{"posts":[...]}`. Returns false on a database error.
    func writeFeed(_ out: inout OutBuffer) -> Bool {
        var n = 0
        while n < 20 {
            let rc = sqlite3_step(feedIDs)
            if rc == SQLITE_ROW_ {
                ids[n] = sqlite3_column_int64(feedIDs, 0)
                n &+= 1
            } else if rc == SQLITE_DONE_ {
                break
            } else {
                sqlite3_reset(feedIDs)
                return false
            }
        }
        sqlite3_reset(feedIDs)
        out.append("{\"posts\":[")
        if n == 0 {
            out.append("]}")
            return true
        }

        // The range path needs the ids in strictly descending, contiguous order, which is what new posts
        // produce: then the rowid scan returns rows in feed order. Anything else takes the reference query.
        let maxID = ids[0]
        let minID = ids[n &- 1]
        var contiguous = maxID &- minID == Int64(n &- 1)
        if contiguous {
            for k in 0..<n where ids[k] != maxID &- Int64(k) {
                contiguous = false
                break
            }
        }
        let mark = out.count
        if contiguous, writeFeedRange(minID, maxID, n, &out) {
            out.append("]}")
            return true
        }
        out.truncate(mark)
        guard writeFeedReference(&out) else { return false }
        out.append("]}")
        return true
    }

    func writeFeedRange(_ minID: Int64, _ maxID: Int64, _ n: Int, _ out: inout OutBuffer) -> Bool {
        likeCounts.initialize(repeating: 0, count: n)
        sqlite3_bind_int64(feedLikes, 1, minID)
        sqlite3_bind_int64(feedLikes, 2, maxID)
        while true {
            let rc = sqlite3_step(feedLikes)
            if rc == SQLITE_ROW_ {
                let id = sqlite3_column_int64(feedLikes, 0)
                likeCounts[Int(maxID &- id)] &+= 1
            } else if rc == SQLITE_DONE_ {
                break
            } else {
                sqlite3_reset(feedLikes)
                return false
            }
        }
        sqlite3_reset(feedLikes)

        sqlite3_bind_int64(feedRows, 1, minID)
        sqlite3_bind_int64(feedRows, 2, maxID)
        var k = 0
        var ok = true
        while true {
            let rc = sqlite3_step(feedRows)
            if rc == SQLITE_ROW_ {
                // A post whose author row is missing would be dropped by the join; detect the gap.
                guard k < n, sqlite3_column_int64(feedRows, 0) == maxID &- Int64(k) else {
                    ok = false
                    break
                }
                if k > 0 { out.append(UInt8(ascii: ",")) }
                writePostObject(feedRows, likeCount: likeCounts[k], &out)
                k &+= 1
            } else if rc == SQLITE_DONE_ {
                break
            } else {
                ok = false
                break
            }
        }
        sqlite3_reset(feedRows)
        return ok && k == n
    }

    func writeFeedReference(_ out: inout OutBuffer) -> Bool {
        var first = true
        while true {
            let rc = sqlite3_step(feedReference)
            if rc == SQLITE_ROW_ {
                if !first { out.append(UInt8(ascii: ",")) }
                first = false
                writePostObject(feedReference, likeCount: sqlite3_column_int64(feedReference, 4), &out)
            } else if rc == SQLITE_DONE_ {
                sqlite3_reset(feedReference)
                return true
            } else {
                sqlite3_reset(feedReference)
                return false
            }
        }
    }

    enum Lookup { case found, notFound, failed }

    /// Writes `{"post":{...}}` for one id.
    func writePost(_ id: Int64, _ out: inout OutBuffer) -> Lookup {
        sqlite3_bind_int64(post, 1, id)
        let rc = sqlite3_step(post)
        defer { sqlite3_reset(post) }
        if rc == SQLITE_ROW_ && sqlite3_column_type(post, 3) != 5 /* SQLITE_NULL */ {
            out.append("{\"post\":")
            writePostObject(post, likeCount: sqlite3_column_int64(post, 4), &out)
            out.append(UInt8(ascii: "}"))
            return .found
        }
        return rc == SQLITE_DONE_ || rc == SQLITE_ROW_ ? .notFound : .failed
    }

    /// Inserts a post and writes the 201 body. `body` is the decoded, trimmed text.
    func createPost(userID: Int64, body: ByteView, author: ByteView, _ out: inout OutBuffer) -> Bool {
        sqlite3_bind_int64(insertPost, 1, userID)
        csqlite_bind_text_static(insertPost, 2, body.base, Int32(body.count))
        defer {
            sqlite3_reset(insertPost)
            sqlite3_clear_bindings(insertPost)
        }
        guard sqlite3_step(insertPost) == SQLITE_ROW_ else { return false }
        out.append("{\"post\":{\"id\":")
        out.appendInt(sqlite3_column_int64(insertPost, 0))
        out.append(",\"body\":")
        out.appendJSONString(body)
        out.append(",\"created_at\":")
        out.appendJSONString(textColumn(insertPost, 1))
        out.append(",\"author\":\"")
        out.append(author)
        out.append("\",\"like_count\":0}}")
        // Finish the statement so the insert is complete before the commit.
        guard sqlite3_step(insertPost) == SQLITE_DONE_ else { return false }
        if !inTransaction { noteCommit() }
        return true
    }

    enum LikeResult { case created, already, missingPost, failed }

    func like(userID: Int64, postID: Int64) -> LikeResult {
        sqlite3_bind_int64(postExists, 1, postID)
        let rc = sqlite3_step(postExists)
        sqlite3_reset(postExists)
        if rc == SQLITE_DONE_ { return .missingPost }
        guard rc == SQLITE_ROW_ else { return .failed }

        sqlite3_bind_int64(insertLike, 1, userID)
        sqlite3_bind_int64(insertLike, 2, postID)
        let done = sqlite3_step(insertLike)
        sqlite3_reset(insertLike)
        guard done == SQLITE_DONE_ else { return .failed }
        if !inTransaction { noteCommit() }
        return sqlite3_changes(db) == 0 ? .already : .created
    }
}

@inline(__always)
func textColumn(_ s: Statement, _ i: Int32) -> ByteView {
    let p = sqlite3_column_text(s, i)
    return ByteView(base: p.map { UnsafePointer($0) }, count: Int(sqlite3_column_bytes(s, i)))
}

/// Writes one post object in SPEC.md key order from columns id, body, created_at, username.
@inline(__always)
func writePostObject(_ s: Statement, likeCount: Int64, _ out: inout OutBuffer) {
    out.append("{\"id\":")
    out.appendInt(sqlite3_column_int64(s, 0))
    out.append(",\"body\":")
    out.appendJSONString(textColumn(s, 1))
    out.append(",\"created_at\":")
    out.appendJSONString(textColumn(s, 2))
    out.append(",\"author\":")
    out.appendJSONString(textColumn(s, 3))
    out.append(",\"like_count\":")
    out.appendInt(likeCount)
    out.append(UInt8(ascii: "}"))
}

func appendCString(_ s: UnsafePointer<CChar>, _ out: inout OutBuffer) {
    var p = UnsafeRawPointer(s).assumingMemoryBound(to: UInt8.self)
    while p.pointee != 0 {
        out.append(p.pointee)
        p += 1
    }
}
