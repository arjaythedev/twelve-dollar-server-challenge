require "rayo/sqlite"

lib Rayo::SQLite::LibSQLite
  fun wal_checkpoint = sqlite3_wal_checkpoint_v2(db : Void*, name : UInt8*, mode : Int32, log : Int32*, done : Int32*) : Int32
end

module Feed::Checkpoint
  # Keep ownership on the worker; the helper uses only C calls on this connection.
  # Start after fork. PASSIVE checkpoints never wait for the request connection.
  class_property database : Rayo::SQLite::Database? = nil

  def self.start(path : String)
    db = Rayo::SQLite::Database.new(path)
    db.exec("SELECT count(*) FROM sqlite_schema") # Open the WAL before checkpointing.
    self.database = db
    tid = uninitialized LibC::PthreadT
    rc = LibC.pthread_create(pointerof(tid), Pointer(LibC::PthreadAttrT).null,
      ->(ptr : Void*) { run(ptr); Pointer(Void).null }, db.ptr)
    raise "cannot start SQLite checkpointer" unless rc == 0
    LibC.pthread_detach(tid)
  end

  def self.run(db : Void*) : Nil
    interval = LibC::Timespec.new(tv_sec: 0, tv_nsec: 20_000_000)
    loop do
      LibC.nanosleep(pointerof(interval), Pointer(LibC::Timespec).null)
      Rayo::SQLite::LibSQLite.wal_checkpoint(db, Pointer(UInt8).null, 0,
        Pointer(Int32).null, Pointer(Int32).null)
    end
  end
end
