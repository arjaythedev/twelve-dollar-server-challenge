using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using static Feed.Native;

namespace Feed;

/// <summary>Result of <see cref="Db.Like"/>.</summary>
internal enum LikeOutcome { Liked, AlreadyLiked, PostMissing, Failed }

/// <summary>
/// One SQLite connection, used by one thread at a time (SQLITE_THREADSAFE=2). The reader is shared
/// by the ring workers under App.Gate and is opened query_only. The writer has its own connection
/// on the <see cref="Writer"/> thread. Statements are prepared once. A batch uses one transaction:
/// reads share a snapshot and writes share one commit.
/// </summary>
internal sealed unsafe class Db {
    private const int CheckpointFrames = 8192; // WAL size that wakes the checkpoint thread
    private const int RangeSpan = 256;         // widest id span the feed's range scans handle

    private const string PostColumns = "SELECT p.id,p.body,p.created_at,u.username";
    private const string LikeCount = "(SELECT count(*) FROM likes l WHERE l.post_id=p.id)";
    private const string PostJoin = " FROM posts p JOIN users u ON u.id=p.user_id";

    // WAL size after the last commit. Static because the wal hook is a static callback.
    private static int _walFrames;

    private readonly nint _db;
    private readonly nint _feedIds, _likeRange, _postRange, _feedRef, _post, _insertPost, _createdAt,
        _insertLike, _postExists, _selectOne, _begin, _beginImmediate, _commit, _rollback;

    // Scratch for feeds whose rows don't come back in feed order.
    private readonly long[] _ids = new long[20];
    private readonly int[] _likes = new int[RangeSpan];
    private readonly int[] _rowStart = new int[RangeSpan];
    private readonly int[] _rowEnd = new int[RangeSpan];
    private readonly Out _rows = new(64 * 1024);

