// The $12 server challenge API: ASP.NET Core (Kestrel) compiled with Native AOT, and SQLite compiled from source
// and linked into the binary. The box has one core, so a request runs start to finish on Kestrel's socket thread
// against prepared statements on one connection, and JSON is written straight from SQLite's UTF-8 column bytes.
using System.Buffers;
using System.Buffers.Text;
using System.Globalization;
using System.IO.Pipelines;
using System.Net;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Microsoft.AspNetCore.Server.Kestrel.Core;

// stackalloc'd buffers are always written before they are read, so skip zeroing them.
[module: SkipLocalsInit]

// Socket completions run on .NET's socket thread instead of being handed to a thread-pool worker: on one core that
// handoff was a context switch per request (see README). Read when the first socket is created, so set it first.
Environment.SetEnvironmentVariable("DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS", "1");
Libc.RaiseOpenFileLimit();
Db.Open(Environment.GetEnvironmentVariable("SQLITE_PATH")!);
Auth.Key = Encoding.UTF8.GetBytes(Environment.GetEnvironmentVariable("JWT_SECRET")!);

// No configuration files, file watchers or logging providers: the server reads four environment variables.
var builder = WebApplication.CreateEmptyBuilder(new() { Args = args });
builder.WebHost.UseKestrelCore();
builder.WebHost.UseSockets(o =>
{
    // Continuations run on the socket thread instead of hopping to the thread pool.
    o.UnsafePreferInlineScheduling = true;
    // Read straight into a buffer instead of a zero-byte read first: one syscall fewer per request,
    // for one 4 KB pooled buffer held by each idle connection.
    o.WaitForDataBeforeAllocatingBuffer = false;
});
builder.WebHost.ConfigureKestrel(o =>
{
    o.AddServerHeader = false;
    o.Limits.KeepAliveTimeout = TimeSpan.FromSeconds(120); // SPEC: keep idle connections at least 65 s
    o.Limits.MinRequestBodyDataRate = null; // bodies are tiny: skip timing every body read against a minimum rate
    o.Listen(
        IPAddress.Parse(Environment.GetEnvironmentVariable("HOST") ?? "127.0.0.1"),
        int.Parse(Environment.GetEnvironmentVariable("PORT") ?? "3000"),
        l => l.Protocols = HttpProtocols.Http1);
});
var app = builder.Build();
((IApplicationBuilder)app).Run(Api.Handle);
app.Run();

// Every keep-alive connection is a file descriptor. The JIT runtime raises the soft open-files limit to the hard limit
// at startup, but Native AOT doesn't, so a shell's default soft limit of 1024 would cap the server at ~1,000 users.
static unsafe partial class Libc
{
    const int RlimitNofile = 7; // Linux, every architecture

    [LibraryImport("libc", EntryPoint = "getrlimit")] private static partial int GetRlimit(int resource, ulong* limit);
    [LibraryImport("libc", EntryPoint = "setrlimit")] private static partial int SetRlimit(int resource, ulong* limit);

    public static void RaiseOpenFileLimit()
    {
        var limit = stackalloc ulong[2]; // struct rlimit { rlim_cur, rlim_max }
        if (GetRlimit(RlimitNofile, limit) == 0 && limit[0] < limit[1])
        {
            limit[0] = limit[1];
            SetRlimit(RlimitNofile, limit);
        }
    }
}

// The SQLite C API, bound directly: raw handles, no marshalling. The getters are tiny and never block or call
// back into .NET, so they skip the GC transition.
static unsafe partial class Sqlite
{
    const string Lib = "sqlite3";
    public const int Ok = 0, Row = 100, Done = 101;
    public const int OpenReadWrite = 0x02;
    public const uint PreparePersistent = 0x01;
    public const nint Transient = -1; // SQLITE_TRANSIENT: SQLite copies the bound value

