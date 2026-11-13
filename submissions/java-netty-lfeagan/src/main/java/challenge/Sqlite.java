package challenge;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

/**
 * A minimal FFM binding to the SQLite C API: only the calls this server makes. The library is the
 * amalgamation that build.sh compiles, loaded from -Dsqlite.lib.
 *
 * Short calls that never block or call back into Java (bind, column, reset, changes) are linked as
 * "critical", which skips the thread-state transition. step, prepare, exec and open can do I/O, so
 * they use regular downcalls.
 */
final class Sqlite {
    static final int OK = 0, ROW = 100, DONE = 101;

    private static final int OPEN_READWRITE = 0x00000002;
    private static final int OPEN_NOMUTEX = 0x00008000;
    private static final int PREPARE_PERSISTENT = 0x01;
    private static final MemorySegment TRANSIENT = MemorySegment.ofAddress(-1);

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB =
            SymbolLookup.libraryLookup(Path.of(System.getProperty("sqlite.lib")), Arena.global());

    private static final MethodHandle OPEN = fn("sqlite3_open_v2", false, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS);
    private static final MethodHandle CLOSE = fn("sqlite3_close_v2", false, JAVA_INT, ADDRESS);
    private static final MethodHandle EXEC = fn("sqlite3_exec", false, JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS);
    private static final MethodHandle PREPARE = fn("sqlite3_prepare_v3", false, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS);
    private static final MethodHandle STEP = fn("sqlite3_step", false, JAVA_INT, ADDRESS);
    private static final MethodHandle RESET = fn("sqlite3_reset", true, JAVA_INT, ADDRESS);
    private static final MethodHandle BIND_INT64 = fn("sqlite3_bind_int64", true, JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG);
    private static final MethodHandle BIND_TEXT = fn("sqlite3_bind_text", true, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS);
    private static final MethodHandle COLUMN_INT64 = fn("sqlite3_column_int64", true, JAVA_LONG, ADDRESS, JAVA_INT);
    private static final MethodHandle COLUMN_TEXT = fn("sqlite3_column_text", true, ADDRESS, ADDRESS, JAVA_INT);
    private static final MethodHandle COLUMN_BYTES = fn("sqlite3_column_bytes", true, JAVA_INT, ADDRESS, JAVA_INT);
    private static final MethodHandle CHANGES = fn("sqlite3_changes", true, JAVA_INT, ADDRESS);
    private static final MethodHandle ERRMSG = fn("sqlite3_errmsg", true, ADDRESS, ADDRESS);

    private Sqlite() {}

    private static MethodHandle fn(String name, boolean critical, MemoryLayout ret, MemoryLayout... args) {
        var desc = FunctionDescriptor.of(ret, args);
        var sym = LIB.find(name).orElseThrow(() -> new UnsatisfiedLinkError(name));
        return critical ? LINKER.downcallHandle(sym, desc, Linker.Option.critical(false)) : LINKER.downcallHandle(sym, desc);
    }

    static final class SqliteException extends RuntimeException {
        SqliteException(String message) { super(message); }
    }

    static MemorySegment open(String path) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(ADDRESS);
            int rc = (int) OPEN.invokeExact(arena.allocateFrom(path), out, OPEN_READWRITE | OPEN_NOMUTEX, MemorySegment.NULL);
            MemorySegment db = out.get(ADDRESS, 0);
            if (rc != OK) {
                String msg = db.equals(MemorySegment.NULL) ? "sqlite3_open_v2 failed: " + rc : errmsg(db);
                close(db);
                throw new SqliteException(msg);
            }
            return db;
        } catch (SqliteException e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    static void close(MemorySegment db) {
        try {
            int unused = (int) CLOSE.invokeExact(db);
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    static void exec(MemorySegment db, String sql) {
        try (Arena arena = Arena.ofConfined()) {
            int rc = (int) EXEC.invokeExact(db, arena.allocateFrom(sql), MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL);
            if (rc != OK) throw new SqliteException(errmsg(db));
        } catch (SqliteException e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    /** Prepares a statement that lives as long as the connection. */
    static MemorySegment prepare(MemorySegment db, String sql) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(ADDRESS);
            int rc = (int) PREPARE.invokeExact(db, arena.allocateFrom(sql), -1, PREPARE_PERSISTENT, out, MemorySegment.NULL);
            if (rc != OK) throw new SqliteException(errmsg(db) + ": " + sql);
            return out.get(ADDRESS, 0);
        } catch (SqliteException e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    /** Returns ROW or DONE; anything else throws. */
    static int step(MemorySegment db, MemorySegment stmt) {
        int rc;
        try {
            rc = (int) STEP.invokeExact(stmt);
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
        if (rc != ROW && rc != DONE) throw new SqliteException(errmsg(db));
        return rc;
    }

    static void reset(MemorySegment stmt) {
        try {
            int unused = (int) RESET.invokeExact(stmt);
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    static void bindLong(MemorySegment stmt, int index, long value) {
        try {
            int rc = (int) BIND_INT64.invokeExact(stmt, index, value);
            if (rc != OK) throw new SqliteException("bind failed: " + rc);
        } catch (SqliteException e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    /** Binds len bytes of UTF-8 at text; SQLite copies them (SQLITE_TRANSIENT). */
    static void bindText(MemorySegment stmt, int index, MemorySegment text, int len) {
        try {
            int rc = (int) BIND_TEXT.invokeExact(stmt, index, text, len, TRANSIENT);
            if (rc != OK) throw new SqliteException("bind failed: " + rc);
        } catch (SqliteException e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    static long columnLong(MemorySegment stmt, int col) {
        try {
            return (long) COLUMN_INT64.invokeExact(stmt, col);
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    /** The UTF-8 bytes of a text column, valid until the next step/reset on stmt. */
    static MemorySegment columnText(MemorySegment stmt, int col) {
        try {
            MemorySegment p = (MemorySegment) COLUMN_TEXT.invokeExact(stmt, col);
            int len = (int) COLUMN_BYTES.invokeExact(stmt, col);
            return p.reinterpret(len);
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    static int changes(MemorySegment db) {
        try {
            return (int) CHANGES.invokeExact(db);
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    static String errmsg(MemorySegment db) {
        try {
            MemorySegment p = (MemorySegment) ERRMSG.invokeExact(db);
            return p.reinterpret(Integer.MAX_VALUE).getString(0);
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }
}
