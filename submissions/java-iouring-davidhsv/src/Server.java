import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.util.Arrays;

/** The $12 server, in Java.
 *
 * One thread serves every connection through io_uring and every request through SQLite. All of
 * the server's decisions are here and in the classes beside it: the event loop and its
 * connection table, the accept policy and when to add a ring, group commit and the write queue,
 * the checkpoint policy (this file), HTTP/1.1 (Http), routing, the read plans over SQLite's
 * B-tree cursors and SQLite's record format (Api), a validating JSON reader (Json) and writer
 * (Out), HS256 with SHA-256 and HMAC (Jwt).
 *
 * C is what the JVM cannot do without help: net.c is the kernel (io_uring by system calls,
 * sockets, and the bytes the kernel reads and writes while an operation is in flight) and db.c
 * is SQLite (cursor moves, the two INSERTs, the checkpoint thread). Neither decides anything.
 * Java reads requests and SQLite's records where they lie in native memory and writes
 * responses into the connection's native output; the hot path allocates no Java objects. */
public final class Server {
    private Server() {}

    // ---- connections. A connection is (ring, slot): its slot in that ring's table of direct
    // descriptors, numbered ci = ring * slots + slot. Its bytes are in net.c; these arrays hold
    // what the loop knows about it.

    static char[] gen = new char[0];  // bumped when the slot is reused, so completions for an older connection drop
    static char[] flags = new char[0];
    static int[] mark = new int[0];   // pending output's length when this pass first touched the connection
    static int[] need = new int[0];   // kept input is a request still arriving: its full length once known, else 0

    static final int F_OPEN = 1;
    static final int F_CLOSE = 2;      // close once the output is out
    static final int F_QUEUED = 4;     // a write of this connection waits in the write queue
    static final int F_EOF = 8;        // the client finished sending while its write was queued
    static final int F_CLOSING = 16;   // shutdown and close are on their way
    static final int F_SENDING = 32;
    static final int F_TOUCHED = 64;   // in this pass's touched list
    static final int F_ANSWERED = 128; // answered something in this pass
    static final int F_KEPT = 256;     // net.c keeps input of this connection

    static final int LISTENERS = 8;
    static final int MAX_RINGS = 16;
    static final int RX_SIZE = 1024;

    // user_data: operation (8 bits) | ring (8) | generation (16) | slot or listener (32)
    static final long OP_ACCEPT = 1, OP_RECV = 2, OP_SEND = 3, OP_SHUT = 4, OP_CLOSE = 5;
    static final int CQE_BUFFER = 1;  // IORING_CQE_F_BUFFER
    static final int CQE_MORE = 2;    // IORING_CQE_F_MORE
    static final int ENOBUFS = 105, ENFILE = 23;

    static final long[] ringOpen = new long[MAX_RINGS];
    static final boolean[] ringFull = new boolean[MAX_RINGS];
    static final int[] accepts = new int[MAX_RINGS * LISTENERS];
    static final long[] rxBase = new long[MAX_RINGS];
    static int nrings, slots, depth, margin, accRing;
    static long ringRetry;  // adding a ring failed: not again before this (µs)

    private static final int CQ_ENTRIES = 65536;
    private static final long DISK = Arena.global().allocate(9 * 8L, 8).address();  // db_stats_take's counters
    private static final long CQES = Arena.global().allocate(CQ_ENTRIES * 16L, 64).address();
    private static int[] touched = new int[4096];  // connections with output (or a close) for the end of this pass
    private static int ntouched;
    private static int[] rearm = new int[4096];    // connections whose multishot receive ended
    private static int nrearm;

    // A pass waits on ring 0 for this many completions or this long, whichever comes first, so a
    // busy pass shares one transaction across many requests.
    static int waitMin = 256;
    static long waitNs = 1000000;

    // ---- the write queue. A write is validated when it arrives and then waits here, with
    // everything it needs, until the next write pass: all queued writes run in one transaction
    // and share its COMMIT (group commit, rule 6), and the pages several of them touch reach the
    // log once. Each response still waits for the COMMIT of its own write.

