using Microsoft.Data.Sqlite;

namespace Feed;

// Property order is the JSON key order; the snake_case naming policy in Program.cs gives created_at, like_count, etc.
public sealed record Post(long Id, string Body, string CreatedAt, string Author, long LikeCount);

public sealed record PostResponse(Post Post);

public sealed record FeedResponse(IEnumerable<Post> Posts);

public sealed record LikeResponse(bool Liked, bool AlreadyLiked, long PostId);

public sealed record HealthResponse(string Status, string Db, long UptimeS);

public sealed record HealthDegradedResponse(string Status, string Db, string Error);

public sealed record ErrorResponse(string Error);

public sealed class Db(string connectionString)
{
    // Microsoft.Data.Sqlite pools connections, so a new SqliteConnection per request is cheap.
    public SqliteConnection Connect() => new(connectionString);
}
