require "./sqlite"

module Feed
  # Borrowed SQLite row, serialized before stepping/resetting its statement.
  struct Post
    def initialize(@stmt : Sq::Stmt, @likes : Int64? = nil)
    end

    def to_json(j : Rayo::JSON::Writer)
      j.value({id: Sq.column_int64(@stmt, 0), body: text(1), created_at: text(2),
               author: text(3), like_count: @likes || Sq.column_int64(@stmt, 4)})
    end

    private def text(col)
      Bytes.new(Sq.column_text(@stmt, col), Sq.column_bytes(@stmt, col))
    end
  end

  class Database
    include Rayo::Uring::Flusher
    SELECT = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id "
    @feed : Sq::Stmt
    @ids : Sq::Stmt
    @rows : Sq::Stmt
    @likes : Sq::Stmt
    @one : Sq::Stmt
    @create : Sq::Stmt
    @like : Sq::Stmt
    @ping : Sq::Stmt
    @db = Pointer(Void).null.as(Sq::DB)
    @pending = Pointer(Rayo::Context::Deferred).malloc(8192)
    @finishing = Pointer(Rayo::Context::Deferred).malloc(8192)
    @size = 0
    @fragments = StaticArray(Rayo::Buf, 20).new { Rayo::Buf.new(1024) }
    @slots = StaticArray(Int32, 256).new(-1)
    @counts = StaticArray(Int64, 256).new(0)
    @started = Time.utc.to_unix

    def initialize
      path = ENV["SQLITE_PATH"]
      raise "cannot open SQLite" unless Sq.open_v2(path.to_unsafe, pointerof(@db), Sq::OPEN_READWRITE | Sq::OPEN_NOMUTEX | Sq::OPEN_EXRESCODE, Pointer(UInt8).null) == Sq::OK
      exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY; PRAGMA wal_autocheckpoint=1000; PRAGMA busy_timeout=5000")
      @feed = prepare(SELECT + "ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
      @ids = prepare("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20")
      @rows = prepare("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2")
      @likes = prepare("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id")
      @one = prepare(SELECT + "WHERE p.id=?")
      @create = prepare("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
      @like = prepare("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING")
      @ping = prepare("SELECT 1")
      Rayo::Uring.add_flusher(self)
    end

    private def exec(sql)
      raise "SQLite execution failed" unless Sq.exec(@db, sql.to_unsafe, Pointer(Void).null, Pointer(Void).null, Pointer(Pointer(UInt8)).null) == Sq::OK
    end

    private def prepare(sql) : Sq::Stmt
      stmt = Pointer(Void).null.as(Sq::Stmt)
      raise "SQLite prepare failed" unless Sq.prepare_v3(@db, sql.to_unsafe, sql.bytesize, Sq::PREPARE_PERSISTENT, pointerof(stmt), Pointer(Pointer(UInt8)).null) == Sq::OK
      stmt
    end

    # Keep the response deferred until this loop turn's transaction commits.
    def transaction(ctx)
      flush if @size == 8192
      exec("BEGIN") if @size == 0
      @pending[@size] = ctx.defer
      @size += 1
    end

    def flush : Nil
      # Finishing a deferred response can dispatch the next pipelined request.
      # A separate queue keeps those handles out of the batch being finished.
      while @size > 0
        rc = Sq.exec(@db, "COMMIT", Pointer(Void).null, Pointer(Void).null, Pointer(Pointer(UInt8)).null)
        if rc != Sq::OK
          Sq.exec(@db, "ROLLBACK", Pointer(Void).null, Pointer(Void).null, Pointer(Pointer(UInt8)).null)
        end
        count = @size
        @size = 0
        @pending, @finishing = @finishing, @pending
        count.times do |i|
          handle = @finishing[i]
          if rc != Sq::OK && (ctx = handle.context)
            ctx.output.clear
            Feed.error(ctx, 500, "internal server error")
          end
          handle.finish
        end
      end
    end

    private def query(stmt, &)
      begin
        yield stmt
      ensure
        Sq.reset(stmt)
      end
    end

    def health(ctx)
      query(@ping) do |st|
        if Sq.step(st) == Sq::ROW
          ctx.json status: "ok", db: "ok", uptime_s: Time.utc.to_unix - @started
        else
          ctx.status 503
          ctx.json status: "degraded", db: "unreachable", error: Bytes.new(Sq.errmsg(@db), LibC.strlen(Sq.errmsg(@db)).to_i32)
        end
      end
    end

    def post(ctx, id)
      query(@one) do |st|
        Sq.bind_int64(st, 1, id)
        case Sq.step(st)
        when Sq::ROW  then ctx.json post: Post.new(st)
        when Sq::DONE then Feed.error(ctx, 404, "post not found")
        else               Feed.error(ctx, 500, "internal server error")
        end
      end
    end

    def feed(ctx)
      ids = uninitialized Int64[20]
      n = 0
      lo = Int64::MAX
      hi = 0_i64
      query(@ids) do |st|
        while (rc = Sq.step(st)) == Sq::ROW
          id = Sq.column_int64(st, 0)
          ids[n] = id
          n += 1
          lo = Math.min(lo, id)
          hi = Math.max(hi, id)
        end
        raise "SQLite read failed" unless rc == Sq::DONE
      end
      if n == 0 || hi - lo >= 256
        ctx.json do |j|
          j.object do
            j.field("posts") do
              j.array do
                query(@feed) do |st|
                  while (rc = Sq.step(st)) == Sq::ROW
                    j.value Post.new(st)
                  end
                  raise "SQLite read failed" unless rc == Sq::DONE
                end
              end
            end
          end
        end
        return
      end
      @slots.fill(-1)
      @counts.fill(0)
      n.times { |i| @slots[ids[i] - lo] = i; @fragments[i].clear }
      query(@likes) do |st|
        Sq.bind_int64(st, 1, lo)
        Sq.bind_int64(st, 2, hi)
        while (rc = Sq.step(st)) == Sq::ROW
          @counts[Sq.column_int64(st, 0) - lo] = Sq.column_int64(st, 1)
        end
        raise "SQLite read failed" unless rc == Sq::DONE
      end
      query(@rows) do |st|
        Sq.bind_int64(st, 1, lo)
        Sq.bind_int64(st, 2, hi)
        while (rc = Sq.step(st)) == Sq::ROW
          offset = Sq.column_int64(st, 0) - lo
          slot = @slots[offset]
          Post.new(st, @counts[offset]).to_json(Rayo::JSON::Writer.new(@fragments[slot])) if slot >= 0
        end
        raise "SQLite read failed" unless rc == Sq::DONE
      end
      ctx.json do |j|
        j.object do
          j.field("posts") do
            j.array do
              n.times do |i|
                buf = @fragments[i]
                j.raw Bytes.new(buf.ptr, buf.len)
              end
            end
          end
        end
      end
    end

    def create(ctx, uid, name, body)
      query(@create) do |st|
        Sq.bind_int64(st, 1, uid)
        Sq.bind_text(st, 2, body.to_unsafe, body.size, Pointer(Void).null)
        if Sq.step(st) == Sq::ROW
          ctx.status 201
          ctx.json post: {id: Sq.column_int64(st, 0), body: body,
                          created_at: Bytes.new(Sq.column_text(st, 1), Sq.column_bytes(st, 1)), author: name, like_count: 0}
          raise "SQLite insert failed" unless Sq.step(st) == Sq::DONE
        else
          Feed.error(ctx, 500, "internal server error")
        end
      end
    end

    def like(ctx, uid, id)
      query(@like) do |st|
        Sq.bind_int64(st, 1, uid)
        Sq.bind_int64(st, 2, id)
        if Sq.step(st) == Sq::DONE
          fresh = Sq.changes(@db) > 0
          ctx.status(fresh ? 201 : 200)
          ctx.json liked: true, already_liked: !fresh, post_id: id
        elsif Sq.extended_errcode(@db) == Sq::CONSTRAINT_FOREIGNKEY
          Feed.error(ctx, 404, "post not found")
        else
          Feed.error(ctx, 500, "internal server error")
        end
      end
    end
  end
end
