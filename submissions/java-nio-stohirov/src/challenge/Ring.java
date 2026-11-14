package challenge;

import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.VarHandle;

/**
 * One io_uring: its submission and completion queues, a sparse table of direct descriptors, and a
 * ring of provided receive buffers, so an idle connection holds no buffer. All of it is shared memory
 * that Java reads and writes directly, and the only system call is io_uring_enter. Used by one thread.
 * Assumes a little-endian machine, which x86_64 and aarch64 Linux both are.
 */
final class Ring {
    private static final long OFF_SQ_RING = 0L, OFF_CQ_RING = 0x8000000L, OFF_SQES = 0x10000000L;
    private static final int SETUP_CQSIZE = 1 << 3, SETUP_CLAMP = 1 << 4, SETUP_SUBMIT_ALL = 1 << 7,
            SETUP_SINGLE_ISSUER = 1 << 12, SETUP_DEFER_TASKRUN = 1 << 13;
    private static final int ENTER_GETEVENTS = 1, ENTER_EXT_ARG = 1 << 3;
    private static final int REGISTER_FILES2 = 13, REGISTER_PBUF_RING = 22, RSRC_REGISTER_SPARSE = 1;

    private static final int OP_POLL_ADD = 6, OP_ACCEPT = 13, OP_CLOSE = 19, OP_SEND = 26, OP_RECV = 27, OP_SHUTDOWN = 34;
    private static final long F_FIXED_FILE = 1, F_HARDLINK = 1 << 3, F_BUFFER_SELECT = 1 << 5;
    private static final long RECV_MULTISHOT = 1 << 1, FILE_INDEX_ALLOC = 0xFFFFFFFFL;
    private static final long POLLOUT_EVENTS = 0x4 | 0x8 | 0x10; // POLLOUT, POLLERR, POLLHUP
    private static final long MSG_DONTWAIT = 0x40, MSG_NOSIGNAL = 0x4000, SHUT_RDWR = 2;

    static final int CQE_F_BUFFER = 1, CQE_F_MORE = 2;

    private static final int SQ_ENTRIES = 4096, CQ_ENTRIES = 65536;
    private static final int BUF_COUNT = 1024, BUF_SIZE = 2048;

    private static final VarHandle INT = ValueLayout.JAVA_INT.varHandle();
    private static final VarHandle SHORT = ValueLayout.JAVA_SHORT.varHandle();

    private final int fd;
    private final MemorySegment sq, sqes;
    private final int sqMask;
    private final long sqTailOff;
    private int sqTail, submitted;

    final MemorySegment cq;
    final int cqMask;
    final long cqTailOff, cqHeadOff, cqesOff;
    int cqHead;

    private final MemorySegment bufRing;
    final MemorySegment bufs = Arena.global().allocate((long) BUF_COUNT * BUF_SIZE, 4096);
    private final long bufsAddr = bufs.address();
    private int bufTail;
    private boolean bufsDirty;

    final Server.Conn[] conns;
    final int[] accepts = new int[Server.LISTENERS];
    int open;
    boolean full;
    Server.Conn[] rearm = new Server.Conn[64];
    int rearmCount;

    Ring(int slots) {
        MemorySegment params = Arena.global().allocate(120, 8);
        params.set(JAVA_INT, 4, CQ_ENTRIES);
        params.set(JAVA_INT, 8, SETUP_CQSIZE | SETUP_CLAMP | SETUP_SUBMIT_ALL | SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN);
        long ringFd = Sys.uringSetup(SQ_ENTRIES, params);
        if (ringFd < 0) throw new IllegalStateException("io_uring_setup: errno " + Sys.errno());
        fd = (int) ringFd;
        int sqEntries = params.get(JAVA_INT, 0), cqEntries = params.get(JAVA_INT, 4);
        int sqArray = params.get(JAVA_INT, 64);
        sqTailOff = params.get(JAVA_INT, 44);
        cqHeadOff = params.get(JAVA_INT, 80);
        cqTailOff = params.get(JAVA_INT, 84);
        cqesOff = params.get(JAVA_INT, 100);
        sq = Sys.mapRing(fd, OFF_SQ_RING, sqArray + (long) sqEntries * 4);
        cq = Sys.mapRing(fd, OFF_CQ_RING, cqesOff + (long) cqEntries * 16);
        sqes = Sys.mapRing(fd, OFF_SQES, (long) sqEntries * 64);
        sqMask = sq.get(JAVA_INT, params.get(JAVA_INT, 48));
        cqMask = cq.get(JAVA_INT, params.get(JAVA_INT, 88));
        // The array maps queue positions to SQEs. Identity is all this server needs, so it is set once.
        for (int i = 0; i < sqEntries; i++) sq.set(JAVA_INT, sqArray + i * 4L, i);

        MemorySegment files = Arena.global().allocate(32, 8);
        files.set(JAVA_INT, 0, slots);
        files.set(JAVA_INT, 4, RSRC_REGISTER_SPARSE);
        if (Sys.uringRegister(fd, REGISTER_FILES2, files, 32) < 0) {
            throw new IllegalStateException("register files: errno " + Sys.errno());
        }
        conns = new Server.Conn[slots];

        bufRing = Arena.global().allocate((long) BUF_COUNT * 16, 4096);
        registerBufRing();
        for (int i = 0; i < BUF_COUNT; i++) recycle(i);
        publishBufs();
    }

