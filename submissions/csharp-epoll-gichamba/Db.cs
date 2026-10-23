using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using static Feed.Native;

namespace Feed;

/// <summary>Result of <see cref="Db.Like"/>.</summary>
internal enum LikeOutcome { Liked, AlreadyLiked, PostMissing, Failed }

/// <summary>
/// The one SQLite connection, used only by the event loop thread. Statements are prepared once.
/// Each batch of requests runs inside one transaction (Begin/Commit), so writes from many
/// requests share one commit and reads share one snapshot.
/// </summary>
internal sealed unsafe class Db {
    private const int CheckpointFrames = 1000; // WAL size that triggers a checkpoint

    // WAL size after the last commit. Static because the wal hook is a static callback.
    private static int _walFrames;

    private readonly nint _db;
    private readonly nint _feed, _post, _insertPost, _insertLike, _postExists, _selectOne, _begin, _commit, _rollback;

    /// <summary>Opens the database at <paramref name="path"/>, applies the pragmas and prepares every statement. Throws on any SQLite error.</summary>
    public Db(string path) {
        Check(sqlite3_initialize(), "initialize");
        nint db;
        byte[] file = Encoding.UTF8.GetBytes(path + "\0");
        fixed (byte* p = file) {
            int rc = sqlite3_open_v2(p, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, null);
            _db = db;
            Check(rc, "open");
        }

        // Rule 6: WAL with synchronous=NORMAL. Exclusive locking keeps the WAL index in heap
        // memory and skips file locking on every transaction (this process is the only user).
        // Checkpoints run after responses are sent, not inside COMMIT.
        Exec("PRAGMA locking_mode=EXCLUSIVE");
        Exec("PRAGMA journal_mode=WAL");
        Exec("PRAGMA synchronous=NORMAL");
        Exec("PRAGMA wal_autocheckpoint=0");
        Exec("PRAGMA mmap_size=1073741824");
        Exec("PRAGMA cache_size=-16384");
        Exec("PRAGMA temp_store=MEMORY");
        sqlite3_wal_hook(_db, &OnWal, 0);

        const string select = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id";
        _feed = Prepare(select + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
        _post = Prepare(select + " WHERE p.id=?1");
        _insertPost = Prepare("INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at");
        _insertLike = Prepare("INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING");
        _postExists = Prepare("SELECT 1 FROM posts WHERE id=?1");
        _selectOne = Prepare("SELECT 1");
        _begin = Prepare("BEGIN");
        _commit = Prepare("COMMIT");
        _rollback = Prepare("ROLLBACK");
    }

    /// <summary>True while a transaction is open.</summary>
    public bool InTransaction => sqlite3_get_autocommit(_db) == 0;

    /// <summary>Starts the batch transaction. False if it failed.</summary>
    public bool Begin() => Run(_begin) == SQLITE_DONE;

    /// <summary>Rolls back the open transaction, if any.</summary>
    public void Rollback() => Run(_rollback);

    /// <summary>Commits the batch. On failure the transaction is rolled back.</summary>
    public bool Commit() {
        if (Run(_commit) == SQLITE_DONE) {
            return true;
        }

        if (InTransaction) {
            Run(_rollback);
        }

        return false;
    }

    /// <summary>Copies frames from the WAL into the database once enough have built up.</summary>
    public void MaybeCheckpoint() {
        if (_walFrames < CheckpointFrames) {
            return;
        }

        _walFrames = 0;
        sqlite3_wal_checkpoint_v2(_db, null, SQLITE_CHECKPOINT_PASSIVE, null, null);
    }

    /// <summary>SQLite's wal hook: records the WAL size after each commit. Checkpointing is left to MaybeCheckpoint.</summary>
    [UnmanagedCallersOnly]
    private static int OnWal(nint arg, nint db, byte* name, int frames) {
        _walFrames = frames;
        return SQLITE_OK;
    }

    /// <summary>Writes {"posts":[...]} for the 20 newest posts. False if a statement failed.</summary>
    public bool Feed(Out o) {
        o.Bytes("{\"posts\":["u8);
        bool first = true;
        int rc;
        try {
            while ((rc = sqlite3_step(_feed)) == SQLITE_ROW) {
                if (!first) {
                    o.Byte((byte)',');
                }

                first = false;
                PostObject(_feed, o);
            }
        } finally {
            sqlite3_reset(_feed);
        }
        o.Bytes("]}"u8);
        return rc == SQLITE_DONE;
    }

    /// <summary>Writes {"post":{...}}. Returns 200, 404 or 500.</summary>
    public int Post(long id, Out o) {
        sqlite3_bind_int64(_post, 1, id);
        try {
            int rc = sqlite3_step(_post);
            if (rc == SQLITE_DONE) {
                return 404;
            }

            if (rc != SQLITE_ROW) {
                return 500;
            }

            o.Bytes("{\"post\":"u8);
            PostObject(_post, o);
            o.Byte((byte)'}');
            return 200;
        } finally {
            sqlite3_reset(_post);
        }
    }

    /// <summary>Inserts a post and writes its {"post":{...}} response. False if the insert failed.</summary>
    public bool CreatePost(long userId, ReadOnlySpan<byte> body, ReadOnlySpan<byte> username, Out o) {
        sqlite3_bind_int64(_insertPost, 1, userId);
        fixed (byte* b = body) {
            sqlite3_bind_text(_insertPost, 2, b, body.Length, 0); // 0 = SQLITE_STATIC: the pinned bytes outlive the statement run
            try {
                if (sqlite3_step(_insertPost) != SQLITE_ROW) {
                    return false;
                }

                o.Bytes("{\"post\":{\"id\":"u8);
                o.Int(sqlite3_column_int64(_insertPost, 0));
                o.Bytes(",\"body\":"u8);
                o.JsonString(body);
                o.Bytes(",\"created_at\":"u8);
                o.JsonString(Text(_insertPost, 1));
                o.Bytes(",\"author\":"u8);
                o.JsonString(username);
                o.Bytes(",\"like_count\":0}}"u8);
                // Stepping to DONE finishes the insert before the statement is reset.
                return sqlite3_step(_insertPost) == SQLITE_DONE;
            } finally {
                sqlite3_reset(_insertPost);
            }
        }
    }

    /// <summary>Records that the user likes the post. Reports whether the like is new, was already there, or the post does not exist.</summary>
    public LikeOutcome Like(long userId, long postId) {
        sqlite3_bind_int64(_insertLike, 1, userId);
        sqlite3_bind_int64(_insertLike, 2, postId);
        int rc = sqlite3_step(_insertLike);
        sqlite3_reset(_insertLike);
        if (rc != SQLITE_DONE) {
            return LikeOutcome.Failed;
        }

        if (sqlite3_changes(_db) == 1) {
            return LikeOutcome.Liked;
        }

        // Nothing inserted: either already liked or the post does not exist.
        sqlite3_bind_int64(_postExists, 1, postId);
        rc = sqlite3_step(_postExists);
        sqlite3_reset(_postExists);
        return rc switch {
            SQLITE_ROW => LikeOutcome.AlreadyLiked,
            SQLITE_DONE => LikeOutcome.PostMissing,
            _ => LikeOutcome.Failed,
        };
    }

    /// <summary>Runs SELECT 1. Returns null when it works, otherwise SQLite's error message.</summary>
    public string? Health() {
        int rc = sqlite3_step(_selectOne);
        sqlite3_reset(_selectOne);
        return rc == SQLITE_ROW ? null : ErrorMessage();
    }

    /// <summary>Writes the post JSON object for the statement's current row (columns as in the select in the constructor).</summary>
    private static void PostObject(nint stmt, Out o) {
        o.Bytes("{\"id\":"u8);
        o.Int(sqlite3_column_int64(stmt, 0));
        o.Bytes(",\"body\":"u8);
        o.JsonString(Text(stmt, 1));
        o.Bytes(",\"created_at\":"u8);
        o.JsonString(Text(stmt, 2));
        o.Bytes(",\"author\":"u8);
        o.JsonString(Text(stmt, 3));
        o.Bytes(",\"like_count\":"u8);
        o.Int(sqlite3_column_int64(stmt, 4));
        o.Byte((byte)'}');
    }

    /// <summary>The column's UTF-8 text, straight from SQLite's memory. Valid only until the statement is stepped or reset.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static ReadOnlySpan<byte> Text(nint stmt, int column) {
        byte* p = sqlite3_column_text(stmt, column);
        return new ReadOnlySpan<byte>(p, sqlite3_column_bytes(stmt, column));
    }

