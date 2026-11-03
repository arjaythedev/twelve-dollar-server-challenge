# SQLite 3.54.1 compiled from the amalgamation by build.sh (-O3, native, PGO)
# and linked statically.
@[Link(ldflags: "#{__DIR__}/../build/libsqlite3.a -lm")]
lib Sq
  type DB = Void*
  type Stmt = Void*

  OK         =   0
  ROW        = 100
  DONE       = 101
  CONSTRAINT_FOREIGNKEY = 787
  CONSTRAINT_PRIMARYKEY = 1555
  OPEN_READWRITE = 0x02
  OPEN_NOMUTEX   = 0x8000
  OPEN_EXRESCODE = 0x02000000
  PREPARE_PERSISTENT = 0x01

  fun open_v2 = sqlite3_open_v2(file : UInt8*, db : DB*, flags : LibC::Int, vfs : UInt8*) : LibC::Int
  fun exec = sqlite3_exec(db : DB, sql : UInt8*, cb : Void*, arg : Void*, err : UInt8**) : LibC::Int
  fun prepare_v3 = sqlite3_prepare_v3(db : DB, sql : UInt8*, n : LibC::Int, flags : LibC::UInt, st : Stmt*, tail : UInt8**) : LibC::Int
  fun step = sqlite3_step(st : Stmt) : LibC::Int
  fun reset = sqlite3_reset(st : Stmt) : LibC::Int
  fun bind_int64 = sqlite3_bind_int64(st : Stmt, i : LibC::Int, v : Int64) : LibC::Int
  fun bind_text = sqlite3_bind_text(st : Stmt, i : LibC::Int, p : UInt8*, n : LibC::Int, d : Void*) : LibC::Int
  fun column_int64 = sqlite3_column_int64(st : Stmt, i : LibC::Int) : Int64
  fun column_text = sqlite3_column_text(st : Stmt, i : LibC::Int) : UInt8*
  fun column_bytes = sqlite3_column_bytes(st : Stmt, i : LibC::Int) : LibC::Int
  fun changes = sqlite3_changes(db : DB) : LibC::Int
  fun extended_errcode = sqlite3_extended_errcode(db : DB) : LibC::Int
  fun errmsg = sqlite3_errmsg(db : DB) : UInt8*
  fun initialize = sqlite3_initialize : LibC::Int
  fun wal_checkpoint_v2 = sqlite3_wal_checkpoint_v2(db : DB, name : UInt8*, mode : LibC::Int, log : LibC::Int*, ckpt : LibC::Int*) : LibC::Int
end
