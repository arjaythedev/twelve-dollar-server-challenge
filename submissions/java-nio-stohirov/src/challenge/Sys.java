package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
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

/** Linux sockets and epoll through FFM: the server's I/O without SocketChannel or Selector. Single-threaded. */
final class Sys {
    // Linux values, the same on x86_64 and aarch64.
    static final int AF_INET = 2, AF_INET6 = 10, SOCK_STREAM = 1, SOCK_NONBLOCK = 0x800, SOCK_CLOEXEC = 0x80000;
    static final int SOL_SOCKET = 1, SO_REUSEADDR = 2, SO_LINGER = 13, IPPROTO_TCP = 6, TCP_NODELAY = 1;
    static final int EPOLL_CLOEXEC = 0x80000, EPOLL_CTL_ADD = 1, EPOLL_CTL_MOD = 3;
    static final int EPOLLIN = 0x1, EPOLLOUT = 0x4, EPOLLERR = 0x8, EPOLLHUP = 0x10;
    static final int EINTR = 4, EAGAIN = 11, ENFILE = 23, EMFILE = 24, ECONNABORTED = 103;

    // struct epoll_event is packed (12 bytes) on x86_64 and naturally aligned (16 bytes) elsewhere.
    private static final boolean X86 = switch (System.getProperty("os.arch")) {
        case "amd64", "x86_64" -> true;
        default -> false;
    };
    static final int EVENT_SIZE = X86 ? 12 : 16;
    private static final int EVENT_DATA = X86 ? 4 : 8;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Linker.Option CRITICAL = Linker.Option.critical(false);
    private static final Linker.Option ERRNO = Linker.Option.captureCallState("errno");
    private static final MemoryLayout STATE_LAYOUT = Linker.Option.captureStateLayout();
    private static final VarHandle ERRNO_VALUE =
            STATE_LAYOUT.varHandle(MemoryLayout.PathElement.groupElement("errno"));

    private static MethodHandle fn(String name, FunctionDescriptor fd, Linker.Option... options) {
        MemorySegment f = LINKER.defaultLookup().find(name).orElseThrow(() -> new UnsatisfiedLinkError(name));
        return LINKER.downcallHandle(f, fd, options);
    }

