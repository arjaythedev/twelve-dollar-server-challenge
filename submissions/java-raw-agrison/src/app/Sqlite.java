package app;

import java.lang.foreign.AddressLayout;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;

/**
 * Raw FFM (Panama) binding to the self-compiled libsqlite3. No JDBC, no wrapper objects:
 * a sqlite3* / sqlite3_stmt* is just a MemorySegment carried around as an opaque handle.
 *
 * Every hot function has two handles. The {@code c}-prefixed ones are {@link Linker.Option#critical}
 * downcalls: no thread-state transition, which is worth a lot when the call itself is a few
 * microseconds (bind/step/column on a warm page cache). They delay safepoints for their duration,
 * so only short, non-blocking calls get one -- the writer thread (whose COMMIT can fsync) uses the
 * plain handles.
 */
final class Sqlite {
    private Sqlite() {}

    static final int OK = 0, ROW = 100, DONE = 101;
    static final int OPEN_READWRITE = 0x00000002, OPEN_NOMUTEX = 0x00008000;
    static final int PREPARE_PERSISTENT = 0x01;

    private static final ValueLayout.OfInt I = ValueLayout.JAVA_INT;
    private static final ValueLayout.OfLong L = ValueLayout.JAVA_LONG;
    private static final AddressLayout P = ValueLayout.ADDRESS;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB = SymbolLookup.libraryLookup(
            Path.of(System.getProperty("app.sqliteLib", "native/libsqlite3.so")), Arena.global());

    private static MemorySegment sym(String name) {
        return LIB.find(name).orElseThrow(() -> new UnsatisfiedLinkError(name));
    }
    private static MethodHandle h(String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(sym(name), fd);
    }
    private static MethodHandle hc(String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(sym(name), fd, Linker.Option.critical(false));
    }

    private static final FunctionDescriptor FD_STEP    = FunctionDescriptor.of(I, P);
    private static final FunctionDescriptor FD_BIND_I  = FunctionDescriptor.of(I, P, I, L);
    private static final FunctionDescriptor FD_BIND_T  = FunctionDescriptor.of(I, P, I, P, I, P);
    private static final FunctionDescriptor FD_COL_P   = FunctionDescriptor.of(P, P, I);
    private static final FunctionDescriptor FD_COL_I   = FunctionDescriptor.of(I, P, I);
    private static final FunctionDescriptor FD_COL_L   = FunctionDescriptor.of(L, P, I);

    private static final MethodHandle INITIALIZE = h("sqlite3_initialize", FunctionDescriptor.of(I));
    private static final MethodHandle OPEN_V2    = h("sqlite3_open_v2", FunctionDescriptor.of(I, P, P, I, P));
    private static final MethodHandle EXEC       = h("sqlite3_exec", FunctionDescriptor.of(I, P, P, P, P, P));
    private static final MethodHandle PREPARE_V3 = h("sqlite3_prepare_v3", FunctionDescriptor.of(I, P, P, I, I, P, P));
    private static final MethodHandle ERRMSG     = h("sqlite3_errmsg", FunctionDescriptor.of(P, P));
    private static final MethodHandle CHANGES    = h("sqlite3_changes", FunctionDescriptor.of(I, P));
    private static final MethodHandle LIBVERSION = h("sqlite3_libversion", FunctionDescriptor.of(P));

    // Writer side: a COMMIT can fsync, so no critical downcalls here.
    private static final MethodHandle STEP      = h("sqlite3_step", FD_STEP);
    private static final MethodHandle RESET     = h("sqlite3_reset", FD_STEP);
    private static final MethodHandle BIND_I64  = h("sqlite3_bind_int64", FD_BIND_I);
    private static final MethodHandle BIND_TEXT = h("sqlite3_bind_text", FD_BIND_T);
    private static final MethodHandle COL_TEXT  = h("sqlite3_column_text", FD_COL_P);
    private static final MethodHandle COL_BYTES = h("sqlite3_column_bytes", FD_COL_I);

    // Read side: short calls on a warm page cache.
    private static final MethodHandle C_STEP      = hc("sqlite3_step", FD_STEP);
    private static final MethodHandle C_RESET     = hc("sqlite3_reset", FD_STEP);
    private static final MethodHandle C_BIND_I64  = hc("sqlite3_bind_int64", FD_BIND_I);
    private static final MethodHandle C_BIND_TEXT = hc("sqlite3_bind_text", FD_BIND_T);
    private static final MethodHandle C_COL_TEXT  = hc("sqlite3_column_text", FD_COL_P);
    private static final MethodHandle C_COL_BYTES = hc("sqlite3_column_bytes", FD_COL_I);
    private static final MethodHandle C_COL_I64   = hc("sqlite3_column_int64", FD_COL_L);

