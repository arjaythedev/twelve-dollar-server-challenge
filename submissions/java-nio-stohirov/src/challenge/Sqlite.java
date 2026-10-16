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

final class Sqlite {
    static final int OK = 0, ROW = 100, DONE = 101;
    static final int OPEN_READWRITE = 0x2, OPEN_NOMUTEX = 0x8000, OPEN_EXRESCODE = 0x02000000;
    static final int PREPARE_PERSISTENT = 0x01;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB =
            SymbolLookup.libraryLookup(Path.of(System.getProperty("sqlite.lib", "build/libsqlite3.so")), Arena.global());
    private static final Linker.Option CRITICAL = Linker.Option.critical(false);

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
    private static final MethodHandle STEP = fn("sqlite3_step", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle RESET = fn("sqlite3_reset", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle BIND_INT64 =
            fn("sqlite3_bind_int64", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG), CRITICAL);
    private static final MethodHandle BIND_TEXT = fn("sqlite3_bind_text",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS), CRITICAL);
    private static final MethodHandle COLUMN_TEXT =
            fn("sqlite3_column_text", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle COLUMN_BYTES =
            fn("sqlite3_column_bytes", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
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
        try { return (int) OPEN_V2.invokeExact(path, ppDb, flags, MemorySegment.NULL); } catch (Throwable t) { throw rethrow(t); }
    }

    static int exec(MemorySegment db, MemorySegment sql) {
        try {
            return (int) EXEC.invokeExact(db, sql, MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static int prepareV3(MemorySegment db, MemorySegment sql, int flags, MemorySegment ppStmt) {
        try {
            return (int) PREPARE_V3.invokeExact(db, sql, -1, flags, ppStmt, MemorySegment.NULL);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static String errmsg(MemorySegment db) {
        try {
            MemorySegment p = (MemorySegment) ERRMSG.invokeExact(db);
            return p.reinterpret(Long.MAX_VALUE).getString(0);
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
        try {
            return (int) BIND_TEXT.invokeExact(stmt, index, text, nBytes, MemorySegment.NULL);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static MemorySegment columnText(MemorySegment stmt, int col) {
        try { return (MemorySegment) COLUMN_TEXT.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    static int columnBytes(MemorySegment stmt, int col) {
        try { return (int) COLUMN_BYTES.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    static int changes(MemorySegment db) {
        try { return (int) CHANGES.invokeExact(db); } catch (Throwable t) { throw rethrow(t); }
    }

    static int walCheckpointPassive(MemorySegment db) {
        try {
            return (int) WAL_CHECKPOINT_V2.invokeExact(db, MemorySegment.NULL, 0, MemorySegment.NULL, MemorySegment.NULL);
        } catch (Throwable t) { throw rethrow(t); }
    }
}