    /// <summary>Opens the database at <paramref name="path"/>, applies the pragmas and prepares every statement. Throws on any SQLite error.</summary>
    public Db(string path, bool writer) {
        Check(sqlite3_initialize(), "initialize");
        nint db;
        byte[] file = Encoding.UTF8.GetBytes(path + "\0");
        fixed (byte* p = file) {
            int rc = sqlite3_open_v2(p, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, null);
            _db = db;
            Check(rc, "open");
        }

        // Rule 6: WAL with synchronous=NORMAL. Automatic checkpoints are off. A separate thread
        // runs passive checkpoints so their I/O does not sit on the ring threads. Locking stays
        // NORMAL because several connections share the file and the WAL index in -shm.
        Exec("PRAGMA journal_mode=WAL");
        Exec("PRAGMA synchronous=NORMAL");
        Exec("PRAGMA foreign_keys=ON");
        Exec("PRAGMA wal_autocheckpoint=0");
        Exec("PRAGMA journal_size_limit=16777216");
        Exec("PRAGMA mmap_size=1073741824");
        Exec("PRAGMA cache_size=-1024");
        Exec("PRAGMA temp_store=MEMORY");
        if (writer) {
            Exec("PRAGMA busy_timeout=5000");
            sqlite3_wal_hook(_db, &OnWal, 0);
        } else {
            Exec("PRAGMA query_only=1"); // the reader must never write: writes go through the Write
        }

        _feedIds = Prepare("SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20");
        _likeRange = Prepare("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
        _postRange = Prepare(PostColumns + PostJoin + " WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC");
        _feedRef = Prepare(PostColumns + "," + LikeCount + PostJoin + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
        _post = Prepare(PostColumns + "," + LikeCount + PostJoin + " WHERE p.id=?1");
        _insertPost = Prepare("INSERT INTO posts(user_id,body) VALUES(?1,?2)");
        _createdAt = Prepare("SELECT created_at FROM posts WHERE id=?1");
        _insertLike = Prepare("INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING");
        _postExists = Prepare("SELECT 1 FROM posts WHERE id=?1");
        _selectOne = Prepare("SELECT 1");
        _begin = Prepare("BEGIN");
        _beginImmediate = Prepare("BEGIN IMMEDIATE");
        _commit = Prepare("COMMIT");
        _rollback = Prepare("ROLLBACK");
    }

    /// <summary>True while a transaction is open.</summary>
    public bool InTransaction => sqlite3_get_autocommit(_db) == 0;

    /// <summary>Starts a read batch's transaction (one snapshot for every read in it). False if it failed.</summary>
    public bool Begin() => Run(_begin) == SQLITE_DONE;

    /// <summary>Starts a write batch's transaction, taking the write lock up front. False if it failed.</summary>
    public bool BeginImmediate() {
        int rc = sqlite3_step_blocking(_beginImmediate);
        sqlite3_reset(_beginImmediate);
        return rc == SQLITE_DONE;
    }

    /// <summary>Rolls back the open transaction, if any.</summary>
    public void Rollback() => Run(_rollback);

    /// <summary>Commits the batch. On failure the transaction is rolled back.</summary>
    public bool Commit() {
        int rc = sqlite3_step_blocking(_commit);
        sqlite3_reset(_commit);
        if (rc == SQLITE_DONE) {
            return true;
        }

        if (InTransaction) {
            Run(_rollback);
        }

        return false;
    }

    /// <summary>Writer only: wakes the checkpointer after enough WAL frames have accumulated.</summary>
    public void RequestCheckpoint(Checkpointer checkpointer) {
        if (_walFrames >= CheckpointFrames) {
            checkpointer.Wake();
        }
    }

    /// <summary>SQLite's wal hook: records the WAL size after each commit. Checkpointing is left to Checkpoint.</summary>
    [UnmanagedCallersOnly]
    private static int OnWal(nint arg, nint db, byte* name, int frames) {
        _walFrames = frames;
        return SQLITE_OK;
    }

    /// <summary>
    /// Writes {"posts":[...]} for the 20 newest posts. False if a statement failed.
    /// The 20 ids come from the (created_at, id) index. When they lie within a narrow id range,
    /// as new posts do, one scan of likes_post_id_idx counts all their likes and one rowid scan
    /// of posts fetches their rows, instead of 20 separate count lookups. Otherwise it falls back
    /// to the reference query. Every value is read live in this request's transaction.
    /// </summary>
    public bool Feed(Out o) {
        int n = 0;
        int rc;
        while ((rc = sqlite3_step(_feedIds)) == SQLITE_ROW) {
            _ids[n++] = sqlite3_column_int64(_feedIds, 0);
        }
        sqlite3_reset(_feedIds);
        if (rc != SQLITE_DONE) {
            return false;
        }

        long hi = long.MinValue, lo = long.MaxValue;
        for (int i = 0; i < n; i++) {
            hi = Math.Max(hi, _ids[i]);
            lo = Math.Min(lo, _ids[i]);
        }
        if (n == 0 || hi - lo >= RangeSpan) {
            return FeedReference(o);
        }

        int start = o.Len;
        int result = FeedRange(o, n, lo, hi);
        if (result > 0) {
            return true;
        }

        o.Len = start;
        return result == 0 && FeedReference(o); // 0: the range rows did not match the ids; use the plain query
    }

    /// <summary>The range-scan feed. 1 on success, 0 if the rows didn't match the ids, -1 on a SQLite error.</summary>
    private int FeedRange(Out o, int n, long lo, long hi) {
        int span = (int)(hi - lo) + 1;
        Array.Clear(_likes, 0, span);
        sqlite3_bind_int64(_likeRange, 1, lo);
        sqlite3_bind_int64(_likeRange, 2, hi);
        int rc;
        while ((rc = sqlite3_step(_likeRange)) == SQLITE_ROW) {
            _likes[sqlite3_column_int64(_likeRange, 0) - lo]++;
        }
        sqlite3_reset(_likeRange);
        if (rc != SQLITE_DONE) {
            return -1;
        }

        // Fast path: the newest ids are hi, hi-1, ..., hi-n+1, so the descending rowid scan
        // returns exactly the feed, in feed order.
        bool contiguous = span == n;
        for (int i = 0; contiguous && i < n; i++) {
            contiguous = _ids[i] == hi - i;
        }

        sqlite3_bind_int64(_postRange, 1, lo);
        sqlite3_bind_int64(_postRange, 2, hi);
        try {
            if (contiguous) {
                o.Bytes("{\"posts\":["u8);
                int i = 0;
                while ((rc = sqlite3_step(_postRange)) == SQLITE_ROW) {
                    long id = sqlite3_column_int64(_postRange, 0);
                    if (i >= n || id != _ids[i]) {
                        return 0;
                    }

                    if (i > 0) {
                        o.Byte((byte)',');
                    }

                    PostObject(_postRange, id, _likes[id - lo], o);
                    i++;
                }
                if (rc != SQLITE_DONE) {
                    return -1;
                }

                if (i != n) {
                    return 0;
                }

                o.Bytes("]}"u8);
                return 1;
            }

            // General path: format each row into scratch, then emit them in feed order.
            _rows.Reset();
            Array.Fill(_rowEnd, -1, 0, span);
            while ((rc = sqlite3_step(_postRange)) == SQLITE_ROW) {
                long id = sqlite3_column_int64(_postRange, 0);
                int k = (int)(id - lo);
                _rowStart[k] = _rows.Len;
                PostObject(_postRange, id, _likes[k], _rows);
                _rowEnd[k] = _rows.Len;
            }
            if (rc != SQLITE_DONE) {
                return -1;
            }

            o.Bytes("{\"posts\":["u8);
            for (int i = 0; i < n; i++) {
                int k = (int)(_ids[i] - lo);
                if (_rowEnd[k] < 0) {
                    return 0;
                }

                if (i > 0) {
                    o.Byte((byte)',');
                }

                o.Bytes(_rows.Written[_rowStart[k].._rowEnd[k]]);
            }
            o.Bytes("]}"u8);
            return 1;
        } finally {
            sqlite3_reset(_postRange);
        }
    }

    /// <summary>The reference feed query: 20 newest posts with a count subquery each.</summary>
    private bool FeedReference(Out o) {
        o.Bytes("{\"posts\":["u8);
        bool first = true;
        int rc;
        try {
            while ((rc = sqlite3_step(_feedRef)) == SQLITE_ROW) {
                if (!first) {
                    o.Byte((byte)',');
                }

                first = false;
                PostObject(_feedRef, sqlite3_column_int64(_feedRef, 0), sqlite3_column_int64(_feedRef, 4), o);
            }
        } finally {
            sqlite3_reset(_feedRef);
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
            PostObject(_post, sqlite3_column_int64(_post, 0), sqlite3_column_int64(_post, 4), o);
            o.Byte((byte)'}');
            return 200;
        } finally {
            sqlite3_reset(_post);
        }
    }

    /// <summary>
    /// Inserts a post and writes its {"post":{...}} response. False if the insert failed.
    /// created_at comes from the column default; it is read back by rowid (cheaper than RETURNING,
    /// which materializes its result in a temporary table).
    /// </summary>
    public bool CreatePost(long userId, ReadOnlySpan<byte> body, ReadOnlySpan<byte> username, Out o) {
        sqlite3_bind_int64(_insertPost, 1, userId);
        fixed (byte* b = body) {
            sqlite3_bind_text(_insertPost, 2, b, body.Length, 0); // 0 = SQLITE_STATIC: the pinned bytes outlive the statement run
            int rc = sqlite3_step(_insertPost);
            sqlite3_reset(_insertPost);
            if (rc != SQLITE_DONE) {
                return false;
            }
        }

        long id = sqlite3_last_insert_rowid(_db);
        sqlite3_bind_int64(_createdAt, 1, id);
        try {
            if (sqlite3_step(_createdAt) != SQLITE_ROW) {
                return false;
            }

            o.Bytes("{\"post\":{\"id\":"u8);
            o.Int(id);
            o.Bytes(",\"body\":"u8);
            o.JsonString(body);
            o.Bytes(",\"created_at\":"u8);
            o.JsonString(Text(_createdAt, 0));
            o.Bytes(",\"author\":"u8);
            o.JsonString(username);
            o.Bytes(",\"like_count\":0}}"u8);
            return true;
        } finally {
            sqlite3_reset(_createdAt);
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

    /// <summary>Writes the post JSON object for the statement's current row (id, body, created_at, username columns).</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void PostObject(nint stmt, long id, long likes, Out o) {
        o.Bytes("{\"id\":"u8);
        o.Int(id);
        o.Bytes(",\"body\":"u8);
        o.JsonString(Text(stmt, 1));
        o.Bytes(",\"created_at\":"u8);
        o.JsonString(Text(stmt, 2));
        o.Bytes(",\"author\":"u8);
        o.JsonString(Text(stmt, 3));
        o.Bytes(",\"like_count\":"u8);
        o.Int(likes);
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

/// <summary>
/// Runs PASSIVE WAL checkpoints on its own connection and thread. PASSIVE does not wait for readers
/// or writers; the reader and writer can keep working while this thread does checkpoint I/O.
/// </summary>
internal sealed unsafe class Checkpointer {
    private readonly nint _db;
    private readonly AutoResetEvent _wake = new(false);
    private volatile int _pending;

    public Checkpointer(string path) {
        nint db;
        byte[] file = Encoding.UTF8.GetBytes(path + "\0");
        fixed (byte* p = file) {
            if (sqlite3_open_v2(p, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, null) != SQLITE_OK) {
                throw new InvalidOperationException("SQLite checkpoint connection failed to open");
            }
        }
        _db = db;
        foreach (string sql in new[] { "PRAGMA synchronous=NORMAL", "PRAGMA wal_autocheckpoint=0", "PRAGMA journal_size_limit=16777216", "PRAGMA cache_size=-1024" }) {
            byte[] bytes = Encoding.UTF8.GetBytes(sql + "\0");
            fixed (byte* p = bytes) {
                sqlite3_exec(_db, p, 0, 0, null);
            }
        }
        Thread t = new(Run, 256 * 1024) { IsBackground = true, Name = "checkpoint" };
        t.Start();
    }

    /// <summary>Requests a checkpoint; does nothing extra if one is already pending.</summary>
    public void Wake() {
        if (_pending == 0) {
            _pending = 1;
            _wake.Set();
        }
    }

    private void Run() {
        while (true) {
            _wake.WaitOne(1000);
            _pending = 0;
            int log, done;
            int rc = sqlite3_wal_checkpoint_v2(_db, null, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
            if (rc != SQLITE_OK && rc != 5 /* SQLITE_BUSY */) {
                Log($"checkpoint failed: {rc}");
            }
        }
    }
}