    private static RuntimeException fail(Throwable t) {
        if (t instanceof Error e) throw e;
        if (t instanceof RuntimeException r) return r;
        return new RuntimeException(t);
    }

    static int initialize() {
        try { return (int) INITIALIZE.invokeExact(); } catch (Throwable t) { throw fail(t); }
    }
    static String libVersion() {
        try {
            MemorySegment p = (MemorySegment) LIBVERSION.invokeExact();
            return p.reinterpret(64).getString(0);
        } catch (Throwable t) { throw fail(t); }
    }
    static int openV2(MemorySegment path, MemorySegment outDb, int flags, MemorySegment vfs) {
        try { return (int) OPEN_V2.invokeExact(path, outDb, flags, vfs); } catch (Throwable t) { throw fail(t); }
    }
    static int exec(MemorySegment db, MemorySegment sql) {
        try {
            return (int) EXEC.invokeExact(db, sql, MemorySegment.NULL, MemorySegment.NULL, MemorySegment.NULL);
        } catch (Throwable t) { throw fail(t); }
    }
    static int prepareV3(MemorySegment db, MemorySegment sql, int nBytes, int flags, MemorySegment outStmt) {
        try {
            return (int) PREPARE_V3.invokeExact(db, sql, nBytes, flags, outStmt, MemorySegment.NULL);
        } catch (Throwable t) { throw fail(t); }
    }
    static String errmsg(MemorySegment db) {
        try {
            MemorySegment p = (MemorySegment) ERRMSG.invokeExact(db);
            return p.equals(MemorySegment.NULL) ? "?" : p.reinterpret(512).getString(0);
        } catch (Throwable t) { throw fail(t); }
    }
    static int changes(MemorySegment db) {
        try { return (int) CHANGES.invokeExact(db); } catch (Throwable t) { throw fail(t); }
    }

    static int step(MemorySegment stmt) {
        try { return (int) STEP.invokeExact(stmt); } catch (Throwable t) { throw fail(t); }
    }
    static int reset(MemorySegment stmt) {
        try { return (int) RESET.invokeExact(stmt); } catch (Throwable t) { throw fail(t); }
    }
    static int bindLong(MemorySegment stmt, int idx, long v) {
        try { return (int) BIND_I64.invokeExact(stmt, idx, v); } catch (Throwable t) { throw fail(t); }
    }
    /** Binds text by pointer with SQLITE_STATIC: the bytes must stay put until reset. */
    static int bindText(MemorySegment stmt, int idx, MemorySegment base, long off, int len) {
        try {
            return (int) BIND_TEXT.invokeExact(stmt, idx, base.asSlice(off, len), len, MemorySegment.NULL);
        } catch (Throwable t) { throw fail(t); }
    }
    static MemorySegment columnText(MemorySegment stmt, int col) {
        try { return (MemorySegment) COL_TEXT.invokeExact(stmt, col); } catch (Throwable t) { throw fail(t); }
    }
    static int columnBytes(MemorySegment stmt, int col) {
        try { return (int) COL_BYTES.invokeExact(stmt, col); } catch (Throwable t) { throw fail(t); }
    }

    static int cstep(MemorySegment stmt) {
        try { return (int) C_STEP.invokeExact(stmt); } catch (Throwable t) { throw fail(t); }
    }
    static int creset(MemorySegment stmt) {
        try { return (int) C_RESET.invokeExact(stmt); } catch (Throwable t) { throw fail(t); }
    }
    static int cbindLong(MemorySegment stmt, int idx, long v) {
        try { return (int) C_BIND_I64.invokeExact(stmt, idx, v); } catch (Throwable t) { throw fail(t); }
    }
    static int cbindText(MemorySegment stmt, int idx, MemorySegment base, long off, int len) {
        try {
            return (int) C_BIND_TEXT.invokeExact(stmt, idx, base.asSlice(off, len), len, MemorySegment.NULL);
        } catch (Throwable t) { throw fail(t); }
    }
    static MemorySegment ccolumnText(MemorySegment stmt, int col) {
        try { return (MemorySegment) C_COL_TEXT.invokeExact(stmt, col); } catch (Throwable t) { throw fail(t); }
    }
    static int ccolumnBytes(MemorySegment stmt, int col) {
        try { return (int) C_COL_BYTES.invokeExact(stmt, col); } catch (Throwable t) { throw fail(t); }
    }
    static long ccolumnLong(MemorySegment stmt, int col) {
        try { return (long) C_COL_I64.invokeExact(stmt, col); } catch (Throwable t) { throw fail(t); }
    }
}
