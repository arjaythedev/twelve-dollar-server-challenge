using System.Buffers;
using Microsoft.Data.Sqlite;
using SQLitePCL;

namespace FeedApi.Data;

// The owner serializes every operation and disposes this before its SqliteConnection.
internal sealed class SqliteJsonStatement : IDisposable
{
    private readonly sqlite3 _connection;
    private readonly sqlite3_stmt _statement;
    private readonly bool _hasPostId;
    private bool _disposed;

    public SqliteJsonStatement(SqliteConnection connection, string sql)
    {
        ArgumentNullException.ThrowIfNull(connection);
        ArgumentNullException.ThrowIfNull(sql);
        _connection = connection.Handle
            ?? throw new InvalidOperationException("Open the connection before preparing JSON statements.");

        sqlite3_stmt? statement = null;
        try
        {
            RequireResult(raw.sqlite3_prepare_v3(_connection, sql, raw.SQLITE_PREPARE_PERSISTENT, out statement), raw.SQLITE_OK);
            if (statement is null || statement.IsInvalid || raw.sqlite3_stmt_readonly(statement) != 1)
                throw new ArgumentException("The JSON statement must be a read-only query.", nameof(sql));

            var parameterCount = raw.sqlite3_bind_parameter_count(statement);
            if (parameterCount > 1 ||
                (parameterCount == 1 && raw.sqlite3_bind_parameter_index(statement, "$id") != 1))
                throw new ArgumentException("Only the optional $id parameter is supported.", nameof(sql));

            _hasPostId = parameterCount == 1;
            _statement = statement;
        }
        catch
        {
            statement?.Dispose();
            throw;
        }
    }

    // Returns -1 for no row. The buffer is advanced only after SQLite finishes successfully.
    public int CopyTo(IBufferWriter<byte> destination, long? postId = null)
    {
        ArgumentNullException.ThrowIfNull(destination);
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_hasPostId != postId.HasValue)
            throw new ArgumentException("Supply a post ID only for the parameterized query.", nameof(postId));

        var needsReset = false;
        try
        {
            if (postId.HasValue)
                RequireResult(raw.sqlite3_bind_int64(_statement, 1, postId.Value), raw.SQLITE_OK);

            needsReset = true;
            var result = raw.sqlite3_step(_statement);
            var byteCount = -1;
            if (result != raw.SQLITE_DONE)
            {
                RequireResult(result, raw.SQLITE_ROW);
                if (raw.sqlite3_column_type(_statement, 0) != raw.SQLITE_BLOB)
                    throw new InvalidOperationException("The JSON query did not return a BLOB.");

                var bytes = raw.sqlite3_column_blob(_statement, 0);
                if (bytes.IsEmpty)
                    throw new InvalidOperationException("The JSON query returned an empty BLOB.");

                byteCount = bytes.Length;
                bytes.CopyTo(destination.GetSpan(byteCount));
                // The next step invalidates bytes. Its contents are already copied and are not used again.
                RequireResult(raw.sqlite3_step(_statement), raw.SQLITE_DONE);
            }

            var resetResult = raw.sqlite3_reset(_statement);
            needsReset = false;
            RequireResult(resetResult, raw.SQLITE_OK);
            if (byteCount >= 0)
                destination.Advance(byteCount);
            return byteCount;
        }
        finally
        {
            if (needsReset)
            {
                // Preserve the original failure. The repository discards a failed owning session.
                _ = raw.sqlite3_reset(_statement);
            }
        }
    }

    private void RequireResult(int actual, int expected)
    {
        if (actual == expected) return;
        SqliteException.ThrowExceptionForRC(actual, _connection);
        throw new InvalidOperationException($"Expected SQLite result {expected}; received {actual}.");
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _statement.Dispose();
    }
}
