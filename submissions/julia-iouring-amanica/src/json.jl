# Minimal JSON reader: validates a whole document and picks out a few top-level object fields.
# Positions are 1-based indices into a byte vector; `e` is the last valid index.

const JT_MISSING = 0
const JT_STRING = 1
const JT_NUMBER = 2
const JT_OTHER = 3

const MAXFIELDS = 3

# Result of scanning a top-level object for up to MAXFIELDS wanted keys (last duplicate wins).
mutable struct Fields
    isobject::Bool
    typ::Vector{Int}
    vs::Vector{Int}       # value span (raw, strings include their quotes)
    ve::Vector{Int}
end
Fields() = Fields(false, zeros(Int, MAXFIELDS), zeros(Int, MAXFIELDS), zeros(Int, MAXFIELDS))

@inline isws(c::UInt8) = c == 0x20 || c == 0x09 || c == 0x0a || c == 0x0d
@inline function skipws(d::Vector{UInt8}, i::Int, e::Int)
    @inbounds while i <= e && isws(d[i])
        i += 1
    end
    i
end
@inline isdigitb(c::UInt8) = 0x30 <= c <= 0x39
@inline function ishex(c::UInt8)
    isdigitb(c) || (0x61 <= c <= 0x66) || (0x41 <= c <= 0x46)
end

# Each scan_* returns the index just past the value, or 0 on a syntax error.
function scan_string(d::Vector{UInt8}, i::Int, e::Int)
    @inbounds begin
        (i <= e && d[i] == UInt8('"')) || return 0
        i += 1
        while i <= e
            c = d[i]
            if c == UInt8('"')
                return i + 1
            elseif c == UInt8('\\')
                i + 1 <= e || return 0
                x = d[i+1]
                if x == UInt8('u')
                    i + 5 <= e || return 0
                    for k in 2:5
                        ishex(d[i+k]) || return 0
                    end
                    i += 6
                elseif x in (UInt8('"'), UInt8('\\'), UInt8('/'), UInt8('b'), UInt8('f'), UInt8('n'), UInt8('r'), UInt8('t'))
                    i += 2
                else
                    return 0
                end
            elseif c < 0x20
                return 0
            else
                i += 1
            end
        end
    end
    0
end

function scan_number(d::Vector{UInt8}, i::Int, e::Int)
    @inbounds begin
        i <= e && d[i] == UInt8('-') && (i += 1)
        i <= e || return 0
        if d[i] == UInt8('0')
            i += 1
        elseif isdigitb(d[i])
            while i <= e && isdigitb(d[i]); i += 1; end
        else
            return 0
        end
        if i <= e && d[i] == UInt8('.')
            i += 1
            (i <= e && isdigitb(d[i])) || return 0
            while i <= e && isdigitb(d[i]); i += 1; end
        end
        if i <= e && (d[i] == UInt8('e') || d[i] == UInt8('E'))
            i += 1
            i <= e && (d[i] == UInt8('+') || d[i] == UInt8('-')) && (i += 1)
            (i <= e && isdigitb(d[i])) || return 0
            while i <= e && isdigitb(d[i]); i += 1; end
        end
    end
    i
end

@inline function scan_lit(d::Vector{UInt8}, i::Int, e::Int, lit::String)
    n = sizeof(lit)
    i + n - 1 <= e || return 0
    @inbounds for k in 1:n
        d[i+k-1] == codeunit(lit, k) || return 0
    end
    i + n
end

function scan_value(d::Vector{UInt8}, i::Int, e::Int, depth::Int)
    depth > 64 && return 0
    i > e && return 0
    @inbounds c = d[i]
    if c == UInt8('"')
        return scan_string(d, i, e)
    elseif c == UInt8('{')
        i = skipws(d, i + 1, e)
        i <= e || return 0
        @inbounds d[i] == UInt8('}') && return i + 1
        while true
            i = scan_string(d, i, e); i == 0 && return 0
            i = skipws(d, i, e)
            (i <= e && @inbounds(d[i]) == UInt8(':')) || return 0
            i = skipws(d, i + 1, e)
            i = scan_value(d, i, e, depth + 1); i == 0 && return 0
            i = skipws(d, i, e)
            i <= e || return 0
            @inbounds x = d[i]
            x == UInt8('}') && return i + 1
            x == UInt8(',') || return 0
            i = skipws(d, i + 1, e)
        end
    elseif c == UInt8('[')
        i = skipws(d, i + 1, e)
        i <= e || return 0
        @inbounds d[i] == UInt8(']') && return i + 1
        while true
            i = scan_value(d, i, e, depth + 1); i == 0 && return 0
            i = skipws(d, i, e)
            i <= e || return 0
            @inbounds x = d[i]
            x == UInt8(']') && return i + 1
            x == UInt8(',') || return 0
            i = skipws(d, i + 1, e)
        end
    elseif c == UInt8('-') || isdigitb(c)
        return scan_number(d, i, e)
    elseif c == UInt8('t')
        return scan_lit(d, i, e, "true")
    elseif c == UInt8('f')
        return scan_lit(d, i, e, "false")
    elseif c == UInt8('n')
        return scan_lit(d, i, e, "null")
    end
    0
