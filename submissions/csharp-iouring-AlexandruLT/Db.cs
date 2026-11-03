using System.Runtime.InteropServices;

namespace Feed;

static partial class Sqlite
{
    const string Lib = "sqlite3";
    public const int Ok = 0, Row = 100, Done = 101;
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial int sqlite3_open_v2(string path, out nint db, int flags, nint vfs);
    [LibraryImport(Lib)] public static partial int sqlite3_close_v2(nint db);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial int sqlite3_exec(nint db, string sql, nint callback, nint arg, nint error);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial int sqlite3_prepare_v3(nint db, string sql, int length, uint flags, out nint stmt, nint tail);
    [LibraryImport(Lib)] public static partial int sqlite3_step(nint stmt);
    [LibraryImport(Lib)] public static partial int sqlite3_reset(nint stmt);
    [LibraryImport(Lib)] public static partial int sqlite3_clear_bindings(nint stmt);
    [LibraryImport(Lib)] public static partial int sqlite3_finalize(nint stmt);
    [LibraryImport(Lib)] public static partial int sqlite3_bind_int64(nint stmt, int index, long value);
    [LibraryImport(Lib)] public static unsafe partial int sqlite3_bind_text(nint stmt, int index, byte* value, int length, nint destructor);
    [LibraryImport(Lib)] public static partial long sqlite3_column_int64(nint stmt, int index);
    [LibraryImport(Lib)] public static partial nint sqlite3_column_text(nint stmt, int index);
    [LibraryImport(Lib)] public static partial int sqlite3_column_bytes(nint stmt, int index);
    [LibraryImport(Lib)] public static partial long sqlite3_last_insert_rowid(nint db);
    [LibraryImport(Lib)] public static partial int sqlite3_changes(nint db);
    [LibraryImport(Lib)] public static partial nint sqlite3_errmsg(nint db);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial int sqlite3_wal_checkpoint_v2(nint db, string name, int mode, out int log, out int checkpointed);
    [LibraryImport(Lib)] public static unsafe partial nint sqlite3_wal_hook(nint db, delegate* unmanaged[Cdecl]<nint, nint, nint, int, int> hook, nint arg);
}

// The only request connection. It is used by the poll thread alone and is the only writer,
// so a read transaction left open between write groups always sees the latest commit.
sealed class Db : IDisposable
{
    const int ReadWrite = 2, NoMutex = 0x8000, Persistent = 1, Transient = -1;
    readonly nint handle;
    readonly Dictionary<string, nint> statements = new();
    GCHandle walHandle;
    int walFrames;
    long walSerial;
    bool readOpen;
    public int WalFrames => Volatile.Read(ref walFrames);
    public long WalSerial => Interlocked.Read(ref walSerial);

