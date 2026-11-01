using System.Runtime.InteropServices;
using static Feed.Native;

namespace Feed;

/// <summary>
/// One io_uring instance with its own sparse file table (direct descriptors) and its own receive
/// buffers: a provided buffer ring, or classic provided buffers where the kernel refuses the ring.
/// It is used only from the thread that created it, which SINGLE_ISSUER requires. Submission
/// entries are written to the shared ring and become visible to the kernel in <see cref="Submit"/>
/// or <see cref="Enter"/>; completions are read by the owner straight from <see cref="Cqes"/>.
/// </summary>
internal sealed unsafe class Ring {
    /// <summary>Entries in the submission queue.</summary>
    public const int SqEntries = 4096;
    /// <summary>Entries asked for in the completion queue. Halved while the kernel answers ENOMEM.</summary>
    public const int CqEntries = 16384;
    /// <summary>Receive buffers per ring (a power of two, as the buffer ring needs).</summary>
    public const int BufCount = 4096;
    /// <summary>Bytes in each provided receive buffer.</summary>
    public const int BufSize = 2048;

    /// <summary>Position of this ring in the server's ring list.</summary>
    public readonly int Index;
    /// <summary>The io_uring file descriptor.</summary>
    public readonly int Fd;
    /// <summary>Size of the file table: the most connections this ring can hold.</summary>
    public readonly int Slots;
    /// <summary>
    /// The ring was set up with DEFER_TASKRUN and COOP_TASKRUN: completions appear only when the owner
    /// enters the ring. False on a kernel that refused those flags.
    /// </summary>
    public readonly bool Deferred;
    /// <summary>
    /// True when the kernel refused a provided buffer ring, so buffers are handed over with classic
    /// IORING_OP_PROVIDE_BUFFERS entries instead.
    /// </summary>
    public readonly bool LegacyBuffers;

    /// <summary>Completion queue head (we write it) and tail (the kernel writes it).</summary>
    public readonly uint* CqHead, CqTail;
    /// <summary>The completion entries.</summary>
    public readonly Cqe* Cqes;
    /// <summary>Mask for indexing <see cref="Cqes"/>.</summary>
    public readonly uint CqMask;

    /// <summary>Start of the receive buffer memory. Buffer <c>bid</c> sits at <c>BufBase + bid * BufSize</c>.</summary>
    public readonly byte* BufBase;

    /// <summary>Connections by slot, created on demand.</summary>
    public Conn?[] Conns = new Conn?[256];
    /// <summary>Slots in use: accepted and not yet queued for closing in the file table.</summary>
    public int OpenCount;
    /// <summary>A multishot accept is armed and has not ended.</summary>
    public bool AcceptArmed;
    /// <summary>A cancel for the accept has been queued.</summary>
    public bool AcceptCancelling;
    /// <summary>Earliest tick (ms) at which a failed accept may be armed again.</summary>
    public long AcceptRetryAt;
    /// <summary>Count of failed accepts and failed submits, for debugging.</summary>
    public int AcceptErrors, SubmitErrors;

    private readonly uint* _sqHead, _sqTail, _sqFlags;
    private readonly Sqe* _sqes;
    private readonly uint _sqMask;
    private uint _sqLocalTail;
    private readonly BufRingEntry* _bufRing;
    private ushort _bufTail;

