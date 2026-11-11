/// SQLite 3.54.1, the stock amalgamation compiled by build.sh into out/libsqlite3td.so, called through P/Invoke.
/// Two kinds of calls:
/// - the public C API (open, prepare, step, bind, column, WAL checkpoints);
/// - SQLite's own B-tree cursor functions (sqlite3Btree*), which the amalgamation declares SQLITE_PRIVATE. build.sh
///   compiles it with -DSQLITE_PRIVATE= (the amalgamation's own switch for that macro; the source is not edited) so
///   they are exported too. Reading /feed and /posts/:id through them is allowed (LEADERBOARD.md, Arjay's rulings on
///   SQLite, 31 Oct). The server never writes SQLite's private state: it opens, moves and closes cursors, reads the
///   payload SQLite returns, and fills only search keys of its own (UnpackedRecord + Mem, laid out below) for
///   sqlite3BtreeIndexMoveto.
/// Calls that are short and never block are marked SuppressGCTransition (the loop thread is the only caller).
module Sqlite

#nowarn "9"
#nowarn "51"

open System
open System.Runtime.InteropServices
open Microsoft.FSharp.NativeInterop

[<Literal>]
let L = "libsqlite3td"

let OK = 0
let ROW = 100
let DONE = 101
let OPEN_READWRITE = 0x2
let OPEN_NOMUTEX = 0x8000
let PREPARE_PERSISTENT = 1u
let CHECKPOINT_PASSIVE = 0
let DBSTATUS_CACHE_USED = 1
let MEM_Int = 0x0004us
let BTREE_SCHEMA_VERSION = 1

// ---------- public API ----------

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_open_v2(byte[] filename, nativeint& db, int flags, byte[] vfs)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_initialize()

[<DllImport(L, ExactSpelling = true)>]
extern nativeint sqlite3_libversion()

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_close_v2(nativeint db)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_exec(nativeint db, byte[] sql, nativeint cb, nativeint arg, nativeint err)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_prepare_v3(nativeint db, byte[] sql, int n, uint32 flags, nativeint& stmt, nativeint tail)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3_step(nativeint stmt)

/// sqlite3_step for statements that may commit: a commit calls the WAL hook, a callback into managed code, which a
/// call without a GC transition must never make
[<DllImport(L, EntryPoint = "sqlite3_step", ExactSpelling = true)>]
extern int sqlite3_step_commit(nativeint stmt)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3_reset(nativeint stmt)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3_bind_int64(nativeint stmt, int i, int64 v)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3_bind_text(nativeint stmt, int i, nativeint p, int n, nativeint destructor)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int64 sqlite3_column_int64(nativeint stmt, int i)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern nativeint sqlite3_column_text(nativeint stmt, int i)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3_column_bytes(nativeint stmt, int i)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3_changes(nativeint db)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int64 sqlite3_last_insert_rowid(nativeint db)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3_get_autocommit(nativeint db)

[<DllImport(L, ExactSpelling = true)>]
extern nativeint sqlite3_errmsg(nativeint db)

[<DllImport(L, ExactSpelling = true)>]
extern nativeint sqlite3_wal_hook(nativeint db, nativeint callback, nativeint arg)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_wal_autocheckpoint(nativeint db, int n)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_wal_checkpoint_v2(nativeint db, byte[] name, int mode, int& log, int& ckpt)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_file_control(nativeint db, byte[] name, int op, int& arg)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_busy_timeout(nativeint db, int ms)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_db_status(nativeint db, int op, int& cur, int& hi, int reset)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_threadsafe()

// ---------- SQLite's B-tree layer (SQLITE_PRIVATE in the amalgamation) ----------

[<DllImport(L, ExactSpelling = true)>]
extern nativeint sqlite3DbNameToBtree(nativeint db, byte[] name)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3BtreeBeginTrans(nativeint bt, int wrflag, nativeint schemaVersion)

[<DllImport(L, EntryPoint = "sqlite3BtreeGetMeta", ExactSpelling = true); SuppressGCTransition>]
extern void btGetMeta(nativeint bt, int idx, nativeint value)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3BtreeCursorSize()

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern void sqlite3BtreeCursorZero(nativeint cur)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3BtreeCursor(nativeint bt, uint32 root, int wrflag, nativeint keyInfo, nativeint cur)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3BtreeCloseCursor(nativeint cur)

[<DllImport(L, EntryPoint = "sqlite3BtreeFirst", ExactSpelling = true); SuppressGCTransition>]
extern int btFirst(nativeint cur, nativeint res)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3BtreeNext(nativeint cur, int flags)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern int sqlite3BtreeEof(nativeint cur)

[<DllImport(L, EntryPoint = "sqlite3BtreeTableMoveto", ExactSpelling = true); SuppressGCTransition>]
extern int btTableMoveto(nativeint cur, int64 key, int bias, nativeint res)

[<DllImport(L, EntryPoint = "sqlite3BtreeIndexMoveto", ExactSpelling = true); SuppressGCTransition>]
extern int btIndexMoveto(nativeint cur, nativeint unpacked, nativeint res)

[<DllImport(L, EntryPoint = "sqlite3BtreePayloadFetch", ExactSpelling = true); SuppressGCTransition>]
extern nativeint btPayloadFetch(nativeint cur, nativeint amt)

[<DllImport(L, ExactSpelling = true); SuppressGCTransition>]
extern uint32 sqlite3BtreePayloadSize(nativeint cur)

[<DllImport(L, ExactSpelling = true)>]
extern nativeint sqlite3KeyInfoAlloc(nativeint db, int n, int x)

// The calls above with an out parameter take it as a raw pointer: a byref (`int&`) parameter makes the runtime call
// through a marshalling stub, where a call with only blittable arguments is emitted inline. These keep the byref
// signatures for the callers (the out values are always stack locals).
let inline sqlite3BtreeGetMeta (bt: nativeint, idx: int, value: byref<uint32>) = btGetMeta (bt, idx, NativePtr.toNativeInt &&value)
let inline sqlite3BtreeFirst (cur: nativeint, res: byref<int>) = btFirst (cur, NativePtr.toNativeInt &&res)
let inline sqlite3BtreeTableMoveto (cur: nativeint, key: int64, bias: int, res: byref<int>) = btTableMoveto (cur, key, bias, NativePtr.toNativeInt &&res)
let inline sqlite3BtreeIndexMoveto (cur: nativeint, unpacked: nativeint, res: byref<int>) = btIndexMoveto (cur, unpacked, NativePtr.toNativeInt &&res)
let inline sqlite3BtreePayloadFetch (cur: nativeint, amt: byref<uint32>) = btPayloadFetch (cur, NativePtr.toNativeInt &&amt)

/// SQLite's Mem (sqlite3_value) and UnpackedRecord in 3.54.1 without SQLITE_DEBUG (x86-64): only the fields a search
/// key needs. Mem: u.i @0, flags @20, size 56. UnpackedRecord: pKeyInfo @0, aMem @8, nField @28, default_rc @30, size 40.
[<Literal>]
let MemSize = 56
[<Literal>]
let UnpackedSize = 40

/// a NUL-terminated UTF-8 copy of s, for the calls that take a C string
let z (s: string) =
    let b = Text.Encoding.UTF8.GetBytes s
    Array.append b [| 0uy |]

let errmsg (db: nativeint) = Marshal.PtrToStringUTF8(sqlite3_errmsg db)
