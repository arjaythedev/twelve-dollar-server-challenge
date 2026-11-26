using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using static Feed.Native;

namespace Feed;

/// <summary>Result of <see cref="Db.Like"/>.</summary>
internal enum LikeOutcome { Liked, AlreadyLiked, PostMissing, Failed }

/// <summary>
/// What the request thread and the checkpoint thread share about the WAL. A PASSIVE checkpoint never blocks writes, so
/// under steady writes it never finds every frame copied, and the WAL would grow without limit. So at
/// <see cref="HoldFrames"/> the loop holds writes (reads go on): the checkpoint thread copies the last frames and starts
/// the WAL again itself, so that the header fsync of a new WAL does not stop the request thread.
/// </summary>
internal static class Wal {
    /// <summary>A hold lasts as long as the checkpoint's fsyncs, not its copying, so a big WAL means fewer holds, not longer ones.</summary>
    public static readonly int HoldFrames = int.TryParse(Environment.GetEnvironmentVariable("MB_HOLD_FRAMES"), out int n) && n > 0 ? n : 15000;

    public static int Frames;      // WAL size after the last checkpoint (checkpoint thread)
    public static int HoldWrites;  // 1: writes wait until every frame is copied (request thread)
    public static int CaughtUp;    // 1: with writes held, every frame is copied (checkpoint thread)
    public static int RestartGate; // 1: the checkpoint thread is starting the WAL; 2: the loop is ending a hold that timed out
    public static int Commits;     // successful commits since the start (request thread)

    private static readonly AutoResetEvent Waker = new(false);

    /// <summary>Wakes the checkpoint thread at once when a hold starts, so the hold does not wait out its sleep.</summary>
    public static void Wake() => Waker.Set();

    public static void Sleep() => Waker.WaitOne(50);
}

/// <summary>
/// Reads walk SQLite's B-tree cursors (shim.c) inside the pass's transaction, and this class decodes the record bytes
/// straight into the arena: no bytecode engine, no sqlite3_column calls, no copies. The feed takes the 20 ids off the
/// index, then walks the posts table backwards from the newest id, so the rows come in response order. Writes are SQL.
/// A second connection on a low-priority thread runs the checkpoints, so their fsyncs never stop a request.
/// </summary>
internal sealed unsafe class Db {
    private const int FeedSize = 20;
    private const int Posts = 0, Users = 1, FeedIndex = 2, LikesIndex = 3;

    private readonly nint _db;
    private readonly nint _feed, _insertPost, _insertLike, _ping;
    private readonly void* _posts, _users, _feedIndex, _likesIndex;
    private readonly long[] _ids = new long[FeedSize];
    private int _commitFailures;

    /// <summary>True while the pass's transaction is open.</summary>
    public bool InTransaction { get; private set; }

    /// <summary>One column of a record: serial type and bytes.</summary>
    private struct Column {
        public ulong Type;
        public byte* Data;
        public int Length;
    }

