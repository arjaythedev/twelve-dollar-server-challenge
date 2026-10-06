using System.Buffers;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Text.Json;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Data.Sqlite;

var uptime = Stopwatch.StartNew();
var host = Environment.GetEnvironmentVariable("HOST") ?? "127.0.0.1";
var port = int.Parse(Environment.GetEnvironmentVariable("PORT") ?? "3000", CultureInfo.InvariantCulture);
var secret = Environment.GetEnvironmentVariable("JWT_SECRET")
    ?? throw new InvalidOperationException("JWT_SECRET is required");
var database = new FeedDatabase(Environment.GetEnvironmentVariable("SQLITE_PATH")
    ?? throw new InvalidOperationException("SQLITE_PATH is required"));
var auth = new TokenAuth(secret);

// With one vCPU the pool starts at one thread and injects more slowly; a few spare threads
// keep requests moving while one is blocked (for example on a WAL checkpoint fsync).
ThreadPool.GetMinThreads(out _, out var completionThreads);
ThreadPool.SetMinThreads(Math.Max(8, Environment.ProcessorCount), completionThreads);

var builder = WebApplication.CreateSlimBuilder(args);
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole();
builder.Logging.SetMinimumLevel(LogLevel.Warning);
builder.WebHost.ConfigureKestrel(options =>
{
    options.AddServerHeader = false;
    options.Limits.KeepAliveTimeout = TimeSpan.FromSeconds(130);
    options.Listen(IPAddress.Parse(host), port, endpoint => endpoint.Protocols = HttpProtocols.Http1);
});
var app = builder.Build();
try
{
    database.Initialize();
}
catch (Exception ex) when (ex is SqliteException or InvalidOperationException)
{
    // Remain available so /health can report the database failure.
    app.Logger.LogError(ex, "Database initialization failed");
}

app.Use(async (context, next) =>
{
    try
    {
        await next(context);
    }
    catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
    {
        context.Abort();
    }
    catch (Exception ex)
    {
        app.Logger.LogError(ex, "Request failed");
        if (context.Response.HasStarted)
        {
            context.Abort();
            return;
        }
        context.Response.Clear();
        await JsonResponse.Error(context, 500, "internal server error");
    }
});

app.MapGet("/health", context =>
{
    var json = JsonResponse.Begin();
    try
    {
        database.CheckHealth();
    }
    catch (Exception ex)
    {
        json.WriteStartObject();
        json.WriteString("status"u8, "degraded");
        json.WriteString("db"u8, "unreachable");
        json.WriteString("error"u8, ex.Message);
        json.WriteEndObject();
        return JsonResponse.Send(context, 503);
    }
    json.WriteStartObject();
    json.WriteString("status"u8, "ok");
    json.WriteString("db"u8, "ok");
    json.WriteNumber("uptime_s"u8, (long)uptime.Elapsed.TotalSeconds);
    json.WriteEndObject();
    return JsonResponse.Send(context, 200);
});

app.MapGet("/feed", context =>
{
    database.WriteFeed(JsonResponse.Begin());
    return JsonResponse.Send(context, 200);
});

app.MapGet("/posts/{id}", context =>
{
    if (!IdParser.TryParse(context.Request.RouteValues["id"] as string, out var postId))
        return JsonResponse.Error(context, 400, "invalid post id");
    return database.WritePost(JsonResponse.Begin(), postId)
        ? JsonResponse.Send(context, 200)
        : JsonResponse.Error(context, 404, "post not found");
});

