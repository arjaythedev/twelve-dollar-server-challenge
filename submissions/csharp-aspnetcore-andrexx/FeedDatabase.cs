using System.Collections.Concurrent;
using System.Text.Json;
using Microsoft.Data.Sqlite;

internal sealed class FeedDatabase
{
    private const string PostQuery = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
        FROM posts p JOIN users u ON u.id = p.user_id
        """;
    // Reads go through the OS page cache shared by all connections instead of per-connection copies.
    private const string MmapPragma = "PRAGMA mmap_size=536870912;";
    private const int SqliteConstraint = 19;

    private readonly string readerConnectionString;
    private readonly string writerConnectionString;
    // Long-lived connections with prepared statements; Microsoft.Data.Sqlite pooling would
    // re-run connection pragmas and re-prepare every statement on each request.
    private readonly ConcurrentBag<Reader> readers = new();
    private readonly SemaphoreSlim writeLock = new(1, 1);
    private Writer? writer;

    public FeedDatabase(string path)
    {
        var builder = new SqliteConnectionStringBuilder
        {
            DataSource = Path.GetFullPath(path),
            Mode = SqliteOpenMode.ReadWrite,
            Pooling = false,
            DefaultTimeout = 30
        };
        readerConnectionString = builder.ToString();
        builder.ForeignKeys = true;
        writerConnectionString = builder.ToString();
    }

    public void Initialize()
    {
        writeLock.Wait();
        try { writer ??= new Writer(writerConnectionString); }
        finally { writeLock.Release(); }
    }

    public void CheckHealth() => Read(reader => reader.Health.ExecuteScalar());

    /// <summary>Writes <c>{"posts":[...]}</c> with the 20 newest posts.</summary>
    public void WriteFeed(Utf8JsonWriter json) => Read(reader =>
    {
        json.WriteStartObject();
        json.WriteStartArray("posts"u8);
        using (var rows = reader.Feed.ExecuteReader())
        {
            while (rows.Read()) WritePost(json, rows);
        }
        json.WriteEndArray();
        json.WriteEndObject();
    });

    /// <summary>Writes <c>{"post":...}</c>, or nothing and returns false when the post doesn't exist.</summary>
    public bool WritePost(Utf8JsonWriter json, long id) => Read(reader =>
    {
        reader.PostId.Value = id;
        using var rows = reader.Post.ExecuteReader();
        if (!rows.Read()) return false;
        json.WriteStartObject();
        json.WritePropertyName("post"u8);
        WritePost(json, rows);
        json.WriteEndObject();
        return true;
    });

    public async Task<(long Id, string CreatedAt)> CreatePost(long userId, string body, CancellationToken cancellation)
    {
        await writeLock.WaitAsync(cancellation);
        try
        {
            return Write(writer =>
            {
                writer.InsertPostUser.Value = userId;
                writer.InsertPostBody.Value = body;
                using var rows = writer.InsertPost.ExecuteReader();
                if (!rows.Read()) throw new InvalidOperationException("Insert returned no post");
                var created = (rows.GetInt64(0), rows.GetString(1));
                // Finish RETURNING so SQLite commits the autocommit transaction before responding.
                while (rows.Read()) { }
                return created;
            });
        }
        finally { writeLock.Release(); }
    }

    /// <summary>True when the like was inserted, false when it already existed, null when the post doesn't exist.</summary>
    public async Task<bool?> LikePost(long userId, long postId, CancellationToken cancellation)
    {
        await writeLock.WaitAsync(cancellation);
        try
        {
            return Write(writer =>
            {
                writer.LikeUser.Value = userId;
                writer.LikePostId.Value = postId;
                if (writer.Like.ExecuteNonQuery() == 1) return true;
                // Rare path: tell an existing like apart from a missing post.
                writer.PostExistsId.Value = postId;
                return writer.PostExists.ExecuteScalar() is null ? (bool?)null : false;
            });
        }
        finally { writeLock.Release(); }
    }

    private static void WritePost(Utf8JsonWriter json, SqliteDataReader rows)
    {
        json.WriteStartObject();
        json.WriteNumber("id"u8, rows.GetInt64(0));
        json.WriteString("body"u8, rows.GetString(1));
        json.WriteString("created_at"u8, rows.GetString(2));
        json.WriteString("author"u8, rows.GetString(3));
        json.WriteNumber("like_count"u8, rows.GetInt64(4));
        json.WriteEndObject();
    }

    private T Read<T>(Func<Reader, T> action)
    {
        var reader = readers.TryTake(out var pooled) ? pooled : new Reader(readerConnectionString);
        T result;
        try { result = action(reader); }
        catch
        {
            // Don't hand a connection in an unknown state to the next request.
            reader.Dispose();
            throw;
        }
        readers.Add(reader);
        return result;
    }

    private void Read(Action<Reader> action) => Read(reader => { action(reader); return 0; });

    // Callers hold writeLock.
    private T Write<T>(Func<Writer, T> action)
    {
        writer ??= new Writer(writerConnectionString);
        try { return action(writer); }
        catch (Exception ex) when (ex is not SqliteException { SqliteErrorCode: SqliteConstraint })
        {
            // A constraint failure leaves the connection usable; anything else gets a fresh one.
            writer.Dispose();
            writer = null;
            throw;
        }
    }

    private static SqliteConnection Open(string connectionString, string pragmas)
    {
        var connection = new SqliteConnection(connectionString);
        try
        {
            connection.Open();
            using var command = connection.CreateCommand();
            command.CommandText = pragmas;
            command.ExecuteNonQuery();
            return connection;
        }
        catch
        {
            connection.Dispose();
            throw;
        }
    }

    private static SqliteCommand Prepare(SqliteConnection connection, string sql, params SqliteParameter[] parameters)
    {
        var command = connection.CreateCommand();
        command.CommandText = sql;
        command.Parameters.AddRange(parameters);
        command.Prepare();
        return command;
    }

    private sealed class Reader : IDisposable
    {
        private readonly SqliteConnection connection;
        public readonly SqliteCommand Health, Feed, Post;
        public readonly SqliteParameter PostId = new("$id", SqliteType.Integer);

        public Reader(string connectionString)
        {
            connection = Open(connectionString, MmapPragma);
            try
            {
                Health = Prepare(connection, "SELECT 1;");
                Feed = Prepare(connection, PostQuery + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20;");
                Post = Prepare(connection, PostQuery + " WHERE p.id = $id;", PostId);
            }
            catch
            {
                connection.Dispose();
                throw;
            }
        }

        public void Dispose()
        {
            Health?.Dispose();
            Feed?.Dispose();
            Post?.Dispose();
            connection.Dispose();
        }
    }

    private sealed class Writer : IDisposable
    {
        private readonly SqliteConnection connection;
        public readonly SqliteCommand InsertPost, Like, PostExists;
        public readonly SqliteParameter InsertPostUser = new("$user", SqliteType.Integer);
        public readonly SqliteParameter InsertPostBody = new("$body", SqliteType.Text);
        public readonly SqliteParameter LikeUser = new("$user", SqliteType.Integer);
        public readonly SqliteParameter LikePostId = new("$post", SqliteType.Integer);
        public readonly SqliteParameter PostExistsId = new("$post", SqliteType.Integer);

        public Writer(string connectionString)
        {
            connection = Open(connectionString, "PRAGMA synchronous=NORMAL; " + MmapPragma);
            try
            {
                using (var command = connection.CreateCommand())
                {
                    command.CommandText = "PRAGMA journal_mode=WAL;";
                    if (!string.Equals(command.ExecuteScalar()?.ToString(), "wal", StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("Database must support WAL mode");
                }
                InsertPost = Prepare(connection,
                    "INSERT INTO posts (user_id, body) VALUES ($user, $body) RETURNING id, created_at;",
                    InsertPostUser, InsertPostBody);
                // One statement for the common case; the WHERE clause is also required for upsert after SELECT.
                Like = Prepare(connection, """
                    INSERT INTO likes (user_id, post_id)
                    SELECT $user, $post WHERE EXISTS (SELECT 1 FROM posts WHERE id = $post)
                    ON CONFLICT (user_id, post_id) DO NOTHING;
                    """, LikeUser, LikePostId);
                PostExists = Prepare(connection, "SELECT 1 FROM posts WHERE id = $post;", PostExistsId);
            }
            catch
            {
                connection.Dispose();
                throw;
            }
        }

        public void Dispose()
        {
            InsertPost?.Dispose();
            Like?.Dispose();
            PostExists?.Dispose();
            connection.Dispose();
        }
    }
}