    /**
     * Ubuntu's 6.8.0-136 and later kernels have an inverted check on this structure's reserved words
     * (Launchpad #2162843): they refuse all zeroes and accept a nonzero resv[0]. A fixed kernel takes
     * the first form, so the second is only tried after an EINVAL.
     */
    private void registerBufRing() {
        MemorySegment reg = Arena.global().allocate(40, 8);
        reg.set(JAVA_LONG, 0, bufRing.address());
        reg.set(JAVA_INT, 8, BUF_COUNT);
        if (Sys.uringRegister(fd, REGISTER_PBUF_RING, reg, 1) >= 0) return;
        if (Sys.errno() == Sys.EINVAL) {
            reg.set(JAVA_LONG, 16, 1L);
            if (Sys.uringRegister(fd, REGISTER_PBUF_RING, reg, 1) >= 0) return;
        }
        throw new IllegalStateException("register buffer ring: errno " + Sys.errno());
    }

    long bufOffset(int bid) { return (long) bid * BUF_SIZE; }

    /** Hands a receive buffer back. The kernel sees it at the next {@link #publishBufs}. */
    void recycle(int bid) {
        long off = (long) (bufTail & (BUF_COUNT - 1)) * 16;
        bufRing.set(JAVA_LONG, off, bufsAddr + (long) bid * BUF_SIZE);
        bufRing.set(JAVA_INT, off + 8, BUF_SIZE);
        bufRing.set(JAVA_SHORT, off + 12, (short) bid);
        bufTail++;
        bufsDirty = true;
    }

    /** The tail shares its 16 bits with the first entry's reserved field, at byte 14 of the ring. */
    void publishBufs() {
        if (!bufsDirty) return;
        bufsDirty = false;
        SHORT.setRelease(bufRing, 14L, (short) bufTail);
    }

    private void push(long op, long flags, long ioprio, long sockFd, long addr, long lenAndOpFlags, long ud, long fileIndex) {
        int i = sqTail & sqMask;
        long o = i * 64L;
        sqes.set(JAVA_LONG, o, op | flags << 8 | ioprio << 16 | sockFd << 32);
        sqes.set(JAVA_LONG, o + 8, 0L);
        sqes.set(JAVA_LONG, o + 16, addr);
        sqes.set(JAVA_LONG, o + 24, lenAndOpFlags);
        sqes.set(JAVA_LONG, o + 32, ud);
        sqes.set(JAVA_LONG, o + 40, fileIndex << 32);
        sqes.set(JAVA_LONG, o + 48, 0L);
        sqes.set(JAVA_LONG, o + 56, 0L);
        sqTail++;
    }

    /** Makes room for need entries, so a linked pair is never split across two submissions. */
    private void room(int need) {
        for (int tries = 0; SQ_ENTRIES - (sqTail - submitted) < need; tries++) {
            submit(0);
            if (tries == 1000) throw new IllegalStateException("submission queue stuck full");
        }
    }

    void accept(int listenFd, long ud) {
        room(1);
        push(OP_ACCEPT, 0, 0, listenFd, 0, 0, ud, FILE_INDEX_ALLOC);
    }

    void recv(int slot, long ud) {
        room(1);
        push(OP_RECV, F_FIXED_FILE | F_BUFFER_SELECT, RECV_MULTISHOT, slot, 0, 0, ud, 0);
    }

    /**
     * MSG_DONTWAIT makes the kernel finish the send while it reads the queue, with -EAGAIN or a short
     * count if the socket buffer is full, and never hold on to the address afterwards.
     */
    void send(int slot, long addr, int len, long ud) {
        room(1);
        push(OP_SEND, F_FIXED_FILE, 0, slot, addr, (long) len | (MSG_NOSIGNAL | MSG_DONTWAIT) << 32, ud, 0);
    }

    void pollWritable(int slot, long ud) {
        room(1);
        push(OP_POLL_ADD, F_FIXED_FILE, 0, slot, 0, POLLOUT_EVENTS << 32, ud, 0);
    }

    /** The shutdown ends the multishot receive, which would otherwise keep the socket alive; the close frees the slot. */
    void shutdownAndClose(int slot, long shutdownUd, long closeUd) {
        room(2);
        push(OP_SHUTDOWN, F_FIXED_FILE | F_HARDLINK, 0, slot, 0, SHUT_RDWR, shutdownUd, 0);
        push(OP_CLOSE, 0, 0, 0, 0, 0, closeUd, slot + 1L);
    }

    private void publishSq() { INT.setRelease(sq, sqTailOff, sqTail); }

    /** Submits what is queued. The kernel completes sends inline here. */
    private void submit(int getEventsFlag) {
        publishSq();
        long rc = Sys.uringEnter(fd, sqTail - submitted, 0, getEventsFlag, 0, 0);
        if (rc >= 0) submitted += (int) rc;
        else if (rc == -1 && Sys.errno() == Sys.EBUSY && getEventsFlag == 0) submit(ENTER_GETEVENTS);
    }

    /** Submits and collects completions without waiting. Deferred completions only run inside an enter. */
    void enter() { submit(ENTER_GETEVENTS); }

    /** Submits, then waits for min completions or until the timeout in waitArg, an io_uring_getevents_arg. */
    void enterAndWait(int min, long waitArg, long waitArgSize) {
        publishSq();
        long rc = Sys.uringEnter(fd, sqTail - submitted, min, ENTER_GETEVENTS | ENTER_EXT_ARG, waitArg, waitArgSize);
        if (rc >= 0) {
            submitted += (int) rc;
            return;
        }
        int e = Sys.errno();
        if (e != Sys.ETIME && e != Sys.EINTR && e != Sys.EBUSY && e != Sys.EAGAIN) {
            System.err.println("io_uring_enter: errno " + e);
        }
    }
}