    private static final MethodHandle SOCKET =
            fn("socket", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT), ERRNO);
    private static final MethodHandle SETSOCKOPT = fn("setsockopt",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
    private static final MethodHandle BIND =
            fn("bind", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), ERRNO);
    private static final MethodHandle LISTEN = fn("listen", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT), ERRNO);
    private static final MethodHandle ACCEPT4 = fn("accept4",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT), CRITICAL, ERRNO);
    private static final MethodHandle EPOLL_CREATE1 =
            fn("epoll_create1", FunctionDescriptor.of(JAVA_INT, JAVA_INT), ERRNO);
    private static final MethodHandle EPOLL_CTL =
            fn("epoll_ctl", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS), CRITICAL);
    // Blocks, so not critical: a GC must be able to run while the loop waits.
    private static final MethodHandle EPOLL_WAIT =
            fn("epoll_wait", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), ERRNO);
    private static final MethodHandle READ =
            fn("read", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG), CRITICAL, ERRNO);
    private static final MethodHandle WRITE =
            fn("write", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG), CRITICAL, ERRNO);
    private static final MethodHandle CLOSE = fn("close", FunctionDescriptor.of(JAVA_INT, JAVA_INT), CRITICAL);

    private static final MemorySegment STATE = Arena.global().allocate(STATE_LAYOUT);
    private static final MemorySegment SCRATCH = Arena.global().allocate(64, 8);

    private Sys() {}

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) return r;
        if (t instanceof Error e) throw e;
        return new IllegalStateException(t);
    }

    /** errno of the last failed call made through a handle that captures it. */
    static int errno() { return (int) ERRNO_VALUE.get(STATE, 0L); }

    static int listen(InetAddress host, int port, int backlog) {
        boolean v6 = host instanceof Inet6Address;
        int fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) throw new IllegalStateException("socket: errno " + errno());
        setInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
        MemorySegment sa = SCRATCH.asSlice(0, v6 ? 28 : 16).fill((byte) 0);
        sa.set(JAVA_SHORT, 0, (short) (v6 ? AF_INET6 : AF_INET));
        sa.set(JAVA_SHORT.withOrder(ByteOrder.BIG_ENDIAN), 2, (short) port);
        byte[] addr = host.getAddress();
        MemorySegment.copy(addr, 0, sa, JAVA_BYTE, host instanceof Inet4Address ? 4 : 8, addr.length);
        try {
            if ((int) BIND.invokeExact(STATE, fd, sa, (int) sa.byteSize()) != 0) {
                throw new IllegalStateException("bind " + host.getHostAddress() + ":" + port + ": errno " + errno());
            }
            if ((int) LISTEN.invokeExact(STATE, fd, backlog) != 0) throw new IllegalStateException("listen: errno " + errno());
        } catch (Throwable t) { throw rethrow(t); }
        return fd;
    }

    private static int socket(int domain, int type) {
        try { return (int) SOCKET.invokeExact(STATE, domain, type, 0); } catch (Throwable t) { throw rethrow(t); }
    }

    static int setInt(int fd, int level, int name, int value) {
        SCRATCH.set(JAVA_INT, 32, value);
        try {
            return (int) SETSOCKOPT.invokeExact(fd, level, name, SCRATCH.asSlice(32, 4), 4);
        } catch (Throwable t) { throw rethrow(t); }
    }

    /** SO_LINGER {on, 0 s}: close() then resets the connection instead of sending FIN, leaving no TIME_WAIT. */
    static int lingerZero(int fd) {
        SCRATCH.set(JAVA_INT, 40, 1);
        SCRATCH.set(JAVA_INT, 44, 0);
        try {
            return (int) SETSOCKOPT.invokeExact(fd, SOL_SOCKET, SO_LINGER, SCRATCH.asSlice(40, 8), 8);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static int accept(int fd) {
        try {
            return (int) ACCEPT4.invokeExact(STATE, fd, MemorySegment.NULL, MemorySegment.NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static int epollCreate() {
        try {
            int ep = (int) EPOLL_CREATE1.invokeExact(STATE, EPOLL_CLOEXEC);
            if (ep < 0) throw new IllegalStateException("epoll_create1: errno " + errno());
            return ep;
        } catch (Throwable t) { throw rethrow(t); }
    }

    /** Registers or changes fd's events; the event's data is the fd itself. */
    static int epollCtl(int ep, int op, int fd, int events) {
        MemorySegment ev = SCRATCH.asSlice(0, EVENT_SIZE);
        ev.set(JAVA_INT_UNALIGNED, 0, events);
        ev.set(JAVA_LONG_UNALIGNED, EVENT_DATA, fd);
        try { return (int) EPOLL_CTL.invokeExact(ep, op, fd, ev); } catch (Throwable t) { throw rethrow(t); }
    }

    static int epollWait(int ep, MemorySegment events, int max, int timeoutMillis) {
        try {
            return (int) EPOLL_WAIT.invokeExact(STATE, ep, events, max, timeoutMillis);
        } catch (Throwable t) { throw rethrow(t); }
    }

    static int eventFd(MemorySegment events, int i) {
        return events.get(JAVA_INT_UNALIGNED, (long) i * EVENT_SIZE + EVENT_DATA);
    }

    static int eventFlags(MemorySegment events, int i) {
        return events.get(JAVA_INT_UNALIGNED, (long) i * EVENT_SIZE);
    }

    static long read(int fd, MemorySegment buf, long n) {
        try { return (long) READ.invokeExact(STATE, fd, buf, n); } catch (Throwable t) { throw rethrow(t); }
    }

    static long write(int fd, MemorySegment buf, long n) {
        try { return (long) WRITE.invokeExact(STATE, fd, buf, n); } catch (Throwable t) { throw rethrow(t); }
    }

    static void close(int fd) {
        try { int ignored = (int) CLOSE.invokeExact(fd); } catch (Throwable t) { throw rethrow(t); }
    }
}