    /// <summary>
    /// Creates the ring, maps its queues, registers a sparse file table of <paramref name="slots"/>
    /// entries and a full provided buffer ring, or classic provided buffers where the kernel refuses
    /// the ring (see <see cref="LegacyBuffers"/>). Throws with the errno of the call that failed.
    /// </summary>
    public Ring(int index, int slots) {
        if (sizeof(UringParams) != 120 || sizeof(Sqe) != 64 || sizeof(Cqe) != 16 || sizeof(RsrcRegister) != 32
            || sizeof(BufReg) != 40 || sizeof(BufRingEntry) != 16 || sizeof(GetEventsArg) != 24
            || sizeof(KernelTimespec) != 16) {
            throw new InvalidOperationException("io_uring struct layout does not match the kernel ABI");
        }

        Index = index;
        Slots = slots;
        // Some kernels count ring memory against RLIMIT_MEMLOCK, which is 8 MiB for a systemd service
        // by default, so a smaller completion queue is tried before giving up. A full queue only costs
        // extra flushes, because the kernel keeps the overflow (see FlushOverflow).
        UringParams p;
        long fd;
        uint cqSize = CqEntries;
        bool deferred = true;
        while (true) {
            p = default;
            p.SqEntries = SqEntries;
            p.CqEntries = cqSize;
            // DEFER_TASKRUN batches the kernel's completion work until we enter the ring, which saves
            // CPU. It needs kernel 6.1 or later, so EINVAL means: try again without it (SINGLE_ISSUER
            // stays, which needs 6.0).
            p.Flags = IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER;
            if (deferred) {
                p.Flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN;
            }

            fd = syscall2(SYS_io_uring_setup, SqEntries, (nint)(&p));
            if (fd >= 0) {
                break;
            }

            int errno = Marshal.GetLastPInvokeError();
            if (errno == EINVAL && deferred) {
                deferred = false;
            } else if (errno != ENOMEM || cqSize <= SqEntries) {
                break;
            } else {
                cqSize /= 2;
            }
        }

        if (fd < 0) {
            Fail("io_uring_setup");
        }

        Fd = (int)fd;
        Deferred = deferred;
        byte* sq = Map((nuint)(p.Sq.Array + p.SqEntries * 4), IORING_OFF_SQ_RING, "mmap sq ring");
        byte* cq = Map((nuint)(p.Cq.Cqes + p.CqEntries * (uint)sizeof(Cqe)), IORING_OFF_CQ_RING, "mmap cq ring");
        _sqes = (Sqe*)Map((nuint)(p.SqEntries * (uint)sizeof(Sqe)), IORING_OFF_SQES, "mmap sqes");
        _sqHead = (uint*)(sq + p.Sq.Head);
        _sqTail = (uint*)(sq + p.Sq.Tail);
        _sqFlags = (uint*)(sq + p.Sq.Flags);
        _sqMask = *(uint*)(sq + p.Sq.RingMask);
        // The index array maps entry i to SQE i for good, so it is filled once.
        uint* array = (uint*)(sq + p.Sq.Array);
        for (uint i = 0; i <= _sqMask; i++) {
            array[i] = i;
        }

        CqHead = (uint*)(cq + p.Cq.Head);
        CqTail = (uint*)(cq + p.Cq.Tail);
        CqMask = *(uint*)(cq + p.Cq.RingMask);
        Cqes = (Cqe*)(cq + p.Cq.Cqes);

        RsrcRegister files = default;
        files.Nr = (uint)slots;
        files.Flags = IORING_RSRC_REGISTER_SPARSE;
        if (syscall4(SYS_io_uring_register, Fd, IORING_REGISTER_FILES2, (nint)(&files), sizeof(RsrcRegister)) < 0) {
            Fail("io_uring_register files");
        }

        BufBase = (byte*)Anonymous((nuint)BufCount * BufSize, "mmap buffers");
        _bufRing = (BufRingEntry*)Anonymous((nuint)BufCount * (nuint)sizeof(BufRingEntry), "mmap buffer ring");
        BufReg reg = default;
        reg.RingAddr = (ulong)_bufRing;
        reg.RingEntries = BufCount;
        reg.Bgid = 0;
        if (syscall4(SYS_io_uring_register, Fd, IORING_REGISTER_PBUF_RING, (nint)(&reg), 1) < 0) {
            // Ubuntu 24.04's 6.8 kernel (seen on 6.8.0-147) refuses IORING_REGISTER_PBUF_RING with EINVAL, while classic
            // provided buffers work there (Rust #82 and C #26 fall back the same way).
            if (Marshal.GetLastPInvokeError() != EINVAL) {
                Fail("io_uring_register buffer ring");
            }

            LegacyBuffers = true;
            PrepProvide(0, BufCount);
            Submit();
            return;
        }

        for (int bid = 0; bid < BufCount; bid++) {
            ReturnBuffer(bid);
        }

        PublishBuffers();
    }

    /// <summary>Maps part of the ring's memory shared with the kernel.</summary>
    private byte* Map(nuint length, long offset, string what) {
        void* p = mmap(null, length, PROT_READ_WRITE, MAP_SHARED | MAP_POPULATE, Fd, offset);
        if (p == (void*)-1) {
            Fail(what);
        }

        return (byte*)p;
    }

