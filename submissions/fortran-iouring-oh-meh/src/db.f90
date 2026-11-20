! SQLite access: one connection, one transaction per batch.
!
! Reads walk SQLite's B-tree cursors (through the shim's bt_* primitives) and
! decode the record format here; writes are ordinary prepared statements and
! are group-committed by the caller. Every request reads its rows from SQLite
! inside the batch's transaction; nothing is cached across requests.
module db
  use bytes
  use shim
  use record
  implicit none
  private
  public :: db_open, db_startup, db_begin, db_open_cursors, db_close_cursors, db_commit
  public :: db_release_hold, db_health_ok, db_write_feed, db_write_post, db_insert_post, db_like
  public :: writes_held, holds, restarts, holds_expired, truncates, tail_ok
  public :: put_post_head, put_post_tail
  public :: CREATED_LEN, LIKE_MISSING, LIKE_NEW, LIKE_DUP, LIKE_ERR

  integer, parameter :: SQLITE_ROW = 100, SQLITE_DONE = 101
  integer, parameter :: FEED_N = 20
  integer, parameter :: CREATED_LEN = 24
  ! The WAL is kept short by restarting it, never by a checkpoint on the loop: past
  ! WAL_HOLD_FRAMES the loop holds its writes (reads go on) until the checkpoint
  ! thread reports every frame backfilled, and the next write then restarts the log at
  ! frame 1 inside SQLite's own frame write. A hold lasts one passive pass, at most
  ! WAL_HOLD_MS. Only if the log still reaches WAL_MAX_FRAMES (the thread starved,
  ! a reader pinned the old log) does the loop fall back to a TRUNCATE of its own.
  integer, parameter :: WAL_HOLD_FRAMES = 8192   ! 32 MB at 4 KB pages: at most 2 hash segments to search
  integer, parameter :: WAL_HOLD_MS = 1000
  integer, parameter :: WAL_MAX_FRAMES = 16384
  integer, parameter :: WAL_BUSY_SKIP = 64       ! batches to wait after a BUSY truncate before trying again

  ! db_like results
  integer, parameter :: LIKE_MISSING = 0, LIKE_NEW = 1, LIKE_DUP = 2, LIKE_ERR = -1

  integer(ip) :: h = 0
  integer :: posts_root = 0, users_root = 0, likes_idx_root = 0
  integer(ip) :: cp_ = 0, cu = 0, cl = 0
  logical :: cursors_open = .false., in_txn = .false.
  ! prepared statements, by name
  integer, parameter :: ST_INSERT_POST = 1, ST_INSERT_LIKE = 2, ST_POST_EXISTS = 3, ST_HEALTH = 4, &
                        ST_FEED_SQL = 5, ST_BEGIN = 6, ST_COMMIT = 7, ST_ROLLBACK = 8, ST_PREV = 9, NST = 9
  integer(ip) :: st(NST) = 0
  logical :: tail_ok = .false.
  integer :: wal_skip = 0
  integer :: hold_frames = WAL_HOLD_FRAMES
  logical :: writes_held = .false.
  integer(i8) :: hold_since = 0
  logical :: expect_restart = .false.
  integer(i8) :: frames_at_release = 0
  integer :: holds = 0, restarts = 0, holds_expired = 0, truncates = 0
  integer(ip) :: scratch = 0
  type(rec_t) :: rec, urec
  integer(i8) :: counts(0:FEED_N - 1)

