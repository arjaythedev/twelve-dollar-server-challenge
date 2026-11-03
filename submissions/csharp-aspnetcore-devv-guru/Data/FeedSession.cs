using Microsoft.Data.Sqlite;
using SQLitePCL;

namespace FeedApi.Data;

// The repository serializes all use of this connection and its prepared commands.
internal sealed class FeedSession : IDisposable
{
    private readonly List<SqliteCommand> _commands = [];
    private readonly List<SqliteJsonStatement> _jsonStatements = [];
    public SqliteConnection Connection { get; }
    public string SqliteVersion { get; }
    public SqliteCommand Health { get; }
    public SqliteJsonStatement Feed { get; }
    public SqliteJsonStatement Post { get; }
    public SqliteCommand CreatePost { get; }
    public SqliteCommand Like { get; }
    public SqliteCommand PostExists { get; }

    public FeedSession(string connectionString)
    {
        Connection = new SqliteConnection(connectionString);
        try
        {
            Connection.Open();
            using var setup = Connection.CreateCommand();
            // Set exclusive ownership before the first WAL access, including on recovery.
            setup.CommandText = "PRAGMA locking_mode=EXCLUSIVE";
            if (!string.Equals(setup.ExecuteScalar()?.ToString(), "exclusive", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("SQLite exclusive locking is required.");
            setup.CommandText = "PRAGMA journal_mode=WAL";
            if (!string.Equals(setup.ExecuteScalar()?.ToString(), "wal", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("SQLite must support WAL mode.");
            setup.CommandText = "PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON";
            setup.ExecuteNonQuery();
            setup.CommandText = "SELECT sqlite_version()";
            SqliteVersion = (string)setup.ExecuteScalar()!;
            if (Version.Parse(SqliteVersion) < new Version(3, 51, 3))
                throw new InvalidOperationException("A SQLite engine with the WAL-reset fix is required (3.51.3 or later).");
            setup.CommandText = "PRAGMA synchronous";
            if (Convert.ToInt64(setup.ExecuteScalar()) != 1)
                throw new InvalidOperationException("SQLite synchronous=NORMAL is required.");
            setup.CommandText = "PRAGMA foreign_keys";
            if (Convert.ToInt64(setup.ExecuteScalar()) != 1)
                throw new InvalidOperationException("SQLite foreign key enforcement is required.");

            // These indexed lookups do not need a new plan when only an ID changes.
            var result = raw.sqlite3_db_config(Connection.Handle!, raw.SQLITE_DBCONFIG_ENABLE_QPSG, 1, out var enabled);
            if (result != raw.SQLITE_OK || enabled != 1)
                throw new InvalidOperationException("SQLite query planner stability is required.");

            Health = Prepare("SELECT 1");
            Feed = PrepareJson("""
                SELECT CAST(json_object('posts', json_group_array(json_object(
                    'id', p.id, 'body', p.body, 'created_at', p.created_at,
                    'author', p.author, 'like_count', p.like_count))) AS BLOB)
                  FROM (
                    SELECT p.id, p.body, p.created_at, u.username AS author,
                           (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
                      FROM posts p JOIN users u ON u.id = p.user_id
                     ORDER BY p.created_at DESC, p.id DESC LIMIT 20
                  ) p
                """);
            Post = PrepareJson("""
                SELECT CAST(json_object('post', json_object(
                    'id', p.id, 'body', p.body, 'created_at', p.created_at,
                    'author', u.username,
                    'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))) AS BLOB)
                  FROM posts p JOIN users u ON u.id = p.user_id
                 WHERE p.id = $id
                """);
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

    private SqliteJsonStatement PrepareJson(string sql)
    {
        var statement = new SqliteJsonStatement(Connection, sql);
        _jsonStatements.Add(statement);
        return statement;
    }

    private SqliteCommand Prepare(string sql, params (string Name, SqliteType Type)[] parameters)
    {
        var command = Connection.CreateCommand();
        _commands.Add(command);
        command.CommandText = sql;
        foreach (var (name, type) in parameters)
            command.Parameters.Add(name, type).Value = DBNull.Value;
        command.Prepare();
        return command;
    }

    public static void Reset(SqliteCommand command)
    {
        command.Transaction = null;
        foreach (SqliteParameter parameter in command.Parameters)
            parameter.Value = DBNull.Value;
    }

    public void Dispose()
    {
        foreach (var statement in _jsonStatements)
            statement.Dispose();
        foreach (var command in _commands)
            command.Dispose();
        Connection.Dispose();
    }
}