    /// <summary>Maps zeroed, page-aligned private memory.</summary>
    private static void* Anonymous(nuint length, string what) {
        void* p = mmap(null, length, PROT_READ_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == (void*)-1) {
            Fail(what);
        }

        return p;
    }

    /// <summary>
    /// Gives receive buffer <paramref name="bid"/> back to the kernel. With a buffer ring it is not visible
    /// until <see cref="PublishBuffers"/>; with classic provided buffers a provide entry is queued.
    /// </summary>
    public void ReturnBuffer(int bid) {
        if (LegacyBuffers) {
            PrepProvide(bid, 1);
            return;
        }

        BufRingEntry* e = _bufRing + (_bufTail & (BufCount - 1));
        e->Addr = (ulong)(BufBase + (long)bid * BufSize);
        e->Len = BufSize;
        e->Bid = (ushort)bid;   // Resv is left alone: for entry 0 it is the shared tail
        _bufTail++;
    }

    /// <summary>
    /// Makes the returned buffers visible to the kernel. The ring's tail lives in the reserved field of entry 0.
    /// Does nothing with classic provided buffers, which are queued as entries.
    /// </summary>
    public void PublishBuffers() {
        if (!LegacyBuffers) {
            Volatile.Write(ref *(ushort*)((byte*)_bufRing + 14), _bufTail);
        }
    }

    /// <summary>
    /// Queues a provide of <paramref name="count"/> buffers of group 0 starting at id <paramref name="first"/>.
    /// A success posts no completion; a failure arrives with the ignored tag.
    /// </summary>
    private void PrepProvide(int first, int count) {
        Sqe* s = GetSqe();
        s->Opcode = IORING_OP_PROVIDE_BUFFERS;
        s->Flags = IOSQE_CQE_SKIP_SUCCESS;
        s->Fd = count;
        s->Addr = (ulong)(BufBase + (long)first * BufSize);
        s->Len = BufSize;
        s->Off = (ulong)first;
        s->BufGroup = 0;
        s->UserData = 4;   // Server's OpIgnore in the low byte
    }

    /// <summary>True when entries are written but not yet taken by the kernel.</summary>
    public bool HasUnsubmitted => _sqLocalTail != Volatile.Read(ref *_sqHead);

    /// <summary>
    /// Returns a zeroed submission entry to fill in. If the queue is full it submits first. Throws only if
    /// the kernel still refuses to take entries after many tries.
    /// </summary>
    public Sqe* GetSqe() {
        for (int tries = 0; _sqLocalTail - Volatile.Read(ref *_sqHead) > _sqMask; tries++) {
            if (tries == 1000) {
                throw new InvalidOperationException("io_uring submission queue stays full");
            }

            Submit();
        }
        Sqe* sqe = _sqes + (_sqLocalTail & _sqMask);
        *sqe = default;
        _sqLocalTail++;
        return sqe;
    }

    /// <summary>
    /// Publishes the queued entries and asks the kernel to take them. EINTR, EAGAIN and EBUSY leave the
    /// entries queued; <see cref="HasUnsubmitted"/> tells the caller to try again later.
    /// </summary>
    public void Submit() {
        uint pending = _sqLocalTail - Volatile.Read(ref *_sqHead);
        if (pending == 0) {
            return;
        }

        Volatile.Write(ref *_sqTail, _sqLocalTail);
        if (syscall6(SYS_io_uring_enter, Fd, (nint)pending, 0, 0, 0, 0) < 0
            && Marshal.GetLastPInvokeError() is not (EINTR or EAGAIN or EBUSY)) {
            SubmitErrors++;
        }
    }

