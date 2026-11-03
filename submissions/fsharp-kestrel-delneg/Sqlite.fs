/// The SQLite C API, bound directly with blittable types: raw handles, no marshalling.
/// The column getters are tiny and never block or call back into .NET, so they skip the GC transition. So do step
/// and reset: SQLite never calls back into .NET here (no hooks or functions of ours; busy_timeout is SQLite's own native
/// sleep), and only the loop thread allocates (the checkpoint thread's loop does not), so a step that waits (a lock,
/// the fsync that starts the WAL again) holds up no GC.
module Sqlite

#nowarn "9"

open System
open System.Runtime.InteropServices

[<Literal>]
let private Lib = "sqlite3"

[<Literal>]
let Ok = 0

[<Literal>]
let Row = 100

[<Literal>]
let Done = 101

/// SQLITE_TRANSIENT: SQLite copies the bound value.
let Transient = nativeint -1

[<DllImport(Lib, EntryPoint = "sqlite3_initialize")>]
extern int initialize()

[<DllImport(Lib, EntryPoint = "sqlite3_open_v2")>]
extern int open_v2(nativeint filename, nativeint& db, int flags, nativeint vfs)

[<DllImport(Lib, EntryPoint = "sqlite3_exec")>]
extern int exec(nativeint db, nativeint sql, nativeint callback, nativeint arg, nativeint errmsg)

[<DllImport(Lib, EntryPoint = "sqlite3_prepare_v3")>]
extern int prepare_v3(nativeint db, nativeint sql, int length, uint32 flags, nativeint& stmt, nativeint tail)

[<DllImport(Lib, EntryPoint = "sqlite3_step"); SuppressGCTransition>]
extern int step(nativeint stmt)

[<DllImport(Lib, EntryPoint = "sqlite3_reset"); SuppressGCTransition>]
extern int reset(nativeint stmt)

[<DllImport(Lib, EntryPoint = "sqlite3_finalize")>]
extern int finalize(nativeint stmt)

[<DllImport(Lib, EntryPoint = "sqlite3_bind_text")>]
extern int bind_text(nativeint stmt, int index, nativeint text, int length, nativeint destructor)

[<DllImport(Lib, EntryPoint = "sqlite3_errmsg")>]
extern nativeint errmsg(nativeint db)

[<DllImport(Lib, EntryPoint = "sqlite3_bind_int64"); SuppressGCTransition>]
extern int bind_int64(nativeint stmt, int index, int64 value)

[<DllImport(Lib, EntryPoint = "sqlite3_column_int64"); SuppressGCTransition>]
extern int64 column_int64(nativeint stmt, int column)

[<DllImport(Lib, EntryPoint = "sqlite3_column_text"); SuppressGCTransition>]
extern nativeint column_text(nativeint stmt, int column)

[<DllImport(Lib, EntryPoint = "sqlite3_column_bytes"); SuppressGCTransition>]
extern int column_bytes(nativeint stmt, int column)

[<DllImport(Lib, EntryPoint = "sqlite3_changes"); SuppressGCTransition>]
extern int changes(nativeint db)

/// A TEXT column as UTF-8 bytes, valid until the statement is stepped or reset. Text before bytes, per the docs.
let text (stmt: nativeint) (column: int) : ReadOnlySpan<byte> =
    let p = column_text(stmt, column)
    ReadOnlySpan<byte>(p.ToPointer(), column_bytes(stmt, column))

/// sqlite3_wal_checkpoint_v2(db, zDb, eMode, pnLog, pnCkpt). eMode 0 = SQLITE_CHECKPOINT_PASSIVE.
[<DllImport(Lib, EntryPoint = "sqlite3_wal_checkpoint_v2")>]
extern int wal_checkpoint_v2(nativeint db, nativeint dbName, int mode, int& logFrames, int& checkpointed)
