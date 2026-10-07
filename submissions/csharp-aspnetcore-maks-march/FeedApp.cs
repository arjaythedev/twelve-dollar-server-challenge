using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;

namespace FeedApi;

/// <summary>
/// A pooled pair of request/response features; 
/// Kestrel gives them to us per request.
/// </summary>
internal sealed class RequestContext
{
    public IHttpRequestFeature Request = null!;
    public IHttpResponseFeature Response = null!;
    public IHttpResponseBodyFeature ResponseBody = null!;
}

/// <summary>
/// The whole application as Kestrel's IHttpApplication: no middleware pipeline, no
/// HttpContext, no routing table. Handle() matches the method and path by hand, runs the
/// (synchronous, sub-millisecond) SQLite query and writes pre-built or pooled JSON bytes.
/// </summary>
internal sealed class FeedApp : IHttpApplication<RequestContext>
{
    private const int MaxBodyBytes = 1 << 20;

    // JavaScript's String.prototype.trim() whitespace set, so bodies are trimmed exactly
    // like the reference implementations (includes U+00A0..U+3000 and U+FEFF).
    private const string JsWhitespace =
        "\t\n\v\f\r \u00A0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202F\u205F\u3000\uFEFF";

    private static readonly ReadOnlyMemory<byte> ErrNotFound = "{\"error\":\"not found\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrInvalidId = "{\"error\":\"invalid post id\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrPostNotFound = "{\"error\":\"post not found\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrMissingToken = "{\"error\":\"missing bearer token\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrInvalidToken = "{\"error\":\"invalid or expired token\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrBadPayload = "{\"error\":\"invalid token payload\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrMalformedJson = "{\"error\":\"malformed JSON body\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrBodyRequired = "{\"error\":\"body is required\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrBodyTooLong = "{\"error\":\"body must be at most 500 characters\"}"u8.ToArray();
    private static readonly ReadOnlyMemory<byte> ErrInternal = "{\"error\":\"internal server error\"}"u8.ToArray();

    private readonly Db _db;
    private readonly byte[] _secret;
    private readonly long _started = Stopwatch.GetTimestamp();
    private readonly ConcurrentQueue<RequestContext> _pool = new();

    public FeedApp(Db db, byte[] secret)
    {
        _db = db;
        _secret = secret;
    }

    public RequestContext CreateContext(IFeatureCollection contextFeatures)
    {
        var context = _pool.TryDequeue(out var pooled) ? pooled : new RequestContext();
        context.Request = contextFeatures.Get<IHttpRequestFeature>()!;
        context.Response = contextFeatures.Get<IHttpResponseFeature>()!;
        context.ResponseBody = contextFeatures.Get<IHttpResponseBodyFeature>()!;
        return context;
    }

    public void DisposeContext(RequestContext context, Exception? exception)
    {
        context.Request = null!;
        context.Response = null!;
        context.ResponseBody = null!;
        _pool.Enqueue(context);
    }

    public Task ProcessRequestAsync(RequestContext context) => HandleSafeAsync(context);

    private async Task HandleSafeAsync(RequestContext context)
    {
        try
        {
            await Handle(context).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            try { await WriteAsync(context, 500, ErrInternal).ConfigureAwait(false); }
            catch { /* response already started or the connection is gone */ }
        }
    }

    private async Task Handle(RequestContext context)
    {
        var request = context.Request;
        var method = request.Method;
        var path = request.Path;

        if (method == "GET")
        {
            if (path == "/health") { await HealthAsync(context).ConfigureAwait(false); return; }
            if (path == "/feed") { await FeedAsync(context).ConfigureAwait(false); return; }
            else if (path.StartsWith("/posts/", StringComparison.Ordinal)
                && path.Length > 7
                && path.IndexOf('/', 7) < 0)
            {
                // GET /posts/:id — the tail is a single non-empty segment.
                if (!TryParseId(path.AsSpan(7), out long id, out bool tooLarge))
                {
                    await WriteAsync(context, 400, ErrInvalidId).ConfigureAwait(false);
                    return;
                }
                if (tooLarge)
                {
                    await WriteAsync(context, 404, ErrPostNotFound).ConfigureAwait(false);
                    return;
                }
                await GetPostAsync(context, id).ConfigureAwait(false);
                return;
            }
        }
        else if (method == "POST")
        {
            if (path == "/posts")
            {
                await CreatePostAsync(context).ConfigureAwait(false);
                return;
            }
            if (path.StartsWith("/posts/", StringComparison.Ordinal)
                && path.EndsWith("/like", StringComparison.Ordinal)
                && IsPlainSegment(path, 7, path.Length - 5))
            {
                // POST /posts/:id/like — auth first, then the id (per the spec).
                var auth = Jwt.Verify(request.Headers, _secret);
                if (auth.Status != AuthStatus.Ok)
                {
                    await WriteAsync(context, 401, AuthError(auth.Status)).ConfigureAwait(false);
                    return;
                }
                if (!TryParseId(path.AsSpan(7, path.Length - 12), out long id, out bool tooLarge))
                {
                    await WriteAsync(context, 400, ErrInvalidId).ConfigureAwait(false);
                    return;
                }
                if (tooLarge)
                {
                    await WriteAsync(context, 404, ErrPostNotFound).ConfigureAwait(false);
                    return;
                }
                await LikeAsync(context, auth, id).ConfigureAwait(false);
                return;
            }
        }

        await WriteAsync(context, 404, ErrNotFound).ConfigureAwait(false);
    }

    # region handlers

    private async Task HealthAsync(RequestContext context)
    {
        string? dbError = _db.Health();
        if (dbError != null)
        {
            var json = Json.Rent();
            try
            {
                var w = json.Writer;
                w.WriteStartObject();
                w.WriteString("status", "degraded");
                w.WriteString("db", "unreachable");
                w.WriteString("error", dbError);
                w.WriteEndObject();
                w.Flush();
                await WriteAsync(context, 503, json.Buffer.WrittenMemory).ConfigureAwait(false);
            }
            finally { Json.Return(json); }
            return;
        }

        int uptime = (int)((Stopwatch.GetTimestamp() - _started) / Stopwatch.Frequency);
        var ok = Json.Rent();
        try
        {
            var w = ok.Writer;
            w.WriteStartObject();
            w.WriteString("status", "ok");
            w.WriteString("db", "ok");
            w.WriteNumber("uptime_s", uptime);
            w.WriteEndObject();
            w.Flush();
            await WriteAsync(context, 200, ok.Buffer.WrittenMemory).ConfigureAwait(false);
        }
        finally { Json.Return(ok); }
    }

    private async Task FeedAsync(RequestContext context)
    {
        var json = Json.Rent();
        try
        {
            _db.WriteFeed(json);
            await WriteAsync(context, 200, json.Buffer.WrittenMemory).ConfigureAwait(false);
        }
        finally { Json.Return(json); }
    }

    private async Task GetPostAsync(RequestContext context, long id)
    {
        var json = Json.Rent();
        try
        {
            if (!_db.WritePost(json, id))
            {
                await WriteAsync(context, 404, ErrPostNotFound).ConfigureAwait(false);
                return;
            }
            await WriteAsync(context, 200, json.Buffer.WrittenMemory).ConfigureAwait(false);
        }
        finally { Json.Return(json); }
    }

    private async Task CreatePostAsync(RequestContext context)
    {
        var auth = Jwt.Verify(context.Request.Headers, _secret);
        if (auth.Status != AuthStatus.Ok)
        {
            await WriteAsync(context, 401, AuthError(auth.Status)).ConfigureAwait(false);
            return;
        }

        byte[]? rented = null;
        try
        {
            (rented, int length) = await ReadBodyAsync(context.Request.Body).ConfigureAwait(false);
            string body;
            try
            {
                // JsonDocument.Parse retains the input buffer; both live in this scope together.
                using var doc = JsonDocument.Parse(rented.AsMemory(0, length));
                if (doc.RootElement.ValueKind != JsonValueKind.Object
                    || !doc.RootElement.TryGetProperty("body", out var bodyElement)
                    || bodyElement.ValueKind != JsonValueKind.String)
                {
                    await WriteAsync(context, 400, ErrBodyRequired).ConfigureAwait(false);
                    return;
                }

                var raw = bodyElement.GetString()!;
                var trimmed = raw.AsSpan().Trim(JsWhitespace);
                if (trimmed.IsEmpty)
                {
                    await WriteAsync(context, 400, ErrBodyRequired).ConfigureAwait(false);
                    return;
                }
                if (trimmed.Length > 500)
                {
                    await WriteAsync(context, 400, ErrBodyTooLong).ConfigureAwait(false);
                    return;
                }
                body = trimmed.Length == raw.Length ? raw : new string(trimmed);
            }
            catch (JsonException)
            {
                await WriteAsync(context, 400, ErrMalformedJson).ConfigureAwait(false);
                return;
            }

            var (id, createdAt) = _db.InsertPost(auth.UserId, body);
            var json = Json.Rent();
            try
            {
                var w = json.Writer;
                w.WriteStartObject();
                w.WritePropertyName("post");
                w.WriteStartObject();
                w.WriteNumber("id", id);
                w.WriteString("body", body);
                w.WriteString("created_at", createdAt);
                w.WriteString("author", auth.Username);
                w.WriteNumber("like_count", 0);
                w.WriteEndObject();
                w.WriteEndObject();
                w.Flush();
                await WriteAsync(context, 201, json.Buffer.WrittenMemory).ConfigureAwait(false);
            }
            finally { Json.Return(json); }
        }
        finally
        {
            if (rented is not null) ArrayPool<byte>.Shared.Return(rented);
        }
    }

    private async Task LikeAsync(RequestContext context, AuthResult auth, long postId)
    {
        var outcome = _db.Like(auth.UserId, postId);
        if (outcome == LikeOutcome.PostMissing)
        {
            await WriteAsync(context, 404, ErrPostNotFound).ConfigureAwait(false);
            return;
        }
        var json = Json.Rent();
        try
        {
            var w = json.Writer;
            w.WriteStartObject();
            w.WriteBoolean("liked", true);
            w.WriteBoolean("already_liked", outcome == LikeOutcome.AlreadyLiked);
            w.WriteNumber("post_id", postId);
            w.WriteEndObject();
            w.Flush();
            await WriteAsync(context, outcome == LikeOutcome.Liked ? 201 : 200, json.Buffer.WrittenMemory)
                .ConfigureAwait(false);
        }
        finally { Json.Return(json); }
    }

    #endregion

    #region helpers
    private static async Task WriteAsync(RequestContext context, int status, ReadOnlyMemory<byte> body)
    {
        var response = context.Response;
        response.StatusCode = status;
        response.Headers["Content-Type"] = "application/json";
        response.Headers.ContentLength = body.Length;
        await context.ResponseBody.Stream.WriteAsync(body).ConfigureAwait(false);
    }

    private static ReadOnlyMemory<byte> AuthError(AuthStatus status) => status switch
    {
        AuthStatus.MissingToken => ErrMissingToken,
        AuthStatus.BadPayload => ErrBadPayload,
        _ => ErrInvalidToken,
    };

    private static async Task<(byte[] Buffer, int Length)> ReadBodyAsync(Stream body)
    {
        var buffer = ArrayPool<byte>.Shared.Rent(2048);
        int length = 0;
        while (true)
        {
            if (length == buffer.Length)
            {
                var bigger = ArrayPool<byte>.Shared.Rent(buffer.Length * 2);
                Buffer.BlockCopy(buffer, 0, bigger, 0, length);
                ArrayPool<byte>.Shared.Return(buffer);
                buffer = bigger;
            }
            int read = await body.ReadAsync(buffer.AsMemory(length)).ConfigureAwait(false);
            if (read == 0) return (buffer, length);
            length += read;
            if (length >= MaxBodyBytes) return (buffer, length); // out of spec: parse will fail
        }
    }

    /// <summary>[start, end) is a non-empty run of non-slash characters (a single path segment).</summary>
    private static bool IsPlainSegment(string path, int start, int end)
    {
        if (end <= start) return false;
        for (int i = start; i < end; i++)
        {
            if (path[i] == '/') return false;
        }
        return true;
    }

    /// <summary>
    /// A positive integer in plain decimal digits (no sign, no leading +, no decimals).
    /// tooLarge = all digits but above SQLite's INTEGER range: such a row cannot exist,
    /// so callers answer 404 instead of 400 (matching the reference implementation).
    /// </summary>
    private static bool TryParseId(ReadOnlySpan<char> text, out long id, out bool tooLarge)
    {
        id = 0;
        tooLarge = false;
        if (text.IsEmpty) return false;

        ulong value = 0;
        foreach (var c in text)
        {
            if (c < '0' || c > '9') return false;
            if (!tooLarge)
            {
                value = value * 10 + (ulong)(c - '0');
                if (value > (ulong)long.MaxValue) tooLarge = true;
            }
        }
        if (tooLarge) return true;
        if (value == 0) return false;
        id = (long)value;
        return true;
    }
    
    #endregion
}
