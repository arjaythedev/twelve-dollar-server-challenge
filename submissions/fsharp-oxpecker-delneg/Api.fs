/// The five endpoints as Oxpecker endpoint handlers. Reads run start to finish on the socket thread:
/// SQLite is in-process and every query is an index lookup, so nothing waits.
module Api

#nowarn "9"

open System
open System.Buffers
open System.Globalization
open System.Text
open System.Text.Encodings.Web
open System.Text.Json
open System.Threading.Tasks
open Microsoft.AspNetCore.Http
open Oxpecker

let NotFound = """{"error":"not found"}"""B
let InvalidId = """{"error":"invalid post id"}"""B
let PostNotFound = """{"error":"post not found"}"""B
let Malformed = """{"error":"malformed JSON body"}"""B
let BodyRequired = """{"error":"body is required"}"""B
let BodyTooLong = """{"error":"body must be at most 500 characters"}"""B
let Internal = """{"error":"internal server error"}"""B

let private startedAt = Environment.TickCount64

// One JSON buffer, used only while `Db.gate` is held, and copied into Kestrel's pipe before the lock is released.
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

/// Writes the body with a Content-Length into Kestrel's pipe. Only a flush that cannot finish at once becomes a Task.
let private write (ctx: HttpContext) (status: int) (body: ReadOnlySpan<byte>) =
    let r = ctx.Response
    r.StatusCode <- status
    r.ContentType <- "application/json"
    r.ContentLength <- Nullable(int64 body.Length)
    r.BodyWriter.Write(body)

let private flush (ctx: HttpContext) : Task =
    let f = ctx.Response.BodyWriter.FlushAsync()
    if f.IsCompletedSuccessfully then Task.CompletedTask else f.AsTask()

let send (ctx: HttpContext) (status: int) (body: byte[]) : Task =
    write ctx status (ReadOnlySpan body)
    flush ctx

/// Call with `Db.gate` held: sends what the JSON writer holds and clears it.
let private writeJson (ctx: HttpContext) (status: int) =
    json.Flush()
    write ctx status buffer.WrittenSpan
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

let private clear () =
    json.Reset(buffer)
    buffer.ResetWrittenCount()

/// The `{id}` route value: ASCII digits only, no sign or separators, and above zero. 0 means invalid.
let private routeId (ctx: HttpContext) =
    let mutable id = 0L
    match ctx.Request.RouteValues["id"] with
    | :? string as raw when Int64.TryParse(raw, NumberStyles.None, CultureInfo.InvariantCulture, &id) && id > 0L -> id
    | _ -> 0L

let health: EndpointHandler =
    fun ctx ->
        lock Db.gate (fun () ->
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
                writeJson ctx 200
            with e ->
                clear ()
                json.WriteStartObject()
                json.WriteString(pStatus, vDegraded)
                json.WriteString(pDb, vUnreachable)
                json.WriteString(pError, e.Message)
                json.WriteEndObject()
                writeJson ctx 503)
        flush ctx

let feed: EndpointHandler =
    fun ctx ->
        lock Db.gate (fun () ->
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
            writeJson ctx 200)
        flush ctx

let getPost: EndpointHandler =
    fun ctx ->
        let id = routeId ctx
        if id = 0L then
            send ctx 400 InvalidId
        else
            let found =
                lock Db.gate (fun () ->
                    let st = Db.post
                    try
                        Sqlite.bind_int64(st, 1, id) |> ignore
                        if Db.stepRow st then
                            json.WriteStartObject()
                            json.WritePropertyName(pPost)
                            writePost st
                            json.WriteEndObject()
                            writeJson ctx 200
                            true
                        else
                            false
                    finally
                        Sqlite.reset st |> ignore)
            if found then flush ctx else send ctx 404 PostNotFound

let private postExists (id: int64) =
    let st = Db.postExists
    try
        Sqlite.bind_int64(st, 1, id) |> ignore
        Db.stepRow st
    finally
        Sqlite.reset st |> ignore

let like: EndpointHandler =
    fun ctx ->
        let mutable userId = 0L
        let mutable username: string = null
        match Auth.verify ctx &userId &username with
        | null ->
            let id = routeId ctx
            if id = 0L then
                send ctx 400 InvalidId
            else
                let likeIt () =
                    lock Db.gate (fun () ->
                        let st = Db.insertLike
                        let inserted =
                            try
                                Sqlite.bind_int64(st, 1, userId) |> ignore
                                Sqlite.bind_int64(st, 2, id) |> ignore
                                // Stepping to DONE finishes the autocommit statement: the row is committed.
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
                            let status = if inserted then 201 else 200
                            writeJson ctx status
                            status
                        else
                            404)
                let finish status = if status = 404 then send ctx 404 PostNotFound else flush ctx
                let status = Db.runWrite likeIt
                if status.IsCompletedSuccessfully then
                    finish status.Result
                else
                    task {
                        let! status = status
                        return! finish status
                    }
        | authError -> send ctx 401 authError

/// The 400 body, or null when the post body is valid. Reads the whole document, so trailing junk is an error.
let private parseBody (data: ReadOnlySequence<byte>) (body: byref<string>) : byte[] =
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

let private insertPost (ctx: HttpContext) (userId: int64) (username: string) (body: string) =
    let utf8 = Encoding.UTF8.GetBytes body
    lock Db.gate (fun () ->
        let st = Db.insertPost
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
            // Stepping to DONE finishes the autocommit statement: the row is committed before we respond.
            if Sqlite.step st <> Sqlite.Done then
                clear ()
                raise (Db.failure ())
            Db.writes <- Db.writes + 1
            writeJson ctx 201
        finally
            Sqlite.reset st |> ignore)

let createPost: EndpointHandler =
    fun ctx ->
        let mutable userId = 0L
        let mutable username: string = null
        match Auth.verify ctx &userId &username with
        | null ->
            let userId = userId
            let username = username
            task {
                let reader = ctx.Request.BodyReader
                let mutable result = Unchecked.defaultof<_>
                let mutable reading = true
                while reading do
                    let! r = reader.ReadAsync()
                    result <- r
                    if r.IsCompleted then reading <- false
                    else reader.AdvanceTo(r.Buffer.Start, r.Buffer.End)
                let mutable body: string = null
                let error = parseBody result.Buffer &body
                reader.AdvanceTo(result.Buffer.End)
                if isNull error then
                    do! Db.runWrite (fun () -> insertPost ctx userId username body)
                    do! flush ctx
                else
                    do! send ctx 400 error
            }
        | authError -> send ctx 401 authError

let notFound: RequestDelegate = RequestDelegate(fun ctx -> send ctx 404 NotFound)

/// Any exception before the response starts becomes the spec's 500 body.
let guard (handler: EndpointHandler) : EndpointHandler =
    fun ctx ->
        let t =
            try handler ctx
            with _ when not ctx.Response.HasStarted -> send ctx 500 Internal
        if t.IsCompletedSuccessfully then t
        else
            task {
                try do! t
                with _ when not ctx.Response.HasStarted -> do! send ctx 500 Internal
            }
