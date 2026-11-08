# io_uring transport: raw syscalls, no liburing. One thread drives one or more rings.
#
# - Sockets are direct descriptors: accept places each one in a ring's registered file table
#   (IORING_FILE_INDEX_ALLOC), not in the process fd table, so LimitNOFILE does not cap connections.
#   A table holds at most RLIMIT_NOFILE slots, so a new ring is opened when the current ones are full.
# - Receives are multishot into a provided-buffer ring: an idle connection holds no buffer.
# - Responses are copied into arenas that stay untouched until every send pointing into them completes.
# - Close = SHUTDOWN hard-linked to CLOSE of the direct descriptor (wakes the pending receive).
# Every completion carries (op, ring, arena, generation, slot) in its user_data; a generation that no
# longer matches the slot means the completion belongs to an earlier connection and is dropped.

const SYS_IO_URING_SETUP = 425
const SYS_IO_URING_ENTER = 426
const SYS_IO_URING_REGISTER = 427

const OP_ACCEPT = 0x0d
const OP_CLOSE = 0x13
const OP_SEND = 0x1a
const OP_RECV = 0x1b
const OP_SHUTDOWN = 0x22

const SQE_FIXED_FILE = 0x01
const SQE_IO_HARDLINK = 0x08
const SQE_BUFFER_SELECT = 0x20
const SQE_CQE_SKIP_SUCCESS = 0x40

const SETUP_CQSIZE = UInt32(1) << 3
const SETUP_SUBMIT_ALL = UInt32(1) << 7
const SETUP_COOP_TASKRUN = UInt32(1) << 8
const SETUP_SINGLE_ISSUER = UInt32(1) << 12
const SETUP_DEFER_TASKRUN = UInt32(1) << 13
const FEAT_SINGLE_MMAP = UInt32(1)
const FEAT_EXT_ARG = UInt32(1) << 8

const ENTER_GETEVENTS = UInt32(1)
const ENTER_EXT_ARG = UInt32(8)
const REGISTER_FILES2 = 13
const REGISTER_PBUF_RING = 22
const RSRC_REGISTER_SPARSE = UInt32(1)
const FILE_INDEX_ALLOC = 0xffffffff
const RECV_MULTISHOT = UInt16(2)
const CQE_F_BUFFER = UInt32(1)
const CQE_F_MORE = UInt32(2)

const ENOBUFS = 105
const ETIME = 62

const U_ACCEPT = UInt64(1)
const U_RECV = UInt64(2)
const U_SEND = UInt64(3)
const U_CLOSE = UInt64(4)
const OWN_ARENA = 0xff          # a send from the slot's own buffer, not from an arena

const SQ_ENTRIES = 4096
const CQ_ENTRIES = 65536
const NBUF = 16384              # provided receive buffers per ring (16 MB): every completion not yet handled
const BUFSZ = 1024              # holds one; a request is a few hundred bytes, longer ones arrive in parts
const ACCEPTS = 4096            # single-shot accepts in flight: refilled once per pass, so this is the most a
                                # pass can accept. 256 overflowed the 4096-entry listen backlog during ramp-up
                                # (dropped SYNs, clients retrying after 1 s / 3 s)
const PASS_CAP = 1024           # completions handled per pass, so accepts and sends go out at least that often
const ARENA_SIZE = 1 << 20
const MAX_RINGS = 8
const ACCEPT_ERR_SHOWN = Ref(false)

@inline ud(op, ring, arena, gen, slot) = (UInt64(op) << 60) | (UInt64(ring) << 56) | (UInt64(arena) << 48) |
                                         (UInt64(gen % UInt32) << 16) | UInt64(slot)
@inline ud_op(u::UInt64) = u >> 60
@inline ud_ring(u::UInt64) = Int((u >> 56) & 0x0f)
@inline ud_arena(u::UInt64) = Int((u >> 48) & 0xff)
@inline ud_gen(u::UInt64) = UInt32((u >> 16) & 0xffffffff)
@inline ud_slot(u::UInt64) = Int(u & 0xffff)

