package feed;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

/**
 * FFM downcalls into {@code libsqlite3.so.0}. Accessors that never block are critical (no thread-state transition);
 * {@code sqlite3_step} is not, since a COMMIT fsyncs and a critical call would hold up safepoints meanwhile.
 */
final class Sqlite {
    private Sqlite() {}

    static final int OK = 0, ROW = 100, DONE = 101;
    static final int OPEN_READWRITE = 0x00000002, OPEN_NOMUTEX = 0x00008000, OPEN_EXRESCODE = 0x02000000;
    static final int PREPARE_PERSISTENT = 0x01;
    /** {@code SQLITE_TRANSIENT}: SQLite copies a bound value before the bind call returns. */
    static final MemorySegment TRANSIENT = MemorySegment.ofAddress(-1L);

    private static final Linker LINKER = Linker.nativeLinker();
    static final String LIBRARY = System.getProperty("feed.sqlite.lib", "libsqlite3.so.0");
    private static final SymbolLookup LIB = SymbolLookup.libraryLookup(LIBRARY, Arena.global());
    private static final Linker.Option CRITICAL = Linker.Option.critical(false);

    private static MethodHandle fn(String name, FunctionDescriptor descriptor, Linker.Option... options) {
        return LINKER.downcallHandle(LIB.findOrThrow(name), descriptor, options);
    }

    static final MethodHandle libversion = fn("sqlite3_libversion", FunctionDescriptor.of(ADDRESS));
    static final MethodHandle open_v2 = fn("sqlite3_open_v2", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
    static final MethodHandle close_v2 = fn("sqlite3_close_v2", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle exec = fn("sqlite3_exec", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS));
    static final MethodHandle errmsg = fn("sqlite3_errmsg", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle prepare_v3 = fn("sqlite3_prepare_v3",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
    static final MethodHandle finalize_ = fn("sqlite3_finalize", FunctionDescriptor.of(JAVA_INT, ADDRESS));

    static final MethodHandle step = fn("sqlite3_step", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle reset = fn("sqlite3_reset", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
    static final MethodHandle bind_int64 = fn("sqlite3_bind_int64", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG), CRITICAL);
    static final MethodHandle bind_text = fn("sqlite3_bind_text",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS), CRITICAL);
    static final MethodHandle column_int64 = fn("sqlite3_column_int64", FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT), CRITICAL);
    static final MethodHandle column_text = fn("sqlite3_column_text", FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT), CRITICAL);
    static final MethodHandle column_bytes = fn("sqlite3_column_bytes", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
    static final MethodHandle changes = fn("sqlite3_changes", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
    static final MethodHandle get_autocommit = fn("sqlite3_get_autocommit", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);

    static final class SqliteException extends RuntimeException {
        SqliteException(int code, String message) {
            super(message + " (sqlite code " + code + ")");
        }
    }

    static String string(MemorySegment cString) {
        return cString.equals(MemorySegment.NULL) ? null : cString.reinterpret(Long.MAX_VALUE).getString(0);
    }

    static String version() {
        try {
            return string((MemorySegment) libversion.invokeExact());
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) return r;
        if (t instanceof Error e) throw e;
        return new IllegalStateException(t);
    }
}
