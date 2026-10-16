package server

import java.lang.foreign.{Arena, FunctionDescriptor, Linker, MemoryLayout, MemorySegment, SymbolLookup}
import java.lang.foreign.ValueLayout.{ADDRESS, JAVA_BYTE, JAVA_INT, JAVA_LONG}
import java.lang.invoke.MethodHandle
import java.nio.charset.StandardCharsets.UTF_8
import java.nio.file.Path

/** Direct bindings to the C API of the libsqlite3.so that build.sh compiles, through the JDK's
  * foreign function API (no JDBC, no JNI glue). Every call happens on the one event-loop thread. */
object Sqlite:
  final val OK = 0
  final val ROW = 100
  final val DONE = 101
  private final val OPEN_READWRITE = 0x02
  private final val OPEN_NOMUTEX = 0x8000
  private final val PREPARE_PERSISTENT = 0x01

  private val linker = Linker.nativeLinker()
  private val lib = SymbolLookup.libraryLookup(Path.of(System.getProperty("sqlite.lib")), Arena.global())

  // critical: these calls never call back into Java, so skip the thread-state transition.
  // Heap access lets sqlite3_bind_text read a byte[] in place (SQLITE_TRANSIENT copies it during the call).
  private def fn(name: String, ret: MemoryLayout | Null, args: MemoryLayout*): MethodHandle =
    val desc = if ret == null then FunctionDescriptor.ofVoid(args*) else FunctionDescriptor.of(ret, args*)
    linker.downcallHandle(lib.find(name).orElseThrow(), desc, Linker.Option.critical(true))

  private val initialize = fn("sqlite3_initialize", JAVA_INT)
  private val openV2 = fn("sqlite3_open_v2", JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS)
  private val exec = fn("sqlite3_exec", JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS)
  private val prepareV3 = fn("sqlite3_prepare_v3", JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS)
  private val errmsg = fn("sqlite3_errmsg", ADDRESS, ADDRESS)
  private val changes = fn("sqlite3_changes", JAVA_INT, ADDRESS)
  private val step = fn("sqlite3_step", JAVA_INT, ADDRESS)
  private val reset = fn("sqlite3_reset", JAVA_INT, ADDRESS)
  private val bindInt64 = fn("sqlite3_bind_int64", JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG)
  private val bindText = fn("sqlite3_bind_text", JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS)
  private val columnInt64 = fn("sqlite3_column_int64", JAVA_LONG, ADDRESS, JAVA_INT)
  private val columnText = fn("sqlite3_column_text", ADDRESS, ADDRESS, JAVA_INT)
  private val columnBytes = fn("sqlite3_column_bytes", JAVA_INT, ADDRESS, JAVA_INT)

  private val TRANSIENT = MemorySegment.ofAddress(-1L)

  final class Db private[Sqlite] (val handle: MemorySegment):
    def errorMessage: String = cString((errmsg.invokeExact(handle): MemorySegment))
    def changeCount: Int = (changes.invokeExact(handle): Int)

    def execute(sql: String): Unit =
      val arena = Arena.ofConfined()
      try
        val rc = (exec.invokeExact(handle, arena.allocateFrom(sql), MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL): Int)
        if rc != OK then throw SqliteException(s"$sql: $errorMessage")
      finally arena.close()

    def prepare(sql: String): Stmt =
      val arena = Arena.ofConfined()
      try
        val out = arena.allocate(ADDRESS)
        val rc = (prepareV3.invokeExact(handle, arena.allocateFrom(sql), -1, PREPARE_PERSISTENT, out, MemorySegment.NULL): Int)
        if rc != OK then throw SqliteException(s"$sql: $errorMessage")
        Stmt(this, out.get(ADDRESS, 0))
      finally arena.close()

  /** A prepared statement, reused for every request. Callers bind, step, read columns, then reset. */
  final class Stmt private[Sqlite] (db: Db, handle: MemorySegment):
    def bind(i: Int, v: Long): Unit = check((bindInt64.invokeExact(handle, i, v): Int))
    def bind(i: Int, utf8: Array[Byte], len: Int): Unit =
      check((bindText.invokeExact(handle, i, MemorySegment.ofArray(utf8), len, TRANSIENT): Int))
    /** Returns ROW or DONE, throws on errors (after resetting, so the statement is reusable). */
    def step(): Int =
      val rc = (Sqlite.step.invokeExact(handle): Int)
      if rc != ROW && rc != DONE then
        val msg = db.errorMessage
        reset()
        throw SqliteException(msg)
      rc
    def reset(): Unit = { val _ = (Sqlite.reset.invokeExact(handle): Int) }
    def long(col: Int): Long = (columnInt64.invokeExact(handle, col): Long)
    /** Copies a TEXT column's UTF-8 bytes into `out` as a JSON string literal. */
    def jsonText(col: Int, out: Out): Unit =
      val p = (columnText.invokeExact(handle, col): MemorySegment)
      val n = (columnBytes.invokeExact(handle, col): Int)
      out.jsonUtf8(p.reinterpret(n.toLong), n)
    private def check(rc: Int): Unit = if rc != OK then throw SqliteException(db.errorMessage)

  final class SqliteException(msg: String) extends RuntimeException(msg, null, false, false)

  def open(path: String): Db =
    val init = (initialize.invokeExact(): Int)
    if init != OK then throw SqliteException("sqlite3_initialize failed")
    val arena = Arena.ofConfined()
    try
      val out = arena.allocate(ADDRESS)
      val rc = (openV2.invokeExact(arena.allocateFrom(path), out, OPEN_READWRITE | OPEN_NOMUTEX, MemorySegment.NULL): Int)
      val db = Db(out.get(ADDRESS, 0))
      if rc != OK then throw SqliteException(s"open $path: ${db.errorMessage}")
      db
    finally arena.close()

  private def cString(p: MemorySegment): String =
    val s = p.reinterpret(Long.MaxValue)
    var n = 0L
    while s.get(JAVA_BYTE, n) != 0 do n += 1
    String(s.asSlice(0, n).toArray(JAVA_BYTE), UTF_8)