# syscall(2) is variadic; ccall needs literal arguments, so one method per arity.
@inline sys(n, a, b) = ccall(:syscall, Clong, (Clong, Clong...), n, a, b)
@inline sys(n, a, b, c, d) = ccall(:syscall, Clong, (Clong, Clong...), n, a, b, c, d)
@inline sys(n, a, b, c, d, e, f) = ccall(:syscall, Clong, (Clong, Clong...), n, a, b, c, d, e, f)
@inline pu8!(p::Ptr{UInt8}, off, v) = unsafe_store!(p + off, v % UInt8)
@inline pu16!(p::Ptr{UInt8}, off, v) = unsafe_store!(Ptr{UInt16}(p + off), v % UInt16)
@inline pu32!(p::Ptr{UInt8}, off, v) = unsafe_store!(Ptr{UInt32}(p + off), v % UInt32)
@inline pu64!(p::Ptr{UInt8}, off, v) = unsafe_store!(Ptr{UInt64}(p + off), v % UInt64)
@inline gu32(p::Ptr{UInt8}, off) = unsafe_load(Ptr{UInt32}(p + off))

function mmap_or_fail(len, prot, flags, fd, off)
    p = ccall(:mmap, Ptr{UInt8}, (Ptr{Cvoid}, Csize_t, Cint, Cint, Cint, Int64), C_NULL, len, prot, flags, fd, off)
    p == Ptr{UInt8}(typemax(UInt)) && error("mmap: $(Libc.strerror(Libc.errno()))")
    p
end

mutable struct Arena
    data::Vector{UInt8}
    len::Int
    refs::Int                   # sends in flight that point into data
end

mutable struct Ring
    idx::Int
    fd::Cint
    sqes::Ptr{UInt8}
    sq_tail_p::Ptr{UInt32}
    sq_head_p::Ptr{UInt32}
    sq_mask::UInt32
    sq_tail::UInt32             # local tail, published before each enter
    to_submit::Int
    cq_head_p::Ptr{UInt32}
    cq_tail_p::Ptr{UInt32}
    cq_mask::UInt32
    cqes::Ptr{UInt8}
    # provided-buffer ring (buffer group 0 of this ring)
    bufs::Vector{UInt8}
    bring::Ptr{UInt8}
    btail::UInt16
    # per-slot connection state (struct of arrays, indexed by direct descriptor + 1)
    slots::Int
    open::Int
    accepts::Int                # accepts in flight
    accepting::Bool
    gen::Vector{UInt32}
    live::Vector{Bool}
    recving::Vector{Bool}
    closing::Vector{Bool}
    sending::Vector{Bool}
    sptr::Vector{Ptr{UInt8}}    # in-flight send: next byte, bytes left, arena
    slen::Vector{Int}
    sarena::Vector{Int}
    own::Vector{Vector{UInt8}}  # the slot's own send buffer (OWN_ARENA)
    pend::Vector{Vector{UInt8}} # responses waiting for the in-flight send
    pendlen::Vector{Int}
    inbuf::Vector{Vector{UInt8}}# a partial request waiting for more bytes
    inlen::Vector{Int}
end

mutable struct Uring
    lfd::Cint
    rings::Vector{Ring}
    acc::Int                    # ring that accepts new connections
    flags::UInt32               # setup flags that worked
    arenas::Vector{Arena}
    cur::Int
    out::Buf                    # one connection's responses, before they are copied into an arena
    spill::Vector{Vector{UInt8}}
    limbo::Dict{UInt64,Vector{UInt8}}  # own buffers of closed slots, kept alive until their send completes
    rearm::Vector{Tuple{Int,Int,UInt32}}   # receives that found no buffer, re-armed in order as buffers return
    rearm_head::Int
    returned::Int                          # buffers given back this pass
    timeout::Vector{Int64}      # struct __kernel_timespec + struct io_uring_getevents_arg
    evarg::Vector{UInt64}
    nslots::Int
    passes_total::Int
    conns::Int                  # --debug only: open connections, passes and completions since the last stats line
    passes::Int
    ncqes::Int
    max_pass_ns::UInt64
    ring_cqes::Vector{Int}
    nobufs::Int
    backlogs::Int
    wait_ns::UInt64             # time blocked in io_uring_enter waiting for completions
end

function nofile_limit()
    rl = zeros(UInt64, 2)
    ccall(:getrlimit, Cint, (Cint, Ptr{UInt64}), 7, rl) == 0 || return 1024   # RLIMIT_NOFILE
    Int(min(rl[1], 65535))
end