    public unsafe Db(string path, int cacheKiB = 16384)
    {
        Check(Sqlite.sqlite3_open_v2(path, out handle, ReadWrite | NoMutex, 0)); // never creates the file
        // WAL with synchronous=NORMAL (rule 6). Checkpoints run on BackgroundCheckpoint's connection.
        Exec($"PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000; PRAGMA cache_size=-{cacheKiB}; PRAGMA wal_autocheckpoint=0;");
        const string select = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id";
        Prepare("feed", select + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
        Prepare("post", select + " WHERE p.id=?1");
        Prepare("health", "SELECT 1");
        Prepare("insert", "INSERT INTO posts(user_id,body) VALUES(?1,?2)");
        Prepare("created", "SELECT id,body,created_at FROM posts WHERE id=?1");
        Prepare("like", "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING");
        Prepare("exists", "SELECT 1 FROM posts WHERE id=?1");
        // Transaction control runs per request or per group, so it is prepared once.
        Prepare("begin", "BEGIN"); Prepare("begin_immediate", "BEGIN IMMEDIATE"); Prepare("commit", "COMMIT");
        Prepare("savepoint", "SAVEPOINT request"); Prepare("release", "RELEASE request"); Prepare("rollback_to", "ROLLBACK TO request");
        walHandle = GCHandle.Alloc(this);
        Sqlite.sqlite3_wal_hook(handle, &WalCommitted, GCHandle.ToIntPtr(walHandle));
    }
    void Check(int rc)
    {
        if (rc is Sqlite.Ok or Sqlite.Row or Sqlite.Done) return;
        throw new InvalidOperationException($"SQLite {rc}: {Marshal.PtrToStringUTF8(Sqlite.sqlite3_errmsg(handle))}");
    }
    void Prepare(string name, string sql)
    {
        Check(Sqlite.sqlite3_prepare_v3(handle, sql, -1, Persistent, out nint stmt, 0));
        statements.Add(name, stmt);
    }
    void Exec(string sql) => Check(Sqlite.sqlite3_exec(handle, sql, 0, 0, 0));
    int Step(nint stmt) { int rc = Sqlite.sqlite3_step(stmt); Check(rc); return rc; }
    void Reset(nint stmt) { Sqlite.sqlite3_reset(stmt); Check(Sqlite.sqlite3_clear_bindings(stmt)); }
    void Bind(nint stmt, int index, long value) => Check(Sqlite.sqlite3_bind_int64(stmt, index, value));
    unsafe void Bind(nint stmt, int index, ReadOnlySpan<byte> value)
    {
        fixed (byte* pointer = value) Check(Sqlite.sqlite3_bind_text(stmt, index, pointer, value.Length, Transient));
    }
    static long Int(nint stmt, int col) => Sqlite.sqlite3_column_int64(stmt, col);
    static unsafe ReadOnlySpan<byte> Text(nint stmt, int col) => new((void*)Sqlite.sqlite3_column_text(stmt, col), Sqlite.sqlite3_column_bytes(stmt, col));
    void Control(string name)
    {
        nint s = statements[name];
        try { Check(Sqlite.sqlite3_step(s)); }
        finally { Sqlite.sqlite3_reset(s); }
    }

    // Reads: one transaction stays open until the next write group (two fewer fcntl calls per read).
    public void EnsureRead() { if (!readOpen) { Control("begin"); readOpen = true; } }
    public void EndRead() { if (readOpen) { readOpen = false; Control("commit"); } }
    // Writes: one group transaction with a savepoint per request.
    public void BeginWrites() => Control("begin_immediate");
    public void CommitWrites() => Control("commit");
    public void RollbackWrites() { try { Exec("ROLLBACK"); } catch { /* keep the original error */ } }
    public void Savepoint() => Control("savepoint");
    public void ReleaseSavepoint() => Control("release");
    public void RollbackToSavepoint() => Control("rollback_to");

    public void Health()
    {
        nint s = statements["health"];
        try { if (Step(s) != Sqlite.Row) throw new InvalidOperationException("SELECT 1 failed"); }
        finally { Reset(s); }
    }
    public bool Read(Output output, long? id)
    {
        nint s = statements[id.HasValue ? "post" : "feed"];
        try
        {
            if (id.HasValue) Bind(s, 1, id.Value);
            int rc = Step(s);
            if (id.HasValue && rc == Sqlite.Done) return false;
            output.Begin(!id.HasValue);
            bool first = true;
            while (rc == Sqlite.Row)
            {
                if (!first) output.Raw(","u8);
                output.Post(Int(s, 0), Text(s, 1), Text(s, 2), Text(s, 3), Int(s, 4));
                first = false; rc = Step(s);
            }
            output.End(!id.HasValue);
            return true;
        }
        finally { Reset(s); }
    }
    // Create and Like run inside a write group; their responses are sent only after COMMIT.
    public void Create(Output output, long user, ReadOnlySpan<byte> body, ReadOnlySpan<byte> author)
    {
        nint s = statements["insert"];
        try { Bind(s, 1, user); Bind(s, 2, body); if (Step(s) != Sqlite.Done) throw new InvalidOperationException("insert failed"); }
        finally { Reset(s); }
        s = statements["created"];
        try
        {
            Bind(s, 1, Sqlite.sqlite3_last_insert_rowid(handle));
            if (Step(s) != Sqlite.Row) throw new InvalidOperationException("created post missing");
            output.Begin(false); output.Post(Int(s, 0), Text(s, 1), Text(s, 2), author, 0); output.End(false);
        }
        finally { Reset(s); }
    }
    public int Like(long user, long post)
    {
        nint s = statements["like"];
        try { Bind(s, 1, user); Bind(s, 2, post); if (Step(s) != Sqlite.Done) throw new InvalidOperationException("like failed"); }
        finally { Reset(s); }
        if (Sqlite.sqlite3_changes(handle) != 0) return 201;
        s = statements["exists"];
        try { Bind(s, 1, post); return Step(s) == Sqlite.Row ? 200 : 404; }
        finally { Reset(s); }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    static int WalCommitted(nint arg, nint db, nint name, int frames)
    {
        // Runs after a physical COMMIT; tells the checkpoint thread how large the WAL is.
        var owner = (Db)GCHandle.FromIntPtr(arg).Target!;
        Volatile.Write(ref owner.walFrames, frames); Interlocked.Increment(ref owner.walSerial);
        return 0;
    }
    public void Dispose()
    {
        foreach (nint s in statements.Values) Sqlite.sqlite3_finalize(s);
        Sqlite.sqlite3_close_v2(handle);
        if (walHandle.IsAllocated) walHandle.Free();
    }
}
