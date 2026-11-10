import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;

import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

/** The calls into libfeed.so (net.c and db.c), and native memory as Java reads it.
 *
 * Every pointer crosses as a 64-bit integer: on the 64-bit ABIs this runs on a pointer argument
 * and a long are passed the same way, and Java never needs a MemorySegment object per call.
 * Calls that are short and never block (queueing an operation, moving a cursor, buffer
 * bookkeeping) are linked with Linker.Option.critical(false): the thread stays in Java, which
 * saves the state transition of an ordinary downcall. Only native memory is passed to them. The
 * waiting io_uring_enter, COMMIT, the writes, opening and the checkpoint request are ordinary
 * downcalls, so that the JVM can reach a safepoint while they run. */
final class N {
    private N() {}

    /** All of native memory: bytes are read and written at their absolute address. */
    static final MemorySegment M = MemorySegment.NULL.reinterpret(Long.MAX_VALUE);
    static final ValueLayout.OfLong LONG_U = ValueLayout.JAVA_LONG_UNALIGNED;
    static final ValueLayout.OfInt INT_U = ValueLayout.JAVA_INT_UNALIGNED;
    static final ValueLayout.OfInt INT_BE = ValueLayout.JAVA_INT_UNALIGNED.withOrder(java.nio.ByteOrder.BIG_ENDIAN);

    static byte b(long a) { return M.get(ValueLayout.JAVA_BYTE, a); }
    static int u(long a) { return M.get(ValueLayout.JAVA_BYTE, a) & 0xff; }
    static void put(long a, int v) { M.set(ValueLayout.JAVA_BYTE, a, (byte) v); }
    static long getLong(long a) { return M.get(JAVA_LONG, a); }
    static int getInt(long a) { return M.get(JAVA_INT, a); }
    static void copy(long from, long to, long n) { MemorySegment.copy(M, from, M, to, n); }
    static void copy(byte[] from, int at, long to, int n) {
        MemorySegment.copy(from, at, M, ValueLayout.JAVA_BYTE, to, n);
    }

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Linker.Option[] CRITICAL = { Linker.Option.critical(false) };
    private static final Linker.Option[] PLAIN = {};

    /** A downcall handle that takes the function's address as its first argument. The handles
     * depend on nothing loaded at run time, so a native image can build them in advance. */
    private static MethodHandle fn(boolean critical, FunctionDescriptor d) {
        return LINKER.downcallHandle(d, critical ? CRITICAL : PLAIN);
    }

    private static FunctionDescriptor v(java.lang.foreign.MemoryLayout... args) { return FunctionDescriptor.ofVoid(args); }
    private static FunctionDescriptor i(java.lang.foreign.MemoryLayout... args) { return FunctionDescriptor.of(JAVA_INT, args); }
    private static FunctionDescriptor l(java.lang.foreign.MemoryLayout... args) { return FunctionDescriptor.of(JAVA_LONG, args); }

    private static Error fail(Throwable t) { return new Error(t); }

    // ---- net.c