# Creates a ring with a sparse direct-descriptor table of `nslots` and a provided-buffer ring.
function Ring(idx::Int, nslots::Int, flags::UInt32)
    p = zeros(UInt8, 120)                               # struct io_uring_params
    GC.@preserve p ring_setup(p, pointer(p), idx, nslots, flags)
end

function ring_setup(p::Vector{UInt8}, pp::Ptr{UInt8}, idx::Int, nslots::Int, flags::UInt32)
    fd = -1
    for f in (flags, flags & ~(SETUP_DEFER_TASKRUN | SETUP_SINGLE_ISSUER), SETUP_CQSIZE)
        fill!(p, 0x00)
        pu32!(pp, 4, CQ_ENTRIES)
        pu32!(pp, 8, f)
        fd = GC.@preserve p sys(SYS_IO_URING_SETUP, SQ_ENTRIES, Clong(UInt(pp)))
        if fd >= 0
            flags = f
            break
        end
    end
    fd < 0 && error("io_uring_setup: $(Libc.strerror(Libc.errno()))")
    fd = Cint(fd)
    feat = gu32(pp, 20)
    (feat & FEAT_SINGLE_MMAP != 0 && feat & FEAT_EXT_ARG != 0) || error("io_uring: kernel too old (features $feat)")
    sq_entries = gu32(pp, 0)
    cq_entries = gu32(pp, 4)
    sq_off = 40; cq_off = 80
    sq_size = gu32(pp, sq_off + 24) + 4 * sq_entries       # sq_off.array + entries * u32
    cq_size = gu32(pp, cq_off + 20) + 16 * cq_entries      # cq_off.cqes + entries * cqe
    rsize = max(sq_size, cq_size)
    rp = mmap_or_fail(rsize, 3, 0x01 | 0x8000, fd, 0)       # PROT_READ|WRITE, MAP_SHARED|POPULATE
    sqes = mmap_or_fail(64 * sq_entries, 3, 0x01 | 0x8000, fd, 0x10000000)
    sq_array = Ptr{UInt32}(rp + gu32(pp, sq_off + 24))
    for i in 0:sq_entries-1                                  # identity mapping: index i holds sqe i
        unsafe_store!(sq_array + 4i, UInt32(i))
    end

    # sparse fixed-file table
    reg = zeros(UInt64, 4)                                   # struct io_uring_rsrc_register
    rp_ = Ptr{UInt8}(pointer(reg))
    pu32!(rp_, 0, nslots); pu32!(rp_, 4, RSRC_REGISTER_SPARSE)
    r = GC.@preserve reg sys(SYS_IO_URING_REGISTER, fd, REGISTER_FILES2, Clong(UInt(rp_)), 32)
    r < 0 && error("io_uring register files ($nslots): $(Libc.strerror(Libc.errno()))")

    # provided-buffer ring
    bring = mmap_or_fail(16 * NBUF, 3, 0x02 | 0x20, -1, 0)   # MAP_PRIVATE|ANONYMOUS, page aligned
    breg = zeros(UInt64, 5)                                  # struct io_uring_buf_reg
    bp = Ptr{UInt8}(pointer(breg))
    pu64!(bp, 0, UInt(bring)); pu32!(bp, 8, NBUF); pu16!(bp, 12, 0)
    r = GC.@preserve breg sys(SYS_IO_URING_REGISTER, fd, REGISTER_PBUF_RING, Clong(UInt(bp)), 1)
    if r < 0
        # Ubuntu 6.8.0-147 and related builds check the struct's reserved words the wrong way round: they refuse
        # zeroes and accept anything else (Launchpad #2162843). A correct kernel refuses non-zero, so this
        # second try only succeeds where the first failed for that reason.
        pu64!(bp, 16, 1)
        r = GC.@preserve breg sys(SYS_IO_URING_REGISTER, fd, REGISTER_PBUF_RING, Clong(UInt(bp)), 1)
        r < 0 && error("io_uring register buffer ring: $(Libc.strerror(Libc.errno()))")
        idx == 0 && println("io_uring: buffer ring registered with resv[0]=1 (kernel with the inverted check)")
    end
    bufs = Vector{UInt8}(undef, NBUF * BUFSZ)

    ring = Ring(idx, fd, sqes,
        Ptr{UInt32}(rp + gu32(pp, sq_off + 4)), Ptr{UInt32}(rp + gu32(pp, sq_off + 0)),
        unsafe_load(Ptr{UInt32}(rp + gu32(pp, sq_off + 8))), UInt32(0), 0,
        Ptr{UInt32}(rp + gu32(pp, cq_off + 0)), Ptr{UInt32}(rp + gu32(pp, cq_off + 4)),
        unsafe_load(Ptr{UInt32}(rp + gu32(pp, cq_off + 8))), rp + gu32(pp, cq_off + 20),
        bufs, bring, UInt16(0),
        nslots, 0, 0, true,
        zeros(UInt32, nslots), fill(false, nslots), fill(false, nslots), fill(false, nslots), fill(false, nslots),
        fill(Ptr{UInt8}(0), nslots), zeros(Int, nslots), zeros(Int, nslots),
        fill(EMPTY, nslots), fill(EMPTY, nslots), zeros(Int, nslots), fill(EMPTY, nslots), zeros(Int, nslots))
    ring.sq_tail = unsafe_load(ring.sq_tail_p)
    for b in 0:NBUF-1
        give_buf!(ring, b)
    end
    publish_bufs!(ring)
    ring, flags