app.MapPost("/posts", async context =>
{
    var identity = auth.Authenticate(context.Request.Headers.Authorization.ToString(), out var authError);
    if (identity is null) { await JsonResponse.Error(context, 401, authError!); return; }

    string? body;
    try
    {
        using var document = await JsonDocument.ParseAsync(context.Request.Body, cancellationToken: context.RequestAborted);
        body = document.RootElement.ValueKind == JsonValueKind.Object
            && document.RootElement.TryGetProperty("body", out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()?.Trim() : null;
    }
    catch (JsonException)
    {
        await JsonResponse.Error(context, 400, "malformed JSON body");
        return;
    }
    if (string.IsNullOrEmpty(body)) { await JsonResponse.Error(context, 400, "body is required"); return; }
    // SQLite length() counts Unicode characters rather than UTF-16 code units; a string of at
    // most 500 UTF-16 code units can't exceed 500 characters, so only count longer ones.
    if (body.Length > 500 && CountRunes(body) > 500)
    {
        await JsonResponse.Error(context, 400, "body must be at most 500 characters");
        return;
    }
    var (id, createdAt) = await database.CreatePost(identity.UserId, body, context.RequestAborted);

    var json = JsonResponse.Begin();
    json.WriteStartObject();
    json.WritePropertyName("post"u8);
    json.WriteStartObject();
    json.WriteNumber("id"u8, id);
    json.WriteString("body"u8, body);
    json.WriteString("created_at"u8, createdAt);
    json.WriteString("author"u8, identity.Username);
    json.WriteNumber("like_count"u8, 0);
    json.WriteEndObject();
    json.WriteEndObject();
    await JsonResponse.Send(context, 201);
});

app.MapPost("/posts/{id}/like", async context =>
{
    var identity = auth.Authenticate(context.Request.Headers.Authorization.ToString(), out var authError);
    if (identity is null) { await JsonResponse.Error(context, 401, authError!); return; }
    if (!IdParser.TryParse(context.Request.RouteValues["id"] as string, out var postId))
    {
        await JsonResponse.Error(context, 400, "invalid post id");
        return;
    }
    var inserted = await database.LikePost(identity.UserId, postId, context.RequestAborted);
    if (inserted is null) { await JsonResponse.Error(context, 404, "post not found"); return; }

    var json = JsonResponse.Begin();
    json.WriteStartObject();
    json.WriteBoolean("liked"u8, true);
    json.WriteBoolean("already_liked"u8, !inserted.Value);
    json.WriteNumber("post_id"u8, postId);
    json.WriteEndObject();
    await JsonResponse.Send(context, inserted.Value ? 201 : 200);
});

app.MapFallback(context => JsonResponse.Error(context, 404, "not found"));
app.Run();

static int CountRunes(string value)
{
    var count = 0;
    foreach (var _ in value.EnumerateRunes()) count++;
    return count;
}

internal static class IdParser
{
    public static bool TryParse(string? value, out long id)
    {
        id = 0;
        // Vectorized digit check; long.TryParse alone would accept trailing '\0' characters.
        return !string.IsNullOrEmpty(value) && !value.AsSpan().ContainsAnyExceptInRange('0', '9')
            && long.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0;
    }
}

/// <summary>
/// Builds a response in a per-thread buffer so it goes out with Content-Length instead of chunked.
/// Begin and Send must run on the same thread with no await between them.
/// </summary>
internal static class JsonResponse
{
    [ThreadStatic] private static ArrayBufferWriter<byte>? buffer;
    [ThreadStatic] private static Utf8JsonWriter? writer;

    public static Utf8JsonWriter Begin()
    {
        buffer ??= new ArrayBufferWriter<byte>(4096);
        buffer.ResetWrittenCount();
        if (writer is null) writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { SkipValidation = true });
        else writer.Reset(buffer);
        return writer;
    }

    public static Task Send(HttpContext context, int status)
    {
        writer!.Flush();
        var response = context.Response;
        response.StatusCode = status;
        response.ContentType = "application/json; charset=utf-8";
        response.ContentLength = buffer!.WrittenCount;
        response.BodyWriter.Write(buffer.WrittenSpan);
        var flush = response.BodyWriter.FlushAsync();
        return flush.IsCompletedSuccessfully ? Task.CompletedTask : flush.AsTask();
    }

    public static Task Error(HttpContext context, int status, string message)
    {
        var json = Begin();
        json.WriteStartObject();
        json.WriteString("error"u8, message);
        json.WriteEndObject();
        return Send(context, status);
    }
}
