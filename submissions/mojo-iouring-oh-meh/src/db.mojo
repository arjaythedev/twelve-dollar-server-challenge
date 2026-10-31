# SQLite access: one connection, one transaction per batch.
#
# Reads walk SQLite's B-tree cursors (through the shim's bt_* primitives) and
# decode the record format here; writes are ordinary prepared statements and
# are group-committed by the caller. Every request reads its rows from SQLite
# inside the batch's transaction; nothing is cached across requests.
from std.ffi import c_int, external_call
from bytes import Bytes, balloc, bcopy, beq, Out, alloc_i64
from record import Rec

comptime SQLITE_ROW = 100
comptime SQLITE_DONE = 101
comptime FEED_N = 20
comptime CREATED_LEN = 24
comptime WAL_MAX_FRAMES = 8192   # 32 MB at 4 KB pages: at most 2 hash segments to search


@always_inline
def sq_prepare(db: Int, sql: String) -> Int:
    var s = sql
    return external_call["sq_prepare", Int](db, s.as_c_string_span())


@always_inline
def sq_exec(db: Int, sql: String) -> Int:
    var s = sql
    return Int(external_call["sq_exec", c_int](db, s.as_c_string_span()))


@always_inline
def sq_step(st: Int) -> Int:
    return Int(external_call["sq_step", c_int](st))


@always_inline
def sq_reset(st: Int) -> Int:
    return Int(external_call["sq_reset", c_int](st))


@always_inline
def sq_col_int(st: Int, i: Int) -> Int64:
    return external_call["sq_col_int", Int64](st, c_int(i))


@always_inline
def sq_col_text(st: Int, i: Int) -> Bytes:
    return Bytes(unsafe_from_address=external_call["sq_col_text", Int](st, c_int(i)))


@always_inline
def sq_col_bytes(st: Int, i: Int) -> Int:
    return Int(external_call["sq_col_bytes", c_int](st, c_int(i)))


@always_inline
def sq_bind_int(st: Int, i: Int, v: Int64) -> Int:
    return Int(external_call["sq_bind_int", c_int](st, c_int(i), v))


@always_inline
def sq_bind_text(st: Int, i: Int, p: Bytes, n: Int) -> Int:
    return Int(external_call["sq_bind_text", c_int](st, c_int(i), p, c_int(n)))


@always_inline
def sq_changes(db: Int) -> Int:
    return Int(external_call["sq_changes", c_int](db))


def sq_errmsg(db: Int) -> String:
    return String(unsafe_from_utf8_ptr=Bytes(unsafe_from_address=external_call["sq_errmsg", Int](db)))


@always_inline
def bt_open(db: Int, cur: Int, root: Int) -> Int:
    return Int(external_call["bt_open", c_int](db, cur, c_int(root)))


@always_inline
def bt_close(cur: Int):
    external_call["bt_close", NoneType](cur)


@always_inline
def bt_last(cur: Int, mut empty: c_int) -> Int:
    return Int(external_call["bt_last", c_int](cur, Pointer(to=empty)))


@always_inline
def bt_prev(cur: Int) -> Int:
    return Int(external_call["bt_prev", c_int](cur))


@always_inline
def bt_next(cur: Int) -> Int:
    return Int(external_call["bt_next", c_int](cur))


@always_inline
def bt_eof(cur: Int) -> Bool:
    return external_call["bt_eof", c_int](cur) != 0


@always_inline
def bt_moveto(cur: Int, rowid: Int64, mut res: c_int) -> Int:
    return Int(external_call["bt_moveto", c_int](cur, rowid, Pointer(to=res)))


@always_inline
def bt_idx_seek(cur: Int, key: Int64, mut res: c_int) -> Int:
    return Int(external_call["bt_idx_seek_int", c_int](cur, key, Pointer(to=res)))


@always_inline
def bt_rowid(cur: Int) -> Int64:
    return external_call["bt_rowid", Int64](cur)