    private static final MethodHandle RING_ADD = fn(false, i(JAVA_LONG));
    private static final MethodHandle RX_BASE = fn(true, l(JAVA_INT));
    private static final MethodHandle ENTER = fn(false, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle SWEEP = fn(true, v(JAVA_INT));
    private static final MethodHandle REAP = fn(true, l(JAVA_INT, JAVA_LONG));
    private static final MethodHandle ACCEPT = fn(true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle RECV = fn(true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle SEND = fn(true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle SENT = fn(true, i(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle CLOSE = fn(true, v(JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG));
    private static final MethodHandle RX_DROP = fn(true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle RX_KEEP = fn(true, l(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT));
    private static final MethodHandle KEEP = fn(true, v(JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG));
    private static final MethodHandle KEPT_TAKE = fn(true, l(JAVA_INT, JAVA_INT));
    private static final MethodHandle CHUNK_FREE = fn(true, v(JAVA_LONG));
    private static final MethodHandle OUT_RESERVE = fn(true, l(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle OUT_LEN = fn(true, l(JAVA_INT, JAVA_INT));
    private static final MethodHandle OUT_CUT = fn(true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle FREE = fn(true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle DROP_OUTPUT = fn(true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle DROP_KEPT = fn(true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle LISTEN = fn(false, i(JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_INT));
    private static final MethodHandle SLOT_LIMIT = fn(false, l());
    private static final MethodHandle LOCK_CODE = fn(false, l());
    private static final MethodHandle TIME_NS = fn(true, l());

    static int ringAdd(long slots) { try { return (int) RING_ADD.invokeExact(A.RING_ADD, slots); } catch (Throwable t) { throw fail(t); } }
    static long rxBase(int r) { try { return (long) RX_BASE.invokeExact(A.RX_BASE, r); } catch (Throwable t) { throw fail(t); } }
    static void enter(int r, int wait, long ns) { try { ENTER.invokeExact(A.ENTER, r, wait, ns); } catch (Throwable t) { throw fail(t); } }
    static void sweep(int r) { try { SWEEP.invokeExact(A.SWEEP, r); } catch (Throwable t) { throw fail(t); } }
    static long reap(int r, long out) { try { return (long) REAP.invokeExact(A.REAP, r, out); } catch (Throwable t) { throw fail(t); } }
    static void accept(int r, int listener, long ud) { try { ACCEPT.invokeExact(A.ACCEPT, r, listener, ud); } catch (Throwable t) { throw fail(t); } }
    static void recv(int r, int slot, long ud) { try { RECV.invokeExact(A.RECV, r, slot, ud); } catch (Throwable t) { throw fail(t); } }
    static void send(int r, int slot, long ud) { try { SEND.invokeExact(A.SEND, r, slot, ud); } catch (Throwable t) { throw fail(t); } }
    static int sent(int r, int slot, int n, long ud) { try { return (int) SENT.invokeExact(A.SENT, r, slot, n, ud); } catch (Throwable t) { throw fail(t); } }
    static void close(int r, int slot, long udShut, long udClose) { try { CLOSE.invokeExact(A.CLOSE, r, slot, udShut, udClose); } catch (Throwable t) { throw fail(t); } }
    static void rxDrop(int r, int bid) { try { RX_DROP.invokeExact(A.RX_DROP, r, bid); } catch (Throwable t) { throw fail(t); } }
    static long rxKeep(int r, int slot, int bid, int len) { try { return (long) RX_KEEP.invokeExact(A.RX_KEEP, r, slot, bid, len); } catch (Throwable t) { throw fail(t); } }
    static void keep(int r, int slot, long p, long n) { try { KEEP.invokeExact(A.KEEP, r, slot, p, n); } catch (Throwable t) { throw fail(t); } }
    static long keptTake(int r, int slot) { try { return (long) KEPT_TAKE.invokeExact(A.KEPT_TAKE, r, slot); } catch (Throwable t) { throw fail(t); } }
    static void chunkFree(long c) { try { CHUNK_FREE.invokeExact(A.CHUNK_FREE, c); } catch (Throwable t) { throw fail(t); } }
    static long outReserve(int r, int slot, long n) { try { return (long) OUT_RESERVE.invokeExact(A.OUT_RESERVE, r, slot, n); } catch (Throwable t) { throw fail(t); } }
    static long outLen(int r, int slot) { try { return (long) OUT_LEN.invokeExact(A.OUT_LEN, r, slot); } catch (Throwable t) { throw fail(t); } }
    static void outCut(int r, int slot, long len) { try { OUT_CUT.invokeExact(A.OUT_CUT, r, slot, len); } catch (Throwable t) { throw fail(t); } }
    static void free(int r, int slot) { try { FREE.invokeExact(A.FREE, r, slot); } catch (Throwable t) { throw fail(t); } }
    static void dropOutput(int r, int slot) { try { DROP_OUTPUT.invokeExact(A.DROP_OUTPUT, r, slot); } catch (Throwable t) { throw fail(t); } }
    static void dropKept(int r, int slot) { try { DROP_KEPT.invokeExact(A.DROP_KEPT, r, slot); } catch (Throwable t) { throw fail(t); } }
    static int listen(long host, long len, long port, int n) { try { return (int) LISTEN.invokeExact(A.LISTEN, host, len, port, n); } catch (Throwable t) { throw fail(t); } }
    static long slotLimit() { try { return (long) SLOT_LIMIT.invokeExact(A.SLOT_LIMIT); } catch (Throwable t) { throw fail(t); } }
    static long lockCode() { try { return (long) LOCK_CODE.invokeExact(A.LOCK_CODE); } catch (Throwable t) { throw fail(t); } }
    static long timeNs() { try { return (long) TIME_NS.invokeExact(A.TIME_NS); } catch (Throwable t) { throw fail(t); } }

    // ---- db.c

    private static final MethodHandle IO = fn(false, l());
    private static final MethodHandle ERROR = fn(false, l());
    private static final MethodHandle OPEN = fn(false, i(JAVA_LONG, JAVA_LONG));
    private static final MethodHandle BEGIN = fn(true, i());
    private static final MethodHandle COMMIT = fn(false, i());
    private static final MethodHandle SEEK = fn(true, i(JAVA_INT, JAVA_LONG));
    private static final MethodHandle SEEK_GE = fn(true, i(JAVA_INT, JAVA_LONG));
    private static final MethodHandle FIRST = fn(true, i(JAVA_INT));
    private static final MethodHandle NEXT = fn(true, i(JAVA_INT));
    private static final MethodHandle REC = fn(true, l(JAVA_INT));
    private static final MethodHandle ADD_POST = fn(false, i(JAVA_LONG, JAVA_LONG, JAVA_LONG));
    private static final MethodHandle ADD_LIKE = fn(false, i(JAVA_LONG, JAVA_LONG));
    private static final MethodHandle PING = fn(true, i());
    private static final MethodHandle FEED_STEP = fn(false, i());
    private static final MethodHandle FEED_RESET = fn(false, v());
    private static final MethodHandle CK_ASK = fn(false, v());
    private static final MethodHandle CK_POLL = fn(true, i());
    private static final MethodHandle WAL_FRAMES = fn(true, l());
    private static final MethodHandle WAL_RESTARTS = fn(true, v());

    /** The functions' addresses, found when the library is loaded at run time, and where db.c's
     * feed_io_t is, with its fields at fixed offsets (see feed.h). */
    static final class A {
        private static final SymbolLookup LIB = SymbolLookup.libraryLookup(
                Path.of(System.getProperty("feed.lib", "bin/libfeed.so")), Arena.global());

        private static MemorySegment sym(String name) {
            return LIB.find(name).orElseThrow(() -> new IllegalStateException("libfeed.so has no " + name));
        }

        static final MemorySegment RING_ADD = sym("net_ring_add");
        static final MemorySegment RX_BASE = sym("net_rx_base");
        static final MemorySegment ENTER = sym("net_enter");
        static final MemorySegment SWEEP = sym("net_sweep");
        static final MemorySegment REAP = sym("net_reap");
        static final MemorySegment ACCEPT = sym("net_accept");
        static final MemorySegment RECV = sym("net_recv");
        static final MemorySegment SEND = sym("net_send");
        static final MemorySegment SENT = sym("net_sent");
        static final MemorySegment CLOSE = sym("net_close");
        static final MemorySegment RX_DROP = sym("net_rx_drop");
        static final MemorySegment RX_KEEP = sym("net_rx_keep");
        static final MemorySegment KEEP = sym("net_keep");
        static final MemorySegment KEPT_TAKE = sym("net_kept_take");
        static final MemorySegment CHUNK_FREE = sym("net_chunk_free");
        static final MemorySegment OUT_RESERVE = sym("net_out_reserve");
        static final MemorySegment OUT_LEN = sym("net_out_len");
        static final MemorySegment OUT_CUT = sym("net_out_cut");
        static final MemorySegment FREE = sym("net_free");
        static final MemorySegment DROP_OUTPUT = sym("net_drop_output");
        static final MemorySegment DROP_KEPT = sym("net_drop_kept");
        static final MemorySegment LISTEN = sym("net_listen");
        static final MemorySegment SLOT_LIMIT = sym("net_slot_limit");
        static final MemorySegment LOCK_CODE = sym("net_lock_code");
        static final MemorySegment TIME_NS = sym("net_time_ns");
        static final MemorySegment IO = sym("db_io");
        static final MemorySegment ERROR = sym("db_error");
        static final MemorySegment OPEN = sym("db_open");
        static final MemorySegment BEGIN = sym("db_begin");
        static final MemorySegment COMMIT = sym("db_commit");
        static final MemorySegment SEEK = sym("db_seek");
        static final MemorySegment SEEK_GE = sym("db_seek_ge");
        static final MemorySegment FIRST = sym("db_first");
        static final MemorySegment NEXT = sym("db_next");
        static final MemorySegment REC = sym("db_rec");
        static final MemorySegment ADD_POST = sym("db_add_post");
        static final MemorySegment ADD_LIKE = sym("db_add_like");
        static final MemorySegment PING = sym("db_ping");
        static final MemorySegment FEED_STEP = sym("db_feed_step");
        static final MemorySegment FEED_RESET = sym("db_feed_reset");
        static final MemorySegment CK_ASK = sym("db_ck_ask");
        static final MemorySegment CK_POLL = sym("db_ck_poll");
        static final MemorySegment WAL_FRAMES = sym("db_wal_frames");
        static final MemorySegment WAL_RESTARTS = sym("db_wal_restarts");

        static final long IO_LEN = dbIo(), IO_ID = IO_LEN + 8, IO_LIKES = IO_LEN + 16, IO_TEXT = IO_LEN + 24,
                IO_TEXT_LEN = IO_LEN + 48;
    }

    private static long dbIo() { try { return (long) IO.invokeExact(A.IO); } catch (Throwable t) { throw fail(t); } }
    static long dbError() { try { return (long) ERROR.invokeExact(A.ERROR); } catch (Throwable t) { throw fail(t); } }
    static int open(long path, long len) { try { return (int) OPEN.invokeExact(A.OPEN, path, len); } catch (Throwable t) { throw fail(t); } }
    static int begin() { try { return (int) BEGIN.invokeExact(A.BEGIN); } catch (Throwable t) { throw fail(t); } }
    static int commit() { try { return (int) COMMIT.invokeExact(A.COMMIT); } catch (Throwable t) { throw fail(t); } }
    static int seek(int tree, long key) { try { return (int) SEEK.invokeExact(A.SEEK, tree, key); } catch (Throwable t) { throw fail(t); } }
    static int seekGe(int tree, long key) { try { return (int) SEEK_GE.invokeExact(A.SEEK_GE, tree, key); } catch (Throwable t) { throw fail(t); } }
    static int first(int tree) { try { return (int) FIRST.invokeExact(A.FIRST, tree); } catch (Throwable t) { throw fail(t); } }
    static int next(int tree) { try { return (int) NEXT.invokeExact(A.NEXT, tree); } catch (Throwable t) { throw fail(t); } }
    static long rec(int tree) { try { return (long) REC.invokeExact(A.REC, tree); } catch (Throwable t) { throw fail(t); } }
    static int addPost(long uid, long body, long len) { try { return (int) ADD_POST.invokeExact(A.ADD_POST, uid, body, len); } catch (Throwable t) { throw fail(t); } }
    static int addLike(long uid, long postId) { try { return (int) ADD_LIKE.invokeExact(A.ADD_LIKE, uid, postId); } catch (Throwable t) { throw fail(t); } }
    static int ping() { try { return (int) PING.invokeExact(A.PING); } catch (Throwable t) { throw fail(t); } }
    static int feedStep() { try { return (int) FEED_STEP.invokeExact(A.FEED_STEP); } catch (Throwable t) { throw fail(t); } }
    static void feedReset() { try { FEED_RESET.invokeExact(A.FEED_RESET); } catch (Throwable t) { throw fail(t); } }
    static void ckAsk() { try { CK_ASK.invokeExact(A.CK_ASK); } catch (Throwable t) { throw fail(t); } }
    static int ckPoll() { try { return (int) CK_POLL.invokeExact(A.CK_POLL); } catch (Throwable t) { throw fail(t); } }
    static long walFrames() { try { return (long) WAL_FRAMES.invokeExact(A.WAL_FRAMES); } catch (Throwable t) { throw fail(t); } }
    static void walRestarts() { try { WAL_RESTARTS.invokeExact(A.WAL_RESTARTS); } catch (Throwable t) { throw fail(t); } }

    /** The length of a NUL-terminated C string. */
    static int cstrlen(long p) {
        int n = 0;
        while (b(p + n) != 0) n++;
        return n;
    }
}