    /// <summary>Steps a statement once, resets it and returns the step result.</summary>
    private static int Run(nint stmt) {
        int rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        return rc;
    }

    /// <summary>The connection's last error message.</summary>
    private string ErrorMessage() => Marshal.PtrToStringUTF8((nint)sqlite3_errmsg(_db)) ?? "unknown error";

    /// <summary>Runs one SQL statement that returns no rows. Throws on failure.</summary>
    private void Exec(string sql) {
        byte[] bytes = Encoding.UTF8.GetBytes(sql + "\0");
        fixed (byte* p = bytes) {
            Check(sqlite3_exec(_db, p, 0, 0, null), sql);
        }
    }

    /// <summary>Prepares a statement that lives for the whole process. Throws on failure.</summary>
    private nint Prepare(string sql) {
        byte[] bytes = Encoding.UTF8.GetBytes(sql);
        nint stmt;
        fixed (byte* p = bytes) {
            Check(sqlite3_prepare_v3(_db, p, bytes.Length, SQLITE_PREPARE_PERSISTENT, &stmt, null), sql);
        }

        return stmt;
    }

    /// <summary>Throws unless <paramref name="rc"/> is SQLITE_OK.</summary>
    private void Check(int rc, string what) {
        if (rc != SQLITE_OK) {
            throw new InvalidOperationException($"SQLite {what} failed ({rc}): {(_db == 0 ? "" : ErrorMessage())}");
        }
    }
}
