/// libc calls the server makes itself (io_uring has no .NET API).
module Native

#nowarn "9"

open System.Runtime.InteropServices

[<Literal>]
let L = "libc.so.6"

[<DllImport(L, SetLastError = true)>]
extern int64 syscall(int64 n, int64 a1, int64 a2, int64 a3, int64 a4, int64 a5, int64 a6)

[<DllImport(L, SetLastError = true)>]
extern nativeint mmap(nativeint addr, unativeint len, int prot, int flags, int fd, int64 off)

[<DllImport(L, SetLastError = true)>]
extern int close(int fd)

[<DllImport(L, SetLastError = true)>]
extern int socket(int domain, int typ, int protocol)

[<DllImport(L, SetLastError = true)>]
extern int setsockopt(int fd, int level, int name, int& value, int len)

[<DllImport(L, SetLastError = true)>]
extern int bind(int fd, nativeint addr, int len)

[<DllImport(L, SetLastError = true)>]
extern int listen(int fd, int backlog)

[<DllImport(L, SetLastError = true)>]
extern int getrlimit(int resource, uint64[] rlim)

[<DllImport(L, SetLastError = true)>]
extern int eventfd(uint32 initval, int flags)

[<DllImport(L, SetLastError = true)>]
extern nativeint write(int fd, nativeint buf, unativeint count)

[<DllImport(L, SetLastError = true)>]
extern int setrlimit(int resource, uint64[] rlim)

[<DllImport(L, SetLastError = true)>]
extern int mlock(nativeint addr, unativeint len)

[<DllImport(L, SetLastError = true)>]
extern int madvise(nativeint addr, unativeint len, int advice)

[<DllImport(L, SetLastError = true)>]
extern int setpriority(int which, int who, int prio)

[<DllImport(L)>]
extern int gettid()

[<DllImport(L)>]
extern int usleep(uint32 usec)

[<DllImport(L, SetLastError = true)>]
extern int ``open``(byte[] path, int flags)

[<DllImport(L)>]
extern int posix_fadvise(int fd, int64 offset, int64 len, int advice)

[<DllImport(L)>]
extern int clock_gettime(int clk, int64[] ts)

/// this thread's CPU time, in microseconds
let threadCpuUs () =
    let ts = Array.zeroCreate<int64> 2
    clock_gettime (3, ts) |> ignore // CLOCK_THREAD_CPUTIME_ID
    ts.[0] * 1000000L + ts.[1] / 1000L

let errno () = Marshal.GetLastPInvokeError()

