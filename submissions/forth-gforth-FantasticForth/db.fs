\ ============================================================================
\ db.fs - SQLite layer: open + durability/perf pragmas (WAL, synchronous=NORMAL,
\ EXCLUSIVE locking, no mmap, 64 MiB cache, autocheckpoint off), prepared
\ statements, bind/step/column accessors, off-path WAL checkpointing, and the
\ group-commit transaction words. Depends on ffi.fs being loaded first.
\ ============================================================================

\ sqlite constants
0   constant SQLITE_OK
100 constant SQLITE_ROW
101 constant SQLITE_DONE
2   constant SQLITE_OPEN_READWRITE
-1  constant SQLITE_TRANSIENT   \ ((void*)-1): tells sqlite to copy the bound text

\ global db handle
variable dbh   0 dbh !

\ --- helpers -------------------------------------------------------------

\ turn a Forth string into a NUL-terminated C string in a scratch buffer.
\ returns ( zaddr ). Not reentrant; copy before the next call if needed.
create zbuf 4096 allot
: >z ( addr u -- zaddr )
  dup 4095 > abort" >z: string too long"
  zbuf swap 2dup + >r  ( addr zbuf u ; R: end )
  move 0 r> c! zbuf ;

: db-errmsg ( -- addr u ) dbh @ sqlite3_errmsg
  dup 0 begin 2dup + c@ while 1+ repeat nip ;

\ --- open ----------------------------------------------------------------
\ open the database read/write (pragmas are applied separately by db-configure).
: db-open ( addr u -- )
  >z                                   ( zpath )
  dbh                                  ( zpath &dbh )
  SQLITE_OPEN_READWRITE                ( zpath &dbh flags )
  0                                    ( zpath &dbh flags zvfs=NULL )
  sqlite3_open_v2
  SQLITE_OK <> abort" sqlite open failed" ;

\ run a pragma / statement with no result rows (exec).
: db-exec ( addr u -- )
  >z dbh @ swap                        ( dbh zsql )
  0 0 0                                ( dbh zsql cb arg errmsg )
  sqlite3_exec
  SQLITE_OK <> abort" sqlite exec failed" ;

: db-configure ( -- )
  s" PRAGMA busy_timeout=5000;"        db-exec
  \ EXCLUSIVE locking: single-connection server, so we can hold the database
  \ lock for the process lifetime. Avoids per-transaction lock/unlock syscalls
  \ and lets WAL skip some shared-memory coordination. Set before journal_mode.
  s" PRAGMA locking_mode=EXCLUSIVE;"   db-exec
  s" PRAGMA journal_mode=WAL;"         db-exec
  s" PRAGMA synchronous=NORMAL;"       db-exec
  \ mmap is OFF on purpose. In WAL mode a non-zero mmap_size routes page fetches
  \ through getPageMMap, which does NOT populate SQLite's pcache1 and forces a
  \ WAL hash-segment probe on every b-tree lookup. Profiling this server showed
  \ mmap made /feed ~2 us/request SLOWER than mmap=0 + a real page cache, so we
  \ read through pcache1 + the OS page cache instead.
  s" PRAGMA mmap_size=0;"              db-exec
  \ SQLite page cache: negative = KiB. -65536 = 64 MiB of hot b-tree pages.
  \ 64 MiB performed the same as 256 MiB in profiling (the OS page cache holds
  \ the rest of the 238 MB db), so we take the smaller footprint on the 2 GB box.
  s" PRAGMA cache_size=-65536;"        db-exec
  \ Temp b-trees / sorters live in memory, never spill to a temp file.
  s" PRAGMA temp_store=MEMORY;"        db-exec
  \ Turn OFF automatic WAL checkpointing: under sustained writes the default
  \ (1000 frames, on the writing thread) lets the WAL grow and stalls reads
  \ (every page lookup probes WAL hash segments). We instead checkpoint OFF the
  \ request path from the event loop, gated by a committed-write counter (see
  \ WAL-CKPT-WRITES in epoll.fs), and TRUNCATE to keep the WAL file small.
  s" PRAGMA wal_autocheckpoint=0;"     db-exec
  \ Cap the WAL file size so a completed checkpoint truncates it back down.
  s" PRAGMA journal_size_limit=67108864;" db-exec
  s" PRAGMA foreign_keys=ON;"          db-exec ;

\ --- WAL checkpointing (called off the request path) ---------------------
\ Only TRUNCATE is used (copy committed frames back + shrink the WAL file).
3 constant SQLITE_CHECKPOINT_TRUNCATE

