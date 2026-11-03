/// The five endpoints. Each handler writes one whole HTTP/1.1 response (status line, headers, JSON body) into the
/// connection's output pipe. Reads run start to finish on the socket thread: SQLite is in-process and every query
/// is an index lookup, so nothing waits.
module Api

#nowarn "9"

open System
open System.Buffers
open System.Buffers.Text
open System.Text
open System.Text.Json
open System.Threading.Tasks

let NotFound = """{"error":"not found"}"""B
let InvalidId = """{"error":"invalid post id"}"""B
let PostNotFound = """{"error":"post not found"}"""B
let Malformed = """{"error":"malformed JSON body"}"""B
let BodyRequired = """{"error":"body is required"}"""B
let BodyTooLong = """{"error":"body must be at most 500 characters"}"""B
let Internal = """{"error":"internal server error"}"""B

let private startedAt = Environment.TickCount64

// ---- HTTP framing ------------------------------------------------------------------------------------------------

let private s200 = "HTTP/1.1 200 OK\r\n"B
let private s201 = "HTTP/1.1 201 Created\r\n"B
let private s400 = "HTTP/1.1 400 Bad Request\r\n"B
let private s401 = "HTTP/1.1 401 Unauthorized\r\n"B
let private s404 = "HTTP/1.1 404 Not Found\r\n"B
let private s503 = "HTTP/1.1 503 Service Unavailable\r\n"B
let private s500 = "HTTP/1.1 500 Internal Server Error\r\n"B

/// The status line. The arrays are made once (an F# "..."B literal in a function makes a new array each call).
let private statusLine (status: int) : byte[] =
    match status with
    | 200 -> s200
    | 201 -> s201
    | 400 -> s400
    | 401 -> s401
    | 404 -> s404
    | 503 -> s503
    | _ -> s500

let private headers1 = "Content-Type: application/json\r\nDate: "B
let private headers2 = "\r\nContent-Length: "B
let private closeHeader = "\r\nConnection: close"B
let private crlf2 = "\r\n\r\n"B

/// The Date header value (RFC 1123, 29 bytes), formatted again only when the second changes.
let private date = Array.zeroCreate<byte> 29
let mutable private dateSecond = 0L

let private refreshDate () =
    let now = DateTime.UtcNow
    let s = now.Ticks / TimeSpan.TicksPerSecond
    if s <> dateSecond then
        let mutable n = 0
        Utf8Formatter.TryFormat(now, Span date, &n, StandardFormat 'R') |> ignore
        dateSecond <- s

/// Writes a whole response into the pipe: status line, Content-Type, Date, Content-Length, then the body.
let respond (w: IBufferWriter<byte>) (status: int) (close: bool) (body: ReadOnlySpan<byte>) =
    refreshDate ()
    let line = statusLine status
    let size = line.Length + headers1.Length + 29 + headers2.Length + 11 + closeHeader.Length + 4 + body.Length
    let span = w.GetSpan size
    let mutable i = 0
    line.CopyTo(span)
    i <- line.Length
    headers1.CopyTo(span.Slice i)
    i <- i + headers1.Length
    date.CopyTo(span.Slice i)
    i <- i + 29
    headers2.CopyTo(span.Slice i)
    i <- i + headers2.Length
    let mutable n = 0
    Utf8Formatter.TryFormat(body.Length, span.Slice i, &n) |> ignore
    i <- i + n
    if close then
        closeHeader.CopyTo(span.Slice i)
        i <- i + closeHeader.Length
    crlf2.CopyTo(span.Slice i)
    i <- i + 4
    body.CopyTo(span.Slice i)
    w.Advance(i + body.Length)

let send (w: IBufferWriter<byte>) (status: int) (close: bool) (body: byte[]) = respond w status close (ReadOnlySpan body)

// ---- JSON --------------------------------------------------------------------------------------------------------

// The JSON body being written: one buffer, used only inside `Db.serialized`, and copied into the pipe before the lock
// is released. Every value comes from SQLite or the token as UTF-8 and goes out as those bytes, with only the
// escapes JSON requires (SPEC: escaping <>&/ and non-ASCII is optional).
let mutable private buf = Array.zeroCreate<byte> 16384
let mutable private pos = 0

