using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using static Feed.Native;

namespace Feed;

/// <summary>The kernel's io_uring_sqe (64 bytes), with the union members this server uses.</summary>
[StructLayout(LayoutKind.Explicit, Size = 64)]
internal struct Sqe {
    [FieldOffset(0)] public byte Opcode;
    [FieldOffset(1)] public byte Flags;
    [FieldOffset(2)] public ushort Ioprio;
    [FieldOffset(4)] public int Fd;
    [FieldOffset(8)] public ulong Off;
    [FieldOffset(16)] public ulong Addr;
    [FieldOffset(24)] public uint Len;
    [FieldOffset(28)] public uint OpFlags;   // msg_flags / accept_flags / ...
    [FieldOffset(32)] public ulong UserData;
    [FieldOffset(40)] public ushort BufGroup;
    [FieldOffset(44)] public uint FileIndex;
}

/// <summary>The kernel's io_uring_cqe (16 bytes).</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct Cqe {
    public ulong UserData;
    public int Res;
    public uint Flags;
}

/// <summary>
/// One io_uring instance driven through raw syscalls. The SQ and CQ rings are mapped once and SQEs
/// are written in place. The ring is owned by one thread (IORING_SETUP_SINGLE_ISSUER), which is
/// also the thread that waits on it (IORING_SETUP_DEFER_TASKRUN). Layouts and constants were
/// checked against linux/io_uring.h from kernel 6.8.
/// </summary>
internal sealed unsafe class Ring {
    public const byte OP_NOP = 0;
    public const byte OP_ACCEPT = 13;
    public const byte OP_ASYNC_CANCEL = 14;
    public const byte OP_CLOSE = 19;
    public const byte OP_READ = 22;
    public const byte OP_SEND = 26;
    public const byte OP_RECV = 27;
    public const byte OP_SHUTDOWN = 34;

    public const byte IOSQE_FIXED_FILE = 1 << 0;
    public const byte IOSQE_BUFFER_SELECT = 1 << 5;
    public const byte IOSQE_CQE_SKIP_SUCCESS = 1 << 6;

    public const uint IORING_SETUP_CQSIZE = 1 << 3;
    public const uint IORING_SETUP_SUBMIT_ALL = 1 << 7;
    public const uint IORING_SETUP_COOP_TASKRUN = 1 << 8;
    public const uint IORING_SETUP_SINGLE_ISSUER = 1 << 12;
    public const uint IORING_SETUP_DEFER_TASKRUN = 1 << 13;

    public const uint IORING_ENTER_GETEVENTS = 1 << 0;
    public const uint IORING_ENTER_EXT_ARG = 1 << 3;
    public const uint IORING_ENTER_REGISTERED_RING = 1 << 4;

    public const ushort IORING_ACCEPT_MULTISHOT = 1 << 0;
    public const ushort IORING_RECV_MULTISHOT = 1 << 1;
    public const uint IORING_FILE_INDEX_ALLOC = ~0u;

    public const uint IORING_CQE_F_BUFFER = 1 << 0;
    public const uint IORING_CQE_F_MORE = 1 << 1;
    public const int IORING_CQE_BUFFER_SHIFT = 16;

    private const uint IORING_FEAT_SINGLE_MMAP = 1 << 0;
    private const uint IORING_FEAT_NODROP = 1 << 1;
    private const uint IORING_FEAT_EXT_ARG = 1 << 8;
    private const long IORING_OFF_SQ_RING = 0;
    private const long IORING_OFF_SQES = 0x10000000;
    private const uint IORING_REGISTER_FILES2 = 13;
    private const uint IORING_REGISTER_RING_FDS = 20;
    private const uint IORING_REGISTER_PBUF_RING = 22;
    private const uint IORING_RSRC_REGISTER_SPARSE = 1;