\ out-param cells for pnLog (WAL size in frames) and pnCkpt (frames checkpointed).
\ Required as valid pointers for the call even though we don't read them in the
\ hot path (zeroed so a no-op checkpoint doesn't leave stale values).
variable ckpt-nlog
variable ckpt-nckpt

\ run a checkpoint in the given mode on the main database. Returns rc.
\ pnLog/pnCkpt are 32-bit ints; zero the cells first (SQLite leaves them
\ untouched on SQLITE_LOCKED/BUSY) so a later l@ reads 0 rather than stale bits.
: db-checkpoint ( mode -- rc )
  0 ckpt-nlog !  0 ckpt-nckpt !
  >r
  dbh @                                ( dbh )
  0                                    ( dbh zDb=NULL=main )
  r>                                   ( dbh NULL mode )
  ckpt-nlog                            ( ... pnLog )
  ckpt-nckpt                           ( ... pnCkpt )
  sqlite3_wal_checkpoint_v2 ;

\ --- prepared statements -------------------------------------------------
\ prepare and return a stmt handle. sqlite3_prepare_v2 copies the SQL text, so
\ the scratch zsql buffer need not outlive the call.
variable stmt-slot
: db-prepare ( addr u -- stmt )
  >z                                   ( zsql )
  dbh @ swap                           ( dbh zsql )
  -1                                   ( dbh zsql nbyte=-1 )
  stmt-slot                            ( dbh zsql -1 &stmt )
  0                                    ( dbh zsql -1 &stmt pztail=NULL )
  sqlite3_prepare_v2
  SQLITE_OK <> abort" sqlite prepare failed"
  stmt-slot @ ;

: st-reset        ( stmt -- ) dup sqlite3_reset drop sqlite3_clear_bindings drop ;
: st-step         ( stmt -- rc ) sqlite3_step ;
: st-bind-int     ( stmt col n -- ) sqlite3_bind_int64 drop ;
: st-bind-text    ( stmt col addr u -- )
  SQLITE_TRANSIENT sqlite3_bind_text drop ;
: st-col-int      ( stmt col -- n ) sqlite3_column_int64 ;
: st-col-text     ( stmt col -- addr u )
  2dup sqlite3_column_bytes  -rot  sqlite3_column_text swap ;
: st-finalize     ( stmt -- ) sqlite3_finalize drop ;

\ --- group commit --------------------------------------------------------
\ All writes produced during one epoll_wait batch share one transaction. The
\ event loop opens it lazily on the first write of the pass (db-begin-write)
\ and commits it once at the end of the pass (db-commit-batch), BEFORE any of
\ that batch's responses are sent (durability, rule 6). batch-writes counts
\ writes in the current pass so the loop knows whether to commit/checkpoint.
variable txn-open?     0 txn-open? !
variable batch-writes  0 batch-writes !

\ Prepared transaction-control statements. sqlite3_exec("BEGIN"/"COMMIT") would
\ re-parse the SQL on every write batch; preparing once and stepping is ~2x
\ cheaper (profiled: 1.85 us -> 0.86 us per BEGIN+COMMIT). Prepared by
\ db-prepare-txn once the db is open (called from db-configure).
variable stmt-begin
variable stmt-commit
variable stmt-rollback
: db-prepare-txn ( -- )
  s" BEGIN IMMEDIATE" db-prepare stmt-begin !
  s" COMMIT"          db-prepare stmt-commit !
  s" ROLLBACK"        db-prepare stmt-rollback ! ;

\ step a no-result statement once, reset it, and return the step rc.
\ SQLITE_DONE (101) means success for BEGIN/COMMIT/ROLLBACK.
: st-run ( stmt -- rc )
  dup sqlite3_reset drop
  dup sqlite3_step   ( stmt rc )
  swap sqlite3_reset drop ;

: db-begin-write ( -- )
  txn-open? @ if exit then
  stmt-begin @ st-run drop
  1 txn-open? ! ;

\ called by a write handler after it has stepped its INSERT. Ensures a txn is
\ open and records that the batch contains a write.
: db-note-write ( -- )
  db-begin-write
  1 batch-writes +! ;

: db-rollback ( -- )
  stmt-rollback @ st-run drop ;

\ commit the batch transaction if one is open. Returns rc (SQLITE_DONE = ok).
\ On failure the transaction is rolled back; the caller converts the batch's
\ pending write-responses to 500 before sending them.
: db-commit-batch ( -- rc )
  txn-open? @ 0= if SQLITE_DONE exit then
  0 txn-open? !
  stmt-commit @ st-run              ( rc )
  dup SQLITE_DONE <> if db-rollback then ;
