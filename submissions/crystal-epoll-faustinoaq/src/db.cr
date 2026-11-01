# The SQLite side. One connection, statements prepared once (they cache plans, not data).
# All statements of one event-loop batch share one transaction: BEGIN before the first,
# COMMIT after the last, and the batch's responses are only sent after the COMMIT.
module Db
  @@db = Pointer(Void).null.as(Sq::DB)
  @@feed = Pointer(Void).null.as(Sq::Stmt)
  @@one = Pointer(Void).null.as(Sq::Stmt)
  @@ins_post = Pointer(Void).null.as(Sq::Stmt)
  @@ins_like = Pointer(Void).null.as(Sq::Stmt)
  @@ping = Pointer(Void).null.as(Sq::Stmt)
  @@begin = Pointer(Void).null.as(Sq::Stmt)
  @@commit = Pointer(Void).null.as(Sq::Stmt)
  @@rollback = Pointer(Void).null.as(Sq::Stmt)
  @@in_txn = false
  @@feed_ids = Pointer(Void).null.as(Sq::Stmt)
  @@feed_rows = Pointer(Void).null.as(Sq::Stmt)
  @@feed_likes = Pointer(Void).null.as(Sq::Stmt)
  @@tmp = Buf.new(65536)
  @@slot = Pointer(Int32).null  # id - lo -> position in the feed, or -1
  @@cnt = Pointer(Int64).null   # id - lo -> like count
  @@frag = Pointer(Int32).null  # position -> start of its JSON in @@tmp, len at +20

  SPAN = 256

  POST_SELECT = "SELECT p.id, p.body, p.created_at, u.username, " \
                "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count " \
                "FROM posts p JOIN users u ON u.id = p.user_id "

  def self.open(path : String) : Nil
    Sq.initialize
    db = Pointer(Void).null.as(Sq::DB)
    if Sq.open_v2(path.to_unsafe, pointerof(db), Sq::OPEN_READWRITE | Sq::OPEN_NOMUTEX | Sq::OPEN_EXRESCODE, Pointer(UInt8).null) != Sq::OK
      Sys.die "cannot open #{path}"
    end
    @@db = db
    # WAL + synchronous=NORMAL (rule 6). Checkpoints (the only fsyncs) run on their own
    # thread and connection, so the event loop never blocks on the disk.
    exec "PRAGMA journal_mode = WAL"
    exec "PRAGMA synchronous = NORMAL"
    exec "PRAGMA foreign_keys = ON"
    exec "PRAGMA mmap_size = 1073741824"
    exec "PRAGMA cache_size = -65536"
    exec "PRAGMA temp_store = MEMORY"
    exec "PRAGMA wal_autocheckpoint = 0"
    @@feed = prep(POST_SELECT + "ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    @@feed_ids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20")
    @@feed_rows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id " \
                       "WHERE p.id BETWEEN ?1 AND ?2")
    @@feed_likes = prep("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id")
    @@slot = LibC.malloc(SPAN * 4).as(Int32*)
    @@cnt = LibC.malloc(SPAN * 8).as(Int64*)
    @@frag = LibC.malloc(64 * 4).as(Int32*)
    @@one = prep(POST_SELECT + "WHERE p.id = ?")
    @@ins_post = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
    @@ins_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING")
    @@ping = prep("SELECT 1")
    @@begin = prep("BEGIN")
    @@commit = prep("COMMIT")
    @@rollback = prep("ROLLBACK")
    start_checkpointer(path)
  end

  # A second connection on a plain pthread checkpoints the WAL every 20 ms (PASSIVE: it
  # never blocks the writer). It runs only C calls, no Crystal runtime, no allocation.
  def self.start_checkpointer(path : String) : Nil
    tid = uninitialized LibC::PthreadT
    fn = ->(arg : Void*) { Db.checkpointer(arg.as(UInt8*)); Pointer(Void).null }
    LibC.pthread_create(pointerof(tid), Pointer(LibC::PthreadAttrT).null, fn, path.to_unsafe.as(Void*))
  end

  def self.checkpointer(path : UInt8*) : Nil
    db = Pointer(Void).null.as(Sq::DB)
    return if Sq.open_v2(path, pointerof(db), Sq::OPEN_READWRITE | Sq::OPEN_NOMUTEX, Pointer(UInt8).null) != Sq::OK
    # The pager opens the WAL on its first read; until then a checkpoint is a no-op.
    Sq.exec(db, "SELECT count(*) FROM sqlite_schema".to_unsafe, Pointer(Void).null, Pointer(Void).null, Pointer(Pointer(UInt8)).null)
    ts = LibC::Timespec.new
    ts.tv_nsec = 20_000_000
    log = 0
    done = 0
    loop do
      LibC.nanosleep(pointerof(ts), Pointer(LibC::Timespec).null)
      Sq.wal_checkpoint_v2(db, Pointer(UInt8).null, 0, pointerof(log), pointerof(done))
    end
  end

  private def self.exec(sql : String) : Nil
    err = Pointer(UInt8).null
    if Sq.exec(@@db, sql.to_unsafe, Pointer(Void).null, Pointer(Void).null, pointerof(err)) != Sq::OK
      Sys.die "#{sql}: #{String.new(Sq.errmsg(@@db))}"
    end
  end

  private def self.prep(sql : String) : Sq::Stmt
    st = Pointer(Void).null.as(Sq::Stmt)
    if Sq.prepare_v3(@@db, sql.to_unsafe, sql.bytesize, Sq::PREPARE_PERSISTENT, pointerof(st), Pointer(Pointer(UInt8)).null) != Sq::OK
      Sys.die "prepare #{sql}: #{String.new(Sq.errmsg(@@db))}"
    end
    st
  end

  @[AlwaysInline]
  private def self.txn : Bool
    return true if @@in_txn
    rc = Sq.step(@@begin)
    Sq.reset(@@begin)
    @@in_txn = rc == Sq::DONE
  end

  # COMMIT the batch. false means the writes did not land.
  def self.finish_batch : Bool
    return true unless @@in_txn
    @@in_txn = false
    rc = Sq.step(@@commit)
    Sq.reset(@@commit)
    return true if rc == Sq::DONE
    Sq.step(@@rollback)
    Sq.reset(@@rollback)
    false
  end

  @[AlwaysInline]
  private def self.text(o : Buf, st : Sq::Stmt, col : Int32) : Nil
    p = Sq.column_text(st, col)
    n = Sq.column_bytes(st, col)
    o.json_escape(p, n)
  end

  # Timestamps are stored in wire format and need no escaping.
  @[AlwaysInline]
  private def self.raw(o : Buf, st : Sq::Stmt, col : Int32) : Nil
    o.write(Sq.column_text(st, col), Sq.column_bytes(st, col))
  end

  @[AlwaysInline]
  private def self.post_obj(o : Buf, st : Sq::Stmt) : Nil
    o << %({"id":) << Sq.column_int64(st, 0)
    o << %(,"body":") ; text(o, st, 1)
    o << %(","created_at":") ; raw(o, st, 2)
    o << %(","author":") ; text(o, st, 3)
    o << %(","like_count":) << Sq.column_int64(st, 4) << '}'.ord.to_u8
  end

  # The feed in three index walks instead of 60 seeks: the newest 20 ids come off the
  # (created_at, id) index alone; when they fall in a narrow id range (they do, since ids
  # and timestamps both grow), their rows are one range scan of the posts table and their
  # like counts one range scan of likes_post_id_idx. Every row is still read live; posts in
  # the range that are not in the top 20 are skipped. Otherwise: the plain query.
  def self.feed(o : Buf) : Nil
    return internal(o) unless txn
    ids = uninitialized Int64[20]
    n = 0
    lo = Int64::MAX
    hi = 0_i64
    st = @@feed_ids
    while n < 20 && Sq.step(st) == Sq::ROW
      v = Sq.column_int64(st, 0)
      ids[n] = v
      lo = v if v < lo
      hi = v if v > hi
      n &+= 1
    end
    Sq.reset(st)
    return feed_plain(o) if n == 0 || hi &- lo >= SPAN
    span = (hi &- lo &+ 1).to_i32
    slot = @@slot
    cnt = @@cnt
    span.times { |i| slot[i] = -1; cnt[i] = 0 }
    n.times { |i| slot[ids[i] &- lo] = i }
    st = @@feed_likes
    Sq.bind_int64(st, 1, lo)
    Sq.bind_int64(st, 2, hi)
    while (rc = Sq.step(st)) == Sq::ROW
      cnt[Sq.column_int64(st, 0) &- lo] = Sq.column_int64(st, 1)
    end
    Sq.reset(st)
    return internal(o) if rc != Sq::DONE
    t = @@tmp
    t.clear
    frag = @@frag
    n.times { |i| frag[i] = -1 }
    st = @@feed_rows
    Sq.bind_int64(st, 1, lo)
    Sq.bind_int64(st, 2, hi)
    while (rc = Sq.step(st)) == Sq::ROW
      id = Sq.column_int64(st, 0)
      pos = slot[id &- lo]
      next if pos < 0
      frag[pos] = t.len
      t << %({"id":) << id
      t << %(,"body":") ; text(t, st, 1)
      t << %(","created_at":") ; raw(t, st, 2)
      t << %(","author":") ; text(t, st, 3)
      t << %(","like_count":) << cnt[id &- lo] << '}'.ord.to_u8
      frag[pos &+ 20] = t.len
    end
    Sq.reset(st)
    return internal(o) if rc != Sq::DONE
    b = Resp.open(o, "200 OK")
    o << %({"posts":[)
    first = true
    n.times do |i|
      next if frag[i] < 0 # deleted between the walks: impossible inside one transaction
      o << ','.ord.to_u8 unless first
      first = false
      o.write(t.ptr + frag[i], frag[i &+ 20] &- frag[i])
    end
    o << "]}"
    Resp.close(o, b)
  end

  def self.feed_plain(o : Buf) : Nil
    mark = o.len
    b = Resp.open(o, "200 OK")
    o << %({"posts":[)
    st = @@feed
    first = true
    while (rc = Sq.step(st)) == Sq::ROW
      o << ','.ord.to_u8 unless first
      first = false
      post_obj(o, st)
    end
    Sq.reset(st)
    if rc != Sq::DONE
      o.len = mark
      return internal(o)
    end
    o << "]}"
    Resp.close(o, b)
  end

  def self.post(o : Buf, id : Int64) : Nil
    return internal(o) unless txn
    st = @@one
    Sq.bind_int64(st, 1, id)
    rc = Sq.step(st)
    if rc == Sq::ROW
      b = Resp.open(o, "200 OK")
      o << %({"post":)
      post_obj(o, st)
      o << '}'.ord.to_u8
      Resp.close(o, b)
    elsif rc == Sq::DONE
      o << Resp::POST_NOT_FOUND
    else
      internal(o)
    end
    Sq.reset(st)
  end

  def self.create(o : Buf, uid : Int64, body : UInt8*, len : Int32, name : UInt8*, name_len : Int32) : Nil
    return internal(o) unless txn
    st = @@ins_post
    Sq.bind_int64(st, 1, uid)
    Sq.bind_text(st, 2, body, len, Pointer(Void).null) # SQLITE_STATIC: lives until reset
    if Sq.step(st) == Sq::ROW
      id = Sq.column_int64(st, 0)
      mark = o.len
      b = Resp.open(o, "201 Created")
      o << %({"post":{"id":) << id << %(,"body":")
      o.json_escape(body, len)
      o << %(","created_at":")
      raw(o, st, 1)
      o << %(","author":")
      o.write(name, name_len) # already JSON-escaped: it came out of the token's JSON
      o << %(","like_count":0}})
      Resp.close(o, b)
      if Sq.step(st) != Sq::DONE
        o.len = mark
        internal(o)
      end
    else
      internal(o)
    end
    Sq.reset(st)
  end

  def self.like(o : Buf, uid : Int64, pid : Int64) : Nil
    return internal(o) unless txn
    st = @@ins_like
    Sq.bind_int64(st, 1, uid)
    Sq.bind_int64(st, 2, pid)
    rc = Sq.step(st)
    if rc == Sq::DONE
      fresh = Sq.changes(@@db) > 0
      b = Resp.open(o, fresh ? "201 Created" : "200 OK")
      o << (fresh ? %({"liked":true,"already_liked":false,"post_id":) : %({"liked":true,"already_liked":true,"post_id":))
      o << pid << '}'.ord.to_u8
      Resp.close(o, b)
    elsif Sq.extended_errcode(@@db) == Sq::CONSTRAINT_FOREIGNKEY
      o << Resp::POST_NOT_FOUND
    else
      internal(o)
    end
    Sq.reset(st)
  end

  def self.health(o : Buf, uptime : Int64) : Nil
    rc = txn ? Sq.step(@@ping) : -1
    Sq.reset(@@ping)
    if rc == Sq::ROW
      b = Resp.open(o, "200 OK")
      o << %({"status":"ok","db":"ok","uptime_s":) << uptime << '}'.ord.to_u8
    else
      b = Resp.open(o, "503 Service Unavailable")
      o << %({"status":"degraded","db":"unreachable","error":")
      m = Sq.errmsg(@@db)
      o.json_escape(m, LibC.strlen(m).to_i32)
      o << %("})
    end
    Resp.close(o, b)
  end

  private def self.internal(o : Buf) : Nil
    o << Resp::INTERNAL
  end
end
