using Microsoft.Data.Sqlite;

namespace FeedApi.Data;

// Only the caller leasing a read session or holding the write permit may use it.
internal abstract class PreparedSqliteSession : IDisposable
{
    private readonly List<SqliteCommand> _commands = [];
    public SqliteConnection Connection { get; }

    protected PreparedSqliteSession(string connectionString) => Connection = OpenConnection(connectionString);

    public static SqliteConnection OpenConnection(string connectionString)
    {
        var connection = new SqliteConnection(connectionString);
        try
        {
            connection.Open();
            using var command = connection.CreateCommand();
            command.CommandText = "PRAGMA synchronous=FULL";
            command.ExecuteNonQuery();
            return connection;
        }
        catch
        {
            connection.Dispose();
            throw;
        }
    }

    protected SqliteCommand Prepare(string sql, params (string Name, SqliteType Type)[] parameters)
    {
        var command = Connection.CreateCommand();
        _commands.Add(command);
        command.CommandText = sql;
        foreach (var (name, type) in parameters)
            command.Parameters.Add(name, type).Value = DBNull.Value;
        command.Prepare();
        return command;
    }

    public void Reset()
    {
        foreach (var command in _commands)
        {
            command.Transaction = null;
            foreach (SqliteParameter parameter in command.Parameters)
                parameter.Value = DBNull.Value;
        }
    }

    public void Dispose()
    {
        foreach (var command in _commands)
            command.Dispose();
        Connection.Dispose();
    }
}

internal sealed class FeedReadSession : PreparedSqliteSession
{
    private const string PostColumns = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
          FROM posts p JOIN users u ON u.id = p.user_id
        """;

    public SqliteCommand Health { get; }
    public SqliteCommand Feed { get; }
    public SqliteCommand Post { get; }
    public bool Reusable { get; set; } = true;

    public FeedReadSession(string connectionString) : base(connectionString)
    {
        try
        {
            Health = Prepare("SELECT 1");
            Feed = Prepare(PostColumns + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
            Post = Prepare(PostColumns + " WHERE p.id = $id", ("$id", SqliteType.Integer));
        }
        catch
        {
            Dispose();
            throw;
        }
    }
}

internal sealed class FeedWriteSession : PreparedSqliteSession
{
    public SqliteCommand CreatePost { get; }
    public SqliteCommand Like { get; }
    public SqliteCommand PostExists { get; }

    public FeedWriteSession(string connectionString) : base(connectionString)
    {
        try
        {
            CreatePost = Prepare("INSERT INTO posts (user_id, body) VALUES ($user, $body) RETURNING id, created_at",
                ("$user", SqliteType.Integer), ("$body", SqliteType.Text));
            Like = Prepare("""
                INSERT INTO likes (user_id, post_id)
                SELECT $user, $post WHERE EXISTS (SELECT 1 FROM posts WHERE id = $post)
                ON CONFLICT (user_id, post_id) DO NOTHING
                """, ("$user", SqliteType.Integer), ("$post", SqliteType.Integer));
            PostExists = Prepare("SELECT EXISTS (SELECT 1 FROM posts WHERE id = $post)", ("$post", SqliteType.Integer));
        }
        catch
        {
            Dispose();
            throw;
        }
    }
}