# A pointer to the row's record bytes; falls back to a copy if it spills to overflow pages.
@always_inline
def bt_record(cur: Int, scratch: Bytes) -> Bytes:
    var amt: UInt32 = 0
    var p = external_call["bt_payload_fetch", Int](cur, Pointer(to=amt))
    var size = external_call["bt_payload_size", UInt32](cur)
    if amt == size:
        return Bytes(unsafe_from_address=p)
    _ = external_call["bt_payload", c_int](cur, UInt32(0), size, scratch)
    return scratch


def write_post_obj(mut o: Out, id: Int64, body: Bytes, blen: Int, created: Bytes, clen: Int, author: Bytes, alen: Int, likes: Int64):
    o.put_lit('{"id":')
    o.put_int(id)
    o.put_lit(',"body":"')
    o.put_json(body, blen)
    o.put_lit('","created_at":"')
    o.put_bytes(created, clen)
    o.put_lit('","author":"')
    o.put_json(author, alen)
    o.put_lit('","like_count":')
    o.put_int(likes)
    o.put(0x7D)


struct Db:
    var h: Int
    var posts_root: Int
    var users_root: Int
    var likes_idx_root: Int
    var cp: Int
    var cu: Int
    var cl: Int
    var cursors_open: Bool
    var in_txn: Bool
    var st_insert_post: Int
    var st_insert_like: Int
    var st_post_exists: Int
    var st_health: Int
    var st_feed_sql: Int
    var st_begin: Int
    var st_commit: Int
    var st_rollback: Int
    var tail_ok: Bool
    var max_id: Int64
    var max_created: Bytes
    var scratch: Bytes
    var rec: Rec
    var urec: Rec
    var counts: Pointer[Int64, MutUntrackedOrigin]

    def __init__(out self, path: String) raises:
        var p = path
        # The invariant scan reads the whole posts table once. Do it on a connection
        # without mmap and with a tiny cache, then drop those pages from the page
        # cache, so the server doesn't start with ~100 MB charged to it.
        self.h = external_call["sq_open_plain", Int](p.as_c_string_span())
        self.posts_root = 0
        self.users_root = 0
        self.likes_idx_root = 0
        self.cp = 0
        self.cu = 0
        self.cl = 0
        self.cursors_open = False
        self.in_txn = False
        self.st_insert_post = 0
        self.st_insert_like = 0
        self.st_post_exists = 0
        self.st_health = 0
        self.st_feed_sql = 0
        self.st_begin = 0
        self.st_commit = 0
        self.st_rollback = 0
        self.tail_ok = False
        self.max_id = 0
        self.max_created = balloc(64)
        self.scratch = balloc(1 << 16)
        self.rec = Rec(self.scratch)
        self.urec = Rec(self.scratch)
        self.counts = alloc_i64(FEED_N)
        if self.h == 0:
            raise Error("cannot open " + path)
        self.posts_root = Int(self._scalar("SELECT rootpage FROM sqlite_master WHERE name='posts'"))
        self.users_root = Int(self._scalar("SELECT rootpage FROM sqlite_master WHERE name='users'"))
        self.likes_idx_root = Int(self._scalar("SELECT rootpage FROM sqlite_master WHERE name='likes_post_id_idx'"))
        if self.posts_root <= 0 or self.users_root <= 0 or self.likes_idx_root <= 0:
            raise Error("schema: missing posts/users/likes_post_id_idx")
        self.cp = external_call["bt_cursor_alloc", Int](self.h, c_int(0))
        self.cu = external_call["bt_cursor_alloc", Int](self.h, c_int(0))
        self.cl = external_call["bt_cursor_alloc", Int](self.h, c_int(1))
        self._prepare_all()

    def _prepare_all(mut self) raises:
        self.st_insert_post = sq_prepare(self.h, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
        self.st_insert_like = sq_prepare(self.h, "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING")
        self.st_post_exists = sq_prepare(self.h, "SELECT 1 FROM posts WHERE id = ?")
        self.st_health = sq_prepare(self.h, "SELECT 1")
        self.st_feed_sql = sq_prepare(
            self.h,
            "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
            " FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
        )
        self.st_begin = sq_prepare(self.h, "BEGIN")
        self.st_commit = sq_prepare(self.h, "COMMIT")
        self.st_rollback = sq_prepare(self.h, "ROLLBACK")
        if self.st_insert_post == 0 or self.st_insert_like == 0 or self.st_post_exists == 0 or self.st_health == 0 or self.st_feed_sql == 0 or self.st_begin == 0 or self.st_commit == 0 or self.st_rollback == 0:
            raise Error("prepare failed: " + sq_errmsg(self.h))

    # Startup: scan on the plain connection, drop the page cache, reopen for real.
    def startup(mut self, path: String) raises:
        self.check_invariant()
        _ = external_call["sq_finalize", c_int](self.st_insert_post)
        _ = external_call["sq_finalize", c_int](self.st_insert_like)
        _ = external_call["sq_finalize", c_int](self.st_post_exists)
        _ = external_call["sq_finalize", c_int](self.st_health)
        _ = external_call["sq_finalize", c_int](self.st_feed_sql)
        _ = external_call["sq_finalize", c_int](self.st_begin)
        _ = external_call["sq_finalize", c_int](self.st_commit)
        _ = external_call["sq_finalize", c_int](self.st_rollback)
        _ = external_call["sq_close", c_int](self.h)
        var p = path
        external_call["sh_drop_file_cache", NoneType](p.as_c_string_span())
        self.h = external_call["sq_open", Int](p.as_c_string_span())
        if self.h == 0:
            raise Error("cannot reopen " + path)
        self.cp = external_call["bt_cursor_alloc", Int](self.h, c_int(0))
        self.cu = external_call["bt_cursor_alloc", Int](self.h, c_int(0))
        self.cl = external_call["bt_cursor_alloc", Int](self.h, c_int(1))
        self._prepare_all()

    def _scalar(mut self, sql: String) -> Int64:
        var st = sq_prepare(self.h, sql)
        if st == 0:
            return -1
        var v: Int64 = -1
        if sq_step(st) == SQLITE_ROW:
            v = sq_col_int(st, 0)
        _ = external_call["sq_finalize", c_int](st)
        return v

    # The tail plan is valid iff post ids are dense (1..n) and created_at never
    # decreases with id. Then "ORDER BY created_at DESC, id DESC LIMIT 20" is
    # exactly the last 20 rows of the posts B-tree. Checked once here and after
    # every insert; the SQL plan takes over for good if it ever fails.
    def check_invariant(mut self):
        var dense = self._scalar("SELECT max(id) = count(*) FROM posts")
        var mono_bad = self._scalar(
            "SELECT count(*) FROM (SELECT created_at < lag(created_at) OVER (ORDER BY id) AS bad FROM posts) WHERE bad"
        )
        var st = sq_prepare(self.h, "SELECT id, created_at FROM posts ORDER BY id DESC LIMIT 1")
        if sq_step(st) == SQLITE_ROW:
            self.max_id = sq_col_int(st, 0)
            var n = sq_col_bytes(st, 1)
            if n == CREATED_LEN:
                bcopy(self.max_created, sq_col_text(st, 1), n)
            else:
                dense = 0
        _ = external_call["sq_finalize", c_int](st)
        self.tail_ok = dense == 1 and mono_bad == 0

    # ---- transactions

    def begin(mut self) raises:
        if self.in_txn:
            return
        var rc = sq_step(self.st_begin)
        _ = sq_reset(self.st_begin)
        if rc != SQLITE_DONE:
            raise Error("BEGIN: " + sq_errmsg(self.h))
        _ = external_call["bt_begin_read", c_int](self.h)
        self.in_txn = True

    def open_cursors(mut self) raises:
        if self.cursors_open:
            return
        self.begin()
        if bt_open(self.h, self.cp, self.posts_root) != 0 or bt_open(self.h, self.cu, self.users_root) != 0 or bt_open(self.h, self.cl, self.likes_idx_root) != 0:
            raise Error("cursor open failed")
        self.cursors_open = True

    def close_cursors(mut self):
        if not self.cursors_open:
            return
        bt_close(self.cp)
        bt_close(self.cu)
        bt_close(self.cl)
        self.cursors_open = False

    # Commit the batch. False means the writes were rolled back.
    def commit(mut self) -> Bool:
        self.close_cursors()
        if not self.in_txn:
            return True
        self.in_txn = False
        var rc = sq_step(self.st_commit)
        _ = sq_reset(self.st_commit)
        if rc == SQLITE_DONE:
            # Keep the WAL bounded: SQLite only restarts it on its own during a write
            # lull, and group commit never leaves one. The passive checkpoints on the
            # other thread keep this cheap; BUSY just means try again next batch.
            if external_call["sq_wal_frames", Int64](self.h) > WAL_MAX_FRAMES:
                _ = external_call["sq_wal_truncate", c_int](self.h)
            return True
        _ = sq_step(self.st_rollback)
        _ = sq_reset(self.st_rollback)
        return False

    def health_ok(mut self) -> Bool:
        var rc = sq_step(self.st_health)
        _ = sq_reset(self.st_health)
        return rc == SQLITE_ROW

    # ---- reads

    # like_count for one post: one range of likes_post_id_idx.
    def like_count(mut self, post_id: Int64) -> Int64:
        var res: c_int = 0
        if bt_idx_seek(self.cl, post_id, res) != 0:
            return 0
        if res < 0:
            if bt_next(self.cl) != 0:
                return 0
        var n: Int64 = 0
        while not bt_eof(self.cl):
            self.urec.decode(bt_record(self.cl, self.scratch))
            if self.urec.int_at(0) != post_id:
                break
            n += 1
            if bt_next(self.cl) != 0:
                break
        return n

    # Username of user id (writes nothing if missing). Returns length.
    def username(mut self, user_id: Int64, mut o: Out) -> Bool:
        var res: c_int = 0
        if bt_moveto(self.cu, user_id, res) != 0 or res != 0:
            return False
        self.urec.decode(bt_record(self.cu, self.scratch))
        o.put_json(self.urec.ptr_at(1), self.urec.len_at(1))
        return True

    def write_feed(mut self, mut o: Out) raises:
        self.open_cursors()
        o.put_lit('{"posts":[')
        if not self.tail_ok:
            self._write_feed_sql(o)
            o.put_lit("]}")
            return
        var empty: c_int = 0
        if bt_last(self.cp, empty) != 0 or empty != 0:
            o.put_lit("]}")
            return
        var max_id = bt_rowid(self.cp)
        var lo = max_id - (FEED_N - 1)
        if lo < 1:
            lo = 1
        for k in range(FEED_N):
            self.counts[unsafe_offset=k] = 0
        # One range scan of likes_post_id_idx covers all 20 like counts.
        var res: c_int = 0
        var ok = bt_idx_seek(self.cl, lo, res) == 0
        if ok and res < 0:
            ok = bt_next(self.cl) == 0
        while ok and not bt_eof(self.cl):
            self.urec.decode(bt_record(self.cl, self.scratch))
            var pid = self.urec.int_at(0)
            if pid > max_id:
                break
            if pid >= lo:
                self.counts[unsafe_offset=Int(pid - lo)] += 1
            if bt_next(self.cl) != 0:
                break
        # Then the last 20 rows of posts, newest first.
        var first = True
        for _k in range(FEED_N):
            var id = bt_rowid(self.cp)
            self.rec.decode(bt_record(self.cp, self.scratch))
            if not first:
                o.put(0x2C)
            first = False
            o.put_lit('{"id":')
            o.put_int(id)
            o.put_lit(',"body":"')
            o.put_json(self.rec.ptr_at(2), self.rec.len_at(2))
            o.put_lit('","created_at":"')
            o.put_bytes(self.rec.ptr_at(3), self.rec.len_at(3))
            o.put_lit('","author":"')
            _ = self.username(self.rec.int_at(1), o)
            o.put_lit('","like_count":')
            var cnt: Int64 = 0
            if id >= lo and id <= max_id:
                cnt = self.counts[unsafe_offset=Int(id - lo)]
            else:
                cnt = self.like_count(id)
            o.put_int(cnt)
            o.put(0x7D)
            if bt_prev(self.cp) != 0:
                break
        o.put_lit("]}")

    def _write_feed_sql(mut self, mut o: Out):
        var first = True
        while sq_step(self.st_feed_sql) == SQLITE_ROW:
            if not first:
                o.put(0x2C)
            first = False
            write_post_obj(
                o,
                sq_col_int(self.st_feed_sql, 0),
                sq_col_text(self.st_feed_sql, 1), sq_col_bytes(self.st_feed_sql, 1),
                sq_col_text(self.st_feed_sql, 2), sq_col_bytes(self.st_feed_sql, 2),
                sq_col_text(self.st_feed_sql, 3), sq_col_bytes(self.st_feed_sql, 3),
                sq_col_int(self.st_feed_sql, 4),
            )
        _ = sq_reset(self.st_feed_sql)

    # Writes {"post":{...}}; False if the post doesn't exist.
    def write_post(mut self, id: Int64, mut o: Out) raises -> Bool:
        self.open_cursors()
        var res: c_int = 0
        if bt_moveto(self.cp, id, res) != 0 or res != 0:
            return False
        self.rec.decode(bt_record(self.cp, self.scratch))
        o.put_lit('{"post":{"id":')
        o.put_int(id)
        o.put_lit(',"body":"')
        o.put_json(self.rec.ptr_at(2), self.rec.len_at(2))
        o.put_lit('","created_at":"')
        o.put_bytes(self.rec.ptr_at(3), self.rec.len_at(3))
        o.put_lit('","author":"')
        _ = self.username(self.rec.int_at(1), o)
        o.put_lit('","like_count":')
        o.put_int(self.like_count(id))
        o.put_lit("}}")
        return True

    # ---- writes (call close_cursors() first; the caller commits)

    # Inserts; on success fills id and created (24 bytes). False on error.
    def insert_post(mut self, user_id: Int64, body: Bytes, blen: Int, mut id: Int64, created: Bytes) raises -> Bool:
        self.begin()
        _ = sq_bind_int(self.st_insert_post, 1, user_id)
        _ = sq_bind_text(self.st_insert_post, 2, body, blen)
        var rc = sq_step(self.st_insert_post)
        if rc != SQLITE_ROW:
            _ = sq_reset(self.st_insert_post)
            return False
        id = sq_col_int(self.st_insert_post, 0)
        var n = sq_col_bytes(self.st_insert_post, 1)
        if n != CREATED_LEN:
            _ = sq_reset(self.st_insert_post)
            return False
        bcopy(created, sq_col_text(self.st_insert_post, 1), n)
        _ = sq_reset(self.st_insert_post)
        # keep the tail-plan invariant honest
        if self.tail_ok:
            var mono = external_call["memcmp", c_int](created, self.max_created, CREATED_LEN) >= 0
            if id != self.max_id + 1 or not mono:
                self.tail_ok = False
        self.max_id = id
        bcopy(self.max_created, created, CREATED_LEN)
        return True

    # 0 = post missing, 1 = liked now, 2 = already liked, -1 = error
    def like(mut self, user_id: Int64, post_id: Int64) raises -> Int:
        self.begin()
        _ = sq_bind_int(self.st_post_exists, 1, post_id)
        var exists = sq_step(self.st_post_exists) == SQLITE_ROW
        _ = sq_reset(self.st_post_exists)
        if not exists:
            return 0
        _ = sq_bind_int(self.st_insert_like, 1, user_id)
        _ = sq_bind_int(self.st_insert_like, 2, post_id)
        var rc = sq_step(self.st_insert_like)
        _ = sq_reset(self.st_insert_like)
        if rc != SQLITE_DONE:
            return -1
        return 1 if sq_changes(self.h) == 1 else 2
