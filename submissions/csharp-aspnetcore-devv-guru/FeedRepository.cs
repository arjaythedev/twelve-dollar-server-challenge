using System.Diagnostics;
using System.Threading.RateLimiting;
using FeedApi.Data;
using Microsoft.Data.Sqlite;

namespace FeedApi;

internal enum LikeOutcome { Created, AlreadyLiked, MissingPost }

internal sealed class FeedRepository : IDisposable
{
    private readonly string _connectionString;
    private readonly object _database = new();
    private FeedSession? _session;
    private bool _disposed;

    private readonly ConcurrencyLimiter _writes = new(new ConcurrencyLimiterOptions
    {
        PermitLimit = 1,
        QueueLimit = 256,
        QueueProcessingOrder = QueueProcessingOrder.OldestFirst
    });
    private readonly DatabaseMetrics _metrics;

    public FeedRepository(AppOptions options)
    {
        _connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = options.DatabasePath,
            Mode = SqliteOpenMode.ReadWrite,
            Cache = SqliteCacheMode.Private,
            Pooling = false,
            DefaultTimeout = 2
        }.ToString();
        _metrics = new DatabaseMetrics(_writes);
    }

    public string Initialize()
    {
        lock (_database)
            return GetSession().SqliteVersion;
    }

    public void CheckHealth()
    {
        using var duration = _metrics.Measure("health");
        lock (_database)
        {
            try
            {
                if (Convert.ToInt64(GetSession().Health.ExecuteScalar()) != 1)
                    throw new InvalidOperationException("Database health query failed.");
            }
            catch
            {
                DiscardSession();
                throw;
            }
        }
    }

    public byte[] GetFeedJson()
    {
        using var duration = _metrics.Measure("feed");
        lock (_database)
        {
            try
            {
                return (byte[])GetSession().Feed.ExecuteScalar()!;
            }
            catch
            {
                DiscardSession();
                throw;
            }
        }
    }

    public byte[]? GetPostJson(long id)
    {
        using var duration = _metrics.Measure("post");
        lock (_database)
        {
            try
            {
                var command = GetSession().Post;
                command.Parameters[0].Value = id;
                return (byte[]?)command.ExecuteScalar();
            }
            catch
            {
                DiscardSession();
                throw;
            }
        }
    }

    public async Task<Post> CreatePostAsync(UserIdentity user, string body, CancellationToken cancellationToken)
    {
        using var lease = await AcquireWriteAsync(cancellationToken);
        using var duration = _metrics.Measure("create");
        lock (_database)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                var session = GetSession();
                using var transaction = session.Connection.BeginTransaction();
                var command = session.CreatePost;
                try
                {
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
                    FeedSession.Reset(command);
                }
            }
            catch
            {
                DiscardSession();
                throw;
            }
        }
    }

    public async Task<LikeOutcome> LikeAsync(long userId, long postId, CancellationToken cancellationToken)
    {
        using var lease = await AcquireWriteAsync(cancellationToken);
        using var duration = _metrics.Measure("like");
        lock (_database)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                var session = GetSession();
                using var transaction = session.Connection.BeginTransaction();
                var command = session.Like;
                var exists = session.PostExists;
                try
                {
                    command.Transaction = transaction;
                    command.Parameters[0].Value = userId;
                    command.Parameters[1].Value = postId;
                    LikeOutcome outcome;
                    if (command.ExecuteNonQuery() == 1)
                        outcome = LikeOutcome.Created;
                    else
                    {
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
                    FeedSession.Reset(command);
                    FeedSession.Reset(exists);
                }
            }
            catch
            {
                DiscardSession();
                throw;
            }
        }
    }

    // Call only while holding _database, including during initialization and shutdown.
    private FeedSession GetSession()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return _session ??= new FeedSession(_connectionString);
    }

    private void DiscardSession()
    {
        _session?.Dispose();
        _session = null;
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
        throw new InvalidOperationException("The database write queue is full.");
    }

    public void Dispose()
    {
        lock (_database)
        {
            if (_disposed) return;
            _disposed = true;
            DiscardSession();
        }
        _metrics.Dispose();
        _writes.Dispose();
    }
}