    /// <summary>The kernel's io_uring_params (120 bytes). The offsets blocks are read as raw uints.</summary>
    [StructLayout(LayoutKind.Explicit, Size = 120)]
    private struct Params {
        [FieldOffset(0)] public uint SqEntries;
        [FieldOffset(4)] public uint CqEntries;
        [FieldOffset(8)] public uint Flags;
        [FieldOffset(20)] public uint Features;
        [FieldOffset(40)] public uint SqHead;
        [FieldOffset(44)] public uint SqTail;
        [FieldOffset(48)] public uint SqRingMask;
        [FieldOffset(56)] public uint SqFlags;
        [FieldOffset(64)] public uint SqArray;
        [FieldOffset(80)] public uint CqHead;
        [FieldOffset(84)] public uint CqTail;
        [FieldOffset(88)] public uint CqRingMask;
        [FieldOffset(100)] public uint CqCqes;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct GeteventsArg {
        public ulong Sigmask;
        public uint SigmaskSz;
        public uint Pad;
        public ulong Ts;
    }

    private readonly int _fd;
    private int _enterFd;
    private uint _enterFlags;
    private readonly uint* _sqHead, _sqTail;
    private readonly uint _sqMask, _sqEntries;
    private readonly Sqe* _sqes;
    private readonly uint* _cqHead, _cqTail;
    private readonly uint _cqMask;
    private readonly Cqe* _cqes;
    private uint _sqeTail;      // next SQE to fill (published to the kernel by Flush)
    private uint _submitted;    // SQ tail the kernel has been told about

    /// <summary>Flags the ring was created with (DEFER_TASKRUN etc. are dropped on kernels that refuse them).</summary>
    public uint SetupFlags { get; }

    /// <summary>Creates the ring. Throws if io_uring is unavailable.</summary>
    public Ring(uint entries, uint cqEntries) {
        Params p = default;
        uint[] attempts = [
            IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
            IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN,
            IORING_SETUP_CQSIZE,
        ];
        int fd = -1;
        foreach (uint flags in attempts) {
            p = default;
            p.Flags = flags;
            p.CqEntries = cqEntries;
            fd = (int)syscall(SYS_io_uring_setup, entries, (long)&p, 0, 0, 0, 0);
            if (fd >= 0) {
                SetupFlags = flags;
                break;
            }
            if (Marshal.GetLastPInvokeError() != EINVAL) {
                break;
            }
        }
        if (fd < 0) {
            throw new InvalidOperationException($"io_uring_setup failed: errno {Marshal.GetLastPInvokeError()}");
        }

        if ((p.Features & IORING_FEAT_SINGLE_MMAP) == 0 || (p.Features & IORING_FEAT_EXT_ARG) == 0
            || (p.Features & IORING_FEAT_NODROP) == 0) {
            throw new InvalidOperationException("io_uring: kernel too old (needs SINGLE_MMAP, NODROP and EXT_ARG)");
        }

        _fd = fd;
        _enterFd = fd;
        _sqEntries = p.SqEntries;
        long sqSize = p.SqArray + (p.SqEntries * 4L);
        long cqSize = p.CqCqes + (p.CqEntries * (long)sizeof(Cqe));
        long size = Math.Max(sqSize, cqSize);
        byte* rings = (byte*)Map(size, IORING_OFF_SQ_RING); // SINGLE_MMAP: the CQ ring shares this mapping
        _sqes = (Sqe*)Map(p.SqEntries * (long)sizeof(Sqe), IORING_OFF_SQES);

        _sqHead = (uint*)(rings + p.SqHead);
        _sqTail = (uint*)(rings + p.SqTail);
        _sqMask = *(uint*)(rings + p.SqRingMask);
        uint* array = (uint*)(rings + p.SqArray);
        for (uint i = 0; i < p.SqEntries; i++) {
            array[i] = i; // identity: SQE slot i is always SQ entry i
        }

        _cqHead = (uint*)(rings + p.CqHead);
        _cqTail = (uint*)(rings + p.CqTail);
        _cqMask = *(uint*)(rings + p.CqRingMask);
        _cqes = (Cqe*)(rings + p.CqCqes);
        _sqeTail = *_sqTail;
        _submitted = _sqeTail;

        // Registering the ring fd skips the fd table lookup on every io_uring_enter.
        long* update = stackalloc long[2];
        update[0] = uint.MaxValue; // offset -1: let the kernel pick a slot (the upper half is padding)
        update[1] = fd;
        if (syscall(SYS_io_uring_register, fd, IORING_REGISTER_RING_FDS, (long)update, 1, 0, 0) == 1) {
            _enterFd = (int)(uint)update[0];
            _enterFlags = IORING_ENTER_REGISTERED_RING;
        }
    }

