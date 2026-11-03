// SQLite layer: thin C interop over system libsqlite3.
//
// One connection per hot path:
//   * N reader connections (default 4), each guarded by its own lock, executed
//     inline on the request's event-loop thread. WAL readers never block.
//   * 1 writer connection, executed on a dedicated serial thread. All writes are
//     committed (WAL + synchronous=NORMAL) before the response is produced.
//
// Prepared statements are created once per connection at startup and reused:
// they cache the query plan only, never data (rule 5).

import CSQLite3
import Dispatch
import NIOConcurrencyHelpers
import NIOCore

// sqlite3.h C macros are not imported into Swift; redefine the ones we need.
let SQLITE_OK: Int32 = 0
let SQLITE_ROW: Int32 = 100
let SQLITE_DONE: Int32 = 101
let SQLITE_OPEN_READWRITE: Int32 = 0x00000002

// ((sqlite3_destructor_type)-1)
let SQLITE_TRANSIENT = unsafeBitCast(OpaquePointer(bitPattern: -1), to: sqlite3_destructor_type.self)
// ((sqlite3_destructor_type)0)
let SQLITE_STATIC = unsafeBitCast(OpaquePointer(bitPattern: 0), to: sqlite3_destructor_type.self)

struct SQLiteError: Error, Sendable {
    let message: String
}

enum LogicError: Error, Sendable {
    case postNotFound
}

struct ReadStatements {
    let feed: OpaquePointer?
    let post: OpaquePointer?
    let ping: OpaquePointer?
}

struct WriteStatements {
    let insertPost: OpaquePointer?
    let postMetadata: OpaquePointer?
    let insertLike: OpaquePointer?
    let postExists: OpaquePointer?
}

private struct Reader {
    let lock: NIOLockedValueBox<OpaquePointer>
    let statements: ReadStatements
}

/// @unchecked Sendable: all mutable state is guarded — readers by per-connection
/// NIOLockedValueBox, the writer connection by the serial writerQueue.
final class Database: @unchecked Sendable {
    private let readers: [Reader]
    private let roundRobin = NIOLockedValueBox(UInt64(0))

    let writer: OpaquePointer
    let writerStatements: WriteStatements
    private let writerQueue = DispatchQueue(label: "feed.db.writer")

    // MARK: Open / setup

    static func open(path: String, readers readerCount: Int) throws -> Database {
        let readFlags = SQLITE_OPEN_READWRITE
        let readers = try (0..<readerCount).map { _ in
            let db = try openConnection(path, flags: readFlags)
            try applyPragmas(db)
            let statements = try prepareReadStatements(db)
            return Reader(lock: .init(db), statements: statements)
        }
        let writer = try openConnection(path, flags: readFlags)
        try applyPragmas(writer)
        let writeStatements = try prepareWriteStatements(writer)
        return Database(readers: readers, writer: writer, writerStatements: writeStatements)
    }

    private init(readers: [Reader], writer: OpaquePointer, writerStatements: WriteStatements) {
        self.readers = readers
        self.writer = writer
        self.writerStatements = writerStatements
    }

    private static func openConnection(_ path: String, flags: Int32) throws -> OpaquePointer {
        var db: OpaquePointer?
        guard sqlite3_open_v2(path, &db, flags, nil) == SQLITE_OK, let db else {
            let message = db.map { String(cString: sqlite3_errmsg($0)) } ?? "sqlite3_open_v2 failed"
            sqlite3_close_v2(db)
            throw SQLiteError(message: message)
        }
        return db
    }

    private static func applyPragmas(_ db: OpaquePointer) throws {
        for sql in [
            "PRAGMA journal_mode=WAL;",
            "PRAGMA synchronous=NORMAL;", // allowed by rule 6; writes still hit the WAL file before we respond
            "PRAGMA busy_timeout=10000;",
            "PRAGMA cache_size=-8000;", // 8 MB page cache per connection
            "PRAGMA mmap_size=268435456;", // let SQLite mmap the db file (still SQLite's own cache, rule 5)
            "PRAGMA temp_store=MEMORY;",
        ] {
            var errorMessage: UnsafeMutablePointer<CChar>?
            guard sqlite3_exec(db, sql, nil, nil, &errorMessage) == SQLITE_OK else {
                let message = errorMessage.map { String(cString: $0) } ?? "pragma failed"
                sqlite3_free(errorMessage)
                throw SQLiteError(message: message)
            }
        }
    }

