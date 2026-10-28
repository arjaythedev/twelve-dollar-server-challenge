using System.Text.Json.Serialization;

namespace FeedApi;

internal sealed record Post(
    [property: JsonPropertyName("id"), JsonPropertyOrder(0)] long Id,
    [property: JsonPropertyName("body"), JsonPropertyOrder(1)] string Body,
    [property: JsonPropertyName("created_at"), JsonPropertyOrder(2)] string CreatedAt,
    [property: JsonPropertyName("author"), JsonPropertyOrder(3)] string Author,
    [property: JsonPropertyName("like_count"), JsonPropertyOrder(4)] long LikeCount);

internal sealed record FeedResponse([property: JsonPropertyName("posts")] List<Post> Posts);
internal sealed record PostResponse([property: JsonPropertyName("post")] Post Post);
internal sealed record ApiError([property: JsonPropertyName("error")] string Error);
internal sealed record HealthResponse(
    [property: JsonPropertyName("status"), JsonPropertyOrder(0)] string Status,
    [property: JsonPropertyName("db"), JsonPropertyOrder(1)] string Database,
    [property: JsonPropertyName("uptime_s"), JsonPropertyOrder(2)] long UptimeSeconds);
internal sealed record DegradedHealthResponse(
    [property: JsonPropertyName("status"), JsonPropertyOrder(0)] string Status,
    [property: JsonPropertyName("db"), JsonPropertyOrder(1)] string Database,
    [property: JsonPropertyName("error"), JsonPropertyOrder(2)] string Error);
internal sealed record LikeResponse(
    [property: JsonPropertyName("liked"), JsonPropertyOrder(0)] bool Liked,
    [property: JsonPropertyName("already_liked"), JsonPropertyOrder(1)] bool AlreadyLiked,
    [property: JsonPropertyName("post_id"), JsonPropertyOrder(2)] long PostId);

[JsonSerializable(typeof(FeedResponse))]
[JsonSerializable(typeof(PostResponse))]
[JsonSerializable(typeof(ApiError))]
[JsonSerializable(typeof(HealthResponse))]
[JsonSerializable(typeof(DegradedHealthResponse))]
[JsonSerializable(typeof(LikeResponse))]
internal partial class ApiJsonContext : JsonSerializerContext;