    private static int[] jCi = new int[256];
    private static char[] jGen = new char[256];
    private static boolean[] jLike = new boolean[256];
    private static boolean[] jClose = new boolean[256];  // the request asked to close the connection after its response
    private static long[] jUid = new long[256];
    private static long[] jPost = new long[256];
    private static long[] jWho = new long[256];   // the token's username (a new post's author): offset in jobBytes
    private static int[] jWhoLen = new int[256];
    private static long[] jText = new long[256];  // a new post's trimmed body: offset in jobBytes
    private static int[] jTextLen = new int[256];
    private static final Buf jobBytes = new Buf(1 << 20);
    static int njobs;
    static long writeWindow = 5000;  // µs a queued write waits for company; FEED_WRITE_US overrides it
    static long writeAt;             // µs; when the queue runs
    static boolean writesNow;        // a write pass is running: writes execute as they are parsed
    static long nowUs;               // the pass's clock

    // Checkpoints: db.c's thread copies the log back when asked; the policy is here. Past ckStart
    // log frames, ask for a copy. When it is done, hold the write queue (reads go on) and ask
    // again: that copy takes the frames written meanwhile, and once the whole log is copied the
    // next write starts the log over. Past ckHigh the queue is held too, so the log stays bounded
    // when the loop leaves the thread no idle time. A hold lasts at most gateUs.
    static long ckStart = 16384;  // FEED_CK_FRAMES overrides it
    static long ckHigh = 40960;   // FEED_CK_HIGH overrides it
    static long gateUs = 200000;  // FEED_GATE_US overrides it
    static int ckPhase;           // 0 idle, 1 copying, 2 copying the tail with writes held
    static boolean gate;
    static long gateSince;

    static boolean statsOn;
    static long stAt, stPasses, stReqs, stCqes, stWrites, stWpasses, stNobuf, stRearms, stGates, stRestarts, stMaxpass,
            stGateUs, stGateMax, stOldest,
            stServeUs, stWriteUs, stCommitUs;

    // ---- the loop

    public static void main(String[] args) {
        if (!startup()) System.exit(1);
        for (;;) pass();
    }

    private static void pass() {
        // Every other ring submits and collects without waiting; ring 0 goes last and waits.
        for (int r = nrings - 1; r > 0; r--) N.sweep(r);
        N.enter(0, waitMin, waitNs);
        nowUs = System.nanoTime() / 1000;
        for (int k = 0; k < nrings; k++) drain(k);
        long t1 = statsOn ? System.nanoTime() / 1000 : 0;
        if (njobs > 0 && !gate && nowUs >= writeAt) runWrites();
        long t2 = statsOn ? System.nanoTime() / 1000 : 0;
        // No response of this pass reaches the kernel before its COMMIT returns.
        boolean committed = N.commit() != 0;
        if (statsOn) {
            long t3 = System.nanoTime() / 1000;
            stServeUs = Math.max(stServeUs, t1 - nowUs);
            stWriteUs = Math.max(stWriteUs, t2 - t1);
            stCommitUs = Math.max(stCommitUs, t3 - t2);
        }
        flush(committed);
        rearmReceives();
        acceptMore();
        checkpoints();
        if (statsOn) stats();
    }

    static void log(String s) { System.err.println(s); }