let private ensure (n: int) =
    if pos + n > buf.Length then
        let b = Array.zeroCreate<byte> (max (buf.Length * 2) (pos + n))
        Buffer.BlockCopy(buf, 0, b, 0, pos)
        buf <- b

let private copy (s: ReadOnlySpan<byte>) =
    ensure s.Length
    s.CopyTo(Span<byte>(buf, pos, s.Length))
    pos <- pos + s.Length

let private raw (s: byte[]) = copy (ReadOnlySpan s)

let private put (b: byte) =
    ensure 1
    buf.[pos] <- b
    pos <- pos + 1

let private num (v: int64) =
    ensure 20
    let mutable n = 0
    Utf8Formatter.TryFormat(v, Span<byte>(buf, pos, 20), &n) |> ignore
    pos <- pos + n

/// The bytes a JSON string must escape: the quote, the backslash and the control characters.
let private special = SearchValues.Create(ReadOnlySpan(Array.append [| byte '"'; byte '\\' |] [| 0uy .. 31uy |]))
let private hex = "0123456789abcdef"B

/// A JSON string, quotes included, from UTF-8 bytes. Bytes from 0x80 up pass unchanged: SQLite holds the UTF-8 that
/// `Encoding.UTF8` produced, so it is valid.
let private str (s: ReadOnlySpan<byte>) =
    put '"'B
    let mutable rest = s
    let mutable i = rest.IndexOfAny special
    while i >= 0 do
        copy (rest.Slice(0, i))
        let c = rest.[i]
        ensure 6
        buf.[pos] <- '\\'B
        match c with
        | '"'B | '\\'B -> buf.[pos + 1] <- c; pos <- pos + 2
        | '\n'B -> buf.[pos + 1] <- 'n'B; pos <- pos + 2
        | '\r'B -> buf.[pos + 1] <- 'r'B; pos <- pos + 2
        | '\t'B -> buf.[pos + 1] <- 't'B; pos <- pos + 2
        | '\b'B -> buf.[pos + 1] <- 'b'B; pos <- pos + 2
        | 12uy -> buf.[pos + 1] <- 'f'B; pos <- pos + 2
        | _ ->
            buf.[pos + 1] <- 'u'B
            buf.[pos + 2] <- '0'B
            buf.[pos + 3] <- '0'B
            buf.[pos + 4] <- hex.[int c >>> 4]
            buf.[pos + 5] <- hex.[int c &&& 15]
            pos <- pos + 6
        rest <- rest.Slice(i + 1)
        i <- rest.IndexOfAny special
    copy rest
    put '"'B

let private strOf (s: string) = str (ReadOnlySpan(Encoding.UTF8.GetBytes s))

/// JavaScript's String.prototype.trim() set, so bodies are trimmed exactly like the reference servers.
let private jsWhitespace =
    "\t\n\u000b\u000c\r                  　﻿"
        .ToCharArray()

/// Call inside `Db.serialized`: sends the JSON body as the response, and clears it.
let private writeJson (w: IBufferWriter<byte>) (status: int) (close: bool) =
    respond w status close (ReadOnlySpan<byte>(buf, 0, pos))
    pos <- 0

let private clear () = pos <- 0

/// Drops what a failed handler left in the JSON buffer (call before the 500), so the next response starts clean.
let discardJson () = clear ()

let private kId = "{\"id\":"B
let private kBody = ",\"body\":"B
let private kCreatedAt = ",\"created_at\":"B
let private kAuthor = ",\"author\":"B
let private kLikeCount = ",\"like_count\":"B
let private kPosts = "{\"posts\":["B
let private kPostsEnd = "]}"B
let private kPost = "{\"post\":"B
let private kHealthOk = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"B
let private kHealthDown = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"B
let private kLiked = "{\"liked\":true,\"already_liked\":false,\"post_id\":"B
let private kAlreadyLiked = "{\"liked\":true,\"already_liked\":true,\"post_id\":"B

/// One post object. `created_at` is stored in the wire format, so every column goes out as stored.
let private writePost (id: int64) (body: ReadOnlySpan<byte>) (createdAt: ReadOnlySpan<byte>) (author: ReadOnlySpan<byte>) (likes: int64) =
    raw kId
    num id
    raw kBody
    str body
    raw kCreatedAt
    str createdAt
    raw kAuthor
    str author
    raw kLikeCount
    num likes
    put '}'B