    /// <summary>
    /// Publishes the queued entries, submits them, runs the kernel's deferred completion work and waits
    /// until <paramref name="minComplete"/> completions are available or <paramref name="timeoutNs"/>
    /// nanoseconds pass. With <paramref name="minComplete"/> of 0 it does not wait. A timeout, EINTR,
    /// EAGAIN and EBUSY are normal; other errors are counted in <see cref="SubmitErrors"/>. Entries the
    /// kernel did not take stay queued (<see cref="HasUnsubmitted"/>).
    /// </summary>
    public void Enter(uint minComplete, long timeoutNs) {
        uint pending = _sqLocalTail - Volatile.Read(ref *_sqHead);
        if (pending != 0) {
            Volatile.Write(ref *_sqTail, _sqLocalTail);
        }

        KernelTimespec ts = new() { Sec = timeoutNs / 1_000_000_000, Nsec = timeoutNs % 1_000_000_000 };
        GetEventsArg arg = new() { Ts = (ulong)&ts };
        if (syscall6(SYS_io_uring_enter, Fd, (nint)pending, (nint)minComplete,
                IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, (nint)(&arg), sizeof(GetEventsArg)) < 0
            && Marshal.GetLastPInvokeError() is not (ETIME or EINTR or EAGAIN or EBUSY)) {
            SubmitErrors++;
        }
    }

    /// <summary>
    /// Moves completions the kernel kept aside because the completion queue was full back into the
    /// queue. Only entering the ring with GETEVENTS flushes them, so without this the loop would never see them.
    /// </summary>
    public void FlushOverflow() {
        if ((Volatile.Read(ref *_sqFlags) & IORING_SQ_CQ_OVERFLOW) != 0) {
            syscall6(SYS_io_uring_enter, Fd, 0, 0, IORING_ENTER_GETEVENTS, 0, 0);
        }
    }

    /// <summary>Queues a multishot accept of <paramref name="listenFd"/> into free file table slots.</summary>
    public void PrepAccept(int listenFd, ulong userData) {
        Sqe* s = GetSqe();
        s->Opcode = IORING_OP_ACCEPT;
        s->Fd = listenFd;
        s->Ioprio = IORING_ACCEPT_MULTISHOT;
        s->OpFlags = SOCK_NONBLOCK;
        s->FileIndex = IORING_FILE_INDEX_ALLOC;
        s->UserData = userData;
    }

    /// <summary>Queues a cancel of the operation with user data <paramref name="target"/>.</summary>
    public void PrepCancel(ulong target, ulong userData) {
        Sqe* s = GetSqe();
        s->Opcode = IORING_OP_ASYNC_CANCEL;
        s->Addr = target;
        s->UserData = userData;
    }

    /// <summary>Queues a multishot receive on a slot, taking buffers from group 0.</summary>
    public void PrepRecv(int slot, ulong userData) {
        Sqe* s = GetSqe();
        s->Opcode = IORING_OP_RECV;
        s->Flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
        s->Ioprio = IORING_RECV_MULTISHOT;
        s->Fd = slot;
        s->BufGroup = 0;
        s->UserData = userData;
    }

    /// <summary>Queues a send of <paramref name="len"/> bytes at <paramref name="data"/> on a slot. The memory must stay put until the completion.</summary>
    public void PrepSend(int slot, byte* data, int len, ulong userData) {
        Sqe* s = GetSqe();
        s->Opcode = IORING_OP_SEND;
        s->Flags = IOSQE_FIXED_FILE;
        s->Fd = slot;
        s->Addr = (ulong)data;
        s->Len = (uint)len;
        s->OpFlags = MSG_NOSIGNAL;
        s->UserData = userData;
    }

    /// <summary>Queues a shutdown of both directions on a slot, so a pending receive or send ends. A success posts no completion.</summary>
    public void PrepShutdown(int slot, ulong userData) {
        Sqe* s = GetSqe();
        s->Opcode = IORING_OP_SHUTDOWN;
        s->Flags = IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS;
        s->Fd = slot;
        s->Len = SHUT_RDWR;
        s->UserData = userData;
    }

    /// <summary>Queues a close of a slot in the file table. A success posts no completion.</summary>
    public void PrepClose(int slot, ulong userData) {
        Sqe* s = GetSqe();
        s->Opcode = IORING_OP_CLOSE;
        s->Flags = IOSQE_CQE_SKIP_SUCCESS;
        s->Fd = 0;
        s->FileIndex = (uint)slot + 1;
        s->UserData = userData;
    }

    /// <summary>Throws with the errno of the failed system call.</summary>
    private static void Fail(string what) =>
        throw new InvalidOperationException($"{what} failed: errno {Marshal.GetLastPInvokeError()}");
}