end

@inline function hexval(c::UInt8)
    c <= 0x39 ? Int(c - 0x30) : c >= 0x61 ? Int(c - 0x61 + 10) : Int(c - 0x41 + 10)
end
@inline function hex4(d::Vector{UInt8}, i::Int)
    @inbounds (hexval(d[i]) << 12) | (hexval(d[i+1]) << 8) | (hexval(d[i+2]) << 4) | hexval(d[i+3])
end

function pututf8!(b::Buf, cp::Int)
    if cp < 0x80
        pushb!(b, cp % UInt8)
    elseif cp < 0x800
        pushb!(b, (0xc0 | (cp >> 6)) % UInt8)
        pushb!(b, (0x80 | (cp & 0x3f)) % UInt8)
    elseif cp < 0x10000
        pushb!(b, (0xe0 | (cp >> 12)) % UInt8)
        pushb!(b, (0x80 | ((cp >> 6) & 0x3f)) % UInt8)
        pushb!(b, (0x80 | (cp & 0x3f)) % UInt8)
    else
        pushb!(b, (0xf0 | (cp >> 18)) % UInt8)
        pushb!(b, (0x80 | ((cp >> 12) & 0x3f)) % UInt8)
        pushb!(b, (0x80 | ((cp >> 6) & 0x3f)) % UInt8)
        pushb!(b, (0x80 | (cp & 0x3f)) % UInt8)
    end
end

# Decodes an already-validated JSON string d[s:e] (quotes included) into b as UTF-8.
# A lone surrogate becomes U+FFFD.
function decode_string!(b::Buf, d::Vector{UInt8}, s::Int, e::Int)
    reset!(b)
    i = s + 1
    last = e - 1
    @inbounds while i <= last
        c = d[i]
        if c != UInt8('\\')
            pushb!(b, c); i += 1
            continue
        end
        x = d[i+1]
        if x == UInt8('u')
            cp = hex4(d, i + 2); i += 6
            if 0xd800 <= cp <= 0xdbff && i + 5 <= last && d[i] == UInt8('\\') && d[i+1] == UInt8('u')
                lo = hex4(d, i + 2)
                if 0xdc00 <= lo <= 0xdfff
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); i += 6
                end
            end
            (0xd800 <= cp <= 0xdfff) && (cp = 0xfffd)
            pututf8!(b, cp)
        else
            pushb!(b, x == UInt8('n') ? 0x0a : x == UInt8('t') ? 0x09 : x == UInt8('r') ? 0x0d :
                      x == UInt8('b') ? 0x08 : x == UInt8('f') ? 0x0c : x)
            i += 2
        end
    end
    nothing
end

# Validates d[s:e] as one JSON document. If it is an object, records the top-level fields named in
# `keys` into f. Returns false on a syntax error. `key` is scratch for decoding key names.
function scan_doc!(f::Fields, key::Buf, d::Vector{UInt8}, s::Int, e::Int, keys::NTuple{N,String}) where {N}
    f.isobject = false
    fill!(f.typ, JT_MISSING)
    i = skipws(d, s, e)
    i <= e || return false
    @inbounds if d[i] != UInt8('{')
        j = scan_value(d, i, e, 0)
        return j != 0 && skipws(d, j, e) > e
    end
    f.isobject = true
    i = skipws(d, i + 1, e)
    i <= e || return false
    @inbounds if d[i] == UInt8('}')
        return skipws(d, i + 1, e) > e
    end
    while true
        ks = i
        i = scan_string(d, i, e); i == 0 && return false
        ke = i - 1
        i = skipws(d, i, e)
        (i <= e && @inbounds(d[i]) == UInt8(':')) || return false
        i = skipws(d, i + 1, e)
        vs = i
        i = scan_value(d, i, e, 1); i == 0 && return false
        ve = i - 1
        decode_string!(key, d, ks, ke)
        for k in 1:N
            if bufeq(key, keys[k])
                @inbounds c = d[vs]
                f.typ[k] = c == UInt8('"') ? JT_STRING : (c == UInt8('-') || isdigitb(c)) ? JT_NUMBER : JT_OTHER
                f.vs[k] = vs
                f.ve[k] = ve
            end
        end
        i = skipws(d, i, e)
        i <= e || return false
        @inbounds x = d[i]
        x == UInt8('}') && return skipws(d, i + 1, e) > e
        x == UInt8(',') || return false
        i = skipws(d, i + 1, e)
    end
end

function bufeq(b::Buf, s::String)
    b.len == sizeof(s) || return false
    @inbounds for k in 1:b.len
        b.data[k] == codeunit(s, k) || return false
    end
    true
end

# Parses a validated JSON number span as Float64 (integers fast; others via Base.parse).
function number_value(d::Vector{UInt8}, s::Int, e::Int)
    neg = d[s] == UInt8('-')
    i = neg ? s + 1 : s
    v = 0
    @inbounds while i <= e && isdigitb(d[i]) && v < 1_000_000_000_000_000
        v = v * 10 + Int(d[i] - 0x30)
        i += 1
    end
    if i > e
        return Float64(neg ? -v : v)
    end
    parse(Float64, String(d[s:e]))
end
