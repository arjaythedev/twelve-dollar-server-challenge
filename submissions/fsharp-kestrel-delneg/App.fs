/// The whole API as Kestrel's IHttpApplication: Kestrel parses HTTP and gives us its feature collection.
/// There is no middleware, no HttpContext and no routing table. The path is matched by hand.
/// Reads run start to finish on the socket thread: SQLite is in-process and every query is an index lookup.
module App

#nowarn "9"

open System
open System.Buffers
open System.Globalization
open System.IO.Pipelines
open System.Text
open System.Text.Encodings.Web
open System.Text.Json
open System.Threading.Tasks
open Microsoft.AspNetCore.Hosting.Server
open Microsoft.AspNetCore.Http.Features
open Microsoft.Extensions.Primitives

let NotFound = """{"error":"not found"}"""B
let InvalidId = """{"error":"invalid post id"}"""B
let PostNotFound = """{"error":"post not found"}"""B
let Malformed = """{"error":"malformed JSON body"}"""B
let BodyRequired = """{"error":"body is required"}"""B
let BodyTooLong = """{"error":"body must be at most 500 characters"}"""B
let Internal = """{"error":"internal server error"}"""B

let private jsonType = StringValues "application/json"
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

// Kestrel's request object implements all these features itself, so a cast finds them. Get<T> is the fallback.
let inline private feature<'T when 'T: null> (f: IFeatureCollection) : 'T =
    match box f with
    | :? 'T as x -> x
    | _ -> f.Get<'T>()

/// Sets the status and headers and copies the body into Kestrel's pipe. Kestrel flushes it when the request ends.
let private write (f: IFeatureCollection) (status: int) (body: ReadOnlySpan<byte>) =
    let r = feature<IHttpResponseFeature> f
    r.StatusCode <- status
    let h = r.Headers
    h.ContentType <- jsonType
    h.ContentLength <- Nullable(int64 body.Length)
    (feature<IHttpResponseBodyFeature> f).Writer.Write(body)

let private send (f: IFeatureCollection) (status: int) (body: byte[]) : Task =
    write f status (ReadOnlySpan body)
    Task.CompletedTask

/// Call with `Db.gate` held: sends what the JSON writer holds and clears it.
let private writeJson (f: IFeatureCollection) (status: int) =
    json.Flush()
    write f status buffer.WrittenSpan
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

/// A post id: ASCII digits only, no sign or separators, above zero. 0 means invalid.
let private parseId (s: ReadOnlySpan<char>) =
    let mutable id = 0L
    if s.Length > 0 && s.Length <= 18 && Int64.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture, &id) && id > 0L then id
    else 0L

let private health (f: IFeatureCollection) : Task =
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
            writeJson f 200
        with e ->
            clear ()
            json.WriteStartObject()
            json.WriteString(pStatus, vDegraded)
            json.WriteString(pDb, vUnreachable)
            json.WriteString(pError, e.Message)
            json.WriteEndObject()
            writeJson f 503)
    Task.CompletedTask

let private feed (f: IFeatureCollection) : Task =
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
        writeJson f 200)
    Task.CompletedTask

let private getPost (f: IFeatureCollection) (id: int64) : Task =
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
                    writeJson f 200
                    true
                else
                    false
            finally
                Sqlite.reset st |> ignore)
    if found then Task.CompletedTask else send f 404 PostNotFound

let private postExists (id: int64) =
    let st = Db.postExists
    try
        Sqlite.bind_int64(st, 1, id) |> ignore
        Db.stepRow st
    finally
        Sqlite.reset st |> ignore

let private like (f: IFeatureCollection) (userId: int64) (id: int64) : Task =
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
                writeJson f (if inserted then 201 else 200)
                true
            else
                false)
    let finish found = if found then Task.CompletedTask else send f 404 PostNotFound
    let r = Db.runWrite likeIt
    if r.IsCompletedSuccessfully then
        finish r.Result
    else
        task {
            let! found = r
            return! finish found
        }
        :> Task

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

