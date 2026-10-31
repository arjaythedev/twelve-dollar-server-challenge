package feed.db;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.nio.file.Files;
import java.nio.file.Path;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

/**
 * FFM bindings to the SQLite library built by {@code build.sh} ({@code native/libfeedsqlite.so}, path in the
 * system property {@code feed.sqliteLib}).
 *
 * <p>The library is compiled with {@code SQLITE_THREADSAFE=0}: every call must come from the event-loop thread.
 * These wrappers do not check that; {@link FeedDb} does.
 *
 * <p>To add a function: declare a {@code static final MethodHandle} with {@link #downcall} or
 * {@link #downcallCritical} and a static wrapper that calls {@code invokeExact} with the exact types of the
 * descriptor. Only use {@link #downcallCritical} for short calls that can never re-enter Java (bind_*,
 * column_*, changes, get_autocommit). Never for step, reset, prepare or checkpoints:
 * {@code step} of COMMIT runs the wal_hook upcall, and an upcall from a critical downcall is undefined behavior.
 * Critical handles are created with {@code critical(false)}, so they only accept native segments.
 */
public final class Sqlite {

    public static final String EXPECTED_VERSION = "3.53.4";

    public static final int SQLITE_OK = 0;
    public static final int SQLITE_ERROR = 1;
    public static final int SQLITE_CONSTRAINT = 19;
    public static final int SQLITE_MISUSE = 21;
    public static final int SQLITE_ROW = 100;
    public static final int SQLITE_DONE = 101;

    public static final int SQLITE_OPEN_READWRITE = 0x00000002;
    public static final int SQLITE_OPEN_NOMUTEX = 0x00008000;

    public static final int SQLITE_PREPARE_PERSISTENT = 0x01;

    public static final int SQLITE_CHECKPOINT_RESTART = 2;

    /** {@code SQLITE_STATIC}: SQLite neither copies nor frees the bound buffer. */
    public static final MemorySegment SQLITE_STATIC = MemorySegment.NULL;

