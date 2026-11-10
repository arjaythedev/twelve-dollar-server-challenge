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
    private static final SymbolLookup LIB = SymbolLookup.libraryLookup(
            Path.of(System.getProperty("feed.lib", "bin/libfeed.so")), Arena.global());
    private static final Linker.Option[] CRITICAL = { Linker.Option.critical(false) };
    private static final Linker.Option[] PLAIN = {};

    private static MethodHandle fn(String name, boolean critical, FunctionDescriptor d) {
        MemorySegment at = LIB.find(name).orElseThrow(() -> new IllegalStateException("libfeed.so has no " + name));
        return LINKER.downcallHandle(at, d, critical ? CRITICAL : PLAIN);
    }

    private static FunctionDescriptor v(java.lang.foreign.MemoryLayout... args) { return FunctionDescriptor.ofVoid(args); }
    private static FunctionDescriptor i(java.lang.foreign.MemoryLayout... args) { return FunctionDescriptor.of(JAVA_INT, args); }
    private static FunctionDescriptor l(java.lang.foreign.MemoryLayout... args) { return FunctionDescriptor.of(JAVA_LONG, args); }

    private static Error fail(Throwable t) { return new Error(t); }

    // ---- net.c

    private static final MethodHandle RING_ADD = fn("net_ring_add", false, i(JAVA_LONG));
    private static final MethodHandle RX_BASE = fn("net_rx_base", true, l(JAVA_INT));
    private static final MethodHandle ENTER = fn("net_enter", false, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle SWEEP = fn("net_sweep", true, v(JAVA_INT));
    private static final MethodHandle REAP = fn("net_reap", true, l(JAVA_INT, JAVA_LONG));
    private static final MethodHandle ACCEPT = fn("net_accept", true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle RECV = fn("net_recv", true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle SEND = fn("net_send", true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle SENT = fn("net_sent", true, i(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle CLOSE = fn("net_close", true, v(JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG));
    private static final MethodHandle RX_DROP = fn("net_rx_drop", true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle RX_KEEP = fn("net_rx_keep", true, l(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT));
    private static final MethodHandle KEEP = fn("net_keep", true, v(JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG));
    private static final MethodHandle KEPT_TAKE = fn("net_kept_take", true, l(JAVA_INT, JAVA_INT));
    private static final MethodHandle CHUNK_FREE = fn("net_chunk_free", true, v(JAVA_LONG));
    private static final MethodHandle OUT_RESERVE = fn("net_out_reserve", true, l(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle OUT_LEN = fn("net_out_len", true, l(JAVA_INT, JAVA_INT));
    private static final MethodHandle OUT_CUT = fn("net_out_cut", true, v(JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle FREE = fn("net_free", true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle DROP_OUTPUT = fn("net_drop_output", true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle DROP_KEPT = fn("net_drop_kept", true, v(JAVA_INT, JAVA_INT));
    private static final MethodHandle LISTEN = fn("net_listen", false, i(JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_INT));
    private static final MethodHandle SLOT_LIMIT = fn("net_slot_limit", false, l());
    private static final MethodHandle LOCK_CODE = fn("net_lock_code", false, l());
    private static final MethodHandle TIME_NS = fn("net_time_ns", true, l());

    static int ringAdd(long slots) { try { return (int) RING_ADD.invokeExact(slots); } catch (Throwable t) { throw fail(t); } }
    static long rxBase(int r) { try { return (long) RX_BASE.invokeExact(r); } catch (Throwable t) { throw fail(t); } }
    static void enter(int r, int wait, long ns) { try { ENTER.invokeExact(r, wait, ns); } catch (Throwable t) { throw fail(t); } }
    static void sweep(int r) { try { SWEEP.invokeExact(r); } catch (Throwable t) { throw fail(t); } }
    static long reap(int r, long out) { try { return (long) REAP.invokeExact(r, out); } catch (Throwable t) { throw fail(t); } }
    static void accept(int r, int listener, long ud) { try { ACCEPT.invokeExact(r, listener, ud); } catch (Throwable t) { throw fail(t); } }
    static void recv(int r, int slot, long ud) { try { RECV.invokeExact(r, slot, ud); } catch (Throwable t) { throw fail(t); } }
    static void send(int r, int slot, long ud) { try { SEND.invokeExact(r, slot, ud); } catch (Throwable t) { throw fail(t); } }
    static int sent(int r, int slot, int n, long ud) { try { return (int) SENT.invokeExact(r, slot, n, ud); } catch (Throwable t) { throw fail(t); } }
    static void close(int r, int slot, long udShut, long udClose) { try { CLOSE.invokeExact(r, slot, udShut, udClose); } catch (Throwable t) { throw fail(t); } }
    static void rxDrop(int r, int bid) { try { RX_DROP.invokeExact(r, bid); } catch (Throwable t) { throw fail(t); } }
    static long rxKeep(int r, int slot, int bid, int len) { try { return (long) RX_KEEP.invokeExact(r, slot, bid, len); } catch (Throwable t) { throw fail(t); } }
    static void keep(int r, int slot, long p, long n) { try { KEEP.invokeExact(r, slot, p, n); } catch (Throwable t) { throw fail(t); } }
    static long keptTake(int r, int slot) { try { return (long) KEPT_TAKE.invokeExact(r, slot); } catch (Throwable t) { throw fail(t); } }
    static void chunkFree(long c) { try { CHUNK_FREE.invokeExact(c); } catch (Throwable t) { throw fail(t); } }
    static long outReserve(int r, int slot, long n) { try { return (long) OUT_RESERVE.invokeExact(r, slot, n); } catch (Throwable t) { throw fail(t); } }
    static long outLen(int r, int slot) { try { return (long) OUT_LEN.invokeExact(r, slot); } catch (Throwable t) { throw fail(t); } }
    static void outCut(int r, int slot, long len) { try { OUT_CUT.invokeExact(r, slot, len); } catch (Throwable t) { throw fail(t); } }
    static void free(int r, int slot) { try { FREE.invokeExact(r, slot); } catch (Throwable t) { throw fail(t); } }
    static void dropOutput(int r, int slot) { try { DROP_OUTPUT.invokeExact(r, slot); } catch (Throwable t) { throw fail(t); } }
    static void dropKept(int r, int slot) { try { DROP_KEPT.invokeExact(r, slot); } catch (Throwable t) { throw fail(t); } }
    static int listen(long host, long len, long port, int n) { try { return (int) LISTEN.invokeExact(host, len, port, n); } catch (Throwable t) { throw fail(t); } }
    static long slotLimit() { try { return (long) SLOT_LIMIT.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static long lockCode() { try { return (long) LOCK_CODE.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static long timeNs() { try { return (long) TIME_NS.invokeExact(); } catch (Throwable t) { throw fail(t); } }

    // ---- db.c

    private static final MethodHandle IO = fn("db_io", false, l());
    private static final MethodHandle ERROR = fn("db_error", false, l());
    private static final MethodHandle OPEN = fn("db_open", false, i(JAVA_LONG, JAVA_LONG));
    private static final MethodHandle BEGIN = fn("db_begin", true, i());
    private static final MethodHandle COMMIT = fn("db_commit", false, i());
    private static final MethodHandle SEEK = fn("db_seek", true, i(JAVA_INT, JAVA_LONG));
    private static final MethodHandle SEEK_GE = fn("db_seek_ge", true, i(JAVA_INT, JAVA_LONG));
    private static final MethodHandle FIRST = fn("db_first", true, i(JAVA_INT));
    private static final MethodHandle NEXT = fn("db_next", true, i(JAVA_INT));
    private static final MethodHandle REC = fn("db_rec", true, l(JAVA_INT));
    private static final MethodHandle ADD_POST = fn("db_add_post", false, i(JAVA_LONG, JAVA_LONG, JAVA_LONG));
    private static final MethodHandle ADD_LIKE = fn("db_add_like", false, i(JAVA_LONG, JAVA_LONG));
    private static final MethodHandle PING = fn("db_ping", true, i());
    private static final MethodHandle FEED_STEP = fn("db_feed_step", false, i());
    private static final MethodHandle FEED_RESET = fn("db_feed_reset", false, v());
    private static final MethodHandle CK_ASK = fn("db_ck_ask", false, v());
    private static final MethodHandle CK_POLL = fn("db_ck_poll", true, i());
    private static final MethodHandle WAL_FRAMES = fn("db_wal_frames", true, l());
    private static final MethodHandle WAL_RESTARTS = fn("db_wal_restarts", true, v());

    /** db.c's feed_io_t, at fixed offsets (see feed.h). */
    static final long IO_ADDR = dbIo();
    static final long IO_LEN = IO_ADDR, IO_ID = IO_ADDR + 8, IO_LIKES = IO_ADDR + 16, IO_TEXT = IO_ADDR + 24,
            IO_TEXT_LEN = IO_ADDR + 48;

    private static long dbIo() { try { return (long) IO.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static long dbError() { try { return (long) ERROR.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static int open(long path, long len) { try { return (int) OPEN.invokeExact(path, len); } catch (Throwable t) { throw fail(t); } }
    static int begin() { try { return (int) BEGIN.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static int commit() { try { return (int) COMMIT.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static int seek(int tree, long key) { try { return (int) SEEK.invokeExact(tree, key); } catch (Throwable t) { throw fail(t); } }
    static int seekGe(int tree, long key) { try { return (int) SEEK_GE.invokeExact(tree, key); } catch (Throwable t) { throw fail(t); } }
    static int first(int tree) { try { return (int) FIRST.invokeExact(tree); } catch (Throwable t) { throw fail(t); } }
    static int next(int tree) { try { return (int) NEXT.invokeExact(tree); } catch (Throwable t) { throw fail(t); } }
    static long rec(int tree) { try { return (long) REC.invokeExact(tree); } catch (Throwable t) { throw fail(t); } }
    static int addPost(long uid, long body, long len) { try { return (int) ADD_POST.invokeExact(uid, body, len); } catch (Throwable t) { throw fail(t); } }
    static int addLike(long uid, long postId) { try { return (int) ADD_LIKE.invokeExact(uid, postId); } catch (Throwable t) { throw fail(t); } }
    static int ping() { try { return (int) PING.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static int feedStep() { try { return (int) FEED_STEP.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static void feedReset() { try { FEED_RESET.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static void ckAsk() { try { CK_ASK.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static int ckPoll() { try { return (int) CK_POLL.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static long walFrames() { try { return (long) WAL_FRAMES.invokeExact(); } catch (Throwable t) { throw fail(t); } }
    static void walRestarts() { try { WAL_RESTARTS.invokeExact(); } catch (Throwable t) { throw fail(t); } }

    /** The length of a NUL-terminated C string. */
    static int cstrlen(long p) {
        int n = 0;
        while (b(p + n) != 0) n++;
        return n;
    }
}
