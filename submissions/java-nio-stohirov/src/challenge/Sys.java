package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.nio.ByteOrder;

/** The system calls the server makes through FFM: listen sockets, io_uring and thread priority. */
final class Sys {
    // Linux values, the same on x86_64 and aarch64.
    private static final int AF_INET = 2, AF_INET6 = 10, SOCK_STREAM = 1, SOCK_CLOEXEC = 0x80000;
    private static final int SOL_SOCKET = 1, SO_REUSEADDR = 2, SO_REUSEPORT = 15, IPPROTO_TCP = 6, TCP_NODELAY = 1;
    private static final long SYS_IO_URING_SETUP = 425, SYS_IO_URING_ENTER = 426, SYS_IO_URING_REGISTER = 427;
    private static final int PROT_READ_WRITE = 3, MAP_SHARED_POPULATE = 0x8001, PRIO_PROCESS = 0;
    static final int EINTR = 4, EAGAIN = 11, EBUSY = 16, EINVAL = 22, ENFILE = 23, ETIME = 62, ENOBUFS = 105;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Linker.Option ERRNO = Linker.Option.captureCallState("errno");
    private static final MemoryLayout STATE_LAYOUT = Linker.Option.captureStateLayout();
    private static final VarHandle ERRNO_VALUE =
            STATE_LAYOUT.varHandle(MemoryLayout.PathElement.groupElement("errno"));

    private static MethodHandle fn(String name, FunctionDescriptor fd, Linker.Option... options) {
        MemorySegment f = LINKER.defaultLookup().find(name).orElseThrow(() -> new UnsatisfiedLinkError(name));
        return LINKER.downcallHandle(f, fd, options);
    }

    // syscall(2) is variadic. Every call passes all six arguments, and the kernel ignores the extras.
    private static final MethodHandle SYSCALL = fn("syscall", FunctionDescriptor.of(JAVA_LONG, JAVA_LONG,
            JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG), ERRNO, Linker.Option.firstVariadicArg(1));
    private static final MethodHandle MMAP = fn("mmap",
            FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG), ERRNO);
    private static final MethodHandle SOCKET = fn("socket", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT), ERRNO);
    private static final MethodHandle SETSOCKOPT =
            fn("setsockopt", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), ERRNO);
    private static final MethodHandle BIND = fn("bind", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), ERRNO);
    private static final MethodHandle LISTEN = fn("listen", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT), ERRNO);
    private static final MethodHandle GETTID = fn("gettid", FunctionDescriptor.of(JAVA_INT));
    private static final MethodHandle SETPRIORITY =
            fn("setpriority", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT));

    // Only the event-loop thread uses these.
    private static final MemorySegment STATE = Arena.global().allocate(STATE_LAYOUT);
    private static final MemorySegment SCRATCH = Arena.global().allocate(64, 8);

    private Sys() {}

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) return r;
        if (t instanceof Error e) throw e;
        return new IllegalStateException(t);
    }

    /** errno of the last failed call. */
    static int errno() { return (int) ERRNO_VALUE.get(STATE, 0L); }

    private static long syscall(long nr, long a, long b, long c, long d, long e, long f) {
        try {
            return (long) SYSCALL.invokeExact(STATE, nr, a, b, c, d, e, f);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static long uringSetup(int entries, MemorySegment params) {
        return syscall(SYS_IO_URING_SETUP, entries, params.address(), 0, 0, 0, 0);
    }

    static long uringEnter(int fd, int toSubmit, int minComplete, int flags, long arg, long argSize) {
        return syscall(SYS_IO_URING_ENTER, fd, toSubmit, minComplete, flags, arg, argSize);
    }

    static long uringRegister(int fd, int opcode, MemorySegment arg, int nr) {
        return syscall(SYS_IO_URING_REGISTER, fd, opcode, arg.address(), nr, 0, 0);
    }

    static MemorySegment mapRing(int fd, long offset, long length) {
        try {
            long a = (long) MMAP.invokeExact(STATE, 0L, length, PROT_READ_WRITE, MAP_SHARED_POPULATE, fd, offset);
            if (a == -1L) throw new IllegalStateException("mmap: errno " + errno());
            return MemorySegment.ofAddress(a).reinterpret(length);
        } catch (Throwable t) { throw rethrow(t); }
    }

    /**
     * A blocking listener on a shared port: a non-blocking one would make io_uring fail an accept with
     * EAGAIN instead of waiting for a connection. Accepted sockets inherit TCP_NODELAY, which matters
     * because a direct descriptor has no fd to call setsockopt on.
     */
    static int listen(InetAddress host, int port, int backlog) {
        boolean v6 = host instanceof Inet6Address;
        try {
            int fd = (int) SOCKET.invokeExact(STATE, v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) throw new IllegalStateException("socket: errno " + errno());
            setInt(fd, SOL_SOCKET, SO_REUSEADDR);
            setInt(fd, SOL_SOCKET, SO_REUSEPORT);
            setInt(fd, IPPROTO_TCP, TCP_NODELAY);
            MemorySegment sa = SCRATCH.asSlice(0, v6 ? 28 : 16).fill((byte) 0);
            sa.set(JAVA_SHORT, 0, (short) (v6 ? AF_INET6 : AF_INET));
            sa.set(JAVA_SHORT.withOrder(ByteOrder.BIG_ENDIAN), 2, (short) port);
            byte[] addr = host.getAddress();
            MemorySegment.copy(addr, 0, sa, JAVA_BYTE, host instanceof Inet4Address ? 4 : 8, addr.length);
            if ((int) BIND.invokeExact(STATE, fd, sa, (int) sa.byteSize()) != 0) {
                throw new IllegalStateException("bind " + host.getHostAddress() + ":" + port + ": errno " + errno());
            }
            if ((int) LISTEN.invokeExact(STATE, fd, backlog) != 0) throw new IllegalStateException("listen: errno " + errno());
            return fd;
        } catch (Throwable t) { throw rethrow(t); }
    }

    private static void setInt(int fd, int level, int name) throws Throwable {
        MemorySegment one = SCRATCH.asSlice(32, 4);
        one.set(JAVA_INT, 0, 1);
        if ((int) SETSOCKOPT.invokeExact(STATE, fd, level, name, one, 4) != 0) {
            throw new IllegalStateException("setsockopt " + name + ": errno " + errno());
        }
    }

    /** Lowers the calling thread only: Linux nice values are per thread. */
    static void lowestPriority() {
        try {
            int tid = (int) GETTID.invokeExact();
            int ignored = (int) SETPRIORITY.invokeExact(PRIO_PROCESS, tid, 19);
        } catch (Throwable t) { throw rethrow(t); }
    }
}
