using System.Diagnostics;
using System.Threading.RateLimiting;
using FeedApi.Data;
using Microsoft.Data.Sqlite;
using Microsoft.Extensions.ObjectPool;

namespace FeedApi;

internal enum LikeOutcome { Created, AlreadyLiked, MissingPost }

internal sealed class FeedRepository : IDisposable
{
    private readonly string _connectionString;
    private readonly ObjectPool<FeedReadSession> _reads;
    private FeedWriteSession? _writer;

    // Only the permit holder may use _writer. Queued requests hold no session or worker.
    private readonly ConcurrencyLimiter _writes = new(new ConcurrencyLimiterOptions
    {
        PermitLimit = 1,
        QueueLimit = 256,
        QueueProcessingOrder = QueueProcessingOrder.OldestFirst
    });
    private readonly DatabaseMetrics _metrics;

    public FeedRepository(AppOptions options)
    {
        var connection = new SqliteConnectionStringBuilder
        {
            DataSource = options.DatabasePath,
            Mode = SqliteOpenMode.ReadWrite,
            Cache = SqliteCacheMode.Private,
            // ObjectPool owns connection lifetimes; avoid a second pool underneath it.
            Pooling = false,
            ForeignKeys = true,
            DefaultTimeout = 2
        };
        _connectionString = connection.ToString();
        connection.Mode = SqliteOpenMode.ReadOnly;
        _reads = new DefaultObjectPoolProvider
        {
            MaximumRetained = Math.Clamp(Environment.ProcessorCount * 2, 2, 8)
        }.Create(new ReadSessionPolicy(connection.ToString()));
        _metrics = new DatabaseMetrics(_writes);
    }

