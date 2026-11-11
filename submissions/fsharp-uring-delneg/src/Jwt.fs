/// HS256 JWTs (SPEC.md, Auth): every request that needs a token checks its signature, `alg` and `exp`; nothing is
/// remembered between requests (rule 9). By hand, because the challenge's 23-byte secret is shorter than
/// IdentityModel accepts for HS256 (examples/csharp README). Based on our multicore entry's Jwt.fs (from #22, MIT).
module Jwt

open System
open System.Buffers
open System.Buffers.Text
open System.Security.Cryptography
open System.Text
open System.Text.Json
open System.Runtime.InteropServices
open Microsoft.FSharp.NativeInterop

#nowarn "9"

// HMAC-SHA256 on libcrypto's SHA-256 (the OpenSSL 3 library .NET itself uses), called directly: the key's inner and
// outer padded blocks are hashed once at startup, so each token costs its own blocks plus one outer block, with no
// allocation and none of the per-call error-queue and context work of the framework's HMAC class. Every token's
// signature is still computed and compared in full (constant time).
[<Literal>]
let private Crypto = "libcrypto.so.3"

[<DllImport(Crypto); SuppressGCTransition>]
extern int private SHA256_Init(nativeint ctx)

[<DllImport(Crypto); SuppressGCTransition>]
extern int private SHA256_Update(nativeint ctx, nativeint data, unativeint len)

[<DllImport(Crypto); SuppressGCTransition>]
extern int private SHA256_Final(nativeint md, nativeint ctx)

/// sizeof(SHA256_CTX) is 112; room to spare
[<Literal>]
let private CtxSize = 128

let private ctxInner = Marshal.AllocHGlobal CtxSize
let private ctxOuter = Marshal.AllocHGlobal CtxSize
let private ctxWork = Marshal.AllocHGlobal CtxSize
let private digest = Marshal.AllocHGlobal 32

let init (secret: string) =
    let mutable key = Encoding.UTF8.GetBytes secret
    if key.Length > 64 then key <- SHA256.HashData key
    let pad (b: byte) = Array.init 64 (fun i -> (if i < key.Length then key.[i] else 0uy) ^^^ b)
    let block (ctx: nativeint) (data: byte[]) =
        SHA256_Init ctx |> ignore
        let h = GCHandle.Alloc(data, GCHandleType.Pinned)
        SHA256_Update(ctx, h.AddrOfPinnedObject(), 64un) |> ignore
        h.Free()
    block ctxInner (pad 0x36uy)
    block ctxOuter (pad 0x5cuy)

let private mac (data: ReadOnlySpan<byte>) (dst: Span<byte>) =
    use pd = fixed data
    use po = fixed dst
    Buffer.MemoryCopy(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> ctxInner), NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> ctxWork), int64 CtxSize, int64 CtxSize)
    SHA256_Update(ctxWork, NativePtr.toNativeInt pd, unativeint data.Length) |> ignore
    SHA256_Final(digest, ctxWork) |> ignore
    Buffer.MemoryCopy(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> ctxOuter), NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> ctxWork), int64 CtxSize, int64 CtxSize)
    SHA256_Update(ctxWork, digest, 32un) |> ignore
    SHA256_Final(NativePtr.toNativeInt po, ctxWork) |> ignore

/// per-thread scratch (a module cannot hold [<ThreadStatic>] fields); the loop thread is the only caller
type private Tls =
    [<ThreadStatic; DefaultValue>]
    static val mutable private scratch: byte[]
    static member Scratch
        with get () = Tls.scratch
        and set v = Tls.scratch <- v

/// a positive integer written in decimal digits only (at most 18); 0 otherwise
let private decimalId (digits: ReadOnlySpan<byte>) =
    if digits.Length = 0 || digits.Length > 18 then 0L
    else
        let mutable id = 0L
        let mutable ok = true
        for c in digits do
            let d = uint32 (c - '0'B)
            if d > 9u then ok <- false else id <- id * 10L + int64 d
        if ok then id else 0L

let private bAlg = "alg"B
/// the header nearly every library writes: an exact match needs no JSON reader (anything else is checked in full)
let private bStdHeader = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}"B
let private bHs256 = "HS256"B
let private bSub = "sub"B
let private bExp = "exp"B
let private bUsername = "username"B

[<Literal>]
let Valid = 0
/// bad signature, expired, not HS256 or malformed: 401 "invalid or expired token"
[<Literal>]
let Invalid = 1
/// valid JWT, but `sub` isn't a positive integer string or `username` isn't a string: 401 "invalid token payload"
[<Literal>]
let BadPayload = 2

let private segment (b64: ReadOnlySpan<byte>) (into: Span<byte>) (len: byref<int>) =
    let mutable used = 0
    Base64Url.GetMaxDecodedLength b64.Length <= into.Length
    && Base64Url.DecodeFromUtf8(b64, into, &used, &len) = OperationStatus.Done