    private static func prepare(_ db: OpaquePointer, _ sql: String) throws -> OpaquePointer? {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &statement, nil) == SQLITE_OK, let statement else {
            throw SQLiteError(message: String(cString: sqlite3_errmsg(db)))
        }
        return statement
    }

    private static func prepareReadStatements(_ db: OpaquePointer) throws -> ReadStatements {
        let postColumns = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
          FROM posts p JOIN users u ON u.id = p.user_id
        """
        return ReadStatements(
            feed: try prepare(db, postColumns + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"),
            post: try prepare(db, postColumns + " WHERE p.id = ?1"),
            ping: try prepare(db, "SELECT 1")
        )
    }

    private static func prepareWriteStatements(_ db: OpaquePointer) throws -> WriteStatements {
        WriteStatements(
            insertPost: try prepare(db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2)"),
            postMetadata: try prepare(db, "SELECT id, created_at FROM posts WHERE id = last_insert_rowid()"),
            insertLike: try prepare(
                db,
                "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING"
            ),
            postExists: try prepare(db, "SELECT 1 FROM posts WHERE id = ?1")
        )
    }

    // MARK: Execution

    /// Run a read on the next pooled reader connection, inline on the caller's
    /// event-loop thread. Each connection has its own lock; WAL readers never
    /// contend with the writer.
    private func read<T: Sendable>(_ body: (OpaquePointer, ReadStatements) throws -> T) throws -> T {
        let index = roundRobin.withLockedValue { counter -> UInt64 in
            counter &+= 1
            return counter
        } % UInt64(readers.count)
        let reader = readers[Int(index)]
        return try reader.lock.withLockedValue { db in
            try body(db, reader.statements)
        }
    }

    /// Run a write on the dedicated writer thread and return only after the
    /// statement has committed to the WAL (autocommit; synchronous=NORMAL).
    private func write<T: Sendable>(
        _ body: @escaping @Sendable (OpaquePointer, WriteStatements) throws -> T
    ) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            writerQueue.async {
                do {
                    continuation.resume(returning: try body(self.writer, self.writerStatements))
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    // MARK: Queries

    /// GET /health — `SELECT 1` through a pooled reader.
    func healthCheck() -> Result<Void, SQLiteError> {
        Result {
            try read { db, statements in
                sqlite3_reset(statements.ping)
                defer { sqlite3_reset(statements.ping) }
                guard sqlite3_step(statements.ping) == SQLITE_ROW else {
                    throw SQLiteError(message: String(cString: sqlite3_errmsg(db)))
                }
            }
        }.mapError { ($0 as? SQLiteError) ?? SQLiteError(message: "unknown error") }
    }

    /// GET /feed — build the full `{"posts":[…]}` body into one ByteBuffer.
    func feed() throws -> ByteBuffer {
        try read { db, statements in
            var buffer = ByteBufferAllocator().buffer(capacity: 16 * 1024)
            buffer.writeString("{\"posts\":[")
            var first = true
            while true {
                let rc = sqlite3_step(statements.feed)
                if rc == SQLITE_ROW {
                    if !first { buffer.writeString(",") }
                    first = false
                    appendPostObject(statements.feed, to: &buffer)
                } else if rc == SQLITE_DONE {
                    break
                } else {
                    sqlite3_reset(statements.feed)
                    throw SQLiteError(message: String(cString: sqlite3_errmsg(db)))
                }
            }
            sqlite3_reset(statements.feed)
            buffer.writeString("]}")
            return buffer
        }
    }

    /// GET /posts/:id — nil when the post does not exist.
    func post(id: Int64) throws -> ByteBuffer? {
        try read { db, statements in
            sqlite3_reset(statements.post)
            defer { sqlite3_reset(statements.post) }
            sqlite3_bind_int64(statements.post, 1, id)
            let rc = sqlite3_step(statements.post)
            if rc == SQLITE_ROW {
                var buffer = ByteBufferAllocator().buffer(capacity: 1024)
                buffer.writeString("{\"post\":")
                appendPostObject(statements.post, to: &buffer)
                buffer.writeString("}")
                return buffer
            }
            if rc == SQLITE_DONE { return nil }
            throw SQLiteError(message: String(cString: sqlite3_errmsg(db)))
        }
    }

    /// POST /posts — insert, then read back generated metadata on the writer connection.
    func createPost(userId: Int64, body: String) async throws -> (id: Int64, createdAt: String) {
        try await write { db, statements in
            var body = body
            return try body.withUTF8 { utf8 in
                sqlite3_reset(statements.insertPost)
                defer { sqlite3_reset(statements.insertPost) }
                sqlite3_bind_int64(statements.insertPost, 1, userId)
                sqlite3_bind_text(
                    statements.insertPost, 2,
                    utf8.baseAddress, Int32(utf8.count), SQLITE_STATIC
                )
                guard sqlite3_step(statements.insertPost) == SQLITE_DONE else {
                    throw SQLiteError(message: String(cString: sqlite3_errmsg(db)))
                }
                sqlite3_reset(statements.postMetadata)
                defer { sqlite3_reset(statements.postMetadata) }
                guard sqlite3_step(statements.postMetadata) == SQLITE_ROW else {
                    throw SQLiteError(message: String(cString: sqlite3_errmsg(db)))
                }
                let id = sqlite3_column_int64(statements.postMetadata, 0)
                let createdAt = String(cString: sqlite3_column_text(statements.postMetadata, 1))
                return (id: id, createdAt: createdAt)
            }
        }
    }

    /// POST /posts/:id/like — returns `alreadyLiked`. Throws LogicError.postNotFound
    /// when the post does not exist.
    func like(postId: Int64, userId: Int64) async throws -> Bool {
        try await write { db, statements in
            sqlite3_reset(statements.postExists)
            defer { sqlite3_reset(statements.postExists) }
            sqlite3_bind_int64(statements.postExists, 1, postId)
            guard sqlite3_step(statements.postExists) == SQLITE_ROW else {
                throw LogicError.postNotFound
            }

            sqlite3_reset(statements.insertLike)
            defer { sqlite3_reset(statements.insertLike) }
            sqlite3_bind_int64(statements.insertLike, 1, userId)
            sqlite3_bind_int64(statements.insertLike, 2, postId)
            guard sqlite3_step(statements.insertLike) == SQLITE_DONE else {
                throw SQLiteError(message: String(cString: sqlite3_errmsg(db)))
            }
            // changes == 0 means the ON CONFLICT clause swallowed the insert.
            return sqlite3_changes(db) == 0
        }
    }
}

// MARK: - Row -> JSON

/// Append one post object with the exact key order from SPEC.md.
private func appendPostObject(_ statement: OpaquePointer?, to buffer: inout ByteBuffer) {
    buffer.writeString("{\"id\":")
    buffer.writeString(String(sqlite3_column_int64(statement, 0)))
    buffer.writeString(",\"body\":")
    appendJSONTextColumn(statement, 1, to: &buffer)
    buffer.writeString(",\"created_at\":")
    appendJSONTextColumn(statement, 2, to: &buffer)
    buffer.writeString(",\"author\":")
    appendJSONTextColumn(statement, 3, to: &buffer)
    buffer.writeString(",\"like_count\":")
    buffer.writeString(String(sqlite3_column_int64(statement, 4)))
    buffer.writeString("}")
}

private func appendJSONTextColumn(_ statement: OpaquePointer?, _ column: Int32, to buffer: inout ByteBuffer) {
    guard let pointer = sqlite3_column_text(statement, column) else {
        buffer.writeString("\"\"")
        return
    }
    writeJSONString(&buffer, UnsafeRawBufferPointer(start: pointer, count: Int(sqlite3_column_bytes(statement, column))))
}