    [LibraryImport(Lib, EntryPoint = "sqlite3_initialize")] public static partial int Initialize();
    [LibraryImport(Lib, EntryPoint = "sqlite3_open_v2")] public static partial int OpenV2(byte* filename, out nint db, int flags, byte* vfs);
    [LibraryImport(Lib, EntryPoint = "sqlite3_exec")] public static partial int Exec(nint db, byte* sql, nint callback, nint arg, nint errmsg);
    [LibraryImport(Lib, EntryPoint = "sqlite3_prepare_v3")] public static partial int PrepareV3(nint db, byte* sql, int length, uint flags, out nint stmt, nint tail);
    [LibraryImport(Lib, EntryPoint = "sqlite3_step")] public static partial int Step(nint stmt);
    [LibraryImport(Lib, EntryPoint = "sqlite3_reset")] public static partial int Reset(nint stmt);
    [LibraryImport(Lib, EntryPoint = "sqlite3_bind_text")] public static partial int BindText(nint stmt, int index, byte* text, int length, nint destructor);
    [LibraryImport(Lib, EntryPoint = "sqlite3_errmsg")] public static partial byte* ErrMsg(nint db);
    [LibraryImport(Lib, EntryPoint = "sqlite3_bind_int64"), SuppressGCTransition] public static partial int BindInt64(nint stmt, int index, long value);
    [LibraryImport(Lib, EntryPoint = "sqlite3_column_int64"), SuppressGCTransition] public static partial long ColumnInt64(nint stmt, int column);
    [LibraryImport(Lib, EntryPoint = "sqlite3_column_text"), SuppressGCTransition] public static partial byte* ColumnText(nint stmt, int column);
    [LibraryImport(Lib, EntryPoint = "sqlite3_column_bytes"), SuppressGCTransition] public static partial int ColumnBytes(nint stmt, int column);
    [LibraryImport(Lib, EntryPoint = "sqlite3_changes"), SuppressGCTransition] public static partial int Changes(nint db);

    // A TEXT column as UTF-8 bytes, valid until the statement is stepped or reset. Text before bytes, per the docs.
    public static ReadOnlySpan<byte> Text(nint stmt, int column)
    {
        var text = ColumnText(stmt, column);
        return new ReadOnlySpan<byte>(text, ColumnBytes(stmt, column));
    }
}

static unsafe class Db
{
    // Every statement runs under this lock, start to finish, so the one connection is never shared mid-step
    // (which is also what makes SQLITE_THREADSAFE=0 safe).
    public static readonly Lock Gate = new();
    public static nint Feed, Post, InsertPost, InsertLike, PostExists, Ping;
    public static long OpenedAt; // Environment.TickCount64 at startup, for /health's uptime_s
    static nint _db;