let private writePostRow (st: nativeint) =
    writePost (Sqlite.column_int64(st, 0)) (Sqlite.text st 1) (Sqlite.text st 2) (Sqlite.text st 3) (Sqlite.column_int64(st, 4))

// ---- Endpoints ---------------------------------------------------------------------------------------------------

let health (w: IBufferWriter<byte>) (close: bool) =
    Db.serialized (fun () ->
        try
            try
                Db.beginRead ()
                if not (Db.stepRow Db.ping) then
                    raise (Db.failure ())
            finally
                Sqlite.reset Db.ping |> ignore
            raw kHealthOk
            num ((Environment.TickCount64 - startedAt) / 1000L)
            put '}'B
            writeJson w 200 close
        with e ->
            clear ()
            raw kHealthDown
            strOf e.Message
            put '}'B
            writeJson w 503 close)

/// The posts of a fast feed: ids (newest first) and their like counts.
let private feedIds = Array.zeroCreate<int64> 20
let private feedCounts = Array.zeroCreate<int64> 20

/// The feed when its 20 posts are one run of consecutive ids, newest first (the usual case: posts are created in
/// id order): one rowid range scan for the posts and one range scan of the likes index for all 20 counts, instead
/// of 20 lookups of each (source: #154, #152, #82). Same transaction as the id query, so one snapshot.
/// False (and nothing written) if the rows do not match the ids; then the caller runs the plain query.
let private feedRange (n: int) =
    let hi = feedIds.[0]
    let lo = feedIds.[n - 1]
    Array.Clear(feedCounts, 0, n)
    let lr = Db.likeRange
    try
        Sqlite.bind_int64(lr, 1, lo) |> ignore
        Sqlite.bind_int64(lr, 2, hi) |> ignore
        while Db.stepRow lr do
            feedCounts.[int (hi - Sqlite.column_int64(lr, 0))] <- Sqlite.column_int64(lr, 1)
    finally
        Sqlite.reset lr |> ignore
    let st = Db.feedRange
    let mutable k = 0
    let mutable ok = true
    try
        Sqlite.bind_int64(st, 1, lo) |> ignore
        Sqlite.bind_int64(st, 2, hi) |> ignore
        raw kPosts
        while ok && Db.stepRow st do
            if k < n && Sqlite.column_int64(st, 0) = feedIds.[k] then
                if k > 0 then put ','B
                writePost feedIds.[k] (Sqlite.text st 1) (Sqlite.text st 2) (Sqlite.text st 3) feedCounts.[k]
                k <- k + 1
            else
                ok <- false
        raw kPostsEnd
    finally
        Sqlite.reset st |> ignore
    if ok && k = n then true
    else
        clear ()
        false

let feed (w: IBufferWriter<byte>) (close: bool) =
    Db.serialized (fun () ->
        Db.beginRead ()
        let ids = Db.feedIds
        let mutable n = 0
        try
            while n < 20 && Db.stepRow ids do
                feedIds.[n] <- Sqlite.column_int64(ids, 0)
                n <- n + 1
        finally
            Sqlite.reset ids |> ignore
        let mutable run = n > 0
        for i in 1 .. n - 1 do
            if feedIds.[i] <> feedIds.[i - 1] - 1L then run <- false
        if not (run && feedRange n) then
            let st = Db.feed
            try
                raw kPosts
                let mutable first = true
                while Db.stepRow st do
                    if not first then put ','B
                    first <- false
                    writePostRow st
                raw kPostsEnd
            finally
                Sqlite.reset st |> ignore
        writeJson w 200 close)

let getPost (w: IBufferWriter<byte>) (close: bool) (id: int64) =
    let found =
        Db.serialized (fun () ->
            let st = Db.post
            Db.beginRead ()
            try
                Sqlite.bind_int64(st, 1, id) |> ignore
                if Db.stepRow st then
                    raw kPost
                    writePostRow st
                    put '}'B
                    writeJson w 200 close
                    true
                else
                    false
            finally
                Sqlite.reset st |> ignore)
    if not found then send w 404 close PostNotFound

let private postExists (id: int64) =
    let st = Db.postExists
    try
        Sqlite.bind_int64(st, 1, id) |> ignore
        Db.stepRow st
    finally
        Sqlite.reset st |> ignore

