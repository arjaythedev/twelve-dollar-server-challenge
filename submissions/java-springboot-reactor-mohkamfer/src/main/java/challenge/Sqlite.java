package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;
import java.sql.SQLException;

/**
 * A minimal SQLite binding on the Java FFM API (no JNI driver). It covers exactly what this server
 * needs: open, exec, prepared statements, int64/text binds and int64/text columns.
 *
 * <p>Calls that only touch the statement in memory (bind, column) are "critical" downcalls, which
 * skip the Java-to-native thread-state transition. Calls that may do I/O (step, reset, exec) are
 * regular downcalls. Every object here is confined to one thread at a time.
 */
final class Sqlite {

    static final int OK = 0;
    static final int ROW = 100;
    static final int DONE = 101;
    static final int OPEN_READWRITE = 0x00000002;
    static final int OPEN_NOMUTEX = 0x00008000;
    private static final int PREPARE_PERSISTENT = 0x01;
    private static final MemorySegment TRANSIENT = MemorySegment.ofAddress(-1L);

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB =
            SymbolLookup.libraryLookup(Path.of(System.getProperty("app.sqliteLib", "libsqlite3.so.0")), Arena.global());

    private static MethodHandle downcall(String name, boolean critical, MemoryLayout ret, MemoryLayout... args) {
        FunctionDescriptor fd = FunctionDescriptor.of(ret, args);
        MemorySegment addr = LIB.findOrThrow(name);
        // critical(true) also lets heap arrays be passed without copying them off-heap first
        return critical ? LINKER.downcallHandle(addr, fd, Linker.Option.critical(true)) : LINKER.downcallHandle(addr, fd);
    }

    private static final MethodHandle OPEN = downcall("sqlite3_open_v2", false, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS);
    private static final MethodHandle EXEC = downcall("sqlite3_exec", false, JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS);
    private static final MethodHandle BUSY_TIMEOUT = downcall("sqlite3_busy_timeout", false, JAVA_INT, ADDRESS, JAVA_INT);
    private static final MethodHandle PREPARE =
            downcall("sqlite3_prepare_v3", false, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS);
    private static final MethodHandle STEP = downcall("sqlite3_step", false, JAVA_INT, ADDRESS);
    private static final MethodHandle RESET = downcall("sqlite3_reset", false, JAVA_INT, ADDRESS);
    private static final MethodHandle BIND_INT64 = downcall("sqlite3_bind_int64", true, JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG);
    private static final MethodHandle BIND_TEXT =
            downcall("sqlite3_bind_text", true, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS);
    private static final MethodHandle COLUMN_INT64 = downcall("sqlite3_column_int64", true, JAVA_LONG, ADDRESS, JAVA_INT);
    private static final MethodHandle COLUMN_TEXT = downcall("sqlite3_column_text", true, ADDRESS, ADDRESS, JAVA_INT);
    private static final MethodHandle COLUMN_BYTES = downcall("sqlite3_column_bytes", true, JAVA_INT, ADDRESS, JAVA_INT);
    private static final MethodHandle CHANGES = downcall("sqlite3_changes", true, JAVA_INT, ADDRESS);
    private static final MethodHandle ERRMSG = downcall("sqlite3_errmsg", true, ADDRESS, ADDRESS);

    private Sqlite() {}

    static final class Conn {
        private final MemorySegment db;