end

# ---- submission

@inline function enter(r::Ring, min_complete::Integer, flags::UInt32, arg::Ptr{Cvoid} = C_NULL, argsz = 0)
    unsafe_store!(r.sq_tail_p, r.sq_tail, 1, :release)
    n = r.to_submit
    ret = sys(SYS_IO_URING_ENTER, r.fd, n, min_complete, flags, Clong(UInt(arg)), argsz)
    if ret >= 0
        r.to_submit = max(n - Int(ret), 0)
    end
    ret
end

@inline function sqe!(r::Ring)
    if r.sq_tail - unsafe_load(r.sq_head_p, 1, :acquire) >= r.sq_mask + 1   # full: submit what we have
        enter(r, 0, UInt32(0))
    end
    p = r.sqes + 64 * Int(r.sq_tail & r.sq_mask)
    for k in 0:8:56
        unsafe_store!(Ptr{UInt64}(p + k), UInt64(0))
    end
    r.sq_tail += UInt32(1)
    r.to_submit += 1
    p
end

function submit_accept!(U::Uring, r::Ring)
    p = sqe!(r)
    pu8!(p, 0, OP_ACCEPT)
    pu32!(p, 4, U.lfd)                                       # accept flags 0: SOCK_CLOEXEC is EINVAL for a direct descriptor
    pu32!(p, 44, FILE_INDEX_ALLOC)
    pu64!(p, 32, ud(U_ACCEPT, r.idx, 0, 0, 0))
    r.accepts += 1
    nothing
end

function submit_recv!(r::Ring, slot::Int)
    p = sqe!(r)
    pu8!(p, 0, OP_RECV)
    pu8!(p, 1, SQE_FIXED_FILE | SQE_BUFFER_SELECT)
    pu16!(p, 2, RECV_MULTISHOT)
    pu32!(p, 4, slot)
    pu16!(p, 40, 0)                                          # buffer group
    pu64!(p, 32, ud(U_RECV, r.idx, 0, r.gen[slot+1], slot))
    r.recving[slot+1] = true
    nothing
end

function submit_send!(r::Ring, slot::Int)
    p = sqe!(r)
    pu8!(p, 0, OP_SEND)
    pu8!(p, 1, SQE_FIXED_FILE)
    pu32!(p, 4, slot)
    pu64!(p, 16, UInt(r.sptr[slot+1]))
    pu32!(p, 24, r.slen[slot+1])
    pu32!(p, 28, MSG_NOSIGNAL)
    pu64!(p, 32, ud(U_SEND, r.idx, r.sarena[slot+1], r.gen[slot+1], slot))
    nothing
end

function submit_close!(r::Ring, slot::Int)
    p = sqe!(r)
    pu8!(p, 0, OP_SHUTDOWN)
    pu8!(p, 1, SQE_FIXED_FILE | SQE_IO_HARDLINK | SQE_CQE_SKIP_SUCCESS)
    pu32!(p, 4, slot)
    pu32!(p, 24, 2)                                          # SHUT_RDWR
    pu64!(p, 32, ud(U_CLOSE, r.idx, 0, 0, slot))
    p = sqe!(r)
    pu8!(p, 0, OP_CLOSE)
    pu8!(p, 1, SQE_CQE_SKIP_SUCCESS)
    pu32!(p, 44, slot + 1)                                   # direct descriptor to close
    pu64!(p, 32, ud(U_CLOSE, r.idx, 0, 0, slot))
    nothing