    public Db(string path) {
        Check(sqlite3_initialize(), "initialize");
        if (td_vfs_install() != 0) {
            throw new InvalidOperationException("the unix-excl VFS is not available");
        }

        _db = Open(path);
        // unix-excl: only this process opens the file, so no fcntl lock per transaction, and the WAL index lives in heap
        // memory shared by the two connections. On top of it, a shim in shim.c writes each commit with one pwrite.
        Exec(_db, "PRAGMA journal_mode = WAL");
        Exec(_db, "PRAGMA synchronous = NORMAL");
        Exec(_db, "PRAGMA wal_autocheckpoint = 0");
        // The WAL is cut to nothing each time it starts again. Frames aren't page-aligned, so overwriting an old WAL page that
        // reclaim has evicted reads it back from disk first, inside the COMMIT, on the request thread. Past the end of the file
        // there is nothing to read. The kernel also gets the old WAL's page cache back at once.
        Exec(_db, "PRAGMA journal_size_limit = 0");
        Exec(_db, "PRAGMA busy_timeout = 5000");
        // No mmap: a page in SQLite's cache is found with one lookup and keeps its parsed header; an mmap page is looked
        // up in the WAL index and parsed again on every fetch.
        Exec(_db, "PRAGMA mmap_size = 0");
        // The hot pages live here, in anonymous memory, which the kernel can't take back on a box with no swap. In the kernel's
        // page cache they look cold (SQLite's cache keeps them from being touched), so at the memory wall reclaim evicts them,
        // and the request thread blocks on a disk read in pread. MB_CACHE_KB overrides the size, for experiments.
        int cacheKb = int.TryParse(Environment.GetEnvironmentVariable("MB_CACHE_KB"), out int kb) && kb > 0 ? kb : 65536;
        Exec(_db, $"PRAGMA cache_size = -{cacheKb}");
        Exec(_db, "PRAGMA temp_store = MEMORY");

        byte* error = stackalloc byte[256];
        if (td_attach(_db, error, 256) != 0) {
            throw new InvalidOperationException(Marshal.PtrToStringUTF8((nint)error));
        }

        _posts = td_cursor(Posts);
        _users = td_cursor(Users);
        _feedIndex = td_cursor(FeedIndex);
        _likesIndex = td_cursor(LikesIndex);

        // The reference query answers for data the B-tree plan does not expect: a post without a user row, ids with gaps.
        _feed = Prepare("SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
        _insertPost = Prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
        _insertLike = Prepare("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
        _ping = Prepare("SELECT 1");

        new Checkpointer(path).Start();
    }

    private static nint Open(string path) {
        byte[] file = Encoding.UTF8.GetBytes(path + "\0");
        nint db;
        // MB_VFS=0 leaves the shim out, for experiments.
        byte[] vfsName = Environment.GetEnvironmentVariable("MB_VFS") == "0" ? "unix-excl\0"u8.ToArray() : "unix-excl-1write\0"u8.ToArray();
        fixed (byte* p = file, vfs = vfsName) {
            int rc = sqlite3_open_v2(p, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs);
            if (rc != SQLITE_OK) {
                throw new InvalidOperationException($"SQLite open failed ({rc})");
            }
        }

        return db;
    }

    private bool EnsureTransaction() {
        if (InTransaction) {
            return true;
        }

        InTransaction = true; // Commit must run even if this fails, to report the lost pass
        return td_begin() == 0;
    }

    /// <summary>Commits the pass. False if it was rolled back, by this failure or by SQLite earlier in the pass.</summary>
    public bool Commit() {
        InTransaction = false;
        if (td_commit() == 0) {
            Volatile.Write(ref Wal.Commits, Wal.Commits + 1);
            return true;
        }

        if (_commitFailures++ < 20) {
            Console.Error.WriteLine($"commit failed: {ErrorMessage(_db)}");
        }

        return false;
    }

    /// <summary>Writes {"posts":[...]}. False if SQLite reported an error.</summary>
    public bool Feed(Out o) {
        if (!EnsureTransaction()) {
            return false;
        }

        Column* cols = stackalloc Column[4];
        int n = 0;
        int r = td_first(_feedIndex);
        while (r == 1 && n < FeedSize) {
            uint len;
            byte* rec = td_payload(_feedIndex, &len);
            if (rec == null || !Decode(rec, len, 2, cols) || !Int(in cols[1], out _ids[n])) {
                return SqlFeed(o);
            }

            n++;
            r = td_next(_feedIndex);
        }

        if (r < 0) {
            return false;
        }

        if (n == 0) {
            o.Bytes("{\"posts\":[]}"u8);
            return true;
        }

        for (int i = 1; i < n; i++) {
            if (_ids[i] != _ids[i - 1] - 1) {
                return SqlFeed(o); // not one run of ids: the backward walk would not give response order
            }
        }

        long hi = _ids[0], lo = _ids[n - 1];
        uint* counts = stackalloc uint[FeedSize];
        new Span<uint>(counts, FeedSize).Clear();
        if (td_count_likes(_likesIndex, lo, hi, counts) < 0) {
            return false;
        }

        int start = o.Len;
        o.Bytes("{\"posts\":["u8);
        r = td_seek(_posts, hi);
        for (int i = 0; i < n; i++) {
            if (r < 0) {
                return false;
            }

            if (r == 0 || td_rowid(_posts) != _ids[i]) {
                o.Len = start;
                return SqlFeed(o);
            }

            uint len;
            byte* rec = td_payload(_posts, &len);
            if (rec == null || !Decode(rec, len, 4, cols) || !Int(in cols[1], out long userId) || !IsText(in cols[2]) || !IsText(in cols[3])) {
                o.Len = start;
                return SqlFeed(o);
            }

            if (i > 0) {
                o.Byte((byte)',');
            }

            o.Bytes("{\"id\":"u8);
            o.Int(_ids[i]);
            o.Bytes(",\"body\":"u8);
            o.JsonString(new ReadOnlySpan<byte>(cols[2].Data, cols[2].Length));
            o.Bytes(",\"created_at\":"u8);
            o.JsonString(new ReadOnlySpan<byte>(cols[3].Data, cols[3].Length));
            o.Bytes(",\"author\":"u8);
            int found = Author(userId, o);
            if (found <= 0) {
                o.Len = start;
                return found == 0 && SqlFeed(o); // a post without a user: the JOIN drops it
            }

            o.Bytes(",\"like_count\":"u8);
            o.Int(counts[_ids[i] - lo]);
            o.Byte((byte)'}');
            if (i + 1 < n) {
                r = td_prev(_posts);
            }
        }

        o.Bytes("]}"u8);
        return true;
    }

    /// <summary>The username of a user, as a JSON string. 1 written, 0 no such user, -1 error.</summary>
    private int Author(long userId, Out o) {
        int r = td_seek(_users, userId);
        if (r <= 0) {
            return r;
        }

        Column* cols = stackalloc Column[2];
        uint len;
        byte* rec = td_payload(_users, &len);
        if (rec == null || !Decode(rec, len, 2, cols) || !IsText(in cols[1])) {
            return -1;
        }

        o.JsonString(new ReadOnlySpan<byte>(cols[1].Data, cols[1].Length));
        return 1;
    }

    private bool SqlFeed(Out o) {
        o.Bytes("{\"posts\":["u8);
        bool first = true;
        int rc;
        while ((rc = sqlite3_step(_feed)) == SQLITE_ROW) {
            if (!first) {
                o.Byte((byte)',');
            }

            first = false;
            o.Bytes("{\"id\":"u8);
            o.Int(sqlite3_column_int64(_feed, 0));
            o.Bytes(",\"body\":"u8);
            o.JsonString(Text(_feed, 1));
            o.Bytes(",\"created_at\":"u8);
            o.JsonString(Text(_feed, 2));
            o.Bytes(",\"author\":"u8);
            o.JsonString(Text(_feed, 3));
            o.Bytes(",\"like_count\":"u8);
            o.Int(sqlite3_column_int64(_feed, 4));
            o.Byte((byte)'}');
        }

        sqlite3_reset(_feed);
        o.Bytes("]}"u8);
        return rc == SQLITE_DONE;
    }

    /// <summary>Writes {"post":{...}}. Returns 200, 404 or 500.</summary>
    public int Post(long id, Out o) {
        if (!EnsureTransaction()) {
            return 500;
        }

        int r = td_seek(_posts, id);
        if (r <= 0) {
            return r == 0 ? 404 : 500;
        }

        Column* cols = stackalloc Column[4];
        uint len;
        byte* rec = td_payload(_posts, &len);
        if (rec == null || !Decode(rec, len, 4, cols) || !Int(in cols[1], out long userId) || !IsText(in cols[2]) || !IsText(in cols[3])) {
            return 500;
        }

        int start = o.Len;
        o.Bytes("{\"post\":{\"id\":"u8);
        o.Int(id);
        o.Bytes(",\"body\":"u8);
        o.JsonString(new ReadOnlySpan<byte>(cols[2].Data, cols[2].Length));
        o.Bytes(",\"created_at\":"u8);
        o.JsonString(new ReadOnlySpan<byte>(cols[3].Data, cols[3].Length));
        o.Bytes(",\"author\":"u8);
        int found = Author(userId, o);
        if (found <= 0) {
            o.Len = start;
            return found == 0 ? 404 : 500; // the JOIN finds no row
        }

        uint likes = 0;
        if (td_count_likes(_likesIndex, id, id, &likes) < 0) {
            o.Len = start;
            return 500;
        }

        o.Bytes(",\"like_count\":"u8);
        o.Int(likes);
        o.Bytes("}}"u8);
        return 200;
    }

    /// <summary>Inserts a post and writes its {"post":{...}} response. False if the insert failed.</summary>
    public bool CreatePost(long userId, ReadOnlySpan<byte> body, ReadOnlySpan<byte> username, Out o) {
        if (!EnsureTransaction()) {
            return false;
        }

        sqlite3_bind_int64(_insertPost, 1, userId);
        fixed (byte* b = body) {
            sqlite3_bind_text(_insertPost, 2, b, body.Length, 0); // SQLITE_STATIC: the pinned bytes outlive the run
            bool ok = false;
            if (sqlite3_step(_insertPost) == SQLITE_ROW) {
                o.Bytes("{\"post\":{\"id\":"u8);
                o.Int(sqlite3_column_int64(_insertPost, 0));
                o.Bytes(",\"body\":"u8);
                o.JsonString(body);
                o.Bytes(",\"created_at\":"u8);
                o.JsonString(Text(_insertPost, 1));
                o.Bytes(",\"author\":"u8);
                o.JsonString(username);
                o.Bytes(",\"like_count\":0}}"u8);
                ok = sqlite3_step(_insertPost) == SQLITE_DONE; // finishes the insert before the reset
            }

            sqlite3_reset(_insertPost);
            return ok;
        }
    }

    /// <summary>Records that the user likes the post. The post is checked with one B-tree seek.</summary>
    public LikeOutcome Like(long userId, long postId) {
        if (!EnsureTransaction()) {
            return LikeOutcome.Failed;
        }

        int r = td_seek(_posts, postId);
        if (r <= 0) {
            return r == 0 ? LikeOutcome.PostMissing : LikeOutcome.Failed;
        }

        sqlite3_bind_int64(_insertLike, 1, userId);
        sqlite3_bind_int64(_insertLike, 2, postId);
        int rc = sqlite3_step(_insertLike);
        sqlite3_reset(_insertLike);
        if (rc != SQLITE_DONE) {
            return LikeOutcome.Failed;
        }

        return sqlite3_changes(_db) == 1 ? LikeOutcome.Liked : LikeOutcome.AlreadyLiked;
    }

    /// <summary>Runs SELECT 1. Null when it works, otherwise SQLite's error message.</summary>
    public string? Health() {
        if (!EnsureTransaction()) {
            return ErrorMessage(_db);
        }

        int rc = sqlite3_step(_ping);
        sqlite3_reset(_ping);
        return rc == SQLITE_ROW ? null : ErrorMessage(_db);
    }

    /// <summary>Decodes the first <paramref name="count"/> columns of a record. False if it is not well formed.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static bool Decode(byte* rec, uint length, int count, Column* cols) {
        if (length == 0) {
            return false;
        }

        uint i = 0;
        ulong headerSize;
        if (rec[0] < 0x80) {
            headerSize = rec[0];
            i = 1;
        } else if (!LongVarint(rec, length, ref i, out headerSize)) {
            return false;
        }

        if (headerSize > length) {
            return false;
        }

        uint header = (uint)headerSize, body = header;
        for (int c = 0; c < count; c++) {
            if (i >= header) {
                return false;
            }

            ulong t;
            if (rec[i] < 0x80) {
                t = rec[i++];
            } else if (!LongVarint(rec, header, ref i, out t)) {
                return false;
            }

            uint n = t switch {
                0 or 8 or 9 or 10 or 11 => 0,
                1 => 1,
                2 => 2,
                3 => 3,
                4 => 4,
                5 => 6,
                6 or 7 => 8,
                _ => (uint)((t - 12) / 2),
            };
            if ((ulong)body + n > length) {
                return false;
            }

            cols[c].Type = t;
            cols[c].Data = rec + body;
            cols[c].Length = (int)n;
            body += n;
        }

        return true;
    }

    private static bool LongVarint(byte* p, uint end, ref uint i, out ulong value) {
        value = 0;
        for (int k = 0; k < 9; k++) {
            if (i >= end) {
                return false;
            }

            byte b = p[i++];
            if (k == 8) {
                value = (value << 8) | b;
                return true;
            }

            value = (value << 7) | (uint)(b & 0x7F);
            if ((b & 0x80) == 0) {
                return true;
            }
        }

        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static bool IsText(in Column c) => c.Type >= 13 && (c.Type & 1) == 1;

    /// <summary>A column as an integer: serial types 1 to 6, 8 and 9.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static bool Int(in Column c, out long value) {
        switch (c.Type) {
            case 8: value = 0; return true;
            case 9: value = 1; return true;
            case >= 1 and <= 6:
                long v = (sbyte)c.Data[0];
                for (int j = 1; j < c.Length; j++) {
                    v = (v << 8) | c.Data[j];
                }

                value = v;
                return true;
            default:
                value = 0;
                return false;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static ReadOnlySpan<byte> Text(nint stmt, int column) {
        byte* p = sqlite3_column_text(stmt, column); // text before bytes, as SQLite's docs ask
        return new ReadOnlySpan<byte>(p, sqlite3_column_bytes(stmt, column));
    }

    private static string ErrorMessage(nint db) => Marshal.PtrToStringUTF8((nint)sqlite3_errmsg(db)) ?? "unknown error";

    private static void Exec(nint db, string sql) {
        byte[] bytes = Encoding.UTF8.GetBytes(sql + "\0");
        fixed (byte* p = bytes) {
            int rc = sqlite3_exec(db, p, 0, 0, null);
            if (rc != SQLITE_OK) {
                throw new InvalidOperationException($"{sql}: {ErrorMessage(db)}");
            }
        }
    }

    private nint Prepare(string sql) {
        byte[] bytes = Encoding.UTF8.GetBytes(sql);
        nint stmt;
        fixed (byte* p = bytes) {
            int rc = sqlite3_prepare_v3(_db, p, bytes.Length, SQLITE_PREPARE_PERSISTENT, &stmt, null);
            if (rc != SQLITE_OK) {
                throw new InvalidOperationException($"{sql}: {ErrorMessage(_db)}");
            }
        }

        return stmt;
    }

    private static void Check(int rc, string what) {
        if (rc != SQLITE_OK) {
            throw new InvalidOperationException($"SQLite {what} failed ({rc})");
        }
    }

    /// <summary>
    /// The checkpoint thread and its own connection. Passive checkpoints copy the WAL and fsync it, and with writes held
    /// and every frame copied, one write of its own starts the WAL again here. That first frame makes SQLite write the WAL
    /// header and fsync the WAL even at synchronous=NORMAL, and that fsync can wait behind the file system's journal: on
    /// the request thread it would stop every request. It runs at nice 19: on one vCPU a checkpoint at normal priority
    /// holds the request thread off the CPU for its whole length, and the request thread's passes are short enough that
    /// the checkpoint still runs between them.
    /// </summary>
    private sealed class Checkpointer {
        private readonly nint _ck;
        private readonly byte[] _restart;

        public Checkpointer(string path) {
            _ck = Open(path);
            Exec(_ck, "PRAGMA journal_mode = WAL");
            Exec(_ck, "PRAGMA synchronous = NORMAL");
            Exec(_ck, "PRAGMA wal_autocheckpoint = 0");
            Exec(_ck, "PRAGMA journal_size_limit = 0"); // this connection usually starts the WAL again, so it does the cut
            Exec(_ck, "PRAGMA busy_timeout = 5000");
            Exec(_ck, "PRAGMA cache_size = 16"); // a checkpoint copies frames straight from the WAL; this connection reads no tables
            long version = 0;
            nint st;
            fixed (byte* q = "PRAGMA user_version"u8) {
                Check(sqlite3_prepare_v3(_ck, q, 19, 0, &st, null), "prepare user_version");
            }

            if (sqlite3_step(st) == SQLITE_ROW) {
                version = sqlite3_column_int64(st, 0);
            }

            sqlite3_finalize(st);
            _restart = Encoding.UTF8.GetBytes($"BEGIN IMMEDIATE; PRAGMA user_version = {version}; COMMIT\0");
        }

        public void Start() => new Thread(Run, 256 * 1024) { IsBackground = true, Name = "checkpoint" }.Start();

        private void Run() {
            if (!int.TryParse(Environment.GetEnvironmentVariable("MB_CK_NICE"), out int nice)) {
                nice = 19;
            }

            setpriority(0, (uint)syscall_fast(SYS_gettid, 0, 0, 0, 0, 0, 0), nice);
            int seen = -1;
            fixed (byte* restart = _restart, rollback = "ROLLBACK\0"u8) {
                while (true) {
                    if (Volatile.Read(ref Wal.HoldWrites) != 0) {
                        Thread.Sleep(1); // the loop waits for this checkpoint
                    } else {
                        Wal.Sleep();
                    }

                    bool held = Volatile.Read(ref Wal.HoldWrites) != 0;
                    int now = Volatile.Read(ref Wal.Commits);
                    // Caught up and the hold has not ended: a checkpoint now would copy the frame that started the WAL
                    // again, and the loop's next write would start it once more.
                    if ((held && Volatile.Read(ref Wal.CaughtUp) != 0) || (now == seen && !held)) {
                        continue;
                    }

                    seen = now;
                    int frames = 0, copied = 0;
                    sqlite3_wal_checkpoint_v2(_ck, null, SQLITE_CHECKPOINT_PASSIVE, &frames, &copied);
                    Volatile.Write(ref Wal.Frames, frames);
                    if (!held || copied < frames) {
                        continue;
                    }

                    if (frames == 0) {
                        Volatile.Write(ref Wal.CaughtUp, 1);
                    } else if (Interlocked.CompareExchange(ref Wal.RestartGate, 1, 0) == 0) {
                        if (Volatile.Read(ref Wal.HoldWrites) != 0) {
                            // If a pass's transaction still reads the old frames, the WAL does not start again: this
                            // only adds a frame, and the WAL goes on growing until the next hold.
                            if (sqlite3_exec(_ck, restart, 0, 0, null) == SQLITE_OK) {
                                Volatile.Write(ref Wal.CaughtUp, 1);
                            } else {
                                sqlite3_exec(_ck, rollback, 0, 0, null);
                            }
                        }

                        Volatile.Write(ref Wal.RestartGate, 0);
                    }
                }
            }
        }
    }
}
