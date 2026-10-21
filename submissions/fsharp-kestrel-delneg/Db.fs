/// The request connection: every read and write runs on it under `gate` (or on the one loop thread), start to finish. Writes are autocommit,
/// so a 201 goes out only after its COMMIT, which is durable in the WAL (synchronous=NORMAL).
/// The connection holds the database file lock for its whole life (locking_mode=EXCLUSIVE).
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

/// Call before each write statement.
let beginWrite () =
    if batching && not inTxn then
        if Sqlite.step beginStmt <> Sqlite.Done then
            Sqlite.reset beginStmt |> ignore
            raise (failure ())
        Sqlite.reset beginStmt |> ignore
        inTxn <- true

/// Commits the open transaction. False when the commit failed: then the transaction is rolled back.
let commit () : bool =
    inTxn <- false
    let ok = Sqlite.step commitStmt = Sqlite.Done
    Sqlite.reset commitStmt |> ignore
    if not ok then
        Sqlite.step rollbackStmt |> ignore
        Sqlite.reset rollbackStmt |> ignore
    ok

/// Runs a write on the request thread. SQLite's automatic checkpoint runs inside the commit that fills the WAL
/// to 1,000 frames, so a write never waits for another thread.
let runWrite (f: unit -> 'T) : ValueTask<'T> = ValueTask<'T>(f ())

let openDb (path: string) =
    check (Sqlite.initialize ()) // required: SQLite is built with SQLITE_OMIT_AUTOINIT
    let p = Marshal.StringToCoTaskMemUTF8 path
    try
        let mutable h = 0n
        check (Sqlite.open_v2(p, &h, 0x02 (* SQLITE_OPEN_READWRITE *), 0n))
        db <- h
    finally
        Marshal.FreeCoTaskMem p
    // One process, one connection: take the file lock once, not around every statement. Set before WAL is first
    // used, so the WAL index lives in memory. SQLite's automatic checkpoint (1,000 frames) runs on this connection.
    exec "PRAGMA locking_mode = EXCLUSIVE"
    exec "PRAGMA journal_mode = WAL"
    exec "PRAGMA synchronous = NORMAL" // rule 6; autocommit, so each write commits before its response
    exec "PRAGMA busy_timeout = 5000"
    // No mmap: a page that stays in SQLite's page cache is found with one hash lookup and keeps its parsed b-tree
    // header. A page read through mmap is looked up in the WAL index and parsed again on every fetch.
    exec "PRAGMA mmap_size = 0"
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
    beginStmt <- prepare "BEGIN"
    commitStmt <- prepare "COMMIT"
    rollbackStmt <- prepare "ROLLBACK"
