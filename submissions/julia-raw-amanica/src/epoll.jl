# Baseline transport: single-threaded, level-triggered epoll on raw fds (no libuv, no Tasks).
# Phase 4 replaces this with io_uring; it stays as the fallback.

const AF_INET = Cint(2)
const SOCK_STREAM = Cint(1)
const SOCK_NONBLOCK = Cint(0o4000)
const SOCK_CLOEXEC = Cint(0o2000000)
const SOL_SOCKET = Cint(1)
const SO_REUSEADDR = Cint(2)
const IPPROTO_TCP = Cint(6)
const TCP_NODELAY = Cint(1)
const MSG_NOSIGNAL = Cint(0x4000)
const EPOLLIN = UInt32(0x001)
const EPOLLOUT = UInt32(0x004)
const EPOLLERR = UInt32(0x008)
const EPOLLHUP = UInt32(0x010)
const EPOLL_CTL_ADD = Cint(1)
const EPOLL_CTL_MOD = Cint(3)
const EAGAIN = 11
const EINTR = 4

const MAXEV = 512
const RBUF_SIZE = 16384
const RBUF_MAX = 1 << 20

mutable struct Conn
    fd::Cint
    rbuf::Vector{UInt8}
    rlen::Int
    out::Buf
    woff::Int
    pollout::Bool
    closing::Bool
end

const EVSCRATCH = zeros(UInt8, 12)

function ep_ctl(ep::Cint, op::Cint, fd::Cint, events::UInt32)
    p = pointer(EVSCRATCH)
    GC.@preserve EVSCRATCH begin
        unsafe_store!(Ptr{UInt32}(p), events)
        unsafe_store!(Ptr{UInt64}(p + 4), UInt64(fd))
        ccall(:epoll_ctl, Cint, (Cint, Cint, Cint, Ptr{UInt8}), ep, op, fd, p)
    end
end

function setopt(fd::Cint, level::Cint, opt::Cint, v::Cint)
    r = Ref(v)
    ccall(:setsockopt, Cint, (Cint, Cint, Cint, Ref{Cint}, Cuint), fd, level, opt, r, 4)
end