end

# ---- provided buffers

@inline function give_buf!(r::Ring, bid::Int)
    e = r.bring + 16 * Int(r.btail & UInt16(NBUF - 1))
    pu64!(e, 0, UInt(pointer(r.bufs)) + bid * BUFSZ)   # (entry 0's last 2 bytes are the ring tail: not written)
    pu32!(e, 8, BUFSZ)
    pu16!(e, 12, bid)
    r.btail += UInt16(1)
    nothing
end
@inline publish_bufs!(r::Ring) = unsafe_store!(Ptr{UInt16}(r.bring + 14), r.btail, 1, :release)

# ---- buffers for partial requests and own sends

function take_spill!(U::Uring, need::Int)
    need > SPILL_SIZE && return Vector{UInt8}(undef, need)
    isempty(U.spill) ? Vector{UInt8}(undef, SPILL_SIZE) : pop!(U.spill)
end
@inline give_spill!(U::Uring, v::Vector{UInt8}) = (length(v) == SPILL_SIZE && push!(U.spill, v); nothing)

# An arena with room for n bytes (the current one, or a free / new one).
function arena_for!(U::Uring, n::Int)
    A = U.arenas[U.cur]
    A.len + n <= ARENA_SIZE && return U.cur
    for i in eachindex(U.arenas)
        if U.arenas[i].refs == 0
            U.arenas[i].len = 0
            U.cur = i
            return i
        end
    end
    length(U.arenas) >= OWN_ARENA && return 0           # out of arena ids: caller uses an own buffer
    push!(U.arenas, Arena(Vector{UInt8}(undef, ARENA_SIZE), 0, 0))
    U.cur = length(U.arenas)
    U.cur
end

# ---- connection lifecycle

function close_slot!(U::Uring, r::Ring, slot::Int)
    i = slot + 1
    r.live[i] || return nothing
    r.live[i] = false
    r.open -= 1
    @dbg U.conns -= 1
    if r.own[i] !== EMPTY
        if r.sending[i] && r.sarena[i] == OWN_ARENA          # the kernel may still read it
            U.limbo[ud(0, r.idx, 0, r.gen[i], slot)] = r.own[i]
        else
            give_spill!(U, r.own[i])
        end
        r.own[i] = EMPTY
    end
    r.inbuf[i] !== EMPTY && (give_spill!(U, r.inbuf[i]); r.inbuf[i] = EMPTY)
    r.pend[i] !== EMPTY && (give_spill!(U, r.pend[i]); r.pend[i] = EMPTY)
    r.inlen[i] = 0; r.pendlen[i] = 0
    submit_close!(r, slot)
    nothing
end

# Queues the bytes in U.out for the slot: sent now from an arena, or appended behind an in-flight send.
function queue_send!(U::Uring, r::Ring, slot::Int)
    i = slot + 1
    out = U.out
    n = out.len
    if r.sending[i]
        need = r.pendlen[i] + n
        if r.pend[i] === EMPTY
            r.pend[i] = take_spill!(U, need)
        elseif need > length(r.pend[i])
            resize!(r.pend[i], max(need, 2 * length(r.pend[i])))
        end
        GC.@preserve out copyto!(r.pend[i], r.pendlen[i] + 1, out.data, 1, n)
        r.pendlen[i] = need
        return nothing
    end
    ai = n <= ARENA_SIZE ? arena_for!(U, n) : 0
    if ai == 0
        r.own[i] = take_spill!(U, n)
        copyto!(r.own[i], 1, out.data, 1, n)
        r.sptr[i] = pointer(r.own[i]); r.sarena[i] = OWN_ARENA
    else
        A = U.arenas[ai]
        copyto!(A.data, A.len + 1, out.data, 1, n)
        r.sptr[i] = pointer(A.data, A.len + 1); r.sarena[i] = ai
        A.len += n
        A.refs += 1
    end
    r.slen[i] = n
    r.sending[i] = true
    submit_send!(r, slot)
    nothing
end

