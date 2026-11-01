using System.Runtime.InteropServices;

namespace Feed;

/// <summary>
/// Direct calls into libc (sockets, io_uring) and into the SQLite library that build.sh compiles
/// and links statically. All signatures are blittable, so arguments are not marshalled.
/// Constant values come from the Linux x86-64 headers and sqlite3.h.
/// </summary>
internal static unsafe class Native {
    public const int AF_INET = 2;
    public const int SOCK_STREAM = 1;
    public const int SOCK_NONBLOCK = 0x800;
    public const int SOCK_CLOEXEC = 0x80000;
    public const int SOL_SOCKET = 1;
    public const int SO_REUSEADDR = 2;
    public const int IPPROTO_TCP = 6;
    public const int TCP_NODELAY = 1;
    public const int MSG_NOSIGNAL = 0x4000;

    public const int EINTR = 4;
    public const int EAGAIN = 11;
    public const int ENOMEM = 12;
    public const int EBUSY = 16;
    public const int EINVAL = 22;
    public const int ETIME = 62;
    public const int ENOBUFS = 105;
    public const int ECANCELED = 125;

    public const int RLIMIT_NOFILE = 7;
    public const int PROT_READ_WRITE = 3;
    public const int MAP_SHARED = 0x01;
    public const int MAP_PRIVATE = 0x02;
    public const int MAP_ANONYMOUS = 0x20;
    public const int MAP_POPULATE = 0x8000;

    // io_uring, from linux/io_uring.h (kernel 6.1 and later). System call numbers are x86-64.
    public const long SYS_io_uring_setup = 425;
    public const long SYS_io_uring_enter = 426;
    public const long SYS_io_uring_register = 427;
    public const uint IORING_SETUP_CQSIZE = 1u << 3;
    public const uint IORING_SETUP_SUBMIT_ALL = 1u << 7;
    public const uint IORING_SETUP_COOP_TASKRUN = 1u << 8;
    public const uint IORING_SETUP_SINGLE_ISSUER = 1u << 12;
    public const uint IORING_SETUP_DEFER_TASKRUN = 1u << 13;
    public const long IORING_OFF_SQ_RING = 0;
    public const long IORING_OFF_CQ_RING = 0x8000000;
    public const long IORING_OFF_SQES = 0x10000000;
    public const int IORING_REGISTER_FILES2 = 13;
    public const int IORING_REGISTER_PBUF_RING = 22;
    public const uint IORING_RSRC_REGISTER_SPARSE = 1;
    public const int IORING_ENTER_GETEVENTS = 1;
    public const int IORING_ENTER_EXT_ARG = 1 << 3;
    public const uint IORING_SQ_CQ_OVERFLOW = 1u << 1;
    public const byte IORING_OP_ACCEPT = 13;
    public const byte IORING_OP_ASYNC_CANCEL = 14;
    public const byte IORING_OP_CLOSE = 19;
    public const byte IORING_OP_SEND = 26;
    public const byte IORING_OP_RECV = 27;
    public const byte IORING_OP_PROVIDE_BUFFERS = 31;
    public const byte IORING_OP_SHUTDOWN = 34;
    public const byte IOSQE_FIXED_FILE = 1;
    public const byte IOSQE_BUFFER_SELECT = 1 << 5;
    public const byte IOSQE_CQE_SKIP_SUCCESS = 1 << 6;
    public const ushort IORING_ACCEPT_MULTISHOT = 1;
    public const ushort IORING_RECV_MULTISHOT = 2;
    public const uint IORING_FILE_INDEX_ALLOC = 0xFFFFFFFF;
    public const uint IORING_CQE_F_BUFFER = 1;
    public const uint IORING_CQE_F_MORE = 2;
    public const uint SHUT_RDWR = 2;

