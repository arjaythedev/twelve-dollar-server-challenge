# Growable byte buffer that is reused across requests (reset!, never reallocated once warm).

mutable struct Buf
    data::Vector{UInt8}
    len::Int
end
Buf(n::Int = 4096) = Buf(Vector{UInt8}(undef, n), 0)

@inline reset!(b::Buf) = (b.len = 0; nothing)

@inline function reserve!(b::Buf, n::Int)
    need = b.len + n
    if need > length(b.data)
        resize!(b.data, max(need, 2 * length(b.data)))
    end
    nothing
end

@inline function pushb!(b::Buf, x::UInt8)
    reserve!(b, 1)
    b.len += 1
    @inbounds b.data[b.len] = x
    nothing
end
@inline pushb!(b::Buf, c::Char) = pushb!(b, UInt8(c))

@inline function putraw!(b::Buf, p::Ptr{UInt8}, n::Int)
    n <= 0 && return nothing
    reserve!(b, n)
    GC.@preserve b unsafe_copyto!(pointer(b.data, b.len + 1), p, n)
    b.len += n
    nothing
end
@inline putraw!(b::Buf, src::Vector{UInt8}, s::Int, n::Int) = GC.@preserve src putraw!(b, pointer(src, s), n)
@inline putstr!(b::Buf, s::String) = GC.@preserve s putraw!(b, pointer(s), sizeof(s))
@inline putbuf!(b::Buf, o::Buf) = putraw!(b, o.data, 1, o.len)

function putint!(b::Buf, x::Int)
    if x < 0
        pushb!(b, UInt8('-'))
        x = -x
    end
    n = ndigits(x)
    reserve!(b, n)
    p = b.len + n
    @inbounds while true
        b.data[p] = UInt8('0') + (x % 10) % UInt8
        x ÷= 10
        p -= 1
        x == 0 && break
    end
    b.len += n
    nothing
end

const HEXDIGITS = b"0123456789abcdef"

# JSON string with JSON.stringify escaping (", \, control chars); non-ASCII passes through raw.
function putjsonstr!(b::Buf, p::Ptr{UInt8}, n::Int)
    reserve!(b, n + 2)
    pushb!(b, UInt8('"'))
    run = 0                       # start offset of the pending run of plain bytes
    for i in 0:n-1
        c = unsafe_load(p + i)
        (c >= 0x20 && c != 0x22 && c != 0x5c) && continue
        putraw!(b, p + run, i - run)
        run = i + 1
        pushb!(b, UInt8('\\'))
        if c == 0x22
            pushb!(b, UInt8('"'))
        elseif c == 0x5c
            pushb!(b, UInt8('\\'))
        elseif c == 0x0a
            pushb!(b, UInt8('n'))
        elseif c == 0x0d
            pushb!(b, UInt8('r'))
        elseif c == 0x09
            pushb!(b, UInt8('t'))
        elseif c == 0x08
            pushb!(b, UInt8('b'))
        elseif c == 0x0c
            pushb!(b, UInt8('f'))
        else
            pushb!(b, UInt8('u')); pushb!(b, UInt8('0')); pushb!(b, UInt8('0'))
            @inbounds pushb!(b, HEXDIGITS[(c >> 4) + 1])
            @inbounds pushb!(b, HEXDIGITS[(c & 0x0f) + 1])
        end
    end
    putraw!(b, p + run, n - run)
    pushb!(b, UInt8('"'))
    nothing
end
putjsonstr!(b::Buf, o::Buf) = GC.@preserve o putjsonstr!(b, pointer(o.data), o.len)