# New bytes d[s : s+n-1] for the slot: handles every complete request, keeps a partial one.
function on_data!(U::Uring, r::Ring, slot::Int, d::Vector{UInt8}, s::Int, n::Int)
    i = slot + 1
    if r.inlen[i] > 0
        need = r.inlen[i] + n
        need > length(r.inbuf[i]) && resize!(r.inbuf[i], max(need, 2 * length(r.inbuf[i])))
        copyto!(r.inbuf[i], r.inlen[i] + 1, d, s, n)
        r.inlen[i] = need
        data = r.inbuf[i]; pos = 1; len = need
    else
        data = d; pos = s; len = s + n - 1
    end
    out = U.out
    reset!(out)
    while pos <= len && !r.closing[i]
        st = parse_req!(REQ, data, pos, len)
        st == 0 && break
        if st < 0
            err!(BADREQ, 400, "bad request")
            write_response!(out, 400, BADREQ, false)
            r.closing[i] = true
            pos = len + 1
            break
        end
        handle!(out, data, REQ)
        @dbg NREQ[] += 1
        pos = REQ.next
        REQ.keepalive || (r.closing[i] = true)
    end
    rest = r.closing[i] ? 0 : len - pos + 1
    if data === d
        if rest > 0
            r.inbuf[i] = take_spill!(U, rest)
            copyto!(r.inbuf[i], 1, d, pos, rest)
            r.inlen[i] = rest
        end
    elseif rest > 0
        copyto!(r.inbuf[i], 1, data, pos, rest)
        r.inlen[i] = rest
    else
        give_spill!(U, r.inbuf[i]); r.inbuf[i] = EMPTY; r.inlen[i] = 0
    end
    out.len > 0 && queue_send!(U, r, slot)
    r.closing[i] && !r.sending[i] && close_slot!(U, r, slot)
    nothing
end

# ---- completions

function on_accept!(U::Uring, r::Ring, res::Int)
    r.accepts -= 1
    if res < 0
        if res == -ENFILE
            r.accepting = false                              # table full: this ring stops accepting
        elseif res != -EAGAIN && res != -EINTR && res != -103 && !ACCEPT_ERR_SHOWN[]   # 103: ECONNABORTED
            ACCEPT_ERR_SHOWN[] = true
            println("io_uring: accept failed: $(Libc.strerror(-res))"); flush(stdout)
        end
        return nothing
    end
    slot = res
    i = slot + 1
    r.gen[i] += UInt32(1)
    r.live[i] = true; r.closing[i] = false; r.sending[i] = false; r.recving[i] = false
    r.inlen[i] = 0; r.pendlen[i] = 0
    r.open += 1
    @dbg U.conns += 1
    submit_recv!(r, slot)
    nothing
end

function on_recv!(U::Uring, r::Ring, u::UInt64, res::Int, flags::UInt32)
    slot = ud_slot(u); i = slot + 1
    stale = !r.live[i] || r.gen[i] != ud_gen(u)
    hasbuf = flags & CQE_F_BUFFER != 0
    if res > 0 && hasbuf && !stale
        bid = Int(flags >> 16)
        on_data!(U, r, slot, r.bufs, bid * BUFSZ + 1, res)
    end
    hasbuf && (give_buf!(r, Int(flags >> 16)); U.returned += 1)
    stale && return nothing
    more = flags & CQE_F_MORE != 0
    more || (r.recving[i] = false)
    if res == 0 || (res < 0 && res != -ENOBUFS)
        close_slot!(U, r, slot)
    elseif !more && r.live[i] && !r.closing[i]
        if res == -ENOBUFS
            @dbg U.nobufs += 1
            push!(U.rearm, (r.idx, slot, r.gen[i]))            # once buffers came back
        else
            submit_recv!(r, slot)
        end
    end
    nothing
end

function on_send!(U::Uring, r::Ring, u::UInt64, res::Int)
    slot = ud_slot(u); i = slot + 1
    ai = ud_arena(u)
    ai != OWN_ARENA && (U.arenas[ai].refs -= 1)
    if !r.live[i] || r.gen[i] != ud_gen(u)
        ai == OWN_ARENA && delete!(U.limbo, ud(0, r.idx, 0, ud_gen(u), slot))
        return nothing
    end
    if res < 0
        r.sending[i] = false
        close_slot!(U, r, slot)
        return nothing
    end
    if res < r.slen[i]                                       # short send: the rest from the same buffer
        r.sptr[i] += res
        r.slen[i] -= res
        ai != OWN_ARENA && (U.arenas[ai].refs += 1)
        submit_send!(r, slot)
        return nothing
    end
    r.sending[i] = false
    if ai == OWN_ARENA
        give_spill!(U, r.own[i]); r.own[i] = EMPTY
    end
    if r.pendlen[i] > 0                                      # responses that waited: send them now
        r.own[i] = r.pend[i]; r.pend[i] = EMPTY
        r.sptr[i] = pointer(r.own[i]); r.slen[i] = r.pendlen[i]; r.sarena[i] = OWN_ARENA
        r.pendlen[i] = 0
        r.sending[i] = true
        submit_send!(r, slot)
    elseif r.closing[i]
        close_slot!(U, r, slot)
    end
    nothing