        Conn(String path, int flags) throws SQLException {
            int rc;
            MemorySegment handle;
            try (Arena a = Arena.ofConfined()) {
                MemorySegment pp = a.allocate(ADDRESS);
                rc = (int) OPEN.invokeExact(a.allocateFrom(path), pp, flags, MemorySegment.NULL);
                handle = pp.get(ADDRESS, 0);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            db = handle;
            if (rc != OK) throw error("open " + path, rc);
        }

        void busyTimeout(int ms) throws SQLException {
            int rc;
            try {
                rc = (int) BUSY_TIMEOUT.invokeExact(db, ms);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            check(rc, "busy_timeout");
        }

        void exec(String sql) throws SQLException {
            int rc;
            try (Arena a = Arena.ofConfined()) {
                rc = (int) EXEC.invokeExact(db, a.allocateFrom(sql), MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            check(rc, sql);
        }

        Stmt prepare(String sql) throws SQLException {
            int rc;
            MemorySegment handle;
            try (Arena a = Arena.ofConfined()) {
                MemorySegment pp = a.allocate(ADDRESS);
                rc = (int) PREPARE.invokeExact(db, a.allocateFrom(sql), -1, PREPARE_PERSISTENT, pp, MemorySegment.NULL);
                handle = pp.get(ADDRESS, 0);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            check(rc, sql);
            return new Stmt(this, handle);
        }

        int changes() {
            try {
                return (int) CHANGES.invokeExact(db);
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }

        private void check(int rc, String what) throws SQLException {
            if (rc != OK) throw error(what, rc);
        }

        SQLException error(String what, int rc) {
            String msg;
            try {
                MemorySegment p = db.equals(MemorySegment.NULL) ? MemorySegment.NULL : (MemorySegment) ERRMSG.invokeExact(db);
                msg = p.equals(MemorySegment.NULL) ? "unknown error" : p.reinterpret(Integer.MAX_VALUE).getString(0);
            } catch (Throwable t) {
                msg = String.valueOf(t);
            }
            return new SQLException(what + ": " + msg + " (rc=" + rc + ")", null, rc);
        }
    }

    static final class Stmt {
        private final Conn conn;
        private final MemorySegment handle;

        private Stmt(Conn conn, MemorySegment handle) {
            this.conn = conn;
            this.handle = handle;
        }

        /** Binds a 1-based parameter. */
        Stmt bind(int index, long v) throws SQLException {
            int rc;
            try {
                rc = (int) BIND_INT64.invokeExact(handle, index, v);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            if (rc != OK) throw conn.error("bind", rc);
            return this;
        }

        /** Binds UTF-8 text; SQLite copies it (SQLITE_TRANSIENT), so the array may be reused at once. */
        Stmt bind(int index, byte[] utf8) throws SQLException {
            int rc;
            try {
                rc = (int) BIND_TEXT.invokeExact(handle, index, MemorySegment.ofArray(utf8), utf8.length, TRANSIENT);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            if (rc != OK) throw conn.error("bind", rc);
            return this;
        }

        /** True for a row, false when done. */
        boolean step() throws SQLException {
            int rc;
            try {
                rc = (int) STEP.invokeExact(handle);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            if (rc == ROW) return true;
            if (rc == DONE) return false;
            throw conn.error("step", rc);
        }

        /** Steps a statement that returns no rows through to completion. */
        void run() throws SQLException {
            try {
                while (step()) {
                    // drain
                }
            } finally {
                reset();
            }
        }

        void reset() throws SQLException {
            int rc;
            try {
                rc = (int) RESET.invokeExact(handle);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            if (rc != OK) throw conn.error("reset", rc);
        }

        /** Reads a 0-based column. */
        long getLong(int col) {
            try {
                return (long) COLUMN_INT64.invokeExact(handle, col);
            } catch (Throwable t) {
                throw unchecked(t);
            }
        }

        /** Appends a 0-based text column to {@code out} as a JSON string, straight from SQLite's memory. */
        void textTo(int col, Out out) {
            MemorySegment text;
            int len;
            try {
                text = (MemorySegment) COLUMN_TEXT.invokeExact(handle, col); // must come before column_bytes
                len = (int) COLUMN_BYTES.invokeExact(handle, col);
            } catch (Throwable t) {
                throw unchecked(t);
            }
            out.str(text.reinterpret(len), len);
        }
    }

    static RuntimeException unchecked(Throwable t) {
        if (t instanceof RuntimeException r) return r;
        if (t instanceof Error e) throw e;
        return new IllegalStateException(t);
    }

    /** Copies native bytes into a heap array (used by {@link Out}). */
    static void copy(MemorySegment src, byte[] dst, int dstIndex, int len) {
        MemorySegment.copy(src, JAVA_BYTE, 0, dst, dstIndex, len);
    }
}