/// Returns null when the response is written, or a Task that writes it (only while a WAL catch-up holds writes).
let like (w: IBufferWriter<byte>) (close: bool) (userId: int64) (id: int64) : Task =
    let likeIt () =
        Db.serialized (fun () ->
            let st = Db.insertLike
            Db.beginWrite ()
            let inserted =
                try
                    Sqlite.bind_int64(st, 1, userId) |> ignore
                    Sqlite.bind_int64(st, 2, id) |> ignore
                    // Stepping to DONE finishes the statement. With group commit, the loop sends the response after the COMMIT.
                    if Sqlite.step st <> Sqlite.Done then
                        raise (Db.failure ())
                    Db.writes <- Db.writes + 1
                    Db.changes () = 1
                finally
                    Sqlite.reset st |> ignore
            if inserted || postExists id then
                raw (if inserted then kLiked else kAlreadyLiked)
                num id
                put '}'B
                writeJson w (if inserted then 201 else 200) close
                true
            else
                false)
    let r = Db.runWrite likeIt
    if r.IsCompletedSuccessfully then
        if not r.Result then send w 404 close PostNotFound
        null
    else
        task {
            let! found = r
            if not found then send w 404 close PostNotFound
        }
        :> Task

// Byte strings made once: an F# "..."B literal inside a function allocates a new array each time it runs.
let private kBodyName = "body"B

/// The 400 body, or null when the post body is valid. Reads the whole document, so trailing junk is an error.
let private parseBody (data: ReadOnlySpan<byte>) (body: byref<string>) : byte[] =
    let mutable value: string = null
    let mutable isObject = false
    let mutable malformed = false
    try
        let mutable r = Utf8JsonReader(data)
        while r.Read() do
            if r.CurrentDepth = 0 && r.TokenType = JsonTokenType.StartObject then
                isObject <- true
            elif r.CurrentDepth = 1 && r.TokenType = JsonTokenType.PropertyName && r.ValueTextEquals(kBodyName) then
                r.Read() |> ignore
                value <- if r.TokenType = JsonTokenType.String then r.GetString() else null
                if r.TokenType = JsonTokenType.StartObject || r.TokenType = JsonTokenType.StartArray then
                    r.Skip()
    // GetString throws InvalidOperationException for a lone surrogate escape (\ud800): not valid UTF-16, so 400 too.
    with
    | :? JsonException
    | :? InvalidOperationException -> malformed <- true
    if malformed then
        Malformed
    elif not isObject || isNull value then
        BodyRequired
    else
        let trimmed = value.Trim(jsWhitespace)
        if trimmed.Length = 0 then BodyRequired
        elif trimmed.Length > 500 then BodyTooLong
        else
            body <- trimmed
            null

let private insertPost (w: IBufferWriter<byte>) (close: bool) (userId: int64) (username: string) (body: string) =
    let utf8 = Encoding.UTF8.GetBytes body
    Db.serialized (fun () ->
        let st = Db.insertPost
        Db.beginWrite ()
        try
            Sqlite.bind_int64(st, 1, userId) |> ignore
            use p = fixed utf8
            Sqlite.bind_text(st, 2, NativeInterop.NativePtr.toNativeInt p, utf8.Length, Sqlite.Transient) |> ignore
            if Sqlite.step st <> Sqlite.Row then
                raise (Db.failure ())
            raw kPost
            writePost (Sqlite.column_int64(st, 0)) (ReadOnlySpan utf8) (Sqlite.text st 1) (ReadOnlySpan(Encoding.UTF8.GetBytes username)) 0L
            put '}'B
            // Stepping to DONE finishes the statement. With group commit, the loop sends the response after the COMMIT.
            if Sqlite.step st <> Sqlite.Done then
                clear ()
                raise (Db.failure ())
            Db.writes <- Db.writes + 1
            writeJson w 201 close
        finally
            Sqlite.reset st |> ignore)

/// Returns null when the response is written, or a Task that writes it.
let createPost (w: IBufferWriter<byte>) (close: bool) (userId: int64) (username: string) (data: ReadOnlySpan<byte>) : Task =
    let mutable body: string = null
    match parseBody data &body with
    | null ->
        let body = body
        let r = Db.runWrite (fun () -> insertPost w close userId username body)
        if r.IsCompletedSuccessfully then null else r.AsTask() :> Task
    | error ->
        send w 400 close error
        null