/// the header: an object whose `alg` is the string "HS256"
let private headerOk (json: ReadOnlySpan<byte>) =
    let mutable reader = Utf8JsonReader(json)
    try
        if not (reader.Read()) || reader.TokenType <> JsonTokenType.StartObject then false
        else
            let mutable alg = false
            let mutable result = false
            let mutable go = true
            while go && reader.Read() do
                if reader.TokenType = JsonTokenType.EndObject then
                    result <- alg
                    go <- false
                else
                    let isAlg = reader.ValueTextEquals(ReadOnlySpan bAlg)
                    if not (reader.Read()) then go <- false
                    elif isAlg then
                        if reader.TokenType = JsonTokenType.String && reader.ValueTextEquals(ReadOnlySpan bHs256) then alg <- true
                        else go <- false
                    else reader.Skip()
            result
    with :? JsonException -> false

/// Verifies `token` (the part after "Bearer "). Returns Valid, Invalid or BadPayload; on Valid, `sub` is the user id and,
/// when `wantName`, `name` the username.
let verify (token: ReadOnlySpan<byte>) (wantName: bool) (sub: byref<int64>) (name: byref<string>) : int =
    sub <- 0L
    if token.Length < 5 || token.Length > 4096 || token.IndexOfAnyExceptInRange('!'B, '~'B) >= 0 then Invalid
    else
        let firstDot = token.IndexOf '.'B
        let lastDot = token.LastIndexOf '.'B
        if firstDot < 1 || lastDot <= firstDot + 1 || token.Slice(firstDot + 1, lastDot - firstDot - 1).Contains '.'B then Invalid
        else
            if isNull Tls.Scratch then Tls.Scratch <- Array.zeroCreate 4096
            let buf = Tls.Scratch
            // nothing in the token is looked at before its signature checks out
            let sigBytes = Span<byte>(buf, 0, 64)
            let mutable sigCount = 0
            if not (segment (token.Slice(lastDot + 1)) sigBytes &sigCount) || sigCount <> 32 then Invalid
            else
                let ours = Span<byte>(buf, 64, 32)
                mac (token.Slice(0, lastDot)) ours
                if not (CryptographicOperations.FixedTimeEquals(ours, Span<byte>(buf, 0, 32))) then Invalid
                else
                    let scratch = Span<byte>(buf, 128, buf.Length - 128)
                    let mutable hl = 0
                    if not (segment (token.Slice(0, firstDot)) scratch &hl) || not (let h = Span<byte>.op_Implicit (scratch.Slice(0, hl)) in h.SequenceEqual(ReadOnlySpan bStdHeader) || headerOk h) then
                        Invalid
                    else
                        let mutable cl = 0
                        if not (segment (token.Slice(firstDot + 1, lastDot - firstDot - 1)) scratch &cl) then Invalid
                        else
                            let mutable reader = Utf8JsonReader(Span<byte>.op_Implicit (scratch.Slice(0, cl)))
                            let mutable exp = 0.0
                            let mutable hasExp = false
                            let mutable id = 0L
                            let mutable nameOk = false
                            let mutable shape = false
                            try
                                if reader.Read() && reader.TokenType = JsonTokenType.StartObject then
                                    let mutable go = true
                                    while go && reader.Read() do
                                        if reader.TokenType = JsonTokenType.EndObject then
                                            shape <- true
                                            go <- false
                                        else
                                            let memb =
                                                if reader.ValueTextEquals(ReadOnlySpan bSub) then 1
                                                elif reader.ValueTextEquals(ReadOnlySpan bExp) then 2
                                                elif reader.ValueTextEquals(ReadOnlySpan bUsername) then 3
                                                else 0
                                            if not (reader.Read()) then go <- false
                                            else
                                                match memb with
                                                | 1 ->
                                                    id <-
                                                        if reader.TokenType = JsonTokenType.String && not reader.ValueIsEscaped then decimalId reader.ValueSpan
                                                        else 0L
                                                | 2 ->
                                                    let mutable e = 0.0
                                                    let mutable ei = 0L
                                                    if reader.TokenType <> JsonTokenType.Number then hasExp <- false
                                                    elif reader.TryGetInt64(&ei) then // the usual integer `exp`, without the float parser
                                                        exp <- float ei
                                                        hasExp <- true
                                                    elif reader.TryGetDouble(&e) then
                                                        exp <- e
                                                        hasExp <- true
                                                    else hasExp <- false
                                                | 3 ->
                                                    nameOk <- reader.TokenType = JsonTokenType.String
                                                    if nameOk && wantName then name <- reader.GetString()
                                                | _ -> reader.Skip()
                            with :? JsonException -> shape <- false
                            if not shape || not hasExp || exp <= float (DateTimeOffset.UtcNow.ToUnixTimeSeconds()) then Invalid
                            elif id = 0L || not nameOk then BadPayload
                            else
                                sub <- id
                                Valid
