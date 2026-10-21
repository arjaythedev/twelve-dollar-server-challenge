/// The five endpoints. Each handler writes one whole HTTP/1.1 response (status line, headers, JSON body) into the
/// connection's output pipe. Reads run start to finish on the socket thread: SQLite is in-process and every query
/// is an index lookup, so nothing waits.
module Api

#nowarn "9"

open System
open System.Buffers
open System.Buffers.Text
open System.Text
open System.Text.Encodings.Web
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

let private statusLine (status: int) : byte[] =
    match status with
    | 200 -> "HTTP/1.1 200 OK\r\n"B
    | 201 -> "HTTP/1.1 201 Created\r\n"B
    | 400 -> "HTTP/1.1 400 Bad Request\r\n"B
    | 401 -> "HTTP/1.1 401 Unauthorized\r\n"B
    | 404 -> "HTTP/1.1 404 Not Found\r\n"B
    | 503 -> "HTTP/1.1 503 Service Unavailable\r\n"B
    | _ -> "HTTP/1.1 500 Internal Server Error\r\n"B

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

// One JSON buffer, used only inside `Db.serialized`, and copied into the pipe before the lock is released.
let private buffer = ArrayBufferWriter<byte>(16384)
let private json =
    new Utf8JsonWriter(
        buffer,
        JsonWriterOptions(Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping, SkipValidation = true) // escaping <>&/ and non-ASCII is optional (SPEC)
    )

let private pId = JsonEncodedText.Encode "id"
let private pBody = JsonEncodedText.Encode "body"
let private pCreatedAt = JsonEncodedText.Encode "created_at"
let private pAuthor = JsonEncodedText.Encode "author"
let private pLikeCount = JsonEncodedText.Encode "like_count"
let private pPosts = JsonEncodedText.Encode "posts"
let private pPost = JsonEncodedText.Encode "post"
let private pStatus = JsonEncodedText.Encode "status"
let private pDb = JsonEncodedText.Encode "db"
let private pUptime = JsonEncodedText.Encode "uptime_s"
let private pError = JsonEncodedText.Encode "error"
let private pLiked = JsonEncodedText.Encode "liked"
let private pAlreadyLiked = JsonEncodedText.Encode "already_liked"
let private pPostId = JsonEncodedText.Encode "post_id"
let private vOk = JsonEncodedText.Encode "ok"
let private vDegraded = JsonEncodedText.Encode "degraded"
let private vUnreachable = JsonEncodedText.Encode "unreachable"

/// JavaScript's String.prototype.trim() set, so bodies are trimmed exactly like the reference servers.
let private jsWhitespace =
    "\t\n\u000b\u000c\r                  　﻿"
        .ToCharArray()

/// Call inside `Db.serialized`: sends what the JSON writer holds as the response body, and clears it.
let private writeJson (w: IBufferWriter<byte>) (status: int) (close: bool) =
    json.Flush()
    respond w status close buffer.WrittenSpan
    json.Reset(buffer)
    buffer.ResetWrittenCount()

let private clear () =
    json.Reset(buffer)
    buffer.ResetWrittenCount()

/// `created_at` is stored in the wire format, so every column is sent as stored, straight from SQLite's UTF-8 bytes.
let private writePost (st: nativeint) =
    json.WriteStartObject()
    json.WriteNumber(pId, Sqlite.column_int64(st, 0))
    json.WriteString(pBody, Sqlite.text st 1)
    json.WriteString(pCreatedAt, Sqlite.text st 2)
    json.WriteString(pAuthor, Sqlite.text st 3)
    json.WriteNumber(pLikeCount, Sqlite.column_int64(st, 4))
    json.WriteEndObject()

// ---- Endpoints ---------------------------------------------------------------------------------------------------

let health (w: IBufferWriter<byte>) (close: bool) =
    Db.serialized (fun () ->
        try
            try
                if not (Db.stepRow Db.ping) then
                    raise (Db.failure ())
            finally
                Sqlite.reset Db.ping |> ignore
            json.WriteStartObject()
            json.WriteString(pStatus, vOk)
            json.WriteString(pDb, vOk)
            json.WriteNumber(pUptime, (Environment.TickCount64 - startedAt) / 1000L)
            json.WriteEndObject()
            writeJson w 200 close
        with e ->
            clear ()
            json.WriteStartObject()
            json.WriteString(pStatus, vDegraded)
            json.WriteString(pDb, vUnreachable)
            json.WriteString(pError, e.Message)
            json.WriteEndObject()
            writeJson w 503 close)

let feed (w: IBufferWriter<byte>) (close: bool) =
    Db.serialized (fun () ->
        let st = Db.feed
        try
            json.WriteStartObject()
            json.WriteStartArray(pPosts)
            while Db.stepRow st do
                writePost st
            json.WriteEndArray()
            json.WriteEndObject()
        finally
            Sqlite.reset st |> ignore
        writeJson w 200 close)

let getPost (w: IBufferWriter<byte>) (close: bool) (id: int64) =
    let found =
        Db.serialized (fun () ->
            let st = Db.post
            try
                Sqlite.bind_int64(st, 1, id) |> ignore
                if Db.stepRow st then
                    json.WriteStartObject()
                    json.WritePropertyName(pPost)
                    writePost st
                    json.WriteEndObject()
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
                json.WriteStartObject()
                json.WriteBoolean(pLiked, true)
                json.WriteBoolean(pAlreadyLiked, not inserted)
                json.WriteNumber(pPostId, id)
                json.WriteEndObject()
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
            elif r.CurrentDepth = 1 && r.TokenType = JsonTokenType.PropertyName && r.ValueTextEquals("body"B) then
                r.Read() |> ignore
                value <- if r.TokenType = JsonTokenType.String then r.GetString() else null
                if r.TokenType = JsonTokenType.StartObject || r.TokenType = JsonTokenType.StartArray then
                    r.Skip()
    with :? JsonException ->
        malformed <- true
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
            json.WriteStartObject()
            json.WriteStartObject(pPost)
            json.WriteNumber(pId, Sqlite.column_int64(st, 0))
            json.WriteString(pBody, body)
            json.WriteString(pCreatedAt, Sqlite.text st 1)
            json.WriteString(pAuthor, username)
            json.WriteNumber(pLikeCount, 0)
            json.WriteEndObject()
            json.WriteEndObject()
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
