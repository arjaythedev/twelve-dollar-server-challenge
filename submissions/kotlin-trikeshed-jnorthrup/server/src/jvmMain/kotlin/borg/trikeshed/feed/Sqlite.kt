package borg.trikeshed.feed

import java.lang.foreign.*
import java.lang.foreign.ValueLayout.*
import java.lang.invoke.MethodHandle

/** libsqlite3 entry points as FFM downcalls. Critical: no thread-state transition, heap segments passed in place. */
object Sqlite3 {
    val linker: Linker = Linker.nativeLinker()
    val lookup: SymbolLookup = SymbolLookup.libraryLookup(System.getProperty("sqlite.lib", "libsqlite3.so.0"), Arena.global())
    val critical: Linker.Option = Linker.Option.critical(true)

    fun handle(name: String, fd: FunctionDescriptor): MethodHandle =
        linker.downcallHandle(lookup.find(name).orElseThrow { UnsatisfiedLinkError(name) }, fd, critical)

    val openV2: MethodHandle = handle("sqlite3_open_v2", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS))
    val exec: MethodHandle = handle("sqlite3_exec", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS))
    val prepareV3: MethodHandle =
        handle("sqlite3_prepare_v3", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS))
    val step: MethodHandle = handle("sqlite3_step", FunctionDescriptor.of(JAVA_INT, ADDRESS))
    val reset: MethodHandle = handle("sqlite3_reset", FunctionDescriptor.of(JAVA_INT, ADDRESS))
    val bindInt64: MethodHandle = handle("sqlite3_bind_int64", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG))
    val bindText: MethodHandle =
        handle("sqlite3_bind_text", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS))
    val columnInt64: MethodHandle = handle("sqlite3_column_int64", FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT))
    val columnText: MethodHandle = handle("sqlite3_column_text", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT))
    val columnBytes: MethodHandle = handle("sqlite3_column_bytes", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT))
    val changes: MethodHandle = handle("sqlite3_changes", FunctionDescriptor.of(JAVA_INT, ADDRESS))
    val errmsg: MethodHandle = handle("sqlite3_errmsg", FunctionDescriptor.of(ADDRESS, ADDRESS))
    val finalize: MethodHandle = handle("sqlite3_finalize", FunctionDescriptor.of(JAVA_INT, ADDRESS))
    val closeV2: MethodHandle = handle("sqlite3_close_v2", FunctionDescriptor.of(JAVA_INT, ADDRESS))
}

const val SQLITE_ROW: Int = 100
const val SQLITE_DONE: Int = 101
const val SQLITE_OPEN_READWRITE: Int = 0x2
const val SQLITE_OPEN_NOMUTEX: Int = 0x8000
const val SQLITE_PREPARE_PERSISTENT: Int = 0x1

/** SQLITE_TRANSIENT: SQLite copies bound text during the call, so a heap segment may be bound. */
val TRANSIENT: MemorySegment = MemorySegment.ofAddress(-1L)

const val POST_SELECT: String = "SELECT p.id, p.body, p.created_at, u.username, " +
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id"

val ROW_ID: ByteArray = "{\"id\":".encodeToByteArray()
val ROW_BODY: ByteArray = ",\"body\":\"".encodeToByteArray()
val ROW_CREATED: ByteArray = "\",\"created_at\":\"".encodeToByteArray()
val ROW_AUTHOR: ByteArray = "\",\"author\":\"".encodeToByteArray()
val ROW_LIKES: ByteArray = "\",\"like_count\":".encodeToByteArray()
val FEED_OPEN: ByteArray = "{\"posts\":[".encodeToByteArray()
val FEED_CLOSE: ByteArray = "]}".encodeToByteArray()
val POST_OPEN: ByteArray = "{\"post\":".encodeToByteArray()

/**
 * One connection, prepared statements reused for the process lifetime, one thread. The server never closes it (the
 * process exit does); [close] is for owners that end sooner, such as tests.
 */