let private insertPost (f: IFeatureCollection) (userId: int64) (username: string) (body: string) =
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
            writeJson f 201
        finally
            Sqlite.reset st |> ignore)

let private createWith (f: IFeatureCollection) (userId: int64) (username: string) (data: ReadOnlySequence<byte>) : Task =
    let mutable body: string = null
    match parseBody data &body with
    | null ->
        let r = Db.runWrite (fun () -> insertPost f userId username body)
        if r.IsCompletedSuccessfully then Task.CompletedTask else r.AsTask() :> Task
    | error -> send f 400 error

let private createPost (f: IFeatureCollection) (userId: int64) (username: string) : Task =
    let reader = (feature<IRequestBodyPipeFeature> f).Reader
    let mutable result = Unchecked.defaultof<ReadResult>
    // The body of a small POST is almost always in Kestrel's buffer already: then no async state machine runs.
    let got = reader.TryRead(&result)
    if got && result.IsCompleted then
        let t = createWith f userId username result.Buffer
        reader.AdvanceTo(result.Buffer.End)
        t
    else
        if got then
            reader.AdvanceTo(result.Buffer.Start, result.Buffer.End)
        task {
            let mutable reading = true
            while reading do
                let! r = reader.ReadAsync()
                result <- r
                if r.IsCompleted then reading <- false
                else reader.AdvanceTo(r.Buffer.Start, r.Buffer.End)
            let t = createWith f userId username result.Buffer
            reader.AdvanceTo(result.Buffer.End)
            return! t
        }
        :> Task

let private authorization (req: IHttpRequestFeature) : string =
    let v = req.Headers.Authorization
    if v.Count = 0 then null else v.[0]

let private handle (f: IFeatureCollection) : Task =
    let req = feature<IHttpRequestFeature> f
    let path = req.Path
    let m = req.Method
    if String.Equals(m, "GET", StringComparison.Ordinal) then
        if path = "/feed" then feed f
        elif path.StartsWith("/posts/", StringComparison.Ordinal) && path.IndexOf('/', 7) < 0 then
            let id = parseId (path.AsSpan 7)
            if id = 0L then send f 400 InvalidId else getPost f id
        elif path = "/health" then health f
        else send f 404 NotFound
    elif String.Equals(m, "POST", StringComparison.Ordinal) then
        if path = "/posts" then
            let mutable userId = 0L
            let mutable username: string = null
            match Auth.verify (authorization req) &userId &username with
            | null -> createPost f userId username
            | authError -> send f 401 authError
        elif path.StartsWith("/posts/", StringComparison.Ordinal) && path.EndsWith("/like", StringComparison.Ordinal)
             && path.Length > 12 && path.IndexOf('/', 7) = path.Length - 5 then
            // Auth first, then the id (SPEC).
            let mutable userId = 0L
            let mutable username: string = null
            match Auth.verify (authorization req) &userId &username with
            | null ->
                let id = parseId (path.AsSpan(7, path.Length - 12))
                if id = 0L then send f 400 InvalidId else like f userId id
            | authError -> send f 401 authError
        else send f 404 NotFound
    else
        send f 404 NotFound

let private fail (f: IFeatureCollection) =
    if not (feature<IHttpResponseFeature> f).HasStarted then
        send f 500 Internal
    else
        Task.CompletedTask

/// Any exception before the response starts becomes the spec's 500 body.
let private run (f: IFeatureCollection) : Task =
    let t =
        try handle f
        with _ -> fail f
    if t.IsCompletedSuccessfully then t
    else
        task {
            try do! t
            with _ -> do! fail f
        }
        :> Task

type Application() =
    interface IHttpApplication<IFeatureCollection> with
        member _.CreateContext(features) = features
        member _.ProcessRequestAsync(f) = run f
        member _.DisposeContext(_, _) = ()