    /** {@code int (*)(void*, sqlite3*, const char *zDb, int nFrames)} */
    public static final FunctionDescriptor WAL_HOOK_DESCRIPTOR =
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, JAVA_INT);

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB = SymbolLookup.libraryLookup(libraryPath(), Arena.global());
    private static final Linker.Option[] NO_OPTIONS = {};
    private static final Linker.Option[] CRITICAL = {Linker.Option.critical(false)};

    private static final MethodHandle LIBVERSION =
            downcall("sqlite3_libversion", FunctionDescriptor.of(ADDRESS));
    private static final MethodHandle OPEN_V2 =
            downcall("sqlite3_open_v2", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
    private static final MethodHandle CLOSE_V2 =
            downcall("sqlite3_close_v2", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle ERRMSG =
            downcall("sqlite3_errmsg", FunctionDescriptor.of(ADDRESS, ADDRESS));
    private static final MethodHandle ERRSTR =
            downcall("sqlite3_errstr", FunctionDescriptor.of(ADDRESS, JAVA_INT));
    private static final MethodHandle EXTENDED_ERRCODE =
            downcall("sqlite3_extended_errcode", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle PREPARE_V3 =
            downcall("sqlite3_prepare_v3",
                    FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
    private static final MethodHandle STEP =
            downcall("sqlite3_step", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle RESET =
            downcall("sqlite3_reset", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle FINALIZE =
            downcall("sqlite3_finalize", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle WAL_HOOK =
            downcall("sqlite3_wal_hook", FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS, ADDRESS));
    private static final MethodHandle WAL_CHECKPOINT_V2 =
            downcall("sqlite3_wal_checkpoint_v2",
                    FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS, ADDRESS));

    private static final MethodHandle BIND_INT64 =
            downcallCritical("sqlite3_bind_int64", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG));
    private static final MethodHandle BIND_TEXT =
            downcallCritical("sqlite3_bind_text",
                    FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS));
    private static final MethodHandle COLUMN_BLOB =
            downcallCritical("sqlite3_column_blob", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT));
    private static final MethodHandle COLUMN_BYTES =
            downcallCritical("sqlite3_column_bytes", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT));
    private static final MethodHandle COLUMN_INT64 =
            downcallCritical("sqlite3_column_int64", FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT));
    private static final MethodHandle CHANGES =
            downcallCritical("sqlite3_changes", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle GET_AUTOCOMMIT =
            downcallCritical("sqlite3_get_autocommit", FunctionDescriptor.of(JAVA_INT, ADDRESS));

    private Sqlite() {
    }

    // ---- loading

    private static Path libraryPath() {
        String lib = System.getProperty("feed.sqliteLib");
        if (lib == null || lib.isEmpty()) {
            throw new IllegalStateException("system property feed.sqliteLib is not set (path to libfeedsqlite.so)");
        }
        Path path = Path.of(lib).toAbsolutePath();
        if (!Files.isRegularFile(path)) {
            throw new IllegalStateException("SQLite library not found: " + path + " (run build.sh)");
        }
        return path;
    }

    private static MemorySegment symbol(String name) {
        return LIB.find(name).orElseThrow(() -> new UnsatisfiedLinkError("missing SQLite symbol " + name));
    }

    static MethodHandle downcall(String name, FunctionDescriptor descriptor) {
        return LINKER.downcallHandle(symbol(name), descriptor, NO_OPTIONS);
    }

    /** Critical downcall: no Java re-entry allowed, see the class comment. */
    static MethodHandle downcallCritical(String name, FunctionDescriptor descriptor) {
        return LINKER.downcallHandle(symbol(name), descriptor, CRITICAL);
    }

    /** Native function pointer that calls {@code target}. The target must never throw. */
    static MemorySegment upcall(MethodHandle target, FunctionDescriptor descriptor, Arena arena) {
        return LINKER.upcallStub(target, descriptor, arena);
    }

    static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) {
            return r;
        }
        if (t instanceof Error e) {
            throw e;
        }
        return new IllegalStateException(t);
    }

    /** Reads a NUL-terminated UTF-8 string owned by SQLite; null for a NULL pointer. */
    private static String cString(MemorySegment p) {
        return p.equals(MemorySegment.NULL) ? null : p.reinterpret(Integer.MAX_VALUE).getString(0);
    }

    // ---- functions (raw signatures, raw return codes)

    public static String libversion() {
        try { return cString((MemorySegment) LIBVERSION.invokeExact()); } catch (Throwable t) { throw rethrow(t); }
    }

    /** {@code int sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs)} */
    public static int open_v2(MemorySegment filename, MemorySegment ppDb, int flags, MemorySegment zVfs) {
        try { return (int) OPEN_V2.invokeExact(filename, ppDb, flags, zVfs); } catch (Throwable t) { throw rethrow(t); }
    }

    public static int close_v2(MemorySegment db) {
        try { return (int) CLOSE_V2.invokeExact(db); } catch (Throwable t) { throw rethrow(t); }
    }

    public static String errmsg(MemorySegment db) {
        try { return cString((MemorySegment) ERRMSG.invokeExact(db)); } catch (Throwable t) { throw rethrow(t); }
    }

    /** English description of a result code (static string, no connection needed). */
    public static String errstr(int rc) {
        try { return cString((MemorySegment) ERRSTR.invokeExact(rc)); } catch (Throwable t) { throw rethrow(t); }
    }

    public static int extended_errcode(MemorySegment db) {
        try { return (int) EXTENDED_ERRCODE.invokeExact(db); } catch (Throwable t) { throw rethrow(t); }
    }

    /**
     * {@code int sqlite3_prepare_v3(sqlite3*, const char *zSql, int nByte, unsigned int prepFlags,
     * sqlite3_stmt **ppStmt, const char **pzTail)}
     */
    public static int prepare_v3(MemorySegment db, MemorySegment sql, int nByte, int prepFlags,
                                 MemorySegment ppStmt, MemorySegment pzTail) {
        try {
            return (int) PREPARE_V3.invokeExact(db, sql, nByte, prepFlags, ppStmt, pzTail);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    public static int step(MemorySegment stmt) {
        try { return (int) STEP.invokeExact(stmt); } catch (Throwable t) { throw rethrow(t); }
    }

    public static int reset(MemorySegment stmt) {
        try { return (int) RESET.invokeExact(stmt); } catch (Throwable t) { throw rethrow(t); }
    }

    public static int finalize_(MemorySegment stmt) {
        try { return (int) FINALIZE.invokeExact(stmt); } catch (Throwable t) { throw rethrow(t); }
    }

    /**
     * {@code void *sqlite3_wal_hook(sqlite3*, int(*)(void*, sqlite3*, const char*, int), void*)}; returns the
     * previous hook's argument.
     */
    public static MemorySegment wal_hook(MemorySegment db, MemorySegment callback, MemorySegment arg) {
        try {
            return (MemorySegment) WAL_HOOK.invokeExact(db, callback, arg);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    /**
     * {@code int sqlite3_wal_checkpoint_v2(sqlite3*, const char *zDb, int eMode, int *pnLog, int *pnCkpt)}
     */
    public static int wal_checkpoint_v2(MemorySegment db, MemorySegment zDb, int mode, MemorySegment pnLog,
                                        MemorySegment pnCkpt) {
        try {
            return (int) WAL_CHECKPOINT_V2.invokeExact(db, zDb, mode, pnLog, pnCkpt);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    public static int bind_int64(MemorySegment stmt, int index, long value) {
        try { return (int) BIND_INT64.invokeExact(stmt, index, value); } catch (Throwable t) { throw rethrow(t); }
    }

    /** {@code int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int nBytes, void(*)(void*))} */
    public static int bind_text(MemorySegment stmt, int index, MemorySegment text, int nBytes,
                                MemorySegment destructor) {
        try {
            return (int) BIND_TEXT.invokeExact(stmt, index, text, nBytes, destructor);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    /**
     * Pointer to the column's bytes (zero-length segment; reinterpret with {@link #column_bytes}). Call this
     * before {@link #column_bytes}, as SQLite's documentation recommends. NULL for a NULL or empty value.
     */
    public static MemorySegment column_blob(MemorySegment stmt, int col) {
        try { return (MemorySegment) COLUMN_BLOB.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    public static int column_bytes(MemorySegment stmt, int col) {
        try { return (int) COLUMN_BYTES.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    public static long column_int64(MemorySegment stmt, int col) {
        try { return (long) COLUMN_INT64.invokeExact(stmt, col); } catch (Throwable t) { throw rethrow(t); }
    }

    public static int changes(MemorySegment db) {
        try { return (int) CHANGES.invokeExact(db); } catch (Throwable t) { throw rethrow(t); }
    }

    public static int get_autocommit(MemorySegment db) {
        try { return (int) GET_AUTOCOMMIT.invokeExact(db); } catch (Throwable t) { throw rethrow(t); }
    }
}
