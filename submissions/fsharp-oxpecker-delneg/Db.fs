/// The request connection: every read and write runs on it under `gate`, start to finish. Writes are autocommit,
/// so a 201 goes out only after its COMMIT, which is durable in the WAL (synchronous=NORMAL).
/// The checkpoint connection belongs to one background thread. It copies the WAL into the database file and
/// runs the fsyncs that this needs, so a slow disk does not stop the request thread.
/// SQLite is built with SQLITE_THREADSAFE=2: a connection is never used by two threads at once.
module Db

open System
open System.Runtime.InteropServices
open System.Text
open System.Threading
open System.Threading.Tasks

let gate = obj ()
let mutable private db = 0n
let mutable feed = 0n
let mutable post = 0n
let mutable insertPost = 0n
let mutable insertLike = 0n
let mutable postExists = 0n
let mutable ping = 0n

let failure () =
    InvalidOperationException(Marshal.PtrToStringUTF8(Sqlite.errmsg db) |> Option.ofObj |> Option.defaultValue "sqlite error")

let private check rc =
    if rc <> Sqlite.Ok then
        raise (failure ())

let private exec (sql: string) =
    let p = Marshal.StringToCoTaskMemUTF8 sql
    try check (Sqlite.exec(db, p, 0n, 0n, 0n)) finally Marshal.FreeCoTaskMem p

let private prepare (sql: string) =
    let bytes = Encoding.UTF8.GetBytes sql
    let h = GCHandle.Alloc(bytes, GCHandleType.Pinned)
    try
        let mutable stmt = 0n
        // SQLITE_PREPARE_PERSISTENT: the statement lives for the whole process.
        check (Sqlite.prepare_v3(db, h.AddrOfPinnedObject(), bytes.Length, 1u, &stmt, 0n))
        stmt
    finally
        h.Free()

/// Steps once: true on a row, false when the statement is done, and throws on anything else.
let stepRow (stmt: nativeint) =
    match Sqlite.step stmt with
    | Sqlite.Row -> true
    | Sqlite.Done -> false
    | _ -> raise (failure ())

let changes () = Sqlite.changes db

/// Writes since the start. The checkpoint thread runs only when it has changed.
let mutable writes = 0

/// WAL size, in frames, at which the checkpoint thread catches up (see below). 65,536 frames
/// of 4 KB are 256 MB.
let private catchUpFrames = 65536

/// Held by every write, and by the checkpoint thread while it catches up (see below). Reads never take it.
let private writeGate = new SemaphoreSlim(1, 1)

/// Runs a write on the request thread. Normally the gate is free and it runs at once. While the checkpoint thread
/// catches up, the write waits without blocking the thread, so reads go on.
let runWrite (f: unit -> 'T) : ValueTask<'T> =
    if writeGate.Wait 0 then
        try ValueTask<'T>(f ()) finally writeGate.Release() |> ignore
    else
        ValueTask<'T>(
            task {
                do! writeGate.WaitAsync()
                try
                    return f ()
                finally
                    writeGate.Release() |> ignore
            }
        )

let private startCheckpointer (path: string) =
    let p = Marshal.StringToCoTaskMemUTF8 path
    let mutable cdb = 0n
    try check (Sqlite.open_v2(p, &cdb, 0x02, 0n)) finally Marshal.FreeCoTaskMem p
    let cdb = cdb
    // Read the database once, so this connection is in WAL mode. Until then, a checkpoint here does nothing.
    for sql in [ "PRAGMA journal_mode = WAL"; "PRAGMA busy_timeout = 5000" ] do
        let q = Marshal.StringToCoTaskMemUTF8 sql
        try check (Sqlite.exec(cdb, q, 0n, 0n, 0n)) finally Marshal.FreeCoTaskMem q
    /// One PASSIVE checkpoint: copy the WAL into the database file and fsync, without blocking reads or writes.
    /// Returns the WAL size in frames.
    let passive () =
        let mutable logFrames = 0
        let mutable copied = 0
        Sqlite.wal_checkpoint_v2(cdb, 0n, 0, &logFrames, &copied) |> ignore
        logFrames
    let loop () =
        let mutable seen = 0
        while true do
            Thread.Sleep 100
            let now = Volatile.Read &writes
            if now <> seen then
                seen <- now
                let logFrames = passive ()
                // The request connection starts the WAL again from the beginning only when a write finds every
                // frame already copied. Under steady writes that may not happen, so the WAL would grow without limit.
                // At 256 MB, copy the last frames while no write runs (reads go on). Then the next write restarts the WAL.
                if logFrames >= catchUpFrames then
                    writeGate.Wait()
                    try passive () |> ignore finally writeGate.Release() |> ignore
    Thread(loop, IsBackground = true, Name = "sqlite-checkpoint").Start()

let openDb (path: string) =
    check (Sqlite.initialize ()) // required: SQLite is built with SQLITE_OMIT_AUTOINIT
    let p = Marshal.StringToCoTaskMemUTF8 path
    try
        let mutable h = 0n
        check (Sqlite.open_v2(p, &h, 0x02 (* SQLITE_OPEN_READWRITE *), 0n))
        db <- h
    finally
        Marshal.FreeCoTaskMem p
    exec "PRAGMA journal_mode = WAL"
    exec "PRAGMA synchronous = NORMAL" // rule 6; autocommit, so each write commits before its response
    exec "PRAGMA wal_autocheckpoint = 0" // the checkpoint thread does it (below)
    exec "PRAGMA busy_timeout = 5000"
    exec "PRAGMA mmap_size = 1073741824"
    exec "PRAGMA cache_size = -32768"
    let postSelect =
        "SELECT p.id, p.body, p.created_at, u.username, \
                (SELECT count(*) FROM likes l WHERE l.post_id = p.id) \
           FROM posts p JOIN users u ON u.id = p.user_id"
    feed <- prepare (postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    post <- prepare (postSelect + " WHERE p.id = ?1")
    insertPost <- prepare "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at"
    // Inserts only if the post exists. No change means "already liked" or "no such post".
    insertLike <-
        prepare
            "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) \
             ON CONFLICT (user_id, post_id) DO NOTHING"
    postExists <- prepare "SELECT 1 FROM posts WHERE id = ?1"
    ping <- prepare "SELECT 1"
    startCheckpointer path
