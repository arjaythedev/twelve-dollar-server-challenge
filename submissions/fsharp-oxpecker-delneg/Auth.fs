/// HS256 JWT check by hand. The signature and exp are verified on every request (rule 5: nothing is remembered).
module Auth

#nowarn "9"

open System
open System.Buffers.Text
open System.Globalization
open System.Security.Cryptography
open System.Text
open System.Text.Json
open Microsoft.AspNetCore.Http
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

let inline private part (token: ReadOnlySpan<char>) (r: Range) =
    let struct (offset, length) = r.GetOffsetAndLength(token.Length)
    token.Slice(offset, length)

let private isHs256 (json: ReadOnlySpan<byte>) =
    try
        let mutable r = Utf8JsonReader(json)
        if not (r.Read()) || r.TokenType <> JsonTokenType.StartObject then
            false
        else
            let mutable hs256 = false
            while r.Read() && r.TokenType = JsonTokenType.PropertyName do
                let isAlg = r.ValueTextEquals("alg"B)
                r.Read() |> ignore
                if isAlg then
                    hs256 <- r.TokenType = JsonTokenType.String && r.ValueTextEquals("HS256"B)
                r.Skip() // past a nested value, so its own properties are not read as top-level ones
            hs256
    with :? JsonException ->
        false

/// The payload's `sub`: a string of ASCII digits, a positive integer.
let private parseSub (r: byref<Utf8JsonReader>) (id: byref<int64>) =
    if r.TokenType <> JsonTokenType.String || r.ValueIsEscaped || r.ValueSpan.Length > 19 then
        false
    else
        Int64.TryParse(r.ValueSpan, NumberStyles.None, CultureInfo.InvariantCulture, &id) && id > 0L

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
                let isSub = r.ValueTextEquals("sub"B)
                let isName = not isSub && r.ValueTextEquals("username"B)
                let isExp = not isSub && not isName && r.ValueTextEquals("exp"B)
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

/// Checks the Authorization header. Returns the 401 body, or null when the token is valid.
let verify (ctx: HttpContext) (userId: byref<int64>) (username: byref<string>) : byte[] =
    let header: string = ctx.Request.Headers.Authorization.ToString()
    if isNull header || not (header.StartsWith("Bearer ", StringComparison.Ordinal)) then
        MissingBearer
    else
        let token = header.AsSpan(7)
        let parts = stackSpan<Range> 4
        if token.Length > 2048 || MemoryExtensions.Split(token, parts, '.') <> 3 then
            InvalidToken
        else
            let json = stackSpan<byte> 1536
            let mutable n = 0
            if not (Base64Url.TryDecodeFromChars(part token parts.[0], json, &n))
               || not (isHs256 (ro (json.Slice(0, n)))) then
                InvalidToken
            else
                let signingEnd = parts.[1].End.GetOffset(token.Length)
                let input = stackSpan<byte> 2048
                let inputLength = Encoding.ASCII.GetBytes(token.Slice(0, signingEnd), input)
                let expected = stackSpan<byte> 32
                let hmac = Hmac.Get()
                hmac.AppendData(ro (input.Slice(0, inputLength)))
                hmac.GetHashAndReset(expected) |> ignore
                let signature = stackSpan<byte> 64
                let mutable sigLength = 0
                if not (Base64Url.TryDecodeFromChars(part token parts.[2], signature, &sigLength))
                   || sigLength <> 32
                   || not (CryptographicOperations.FixedTimeEquals(ro (signature.Slice(0, 32)), ro expected)) then
                    InvalidToken
                elif not (Base64Url.TryDecodeFromChars(part token parts.[1], json, &n)) then
                    InvalidToken
                else
                    readPayload (ro (json.Slice(0, n))) &userId &username
