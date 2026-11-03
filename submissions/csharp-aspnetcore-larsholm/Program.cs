using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using Dapper;
using Feed;
using Microsoft.Data.Sqlite;

var builder = WebApplication.CreateBuilder(args);

// SQLITE_PATH, JWT_SECRET, HOST and PORT come in as environment variables, which are part of IConfiguration.
var config = builder.Configuration;
var connectionString = new SqliteConnectionStringBuilder { DataSource = Required("SQLITE_PATH") }.ToString();
builder.WebHost.UseUrls($"http://{config["HOST"] ?? "127.0.0.1"}:{config["PORT"] ?? "3000"}");

builder.Services.ConfigureHttpJsonOptions(o => o.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
builder.Services.AddSingleton(new Db(connectionString));
builder.Services.AddSingleton(new JwtValidator(Required("JWT_SECRET")));

var app = builder.Build();
var uptime = Stopwatch.StartNew();

app.UseExceptionHandler(handler => handler.Run(async context =>
{
    context.Response.StatusCode = StatusCodes.Status500InternalServerError;
    await context.Response.WriteAsJsonAsync(new ErrorResponse("internal server error"));
}));

const string PostSelect = """
    SELECT p.id AS Id, p.body AS Body, p.created_at AS CreatedAt, u.username AS Author,
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS LikeCount
      FROM posts p JOIN users u ON u.id = p.user_id
    """;

app.MapGet("/health", async (Db db) =>
{
    try
    {
        await using var conn = db.Connect();
        await conn.ExecuteScalarAsync<long>("SELECT 1");
        return Results.Ok(new HealthResponse("ok", "ok", (long)uptime.Elapsed.TotalSeconds));
    }
    catch (Exception ex)
    {
        return Results.Json(new HealthDegradedResponse("degraded", "unreachable", ex.Message), statusCode: 503);
    }
});

app.MapGet("/feed", async (Db db) =>
{
    await using var conn = db.Connect();
    var posts = await conn.QueryAsync<Post>($"{PostSelect} ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    return Results.Ok(new FeedResponse(posts));
});

app.MapGet("/posts/{id}", async (string id, Db db) =>
{
    if (!TryParseId(id, out var postId)) return Error(400, "invalid post id");

    await using var conn = db.Connect();
    var post = await conn.QuerySingleOrDefaultAsync<Post>($"{PostSelect} WHERE p.id = @postId", new { postId });
    return post is null ? Error(404, "post not found") : Results.Ok(new PostResponse(post));
});

app.MapPost("/posts", async (HttpRequest request, JwtValidator jwt, Db db) =>
{
    var auth = jwt.Validate(request.Headers.Authorization);
    if (auth.User is not { } user) return Error(401, auth.Error!);

    JsonNode? json;
    try { json = await JsonNode.ParseAsync(request.Body); }
    catch (JsonException) { return Error(400, "malformed JSON body"); }

    var body = (json as JsonObject)?["body"] is JsonValue value && value.TryGetValue(out string? text) ? text.Trim() : "";
    if (body.Length == 0) return Error(400, "body is required");
    if (body.Length > 500) return Error(400, "body must be at most 500 characters");

    await using var conn = db.Connect();
    var (newId, createdAt) = await conn.QuerySingleAsync<(long, string)>(
        "INSERT INTO posts (user_id, body) VALUES (@UserId, @Body) RETURNING id, created_at",
        new { UserId = user.Id, Body = body });
    return Results.Json(new PostResponse(new Post(newId, body, createdAt, user.Username, 0)), statusCode: 201);
});

app.MapPost("/posts/{id}/like", async (string id, HttpRequest request, JwtValidator jwt, Db db) =>
{
    var auth = jwt.Validate(request.Headers.Authorization);
    if (auth.User is not { } user) return Error(401, auth.Error!);
    if (!TryParseId(id, out var postId)) return Error(400, "invalid post id");

    await using var conn = db.Connect();
    var exists = await conn.ExecuteScalarAsync<bool>("SELECT EXISTS (SELECT 1 FROM posts WHERE id = @postId)", new { postId });
    if (!exists) return Error(404, "post not found");

    var inserted = await conn.ExecuteAsync(
        "INSERT INTO likes (user_id, post_id) VALUES (@UserId, @PostId) ON CONFLICT (user_id, post_id) DO NOTHING",
        new { UserId = user.Id, PostId = postId });
    return Results.Json(new LikeResponse(true, inserted == 0, postId), statusCode: inserted == 1 ? 201 : 200);
});

app.MapFallback("{*path}", () => Error(404, "not found"));

app.Run();

string Required(string key) => config[key] ?? throw new InvalidOperationException($"{key} is not set");

static bool TryParseId(string value, out long id) =>
    long.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0;

static IResult Error(int status, string message) => Results.Json(new ErrorResponse(message), statusCode: status);
