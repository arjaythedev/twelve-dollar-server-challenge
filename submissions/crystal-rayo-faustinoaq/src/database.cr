require "rayo/sqlite/group_commit"
require "./checkpoint"

# Direct B-tree reads compiled into the SQLite build (src/native.c).
lib LibNative
  struct Row
    id : Int64
    user_id : Int64
    likes : Int64
    body_off : UInt32
    body_len : UInt32
    at_off : UInt32
    at_len : UInt32
    au_off : UInt32
    au_len : UInt32
  end

  fun nf_init(db : Void*) : Int32
  fun nf_disable : Void
  fun nf_feed(rows : Row*, max : Int32, buf : UInt8*, cap : UInt32) : Int32
  fun nf_post(id : Int64, row : Row*, buf : UInt8*, cap : UInt32) : Int32
end

module Feed
  # Borrowed SQLite row, serialized before stepping/resetting its statement.
  struct Post
    def initialize(@stmt : Rayo::SQLite::Statement)
    end

    def to_json(j : Rayo::JSON::Writer)
      j.value({id: @stmt.i64(0), body: @stmt.str(1), created_at: @stmt.str(2),
               author: @stmt.str(3), like_count: @stmt.i64(4)})
    end
  end

  class Database
    SELECT = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id "
    @feed : Rayo::SQLite::Statement
    @ids : Rayo::SQLite::Statement
    @rows : Rayo::SQLite::Statement
    @likes : Rayo::SQLite::Statement
    @one : Rayo::SQLite::Statement
    @create : Rayo::SQLite::Statement
    @like : Rayo::SQLite::Statement
    @ping : Rayo::SQLite::Statement
    @db : Rayo::SQLite::Database
    @group : Rayo::SQLite::GroupCommit
    alias Fields = NamedTuple(id: Int64, body: Bytes, created_at: Bytes, author: Bytes, like_count: Int64)
    @posts = StaticArray(Fields, 20).new({id: 0_i64, body: Bytes.empty, created_at: Bytes.empty, author: Bytes.empty, like_count: 0_i64})
    @fragments = StaticArray(Rayo::Buf, 20).new { Rayo::Buf.new(1024) }
    @slots = StaticArray(Int32, 256).new(-1)
    @counts = StaticArray(Int64, 256).new(0)
    @started = Time.utc.to_unix
    @native = StaticArray(LibNative::Row, 20).new(LibNative::Row.new)
    @text = Bytes.new(65536)
    @last_at = Bytes.new(32)
    @last_at_len = 0

    def initialize
      path = ENV["SQLITE_PATH"]
      @db = Rayo::SQLite::Database.new(path)
      @db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY; PRAGMA wal_autocheckpoint=0; PRAGMA busy_timeout=5000")
      @feed = @db.prepare(SELECT + "ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
      @ids = @db.prepare("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20")
      @rows = @db.prepare("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2")
      @likes = @db.prepare("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id")
      @one = @db.prepare(SELECT + "WHERE p.id=?")
      @create = @db.prepare("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
      @like = @db.prepare("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING")
      @ping = @db.prepare("SELECT 1")
      @group = Rayo::SQLite::GroupCommit.new(@db, capacity: 8192)
      @group.attach
      native_init
      Checkpoint.start(path)
    end

    # The native feed reads posts in reverse rowid order, which matches the feed order
    # while created_at never decreases with rowid. Checked here over all posts, then on
    # every insert (`create`); SQL is used for good once it fails.
    private def native_init
      ok = false
      st = @db.prepare("SELECT (SELECT count(*) FROM (SELECT created_at < lag(created_at) OVER (ORDER BY id) AS back FROM posts) WHERE back), (SELECT created_at FROM posts ORDER BY id DESC LIMIT 1)")
      begin
        if st.step && st.i64(0) == 0
          last = st.str(1)
          if last.size <= @last_at.size
            last.copy_to(@last_at.to_unsafe, last.size)
            @last_at_len = last.size
            ok = true
          end
        end
      ensure
        st.close
      end
      ok = LibNative.nf_init(@db.ptr) == 1 if ok
      STDERR.puts "native feed reads: #{ok ? "on" : "off"}"
    end

    private def native(row : LibNative::Row)
      {id: row.id, body: Bytes.new(@text.to_unsafe + row.body_off, row.body_len),
       created_at: Bytes.new(@text.to_unsafe + row.at_off, row.at_len),
       author: Bytes.new(@text.to_unsafe + row.au_off, row.au_len), like_count: row.likes}
    end

    private def track(created_at : Bytes)
      last = Bytes.new(@last_at.to_unsafe, @last_at_len)
      if created_at.size > @last_at.size || (created_at <=> last) < 0
        LibNative.nf_disable
      else
        created_at.copy_to(@last_at.to_unsafe, created_at.size)
        @last_at_len = created_at.size
      end
    end

    def transaction(ctx, &)
      @group.transaction(ctx) { yield }
    end

    private def query(stmt, &)
      begin
        yield stmt
      ensure
        stmt.reset
      end
    end

    def health(ctx)
      query(@ping) do |st|
        st.step
        ctx.json status: "ok", db: "ok", uptime_s: Time.utc.to_unix - @started
      end
    rescue ex : Rayo::SQLite::Error
      ctx.status 503
      ctx.json status: "degraded", db: "unreachable", error: ex.message
    end

    def post(ctx, id)
      case LibNative.nf_post(id, @native.to_unsafe, @text.to_unsafe, @text.size.to_u32)
      when 1
        ctx.json post: native(@native[0])
        return
      when 0
        Feed.error(ctx, 404, "post not found")
        return
      end
      query(@one) do |st|
        st.bind(1, id)
        if st.step
          ctx.json post: Post.new(st)
        else
          Feed.error(ctx, 404, "post not found")
        end
      end
    end

    def feed(ctx)
      n = LibNative.nf_feed(@native.to_unsafe, 20, @text.to_unsafe, @text.size.to_u32)
      if n >= 0
        ctx.json do |j|
          j.object do
            j.field("posts") do
              j.array(0...n) { |i| native(@native[i]) }
            end
          end
        end
        return
      end
      ids = uninitialized Int64[20]
      n = 0
      lo = Int64::MAX
      hi = 0_i64
      query(@ids) do |st|
        while st.step
          id = st.i64(0)
          ids[n] = id
          n += 1
          lo = Math.min(lo, id)
          hi = Math.max(hi, id)
        end
      end
      if n == 0 || hi - lo >= 256
        ctx.json do |j|
          j.object do
            j.field("posts") do
              j.array do
                query(@feed) do |st|
                  while st.step
                    j.value Post.new(st)
                  end
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
        st.bind(1, lo)
        st.bind(2, hi)
        while st.step
          @counts[st.i64(0) - lo] = st.i64(1)
        end
      end
      query(@rows) do |st|
        st.bind(1, lo)
        st.bind(2, hi)
        while st.step
          offset = st.i64(0) - lo
          slot = @slots[offset]
          if slot >= 0
            buf = @fragments[slot]
            body, time, name = st.str(1), st.str(2), st.str(3)
            buf.write(body.to_unsafe, body.size)
            buf.write(time.to_unsafe, time.size)
            buf.write(name.to_unsafe, name.size)
            # Take views only after all writes, which may grow the buffer.
            @posts[slot] = {id: st.i64(0), body: Bytes.new(buf.ptr, body.size),
                            created_at: Bytes.new(buf.ptr + body.size, time.size),
                            author: Bytes.new(buf.ptr + body.size + time.size, name.size), like_count: @counts[offset]}
          end
        end
      end
      ctx.json do |j|
        j.object do
          j.field("posts") do
            j.array(0...n) { |i| @posts[i] }
          end
        end
      end
    end

    def create(ctx, uid, name, body)
      query(@create) do |st|
        st.bind(1, uid)
        st.bind(2, body)
        if st.step
          ctx.status 201
          track(st.str(1))
          ctx.json post: {id: st.i64(0), body: body,
                          created_at: st.str(1), author: name, like_count: 0}
          raise "SQLite insert failed" if st.step
        else
          Feed.error(ctx, 500, "internal server error")
        end
      end
    end

    def like(ctx, uid, id)
      query(@like) do |st|
        st.bind(1, uid)
        st.bind(2, id)
        st.step
        fresh = @db.rows_affected > 0
        ctx.status(fresh ? 201 : 200)
        ctx.json liked: true, already_liked: !fresh, post_id: id
      end
    rescue ex : Rayo::SQLite::Error
      raise ex unless ex.code == 787 # SQLITE_CONSTRAINT_FOREIGNKEY
      Feed.error(ctx, 404, "post not found")
    end
  end
end
