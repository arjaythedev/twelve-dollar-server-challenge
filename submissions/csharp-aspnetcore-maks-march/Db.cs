using System.Text.Json;
using Microsoft.Data.Sqlite;

namespace FeedApi;

internal enum LikeOutcome
{
    Liked,
    AlreadyLiked,
    PostMissing,
}

/// <summary>
/// One connection, one lock. Every query is an index or primary-key lookup that finishes in
/// well under a millisecond and the box has a single core anyway, so serializing on the
/// connection keeps the SqliteConnection safe and makes SQLite lock contention impossible.
/// All commands are prepared once and reused: only the parameter values change per call.
/// </summary>
internal sealed class Db : IDisposable
{
    private const string PostSelect = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
          FROM posts p JOIN users u ON u.id = p.user_id
        """;

    private readonly SqliteConnection _conn;
    private readonly object _gate = new();

    private readonly SqliteCommand _feed;
    private readonly SqliteCommand _post;
    private readonly SqliteCommand _insertPost;
    private readonly SqliteCommand _insertLike;
    private readonly SqliteCommand _postExists;
    private readonly SqliteCommand _selectOne;

    public Db(string path)
    {
        _conn = new SqliteConnection(new SqliteConnectionStringBuilder
        {
            DataSource = path,
            Pooling = false,
        }.ToString());
        _conn.Open();

        // Rule 6: WAL + synchronous=NORMAL; the rest is read-side tuning.
        Exec("PRAGMA journal_mode = WAL");
        Exec("PRAGMA synchronous = NORMAL");
        Exec("PRAGMA busy_timeout = 5000");
        Exec("PRAGMA mmap_size = 1073741824");  // read through the OS page cache, no copies
        Exec("PRAGMA cache_size = -65536");     // 64 MiB
        Exec("PRAGMA temp_store = MEMORY");
        // Like the reference implementation: foreign keys stay off, so a like of a missing
        // post is told apart by the EXISTS guard below, not by an FK error.
        Exec("PRAGMA foreign_keys = OFF");

        _feed = Cmd(PostSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");

        _post = Cmd(PostSelect + " WHERE p.id = @id");
        _post.Parameters.Add(new SqliteParameter("@id", SqliteType.Integer));

        _insertPost = Cmd("INSERT INTO posts (user_id, body) VALUES (@user, @body) RETURNING id, created_at");
        _insertPost.Parameters.Add(new SqliteParameter("@user", SqliteType.Integer));
        _insertPost.Parameters.Add(new SqliteParameter("@body", SqliteType.Text));

        // Inserts only if the post exists; 0 changed rows means "already liked" or "no such post".
        _insertLike = Cmd("""
            INSERT INTO likes (user_id, post_id)
            SELECT @user, @post WHERE EXISTS (SELECT 1 FROM posts WHERE id = @post)
            ON CONFLICT (user_id, post_id) DO NOTHING
            """);
        _insertLike.Parameters.Add(new SqliteParameter("@user", SqliteType.Integer));
        _insertLike.Parameters.Add(new SqliteParameter("@post", SqliteType.Integer));

        _postExists = Cmd("SELECT 1 FROM posts WHERE id = @id");
        _postExists.Parameters.Add(new SqliteParameter("@id", SqliteType.Integer));

        _selectOne = Cmd("SELECT 1");
    }

    private void Exec(string sql)
    {
        using var cmd = _conn.CreateCommand();
        cmd.CommandText = sql;
        cmd.ExecuteNonQuery();
    }

    private SqliteCommand Cmd(string sql)
    {
        var cmd = _conn.CreateCommand();
        cmd.CommandText = sql;
        return cmd;
    }

    public void WriteFeed(PooledJson json)
    {
        lock (_gate)
        {
            using var reader = _feed.ExecuteReader();
            var w = json.Writer;
            w.WriteStartObject();
            w.WriteStartArray("posts");
            while (reader.Read()) WritePostObject(w, reader);
            w.WriteEndArray();
            w.WriteEndObject();
            w.Flush();
        }
    }

    /// Returns false (and writes nothing) when the post does not exist.
    public bool WritePost(PooledJson json, long id)
    {
        lock (_gate)
        {
            _post.Parameters[0].Value = id;
            using var reader = _post.ExecuteReader();
            if (!reader.Read()) return false;
            var w = json.Writer;
            w.WriteStartObject();
            w.WritePropertyName("post");
            WritePostObject(w, reader);
            w.WriteEndObject();
            w.Flush();
            return true;
        }
    }

    /// <summary>
    /// The second Read() steps the RETURNING statement to DONE, which commits the autocommit
    /// transaction before we return — a 201 is only sent for a committed row (rule 6).
    /// </summary>
    public (long Id, string CreatedAt) InsertPost(long userId, string body)
    {
        lock (_gate)
        {
            _insertPost.Parameters[0].Value = userId;
            _insertPost.Parameters[1].Value = body;
            using var reader = _insertPost.ExecuteReader();
            if (!reader.Read()) throw new InvalidOperationException("INSERT ... RETURNING returned no row");
            long id = reader.GetInt64(0);
            string createdAt = reader.GetString(1);
            while (reader.Read()) { }
            return (id, createdAt);
        }
    }

    public LikeOutcome Like(long userId, long postId)
    {
        lock (_gate)
        {
            _insertLike.Parameters[0].Value = userId;
            _insertLike.Parameters[1].Value = postId;
            if (_insertLike.ExecuteNonQuery() == 1) return LikeOutcome.Liked;

            _postExists.Parameters[0].Value = postId;
            using var reader = _postExists.ExecuteReader();
            return reader.Read() ? LikeOutcome.AlreadyLiked : LikeOutcome.PostMissing;
        }
    }

    /// null when the database answers; otherwise the error message for the 503 body.
    public string? Health()
    {
        try
        {
            lock (_gate)
            {
                using var reader = _selectOne.ExecuteReader();
                reader.Read();
            }
            return null;
        }
        catch (Exception ex)
        {
            return ex.Message;
        }
    }

    private static void WritePostObject(Utf8JsonWriter w, SqliteDataReader reader)
    {
        w.WriteStartObject();
        w.WriteNumber("id", reader.GetInt64(0));
        w.WriteString("body", reader.GetString(1));
        w.WriteString("created_at", reader.GetString(2));
        w.WriteString("author", reader.GetString(3));
        w.WriteNumber("like_count", reader.GetInt64(4));
        w.WriteEndObject();
    }

    public void Dispose() => _conn.Dispose();
}
