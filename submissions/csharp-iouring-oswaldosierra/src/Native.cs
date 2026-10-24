using System.Runtime.InteropServices;

namespace Feed;

/// <summary>
/// Direct calls into libc (sockets, mmap, raw io_uring syscalls) and into the SQLite library that
/// build.sh compiles and links statically. All signatures are blittable, so nothing is marshalled.
/// Calls that return in well under a microsecond and never block skip the GC transition.
/// Constant values come from the Linux x86-64 headers and sqlite3.h.
/// </summary>
internal static unsafe class Native {
    public const int AF_INET = 2;
    public const int SOCK_STREAM = 1;
    public const int SOCK_CLOEXEC = 0x80000;
    public const int SOL_SOCKET = 1;
    public const int SO_REUSEADDR = 2;
    public const int IPPROTO_TCP = 6;
    public const int TCP_NODELAY = 1;
    public const int MSG_NOSIGNAL = 0x4000;
    public const int SHUT_RDWR = 2;

    public const int PROT_READ = 1;
    public const int PROT_WRITE = 2;
    public const int MAP_SHARED = 0x01;
    public const int MAP_PRIVATE = 0x02;
    public const int MAP_ANONYMOUS = 0x20;
    public const int MAP_POPULATE = 0x8000;

    public const int RLIMIT_NOFILE = 7;
    public const int O_RDONLY = 0;
    public const int O_CLOEXEC = 0x80000;
    public const int EFD_NONBLOCK = 0x800;
    public const int EFD_CLOEXEC = 0x80000;

    public const int EINTR = 4;
    public const int EAGAIN = 11;
    public const int ENOMEM = 12;
    public const int EBUSY = 16;
    public const int EINVAL = 22;
    public const int ENFILE = 23;
    public const int EMFILE = 24;
    public const int ENOBUFS = 105;
    public const int ECANCELED = 125;
    public const int ETIME = 62;

    public const long SYS_io_uring_setup = 425;
    public const long SYS_io_uring_enter = 426;
    public const long SYS_io_uring_register = 427;

    /// <summary>The kernel's sockaddr_in (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SockAddrIn {
        public ushort Family;
        public ushort Port;   // network byte order
        public uint Addr;     // network byte order
        public ulong Zero;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RLimit {
        public ulong Cur;
        public ulong Max;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Timespec {
        public long Sec;
        public long Nsec;
    }

    // libc: the errno of a failed call is read with Marshal.GetLastPInvokeError.
    [DllImport("libc", SetLastError = true)] public static extern int socket(int domain, int type, int protocol);
    [DllImport("libc", SetLastError = true)] public static extern int setsockopt(int fd, int level, int name, int* value, uint len);
    [DllImport("libc", SetLastError = true)] public static extern int bind(int fd, SockAddrIn* addr, uint len);
    [DllImport("libc", SetLastError = true)] public static extern int listen(int fd, int backlog);
    [DllImport("libc", SetLastError = true)] public static extern int close(int fd);
    [DllImport("libc", SetLastError = true)] public static extern int open(byte* path, int flags, int mode);
    [DllImport("libc", SetLastError = true)] public static extern nint pread(int fd, byte* buf, nint count, long offset);
    [DllImport("libc", SetLastError = true)] public static extern nint write(int fd, byte* buf, nint count);
    [DllImport("libc", SetLastError = true)] public static extern void* mmap(void* addr, nint length, int prot, int flags, int fd, long offset);
    [DllImport("libc", SetLastError = true)] public static extern int munmap(void* addr, nint length);
    [DllImport("libc", SetLastError = true)] public static extern int getrlimit(int resource, RLimit* rlim);
    [DllImport("libc", SetLastError = true)] public static extern int setrlimit(int resource, RLimit* rlim);
    [DllImport("libc", SetLastError = true)] public static extern int eventfd(uint initval, int flags);
    [DllImport("libc", SetLastError = true)] public static extern int inet_pton(int af, byte* src, void* dst);

    /// <summary>glibc's syscall(): returns -1 and sets errno on failure. Used for the io_uring calls glibc has no wrapper for.</summary>
    [DllImport("libc", SetLastError = true)] public static extern long syscall(long number, long a1, long a2, long a3, long a4, long a5, long a6);

    /// <summary>Writes a diagnostic straight to fd 2. Never opens anything, so it works when the process is out of descriptors.</summary>
    public static void Log(string message) {
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(message + "\n");
        fixed (byte* p = bytes) {
            write(2, p, bytes.Length);
        }
    }

    // SQLite C API.
    public const int SQLITE_OK = 0;
    public const int SQLITE_ROW = 100;
    public const int SQLITE_DONE = 101;
    public const int SQLITE_OPEN_READWRITE = 0x2;
    public const int SQLITE_OPEN_NOMUTEX = 0x8000;
    public const int SQLITE_CHECKPOINT_PASSIVE = 0;
    public const int SQLITE_CHECKPOINT_RESTART = 2;
    public const uint SQLITE_PREPARE_PERSISTENT = 0x1;

    [DllImport("sqlite3")] public static extern int sqlite3_initialize();
    [DllImport("sqlite3")] public static extern int sqlite3_open_v2(byte* filename, nint* db, int flags, byte* vfs);
    [DllImport("sqlite3")] public static extern int sqlite3_exec(nint db, byte* sql, nint callback, nint arg, byte** errmsg);
    [DllImport("sqlite3")] public static extern int sqlite3_prepare_v3(nint db, byte* sql, int bytes, uint flags, nint* stmt, byte** tail);
    [DllImport("sqlite3")] public static extern byte* sqlite3_errmsg(nint db);
    [DllImport("sqlite3")] public static extern nint sqlite3_wal_hook(nint db, delegate* unmanaged<nint, nint, byte*, int, int> callback, nint arg);
    [DllImport("sqlite3")] public static extern int sqlite3_wal_checkpoint_v2(nint db, byte* name, int mode, int* log, int* checkpointed);

    /// <summary>sqlite3_step for statements that may write to disk (COMMIT): keeps the GC transition.</summary>
    [DllImport("sqlite3", EntryPoint = "sqlite3_step")] public static extern int sqlite3_step_blocking(nint stmt);

    // Reads and in-transaction writes touch only the page cache and the mmap: microseconds, never blocking.
    [DllImport("sqlite3"), SuppressGCTransition] public static extern int sqlite3_step(nint stmt);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern int sqlite3_reset(nint stmt);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern int sqlite3_bind_int64(nint stmt, int index, long value);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern int sqlite3_bind_text(nint stmt, int index, byte* text, int bytes, nint destructor);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern long sqlite3_column_int64(nint stmt, int column);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern byte* sqlite3_column_text(nint stmt, int column);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern int sqlite3_column_bytes(nint stmt, int column);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern int sqlite3_changes(nint db);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern long sqlite3_last_insert_rowid(nint db);
    [DllImport("sqlite3"), SuppressGCTransition] public static extern int sqlite3_get_autocommit(nint db);
}
