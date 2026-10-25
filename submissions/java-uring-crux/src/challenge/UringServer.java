package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.net.InetAddress;
import java.util.ArrayDeque;
import java.util.ArrayList;

/**
 * Linux io_uring server. Each ring has its own listen socket ({@code SO_REUSEPORT}) and a fixed
 * file table, so accepted sockets do not consume process file descriptors. A multishot receive
 * stays armed and borrows a provided buffer only while bytes are arriving. Responses are built
 * in an off-heap buffer and sent from that address.
 */
final class UringServer {
    private static final int SYS_IO_URING_SETUP = 425;
    private static final int SYS_IO_URING_ENTER = 426;
    private static final int SYS_IO_URING_REGISTER = 427;

    private static final long OFF_SQ_RING = 0L;
    private static final long OFF_CQ_RING = 0x8000000L;
    private static final long OFF_SQES = 0x10000000L;

    private static final int SETUP_CQSIZE = 1 << 3;
    private static final int SETUP_CLAMP = 1 << 4;
    private static final int SETUP_COOP_TASKRUN = 1 << 8;
    private static final int SETUP_SINGLE_ISSUER = 1 << 12;
    /** Kernel 6.1+. Requires COOP_TASKRUN. Defers task work until the thread asks for completions. */
    private static final int SETUP_DEFER_TASKRUN = 1 << 13;

    private static final int ENTER_GETEVENTS = 1;
    private static final int ENTER_EXT_ARG = 1 << 3;
    private static final int REGISTER_FILES = 2;
    private static final int REGISTER_PBUF_RING = 22;

    private static final byte OP_ACCEPT = 13;
    private static final byte OP_CLOSE = 19;
    private static final byte OP_POLL = 6;
    private static final byte OP_SEND = 26;
    private static final byte OP_RECV = 27;
    private static final byte IOSQE_FIXED_FILE = 1;
    private static final byte IOSQE_BUFFER_SELECT = 1 << 5;
    /** sqe.ioprio for {@code IORING_OP_RECV}. Multishot requires buffer selection. */
    private static final short RECV_MULTISHOT = 1 << 1;
    private static final int CQE_F_BUFFER = 1;
    private static final int CQE_F_MORE = 1 << 1;
    private static final int RECV_BUFS = 1024;
    private static final int RECV_BUF = 2048;
    private static final int SEND_CAP = 65536;
    private static final int SEND_SLOTS = 256;
    private static final short BGID = 1;

    private static final int POLLIN = 0x001, POLLOUT = 0x004, POLLERR = 0x008, POLLHUP = 0x010;
    private static final int SOCK_NONBLOCK = 0x800;
    private static final int EAGAIN = 11, EINTR = 4, ENOBUFS = 105, ECANCELED = 125;
    private static final int MSG_NOSIGNAL = 0x4000;

    private static final int ACCEPT = 1, POLL = 2, RECV = 3, SEND = 4, CLOSE = 5;
    private static final int BUF = 8192, BUFS = 512, SQ_WANT = 4096, CQ_WANT = 16384;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Linker.Option ERRNO = Linker.Option.captureCallState("errno");
    private static final MemoryLayout STATE_LAYOUT = Linker.Option.captureStateLayout();
    private static final VarHandle ERRNO_VH = STATE_LAYOUT.varHandle(MemoryLayout.PathElement.groupElement("errno"));
    private static final VarHandle INT = ValueLayout.JAVA_INT.varHandle();
    private static final VarHandle SHORT = ValueLayout.JAVA_SHORT.varHandle();
    private static final MemorySegment STATE = Arena.global().allocate(STATE_LAYOUT);
    private static final MemorySegment SCRATCH = Arena.global().allocate(128, 8);