    private void* Map(long size, long offset) {
        void* p = mmap(null, (nint)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, _fd, offset);
        if (p == (void*)-1) {
            throw new InvalidOperationException($"io_uring mmap failed: errno {Marshal.GetLastPInvokeError()}");
        }

        return p;
    }

    /// <summary>Registers a sparse table of <paramref name="slots"/> direct descriptors. Throws on failure.</summary>
    public void RegisterSparseFiles(uint slots) {
        ulong* reg = stackalloc ulong[4]; // io_uring_rsrc_register: nr, flags, resv2, data, tags
        reg[0] = slots | ((ulong)IORING_RSRC_REGISTER_SPARSE << 32);
        reg[1] = 0;
        reg[2] = 0;
        reg[3] = 0;
        if (syscall(SYS_io_uring_register, _fd, IORING_REGISTER_FILES2, (long)reg, 32, 0, 0) < 0) {
            throw new InvalidOperationException($"io_uring sparse file table ({slots}) failed: errno {Marshal.GetLastPInvokeError()}");
        }
    }

    /// <summary>Registers a provided-buffer ring. Throws on failure.</summary>
    public void RegisterBufRing(void* ringAddr, uint entries, ushort group) {
        ulong* reg = stackalloc ulong[5]; // io_uring_buf_reg (40 bytes)
        reg[0] = (ulong)ringAddr;
        reg[1] = entries | ((ulong)group << 32);
        reg[2] = 0;
        reg[3] = 0;
        reg[4] = 0;
        if (syscall(SYS_io_uring_register, _fd, IORING_REGISTER_PBUF_RING, (long)reg, 1, 0, 0) < 0) {
            throw new InvalidOperationException($"io_uring buffer ring failed: errno {Marshal.GetLastPInvokeError()}");
        }
    }

    /// <summary>
    /// The next free SQE, zeroed. When the SQ is full the pending entries are submitted first;
    /// returns null only if the kernel still takes none of them.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Sqe* Get() {
        if (_sqeTail - Volatile.Read(ref *_sqHead) >= _sqEntries) {
            Submit();
            if (_sqeTail - Volatile.Read(ref *_sqHead) >= _sqEntries) {
                return null;
            }
        }
        Sqe* s = &_sqes[_sqeTail & _sqMask];
        *s = default;
        _sqeTail++;
        return s;
    }

    /// <summary>SQEs filled since the last enter.</summary>
    public uint Pending => _sqeTail - _submitted;

    /// <summary>Submits pending SQEs without waiting, but runs deferred completions so they can be reaped.</summary>
    public void Submit() => Enter(0, null);

    /// <summary>
    /// Submits pending SQEs and waits until at least <paramref name="minComplete"/> completions are ready
    /// or <paramref name="ts"/> elapses. ETIME, EINTR, EAGAIN and EBUSY are not errors here.
    /// </summary>
    public void Enter(uint minComplete, Timespec* ts) {
        uint toSubmit = _sqeTail - _submitted;
        Volatile.Write(ref *_sqTail, _sqeTail);
        uint flags = IORING_ENTER_GETEVENTS | _enterFlags;
        long arg = 0, argSize = 0;
        GeteventsArg ga = default;
        if (ts != null) {
            ga.Ts = (ulong)ts;
            arg = (long)&ga;
            argSize = sizeof(GeteventsArg);
            flags |= IORING_ENTER_EXT_ARG;
        }
        long rc = syscall(SYS_io_uring_enter, _enterFd, toSubmit, minComplete, flags, arg, argSize);
        if (rc >= 0) {
            // A partial submit leaves the rest in the SQ; they are counted again on the next enter.
            _submitted += (uint)rc;
            return;
        }
        int errno = Marshal.GetLastPInvokeError();
        if (errno is not (ETIME or EINTR or EAGAIN or EBUSY)) {
            Log($"io_uring_enter failed: errno {errno}");
        }
    }

    /// <summary>Copies up to <paramref name="max"/> ready completions into <paramref name="dst"/> and frees their CQ slots.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public int Reap(Cqe* dst, int max) {
        uint head = *_cqHead;
        uint tail = Volatile.Read(ref *_cqTail);
        int n = 0;
        while (head != tail && n < max) {
            dst[n++] = _cqes[head & _cqMask];
            head++;
        }
        Volatile.Write(ref *_cqHead, head);
        return n;
    }
}