    private static final MethodHandle GETENV = Linker.nativeLinker().downcallHandle(
            Linker.nativeLinker().defaultLookup().find("getenv").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));

    /** An environment variable's bytes as the process got them (no charset decoding), or null. */
    static byte[] env(String name) {
        try (Arena a = Arena.ofConfined()) {
            long v = (long) GETENV.invokeExact(a.allocateFrom(name).address());
            if (v == 0) return null;
            byte[] out = new byte[N.cstrlen(v)];
            for (int i = 0; i < out.length; i++) out[i] = N.b(v + i);
            return out;
        } catch (Throwable t) {
            throw new Error(t);
        }
    }

    /** Goose's parse_int: an optionally signed decimal integer, the whole string. */
    private static Long parseInt(byte[] s) {
        if (s == null) return null;
        try {
            return Long.parseLong(new String(s, java.nio.charset.StandardCharsets.ISO_8859_1));
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static long envInt(String name, long lo, long hi, long dflt) {
        Long n = parseInt(env(name));
        return n != null && n >= lo && n <= hi ? n : dflt;
    }

    private static long nativeBytes(byte[] s) {
        long at = Arena.global().allocate(s.length + 1L, 8).address();
        N.copy(s, 0, at, s.length);
        N.put(at + s.length, 0);
        return at;
    }

    private static String str(byte[] s) { return new String(s, java.nio.charset.StandardCharsets.UTF_8); }

    static boolean startup() {
        byte[] path = env("SQLITE_PATH"), host = env("HOST"), portS = env("PORT"), secret = env("JWT_SECRET");
        if (path == null) path = Out.ascii("feed.db");
        if (host == null) host = Out.ascii("127.0.0.1");
        if (portS == null) portS = Out.ascii("3000");
        if (secret == null) secret = new byte[0];
        Long port = parseInt(portS);
        if (port == null || port <= 0 || port > 65535) { log("PORT must be 1-65535"); return false; }
        // Test knobs; none is set on the benchmark box.
        writeWindow = envInt("FEED_WRITE_US", 0, 1000000, writeWindow);
        ckStart = envInt("FEED_CK_FRAMES", 64, 1000000, ckStart);
        ckHigh = envInt("FEED_CK_HIGH", 64, 10000000, ckStart + 24576);
        gateUs = envInt("FEED_GATE_US", 0, 10000000, gateUs);
        waitNs = envInt("FEED_WAIT_US", 50, 100000, waitNs / 1000) * 1000;
        statsOn = env("FEED_STATS") != null;

        if (N.open(nativeBytes(path), path.length) != 0) {
            long e = N.dbError();
            byte[] m = new byte[N.cstrlen(e)];
            for (int i = 0; i < m.length; i++) m[i] = N.b(e + i);
            log("database: " + str(m));
            return false;
        }
        Jwt.key(secret);
        Api.startedUs = System.nanoTime() / 1000;

        // Rings as large as the descriptor limit allows. A ring is left alone while fewer slots
        // are free than the accepts in flight plus some room: an accept that finds no free slot
        // has already taken the connection off the listen queue.
        slots = (int) envInt("FEED_SLOTS", 64, 1048576, N.slotLimit());
        depth = Math.clamp(slots / 32, LISTENERS, 512) / LISTENERS * LISTENERS;
        margin = depth + 64;
        int lrc = N.listen(nativeBytes(host), host.length, port, LISTENERS);
        if (lrc != 0) {
            log("listen " + str(host) + ":" + port + " failed (errno " + -lrc + ")");
            return false;
        }
        if (!addRing()) { log("io_uring setup failed (needs Linux 6.1+ and io_uring allowed)"); return false; }
        long locked = N.lockCode();
        log("listening on " + str(host) + ":" + port + "; io_uring, " + slots + " direct descriptors per ring, "
                + locked / 1024 + " KB of code locked");
        return true;
    }

    static boolean addRing() {
        if (nrings >= MAX_RINGS) return false;
        if (N.ringAdd(slots) < 0) return false;
        int total = (nrings + 1) * slots;
        gen = Arrays.copyOf(gen, total);
        flags = Arrays.copyOf(flags, total);
        mark = Arrays.copyOf(mark, total);
        need = Arrays.copyOf(need, total);
        rxBase[nrings] = N.rxBase(nrings);
        ringOpen[nrings] = 0;
        ringFull[nrings] = false;
        for (int li = 0; li < LISTENERS; li++) accepts[nrings * LISTENERS + li] = 0;
        nrings++;
        if (nrings > 1) log("ring " + (nrings - 1) + " added");
        return true;
    }

    static long ud(long op, int ci, int g) {
        return (op << 56) | ((long) (ci / slots) << 48) | ((long) g << 32) | (ci % slots);
    }

    static int ringOf(int ci) { return ci / slots; }
    static int slotOf(int ci) { return ci % slots; }

    private static void drain(int r) {
        long n = N.reap(r, CQES);
        stCqes += n;
        int base = r * slots;
        for (long k = 0; k < n; k++) {
            long p = CQES + k * 16;
            long ud = N.getLong(p);
            int res = N.getInt(p + 8);
            int fl = N.getInt(p + 12);
            long op = ud >>> 56;
            int g = (int) ((ud >>> 32) & 0xffff);
            int low = (int) (ud & 0xffffffffL);
            if (op == OP_RECV) onRecv(base + low, g, res, fl);
            else if (op == OP_SEND) onSend(base + low, g, res);
            else if (op == OP_ACCEPT) onAccept(r, low, res);
            else if (op == OP_CLOSE) onClosed(base + low, g);
        }
    }

    private static void onAccept(int r, int listener, int res) {
        accepts[r * LISTENERS + listener]--;
        if (res >= 0) {
            int ci = r * slots + res;
            gen[ci]++;
            flags[ci] = F_OPEN;
            mark[ci] = 0;
            need[ci] = 0;
            N.recv(r, res, ud(OP_RECV, ci, gen[ci]));
            ringOpen[r]++;
            if (ringOpen[r] + margin >= slots) ringFull[r] = true;
        } else if (res == -ENFILE) {
            ringFull[r] = true;
        }
    }

    private static void onRecv(int ci, int g, int res, int fl) {
        int r = ringOf(ci);
        boolean hasBuf = (fl & CQE_BUFFER) != 0;
        int bid = fl >>> 16;
        int f = flags[ci];
        if (gen[ci] != g || (f & F_OPEN) == 0 || (f & F_CLOSING) != 0) {
            if (hasBuf) N.rxDrop(r, bid);
            return;
        }
        if (res > 0 && hasBuf) {
            if ((f & F_CLOSE) != 0) {
                N.rxDrop(r, bid);  // its answers are done; what follows is dropped
            } else if ((f & F_QUEUED) != 0) {
                N.rxKeep(r, slotOf(ci), bid, res);  // behind a queued write: wait with it
                flags[ci] |= F_KEPT;
            } else if ((f & F_KEPT) != 0) {
                // More of a request that is still arriving; looked at again once it can be whole.
                long have = N.rxKeep(r, slotOf(ci), bid, res);
                if (have >= need[ci]) serveKept(ci);
            } else {
                // Served where the kernel put it; the buffer goes back once nothing reads it.
                serve(ci, rxBase[r] + (long) bid * RX_SIZE, res);
                N.rxDrop(r, bid);
            }
            if ((fl & CQE_MORE) == 0) pushRearm(ci);
        } else if (res > 0 || res == -ENOBUFS) {
            // The receive ended for lack of a buffer; armed again at the end of the pass, when the
            // buffers this pass used are back.
            stNobuf++;
            pushRearm(ci);
        } else {
            // End of stream or an error: answer what was asked, then close.
            if (hasBuf) N.rxDrop(r, bid);
            if ((flags[ci] & F_QUEUED) != 0) flags[ci] |= F_EOF;
            else closeAfter(ci);
        }
    }

    private static void onSend(int ci, int g, int res) {
        if (gen[ci] != g || (flags[ci] & F_SENDING) == 0) return;
        if (res < 0) {
            flags[ci] = (char) (flags[ci] & ~F_SENDING);
            N.dropOutput(ringOf(ci), slotOf(ci));
            closeAfter(ci);
            return;
        }
        if (N.sent(ringOf(ci), slotOf(ci), res, ud(OP_SEND, ci, g)) != 0) return;
        flags[ci] = (char) (flags[ci] & ~F_SENDING);
        // Output that queued up behind this send goes out with the pass's other responses.
        if (N.outLen(ringOf(ci), slotOf(ci)) > 0 || (flags[ci] & F_CLOSE) != 0) touch(ci);
    }

    private static void onClosed(int ci, int g) {
        int r = ringOf(ci);
        if (gen[ci] == g && (flags[ci] & F_OPEN) != 0) {
            N.free(r, slotOf(ci));
            flags[ci] = 0;
            ringOpen[r]--;
        }
        if (ringFull[r] && ringOpen[r] + 2L * margin < slots) ringFull[r] = false;
    }

    private static void pushRearm(int ci) {
        if (nrearm == rearm.length) rearm = Arrays.copyOf(rearm, nrearm * 2);
        rearm[nrearm++] = ci;
    }

    /** The connection has output (or a close) for the end of this pass. */
    static void touch(int ci) {
        if ((flags[ci] & F_TOUCHED) != 0) return;
        flags[ci] |= F_TOUCHED;
        mark[ci] = (int) N.outLen(ringOf(ci), slotOf(ci));
        if (ntouched == touched.length) touched = Arrays.copyOf(touched, ntouched * 2);
        touched[ntouched++] = ci;
    }

    static void closeAfter(int ci) {
        flags[ci] |= F_CLOSE;
        if ((flags[ci] & F_KEPT) != 0) {
            N.dropKept(ringOf(ci), slotOf(ci));
            flags[ci] = (char) (flags[ci] & ~F_KEPT);
        }
        touch(ci);
    }

    /** After the pass's COMMIT, every touched connection's output goes to the kernel. If the
     * commit failed, no answer of the pass may stand: each becomes a 500 and its connection
     * closes. */
    private static void flush(boolean committed) {
        for (int k = 0; k < ntouched; k++) {
            int ci = touched[k];
            int r = ringOf(ci), s = slotOf(ci);
            if (!committed && (flags[ci] & F_ANSWERED) != 0) {
                N.outCut(r, s, mark[ci]);
                byte[] e = Api.R_500;
                N.copy(e, 0, N.outReserve(r, s, e.length), e.length);
                flags[ci] |= F_CLOSE;
            }
            flags[ci] = (char) (flags[ci] & ~(F_TOUCHED | F_ANSWERED));
            int f = flags[ci];
            if ((f & F_OPEN) == 0 || (f & (F_CLOSING | F_SENDING)) != 0) continue;
            if (N.outLen(r, s) > 0) {
                flags[ci] |= F_SENDING;
                N.send(r, s, ud(OP_SEND, ci, gen[ci]));
            } else if ((f & F_CLOSE) != 0) {
                flags[ci] |= F_CLOSING;
                N.close(r, s, ud(OP_SHUT, ci, gen[ci]), ud(OP_CLOSE, ci, gen[ci]));
            }
        }
        ntouched = 0;
    }

    private static void rearmReceives() {
        stRearms += nrearm;
        for (int k = 0; k < nrearm; k++) {
            int ci = rearm[k];
            int f = flags[ci];
            if ((f & F_OPEN) != 0 && (f & F_CLOSING) == 0) N.recv(ringOf(ci), slotOf(ci), ud(OP_RECV, ci, gen[ci]));
        }
        nrearm = 0;
    }

    /** Keeps single-shot accepts in flight on the accepting ring, spread over the listeners. When
     * that ring is nearly full, accepting moves to a ring with room, adding one if none has. */
    private static void acceptMore() {
        if (ringFull[accRing]) {
            int next = -1;
            for (int r = 0; r < nrings; r++) if (next < 0 && !ringFull[r]) next = r;
            if (next < 0) {
                if (nowUs < ringRetry) return;
                if (!addRing()) {
                    ringRetry = nowUs + 1000000;
                    log("cannot add a ring; trying again in 1 s");
                    return;
                }
                next = nrings - 1;
            }
            accRing = next;
        }
        int r = accRing;
        int each = depth / LISTENERS;
        for (int li = 0; li < LISTENERS; li++) {
            while (accepts[r * LISTENERS + li] < each) {
                N.accept(r, li, (OP_ACCEPT << 56) | ((long) r << 48) | li);
                accepts[r * LISTENERS + li]++;
            }
        }
    }

    private static void hold() {
        if (gate) return;
        gate = true;
        gateSince = nowUs;
        stGates++;
    }

    private static void release() {
        if (!gate) return;
        gate = false;
        long held = nowUs - gateSince;
        stGateUs += held;
        if (held > stGateMax) stGateMax = held;
    }

    private static void checkpoints() {
        int st = N.ckPoll();  // 0 idle, 1 copying, 2 done with the whole log copied, 3 done, frames left
        if (ckPhase == 0) {
            if (N.walFrames() >= ckStart) { N.ckAsk(); ckPhase = 1; }
        } else if (ckPhase == 1) {
            if (st >= 2) {
                // Copied up to where the log was when it started. Now nothing may be written until
                // the rest is copied, so that the log can start over.
                hold();
                N.ckAsk();
                ckPhase = 2;
            } else if (!gate && N.walFrames() >= ckHigh) {
                hold();
            } else if (gate && nowUs - gateSince > gateUs) {
                release();
            }
        } else if (st == 2) {
            N.walRestarts();
            release();
            ckPhase = 0;
            stRestarts++;
        } else if (st == 3) {
            release();
            ckPhase = 0;
        } else if (gate && nowUs - gateSince > gateUs) {
            release();  // the copy is slow: writes go on, and a later round tries again
        }
    }

    private static void stats() {
        stPasses++;
        long took = System.nanoTime() / 1000 - nowUs;
        if (took > stMaxpass) stMaxpass = took;
        if (stAt == 0) stAt = nowUs;
        if (nowUs - stAt < 1000000) return;
        long open = 0;
        for (int r = 0; r < nrings; r++) open += ringOpen[r];
        if (gate) { long held = nowUs - gateSince; stGateUs += held; stGateMax = Math.max(stGateMax, held); gateSince = nowUs; }
        N.statsTake(DISK);
        long[] d = new long[9];
        for (int i = 0; i < 9; i++) d[i] = N.getLong(DISK + 8L * i);
        log("stats: " + stReqs + " req/s, " + stPasses + " passes, " + stCqes + " cqes, " + stWrites + " writes in "
                + stWpasses + " write passes, max pass " + stMaxpass + " us (serve " + stServeUs + ", writes "
                + stWriteUs + ", commit " + stCommitUs + "), oldest write " + stOldest / 1000 + " ms, nobuf " + stNobuf
                + ", rearms " + stRearms + ", open " + open + ", rings " + nrings + ", wal " + N.walFrames()
                + ", gates " + stGates + " held " + stGateUs / 1000 + " ms max " + stGateMax / 1000 + " ms, restarts "
                + stRestarts + ", queued " + njobs + "; loop log writes " + d[0] + " max " + d[2] + " us, syncs " + d[3]
                + " max " + d[5] + " us; checkpoints " + d[6] + " " + d[7] / 1000 + " ms max " + d[8] / 1000 + " ms");
        stAt = nowUs;
        stReqs = stPasses = stCqes = stWrites = stWpasses = stNobuf = stRearms = stMaxpass = 0;
        stServeUs = stWriteUs = stCommitUs = stGates = stRestarts = stGateUs = stGateMax = stOldest = 0;
    }

    // ---- serving a connection's bytes

    /** Parses and answers every complete request in the n bytes at b. A write that has to wait
     * for the next write pass stops it: what follows that request stays with the connection until
     * its write is answered, so responses keep their order. */
    static void serve(int ci, long b, long n) {
        long off = 0;
        Http.need = 0;
        while (off < n) {
            long rest = b + off;
            int st = Http.parse(rest, n - off);
            if (st == 0) break;
            if (st < 0) {
                Api.fixed(ci, Api.R_BAD_REQUEST);
                closeAfter(ci);
                return;
            }
            stReqs++;
            boolean close = Http.close;
            long used = Http.used;
            boolean queued = Api.handle(ci);
            off += used;
            if (queued) {
                if (off < n) keep(ci, b + off, n - off, 0);
                return;
            }
            if (close) { closeAfter(ci); return; }
        }
        if (off < n) keep(ci, b + off, n - off, Http.need);
    }

    private static void keep(int ci, long rest, long n, long needed) {
        N.keep(ringOf(ci), slotOf(ci), rest, n);
        flags[ci] |= F_KEPT;
        need[ci] = (int) needed;
    }

    /** Serves the input net.c kept for the connection, in its chunk; the chunk is freed after. */
    static void serveKept(int ci) {
        flags[ci] = (char) (flags[ci] & ~F_KEPT);
        long c = N.keptTake(ringOf(ci), slotOf(ci));
        if (c == 0) return;
        serve(ci, c + 8, N.getInt(c) & 0xffffffffL);
        N.chunkFree(c);
    }

    /** Runs the queued writes, in the order they came, in this pass's transaction. */
    private static void runWrites() {
        writesNow = true;
        stWpasses++;
        stOldest = Math.max(stOldest, nowUs - (writeAt - writeWindow));
        long jb = jobBytes.base;
        for (int j = 0; j < njobs; j++) {
            int ci = jCi[j];
            if (gen[ci] != jGen[j] || (flags[ci] & F_OPEN) == 0 || (flags[ci] & (F_CLOSING | F_QUEUED)) != F_QUEUED) continue;
            flags[ci] = (char) (flags[ci] & ~F_QUEUED);
            stWrites++;
            if (jLike[j]) Api.doLike(ci, jUid[j], jPost[j]);
            else Api.doPost(ci, jUid[j], jb + jWho[j], jWhoLen[j], jb + jText[j], jTextLen[j]);
            if (jClose[j]) { closeAfter(ci); continue; }
            // What arrived behind the write is served now; a write among it runs at once.
            if ((flags[ci] & F_KEPT) != 0) serveKept(ci);
            if ((flags[ci] & F_EOF) != 0) closeAfter(ci);
        }
        njobs = 0;
        jobBytes.clear();
        writesNow = false;
    }

    private static int newJob(int ci, boolean like, long uid) {
        if (njobs == jCi.length) {
            int n = njobs * 2;
            jCi = Arrays.copyOf(jCi, n);
            jGen = Arrays.copyOf(jGen, n);
            jLike = Arrays.copyOf(jLike, n);
            jClose = Arrays.copyOf(jClose, n);
            jUid = Arrays.copyOf(jUid, n);
            jPost = Arrays.copyOf(jPost, n);
            jWho = Arrays.copyOf(jWho, n);
            jWhoLen = Arrays.copyOf(jWhoLen, n);
            jText = Arrays.copyOf(jText, n);
            jTextLen = Arrays.copyOf(jTextLen, n);
        }
        int j = njobs;
        jCi[j] = ci;
        jGen[j] = gen[ci];
        jLike[j] = like;
        jClose[j] = Http.close;
        jUid[j] = uid;
        flags[ci] |= F_QUEUED;
        if (njobs == 0) writeAt = nowUs + writeWindow;
        njobs++;
        return j;
    }

    static void queueLike(int ci, long uid, long postId) {
        int j = newJob(ci, true, uid);
        jPost[j] = postId;
        jWhoLen[j] = 0;
        jTextLen[j] = 0;
    }

    static void queuePost(int ci, long uid, long txt, long txtLen) {
        int j = newJob(ci, false, uid);
        jPost[j] = 0;
        jWho[j] = jobBytes.len;
        jWhoLen[j] = (int) Jwt.who.len;
        jobBytes.put(Jwt.who.base, Jwt.who.len);
        jText[j] = jobBytes.len;
        jTextLen[j] = (int) txtLen;
        jobBytes.put(txt, txtLen);
    }
}