contains

  subroutine fail(msg)
    character(len=*), intent(in) :: msg
    call logmsg('fatal: ' // msg)
    error stop 1
  end subroutine

  ! A pointer to the row's record bytes; falls back to a copy if it spills to overflow pages.
  function bt_record(cur) result(a)
    integer(ip), intent(in) :: cur
    integer(ip) :: a
    integer :: amt, sz, r
    a = bt_payload_fetch(cur, amt)
    sz = bt_payload_size(cur)
    if (amt == sz) return
    r = bt_payload(cur, 0, sz, scratch)
    a = scratch
  end function

  ! The post object, in two halves around the author, which each caller writes its
  ! own way (a users-tree lookup, a token's username, a SQL column): head is
  ! {"id":..,"body":"..","created_at":"..","author":" and tail is ","like_count":N}.
  subroutine put_post_head(o, id, body, blen, created, clen)
    type(out_t), intent(inout) :: o
    integer(i8), intent(in) :: id
    integer(ip), intent(in) :: body, created
    integer, intent(in) :: blen, clen
    call put_str(o, '{"id":')
    call put_int(o, id)
    call put_str(o, ',"body":"')
    call put_json(o, body, blen)
    call put_str(o, '","created_at":"')
    call put_bytes(o, created, clen)
    call put_str(o, '","author":"')
  end subroutine

  subroutine put_post_tail(o, likes)
    type(out_t), intent(inout) :: o
    integer(i8), intent(in) :: likes
    call put_str(o, '","like_count":')
    call put_int(o, likes)
    call put(o, 125)
  end subroutine

  function scalar(sql) result(v)
    character(len=*), intent(in) :: sql
    integer(i8) :: v
    integer(ip) :: st
    integer :: r
    v = -1
    st = sq_prepare(h, sql)
    if (st == 0) return
    if (sq_step(st) == SQLITE_ROW) v = sq_col_int(st, 0)
    r = sq_finalize(st)
  end function

  function sql_text(k) result(s)
    integer, intent(in) :: k
    character(len=:), allocatable :: s
    select case (k)
    case (ST_INSERT_POST)
      s = 'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at'
    case (ST_INSERT_LIKE)
      s = 'INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING'
    case (ST_POST_EXISTS)
      s = 'SELECT 1 FROM posts WHERE id = ?'
    case (ST_HEALTH)
      s = 'SELECT 1'
    case (ST_FEED_SQL)
      s = 'SELECT p.id, p.body, p.created_at, u.username, ' // &
          '(SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ' // &
          'ORDER BY p.created_at DESC, p.id DESC LIMIT 20'
    case (ST_BEGIN)
      s = 'BEGIN'
    case (ST_COMMIT)
      s = 'COMMIT'
    case (ST_ROLLBACK)
      s = 'ROLLBACK'
    case default
      s = 'SELECT created_at FROM posts WHERE id = ?'
    end select
  end function

  subroutine prepare_all()
    integer :: k
    do k = 1, NST
      st(k) = sq_prepare(h, sql_text(k))
    end do
    if (any(st == 0)) call fail('prepare failed: ' // sq_errmsg(h))
  end subroutine

  subroutine finalize_all()
    integer :: k, r
    do k = 1, NST
      r = sq_finalize(st(k))
    end do
  end subroutine

  ! The invariant scan reads the whole posts table once. Do it on a connection
  ! without mmap and with a tiny cache, then drop those pages from the page
  ! cache, so the server doesn't start with ~100 MB charged to it.
  subroutine db_open(path)
    character(len=*), intent(in) :: path
    h = sq_open_plain(path)
    if (h == 0) call fail('cannot open ' // path)
    ! Test-only knob (like FORTRAN_TABLE_OVERRIDE): a tiny threshold makes holds
    ! happen after a few writes, so the probes can exercise them. Never set on the box.
    hold_frames = getenv_int('FORTRAN_WAL_HOLD_OVERRIDE', WAL_HOLD_FRAMES)
    scratch = balloc(65536)
    posts_root = int(scalar("SELECT rootpage FROM sqlite_master WHERE name='posts'"))
    users_root = int(scalar("SELECT rootpage FROM sqlite_master WHERE name='users'"))
    likes_idx_root = int(scalar("SELECT rootpage FROM sqlite_master WHERE name='likes_post_id_idx'"))
    if (posts_root <= 0 .or. users_root <= 0 .or. likes_idx_root <= 0) &
      call fail('schema: missing posts/users/likes_post_id_idx')
    cp_ = bt_cursor_alloc(h, 0)
    cu = bt_cursor_alloc(h, 0)
    cl = bt_cursor_alloc(h, 1)
    call prepare_all()
  end subroutine

  ! The tail plan is valid iff post ids are dense (1..n) and created_at never
  ! decreases with id. Then "ORDER BY created_at DESC, id DESC LIMIT 20" is
  ! exactly the last 20 rows of the posts B-tree. Checked once here by SQL, and
  ! after every insert by reading the previous row back from SQLite (nothing
  ! about the data is remembered: tail_ok only says which plan to use, and the
  ! SQL plan takes over for good if the check ever fails).
  subroutine check_invariant()
    integer(i8) :: dense, mono_bad
    dense = scalar('SELECT max(id) = count(*) FROM posts')
    mono_bad = scalar('SELECT count(*) FROM (SELECT created_at < lag(created_at) OVER (ORDER BY id) AS bad FROM posts) WHERE bad')
    tail_ok = dense == 1 .and. mono_bad == 0
  end subroutine

  ! Startup: scan on the plain connection, drop the page cache, reopen for real.
  subroutine db_startup(path)
    character(len=*), intent(in) :: path
    integer :: r
    call check_invariant()
    call finalize_all()
    r = sq_close(h)
    call drop_file_cache(path)
    h = sq_open(path)
    if (h == 0) call fail('cannot reopen ' // path)
    cp_ = bt_cursor_alloc(h, 0)
    cu = bt_cursor_alloc(h, 0)
    cl = bt_cursor_alloc(h, 1)
    call prepare_all()
  end subroutine

  ! ---- transactions

  subroutine db_begin()
    integer :: rc, r
    if (in_txn) return
    rc = sq_step(st(ST_BEGIN))
    r = sq_reset(st(ST_BEGIN))
    if (rc /= SQLITE_DONE) call fail('BEGIN: ' // sq_errmsg(h))
    r = bt_begin_read(h)
    in_txn = .true.
  end subroutine

  subroutine db_open_cursors()
    if (cursors_open) return
    call db_begin()
    if (bt_open(h, cp_, posts_root) /= 0 .or. bt_open(h, cu, users_root) /= 0 .or. &
        bt_open(h, cl, likes_idx_root) /= 0) call fail('cursor open failed')
    cursors_open = .true.
  end subroutine

  subroutine db_close_cursors()
    if (.not. cursors_open) return
    call bt_close(cp_)
    call bt_close(cu)
    call bt_close(cl)
    cursors_open = .false.
  end subroutine

  ! Commit the batch. False means the writes were rolled back.
  function db_commit() result(ok)
    logical :: ok
    integer :: rc, r
    integer(i8) :: frames
    call db_close_cursors()
    ok = .true.
    if (.not. in_txn) return
    in_txn = .false.
    rc = sq_step(st(ST_COMMIT))
    r = sq_reset(st(ST_COMMIT))
    if (rc == SQLITE_DONE) then
      ! Keep the WAL bounded: SQLite only restarts it on its own when a write
      ! begins with the log fully backfilled, and group commit never leaves such
      ! a moment unless the loop makes one by holding its writes (see the top).
      frames = sq_wal_frames(h)
      if (expect_restart .and. frames /= frames_at_release) then
        ! the first write after a hold: a restarted log has few frames
        if (frames < frames_at_release) restarts = restarts + 1
        expect_restart = .false.
      end if
      if (.not. writes_held .and. frames > int(hold_frames, i8)) then
        writes_held = .true.
        hold_since = now_ms()
        holds = holds + 1
        call sq_wal_want_restart(1)
      end if
      if (wal_skip > 0) then
        wal_skip = wal_skip - 1
      else if (frames > WAL_MAX_FRAMES) then
        truncates = truncates + 1
        if (sq_wal_truncate(h) /= 0) wal_skip = WAL_BUSY_SKIP
      end if
      return
    end if
    rc = sq_step(st(ST_ROLLBACK))
    r = sq_reset(st(ST_ROLLBACK))
    ok = .false.
  end function

  ! Called before each batch starts its transaction: end a hold once the log is
  ! fully backfilled (this batch's write restarts it) or once it has lasted too long.
  subroutine db_release_hold()
    if (.not. writes_held) return
    if (sq_wal_backfilled(h) /= 0) then
      writes_held = .false.
      expect_restart = .true.
      frames_at_release = sq_wal_frames(h)
    else if (now_ms() - hold_since > WAL_HOLD_MS) then
      writes_held = .false.
      holds_expired = holds_expired + 1
      call sq_wal_want_restart(0)
    end if
  end subroutine

  function db_health_ok() result(ok)
    logical :: ok
    integer :: rc, r
    rc = sq_step(st(ST_HEALTH))
    r = sq_reset(st(ST_HEALTH))
    ok = rc == SQLITE_ROW
  end function

  ! ---- reads

  ! like_count for one post: one range of likes_post_id_idx.
  function like_count(post_id) result(n)
    integer(i8), intent(in) :: post_id
    integer(i8) :: n
    integer :: res
    n = 0
    if (bt_idx_seek(cl, post_id, res) /= 0) return
    if (res < 0) then
      if (bt_next(cl) /= 0) return
    end if
    do while (.not. bt_eof(cl))
      call rec_decode(urec, bt_record(cl))
      if (rec_int(urec, 0) /= post_id) exit
      n = n + 1
      if (bt_next(cl) /= 0) exit
    end do
  end function

  ! Username of user id (writes nothing if missing).
  function username(user_id, o) result(ok)
    integer(i8), intent(in) :: user_id
    type(out_t), intent(inout) :: o
    logical :: ok
    integer :: res
    ok = .false.
    if (bt_moveto(cu, user_id, res) /= 0) return
    if (res /= 0) return
    call rec_decode(urec, bt_record(cu))
    call put_json(o, rec_ptr(urec, 1), rec_len(urec, 1))
    ok = .true.
  end function

  subroutine db_write_feed(o)
    type(out_t), intent(inout) :: o
    integer :: empty, res, k, rc
    integer(i8) :: max_id, lo, pid, id, cnt
    logical :: ok, first
    call db_open_cursors()
    call put_str(o, '{"posts":[')
    if (.not. tail_ok) then
      call write_feed_sql(o)
      call put_str(o, ']}')
      return
    end if
    rc = bt_last(cp_, empty)   ! the call first: its out-argument may not be read in the same expression
    if (rc /= 0 .or. empty /= 0) then
      call put_str(o, ']}')
      return
    end if
    max_id = bt_rowid(cp_)
    lo = max_id - (FEED_N - 1)
    if (lo < 1) lo = 1
    counts = 0
    ! One range scan of likes_post_id_idx covers all 20 like counts.
    ok = bt_idx_seek(cl, lo, res) == 0
    if (ok .and. res < 0) ok = bt_next(cl) == 0
    do while (ok)
      if (bt_eof(cl)) exit
      call rec_decode(urec, bt_record(cl))
      pid = rec_int(urec, 0)
      if (pid > max_id) exit
      if (pid >= lo) counts(int(pid - lo)) = counts(int(pid - lo)) + 1
      if (bt_next(cl) /= 0) exit
    end do
    ! Then the last 20 rows of posts, newest first.
    first = .true.
    do k = 1, FEED_N
      id = bt_rowid(cp_)
      call rec_decode(rec, bt_record(cp_))
      if (.not. first) call put(o, 44)
      first = .false.
      call put_post_head(o, id, rec_ptr(rec, 2), rec_len(rec, 2), rec_ptr(rec, 3), rec_len(rec, 3))
      ok = username(rec_int(rec, 1), o)
      if (id >= lo .and. id <= max_id) then
        cnt = counts(int(id - lo))
      else
        cnt = like_count(id)
      end if
      call put_post_tail(o, cnt)
      if (bt_prev(cp_) /= 0) exit
    end do
    call put_str(o, ']}')
  end subroutine

  subroutine write_feed_sql(o)
    type(out_t), intent(inout) :: o
    logical :: first
    integer :: r
    first = .true.
    do while (sq_step(st(ST_FEED_SQL)) == SQLITE_ROW)
      if (.not. first) call put(o, 44)
      first = .false.
      call put_post_head(o, sq_col_int(st(ST_FEED_SQL), 0), sq_col_text(st(ST_FEED_SQL), 1), sq_col_bytes(st(ST_FEED_SQL), 1), &
        sq_col_text(st(ST_FEED_SQL), 2), sq_col_bytes(st(ST_FEED_SQL), 2))
      call put_json(o, sq_col_text(st(ST_FEED_SQL), 3), sq_col_bytes(st(ST_FEED_SQL), 3))
      call put_post_tail(o, sq_col_int(st(ST_FEED_SQL), 4))
    end do
    r = sq_reset(st(ST_FEED_SQL))
  end subroutine

  ! Writes {"post":{...}}; false if the post doesn't exist.
  function db_write_post(id, o) result(ok)
    integer(i8), intent(in) :: id
    type(out_t), intent(inout) :: o
    logical :: ok
    integer :: res
    logical :: u
    call db_open_cursors()
    ok = .false.
    if (bt_moveto(cp_, id, res) /= 0) return
    if (res /= 0) return
    call rec_decode(rec, bt_record(cp_))
    call put_str(o, '{"post":')
    call put_post_head(o, id, rec_ptr(rec, 2), rec_len(rec, 2), rec_ptr(rec, 3), rec_len(rec, 3))
    u = username(rec_int(rec, 1), o)
    call put_post_tail(o, like_count(id))
    call put(o, 125)
    ok = .true.
  end function

  ! ---- writes (call db_close_cursors() first; the caller commits)

  ! Inserts; on success fills id and the 24 bytes at created. False on error.
  function db_insert_post(user_id, body, blen, id, created) result(ok)
    integer(i8), intent(in) :: user_id
    integer(ip), intent(in) :: body, created
    integer, intent(in) :: blen
    integer(i8), intent(out) :: id
    logical :: ok
    integer :: rc, r, n, pn
    logical :: prev_ok
    call db_begin()
    ok = .false.
    id = 0
    r = sq_bind_int(st(ST_INSERT_POST), 1, user_id)
    r = sq_bind_text(st(ST_INSERT_POST), 2, body, blen)
    rc = sq_step(st(ST_INSERT_POST))
    if (rc /= SQLITE_ROW) then
      r = sq_reset(st(ST_INSERT_POST))
      return
    end if
    id = sq_col_int(st(ST_INSERT_POST), 0)
    n = sq_col_bytes(st(ST_INSERT_POST), 1)
    if (n /= CREATED_LEN) then
      r = sq_reset(st(ST_INSERT_POST))
      return
    end if
    call bcopy(created, sq_col_text(st(ST_INSERT_POST), 1), n)
    r = sq_reset(st(ST_INSERT_POST))
    ! keep the tail-plan invariant honest: the row before this one must exist
    ! (dense ids) and must not be newer (monotonic created_at); read it back
    if (tail_ok) then
      r = sq_bind_int(st(ST_PREV), 1, id - 1)
      prev_ok = .false.
      if (sq_step(st(ST_PREV)) == SQLITE_ROW) then
        pn = sq_col_bytes(st(ST_PREV), 0)
        if (pn == CREATED_LEN) prev_ok = bcmp(created, sq_col_text(st(ST_PREV), 0), CREATED_LEN) >= 0
      end if
      r = sq_reset(st(ST_PREV))
      if (.not. prev_ok) tail_ok = .false.
    end if
    ok = .true.
  end function

  ! LIKE_MISSING, LIKE_NEW, LIKE_DUP or LIKE_ERR
  function db_like(user_id, post_id) result(code)
    integer(i8), intent(in) :: user_id, post_id
    integer :: code
    integer :: r, rc
    logical :: exists
    call db_begin()
    r = sq_bind_int(st(ST_POST_EXISTS), 1, post_id)
    exists = sq_step(st(ST_POST_EXISTS)) == SQLITE_ROW
    r = sq_reset(st(ST_POST_EXISTS))
    if (.not. exists) then
      code = LIKE_MISSING
      return
    end if
    r = sq_bind_int(st(ST_INSERT_LIKE), 1, user_id)
    r = sq_bind_int(st(ST_INSERT_LIKE), 2, post_id)
    rc = sq_step(st(ST_INSERT_LIKE))
    r = sq_reset(st(ST_INSERT_LIKE))
    if (rc /= SQLITE_DONE) then
      code = LIKE_ERR
      return
    end if
    if (sq_changes(h) == 1) then
      code = LIKE_NEW
    else
      code = LIKE_DUP
    end if
  end function

end module db