function listen_tcp(host::String, port::Int)
    fd = ccall(:socket, Cint, (Cint, Cint, Cint), AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
    fd < 0 && error("socket: $(Libc.strerror(Libc.errno()))")
    setopt(fd, SOL_SOCKET, SO_REUSEADDR, Cint(1))
    setopt(fd, IPPROTO_TCP, TCP_NODELAY, Cint(1))         # inherited by accepted sockets
    addr = zeros(UInt8, 16)                               # struct sockaddr_in
    addr[1] = 0x02                                        # AF_INET, little endian
    addr[3] = (port >> 8) % UInt8
    addr[4] = port % UInt8
    ok = GC.@preserve addr ccall(:inet_pton, Cint, (Cint, Cstring, Ptr{UInt8}), AF_INET, host, pointer(addr, 5))
    ok == 1 || error("HOST must be an IPv4 address, got $host")
    ccall(:bind, Cint, (Cint, Ptr{UInt8}, Cuint), fd, addr, 16) == 0 ||
        error("bind $host:$port: $(Libc.strerror(Libc.errno()))")
    ccall(:listen, Cint, (Cint, Cint), fd, 65535) == 0 || error("listen: $(Libc.strerror(Libc.errno()))")
    fd
end

mutable struct Loop
    ep::Cint
    lfd::Cint
    conns::Vector{Union{Nothing,Conn}}
    pool::Vector{Conn}
end

function close_conn!(L::Loop, c::Conn)
    ccall(:close, Cint, (Cint,), c.fd)
    L.conns[c.fd+1] = nothing
    c.fd = -1; c.rlen = 0; c.woff = 0; c.pollout = false; c.closing = false
    reset!(c.out)
    length(c.rbuf) > RBUF_SIZE && resize!(c.rbuf, RBUF_SIZE)
    push!(L.pool, c)
    nothing
end

function accept_all!(L::Loop)
    while true
        fd = ccall(:accept4, Cint, (Cint, Ptr{Cvoid}, Ptr{Cvoid}, Cint), L.lfd, C_NULL, C_NULL,
            SOCK_NONBLOCK | SOCK_CLOEXEC)
        fd < 0 && return nothing                  # EAGAIN, or EMFILE etc.: try again on the next event
        if fd + 1 > length(L.conns)
            old = length(L.conns)
            resize!(L.conns, 2 * (fd + 1))
            fill!(view(L.conns, old+1:length(L.conns)), nothing)
        end
        c = isempty(L.pool) ? Conn(fd, Vector{UInt8}(undef, RBUF_SIZE), 0, Buf(8192), 0, false, false) : pop!(L.pool)
        c.fd = fd
        L.conns[fd+1] = c
        ep_ctl(L.ep, EPOLL_CTL_ADD, fd, EPOLLIN)
    end
end

# Sends pending output; returns false if the connection was closed.
function flush!(L::Loop, c::Conn)
    out = c.out
    while c.woff < out.len
        n = GC.@preserve out ccall(:send, Cssize_t, (Cint, Ptr{UInt8}, Csize_t, Cint),
            c.fd, pointer(out.data, c.woff + 1), out.len - c.woff, MSG_NOSIGNAL)
        if n > 0
            c.woff += n
        else
            e = Libc.errno()
            e == EINTR && continue
            if e == EAGAIN
                if !c.pollout
                    c.pollout = true
                    ep_ctl(L.ep, EPOLL_CTL_MOD, c.fd, EPOLLIN | EPOLLOUT)
                end
                return true
            end
            close_conn!(L, c)
            return false
        end
    end
    reset!(out)
    c.woff = 0
    if c.pollout
        c.pollout = false
        ep_ctl(L.ep, EPOLL_CTL_MOD, c.fd, EPOLLIN)
    end
    if c.closing
        close_conn!(L, c)
        return false
    end
    true
end

const REQ = Req()
const BADREQ = Buf(64)

function process!(c::Conn)
    d = c.rbuf
    pos = 1
    while pos <= c.rlen && !c.closing
        st = parse_req!(REQ, d, pos, c.rlen)
        st == 0 && break
        if st < 0
            err!(BADREQ, 400, "bad request")
            write_response!(c.out, 400, BADREQ, false)
            c.closing = true
            pos = c.rlen + 1
            break
        end
        handle!(c.out, d, REQ)
        pos = REQ.next
        REQ.keepalive || (c.closing = true)
    end
    if pos > 1
        rest = c.rlen - pos + 1
        rest > 0 && copyto!(d, 1, d, pos, rest)
        c.rlen = max(rest, 0)
    end
    nothing
end

function on_readable!(L::Loop, c::Conn)
    while true
        if c.rlen == length(c.rbuf)
            length(c.rbuf) >= RBUF_MAX && (close_conn!(L, c); return)
            resize!(c.rbuf, 2 * length(c.rbuf))
        end
        room = length(c.rbuf) - c.rlen
        n = GC.@preserve c ccall(:read, Cssize_t, (Cint, Ptr{UInt8}, Csize_t), c.fd, pointer(c.rbuf, c.rlen + 1), room)
        if n > 0
            c.rlen += n
            n < room && break
        elseif n == 0
            close_conn!(L, c)
            return
        else
            e = Libc.errno()
            e == EINTR && continue
            e == EAGAIN && break
            close_conn!(L, c)
            return
        end
    end
    process!(c)
    flush!(L, c)
    nothing
end

function serve(lfd::Cint)
    ep = ccall(:epoll_create1, Cint, (Cint,), SOCK_CLOEXEC)
    ep < 0 && error("epoll_create1: $(Libc.strerror(Libc.errno()))")
    conns = Vector{Union{Nothing,Conn}}(nothing, 65536)
    L = Loop(ep, lfd, conns, Conn[])
    ep_ctl(ep, EPOLL_CTL_ADD, lfd, EPOLLIN)
    evs = zeros(UInt8, 12 * MAXEV)
    while true
        n = GC.@preserve evs ccall(:epoll_wait, Cint, (Cint, Ptr{UInt8}, Cint, Cint), ep, evs, MAXEV, 1000)
        if n < 0
            Libc.errno() == EINTR && continue
            error("epoll_wait: $(Libc.strerror(Libc.errno()))")
        end
        for k in 0:n-1
            p = pointer(evs) + 12k
            ev, fd = GC.@preserve evs (unsafe_load(Ptr{UInt32}(p)), (unsafe_load(Ptr{UInt64}(p + 4)) % Cint))
            if fd == lfd
                accept_all!(L)
                continue
            end
            fd + 1 <= length(L.conns) || continue
            c = L.conns[fd+1]
            c === nothing && continue
            if ev & (EPOLLERR | EPOLLHUP) != 0 && ev & EPOLLIN == 0
                close_conn!(L, c)
                continue
            end
            if ev & EPOLLOUT != 0
                flush!(L, c) || continue
            end
            ev & EPOLLIN != 0 && on_readable!(L, c)
        end
    end
end
