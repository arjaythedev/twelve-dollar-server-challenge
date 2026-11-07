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
const EPOLL_CTL_DEL = Cint(2)
const EPOLL_CTL_MOD = Cint(3)
const EAGAIN = 11
const EINTR = 4
const EMFILE = 24
const ENFILE = 23

const MAXEV = 1024
const RBUF_SHARED = 1 << 18     # one read buffer shared by all connections
const SPILL_SIZE = 16384        # pooled per-connection buffer, only while a request or a send is incomplete
const STATS_EVERY = 10.0        # seconds between "stats" lines in the log (JR_DEBUG=1 only)
const DEBUG = Ref(false)

# Per-connection state. An idle connection holds no buffers: inbuf/outbuf are EMPTY unless a partial
# request is waiting for more bytes or a response could not be sent in full.
mutable struct Conn
    fd::Cint
    inbuf::Vector{UInt8}
    inlen::Int
    outbuf::Vector{UInt8}
    outoff::Int
    outlen::Int
    pollout::Bool
    closing::Bool
end
const EMPTY = UInt8[]

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
    listening::Bool             # false while accept is paused after EMFILE/ENFILE
    resume_at::Float64
    conns::Vector{Union{Nothing,Conn}}
    pool::Vector{Conn}
    spill::Vector{Vector{UInt8}}
    nconns::Int
    rbuf::Vector{UInt8}
    out::Buf
end

function take_spill!(L::Loop, need::Int)
    need > SPILL_SIZE && return Vector{UInt8}(undef, need)          # rare: big request or response
    isempty(L.spill) ? Vector{UInt8}(undef, SPILL_SIZE) : pop!(L.spill)
end
@inline give_spill!(L::Loop, v::Vector{UInt8}) = (length(v) == SPILL_SIZE && push!(L.spill, v); nothing)

function close_conn!(L::Loop, c::Conn)
    ccall(:close, Cint, (Cint,), c.fd)
    L.conns[c.fd+1] = nothing
    L.nconns -= 1
    c.inbuf !== EMPTY && give_spill!(L, c.inbuf)
    c.outbuf !== EMPTY && give_spill!(L, c.outbuf)
    c.fd = -1; c.inbuf = EMPTY; c.inlen = 0; c.outbuf = EMPTY; c.outoff = 0; c.outlen = 0
    c.pollout = false; c.closing = false
    push!(L.pool, c)
    nothing
end

function accept_all!(L::Loop)
    while true
        fd = ccall(:accept4, Cint, (Cint, Ptr{Cvoid}, Ptr{Cvoid}, Cint), L.lfd, C_NULL, C_NULL,
            SOCK_NONBLOCK | SOCK_CLOEXEC)
        if fd < 0
            e = Libc.errno()
            if e == EMFILE || e == ENFILE      # out of fds: stop polling the listener for a moment, or the
                ep_ctl(L.ep, EPOLL_CTL_DEL, L.lfd, UInt32(0))  # level-triggered event would spin the loop
                L.listening = false
                L.resume_at = time() + 0.1
            end
            return nothing
        end
        if fd + 1 > length(L.conns)
            old = length(L.conns)
            resize!(L.conns, 2 * (fd + 1))
            fill!(view(L.conns, old+1:length(L.conns)), nothing)
        end
        c = isempty(L.pool) ? Conn(fd, EMPTY, 0, EMPTY, 0, 0, false, false) : pop!(L.pool)
        c.fd = fd
        L.conns[fd+1] = c
        L.nconns += 1
        ep_ctl(L.ep, EPOLL_CTL_ADD, fd, EPOLLIN)
    end
end

@inline function arm_out!(L::Loop, c::Conn, on::Bool)
    c.pollout == on && return nothing
    c.pollout = on
    ep_ctl(L.ep, EPOLL_CTL_MOD, c.fd, on ? (EPOLLIN | EPOLLOUT) : EPOLLIN)
    nothing
end

# Sends p[1:n] (or queues it behind an incomplete earlier send). Returns false if the connection closed.
function send_or_queue!(L::Loop, c::Conn, p::Ptr{UInt8}, n::Int)
    if c.outlen > c.outoff                      # earlier bytes still queued: keep the order
        need = c.outlen + n
        need > length(c.outbuf) && resize!(c.outbuf, max(need, 2 * length(c.outbuf)))
        GC.@preserve c unsafe_copyto!(pointer(c.outbuf, c.outlen + 1), p, n)
        c.outlen = need
        return true
    end
    sent = 0
    while sent < n
        r = ccall(:send, Cssize_t, (Cint, Ptr{UInt8}, Csize_t, Cint), c.fd, p + sent, n - sent, MSG_NOSIGNAL)
        if r > 0
            sent += r
            continue
        end
        e = Libc.errno()
        e == EINTR && continue
        if e == EAGAIN
            rem = n - sent
            c.outbuf = take_spill!(L, rem)
            GC.@preserve c unsafe_copyto!(pointer(c.outbuf), p + sent, rem)
            c.outoff = 0; c.outlen = rem
            arm_out!(L, c, true)
            return true
        end
        close_conn!(L, c)
        return false
    end
    if c.closing
        close_conn!(L, c)
        return false
    end
    true
end

