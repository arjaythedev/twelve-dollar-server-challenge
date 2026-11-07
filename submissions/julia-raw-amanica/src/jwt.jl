# HS256 JWT verification. Every token's signature is recomputed (rule 5: no remembered verifications).

const LIBCRYPTO = "libcrypto.so.3"
const JWT_KEY = UInt8[]
const EVP_SHA256 = Ref{Ptr{Cvoid}}(C_NULL)

const AUTH_OK = 0
const AUTH_MISSING = 1     # 401 missing bearer token
const AUTH_INVALID = 2     # 401 invalid or expired token
const AUTH_PAYLOAD = 3     # 401 invalid token payload

const MAC = zeros(UInt8, 32)
const JSIG = Buf(64)
const JHDR = Buf(256)
const JPAY = Buf(512)
const JSUB = Buf(32)
const JKEY = Buf(64)
const JF = Fields()
const USERNAME = Buf(128)  # decoded username of the last verified token

function init_jwt(secret::String)
    append!(empty!(JWT_KEY), codeunits(secret))
    EVP_SHA256[] = ccall((:EVP_sha256, LIBCRYPTO), Ptr{Cvoid}, ())
    nothing
end

const B64URL = let t = fill(Int8(-1), 256)
    for (i, c) in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
        t[UInt8(c)+1] = Int8(i - 1)
    end
    t
end

function b64url_decode!(out::Buf, d::Vector{UInt8}, s::Int, e::Int)
    reset!(out)
    @inbounds while e >= s && d[e] == UInt8('=')      # tolerate padding
        e -= 1
    end
    n = e - s + 1
    n % 4 == 1 && return false
    acc = UInt32(0)
    bits = 0
    @inbounds for i in s:e
        v = B64URL[d[i]+1]
        v < 0 && return false
        acc = (acc << 6) | UInt32(v)
        bits += 6
        if bits >= 8
            bits -= 8
            pushb!(out, ((acc >> bits) & 0xff) % UInt8)
        end
    end
    true
end

@inline function str_is(f::Fields, k::Int, d::Vector{UInt8}, scratch::Buf, lit::String)
    f.typ[k] == JT_STRING || return false
    decode_string!(scratch, d, f.vs[k], f.ve[k])
    bufeq(scratch, lit)
end

# Verifies the token d[s:e]. Returns (status, user_id); on AUTH_OK the username is in USERNAME.
function verify_jwt(d::Vector{UInt8}, s::Int, e::Int, now::Float64)
    d1 = 0; d2 = 0
    @inbounds for i in s:e
        if d[i] == UInt8('.')
            if d1 == 0
                d1 = i
            elseif d2 == 0
                d2 = i
            else
                return (AUTH_INVALID, 0)
            end
        end
    end
    (d1 > s && d2 > d1 + 1 && d2 < e) || return (AUTH_INVALID, 0)

    b64url_decode!(JHDR, d, s, d1 - 1) || return (AUTH_INVALID, 0)
    scan_doc!(JF, JKEY, JHDR.data, 1, JHDR.len, ("alg",)) || return (AUTH_INVALID, 0)
    (JF.isobject && str_is(JF, 1, JHDR.data, JKEY, "HS256")) || return (AUTH_INVALID, 0)

    b64url_decode!(JSIG, d, d2 + 1, e) || return (AUTH_INVALID, 0)
    JSIG.len == 32 || return (AUTH_INVALID, 0)
    GC.@preserve d ccall((:HMAC, LIBCRYPTO), Ptr{UInt8},
        (Ptr{Cvoid}, Ptr{UInt8}, Cint, Ptr{UInt8}, Csize_t, Ptr{UInt8}, Ptr{Cuint}),
        EVP_SHA256[], JWT_KEY, length(JWT_KEY), pointer(d, s), d2 - s, MAC, C_NULL)
    diff = 0x00
    @inbounds for i in 1:32
        diff |= MAC[i] ⊻ JSIG.data[i]
    end
    diff == 0x00 || return (AUTH_INVALID, 0)

    b64url_decode!(JPAY, d, d1 + 1, d2 - 1) || return (AUTH_INVALID, 0)
    scan_doc!(JF, JKEY, JPAY.data, 1, JPAY.len, ("exp", "sub", "username")) || return (AUTH_INVALID, 0)
    JF.isobject || return (AUTH_INVALID, 0)
    JF.typ[1] == JT_NUMBER || return (AUTH_INVALID, 0)
    number_value(JPAY.data, JF.vs[1], JF.ve[1]) > now || return (AUTH_INVALID, 0)

    JF.typ[2] == JT_STRING || return (AUTH_PAYLOAD, 0)
    decode_string!(JSUB, JPAY.data, JF.vs[2], JF.ve[2])
    uid = 0
    (1 <= JSUB.len <= 18) || return (AUTH_PAYLOAD, 0)
    @inbounds for i in 1:JSUB.len
        c = JSUB.data[i]
        isdigitb(c) || return (AUTH_PAYLOAD, 0)
        uid = uid * 10 + Int(c - 0x30)
    end
    uid > 0 || return (AUTH_PAYLOAD, 0)
    JF.typ[3] == JT_STRING || return (AUTH_PAYLOAD, 0)
    decode_string!(USERNAME, JPAY.data, JF.vs[3], JF.ve[3])
    (AUTH_OK, uid)
end