    private static final MethodHandle SYSCALL3 = syscall(FunctionDescriptor.of(
            JAVA_LONG, JAVA_LONG, JAVA_LONG, ADDRESS));
    private static final MethodHandle SYSCALL5 = syscall(FunctionDescriptor.of(
            JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, ADDRESS, JAVA_LONG));
    private static final MethodHandle SYSCALL7 = syscall(FunctionDescriptor.of(
            JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, ADDRESS, JAVA_LONG));
    private static final MethodHandle MMAP = down("mmap", FunctionDescriptor.of(
            ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle SOCKET = down("socket", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT));
    private static final MethodHandle SETSOCKOPT = down("setsockopt",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT));
    private static final MethodHandle BIND = down("bind", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT));
    private static final MethodHandle LISTEN = down("listen", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT));
    private static final MethodHandle CLOSEFD = down("close", FunctionDescriptor.of(JAVA_INT, JAVA_INT));

    private final App app;
    private final Db db;
    private final Bufs bufs = new Bufs();
    private final Sends sends = new Sends();
    private final ArrayList<Conn> batch = new ArrayList<>();
    private final ArrayList<Conn> readWait = new ArrayList<>();
    private final ArrayList<Conn> writeWait = new ArrayList<>();
    private final ArrayList<Conn> pollWait = new ArrayList<>();
    private final ArrayList<Conn> closeWait = new ArrayList<>();
    private final ArrayDeque<App.Req> reqPool = new ArrayDeque<>();
    private final Ring[] rings;
    private boolean extArg = true;
    private int waitCursor;

    private UringServer(App app, Db db, Ring[] rings) {
        this.app = app;
        this.db = db;
        this.rings = rings;
    }

    static void run(App app, Db db, String host, int port) throws IOException {
        int slots = slotsPerRing();
        // Eight rings of 65535 fixed files. One ring was slower on this closed loop.
        int ringCount = slots >= 30_000 ? 8 : 1;
        Ring[] rings = new Ring[ringCount];
        try {
            for (int i = 0; i < ringCount; i++) rings[i] = Ring.open(host, port, slots);
        } catch (IOException | RuntimeException e) {
            for (Ring ring : rings) if (ring != null) ring.closeListen();
            throw e instanceof IOException io ? io : new IOException(e.getMessage(), e);
        }
        new UringServer(app, db, rings).loop();
    }

    private void loop() {
        for (Ring ring : rings) armAccepts(ring);
        submitAll();
        while (true) {
            int events = 0;
            for (Ring ring : rings) events += drain(ring);
            runBatch();
            for (Ring ring : rings) {
                ring.publish();
                armAccepts(ring);
            }
            pumpReads();
            pumpWrites();
            pumpPolls();
            pumpCloses();
            submitAll();
            if (events == 0) waitOne();
            db.checkpointIfDue();
        }
    }

    private void runBatch() {
        if (batch.isEmpty()) return;
        boolean wrote = false;
        for (Conn conn : batch) {
            for (App.Req req = conn.pendingHead; req != null; req = req.next) {
                if (req.mutates()) { wrote = true; break; }
            }
            if (wrote) break;
        }
        // Leave the read transaction open across batches so the pager stays set up.
        // Commit only when this batch wrote, and only before those responses are published.
        boolean ok = true;
        try {
            db.begin();
            for (Conn conn : batch) {
                for (App.Req req = conn.pendingHead; req != null; ) {
                    App.Req next = req.next;
                    if (!queueResponse(conn, req)) {
                        byte[] bytes = req.early != null ? req.early : app.execute(req);
                        conn.stageHeap(bytes, req.close);
                    }
                    req.next = null;
                    recycleReq(req);
                    req = next;
                }
            }
        } catch (RuntimeException e) {
            System.err.println("batch failed: " + e.getMessage());
            ok = false;
            db.rollbackQuietly();
        }
        if (ok && wrote && !db.commit(true)) ok = false;
        if (ok) {
            for (Conn conn : batch) publishStaged(conn);
        } else {
            byte[] err = app.internalError(true);
            for (Conn conn : batch) discardStaged(conn, err);
        }
        for (Conn conn : batch) {
            conn.pendingHead = null;
            conn.pendingTail = null;
            conn.pendingCount = 0;
            conn.inBatch = false;
            if (conn.op == 0 && conn.hasSend()) startSend(conn);
        }
        batch.clear();
    }

    private int drain(Ring ring) {
        int head = ring.cqHead;
        int tail = (int) INT.getAcquire(ring.cq, ring.cqTail);
        int n = 0;
        while (head != tail) {
            long off = ring.cqes + (long) (head & ring.cqMask) * 16;
            long user = ring.cq.get(JAVA_LONG, off);
            int res = ring.cq.get(JAVA_INT, off + 8);
            int flags = ring.cq.get(JAVA_INT, off + 12);
            head++;
            n++;
            onCqe(ring, user, res, flags);
        }
        ring.cqHead = head;
        INT.setRelease(ring.cq, ring.cqHeadOff, head);
        return n;
    }

    private void onCqe(Ring ring, long user, int res, int flags) {
        int op = (int) (user & 0xff);
        int slot = (int) ((user >>> 8) & 0xffffff);
        int gen = (int) (user >>> 32);
        int bid = op == RECV && (flags & CQE_F_BUFFER) != 0 ? flags >>> 16 : -1;
        Conn conn = op == ACCEPT ? null : ring.conns[slot];
        boolean live = conn != null && conn.gen == gen;
        if (live && op == RECV && res > 0 && bid >= 0 && bid < RECV_BUFS) {
            int n = Math.min(res, RECV_BUF);
            conn.append(ring.recvSlice(bid, n), n);
        } else if (live && op == RECV && res > 0) {
            conn.needClose = true;
        }
        if (bid >= 0) ring.recycle(bid);
        if (op == ACCEPT) {
            ring.acceptsInFlight--;
            if (res == 0 && gen == ring.gen[slot]) {
                Conn accepted = new Conn(ring, slot, gen);
                ring.conns[slot] = accepted;
                armRecv(accepted);
            } else {
                if (res > 0) quietClose(res);
                ring.free[ring.freeTop++] = slot;
            }
            return;
        }
        if (!live) return;
        if (op == RECV) {
            onRecv(conn, res, flags);
            return;
        }
        conn.op = 0;
        switch (op) {
            case POLL -> {
                boolean err = res < 0 || (res & (POLLERR | POLLHUP)) != 0 && (res & (POLLIN | POLLOUT)) == 0;
                if (err || conn.needClose) armClose(conn);
                else if (conn.hasSend()) startSend(conn);
                else armRecv(conn);
            }
            case SEND -> onSend(conn, res);
            case CLOSE -> {
                giveSend(conn);
                conn.receiving = false;
                ring.conns[slot] = null;
                ring.free[ring.freeTop++] = slot;
            }
            default -> { }
        }
    }

    private void onRecv(Conn conn, int got, int flags) {
        boolean more = (flags & CQE_F_MORE) != 0;
        if (!more) conn.receiving = false;
        boolean hard = got == 0 || got < 0 && got != -EAGAIN && got != -EINTR && got != -ENOBUFS && got != -ECANCELED;
        if (hard || conn.needClose && !more) {
            conn.receiving = false;
            if (conn.op != CLOSE) armClose(conn);
            return;
        }
        if (got > 0) parseAvailable(conn);
        if (!more) conn.wantRead = true;
        if (!conn.receiving && conn.op != CLOSE && !conn.needClose) armRecv(conn);
    }

    private void onSend(Conn conn, int got) {
        boolean heap = conn.buf >= 0;
        if (heap) release(conn);
        // A non-blocking send completes with -EAGAIN when the socket buffer is full.
        if (got == -EAGAIN || got == -EINTR || got == 0) {
            armPollOut(conn);
            return;
        }
        if (got < 0) {
            armClose(conn);
            return;
        }
        if (!heap && conn.sendSlot >= 0) {
            conn.nativeOff += got;
            if (conn.nativeOff < conn.nativeEnd) {
                startSend(conn);
                return;
            }
            giveSend(conn);
            if (conn.outgoing != null) {
                startSend(conn);
                return;
            }
        } else {
            conn.outOff += got;
            if (conn.outgoing != null && conn.outOff < conn.outgoing.length) {
                startSend(conn);
                return;
            }
            conn.outgoing = null;
            conn.outOff = 0;
            if (conn.sendSlot >= 0 && conn.nativeOff < conn.nativeEnd) {
                startSend(conn);
                return;
            }
        }
        if (conn.closeAfter || conn.needClose) armClose(conn);
        else {
            parseAvailable(conn);
            if (!conn.receiving && conn.op != CLOSE) armRecv(conn);
        }
    }

    /**
     * Stages one response into the connection's off-heap send buffer.
     * The bytes stay past {@code nativeEnd} until the batch commits.
     *
     * @return false when the caller should keep a heap copy instead
     */
    private boolean queueResponse(Conn conn, App.Req req) {
        // Heap bytes already queued are the older prefix. Keep later responses behind them.
        if (conn.outgoing != null || conn.stagedOut != null) return false;
        boolean fresh = conn.sendSlot < 0;
        if (fresh) {
            int slot = sends.take();
            if (slot < 0) return false;
            conn.sendSlot = slot;
            conn.nativeOff = 0;
            conn.nativeEnd = 0;
            conn.stagedEnd = 0;
            conn.staging = true;
        } else if (!conn.staging) {
            conn.stagedEnd = conn.nativeEnd;
            conn.staging = true;
        }
        int room = SEND_CAP - conn.stagedEnd;
        if (room <= 0) {
            if (fresh) giveSend(conn);
            return false;
        }
        int n = app.writeTo(req, sends.slice(conn.sendSlot).asSlice(conn.stagedEnd, room));
        if (n < 0) {
            if (fresh) giveSend(conn);
            return false;
        }
        conn.stagedEnd += n;
        if (req.close) conn.stagedClose = true;
        return true;
    }

    /** Makes staged bytes visible to the sender. Call only after the batch commits. */
    private void publishStaged(Conn conn) {
        if (conn.staging) {
            conn.nativeEnd = conn.stagedEnd;
            conn.staging = false;
        }
        if (conn.stagedClose) conn.closeAfter = true;
        conn.stagedClose = false;
        if (conn.stagedOut != null) {
            conn.queue(conn.stagedOut, false);
            conn.stagedOut = null;
        }
    }

    /** Drops this batch's staged bytes and queues one committed error response. */
    private void discardStaged(Conn conn, byte[] err) {
        conn.staging = false;
        conn.stagedOut = null;
        conn.stagedClose = false;
        if (conn.sendSlot >= 0 && conn.op == 0 && conn.nativeOff >= conn.nativeEnd) giveSend(conn);
        conn.queue(err, true);
    }

    private void giveSend(Conn conn) {
        if (conn.sendSlot < 0) return;
        sends.give(conn.sendSlot);
        conn.sendSlot = -1;
        conn.nativeOff = 0;
        conn.nativeEnd = 0;
        conn.staging = false;
    }

    private void parseAvailable(Conn conn) {
        while (conn.inLen > 0 && conn.pendingCount < 32) {
            App.Req req = takeReq();
            int n = app.parse(conn.in, conn.inLen, req);
            if (n == App.INCOMPLETE) {
                recycleReq(req);
                return;
            }
            conn.consume(n);
            req.next = null;
            if (conn.pendingHead == null) conn.pendingHead = req;
            else conn.pendingTail.next = req;
            conn.pendingTail = req;
            conn.pendingCount++;
            if (!conn.inBatch) {
                conn.inBatch = true;
                batch.add(conn);
            }
        }
    }

    private App.Req takeReq() {
        App.Req req = reqPool.pollFirst();
        if (req == null) req = new App.Req();
        req.next = null;
        req.early = null;
        return req;
    }

    private void recycleReq(App.Req req) {
        if (reqPool.size() < 256) reqPool.addFirst(req);
    }

    private void pumpWrites() {
        int n = writeWait.size();
        for (int i = 0; i < n; i++) {
            Conn conn = writeWait.get(i);
            conn.writeQueued = false;
            if (conn.ring.conns[conn.slot] == conn && conn.hasSend() && conn.op == 0) startSend(conn);
        }
        if (n == writeWait.size()) writeWait.clear();
        else writeWait.subList(0, n).clear();
    }

    private void pumpReads() {
        int n = readWait.size();
        for (int i = 0; i < n; i++) {
            Conn conn = readWait.get(i);
            conn.readQueued = false;
            if (conn.ring.conns[conn.slot] == conn && conn.wantRead && !conn.receiving && conn.op != CLOSE) {
                armRecv(conn);
            }
        }
        if (n == readWait.size()) readWait.clear();
        else readWait.subList(0, n).clear();
    }

    private void pumpPolls() {
        int n = pollWait.size();
        for (int i = 0; i < n; i++) {
            Conn conn = pollWait.get(i);
            conn.pollQueued = false;
            if (conn.ring.conns[conn.slot] != conn || conn.op != 0) continue;
            if (conn.needClose) armClose(conn);
            else if (conn.hasSend()) armPollOut(conn);
            else armRecv(conn);
        }
        if (n == pollWait.size()) pollWait.clear();
        else pollWait.subList(0, n).clear();
    }

    private void pumpCloses() {
        int n = closeWait.size();
        for (int i = 0; i < n; i++) {
            Conn conn = closeWait.get(i);
            conn.closeQueued = false;
            if (conn.ring.conns[conn.slot] == conn && conn.op == 0) armClose(conn);
        }
        if (n == closeWait.size()) closeWait.clear();
        else closeWait.subList(0, n).clear();
    }

    private void startSend(Conn conn) {
        if (conn.op != 0) return;
        if (conn.sendSlot >= 0 && conn.nativeOff < conn.nativeEnd) {
            long addr = sends.address(conn.sendSlot) + conn.nativeOff;
            int n = conn.nativeEnd - conn.nativeOff;
            if (!prep(conn.ring, OP_SEND, IOSQE_FIXED_FILE, conn.slot, addr, n, MSG_NOSIGNAL,
                    ud(conn.slot, conn.gen, SEND), 0)) {
                queueWrite(conn);
                return;
            }
            conn.op = SEND;
            return;
        }
        if (conn.outgoing == null || conn.outOff >= conn.outgoing.length) return;
        int buf = bufs.take();
        if (buf < 0) {
            queueWrite(conn);
            return;
        }
        int n = Math.min(BUF, conn.outgoing.length - conn.outOff);
        MemorySegment dst = bufs.slice(buf);
        MemorySegment.copy(MemorySegment.ofArray(conn.outgoing), JAVA_BYTE, conn.outOff, dst, JAVA_BYTE, 0, n);
        conn.buf = buf;
        if (!prep(conn.ring, OP_SEND, IOSQE_FIXED_FILE, conn.slot, dst.address(), n, MSG_NOSIGNAL,
                ud(conn.slot, conn.gen, SEND), 0)) {
            release(conn);
            queueWrite(conn);
            return;
        }
        conn.op = SEND;
    }

    private void armRecv(Conn conn) {
        if (conn.receiving || conn.op == CLOSE || conn.needClose) return;
        conn.wantRead = false;
        if (!prepEx(conn.ring, OP_RECV, (byte) (IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT), RECV_MULTISHOT,
                conn.slot, 0L, 0L, 0, 0, ud(conn.slot, conn.gen, RECV), BGID, 0)) {
            conn.wantRead = true;
            queueRead(conn);
            return;
        }
        conn.receiving = true;
    }

    private void armPollOut(Conn conn) {
        if (conn.op != 0) return;
        if (!prep(conn.ring, OP_POLL, IOSQE_FIXED_FILE, conn.slot, 0, 0, POLLOUT | POLLERR | POLLHUP,
                ud(conn.slot, conn.gen, POLL), 0)) {
            queuePoll(conn);
            return;
        }
        conn.op = POLL;
    }

    private void armClose(Conn conn) {
        if (conn.op == CLOSE) return;
        if (conn.buf >= 0) release(conn);
        conn.wantRead = false;
        conn.outgoing = null;
        conn.needClose = true;
        if (conn.op != 0) return;
        giveSend(conn);
        if (!prep(conn.ring, OP_CLOSE, (byte) 0, 0, 0, 0, 0, ud(conn.slot, conn.gen, CLOSE), conn.slot + 1)) {
            queueClose(conn);
            return;
        }
        conn.op = CLOSE;
        conn.needClose = false;
    }

    private void queueRead(Conn conn) {
        if (!conn.readQueued) {
            conn.readQueued = true;
            readWait.add(conn);
        }
    }

    private void queueWrite(Conn conn) {
        if (!conn.writeQueued) {
            conn.writeQueued = true;
            writeWait.add(conn);
        }
    }

    private void queuePoll(Conn conn) {
        if (!conn.pollQueued) {
            conn.pollQueued = true;
            pollWait.add(conn);
        }
    }

    private void queueClose(Conn conn) {
        if (!conn.closeQueued) {
            conn.closeQueued = true;
            closeWait.add(conn);
        }
    }

    private void armAccepts(Ring ring) {
        while (ring.acceptsInFlight < 64 && ring.freeTop > 0) {
            if (ring.sqRoom() <= 0) {
                submitRing(ring);
                if (ring.sqRoom() <= 0) return;
            }
            int slot = ring.free[--ring.freeTop];
            ring.gen[slot]++;
            // SOCK_CLOEXEC is rejected with EINVAL once file_index selects a fixed slot.
            if (!prep(ring, OP_ACCEPT, IOSQE_FIXED_FILE, 0, 0, 0, SOCK_NONBLOCK,
                    ud(slot, ring.gen[slot], ACCEPT), slot + 1)) {
                ring.gen[slot]--;
                ring.free[ring.freeTop++] = slot;
                return;
            }
            ring.acceptsInFlight++;
        }
    }

    /** @return false when the submission queue is full and the SQE was not written */
    private boolean prep(Ring ring, byte opcode, byte flags, int fd, long addr, int len, int opFlags,
                      long user, int fileIndex) {
        return prepEx(ring, opcode, flags, (short) 0, fd, 0L, addr, len, opFlags, user, 0, fileIndex);
    }

    private boolean prepEx(Ring ring, byte opcode, byte flags, short ioprio, int fd, long off, long addr,
                           int len, int opFlags, long user, int bufGroup, int fileIndex) {
        if (ring.sqRoom() <= 0) {
            submitRing(ring);
            if (ring.sqRoom() <= 0) return false;
        }
        int index = ring.tail & ring.sqMask;
        MemorySegment sqe = ring.sqes.asSlice((long) index * 64, 64);
        sqe.fill((byte) 0);
        sqe.set(JAVA_BYTE, 0, opcode);
        sqe.set(JAVA_BYTE, 1, flags);
        sqe.set(JAVA_SHORT, 2, ioprio);
        sqe.set(JAVA_INT, 4, fd);
        sqe.set(JAVA_LONG, 8, off);
        sqe.set(JAVA_LONG, 16, addr);
        sqe.set(JAVA_INT, 24, len);
        sqe.set(JAVA_INT, 28, opFlags);
        sqe.set(JAVA_LONG, 32, user);
        sqe.set(JAVA_SHORT, 40, (short) bufGroup);
        sqe.set(JAVA_INT, 44, fileIndex);
        ring.sq.set(JAVA_INT, ring.arrayOff + (long) index * 4, index);
        ring.tail++;
        ring.unsubmitted++;
        return true;
    }

    private void submitAll() {
        for (Ring ring : rings) submitRing(ring);
    }

    /** Leaves {@code unsubmitted} equal to SQEs still sitting between the kernel head and the tail. */
    private void submitRing(Ring ring) {
        if (ring.unsubmitted == 0) return;
        INT.setRelease(ring.sq, ring.sqTailOff, ring.tail);
        long rc = enter(ring, ring.unsubmitted, 0, 0, MemorySegment.NULL, 0);
        if (rc < 0) return;
        int done = (int) rc;
        if (done > ring.unsubmitted) done = ring.unsubmitted;
        ring.unsubmitted -= done;
    }

    private void waitOne() {
        Ring ring = rings[waitCursor];
        waitCursor = (waitCursor + 1) % rings.length;
        if (!extArg) {
            java.util.concurrent.locks.LockSupport.parkNanos(1_000_000L);
            return;
        }
        SCRATCH.set(JAVA_LONG, 0, 0);
        SCRATCH.set(JAVA_LONG, 8, 1_000_000L);
        SCRATCH.set(JAVA_LONG, 32, 0);
        SCRATCH.set(JAVA_INT, 40, 0);
        SCRATCH.set(JAVA_INT, 44, 0);
        SCRATCH.set(JAVA_LONG, 48, SCRATCH.address());
        long rc = enter(ring, 0, 1, ENTER_GETEVENTS | ENTER_EXT_ARG, SCRATCH.asSlice(32, 24), 24);
        if (rc < 0 && errno() == 22) extArg = false;
    }

    private void release(Conn conn) {
        if (conn.buf >= 0) bufs.give(conn.buf);
        conn.buf = -1;
    }

    private static long ud(int slot, int gen, int op) {
        return ((long) gen << 32) | ((long) slot << 8) | (op & 0xff);
    }

    private static int slotsPerRing() {
        long max = 65535;
        try {
            if (java.lang.management.ManagementFactory.getOperatingSystemMXBean()
                    instanceof com.sun.management.UnixOperatingSystemMXBean os) {
                long n = os.getMaxFileDescriptorCount();
                if (n > 0 && n < Integer.MAX_VALUE) max = n;
            }
        } catch (RuntimeException | LinkageError ignored) { }
        return (int) Math.max(128, Math.min(65535, max - 32));
    }

    private static long enter(Ring ring, int submit, int minComplete, int flags, MemorySegment arg, int argLen) {
        try {
            long rc = (long) SYSCALL7.invokeExact(STATE, (long) SYS_IO_URING_ENTER, (long) ring.uringFd,
                    (long) submit, (long) minComplete, (long) flags, arg, (long) argLen);
            if (rc < 0) return -errno();
            return rc;
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private static int errno() { return (int) ERRNO_VH.get(STATE, 0L); }

    private static void quietClose(int fd) {
        try {
            if ((int) CLOSEFD.invokeExact(STATE, fd) != 0) return;
        } catch (Throwable ignored) { }
    }

    private static MethodHandle syscall(FunctionDescriptor fd) {
        MemorySegment sym = LINKER.defaultLookup().find("syscall").orElseThrow();
        return LINKER.downcallHandle(sym, fd, ERRNO, Linker.Option.firstVariadicArg(1));
    }

    private static MethodHandle down(String name, FunctionDescriptor fd) {
        MemorySegment sym = LINKER.defaultLookup().find(name).orElseThrow(() -> new UnsatisfiedLinkError(name));
        return LINKER.downcallHandle(sym, fd, ERRNO);
    }

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) return r;
        if (t instanceof Error e) throw e;
        return new IllegalStateException(t);
    }

    private static final class Bufs {
        final MemorySegment slab = Arena.global().allocate((long) BUFS * BUF, 8);
        final int[] free = new int[BUFS];
        int top = BUFS;

        Bufs() {
            for (int i = 0; i < BUFS; i++) free[i] = i;
        }

        int take() { return top == 0 ? -1 : free[--top]; }

        void give(int i) { free[top++] = i; }

        MemorySegment slice(int i) { return slab.asSlice((long) i * BUF, BUF); }
    }

    /** Off-heap response slots. Idle connections do not hold one. */
    private static final class Sends {
        final MemorySegment slab = Arena.global().allocate((long) SEND_SLOTS * SEND_CAP, 4096);
        final int[] free = new int[SEND_SLOTS];
        int top = SEND_SLOTS;

        Sends() {
            for (int i = 0; i < SEND_SLOTS; i++) free[i] = i;
        }

        int take() { return top == 0 ? -1 : free[--top]; }

        void give(int i) { free[top++] = i; }

        long address(int i) { return slab.address() + (long) i * SEND_CAP; }

        MemorySegment slice(int i) { return slab.asSlice((long) i * SEND_CAP, SEND_CAP); }
    }

    private static final class Conn {
        final Ring ring;
        final int slot;
        final int gen;
        byte[] in = NO_BYTES;
        int inLen;
        App.Req pendingHead;
        App.Req pendingTail;
        int pendingCount;
        byte[] outgoing;
        byte[] stagedOut;
        int sendSlot = -1;
        /** Next unsent byte of {@link #outgoing}. Independent of the off-heap slot. */
        int outOff;
        /** Committed, unsent end of the off-heap slot. The kernel is only given this prefix. */
        int nativeOff;
        int nativeEnd;
        /** Bytes written for the open batch. Published into {@link #nativeEnd} after commit. */
        int stagedEnd;
        boolean staging;
        boolean stagedClose;
        boolean closeAfter;
        boolean receiving;
        boolean wantRead;
        boolean readQueued;
        boolean writeQueued;
        boolean pollQueued;
        boolean closeQueued;
        boolean inBatch;
        boolean needClose;
        int op;
        int buf = -1;
        static final byte[] NO_BYTES = new byte[0];
        static final int IN_CAP = 1024;
        static final ArrayDeque<byte[]> IN_POOL = new ArrayDeque<>();

        Conn(Ring ring, int slot, int gen) {
            this.ring = ring;
            this.slot = slot;
            this.gen = gen;
        }

        void append(MemorySegment src, int n) {
            if (inLen + n > in.length) {
                byte[] next = takeIn(Math.max(IN_CAP, inLen + n));
                if (inLen > 0) System.arraycopy(in, 0, next, 0, inLen);
                recycleIn(in);
                in = next;
            }
            MemorySegment.copy(src, JAVA_BYTE, 0, in, inLen, n);
            inLen += n;
        }

        void consume(int n) {
            int rest = inLen - n;
            if (rest > 0) System.arraycopy(in, n, in, 0, rest);
            inLen = rest;
            if (rest == 0) {
                recycleIn(in);
                in = NO_BYTES;
            }
        }

        boolean hasSend() {
            return sendSlot >= 0 && nativeOff < nativeEnd || outgoing != null && outOff < outgoing.length;
        }

        void stageHeap(byte[] bytes, boolean close) {
            if (stagedOut == null) stagedOut = bytes;
            else stagedOut = concat(stagedOut, 0, bytes);
            if (close) stagedClose = true;
        }

        void queue(byte[] bytes, boolean close) {
            if (outgoing == null) {
                outgoing = bytes;
                outOff = 0;
            } else {
                outgoing = concat(outgoing, outOff, bytes);
                outOff = 0;
            }
            if (close) closeAfter = true;
        }

        private static byte[] concat(byte[] left, int off, byte[] right) {
            int n = left.length - off;
            byte[] merged = new byte[n + right.length];
            System.arraycopy(left, off, merged, 0, n);
            System.arraycopy(right, 0, merged, n, right.length);
            return merged;
        }

        private static byte[] takeIn(int need) {
            if (need <= IN_CAP) {
                byte[] pooled = IN_POOL.pollFirst();
                if (pooled != null) return pooled;
                return new byte[IN_CAP];
            }
            return new byte[need];
        }

        private static void recycleIn(byte[] bytes) {
            if (bytes != NO_BYTES && bytes.length == IN_CAP && IN_POOL.size() < 128) IN_POOL.addFirst(bytes);
        }
    }

    private static final class Ring {
        final int uringFd;
        final int listenFd;
        final MemorySegment sq;
        final MemorySegment cq;
        final MemorySegment sqes;
        final int sqMask;
        final int cqMask;
        final long sqTailOff;
        final long sqHeadOff;
        final long arrayOff;
        final long cqHeadOff;
        final long cqTail;
        final long cqes;
        final Conn[] conns;
        final int[] gen;
        final int[] free;
        int freeTop;
        int acceptsInFlight;
        int tail;
        int unsubmitted;
        int cqHead;
        MemorySegment bufRing;
        MemorySegment recvArea;
        byte[] held;
        int[] pending;
        int pendingN;
        int bufTail;

        Ring(int uringFd, int listenFd, MemorySegment sq, MemorySegment cq, MemorySegment sqes,
             int sqMask, int cqMask, long sqTailOff, long sqHeadOff, long arrayOff, long cqHeadOff, long cqTail,
             long cqes, int slots) {
            this.uringFd = uringFd;
            this.listenFd = listenFd;
            this.sq = sq;
            this.cq = cq;
            this.sqes = sqes;
            this.sqMask = sqMask;
            this.cqMask = cqMask;
            this.sqTailOff = sqTailOff;
            this.sqHeadOff = sqHeadOff;
            this.arrayOff = arrayOff;
            this.cqHeadOff = cqHeadOff;
            this.cqTail = cqTail;
            this.cqes = cqes;
            conns = new Conn[slots];
            gen = new int[slots];
            free = new int[slots - 1];
            for (int i = 1; i < slots; i++) free[freeTop++] = i;
        }

        /**
         * Provided-buffer ring for multishot recv. One 2 KiB buffer is checked out only while a
         * completion holds it. The tail lives in the first entry's reserved halfword (offset 14).
         */
        void installBuffers() throws IOException {
            bufRing = Arena.global().allocate((long) RECV_BUFS * 16, 4096);
            recvArea = Arena.global().allocate((long) RECV_BUFS * RECV_BUF, 4096);
            held = new byte[RECV_BUFS];
            pending = new int[RECV_BUFS];
            for (int i = 0; i < RECV_BUFS; i++) pending[i] = i;
            pendingN = RECV_BUFS;
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment reg = arena.allocate(40, 8);
                reg.set(JAVA_LONG, 0, bufRing.address());
                reg.set(JAVA_INT, 8, RECV_BUFS);
                reg.set(JAVA_SHORT, 12, BGID);
                long rc = call(SYSCALL5, (long) SYS_IO_URING_REGISTER, (long) uringFd,
                        (long) REGISTER_PBUF_RING, reg, 1L);
                if (rc < 0) throw new IOException("io_uring_register pbuf errno " + errno());
            }
            // Tail moves only after the kernel has the ring, and before any recv is submitted.
            publish();
        }

        MemorySegment recvSlice(int bid, int len) {
            int n = Math.min(Math.max(len, 0), RECV_BUF);
            return recvArea.asSlice((long) bid * RECV_BUF, n);
        }

        /** Returns a checked-out buffer. Publishing is deferred so completion handling stays small. */
        void recycle(int bid) {
            if (bid < 0 || bid >= RECV_BUFS || held[bid] != 0) return;
            held[bid] = 1;
            pending[pendingN++] = bid;
        }

        /** Release-stores the ring tail after the buffer entries are visible. */
        void publish() {
            int n = pendingN;
            if (n == 0) return;
            int mask = RECV_BUFS - 1;
            int tailNow = bufTail;
            long base = recvArea.address();
            for (int i = 0; i < n; i++) {
                int bid = pending[i];
                long off = (long) ((tailNow + i) & mask) * 16;
                bufRing.set(JAVA_LONG, off, base + (long) bid * RECV_BUF);
                bufRing.set(JAVA_INT, off + 8, RECV_BUF);
                bufRing.set(JAVA_SHORT, off + 12, (short) bid);
                held[bid] = 0;
            }
            int newTail = (tailNow + n) & 0xffff;
            SHORT.setRelease(bufRing, 14L, (short) newTail);
            bufTail = newTail;
            pendingN = 0;
        }

        /** Free SQ slots. The kernel head is the only count of SQEs it has already consumed. */
        int sqRoom() {
            int head = (int) INT.getAcquire(sq, sqHeadOff);
            int room = (sqMask + 1) - (tail - head);
            return Math.max(room, 0);
        }

        void closeListen() {
            quietClose(listenFd);
            quietClose(uringFd);
        }

        static Ring open(String host, int port, int slots) throws IOException {
            int listen = listen(host, port);
            MemorySegment params = Arena.global().allocate(128, 8);
            params.set(JAVA_INT, 4, CQ_WANT);
            params.set(JAVA_INT, 8, SETUP_CQSIZE | SETUP_CLAMP | SETUP_COOP_TASKRUN | SETUP_SINGLE_ISSUER
                    | SETUP_DEFER_TASKRUN);
            long fd = call(SYSCALL3, (long) SYS_IO_URING_SETUP, (long) SQ_WANT, params);
            if (fd < 0) {
                quietClose(listen);
                throw new IOException("io_uring_setup errno " + errno());
            }
            int uring = (int) fd;
            int sqEntries = params.get(JAVA_INT, 0);
            int cqEntries = params.get(JAVA_INT, 4);
            int sqHeadOff = params.get(JAVA_INT, 40);
            int sqTail = params.get(JAVA_INT, 44);
            int sqMaskOff = params.get(JAVA_INT, 48);
            int sqArray = params.get(JAVA_INT, 64);
            int cqHead = params.get(JAVA_INT, 80);
            int cqTail = params.get(JAVA_INT, 84);
            int cqMaskOff = params.get(JAVA_INT, 88);
            int cqes = params.get(JAVA_INT, 100);
            MemorySegment sq = map(uring, OFF_SQ_RING, sqArray + (long) sqEntries * 4);
            MemorySegment cq = map(uring, OFF_CQ_RING, cqes + (long) cqEntries * 16);
            MemorySegment sqes = map(uring, OFF_SQES, (long) sqEntries * 64);
            int mask = sq.get(JAVA_INT, sqMaskOff);
            int cmask = cq.get(JAVA_INT, cqMaskOff);
            try (Arena a = Arena.ofConfined()) {
                MemorySegment files = a.allocate(JAVA_INT, slots);
                for (int i = 0; i < slots; i++) files.setAtIndex(JAVA_INT, i, -1);
                files.setAtIndex(JAVA_INT, 0, listen);
                long rc = call(SYSCALL5, (long) SYS_IO_URING_REGISTER, (long) uring, (long) REGISTER_FILES, files, (long) slots);
                if (rc < 0) {
                    quietClose(listen);
                    quietClose(uring);
                    throw new IOException("io_uring_register files errno " + errno());
                }
            }
            Ring ring = new Ring(uring, listen, sq, cq, sqes, mask, cmask, sqTail, sqHeadOff, sqArray, cqHead, cqTail, cqes, slots);
            try {
                ring.installBuffers();
            } catch (IOException e) {
                ring.closeListen();
                throw e;
            }
            return ring;
        }

        private static MemorySegment map(int fd, long offset, long length) throws IOException {
            try {
                MemorySegment seg = (MemorySegment) MMAP.invokeExact(STATE, MemorySegment.NULL, length, 3, 1, fd, offset);
                if (seg.address() == -1L) throw new IOException("mmap errno " + errno());
                return seg.reinterpret(length);
            } catch (IOException e) {
                throw e;
            } catch (Throwable t) {
                throw rethrow(t);
            }
        }

        private static int listen(String host, int port) throws IOException {
            InetAddress addr = InetAddress.getByName(host);
            boolean v6 = addr instanceof java.net.Inet6Address;
            int fd;
            try {
                fd = (int) SOCKET.invokeExact(STATE, v6 ? 10 : 2, 1, 0);
            } catch (Throwable t) {
                throw rethrow(t);
            }
            if (fd < 0) throw new IOException("socket errno " + errno());
            setInt(fd, 1, 2, 1);
            setInt(fd, 1, 15, 1);
            setInt(fd, 6, 1, 1);
            MemorySegment sa = SCRATCH.asSlice(64, v6 ? 28 : 16).fill((byte) 0);
            sa.set(JAVA_SHORT, 0, (short) (v6 ? 10 : 2));
            sa.set(JAVA_SHORT.withOrder(java.nio.ByteOrder.BIG_ENDIAN), 2, (short) port);
            byte[] raw = addr.getAddress();
            MemorySegment.copy(raw, 0, sa, JAVA_BYTE, v6 ? 8 : 4, raw.length);
            try {
                if ((int) BIND.invokeExact(STATE, fd, sa, (int) sa.byteSize()) != 0) {
                    throw new IOException("bind errno " + errno());
                }
                if ((int) LISTEN.invokeExact(STATE, fd, 4096) != 0) throw new IOException("listen errno " + errno());
            } catch (IOException e) {
                throw e;
            } catch (Throwable t) {
                throw rethrow(t);
            }
            return fd;
        }

        private static void setInt(int fd, int level, int name, int value) throws IOException {
            SCRATCH.set(JAVA_INT, 0, value);
            try {
                // invokeExact rejects a discarded return, so the int result has to be read.
                int rc = (int) SETSOCKOPT.invokeExact(STATE, fd, level, name, SCRATCH.asSlice(0, 4), 4);
                if (rc != 0) throw new IOException("setsockopt " + name + " errno " + errno());
            } catch (IOException e) {
                throw e;
            } catch (Throwable t) {
                throw rethrow(t);
            }
        }
    }

    private static long call(MethodHandle handle, Object... args) {
        Object[] full = new Object[args.length + 1];
        full[0] = STATE;
        System.arraycopy(args, 0, full, 1, args.length);
        try {
            long rc = (long) handle.invokeWithArguments(full);
            return rc < 0 ? -1 : rc;
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }
}