class SqliteStore(path: String) : FeedStore, AutoCloseable {
    val db: MemorySegment
    val feedStmt: MemorySegment
    val postStmt: MemorySegment
    val createStmt: MemorySegment
    val likeStmt: MemorySegment
    val existsStmt: MemorySegment
    val pingStmt: MemorySegment
    val beginStmt: MemorySegment
    val commitStmt: MemorySegment
    val rollbackStmt: MemorySegment
    override var pending: Boolean = false

    init {
        Arena.ofConfined().use { a ->
            val pp = a.allocate(ADDRESS)
            val rc = Sqlite3.openV2.invokeExact(a.allocateFrom(path), pp, SQLITE_OPEN_READWRITE or SQLITE_OPEN_NOMUTEX,
                MemorySegment.NULL) as Int
            db = pp.get(ADDRESS, 0)
            check(rc == 0) { "sqlite3_open_v2($path): ${message()}" }
        }
        // locking_mode before the first WAL access keeps the wal-index in heap memory (no -shm).
        exec("PRAGMA locking_mode=EXCLUSIVE")
        exec("PRAGMA journal_mode=WAL")
        exec("PRAGMA synchronous=NORMAL")
        exec("PRAGMA mmap_size=1073741824")
        exec("PRAGMA cache_size=-16384")
        exec("PRAGMA temp_store=MEMORY")
        feedStmt = prepare("$POST_SELECT ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
        postStmt = prepare("$POST_SELECT WHERE p.id = ?1")
        createStmt = prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")
        likeStmt = prepare("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS " +
            "(SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING")
        existsStmt = prepare("SELECT 1 FROM posts WHERE id = ?1")
        pingStmt = prepare("SELECT 1")
        beginStmt = prepare("BEGIN IMMEDIATE")
        commitStmt = prepare("COMMIT")
        rollbackStmt = prepare("ROLLBACK")
    }

    fun message(): String = (Sqlite3.errmsg.invokeExact(db) as MemorySegment).reinterpret(4096).getString(0)

    var open = true

    /** Rolls back an open transaction, finalizes the statements and closes the connection; idempotent. */
    override fun close() {
        if (!open) return
        open = false
        if (pending) {
            step(rollbackStmt)
            reset(rollbackStmt)
            pending = false
        }
        for (stmt in arrayOf(feedStmt, postStmt, createStmt, likeStmt, existsStmt, pingStmt, beginStmt, commitStmt, rollbackStmt)) {
            val rc = Sqlite3.finalize.invokeExact(stmt) as Int
        }
        val rc = Sqlite3.closeV2.invokeExact(db) as Int
        check(rc == 0) { "sqlite3_close_v2: rc $rc" }
    }

    fun exec(sql: String) = Arena.ofConfined().use { a ->
        val rc = Sqlite3.exec.invokeExact(db, a.allocateFrom(sql), MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL) as Int
        check(rc == 0) { "$sql: ${message()}" }
    }

    fun prepare(sql: String): MemorySegment = Arena.ofConfined().use { a ->
        val pp = a.allocate(ADDRESS)
        val rc = Sqlite3.prepareV3.invokeExact(db, a.allocateFrom(sql), -1, SQLITE_PREPARE_PERSISTENT, pp, MemorySegment.NULL) as Int
        check(rc == 0) { "$sql: ${message()}" }
        pp.get(ADDRESS, 0)
    }

    fun step(stmt: MemorySegment): Int = Sqlite3.step.invokeExact(stmt) as Int

    fun reset(stmt: MemorySegment) {
        val rc = Sqlite3.reset.invokeExact(stmt) as Int
    }

    fun bind(stmt: MemorySegment, index: Int, value: Long) {
        val rc = Sqlite3.bindInt64.invokeExact(stmt, index, value) as Int
    }

    fun bind(stmt: MemorySegment, index: Int, text: ByteArray) {
        val rc = Sqlite3.bindText.invokeExact(stmt, index, MemorySegment.ofArray(text), text.size, TRANSIENT) as Int
    }

    fun long(stmt: MemorySegment, col: Int): Long = Sqlite3.columnInt64.invokeExact(stmt, col) as Long

    /** Column text copied into [out] and escaped as a JSON string body. */
    fun text(stmt: MemorySegment, col: Int, out: Out) {
        val p = Sqlite3.columnText.invokeExact(stmt, col) as MemorySegment
        val n = Sqlite3.columnBytes.invokeExact(stmt, col) as Int
        out.room(n)
        MemorySegment.copy(p.reinterpret(n.toLong()), JAVA_BYTE, 0L, out.bytes, out.size, n)
        val at = out.size
        out.size += n
        out.escapeFrom(at)
    }

    fun row(stmt: MemorySegment, out: Out) {
        out.raw(ROW_ID); out.long(long(stmt, 0))
        out.raw(ROW_BODY); text(stmt, 1, out)
        out.raw(ROW_CREATED); text(stmt, 2, out)
        out.raw(ROW_AUTHOR); text(stmt, 3, out)
        out.raw(ROW_LIKES); out.long(long(stmt, 4))
        out.byte('}'.code)
    }

    fun failure(stmt: MemorySegment): Nothing {
        val m = message()
        reset(stmt)
        error(m)
    }

    override fun ping(): String? {
        val rc = step(pingStmt)
        if (rc == SQLITE_ROW) { reset(pingStmt); return null }
        val m = message()
        reset(pingStmt)
        return m
    }

    override fun feed(out: Out) {
        out.raw(FEED_OPEN)
        var rc = step(feedStmt)
        var first = true
        while (rc == SQLITE_ROW) {
            if (!first) out.byte(','.code)
            first = false
            row(feedStmt, out)
            rc = step(feedStmt)
        }
        if (rc != SQLITE_DONE) failure(feedStmt)
        reset(feedStmt)
        out.raw(FEED_CLOSE)
    }

    override fun post(id: Long, out: Out): Boolean {
        bind(postStmt, 1, id)
        val rc = step(postStmt)
        if (rc == SQLITE_DONE) { reset(postStmt); return false }
        if (rc != SQLITE_ROW) failure(postStmt)
        out.raw(POST_OPEN)
        row(postStmt, out)
        out.byte('}'.code)
        reset(postStmt)
        return true
    }

    fun begin() {
        if (pending) return
        if (step(beginStmt) != SQLITE_DONE) failure(beginStmt)
        reset(beginStmt)
        pending = true
    }

    override fun commit() {
        if (!pending) return
        pending = false
        val rc = step(commitStmt)
        reset(commitStmt)
        if (rc != SQLITE_DONE) {
            step(rollbackStmt)
            reset(rollbackStmt)
            error("COMMIT: ${message()}")
        }
    }

    override fun create(user: Long, author: String, body: String, out: Out) {
        begin()
        bind(createStmt, 1, user)
        bind(createStmt, 2, body.encodeToByteArray())
        if (step(createStmt) != SQLITE_ROW) failure(createStmt)
        out.raw(POST_OPEN)
        out.raw(ROW_ID); out.long(long(createStmt, 0))
        out.raw(ROW_BODY); out.jsonText(body.encodeToByteArray())
        out.raw(ROW_CREATED); text(createStmt, 1, out)
        out.raw(ROW_AUTHOR); out.jsonText(author.encodeToByteArray())
        out.raw(ROW_LIKES); out.byte('0'.code)
        out.byte('}'.code); out.byte('}'.code)
        reset(createStmt)
    }

    override fun like(user: Long, post: Long): Int {
        begin()
        bind(likeStmt, 1, user)
        bind(likeStmt, 2, post)
        if (step(likeStmt) != SQLITE_DONE) failure(likeStmt)
        reset(likeStmt)
        if ((Sqlite3.changes.invokeExact(db) as Int) == 1) return 1
        bind(existsStmt, 1, post)
        val rc = step(existsStmt)
        reset(existsStmt)
        return if (rc == SQLITE_ROW) 0 else -1
    }
}
