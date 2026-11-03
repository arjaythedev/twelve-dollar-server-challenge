package challenge;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.sql.SQLException;
import java.util.*;
import static java.lang.foreign.ValueLayout.*;

/** Small FFM binding. Connections are exclusively accessed under Database's Java lock.
 * Connections own their statements and copied text values; the last close shuts down and unloads the library. */
final class Sqlite {
    interface Conn extends AutoCloseable {
        Stmt createStatement() throws SQLException;
        Stmt prepareStatement(String sql) throws SQLException;
        void setAutoCommit(boolean enabled) throws SQLException;
        boolean getAutoCommit() throws SQLException;
        void commit() throws SQLException;
        void rollback() throws SQLException;
        String setSavepoint(String name) throws SQLException;
        void rollback(String name) throws SQLException;
        void releaseSavepoint(String name) throws SQLException;
        void close() throws SQLException;
    }
    interface Stmt extends AutoCloseable {
        void setLong(int index, long value) throws SQLException;
        void setString(int index, String value) throws SQLException;
        Rows executeQuery() throws SQLException;
        int executeUpdate() throws SQLException;
        boolean execute(String sql) throws SQLException;
        void close() throws SQLException;
    }
    interface Rows extends AutoCloseable {
        boolean next() throws SQLException;
        long getLong(int index) throws SQLException;
        int getInt(int index) throws SQLException;
        String getString(int index) throws SQLException;
        byte[] getBytes(int index) throws SQLException;
        void close() throws SQLException;
    }
    static Conn open(String path, Properties ignored) throws SQLException { return new NativeConn(path); }
    private static final MemorySegment TRANSIENT = MemorySegment.ofAddress(-1);
    private static RuntimeException unchecked(Throwable t) {
        if (t instanceof Error e) throw e;
        return t instanceof RuntimeException r ? r : new IllegalStateException(t);
    }
    private static Api shared;
    private static int users;
    private static synchronized Api acquire() {
        if (shared == null) shared = new Api();
        users++;
        return shared;
    }
    private static synchronized void release(Api api) throws SQLException {
        if (api != shared || users <= 0) throw new IllegalStateException("Library ownership mismatch");
        if (--users == 0) {
            int rc;
            try { rc = (int) api.shutdown.invokeExact(); }
            catch (Throwable t) { throw unchecked(t); }
            if (rc != 0) throw new SQLException("SQLite shutdown failed", null, rc);
            api.arena.close();
            shared = null;
        }
    }
    private static final class Api {
        final Arena arena = Arena.ofShared();
        final SymbolLookup library;
        Api() {
            try { library = SymbolLookup.libraryLookup(Path.of(System.getProperty("app.sqliteLib", "target/native/libsqlite3.so")).toAbsolutePath(), arena);
            shutdown = call("sqlite3_shutdown", false, JAVA_INT);
            open = call("sqlite3_open_v2", false, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, ADDRESS);
            exec = call("sqlite3_exec", false, JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS);
            prepare = call("sqlite3_prepare_v3", false, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS);
            step = call("sqlite3_step", false, JAVA_INT, ADDRESS);
            reset = call("sqlite3_reset", false, JAVA_INT, ADDRESS);
            finalizeStmt = call("sqlite3_finalize", false, JAVA_INT, ADDRESS);
            closeDb = call("sqlite3_close", false, JAVA_INT, ADDRESS);
            bindLong = call("sqlite3_bind_int64", true, JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG);
            bindText = call("sqlite3_bind_text", false, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS);
            columnLong = call("sqlite3_column_int64", true, JAVA_LONG, ADDRESS, JAVA_INT);
            columnText = call("sqlite3_column_text", false, ADDRESS, ADDRESS, JAVA_INT);
            columnBytes = call("sqlite3_column_bytes", true, JAVA_INT, ADDRESS, JAVA_INT);
            changes = call("sqlite3_changes", true, JAVA_INT, ADDRESS);
            errmsg = call("sqlite3_errmsg", true, ADDRESS, ADDRESS);
            autoCommit = call("sqlite3_get_autocommit", true, JAVA_INT, ADDRESS);
            }
            catch (Throwable t) { arena.close(); throw t; }
        }
        MethodHandle call(String name, boolean critical, MemoryLayout result, MemoryLayout... args) {
            return Linker.nativeLinker().downcallHandle(library.findOrThrow(name), FunctionDescriptor.of(result, args),
                critical ? new Linker.Option[]{Linker.Option.critical(true)} : new Linker.Option[0]);
        }
        final MethodHandle shutdown, open, exec, prepare, step, reset, finalizeStmt, closeDb, bindLong, bindText, columnLong, columnText, columnBytes, changes, errmsg, autoCommit;
    }
    private static final class NativeConn implements Conn {
        final Api api = acquire();
        MemorySegment db = MemorySegment.NULL;
        final Set<NativeStmt> statements = new HashSet<>();
        boolean closed;
        NativeConn(String path) throws SQLException {
            try {
                int rc;
                try (Arena a = Arena.ofConfined()) {
                    MemorySegment output = a.allocate(ADDRESS);
                    // Step 11 rejected NOMUTEX: force serialized mode, even in THREADSAFE=2 builds.
                    rc = (int) api.open.invokeExact(a.allocateFrom(path), output, 0x00000002 | 0x00010000, MemorySegment.NULL);
                    db = output.get(ADDRESS, 0);
                }
                check(rc, "open");
            } catch (Throwable t) {
                try { close(); } catch (Throwable cleanup) { t.addSuppressed(cleanup); }
                if (t instanceof SQLException sql) throw sql;
                throw unchecked(t);
            }
        }
        void ensureOpen() throws SQLException { if (closed) throw new SQLException("Connection closed"); }
        SQLException error(int rc, String operation) {
            String message = "SQLite error";
            if (!db.equals(MemorySegment.NULL)) {
                try { message = ((MemorySegment) api.errmsg.invokeExact(db)).reinterpret(Integer.MAX_VALUE).getString(0); }
                catch (Throwable t) { message += ": " + t; }
            }
            return new SQLException(operation + ": " + message, null, rc);
        }
        void check(int rc, String operation) throws SQLException { if (rc != 0) throw error(rc, operation); }
        void exec(String sql) throws SQLException {
            ensureOpen();
            int rc;
            try (Arena a = Arena.ofConfined()) {
                rc = (int) api.exec.invokeExact(db, a.allocateFrom(sql), MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL);
            } catch (Throwable t) { throw unchecked(t); }
            check(rc, sql);
        }
        public Stmt createStatement() throws SQLException { ensureOpen(); return new NativeStmt(this, MemorySegment.NULL); }
        public Stmt prepareStatement(String sql) throws SQLException {
            ensureOpen();
            MemorySegment handle = MemorySegment.NULL;
            int rc;
            try (Arena a = Arena.ofConfined()) {
                MemorySegment output = a.allocate(ADDRESS);
                rc = (int) api.prepare.invokeExact(db, a.allocateFrom(sql), -1, 1, output, MemorySegment.NULL);
                handle = output.get(ADDRESS, 0);
            } catch (Throwable t) { throw unchecked(t); }
            if (rc != 0) {
                SQLException error = error(rc, "prepare");
                if (!handle.equals(MemorySegment.NULL)) {
                    try { int cleanup = (int) api.finalizeStmt.invokeExact(handle); }
                    catch (Throwable t) { error.addSuppressed(t); }
                }
                throw error;
            }
            return new NativeStmt(this, handle);
        }
        public boolean getAutoCommit() throws SQLException {
            ensureOpen();
            try { return (int) api.autoCommit.invokeExact(db) != 0; }
            catch (Throwable t) { throw unchecked(t); }
        }
        public void setAutoCommit(boolean enabled) throws SQLException {
            if (enabled != getAutoCommit()) exec(enabled ? "COMMIT" : "BEGIN IMMEDIATE");
        }
        public void commit() throws SQLException { exec("COMMIT"); }
        public void rollback() throws SQLException { exec("ROLLBACK"); }
        public String setSavepoint(String name) throws SQLException { exec("SAVEPOINT " + name); return name; }
        public void rollback(String name) throws SQLException { exec("ROLLBACK TO " + name); }
        public void releaseSavepoint(String name) throws SQLException { exec("RELEASE " + name); }
        public void close() throws SQLException {
            if (closed) return;
            SQLException failure = null;
            for (NativeStmt stmt : List.copyOf(statements)) {
                try { stmt.close(); } catch (SQLException e) { if (failure == null) failure = e; else failure.addSuppressed(e); }
            }
            if (!db.equals(MemorySegment.NULL)) {
                int rc;
                try { rc = (int) api.closeDb.invokeExact(db); }
                catch (Throwable t) { throw unchecked(t); }
                if (rc != 0) { SQLException e = error(rc, "close"); if (failure != null) e.addSuppressed(failure); throw e; }
                db = MemorySegment.NULL;
            }
            closed = true;
            release(api);
            if (failure != null) throw failure;
        }
    }
    private static final class NativeStmt implements Stmt {
        final NativeConn conn;
        final MemorySegment handle;
        boolean closed, active, failed;
        NativeStmt(NativeConn conn, MemorySegment handle) {
            this.conn = conn; this.handle = handle; conn.statements.add(this);
        }
        void ensureOpen() throws SQLException {
            conn.ensureOpen();
            if (closed) throw new SQLException("Statement closed");
        }
        public void setLong(int index, long value) throws SQLException {
            ensureOpen(); int rc;
            try { rc = (int) conn.api.bindLong.invokeExact(handle, index, value); }
            catch (Throwable t) { throw unchecked(t); }
            conn.check(rc, "bind integer");
        }
        public void setString(int index, String value) throws SQLException {
            ensureOpen(); int rc;
            byte[] utf8 = value.getBytes(StandardCharsets.UTF_8);
            try (Arena a = Arena.ofConfined()) {
                MemorySegment bytes = a.allocate(Math.max(1, utf8.length));
                MemorySegment.copy(utf8, 0, bytes, JAVA_BYTE, 0, utf8.length);
                rc = (int) conn.api.bindText.invokeExact(handle, index, bytes, utf8.length, TRANSIENT);
            } catch (Throwable t) { throw unchecked(t); }
            conn.check(rc, "bind text");
        }
        boolean step() throws SQLException {
            ensureOpen(); int rc;
            try { rc = (int) conn.api.step.invokeExact(handle); }
            catch (Throwable t) { throw unchecked(t); }
            if (rc == 100) return true;
            if (rc == 101) return false;
            failed = true;
            throw conn.error(rc, "step");
        }
        void reset() throws SQLException {
            if (!active) return;
            int rc;
            try { rc = (int) conn.api.reset.invokeExact(handle); }
            catch (Throwable t) { throw unchecked(t); }
            active = false;
            // reset repeats a prior step error; it has already been reported. A new reset error is fatal.
            boolean prior = failed; failed = false;
            if (!prior) conn.check(rc, "reset");
        }
        public Rows executeQuery() throws SQLException {
            ensureOpen(); if (active) throw new SQLException("Result already open");
            active = true;
            return new NativeRows(this);
        }
        public int executeUpdate() throws SQLException {
            try (Rows rows = executeQuery()) { while (rows.next()) {} }
            try { return (int) conn.api.changes.invokeExact(conn.db); }
            catch (Throwable t) { throw unchecked(t); }
        }
        public boolean execute(String sql) throws SQLException { ensureOpen(); conn.exec(sql); return false; }
        public void close() throws SQLException {
            if (closed) return;
            SQLException failure = null;
            if (!handle.equals(MemorySegment.NULL)) {
                try { reset(); } catch (SQLException error) { failure = error; }
                int rc;
                try { rc = (int) conn.api.finalizeStmt.invokeExact(handle); }
                catch (Throwable t) { throw unchecked(t); }
                closed = true; conn.statements.remove(this);
                if (rc != 0) {
                    SQLException error = conn.error(rc, "finalize");
                    if (failure == null) failure = error; else failure.addSuppressed(error);
                }
            } else { closed = true; conn.statements.remove(this); }
            if (failure != null) throw failure;
        }
    }
    private static final class NativeRows implements Rows {
        final NativeStmt stmt;
        boolean closed, row, done;
        NativeRows(NativeStmt stmt) { this.stmt = stmt; }
        public boolean next() throws SQLException {
            if (closed) throw new SQLException("Result closed");
            if (done) return false;
            row = stmt.step(); done = !row; return row;
        }
        void ensureRow() throws SQLException { stmt.ensureOpen(); if (closed || !row) throw new SQLException("No current row"); }
        public long getLong(int index) throws SQLException {
            ensureRow();
            try { return (long) stmt.conn.api.columnLong.invokeExact(stmt.handle, index - 1); }
            catch (Throwable t) { throw unchecked(t); }
        }
        public int getInt(int index) throws SQLException { return (int) getLong(index); }
        public String getString(int index) throws SQLException {
            byte[] bytes = getBytes(index);
            return bytes == null ? null : new String(bytes, StandardCharsets.UTF_8);
        }
        public byte[] getBytes(int index) throws SQLException {
            ensureRow();
            try {
                MemorySegment text = (MemorySegment) stmt.conn.api.columnText.invokeExact(stmt.handle, index - 1);
                int length = (int) stmt.conn.api.columnBytes.invokeExact(stmt.handle, index - 1);
                if (text.equals(MemorySegment.NULL)) return null;
                return text.reinterpret(length).toArray(JAVA_BYTE);
            } catch (Throwable t) { throw unchecked(t); }
        }
        public void close() throws SQLException { if (!closed) { closed = true; row = false; stmt.reset(); } }
    }
}