end

# Handles up to `cap` completions; returns how many. Any left over stay in the CQ for the next pass.
function reap!(U::Uring, r::Ring, cap::Int)
    head = unsafe_load(r.cq_head_p)
    tail = unsafe_load(r.cq_tail_p, 1, :acquire)
    n = 0
    while head != tail && n < cap
        c = r.cqes + 16 * Int(head & r.cq_mask)
        u = unsafe_load(Ptr{UInt64}(c))
        res = Int(unsafe_load(Ptr{Int32}(c + 8)))
        flags = unsafe_load(Ptr{UInt32}(c + 12))
        head += UInt32(1)
        n += 1
        n & 63 == 0 && publish_bufs!(r)                      # buffers go back to the kernel as soon as possible
        op = ud_op(u)
        if op == U_RECV
            on_recv!(U, r, u, res, flags)
        elseif op == U_SEND
            on_send!(U, r, u, res)
        elseif op == U_ACCEPT
            on_accept!(U, r, res)
        end                                                  # U_CLOSE: only failures arrive; nothing to do
        if head == tail                                      # more may have arrived meanwhile
            unsafe_store!(r.cq_head_p, head, 1, :release)
            tail = unsafe_load(r.cq_tail_p, 1, :acquire)
        end
    end
    unsafe_store!(r.cq_head_p, head, 1, :release)
    n
end

@inline cq_pending(r::Ring) = unsafe_load(r.cq_head_p) != unsafe_load(r.cq_tail_p, 1, :acquire)

# Keeps ACCEPTS accepts in flight on the accepting ring, leaving room in its table; opens another ring
# when every ring is (nearly) full.
function top_up_accepts!(U::Uring)
    r = U.rings[U.acc]
    reserve = ACCEPTS + 16
    if !r.accepting || r.open + r.accepts + reserve > r.slots
        r.accepting = false
        next = 0
        for k in eachindex(U.rings)
            q = U.rings[k]
            if q.open + q.accepts + 2 * reserve <= q.slots
                next = k
                break
            end
        end
        if next == 0 && length(U.rings) < MAX_RINGS
            q, _ = Ring(length(U.rings), U.nslots, U.flags)
            push!(U.rings, q)
            next = length(U.rings)
            println("io_uring: ring $(next - 1) opened ($(sum(x -> x.open, U.rings)) connections)")
            flush(stdout)
        end
        next == 0 && return nothing
        U.acc = next
        r = U.rings[next]
        r.accepting = true
    end
    while r.accepts < ACCEPTS && r.open + r.accepts + 16 <= r.slots
        submit_accept!(U, r)
    end
    nothing
end

function uring_init(lfd::Cint)
    nslots = nofile_limit()
    want = SETUP_CQSIZE | SETUP_SUBMIT_ALL | SETUP_COOP_TASKRUN | SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN
    r, flags = Ring(0, nslots, want)
    U = Uring(lfd, [r], 1, flags, [Arena(Vector{UInt8}(undef, ARENA_SIZE), 0, 0)], 1, Buf(1 << 16),
        Vector{UInt8}[], Dict{UInt64,Vector{UInt8}}(), Tuple{Int,Int,UInt32}[], 1, 0,
        Int64[0, 1_000_000], zeros(UInt64, 3), nslots, 0, 0, 0, 0, UInt64(0), zeros(Int, MAX_RINGS), 0, 0, UInt64(0))
    U.evarg[3] = UInt(pointer(U.timeout))                    # getevents_arg.ts
    println("io_uring: flags=0x$(string(flags, base = 16)) slots/ring=$nslots buffers=$NBUF x $BUFSZ B")
    U
end

