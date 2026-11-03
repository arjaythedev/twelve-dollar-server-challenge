/// HS256 JWT check by hand, on the raw header bytes. The signature and exp are verified on every request
/// (rule 5: nothing is remembered).
module Auth

#nowarn "9"

open System
open System.Buffers.Text
open System.Security.Cryptography
open System.Text.Json
open Microsoft.FSharp.NativeInterop

let mutable key: byte[] = [||]

/// One HMAC-SHA256 context per thread, made once with the key. Each token is still hashed in full (rule 5);
/// only the OpenSSL context set-up is reused.
type private Hmac =
    [<ThreadStatic; DefaultValue>]
    static val mutable private ctx: IncrementalHash
    static member Get() =
        if isNull Hmac.ctx then
            Hmac.ctx <- IncrementalHash.CreateHMAC(HashAlgorithmName.SHA256, key)
        Hmac.ctx

let MissingBearer = """{"error":"missing bearer token"}"""B
let InvalidToken = """{"error":"invalid or expired token"}"""B
let InvalidPayload = """{"error":"invalid token payload"}"""B

let inline private stackSpan<'T when 'T: unmanaged> (n: int) =
    Span<'T>(NativePtr.toVoidPtr (NativePtr.stackalloc<'T> n), n)

let inline private ro (s: Span<byte>) : ReadOnlySpan<byte> = Span<byte>.op_Implicit s

// Byte strings made once: an F# "..."B literal inside a function allocates a new array each time it runs.
let private kAlg = "alg"B
let private vHs256 = "HS256"B
let private kSub = "sub"B
let private kUsername = "username"B
let private kExp = "exp"B
let private bBearer = "Bearer "B

let private isHs256 (json: ReadOnlySpan<byte>) =
    try
        let mutable r = Utf8JsonReader(json)
        if not (r.Read()) || r.TokenType <> JsonTokenType.StartObject then
            false
        else
            let mutable hs256 = false
            while r.Read() && r.TokenType = JsonTokenType.PropertyName do
                let isAlg = r.ValueTextEquals(kAlg)
                r.Read() |> ignore
                if isAlg then
                    hs256 <- r.TokenType = JsonTokenType.String && r.ValueTextEquals(vHs256)
                r.Skip() // past a nested value, so its own properties are not read as top-level ones
            hs256
    with :? JsonException ->
        false

/// The payload's `sub`: a string of ASCII digits, a positive integer.
let private parseSub (r: byref<Utf8JsonReader>) (id: byref<int64>) =
    if r.TokenType <> JsonTokenType.String || r.ValueIsEscaped || r.ValueSpan.Length > 18 || r.ValueSpan.Length = 0 then
        false
    else
        let s = r.ValueSpan
        let mutable v = 0L
        let mutable ok = true
        for i in 0 .. s.Length - 1 do
            let c = s.[i]
            if c < '0'B || c > '9'B then ok <- false else v <- v * 10L + int64 (c - '0'B)
        id <- v
        ok && v > 0L

let private readPayload (json: ReadOnlySpan<byte>) (userId: byref<int64>) (username: byref<string>) : byte[] =
    let mutable subOk = false
    let mutable name: string = null
    let mutable exp = Double.NaN
    let mutable bad = false
    try
        let mutable r = Utf8JsonReader(json)
        if not (r.Read()) || r.TokenType <> JsonTokenType.StartObject then
            bad <- true
        else
            while not bad && r.Read() && r.TokenType = JsonTokenType.PropertyName do
                let isSub = r.ValueTextEquals(kSub)
                let isName = not isSub && r.ValueTextEquals(kUsername)
                let isExp = not isSub && not isName && r.ValueTextEquals(kExp)
                r.Read() |> ignore
                if isSub then
                    subOk <- parseSub &r &userId
                elif isName then
                    name <- if r.TokenType = JsonTokenType.String then r.GetString() else null
                elif isExp then
                    if r.TokenType <> JsonTokenType.Number then bad <- true else exp <- r.GetDouble()
                r.Skip()
    with :? JsonException ->
        bad <- true
    if bad || Double.IsNaN exp || float (DateTimeOffset.UtcNow.ToUnixTimeSeconds()) >= exp then
        InvalidToken
    elif not subOk || isNull name then
        InvalidPayload
    else
        username <- name
        null

/// Checks the Authorization header value (raw bytes; empty when there is none).
/// Returns the 401 body, or null when the token is valid.
let verify (header: ReadOnlySpan<byte>) (userId: byref<int64>) (username: byref<string>) : byte[] =
    if not (header.StartsWith(bBearer)) then
        MissingBearer
    else
        let token = header.Slice 7
        let d1 = token.IndexOf '.'B
        let d2 = if d1 < 0 then -1 else (let j = token.Slice(d1 + 1).IndexOf '.'B in if j < 0 then -1 else d1 + 1 + j)
        if token.Length > 2048 || d1 < 0 || d2 < 0 || token.Slice(d2 + 1).IndexOf '.'B >= 0 then
            InvalidToken
        else
            let json = stackSpan<byte> 1536
            let mutable consumed = 0
            let mutable n = 0
            if Base64Url.DecodeFromUtf8(token.Slice(0, d1), json, &consumed, &n) <> Buffers.OperationStatus.Done
               || not (isHs256 (ro (json.Slice(0, n)))) then
                InvalidToken
            else
                let expected = stackSpan<byte> 32
                let hmac = Hmac.Get()
                hmac.AppendData(token.Slice(0, d2))
                hmac.GetHashAndReset(expected) |> ignore
                let signature = stackSpan<byte> 64
                let mutable sigLength = 0
                if Base64Url.DecodeFromUtf8(token.Slice(d2 + 1), signature, &consumed, &sigLength) <> Buffers.OperationStatus.Done
                   || sigLength <> 32
                   || not (CryptographicOperations.FixedTimeEquals(ro (signature.Slice(0, 32)), ro expected)) then
                    InvalidToken
                elif Base64Url.DecodeFromUtf8(token.Slice(d1 + 1, d2 - d1 - 1), json, &consumed, &n) <> Buffers.OperationStatus.Done then
                    InvalidToken
                else
                    readPayload (ro (json.Slice(0, n))) &userId &username