    public static void Open(string path)
    {
        OpenedAt = Environment.TickCount64;
        Check(Sqlite.Initialize()); // required: SQLite is built with SQLITE_OMIT_AUTOINIT
        fixed (byte* p = Encoding.UTF8.GetBytes(path + "\0"))
            Check(Sqlite.OpenV2(p, out _db, Sqlite.OpenReadWrite, null));
        // One process, one connection: take the file lock once and keep it, instead of locking and unlocking around
        // every statement (two fcntl calls each). Set before WAL is touched, so the WAL index lives in heap memory
        // rather than a shared-memory file. Other processes can't open the database while the server runs.
        Exec("PRAGMA locking_mode = EXCLUSIVE\0"u8);
        Exec("PRAGMA journal_mode = WAL\0"u8);
        Exec("PRAGMA synchronous = NORMAL\0"u8); // rule 6; autocommit, so each write is committed before its response
        Exec("PRAGMA mmap_size = 1073741824\0"u8); // read pages from the OS page cache without copying them
        Exec("PRAGMA cache_size = -32768\0"u8);
        const string postSelect = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
              FROM posts p JOIN users u ON u.id = p.user_id
            """;
        Feed = Prepare(postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
        Post = Prepare(postSelect + " WHERE p.id = ?1");
        InsertPost = Prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
        // Inserts only if the post exists; no change means "already liked" or "no such post".
        InsertLike = Prepare("""
            INSERT INTO likes (user_id, post_id)
            SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
            ON CONFLICT (user_id, post_id) DO NOTHING
            """);
        PostExists = Prepare("SELECT 1 FROM posts WHERE id = ?1");
        Ping = Prepare("SELECT 1");
    }

    public static int Changes => Sqlite.Changes(_db);

    // Steps once: true on a row, false when the statement is done, and throws on anything else.
    public static bool StepRow(nint st) => Sqlite.Step(st) switch
    {
        Sqlite.Row => true,
        Sqlite.Done => false,
        _ => throw Failure(),
    };

    public static Exception Failure() =>
        new InvalidOperationException(Marshal.PtrToStringUTF8((nint)Sqlite.ErrMsg(_db)) ?? "sqlite error");

    static nint Prepare(string sql)
    {
        var utf8 = Encoding.UTF8.GetBytes(sql);
        nint stmt;
        fixed (byte* p = utf8)
            Check(Sqlite.PrepareV3(_db, p, utf8.Length, Sqlite.PreparePersistent, out stmt, 0));
        return stmt;
    }

    static void Exec(ReadOnlySpan<byte> sqlWithNul)
    {
        fixed (byte* p = sqlWithNul)
            Check(Sqlite.Exec(_db, p, 0, 0, 0));
    }

    static void Check(int rc)
    {
        if (rc != Sqlite.Ok)
            throw Failure();
    }
}

static class Bodies
{
    public static readonly byte[] NotFound = """{"error":"not found"}"""u8.ToArray();
    public static readonly byte[] InvalidId = """{"error":"invalid post id"}"""u8.ToArray();
    public static readonly byte[] PostNotFound = """{"error":"post not found"}"""u8.ToArray();
    public static readonly byte[] MissingBearer = """{"error":"missing bearer token"}"""u8.ToArray();
    public static readonly byte[] InvalidToken = """{"error":"invalid or expired token"}"""u8.ToArray();
    public static readonly byte[] InvalidPayload = """{"error":"invalid token payload"}"""u8.ToArray();
    public static readonly byte[] Malformed = """{"error":"malformed JSON body"}"""u8.ToArray();
    public static readonly byte[] BodyRequired = """{"error":"body is required"}"""u8.ToArray();
    public static readonly byte[] BodyTooLong = """{"error":"body must be at most 500 characters"}"""u8.ToArray();
    public static readonly byte[] Internal = """{"error":"internal server error"}"""u8.ToArray();
}

// A JSON buffer reused across requests on the same thread; taken out of the slot while in use.
sealed class Scratch
{
    [ThreadStatic] static Scratch? t_cached;
    public readonly ArrayBufferWriter<byte> Buffer = new(8192);
    public readonly Utf8JsonWriter Json;

    Scratch() => Json = new Utf8JsonWriter(Buffer, new JsonWriterOptions
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping, // escaping <>&/ and non-ASCII is optional (SPEC)
        SkipValidation = true,
    });

    public static Scratch Rent()
    {
        var s = t_cached ?? new Scratch();
        t_cached = null;
        return s;
    }

    public void Return()
    {
        Json.Reset(Buffer);
        Buffer.ResetWrittenCount();
        t_cached = this;
    }
}

static class Api
{
    static readonly JsonEncodedText Id = JsonEncodedText.Encode("id");
    static readonly JsonEncodedText Body = JsonEncodedText.Encode("body");
    static readonly JsonEncodedText CreatedAt = JsonEncodedText.Encode("created_at");
    static readonly JsonEncodedText Author = JsonEncodedText.Encode("author");
    static readonly JsonEncodedText LikeCount = JsonEncodedText.Encode("like_count");
    static readonly JsonEncodedText Posts = JsonEncodedText.Encode("posts");
    static readonly JsonEncodedText Post = JsonEncodedText.Encode("post");

    // JavaScript's String.prototype.trim() set, so bodies are trimmed exactly like the reference servers.
    static readonly char[] JsWhitespace =
        "\t\n\v\f\r \u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff".ToCharArray();

    // Reads complete synchronously (SQLite is in-process); only a request whose flush or body read goes
    // asynchronous pays for an async state machine.
    public static Task Handle(HttpContext ctx)
    {
        Task task;
        try
        {
            task = Route(ctx);
        }
        catch when (!ctx.Response.HasStarted)
        {
            return Send(ctx, 500, Bodies.Internal);
        }
        return task.IsCompletedSuccessfully ? task : AwaitRoute(ctx, task);
    }

    static async Task AwaitRoute(HttpContext ctx, Task task)
    {
        try
        {
            await task;
        }
        catch when (!ctx.Response.HasStarted)
        {
            await Send(ctx, 500, Bodies.Internal);
        }
    }

    static Task Route(HttpContext ctx)
    {
        var path = ctx.Request.Path.Value ?? "";
        var isGet = HttpMethods.IsGet(ctx.Request.Method);
        if (path.StartsWith("/posts/", StringComparison.Ordinal))
        {
            var rest = path.AsSpan(7);
            if (!isGet && rest.EndsWith("/like") && !rest[..^5].Contains('/'))
                return Like(ctx, TryParseId(rest[..^5], out var likeId) ? likeId : 0);
            if (isGet && !rest.Contains('/'))
                return GetPost(ctx, TryParseId(rest, out var id) ? id : 0);
        }
        else if (path == "/feed" && isGet)
            return Feed(ctx);
        else if (path == "/posts" && !isGet)
            return CreatePost(ctx);
        else if (path == "/health" && isGet)
            return Health(ctx);
        return Send(ctx, 404, Bodies.NotFound);
    }

    // ASCII digits only: no sign, whitespace or separators.
    static bool TryParseId(ReadOnlySpan<char> raw, out long id) =>
        long.TryParse(raw, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0;

    static Task Feed(HttpContext ctx)
    {
        var s = Scratch.Rent();
        var w = s.Json;
        lock (Db.Gate)
        {
            var st = Db.Feed;
            try
            {
                w.WriteStartObject();
                w.WriteStartArray(Posts);
                while (Db.StepRow(st))
                    WritePost(w, st);
                w.WriteEndArray();
                w.WriteEndObject();
            }
            finally
            {
                Sqlite.Reset(st);
            }
        }
        return SendJson(ctx, 200, s);
    }

    static Task GetPost(HttpContext ctx, long id)
    {
        if (id == 0)
            return Send(ctx, 400, Bodies.InvalidId);
        var s = Scratch.Rent();
        bool found;
        lock (Db.Gate)
        {
            var st = Db.Post;
            try
            {
                Sqlite.BindInt64(st, 1, id);
                found = Db.StepRow(st);
                if (found)
                {
                    s.Json.WriteStartObject();
                    s.Json.WritePropertyName(Post);
                    WritePost(s.Json, st);
                    s.Json.WriteEndObject();
                }
            }
            finally
            {
                Sqlite.Reset(st);
            }
        }
        if (found)
            return SendJson(ctx, 200, s);
        s.Return();
        return Send(ctx, 404, Bodies.PostNotFound);
    }

    static async Task CreatePost(HttpContext ctx)
    {
        var authError = Auth.Verify(ctx, out var userId, out var username);
        if (authError is not null)
        {
            await Send(ctx, 401, authError);
            return;
        }
        var bodyReader = ctx.Request.BodyReader;
        ReadResult read;
        while (true)
        {
            read = await bodyReader.ReadAsync();
            if (read.IsCompleted)
                break;
            bodyReader.AdvanceTo(read.Buffer.Start, read.Buffer.End);
        }
        var error = ParseBody(read.Buffer, out var body);
        bodyReader.AdvanceTo(read.Buffer.End);
        if (error is not null)
        {
            await Send(ctx, 400, error);
            return;
        }
        var (id, createdAt) = InsertPost(userId, body);
        var s = Scratch.Rent();
        var w = s.Json;
        w.WriteStartObject();
        w.WriteStartObject(Post);
        w.WriteNumber(Id, id);
        w.WriteString(Body, body);
        w.WriteString(CreatedAt, createdAt);
        w.WriteString(Author, username);
        w.WriteNumber(LikeCount, 0);
        w.WriteEndObject();
        w.WriteEndObject();
        await SendJson(ctx, 201, s);
    }

    static unsafe (long Id, string CreatedAt) InsertPost(long userId, string body)
    {
        Span<byte> utf8 = stackalloc byte[Encoding.UTF8.GetMaxByteCount(500)];
        var length = Encoding.UTF8.GetBytes(body, utf8);
        lock (Db.Gate)
        {
            var st = Db.InsertPost;
            try
            {
                Sqlite.BindInt64(st, 1, userId);
                fixed (byte* p = utf8)
                    Sqlite.BindText(st, 2, p, length, Sqlite.Transient);
                if (Sqlite.Step(st) != Sqlite.Row)
                    throw Db.Failure();
                var result = (Sqlite.ColumnInt64(st, 0), Encoding.UTF8.GetString(Sqlite.Text(st, 1)));
                // Stepping to DONE finishes the statement, which commits it before we respond.
                if (Sqlite.Step(st) != Sqlite.Done)
                    throw Db.Failure();
                return result;
            }
            finally
            {
                Sqlite.Reset(st);
            }
        }
    }

    // The 400 body, or null when the post body is valid.
    static byte[]? ParseBody(ReadOnlySequence<byte> json, out string body)
    {
        body = "";
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json);
        }
        catch (JsonException)
        {
            return Bodies.Malformed;
        }
        using (doc)
        {
            if (doc.RootElement.ValueKind != JsonValueKind.Object
                || !doc.RootElement.TryGetProperty("body"u8, out var value)
                || value.ValueKind != JsonValueKind.String)
                return Bodies.BodyRequired;
            body = value.GetString()!.Trim(JsWhitespace);
        }
        if (body.Length == 0)
            return Bodies.BodyRequired;
        return body.Length > 500 ? Bodies.BodyTooLong : null;
    }

    static Task Like(HttpContext ctx, long id)
    {
        if (Auth.Verify(ctx, out var userId, out _) is { } authError)
            return Send(ctx, 401, authError);
        if (id == 0)
            return Send(ctx, 400, Bodies.InvalidId);
        bool inserted, exists;
        lock (Db.Gate)
        {
            var st = Db.InsertLike;
            try
            {
                Sqlite.BindInt64(st, 1, userId);
                Sqlite.BindInt64(st, 2, id);
                if (Sqlite.Step(st) != Sqlite.Done)
                    throw Db.Failure();
                inserted = Db.Changes == 1;
            }
            finally
            {
                Sqlite.Reset(st);
            }
            exists = inserted || PostExists(id);
        }
        if (!exists)
            return Send(ctx, 404, Bodies.PostNotFound);
        var s = Scratch.Rent();
        s.Json.WriteStartObject();
        s.Json.WriteBoolean("liked"u8, true);
        s.Json.WriteBoolean("already_liked"u8, !inserted);
        s.Json.WriteNumber("post_id"u8, id);
        s.Json.WriteEndObject();
        return SendJson(ctx, inserted ? 201 : 200, s);
    }

    static bool PostExists(long id)
    {
        var st = Db.PostExists;
        try
        {
            Sqlite.BindInt64(st, 1, id);
            return Db.StepRow(st);
        }
        finally
        {
            Sqlite.Reset(st);
        }
    }

    static Task Health(HttpContext ctx)
    {
        var s = Scratch.Rent();
        var w = s.Json;
        w.WriteStartObject();
        try
        {
            lock (Db.Gate)
            {
                try
                {
                    if (!Db.StepRow(Db.Ping))
                        throw Db.Failure();
                }
                finally
                {
                    Sqlite.Reset(Db.Ping);
                }
            }
        }
        catch (Exception e)
        {
            w.WriteString("status"u8, "degraded");
            w.WriteString("db"u8, "unreachable");
            w.WriteString("error"u8, e.Message);
            w.WriteEndObject();
            return SendJson(ctx, 503, s);
        }
        w.WriteString("status"u8, "ok");
        w.WriteString("db"u8, "ok");
        w.WriteNumber("uptime_s"u8, (Environment.TickCount64 - Db.OpenedAt) / 1000);
        w.WriteEndObject();
        return SendJson(ctx, 200, s);
    }

    // Columns are TEXT stored as UTF-8; created_at is already in the wire format, so it is sent as stored.
    static void WritePost(Utf8JsonWriter w, nint st)
    {
        w.WriteStartObject();
        w.WriteNumber(Id, Sqlite.ColumnInt64(st, 0));
        w.WriteString(Body, Sqlite.Text(st, 1));
        w.WriteString(CreatedAt, Sqlite.Text(st, 2));
        w.WriteString(Author, Sqlite.Text(st, 3));
        w.WriteNumber(LikeCount, Sqlite.ColumnInt64(st, 4));
        w.WriteEndObject();
    }

    // The body is copied into Kestrel's pipe before this returns, so the caller can reuse its buffer at once;
    // only a flush that can't complete immediately is turned into a Task.
    static Task Send(HttpContext ctx, int status, ReadOnlySpan<byte> body)
    {
        var response = ctx.Response;
        response.StatusCode = status;
        response.ContentType = "application/json";
        response.ContentLength = body.Length;
        response.BodyWriter.Write(body);
        var flush = response.BodyWriter.FlushAsync();
        return flush.IsCompletedSuccessfully ? Task.CompletedTask : flush.AsTask();
    }

    static Task SendJson(HttpContext ctx, int status, Scratch s)
    {
        s.Json.Flush();
        var sent = Send(ctx, status, s.Buffer.WrittenSpan);
        s.Return();
        return sent;
    }
}

static class Auth
{
    public static byte[] Key = null!;

    // HS256 only; signature and exp checked on every request (no caching, rule 5). The 401 body, or null when valid.
    public static byte[]? Verify(HttpContext ctx, out long userId, out string username)
    {
        userId = 0;
        username = "";
        string? header = ctx.Request.Headers.Authorization;
        if (header is null || !header.StartsWith("Bearer ", StringComparison.Ordinal))
            return Bodies.MissingBearer;
        var token = header.AsSpan(7);
        Span<Range> parts = stackalloc Range[4];
        if (token.Length > 2048 || token.Split(parts, '.') != 3)
            return Bodies.InvalidToken;

        Span<byte> json = stackalloc byte[1536];
        if (!Base64Url.TryDecodeFromChars(token[parts[0]], json, out var n) || !IsHs256(json[..n]))
            return Bodies.InvalidToken;

        Span<byte> signingInput = stackalloc byte[2048];
        var inputLength = Encoding.ASCII.GetBytes(token[..parts[1].End], signingInput);
        Span<byte> expected = stackalloc byte[32];
        HMACSHA256.HashData(Key, signingInput[..inputLength], expected);
        Span<byte> signature = stackalloc byte[64];
        if (!Base64Url.TryDecodeFromChars(token[parts[2]], signature, out var sigLength)
            || sigLength != 32
            || !CryptographicOperations.FixedTimeEquals(signature[..32], expected))
            return Bodies.InvalidToken;

        if (!Base64Url.TryDecodeFromChars(token[parts[1]], json, out n))
            return Bodies.InvalidToken;
        return ReadPayload(json[..n], out userId, out username);
    }

    static bool IsHs256(ReadOnlySpan<byte> json)
    {
        try
        {
            var r = new Utf8JsonReader(json);
            if (!r.Read() || r.TokenType != JsonTokenType.StartObject)
                return false;
            var hs256 = false;
            while (r.Read() && r.TokenType == JsonTokenType.PropertyName)
            {
                var isAlg = r.ValueTextEquals("alg"u8);
                r.Read();
                if (isAlg)
                    hs256 = r.TokenType == JsonTokenType.String && r.ValueTextEquals("HS256"u8);
                r.Skip(); // past a nested value, so its own properties aren't read as top-level ones
            }
            return hs256;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    static byte[]? ReadPayload(ReadOnlySpan<byte> json, out long userId, out string username)
    {
        userId = 0;
        username = "";
        var subOk = false;
        string? name = null;
        double? exp = null;
        try
        {
            var r = new Utf8JsonReader(json);
            if (!r.Read() || r.TokenType != JsonTokenType.StartObject)
                return Bodies.InvalidToken;
            while (r.Read() && r.TokenType == JsonTokenType.PropertyName)
            {
                var isSub = r.ValueTextEquals("sub"u8);
                var isName = !isSub && r.ValueTextEquals("username"u8);
                var isExp = !isSub && !isName && r.ValueTextEquals("exp"u8);
                r.Read();
                if (isSub)
                    subOk = r.TokenType == JsonTokenType.String && TryParseId(ref r, out userId);
                else if (isName)
                    name = r.TokenType == JsonTokenType.String ? r.GetString() : null;
                else if (isExp)
                {
                    if (r.TokenType != JsonTokenType.Number)
                        return Bodies.InvalidToken;
                    exp = r.GetDouble();
                }
                r.Skip();
            }
        }
        catch (JsonException)
        {
            return Bodies.InvalidToken;
        }
        if (exp is not { } e || DateTimeOffset.UtcNow.ToUnixTimeSeconds() >= e)
            return Bodies.InvalidToken;
        if (!subOk || name is null)
            return Bodies.InvalidPayload;
        username = name;
        return null;
    }

    // A string value holding a user id: ASCII digits only, parsed from its UTF-8 bytes.
    static bool TryParseId(ref Utf8JsonReader r, out long id)
    {
        id = 0;
        Span<byte> utf8 = stackalloc byte[32];
        return r.ValueSpan.Length <= utf8.Length // the escaped form is never shorter than the value
            && long.TryParse(utf8[..r.CopyString(utf8)], NumberStyles.None, CultureInfo.InvariantCulture, out id)
            && id > 0;
    }
}