    /// <summary>The kernel's struct __kernel_timespec (16 bytes): a relative wait when passed to io_uring_enter.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct KernelTimespec {
        public long Sec, Nsec;
    }

    /// <summary>io_uring_getevents_arg (24 bytes), the extended argument of io_uring_enter with EXT_ARG.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct GetEventsArg {
        public ulong Sigmask;
        public uint SigmaskSz, MinWaitUsec;
        public ulong Ts;   // address of a KernelTimespec
    }

    /// <summary>The kernel's sockaddr_in (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SockAddrIn {
        public ushort Family;
        public ushort Port;   // network byte order
        public uint Addr;     // network byte order
        public ulong Zero;
    }

    /// <summary>The kernel's struct rlimit.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct RLimit {
        public ulong Cur;
        public ulong Max;
    }

    /// <summary>io_sqring_offsets (40 bytes): where the fields of the submission ring sit in its mapping.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SqOffsets {
        public uint Head, Tail, RingMask, RingEntries, Flags, Dropped, Array, Resv1;
        public ulong UserAddr;
    }

    /// <summary>io_cqring_offsets (40 bytes): where the fields of the completion ring sit in its mapping.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct CqOffsets {
        public uint Head, Tail, RingMask, RingEntries, Overflow, Cqes, Flags, Resv1;
        public ulong UserAddr;
    }

    /// <summary>io_uring_params (120 bytes), the in/out argument of io_uring_setup.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct UringParams {
        public uint SqEntries, CqEntries, Flags, SqThreadCpu, SqThreadIdle, Features, WqFd, Resv0, Resv1, Resv2;
        public SqOffsets Sq;
        public CqOffsets Cq;
    }

    /// <summary>io_uring_sqe (64 bytes), a submission queue entry.</summary>
    [StructLayout(LayoutKind.Explicit, Size = 64)]
    public struct Sqe {
        [FieldOffset(0)] public byte Opcode;
        [FieldOffset(1)] public byte Flags;
        [FieldOffset(2)] public ushort Ioprio;
        [FieldOffset(4)] public int Fd;
        [FieldOffset(8)] public ulong Off;
        [FieldOffset(16)] public ulong Addr;
        [FieldOffset(24)] public uint Len;
        [FieldOffset(28)] public uint OpFlags;   // msg_flags, accept_flags and so on
        [FieldOffset(32)] public ulong UserData;
        [FieldOffset(40)] public ushort BufGroup;
        [FieldOffset(42)] public ushort Personality;
        [FieldOffset(44)] public uint FileIndex;
        [FieldOffset(48)] public ulong Addr3;
    }

    /// <summary>io_uring_cqe (16 bytes), a completion queue entry.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Cqe {
        public ulong UserData;
        public int Res;
        public uint Flags;
    }

    /// <summary>io_uring_rsrc_register (32 bytes), the argument of IORING_REGISTER_FILES2.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct RsrcRegister {
        public uint Nr, Flags;
        public ulong Resv2, Data, Tags;
    }

    /// <summary>io_uring_buf_reg (40 bytes), the argument of IORING_REGISTER_PBUF_RING.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BufReg {
        public ulong RingAddr;
        public uint RingEntries;
        public ushort Bgid, Flags;
        public ulong Resv0, Resv1, Resv2;
    }

    /// <summary>io_uring_buf (16 bytes), one entry of a provided buffer ring.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BufRingEntry {
        public ulong Addr;
        public uint Len;
        public ushort Bid, Resv;
    }

    // libc: the errno of a failed call is read with Marshal.GetLastPInvokeError.
    [DllImport("libc", SetLastError = true)] public static extern int socket(int domain, int type, int protocol);
    [DllImport("libc", SetLastError = true)] public static extern int setsockopt(int fd, int level, int name, int* value, uint len);
    [DllImport("libc", SetLastError = true)] public static extern int bind(int fd, SockAddrIn* addr, uint len);
    [DllImport("libc", SetLastError = true)] public static extern int listen(int fd, int backlog);
    [DllImport("libc", SetLastError = true)] public static extern int getrlimit(int resource, RLimit* limit);
    [DllImport("libc", SetLastError = true)] public static extern int setrlimit(int resource, RLimit* limit);
    [DllImport("libc", SetLastError = true)] public static extern void* mmap(void* addr, nuint length, int prot, int flags, int fd, long offset);

    // syscall(2) with a fixed argument count, for the io_uring system calls (no glibc wrapper exists).
    [DllImport("libc", EntryPoint = "syscall", SetLastError = true)] public static extern long syscall2(long n, nint a, nint b);
    [DllImport("libc", EntryPoint = "syscall", SetLastError = true)] public static extern long syscall4(long n, nint a, nint b, nint c, nint d);
    [DllImport("libc", EntryPoint = "syscall", SetLastError = true)] public static extern long syscall6(long n, nint a, nint b, nint c, nint d, nint e, nint f);

    // SQLite C API.
    public const int SQLITE_OK = 0;
    public const int SQLITE_ROW = 100;
    public const int SQLITE_DONE = 101;
    public const int SQLITE_OPEN_READWRITE = 0x2;
    public const int SQLITE_OPEN_NOMUTEX = 0x8000;
    public const int SQLITE_CHECKPOINT_PASSIVE = 0;
    public const uint SQLITE_PREPARE_PERSISTENT = 0x1;

    [DllImport("sqlite3")] public static extern int sqlite3_initialize();
    [DllImport("sqlite3")] public static extern int sqlite3_open_v2(byte* filename, nint* db, int flags, byte* vfs);
    [DllImport("sqlite3")] public static extern int sqlite3_exec(nint db, byte* sql, nint callback, nint arg, byte** errmsg);
    [DllImport("sqlite3")] public static extern int sqlite3_prepare_v3(nint db, byte* sql, int bytes, uint flags, nint* stmt, byte** tail);
    [DllImport("sqlite3")] public static extern int sqlite3_step(nint stmt);
    [DllImport("sqlite3")] public static extern int sqlite3_reset(nint stmt);
    [DllImport("sqlite3")] public static extern int sqlite3_bind_int64(nint stmt, int index, long value);
    [DllImport("sqlite3")] public static extern int sqlite3_bind_text(nint stmt, int index, byte* text, int bytes, nint destructor);
    [DllImport("sqlite3")] public static extern long sqlite3_column_int64(nint stmt, int column);
    [DllImport("sqlite3")] public static extern byte* sqlite3_column_text(nint stmt, int column);
    [DllImport("sqlite3")] public static extern int sqlite3_column_bytes(nint stmt, int column);
    [DllImport("sqlite3")] public static extern int sqlite3_changes(nint db);
    [DllImport("sqlite3")] public static extern int sqlite3_get_autocommit(nint db);
    [DllImport("sqlite3")] public static extern byte* sqlite3_errmsg(nint db);
    [DllImport("sqlite3")] public static extern nint sqlite3_wal_hook(nint db, delegate* unmanaged<nint, nint, byte*, int, int> callback, nint arg);
    [DllImport("sqlite3")] public static extern int sqlite3_wal_checkpoint_v2(nint db, byte* name, int mode, int* log, int* checkpointed);
}
