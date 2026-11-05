package server

import java.lang.foreign.Arena
import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.MemoryLayout
import java.lang.foreign.MemorySegment
import java.lang.foreign.SymbolLookup
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.lang.foreign.ValueLayout.JAVA_INT
import java.lang.foreign.ValueLayout.JAVA_LONG
import java.lang.invoke.MethodHandle
import java.nio.file.Path

// Minimal bindings to the SQLite C API through Java's FFM API (java.lang.foreign), against the
// libsqlite3.so that build.sh compiles from the official amalgamation.

private const val SQLITE_OK = 0
private const val SQLITE_ROW = 100
private const val SQLITE_DONE = 101
private const val SQLITE_OPEN_READWRITE = 0x00000002
private const val SQLITE_OPEN_NOMUTEX = 0x00008000
private const val SQLITE_PREPARE_PERSISTENT = 0x01
private val SQLITE_TRANSIENT: MemorySegment = MemorySegment.ofAddress(-1) // bind_text copies the bytes

private val linker = Linker.nativeLinker()
private val lib = SymbolLookup.libraryLookup(
    Path.of(System.getProperty("sqlite.lib") ?: error("-Dsqlite.lib=<path to libsqlite3.so> is not set")),
    Arena.global(),
)

/** A downcall. [critical] ones are short, never block, and may read Java heap arrays directly. */
private fun fn(name: String, ret: MemoryLayout, vararg args: MemoryLayout, critical: Boolean = false): MethodHandle {
    val options = if (critical) arrayOf(Linker.Option.critical(true)) else emptyArray()
    return linker.downcallHandle(lib.find(name).orElseThrow(), FunctionDescriptor.of(ret, *args), *options)
}

private val sqlite3_open_v2 = fn("sqlite3_open_v2", JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS)
private val sqlite3_exec = fn("sqlite3_exec", JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS)
private val sqlite3_busy_timeout = fn("sqlite3_busy_timeout", JAVA_INT, ADDRESS, JAVA_INT)
private val sqlite3_prepare_v3 = fn("sqlite3_prepare_v3", JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS)
private val sqlite3_errmsg = fn("sqlite3_errmsg", ADDRESS, ADDRESS, critical = true)
private val sqlite3_changes = fn("sqlite3_changes", JAVA_INT, ADDRESS, critical = true)
// step does the I/O (and commits, and checkpoints), so it is a regular downcall.
private val sqlite3_step = fn("sqlite3_step", JAVA_INT, ADDRESS)
private val sqlite3_reset = fn("sqlite3_reset", JAVA_INT, ADDRESS, critical = true)
private val sqlite3_bind_int64 = fn("sqlite3_bind_int64", JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG, critical = true)
private val sqlite3_bind_text = fn("sqlite3_bind_text", JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, critical = true)
private val sqlite3_column_int64 = fn("sqlite3_column_int64", JAVA_LONG, ADDRESS, JAVA_INT, critical = true)
private val sqlite3_column_text = fn("sqlite3_column_text", ADDRESS, ADDRESS, JAVA_INT, critical = true)
private val sqlite3_column_bytes = fn("sqlite3_column_bytes", JAVA_INT, ADDRESS, JAVA_INT, critical = true)

class SqliteException(message: String) : Exception(message)

/** One SQLite connection. Not thread-safe (opened with SQLITE_OPEN_NOMUTEX): use it from one thread. */
class SqliteConnection(path: String) {
    private val db: MemorySegment

    init {
        Arena.ofConfined().use { arena ->
            val out = arena.allocate(ADDRESS)
            val rc = sqlite3_open_v2.invokeExact(arena.allocateFrom(path), out, SQLITE_OPEN_READWRITE or SQLITE_OPEN_NOMUTEX, MemorySegment.NULL) as Int
            db = out.get(ADDRESS, 0)
            if (rc != SQLITE_OK) throw SqliteException("cannot open $path: ${if (db == MemorySegment.NULL) "rc $rc" else errmsg()}")
        }
    }

    internal fun errmsg(): String = (sqlite3_errmsg.invokeExact(db) as MemorySegment).reinterpret(Long.MAX_VALUE).getString(0)

    internal fun check(rc: Int, what: String) {
        if (rc != SQLITE_OK) throw SqliteException("$what: ${errmsg()} (rc $rc)")
    }

    fun busyTimeout(ms: Int) = check(sqlite3_busy_timeout.invokeExact(db, ms) as Int, "busy_timeout")

    fun exec(sql: String) = Arena.ofConfined().use { arena ->
        check(sqlite3_exec.invokeExact(db, arena.allocateFrom(sql), MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL) as Int, sql)
    }

    /** Rows changed by the last INSERT/UPDATE/DELETE on this connection. */
    fun changes(): Int = sqlite3_changes.invokeExact(db) as Int

    fun prepare(sql: String): SqliteStatement = Arena.ofConfined().use { arena ->
        val out = arena.allocate(ADDRESS)
        check(sqlite3_prepare_v3.invokeExact(db, arena.allocateFrom(sql), -1, SQLITE_PREPARE_PERSISTENT, out, MemorySegment.NULL) as Int, sql)
        SqliteStatement(this, out.get(ADDRESS, 0))
    }
}

/** A prepared statement, kept for the life of the connection: bind, step until done, then [reset]. */
class SqliteStatement internal constructor(private val conn: SqliteConnection, private val stmt: MemorySegment) {
    fun bind(i: Int, v: Long) = conn.check(sqlite3_bind_int64.invokeExact(stmt, i, v) as Int, "bind")

    /** Binds UTF-8 text, which SQLite copies. */
    fun bindText(i: Int, utf8: ByteArray) =
        conn.check(sqlite3_bind_text.invokeExact(stmt, i, MemorySegment.ofArray(utf8), utf8.size, SQLITE_TRANSIENT) as Int, "bind")

    /** True when a row is ready, false when the statement is done. */
    fun step(): Boolean = when (val rc = sqlite3_step.invokeExact(stmt) as Int) {
        SQLITE_ROW -> true
        SQLITE_DONE -> false
        else -> {
            val msg = conn.errmsg()
            sqlite3_reset.invokeExact(stmt) as Int
            throw SqliteException("step: $msg (rc $rc)")
        }
    }

    /** Ends the statement (and with it, in autocommit mode, its read transaction). */
    fun reset() {
        sqlite3_reset.invokeExact(stmt) as Int
    }

    fun long(col: Int): Long = sqlite3_column_int64.invokeExact(stmt, col) as Long

    /** Copies a TEXT column's UTF-8 bytes into [dst] (grown as needed) and returns its length. */
    fun text(col: Int, dst: Scratch): Int {
        val p = sqlite3_column_text.invokeExact(stmt, col) as MemorySegment
        val n = sqlite3_column_bytes.invokeExact(stmt, col) as Int
        if (n > 0) MemorySegment.copy(p.reinterpret(n.toLong()), JAVA_BYTE, 0, dst.ensure(n), 0, n)
        return n
    }
}

/** A reusable byte array for copying column values out of SQLite. */
class Scratch {
    var bytes = ByteArray(1024)
        private set

    fun ensure(n: Int): ByteArray {
        if (n > bytes.size) bytes = ByteArray(maxOf(n, bytes.size * 2))
        return bytes
    }
}
