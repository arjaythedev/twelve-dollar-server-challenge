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

/** FFM bindings to the SQLite API and to csrc/shim.c, which are one shared library. */
final class Sqlite {
    static final int OK = 0, ROW = 100, DONE = 101;
    static final int PREPARE_PERSISTENT = 0x01;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB =
            SymbolLookup.libraryLookup(Path.of(System.getProperty("shim.lib", "build/libshim.so")), Arena.global());
    private static final Linker.Option CRITICAL = Linker.Option.critical(false);

    private static MethodHandle fn(String name, FunctionDescriptor fd, Linker.Option... options) {
        return LINKER.downcallHandle(LIB.find(name).orElseThrow(() -> new UnsatisfiedLinkError(name)), fd, options);
    }

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
            FunctionDescriptor.of(JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_INT, JAVA_LONG, JAVA_LONG));

    private static final MethodHandle TD_OPEN =
            fn("td_open", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT));
    private static final MethodHandle TD_DB = fn("td_db", FunctionDescriptor.of(ADDRESS));
    private static final MethodHandle TD_CKDB = fn("td_ckdb", FunctionDescriptor.of(JAVA_LONG));
    private static final MethodHandle TD_WAL_FRAMES = fn("td_wal_frames", FunctionDescriptor.of(JAVA_INT), CRITICAL);
    private static final MethodHandle TD_WAL_FRAMES_RESET = fn("td_wal_frames_reset", FunctionDescriptor.ofVoid(), CRITICAL);
    private static final MethodHandle TD_BEGIN = fn("td_begin", FunctionDescriptor.of(JAVA_INT));
    private static final MethodHandle TD_COMMIT = fn("td_commit", FunctionDescriptor.of(JAVA_INT));
    // Pointers are plain longs on the hot path: no MemorySegment to allocate per request.
    private static final MethodHandle TD_FEED =
            fn("td_feed", FunctionDescriptor.of(JAVA_INT, JAVA_LONG, JAVA_INT), CRITICAL);
    private static final MethodHandle TD_POST =
            fn("td_post", FunctionDescriptor.of(JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_INT), CRITICAL);
    private static final MethodHandle TD_POST_EXISTS =
            fn("td_post_exists", FunctionDescriptor.of(JAVA_INT, JAVA_LONG), CRITICAL);

    private Sqlite() {}

    static final class SqliteException extends RuntimeException {
        SqliteException(String message) { super(message); }
    }

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) return r;
        if (t instanceof Error e) throw e;
        return new IllegalStateException(t);
    }

    /** Opens the database and returns its connection. */
    static MemorySegment open(String path) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment err = a.allocate(512);
            int rc = (int) TD_OPEN.invokeExact(a.allocateFrom(path), err, 512);
            if (rc != OK) throw new SqliteException(err.getString(0));
            return (MemorySegment) TD_DB.invokeExact();
        } catch (Throwable t) { throw rethrow(t); }
    }

    static long checkpointConnection() {
        try { return (long) TD_CKDB.invokeExact(); } catch (Throwable t) { throw rethrow(t); }
    }

    static int walFrames() {
        try { return (int) TD_WAL_FRAMES.invokeExact(); } catch (Throwable t) { throw rethrow(t); }
    }

    static void walFramesReset() {
        try { TD_WAL_FRAMES_RESET.invokeExact(); } catch (Throwable t) { throw rethrow(t); }
    }

    static int begin() {
        try { return (int) TD_BEGIN.invokeExact(); } catch (Throwable t) { throw rethrow(t); }
    }

    static int commit() {
        try { return (int) TD_COMMIT.invokeExact(); } catch (Throwable t) { throw rethrow(t); }
    }

    static int feed(long out, int cap) {
        try { return (int) TD_FEED.invokeExact(out, cap); } catch (Throwable t) { throw rethrow(t); }
    }

    static int post(long id, long out, int cap) {
        try { return (int) TD_POST.invokeExact(id, out, cap); } catch (Throwable t) { throw rethrow(t); }
    }

    static int postExists(long id) {
        try { return (int) TD_POST_EXISTS.invokeExact(id); } catch (Throwable t) { throw rethrow(t); }
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

    /**
     * One PASSIVE checkpoint: true if it left nothing to copy back. Called from the checkpoint thread and,
     * as a last resort, from the event loop, so its two result words are not shared between threads.
     */
    static boolean walCheckpointPassive(long connection) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment logAndDone = a.allocate(8, 4);
            logAndDone.set(JAVA_INT, 0, -1);
            logAndDone.set(JAVA_INT, 4, -2);
            int rc = (int) WAL_CHECKPOINT_V2.invokeExact(connection, 0L, 0, logAndDone.address(), logAndDone.address() + 4);
            if (rc != OK) {
                System.err.println("checkpoint failed: rc " + rc);
                return false;
            }
            return logAndDone.get(JAVA_INT, 0) == logAndDone.get(JAVA_INT, 4);
        } catch (Throwable t) { throw rethrow(t); }
    }
}