function serve_uring(U::Uring)
    @dbg (g = Base.gc_num(); st = Stats(time(), NREQ[], Base.gc_bytes(), g.total_time, g.pause))
    top_up_accepts!(U)
    while true
        # 1. Submit, then wait: on ring 0 for one completion (with several rings, at most 1 ms, then the
        #    others are polled), so one pass handles everything that arrived meanwhile.
        rings = U.rings
        r0 = rings[1]
        backlog = false                                      # completions left over from a capped pass
        for r in rings
            backlog |= cq_pending(r)
        end
        if backlog
            @dbg U.backlogs += 1
            ret = 0
            for r in rings
                enter(r, 0, ENTER_GETEVENTS)                 # submit and run deferred work, no waiting
            end
        elseif length(rings) == 1
            @dbg tw = time_ns()
            ret = enter(r0, 1, ENTER_GETEVENTS)
            @dbg U.wait_ns += time_ns() - tw
        else
            @dbg tw = time_ns()
            ret = GC.@preserve U enter(r0, 1, ENTER_GETEVENTS | ENTER_EXT_ARG, Ptr{Cvoid}(pointer(U.evarg)), 24)
            @dbg U.wait_ns += time_ns() - tw
            for k in 2:length(rings)
                enter(rings[k], 0, ENTER_GETEVENTS)
            end
        end
        if ret < 0
            e = Libc.errno()
            (e == EINTR || e == ETIME || e == EAGAIN) || error("io_uring_enter: $(Libc.strerror(e))")
        end
        # 2. Handle the completions of every ring.
        @dbg t0 = time_ns()
        # Each ring gets its own cap, starting from a different ring every pass: with one shared cap ring 0
        # (always first) starved the others, whose unhandled completions held all their buffers (ENOBUFS).
        nr = length(rings)
        start = nr == 1 ? 0 : (U.passes_total % nr)
        U.passes_total += 1
        for k in 0:nr-1
            r = rings[(start + k) % nr + 1]
            n = reap!(U, r, PASS_CAP)
            @dbg (U.ncqes += n; U.ring_cqes[r.idx+1] += n)
        end
        @dbg U.passes += 1
        # 3. Return the receive buffers, re-arm receives that found none, keep accepts in flight.
        for r in rings
            publish_bufs!(r)
        end
        # Re-arm receives that found no buffer, but only about as many as buffers came back: re-arming them all
        # at once made them fail again at once (an ENOBUFS storm that stalled every pass).
        if U.rearm_head <= length(U.rearm)
            budget = min(length(U.rearm) - U.rearm_head + 1, min(max(U.returned, 32), NBUF ÷ 2))
            while budget > 0
                ri, slot, g = U.rearm[U.rearm_head]
                U.rearm_head += 1
                budget -= 1
                q = rings[ri+1]
                (q.live[slot+1] && q.gen[slot+1] == g && !q.recving[slot+1] && !q.closing[slot+1]) &&
                    submit_recv!(q, slot)
            end
            if U.rearm_head > length(U.rearm)
                empty!(U.rearm); U.rearm_head = 1
            elseif U.rearm_head > 4096
                deleteat!(U.rearm, 1:U.rearm_head-1); U.rearm_head = 1
            end
        end
        U.returned = 0
        top_up_accepts!(U)
        A = U.arenas[U.cur]
        A.refs == 0 && (A.len = 0)                           # nothing points into it: reuse from the start
        @dbg begin
            U.max_pass_ns = max(U.max_pass_ns, time_ns() - t0)
            now = time()
            if now - st.at >= STATS_EVERY
                log_stats!(st, U.conns, length(U.spill), now,
                    " passes=$(U.passes) cqes_per_pass=$(U.passes > 0 ? U.ncqes ÷ U.passes : 0) rings=$(length(U.rings))" *
                    " max_pass_ms=$(U.max_pass_ns ÷ 1_000_000) backlog_passes=$(U.backlogs) nobufs=$(U.nobufs)" *
                    " ring_cqes=$(join(U.ring_cqes[1:length(U.rings)], '/')) arenas=$(length(U.arenas))" *
                    " loop_wait_pct=$(round(Int, U.wait_ns / 1e7 / (now - st.at)))")
                U.wait_ns = 0
                U.passes = 0; U.ncqes = 0; U.max_pass_ns = 0; U.backlogs = 0; U.nobufs = 0; fill!(U.ring_cqes, 0)
            end
        end
    end
end
