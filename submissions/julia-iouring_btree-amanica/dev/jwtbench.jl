# Dev-only microbenchmark of HS256 signature variants (PLAN 7.1). Runs inside the dev container:
# dev/ct.sh jwtbench. Every variant computes the full HMAC over each token (rule 5); they differ only in
# what is set up once (the key) and what each call pays for (OpenSSL 3's one-shot HMAC() fetches the
# digest, allocates a context and hashes both key pads on every call). Each must match HMAC() exactly.
include(joinpath(@__DIR__, "..", "src", "JuliaRaw.jl"))
const J = JuliaRaw
const C = J.LIBCRYPTO

const N = 1_000_000
const KEY = Vector{UInt8}(codeunits("twelve-dollar-challenge"))
const TOKEN = let s = read(joinpath(@__DIR__, "..", "..", "..", "seed", "tokens.json"), String)
    m = match(r"\"token\":\"([^\"]+)\"", s)
    Vector{UInt8}(codeunits(m.captures[1]))
end
const MSGLEN = findlast(==(UInt8('.')), TOKEN) - 1          # header.payload

const EVP = ccall((:EVP_sha256, C), Ptr{Cvoid}, ())

# 1. what the server does now
function oneshot!(mac)
    GC.@preserve TOKEN KEY mac ccall((:HMAC, C), Ptr{UInt8},
        (Ptr{Cvoid}, Ptr{UInt8}, Cint, Ptr{UInt8}, Csize_t, Ptr{UInt8}, Ptr{Cuint}),
        EVP, KEY, length(KEY), TOKEN, MSGLEN, mac, C_NULL)
end

# 2. one HMAC_CTX keyed once; Init_ex with a NULL key reuses the key pads
const HCTX = Ref{Ptr{Cvoid}}(C_NULL)
function hctx_init()
    HCTX[] = ccall((:HMAC_CTX_new, C), Ptr{Cvoid}, ())
    ccall((:HMAC_Init_ex, C), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint, Ptr{Cvoid}, Ptr{Cvoid}),
        HCTX[], KEY, length(KEY), EVP, C_NULL) == 1 || error("HMAC_Init_ex")
end
function hctx!(mac)
    c = HCTX[]
    ccall((:HMAC_Init_ex, C), Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cint, Ptr{Cvoid}, Ptr{Cvoid}), c, C_NULL, 0, C_NULL, C_NULL)
    GC.@preserve TOKEN ccall((:HMAC_Update, C), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Csize_t), c, TOKEN, MSGLEN)
    GC.@preserve mac ccall((:HMAC_Final, C), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Ptr{Cuint}), c, mac, C_NULL)
end

# 3. SHA-256 states after the inner and outer key pads, copied per call (SHA256_CTX is 112 bytes)
const CTXSZ = 112
const INNER = zeros(UInt8, CTXSZ); const OUTER = zeros(UInt8, CTXSZ); const WORK = zeros(UInt8, CTXSZ)
const IH = zeros(UInt8, 32)
function pads_init()
    k = zeros(UInt8, 64); copyto!(k, KEY)                   # keys up to 64 bytes are used as is
    for (st, x) in ((INNER, 0x36), (OUTER, 0x5c))
        pad = k .⊻ x
        ccall((:SHA256_Init, C), Cint, (Ptr{UInt8},), st)
        ccall((:SHA256_Update, C), Cint, (Ptr{UInt8}, Ptr{UInt8}, Csize_t), st, pad, 64)
    end
end
function pads!(mac)
    copyto!(WORK, INNER)
    GC.@preserve WORK TOKEN ccall((:SHA256_Update, C), Cint, (Ptr{UInt8}, Ptr{UInt8}, Csize_t), WORK, TOKEN, MSGLEN)
    GC.@preserve WORK ccall((:SHA256_Final, C), Cint, (Ptr{UInt8}, Ptr{UInt8}), IH, WORK)
    copyto!(WORK, OUTER)
    GC.@preserve WORK ccall((:SHA256_Update, C), Cint, (Ptr{UInt8}, Ptr{UInt8}, Csize_t), WORK, IH, 32)
    GC.@preserve WORK mac ccall((:SHA256_Final, C), Cint, (Ptr{UInt8}, Ptr{UInt8}), mac, WORK)
end

# whole verify_jwt as the server runs it (decode, HMAC, JSON, exp)
function verify!(mac)
    st, uid = J.verify_jwt(TOKEN, 1, length(TOKEN), time())
    st == J.AUTH_OK || error("verify_jwt: $st")
    uid
end

function bench(name, f!, ref)
    mac = zeros(UInt8, 32)
    f!(mac)
    ref === nothing || mac == ref || error("$name: MAC differs")
    for _ in 1:10_000; f!(mac); end
    a0 = Base.gc_bytes()
    t = @elapsed for _ in 1:N; f!(mac); end
    a = Base.gc_bytes() - a0
    println(rpad(name, 12), lpad(string(round(t / N * 1e9, digits = 0)), 6), " ns/call   ", a ÷ N, " B/call")
    mac
end

J.init_jwt(String(copy(KEY)))
hctx_init(); pads_init()
println("token $(length(TOKEN)) B, signed part $MSGLEN B, $N calls each")
for round in 1:3
    println("-- round $round")
    ref = bench("oneshot", oneshot!, nothing)
    bench("hmac_ctx", hctx!, ref)
    bench("pads", pads!, ref)
    bench("verify_jwt", verify!, nothing)
end