/// <summary>
/// Receive buffers that the kernel picks from for each multishot receive, so idle connections do
/// not hold receive buffers. A buffer is returned after its bytes have been parsed.
///
/// The preferred path is a provided-buffer ring (IORING_REGISTER_PBUF_RING): returning a buffer is
/// a memory write. Some kernels refuse that registration; on the 6.8 kernel used during testing,
/// the fallback is classic provided buffers (IORING_OP_PROVIDE_BUFFERS), where buffers are handed
/// back with SQEs, one per consecutive run of ids.
/// </summary>
internal sealed unsafe class BufRing {
    private const byte OP_PROVIDE_BUFFERS = 31;
    private const ulong ProvideUserData = 0xFF; // op 255: the workers ignore its (failure-only) completions

    public readonly int Count;
    public readonly int Size;
    public readonly byte* Slab;
    private readonly Ring _uring;
    private readonly ushort _group;
    private readonly bool _classic;
    private readonly byte* _ring;
    private readonly int _mask;
    private ushort _tail;
    private int _added;
    private readonly int[] _returned; // classic mode: buffer ids waiting to be provided again

    public BufRing(Ring ring, int count, int size, ushort group) {
        Count = count;
        Size = size;
        _uring = ring;
        _group = group;
        _mask = count - 1;
        Slab = (byte*)mmap(null, (nint)count * size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        _ring = (byte*)mmap(null, count * 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (_ring == (byte*)-1 || Slab == (byte*)-1) {
            throw new OutOfMemoryException("buffer ring");
        }

        try {
            ring.RegisterBufRing(_ring, (uint)count, group);
        } catch (InvalidOperationException ex) {
            Log($"{ex.Message}; using classic provided buffers");
            munmap(_ring, count * 16);
            _ring = null;
            _classic = true;
            _returned = new int[count];
            Provide(0, count);
            return;
        }
        _returned = [];
        for (int i = 0; i < count; i++) {
            Return(i);
        }
        Publish();
    }

    /// <summary>The bytes of buffer <paramref name="bid"/>.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public byte* Buffer(int bid) => Slab + ((nint)bid * Size);

    /// <summary>Queues buffer <paramref name="bid"/> to be handed back to the kernel on the next Publish.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Return(int bid) {
        if (_classic) {
            _returned[_added++] = bid;
            return;
        }
        byte* e = _ring + (((_tail + _added) & _mask) * 16);
        *(ulong*)e = (ulong)Buffer(bid);
        *(uint*)(e + 8) = (uint)Size;
        *(ushort*)(e + 12) = (ushort)bid;   // bytes 14-15 of entry 0 are the ring tail: never touched here
        _added++;
    }

    /// <summary>Buffers returned since the last Publish.</summary>
    public int Added => _added;

    /// <summary>Hands the returned buffers back to the kernel.</summary>
    public void Publish() {
        if (_added == 0) {
            return;
        }

        if (_classic) {
            Span<int> ids = _returned.AsSpan(0, _added);
            ids.Sort();
            int start = 0;
            for (int i = 1; i <= ids.Length; i++) {
                if (i == ids.Length || ids[i] != ids[i - 1] + 1) {
                    Provide(ids[start], i - start);
                    start = i;
                }
            }
            _added = 0;
            return;
        }

        _tail = (ushort)(_tail + _added);
        _added = 0;
        Volatile.Write(ref *(ushort*)(_ring + 14), _tail);
    }

    /// <summary>Classic mode: provides buffers [first, first + n) to the group with one SQE.</summary>
    private void Provide(int first, int n) {
        Sqe* s = _uring.Get();
        while (s == null) {
            _uring.Submit();
            s = _uring.Get();
        }
        s->Opcode = OP_PROVIDE_BUFFERS;
        s->Flags = Ring.IOSQE_CQE_SKIP_SUCCESS;
        s->Fd = n;                     // number of buffers
        s->Addr = (ulong)Buffer(first);
        s->Len = (uint)Size;           // size of each
        s->Off = (ulong)first;         // first buffer id
        s->BufGroup = _group;
        s->UserData = ProvideUserData;
    }
}