    public string Initialize()
    {
        using var connection = PreparedSqliteSession.OpenConnection(_connectionString);
        using var command = connection.CreateCommand();
        command.CommandText = "PRAGMA journal_mode=WAL";
        if (!string.Equals(command.ExecuteScalar()?.ToString(), "wal", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("SQLite must support WAL mode.");
        command.CommandText = "SELECT sqlite_version()";
        var version = (string)command.ExecuteScalar()!;
        if (Version.Parse(version) < new Version(3, 51, 3))
            throw new InvalidOperationException("A SQLite engine with the WAL-reset fix is required (3.51.3 or later).");
        command.CommandText = "PRAGMA synchronous";
        if (Convert.ToInt64(command.ExecuteScalar()) != 2)
            throw new InvalidOperationException("SQLite synchronous=FULL is required.");
        command.CommandText = "PRAGMA foreign_keys";
        if (Convert.ToInt64(command.ExecuteScalar()) != 1)
            throw new InvalidOperationException("SQLite foreign key enforcement is required.");
        // Preparing the actual commands also validates all required columns.
        _reads.Return(_reads.Get());
        _writer = new FeedWriteSession(_connectionString);
        return version;
    }

    public void CheckHealth()
    {
        using var duration = _metrics.Measure("health");
        var session = _reads.Get();
        try
        {
            if (Convert.ToInt64(session.Health.ExecuteScalar()) != 1)
                throw new InvalidOperationException("Database health query failed.");
        }
        catch
        {
            session.Reusable = false;
            throw;
        }
        finally
        {
            _reads.Return(session);
        }
    }

    public List<Post> GetFeed()
    {
        using var duration = _metrics.Measure("feed");
        var session = _reads.Get();
        try
        {
            using var reader = session.Feed.ExecuteReader();
            var posts = new List<Post>(20);
            while (reader.Read())
                posts.Add(ReadPost(reader));
            return posts;
        }
        catch
        {
            session.Reusable = false;
            throw;
        }
        finally
        {
            _reads.Return(session);
        }
    }

    public Post? GetPost(long id)
    {
        using var duration = _metrics.Measure("post");
        var session = _reads.Get();
        try
        {
            session.Post.Parameters[0].Value = id;
            using var reader = session.Post.ExecuteReader();
            return reader.Read() ? ReadPost(reader) : null;
        }
        catch
        {
            session.Reusable = false;
            throw;
        }
        finally
        {
            _reads.Return(session);
        }
    }

    public async Task<Post> CreatePostAsync(UserIdentity user, string body, CancellationToken cancellationToken)
    {
        using var lease = await AcquireWriteAsync(cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        using var duration = _metrics.Measure("create");
        try
        {
            var writer = _writer ??= new FeedWriteSession(_connectionString);
            try
            {
                using var transaction = writer.Connection.BeginTransaction();
                var command = writer.CreatePost;
                command.Transaction = transaction;
                command.Parameters[0].Value = user.Id;
                command.Parameters[1].Value = body;
                Post post;
                using (var reader = command.ExecuteReader())
                {
                    if (!reader.Read())
                        throw new InvalidOperationException("INSERT did not return the new post.");
                    post = new Post(reader.GetInt64(0), body, reader.GetString(1), user.Username, 0);
                }
                using (_metrics.MeasureCommit("create"))
                    transaction.Commit();
                return post;
            }
            finally
            {
                writer.Reset();
            }
        }
        catch
        {
            DiscardWriter();
            throw;
        }
    }

    public async Task<LikeOutcome> LikeAsync(long userId, long postId, CancellationToken cancellationToken)
    {
        using var lease = await AcquireWriteAsync(cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        using var duration = _metrics.Measure("like");
        try
        {
            var writer = _writer ??= new FeedWriteSession(_connectionString);
            try
            {
                using var transaction = writer.Connection.BeginTransaction();
                var command = writer.Like;
                command.Transaction = transaction;
                command.Parameters[0].Value = userId;
                command.Parameters[1].Value = postId;
                LikeOutcome outcome;
                if (command.ExecuteNonQuery() == 1)
                    outcome = LikeOutcome.Created;
                else
                {
                    var exists = writer.PostExists;
                    exists.Transaction = transaction;
                    exists.Parameters[0].Value = postId;
                    outcome = Convert.ToInt64(exists.ExecuteScalar()) == 1 ? LikeOutcome.AlreadyLiked : LikeOutcome.MissingPost;
                }
                using (_metrics.MeasureCommit("like"))
                    transaction.Commit();
                return outcome;
            }
            finally
            {
                writer.Reset();
            }
        }
        catch
        {
            DiscardWriter();
            throw;
        }
    }

    private void DiscardWriter()
    {
        _writer?.Dispose();
        _writer = null;
    }

    private async ValueTask<RateLimitLease> AcquireWriteAsync(CancellationToken cancellationToken)
    {
        var start = Stopwatch.GetTimestamp();
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(TimeSpan.FromSeconds(2));
        RateLimitLease lease;
        try
        {
            lease = await _writes.AcquireAsync(1, deadline.Token);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            _metrics.WriteRejected("timeout");
            throw new TimeoutException("Timed out waiting for the database write permit.");
        }
        finally
        {
            _metrics.WriteWait(Stopwatch.GetElapsedTime(start).TotalMilliseconds);
        }
        if (lease.IsAcquired) return lease;
        lease.Dispose();
        _metrics.WriteRejected("queue_full");
        // The API contract maps overload failures to a generic 500.
        throw new InvalidOperationException("The database write queue is full.");
    }

    private static Post ReadPost(SqliteDataReader reader) =>
        new(reader.GetInt64(0), reader.GetString(1), reader.GetString(2), reader.GetString(3), reader.GetInt64(4));

    public void Dispose()
    {
        (_reads as IDisposable)?.Dispose();
        DiscardWriter();
        _metrics.Dispose();
        _writes.Dispose();
    }

    private sealed class ReadSessionPolicy(string connectionString) : PooledObjectPolicy<FeedReadSession>
    {
        public override FeedReadSession Create() => new(connectionString);
        public override bool Return(FeedReadSession session)
        {
            if (!session.Reusable) return false;
            session.Reset();
            return true;
        }
    }
}
