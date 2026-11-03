package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;

/** SQLite through the Foreign Function and Memory API. No JDBC. */
final class Sqlite {
    static final int OK = 0, ROW = 100, DONE = 101;
    static final int OPEN_READWRITE = 0x2, OPEN_NOMUTEX = 0x8000, OPEN_EXRESCODE = 0x02000000;
    static final int PREPARE_PERSISTENT = 0x01;
    /** sqlite3_destructor_type that copies the bytes. */
    static final MemorySegment TRANSIENT = MemorySegment.ofAddress(-1L);

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB = SymbolLookup.libraryLookup(libraryPath(), Arena.global());
    private static final Linker.Option CRITICAL = Linker.Option.critical(false);

    private static Path libraryPath() {
        String configured = System.getProperty("sqlite.lib");
        if (configured != null && !configured.isEmpty()) return Path.of(configured);
        String name = System.getProperty("os.name", "").toLowerCase().contains("mac")
                ? "build/libsqlite3.dylib" : "build/libsqlite3.so";
        return Path.of(name);
    }

    private static MethodHandle fn(String name, FunctionDescriptor fd, Linker.Option... options) {
        return LINKER.downcallHandle(LIB.find(name).orElseThrow(() -> new UnsatisfiedLinkError(name)), fd, options);
    }

    private static final MethodHandle INITIALIZE = fn("sqlite3_initialize", FunctionDescriptor.of(JAVA_INT));
    private static final MethodHandle OPEN_V2 =
            fn("sqlite3_open_v2", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
    private static final MethodHandle EXEC =
            fn("sqlite3_exec", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS));
    private static final MethodHandle PREPARE_V3 = fn("sqlite3_prepare_v3",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
    private static final MethodHandle ERRMSG = fn("sqlite3_errmsg", FunctionDescriptor.of(ADDRESS, ADDRESS));
    // step runs the statement. critical() is only valid for empty-function calls and can
    // lose performance or crash the JVM if the call does real work (Linker.Option javadoc).
    private static final MethodHandle STEP = fn("sqlite3_step", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle RESET = fn("sqlite3_reset", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
    private static final MethodHandle BIND_INT64 =
            fn("sqlite3_bind_int64", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG), CRITICAL);
    private static final MethodHandle BIND_TEXT = fn("sqlite3_bind_text",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS), CRITICAL);
    private static final MethodHandle COLUMN_TEXT =
            fn("sqlite3_column_text", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle COLUMN_BYTES =
            fn("sqlite3_column_bytes", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle COLUMN_INT64 =
            fn("sqlite3_column_int64", FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle CHANGES = fn("sqlite3_changes", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
    private static final MethodHandle WAL_CHECKPOINT_V2 = fn("sqlite3_wal_checkpoint_v2",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS, ADDRESS));

    private Sqlite() {}

    static final class SqliteException extends RuntimeException {
        SqliteException(String message) { super(message); }
    }

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) return r;
        if (t instanceof Error e) throw e;
        return new IllegalStateException(t);
    }

    static int initialize() {
        try { return (int) INITIALIZE.invokeExact(); } catch (Throwable t) { throw rethrow(t); }
    }

    static int openV2(MemorySegment path, MemorySegment ppDb, int flags) {
        try { return (int) OPEN_V2.invokeExact(path, ppDb, flags, MemorySegment.NULL); }
        catch (Throwable t) { throw rethrow(t); }
    }

    static int exec(MemorySegment db, MemorySegment sql) {
        try {
            return (int) EXEC.invokeExact(db, sql, MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static int prepareV3(MemorySegment db, MemorySegment sql, int flags, MemorySegment ppStmt) {
        try { return (int) PREPARE_V3.invokeExact(db, sql, -1, flags, ppStmt, MemorySegment.NULL); }
        catch (Throwable t) { throw rethrow(t); }
    }

    static String errmsg(MemorySegment db) {
        try {
            MemorySegment p = (MemorySegment) ERRMSG.invokeExact(db);
            return p.reinterpret(Integer.MAX_VALUE).getString(0);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static int step(MemorySegment stmt) {
        try { return (int) STEP.invokeExact(stmt); } catch (Throwable t) { throw rethrow(t); }
    }

    static int reset(MemorySegment stmt) {
        try { return (int) RESET.invokeExact(stmt); } catch (Throwable t) { throw rethrow(t); }
    }

    static int bindInt64(MemorySegment stmt, int index, long value) {
        try { return (int) BIND_INT64.invokeExact(stmt, index, value); } catch (Throwable t) { throw rethrow(t); }
    }

    static int bindText(MemorySegment stmt, int index, MemorySegment text, int nBytes) {
        try { return (int) BIND_TEXT.invokeExact(stmt, index, text, nBytes, TRANSIENT); }
        catch (Throwable t) { throw rethrow(t); }
    }

    static byte[] text(MemorySegment stmt, int col) {
        int n = columnBytes(stmt, col);
        if (n <= 0) return new byte[0];
        MemorySegment p = columnText(stmt, col).reinterpret(n);
        byte[] out = new byte[n];
        MemorySegment.copy(p, JAVA_BYTE_LAYOUT, 0, out, 0, n);
        return out;
    }

    /** Copies a text column straight into {@code out}. The pointer is valid until reset. */
    static void appendText(MemorySegment stmt, int col, Out out) {
        int n = columnBytes(stmt, col);
        if (n <= 0) return;
        out.putSegment(columnText(stmt, col).reinterpret(n), n);
    }

    private static final java.lang.foreign.ValueLayout.OfByte JAVA_BYTE_LAYOUT =
            java.lang.foreign.ValueLayout.JAVA_BYTE;

    static MemorySegment columnText(MemorySegment stmt, int col) {
        try { return (MemorySegment) COLUMN_TEXT.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    static int columnBytes(MemorySegment stmt, int col) {
        try { return (int) COLUMN_BYTES.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    static long columnInt64(MemorySegment stmt, int col) {
        try { return (long) COLUMN_INT64.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    private static final MethodHandle FEED_JSON = fn("crux_feed_json",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, JAVA_INT));

    /**
     * Writes one feed object into {@code dst}.
     * @return the byte length, 0 when the caller should use the wide-span query, or {@code -sqlite_rc}
     */
    static int feedJson(MemorySegment ids, MemorySegment likes, MemorySegment cols, MemorySegment dst, int cap) {
        try { return (int) FEED_JSON.invokeExact(ids, likes, cols, dst, cap); }
        catch (Throwable t) { throw rethrow(t); }
    }

    private static final MethodHandle POST_JSON = fn("crux_post_json",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT));

    /**
     * Writes one {@code {"post":...}} object into {@code dst}.
     * @return the byte length, 0 when the post is missing, {@code -1000000} when {@code dst} is
     *         too small, or {@code -sqlite_rc}
     */
    static int postJson(MemorySegment stmt, long id, MemorySegment dst, int cap) {
        try { return (int) POST_JSON.invokeExact(stmt, id, dst, cap); }
        catch (Throwable t) { throw rethrow(t); }
    }

    static int changes(MemorySegment db) {
        try { return (int) CHANGES.invokeExact(db); } catch (Throwable t) { throw rethrow(t); }
    }

    /** {@code mode} is the sqlite3_wal_checkpoint_v2 mode: 0 passive, 2 restart. */
    static int walCheckpoint(MemorySegment db, int mode) {
        try {
            return (int) WAL_CHECKPOINT_V2.invokeExact(db, MemorySegment.NULL, mode, MemorySegment.NULL, MemorySegment.NULL);
        } catch (Throwable t) { throw rethrow(t); }
    }
}