function on_writable!(L::Loop, c::Conn)
    while c.outoff < c.outlen
        r = GC.@preserve c ccall(:send, Cssize_t, (Cint, Ptr{UInt8}, Csize_t, Cint),
            c.fd, pointer(c.outbuf, c.outoff + 1), c.outlen - c.outoff, MSG_NOSIGNAL)
        if r > 0
            c.outoff += r
            continue
        end
        e = Libc.errno()
        e == EINTR && continue
        e == EAGAIN && return true
        close_conn!(L, c)
        return false
    end
    give_spill!(L, c.outbuf)
    c.outbuf = EMPTY; c.outoff = 0; c.outlen = 0
    arm_out!(L, c, false)
    if c.closing
        close_conn!(L, c)
        return false
    end
    true
end

const REQ = Req()
const BADREQ = Buf(64)
const NREQ = Ref(0)

# Handles the complete requests in d[1:len], appending responses to out. Returns the index of the first
# unconsumed byte (a partial request).
function process!(c::Conn, d::Vector{UInt8}, len::Int, out::Buf)
    pos = 1
    while pos <= len && !c.closing
        st = parse_req!(REQ, d, pos, len)
        st == 0 && break
        if st < 0
            err!(BADREQ, 400, "bad request")
            write_response!(out, 400, BADREQ, false)
            c.closing = true
            return len + 1
        end
        handle!(out, d, REQ)
        NREQ[] += 1
        pos = REQ.next
        REQ.keepalive || (c.closing = true)
    end
    pos
end

function on_readable!(L::Loop, c::Conn)
    rb = L.rbuf
    n0 = 0
    if c.inlen > 0                              # partial request from before goes first
        copyto!(rb, 1, c.inbuf, 1, c.inlen)
        n0 = c.inlen
        give_spill!(L, c.inbuf)
        c.inbuf = EMPTY; c.inlen = 0
    end
    while true
        room = length(rb) - n0
        if room == 0                            # request larger than the shared buffer: out of scope
            close_conn!(L, c)
            return
        end
        r = GC.@preserve rb ccall(:read, Cssize_t, (Cint, Ptr{UInt8}, Csize_t), c.fd, pointer(rb, n0 + 1), room)
        if r > 0
            n0 += r
            break
        elseif r == 0
            close_conn!(L, c)
            return
        end
        e = Libc.errno()
        e == EINTR && continue
        e == EAGAIN && break
        close_conn!(L, c)
        return
    end
    out = L.out
    reset!(out)
    pos = process!(c, rb, n0, out)
    rest = n0 - pos + 1
    if rest > 0 && !c.closing
        c.inbuf = take_spill!(L, rest)
        copyto!(c.inbuf, 1, rb, pos, rest)
        c.inlen = rest
    end
    if out.len > 0
        GC.@preserve out send_or_queue!(L, c, pointer(out.data), out.len)
    elseif c.closing && c.outlen == c.outoff
        close_conn!(L, c)
    end
    nothing
end

mutable struct Stats
    at::Float64
    reqs::Int
    bytes::Int64
    gc_ns::UInt64
    pauses::Int
end

# One "stats" line per STATS_EVERY seconds: allocation per request is the number Phase 3 drives to zero.
function log_stats!(st::Stats, L::Loop, now::Float64)
    b = Base.gc_bytes()
    g = Base.gc_num()
    dreq = NREQ[] - st.reqs
    per = dreq > 0 ? (b - st.bytes) ÷ dreq : 0
    println("stats conns=", L.nconns, " reqs=", dreq, " alloc_per_req=", per,
        " gc_ms=", (g.total_time - st.gc_ns) ÷ 1_000_000, " gc_pauses=", g.pause - st.pauses,
        " max_pause_ms=", g.max_pause ÷ 1_000_000, " spill_pool=", length(L.spill))
    flush(stdout)
    st.at = now; st.reqs = NREQ[]; st.bytes = Base.gc_bytes(); st.gc_ns = g.total_time; st.pauses = g.pause
    nothing
end

function serve(lfd::Cint)
    ep = ccall(:epoll_create1, Cint, (Cint,), SOCK_CLOEXEC)
    ep < 0 && error("epoll_create1: $(Libc.strerror(Libc.errno()))")
    L = Loop(ep, lfd, true, 0.0, Vector{Union{Nothing,Conn}}(nothing, 65536), Conn[], Vector{UInt8}[], 0,
        Vector{UInt8}(undef, RBUF_SHARED), Buf(1 << 16))
    ep_ctl(ep, EPOLL_CTL_ADD, lfd, EPOLLIN)
    evs = zeros(UInt8, 12 * MAXEV)
    g = Base.gc_num()
    st = Stats(time(), NREQ[], Base.gc_bytes(), g.total_time, g.pause)
    while true
        n = GC.@preserve evs ccall(:epoll_wait, Cint, (Cint, Ptr{UInt8}, Cint, Cint), ep, evs, MAXEV,
            L.listening ? 1000 : 100)
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
                on_writable!(L, c) || continue
            end
            ev & EPOLLIN != 0 && on_readable!(L, c)
        end
        if !L.listening && time() >= L.resume_at
            ep_ctl(ep, EPOLL_CTL_ADD, lfd, EPOLLIN)
            L.listening = true
        end
        if DEBUG[]
            now = time()
            now - st.at >= STATS_EVERY && log_stats!(st, L, now)
        end
    end
end
