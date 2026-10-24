/// The request connection: every read and write runs on it under `gate` (or on the one loop thread), start to finish.
/// A 201 goes out only after its COMMIT, which is durable in the WAL (synchronous=NORMAL).
/// Two modes, chosen by the transport before the first request (`start`):
/// - exclusive (epoll transport, Kestrel sockets): the connection holds the database file lock for its whole life
///   (locking_mode=EXCLUSIVE) and SQLite's automatic checkpoint runs inside the commit that fills the WAL;
/// - background checkpoints (io_uring transport): normal locking, so a second connection on its own thread can run
///   the PASSIVE checkpoints and their fsyncs; the loop thread never waits for the disk. Each loop pass runs in one
///   transaction (reads and writes), so the WAL locks are taken once per pass, not per statement.
/// SQLite is built with SQLITE_THREADSAFE=2: a connection is never used by two threads at once.
module Db

open System
open System.Runtime.InteropServices
open System.Text
open System.Threading
open System.Threading.Tasks

let gate = obj ()

/// True when one thread runs every request (the epoll transport has one loop thread). Then `serialized` needs
/// no lock. Set once at start, before the first request.
let mutable singleThread = false

/// Runs `f` with the connection to itself: under `gate`, or directly when one thread runs every request.
let inline serialized ([<InlineIfLambda>] f: unit -> 'T) : 'T =
    if singleThread then f () else lock gate f
let mutable private db = 0n
let mutable feed = 0n
let mutable feedIds = 0n
let mutable feedRange = 0n
let mutable likeRange = 0n
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

/// Writes since the start (diagnostics only).
let mutable writes = 0

/// Group commit (epoll transport only). The first write of one epoll wake-up opens a transaction; the loop commits
/// it after the last request of that wake-up, and only then sends the responses of that wake-up. So each response
/// still goes out only after the COMMIT that holds its write.
let mutable batching = false
let mutable inTxn = false
let mutable private beginStmt = 0n
let mutable private commitStmt = 0n
let mutable private rollbackStmt = 0n

/// True in background-checkpoint mode (see the top of this file).
let mutable bgCheckpoint = false

/// Call before each write statement. Opens the pass transaction (group commit) if none is open.
let beginWrite () =
    if batching && not inTxn then
        if Sqlite.step beginStmt <> Sqlite.Done then
            Sqlite.reset beginStmt |> ignore
            raise (failure ())
        Sqlite.reset beginStmt |> ignore
        inTxn <- true

/// Call before each read statement: in background-checkpoint mode the reads of a pass share its transaction.
let inline beginRead () = if bgCheckpoint then beginWrite ()

/// Successful commits since the start (read by the checkpoint thread).
let mutable commits = 0

/// Commits the open transaction. False when the commit failed: then the transaction is rolled back.
let commit () : bool =
    inTxn <- false
    let ok = Sqlite.step commitStmt = Sqlite.Done
    if ok then Volatile.Write(&commits, commits + 1)
    Sqlite.reset commitStmt |> ignore
    if not ok then
        Sqlite.step rollbackStmt |> ignore
        Sqlite.reset rollbackStmt |> ignore
    ok

/// Runs a write on the request thread. SQLite's automatic checkpoint runs inside the commit that fills the WAL
/// to 1,000 frames, so a write never waits for another thread.
let runWrite (f: unit -> 'T) : ValueTask<'T> = ValueTask<'T>(f ())

// ---- WAL size bound (background-checkpoint mode) -------------------------------------------------------------------
// A PASSIVE checkpoint never blocks writes, so under steady writes it never finds every frame copied, and the request
// connection never starts the WAL again from the beginning: the WAL would grow without limit. So at `holdFrames`
// the loop holds writes (reads go on): the checkpoint thread then copies the last frames, and the first write after
// the hold starts the WAL again (SQLite does this when a transaction starts with every frame copied).

/// WAL size in frames, and frames copied, after the last checkpoint (written by the checkpoint thread).
let mutable walFrames = 0
let mutable walCopied = 0
/// Set by the loop: hold new writes until the checkpoint thread has copied every frame.
let mutable holdWrites = false
/// Set by the checkpoint thread when, with writes held, every frame is copied.
let mutable caughtUp = false
/// FS3_HOLD_FRAMES overrides it (tests only: a small value makes holds frequent).
let holdFrames =
    match Int32.TryParse(Environment.GetEnvironmentVariable "FS3_HOLD_FRAMES") with
    | true, n when n > 0 -> n
    | _ -> 4096

let mutable private path = ""

let private startCheckpointer () =
    let p = Marshal.StringToCoTaskMemUTF8 path
    let mutable cdb = 0n
    try check (Sqlite.open_v2(p, &cdb, 0x02, 0n)) finally Marshal.FreeCoTaskMem p
    let cdb = cdb
    // Read the database once, so this connection is in WAL mode. Until then, a checkpoint here does nothing.
    for sql in [ "PRAGMA journal_mode = WAL"; "PRAGMA busy_timeout = 5000" ] do
        let q = Marshal.StringToCoTaskMemUTF8 sql
        try check (Sqlite.exec(cdb, q, 0n, 0n, 0n)) finally Marshal.FreeCoTaskMem q
    let loop () =
        let mutable seen = -1
        while true do
            // While writes are held, go on at once: the loop waits for this checkpoint.
            if Volatile.Read &holdWrites then Thread.Sleep 1 else Thread.Sleep 50
            let held = Volatile.Read &holdWrites
            let now = Volatile.Read &commits
            if now <> seen || held then
                seen <- now
                let mutable logFrames = 0
                let mutable copied = 0
                // PASSIVE: copies the WAL into the database file and fsyncs both, without blocking reads or writes.
                Sqlite.wal_checkpoint_v2(cdb, 0n, 0, &logFrames, &copied) |> ignore
                Volatile.Write(&walFrames, logFrames)
                Volatile.Write(&walCopied, copied)
                if held && copied >= logFrames then Volatile.Write(&caughtUp, true)
    Thread(loop, IsBackground = true, Name = "sqlite-checkpoint").Start()

let openDb (p: string) =
    check (Sqlite.initialize ()) // required: SQLite is built with SQLITE_OMIT_AUTOINIT
    path <- p
    let p = Marshal.StringToCoTaskMemUTF8 path
    try
        let mutable h = 0n
        check (Sqlite.open_v2(p, &h, 0x02 (* SQLITE_OPEN_READWRITE *), 0n))
        db <- h
    finally
        Marshal.FreeCoTaskMem p

let mutable private started = false

/// Sets the mode and prepares the statements. Called once, by the transport, before the first request.
let start (background: bool) =
  if not started then
    started <- true
    bgCheckpoint <- background
    if background then
        exec "PRAGMA journal_mode = WAL"
        exec "PRAGMA wal_autocheckpoint = 0" // the checkpoint thread does it
        exec "PRAGMA journal_size_limit = 67108864" // a WAL that grew past 64 MB shrinks when it starts again
    else
        // One process, one connection: take the file lock once, not around every statement. Set before WAL is first
        // used, so the WAL index lives in memory. SQLite's automatic checkpoint (1,000 frames) runs on this connection.
        exec "PRAGMA locking_mode = EXCLUSIVE"
        exec "PRAGMA journal_mode = WAL"
    exec "PRAGMA synchronous = NORMAL" // rule 6
    exec "PRAGMA busy_timeout = 5000"
    // No mmap: a page that stays in SQLite's page cache is found with one hash lookup and keeps its parsed b-tree
    // header. A page read through mmap is looked up in the WAL index and parsed again on every fetch.
    exec "PRAGMA mmap_size = 0"
    exec "PRAGMA cache_size = -32768"
    // Statement journals in memory: inside a group-commit transaction, SQLite keeps one for each write statement.
    // On disk it costs one more file write for each changed page.
    exec "PRAGMA temp_store = MEMORY"
    let postSelect =
        "SELECT p.id, p.body, p.created_at, u.username, \
                (SELECT count(*) FROM likes l WHERE l.post_id = p.id) \
           FROM posts p JOIN users u ON u.id = p.user_id"
    feed <- prepare (postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    post <- prepare (postSelect + " WHERE p.id = ?1")
    feedIds <- prepare "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20"
    feedRange <-
        prepare
            "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id \
             WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC"
    likeRange <- prepare "SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id"
    insertPost <- prepare "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at"
    // Inserts only if the post exists. No change means "already liked" or "no such post".
    insertLike <-
        prepare
            "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) \
             ON CONFLICT (user_id, post_id) DO NOTHING"
    postExists <- prepare "SELECT 1 FROM posts WHERE id = ?1"
    ping <- prepare "SELECT 1"
    beginStmt <- prepare "BEGIN"
    commitStmt <- prepare "COMMIT"
    rollbackStmt <- prepare "ROLLBACK"
    if background then startCheckpointer ()
