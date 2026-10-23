using System.Runtime.InteropServices;

namespace Feed;

/// <summary>
/// Direct calls into libc (sockets, epoll) and into the SQLite library that build.sh compiles
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

    public const int EPOLL_CTL_ADD = 1;
    public const int EPOLL_CTL_DEL = 2;
    public const int EPOLL_CTL_MOD = 3;
    public const uint EPOLLIN = 0x001;
    public const uint EPOLLOUT = 0x004;
    public const uint EPOLLERR = 0x008;
    public const uint EPOLLHUP = 0x010;

    public const int EINTR = 4;
    public const int EAGAIN = 11;
    public const int ENFILE = 23;
    public const int EMFILE = 24;

    /// <summary>The kernel's epoll_event, which is packed on x86-64 (12 bytes).</summary>
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct EpollEvent {
        public uint Events;
        public ulong Data;
    }

    /// <summary>The kernel's sockaddr_in (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SockAddrIn {
        public ushort Family;
        public ushort Port;   // network byte order
        public uint Addr;     // network byte order
        public ulong Zero;
    }

    // libc: the errno of a failed call is read with Marshal.GetLastPInvokeError.
    [DllImport("libc", SetLastError = true)] public static extern int socket(int domain, int type, int protocol);
    [DllImport("libc", SetLastError = true)] public static extern int setsockopt(int fd, int level, int name, int* value, uint len);
    [DllImport("libc", SetLastError = true)] public static extern int bind(int fd, SockAddrIn* addr, uint len);
    [DllImport("libc", SetLastError = true)] public static extern int listen(int fd, int backlog);
    [DllImport("libc", SetLastError = true)] public static extern int accept4(int fd, void* addr, void* len, int flags);
    [DllImport("libc", SetLastError = true)] public static extern nint read(int fd, byte* buf, nint count);
    [DllImport("libc", SetLastError = true)] public static extern nint send(int fd, byte* buf, nint count, int flags);
    [DllImport("libc", SetLastError = true)] public static extern int close(int fd);
    [DllImport("libc", SetLastError = true)] public static extern int epoll_create1(int flags);
    [DllImport("libc", SetLastError = true)] public static extern int epoll_ctl(int epfd, int op, int fd, EpollEvent* ev);
    [DllImport("libc", SetLastError = true)] public static extern int epoll_wait(int epfd, EpollEvent* events, int max, int timeout);

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
