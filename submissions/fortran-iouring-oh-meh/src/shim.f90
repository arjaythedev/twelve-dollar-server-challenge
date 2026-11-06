! Interfaces to the C shim (liburing helpers, SQLite and its B-tree calls, the
! checkpoint thread, a few libc calls) plus the small conveniences around them.
! Opaque C handles (rings by index; db, statements, cursors as void*) travel as
! integer addresses, which is ABI-identical to void*.
module shim
  use iso_c_binding
  use bytes, only: ip, i8, i4, u8, cp, ad, c_str, bytes_at
  implicit none
  private
  public :: cqe_t, CQE_F_BUFFER, CQE_F_MORE, CQE_BUFFER_SHIFT, ENOBUFS, EAGAIN, EINTR
  public :: table_size, ring_init, ring_open, ring_enable_efd, buf_base, buf_return, buf_flush
  public :: listen_on, submit, wait_one, wait_one_batch, settle, wait_rings, reap
  public :: prep_accept, prep_recv, prep_read, prep_send, prep_close
  public :: now_ms, unix_time, mem_available_kb, malloc_bytes, realloc_bytes, free_addr
  public :: getenv_str, getenv_int, logmsg, itoa, drop_file_cache, lock_code, checkpointer_start
  public :: sq_open, sq_open_plain, sq_close, sq_exec, sq_prepare, sq_step, sq_reset, sq_finalize
  public :: sq_col_int, sq_col_text, sq_col_bytes, sq_bind_int, sq_bind_text, sq_changes, sq_errmsg
  public :: sq_wal_frames, sq_wal_backfilled, sq_wal_truncate, sq_wal_want_restart
  public :: bt_begin_read, bt_cursor_alloc, bt_open, bt_close, bt_last, bt_prev, bt_next, bt_moveto
  public :: bt_idx_seek, bt_eof, bt_rowid, bt_payload_size, bt_payload_fetch, bt_payload

  ! CQE flag bits (linux/io_uring.h)
  integer, parameter :: CQE_F_BUFFER = 1, CQE_F_MORE = 2, CQE_BUFFER_SHIFT = 16
  integer, parameter :: ENOBUFS = -105, EAGAIN = -11, EINTR = -4

  type, bind(C) :: cqe_t
    integer(c_int32_t) :: res
    integer(c_int32_t) :: flags     ! uint32 in C; the buffer id lives in the upper 16 bits
    integer(c_int64_t) :: user_data
  end type

  interface
    function sh_table_size() bind(C, name='sh_table_size') result(r)
      import :: c_int
      integer(c_int) :: r
    end function
    function sh_init(sq, table, nbufs, bufsize) bind(C, name='sh_init') result(r)
      import :: c_int
      integer(c_int), value :: sq, table, nbufs, bufsize
      integer(c_int) :: r
    end function
    function sh_ring_open(i) bind(C, name='sh_ring_open') result(r)
      import :: c_int
      integer(c_int), value :: i
      integer(c_int) :: r
    end function
    function sh_ring_enable_efd(i) bind(C, name='sh_ring_enable_efd') result(r)
      import :: c_int
      integer(c_int), value :: i
      integer(c_int) :: r
    end function
    function sh_buf_base(i) bind(C, name='sh_buf_base') result(p)
      import :: c_int, c_ptr
      integer(c_int), value :: i
      type(c_ptr) :: p
    end function
    subroutine sh_buf_return(i, bid) bind(C, name='sh_buf_return')
      import :: c_int
      integer(c_int), value :: i, bid
    end subroutine
    function sh_buf_flush(i) bind(C, name='sh_buf_flush') result(r)
      import :: c_int
      integer(c_int), value :: i
      integer(c_int) :: r
    end function
    function sh_listen(host, port, backlog) bind(C, name='sh_listen') result(r)
      import :: c_int, c_char
      character(kind=c_char), dimension(*) :: host
      integer(c_int), value :: port, backlog
      integer(c_int) :: r
    end function
    function sh_submit(i) bind(C, name='sh_submit') result(r)
      import :: c_int
      integer(c_int), value :: i
      integer(c_int) :: r
    end function
    function sh_wait(timeout_ms) bind(C, name='sh_wait') result(r)
      import :: c_int
      integer(c_int), value :: timeout_ms
      integer(c_int) :: r
    end function
    function sh_wait_one(i) bind(C, name='sh_wait_one') result(r)
      import :: c_int
      integer(c_int), value :: i
      integer(c_int) :: r
    end function
    function sh_wait_one_batch(i, want, timeout_us) bind(C, name='sh_wait_one_batch') result(r)
      import :: c_int
      integer(c_int), value :: i, want, timeout_us
      integer(c_int) :: r
    end function
    subroutine sh_settle(sleep_us) bind(C, name='sh_settle')
      import :: c_int
      integer(c_int), value :: sleep_us
    end subroutine
    function sh_reap(i, cqes, mx) bind(C, name='sh_reap') result(r)
      import :: c_int, cqe_t
      integer(c_int), value :: i, mx
      type(cqe_t), dimension(*) :: cqes
      integer(c_int) :: r
    end function
    function sh_prep_accept(i, lfd, ud) bind(C, name='sh_prep_accept') result(r)
      import :: c_int, c_int64_t
      integer(c_int), value :: i, lfd
      integer(c_int64_t), value :: ud
      integer(c_int) :: r
    end function
    function sh_prep_recv(i, slot, ud) bind(C, name='sh_prep_recv') result(r)
      import :: c_int, c_int64_t
      integer(c_int), value :: i, slot
      integer(c_int64_t), value :: ud
      integer(c_int) :: r
    end function
    function sh_prep_read(i, slot, ud) bind(C, name='sh_prep_read') result(r)
      import :: c_int, c_int64_t
      integer(c_int), value :: i, slot
      integer(c_int64_t), value :: ud
      integer(c_int) :: r
    end function
    function sh_prep_send(i, slot, buf, n, ud) bind(C, name='sh_prep_send') result(r)
      import :: c_int, c_int64_t, c_ptr
      integer(c_int), value :: i, slot, n
      type(c_ptr), value :: buf
      integer(c_int64_t), value :: ud
      integer(c_int) :: r
    end function
    function sh_prep_close(i, slot, ud_shutdown, ud) bind(C, name='sh_prep_close') result(r)
      import :: c_int, c_int64_t
      integer(c_int), value :: i, slot
      integer(c_int64_t), value :: ud_shutdown, ud
      integer(c_int) :: r
    end function
    function sh_malloc(n) bind(C, name='sh_malloc') result(p)
      import :: c_size_t, c_ptr
      integer(c_size_t), value :: n
      type(c_ptr) :: p
    end function
    function sh_realloc(p, n) bind(C, name='sh_realloc') result(q)
      import :: c_size_t, c_ptr
      type(c_ptr), value :: p
      integer(c_size_t), value :: n
      type(c_ptr) :: q
    end function
    subroutine sh_free(p) bind(C, name='sh_free')
      import :: c_ptr
      type(c_ptr), value :: p
    end subroutine
    function sh_now_ms() bind(C, name='sh_now_ms') result(r)
      import :: c_int64_t
      integer(c_int64_t) :: r
    end function
    function sh_unix_time() bind(C, name='sh_unix_time') result(r)
      import :: c_int64_t
      integer(c_int64_t) :: r
    end function
    function sh_getenv(k) bind(C, name='sh_getenv') result(p)
      import :: c_char, c_ptr
      character(kind=c_char), dimension(*) :: k
      type(c_ptr) :: p
    end function
    subroutine sh_log(s) bind(C, name='sh_log')
      import :: c_char
      character(kind=c_char), dimension(*) :: s
    end subroutine
    function sh_mem_available_kb() bind(C, name='sh_mem_available_kb') result(r)
      import :: c_int64_t
      integer(c_int64_t) :: r
    end function
    subroutine sh_drop_file_cache(path) bind(C, name='sh_drop_file_cache')
      import :: c_char
      character(kind=c_char), dimension(*) :: path
    end subroutine
    function sh_lock_code(binary_ok) bind(C, name='sh_lock_code') result(r)
      import :: c_int, c_long
      integer(c_int) :: binary_ok
      integer(c_long) :: r
    end function
    function sh_checkpointer_start(path, interval_ms) bind(C, name='sh_checkpointer_start') result(r)
      import :: c_char, c_int
      character(kind=c_char), dimension(*) :: path
      integer(c_int), value :: interval_ms
      integer(c_int) :: r
    end function
    ! ---- SQLite
    function c_sq_open(path) bind(C, name='sq_open') result(p)
      import :: c_char, c_ptr
      character(kind=c_char), dimension(*) :: path
      type(c_ptr) :: p
    end function
    function c_sq_open_plain(path) bind(C, name='sq_open_plain') result(p)
      import :: c_char, c_ptr
      character(kind=c_char), dimension(*) :: path
      type(c_ptr) :: p
    end function
    function c_sq_close(db) bind(C, name='sq_close') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: db
      integer(c_int) :: r
    end function
    function c_sq_exec(db, sql) bind(C, name='sq_exec') result(r)
      import :: c_ptr, c_int, c_char
      type(c_ptr), value :: db
      character(kind=c_char), dimension(*) :: sql
      integer(c_int) :: r
    end function
    function c_sq_prepare(db, sql) bind(C, name='sq_prepare') result(p)
      import :: c_ptr, c_char
      type(c_ptr), value :: db
      character(kind=c_char), dimension(*) :: sql
      type(c_ptr) :: p
    end function
    function c_sq_step(st) bind(C, name='sq_step') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: st
      integer(c_int) :: r
    end function
    function c_sq_reset(st) bind(C, name='sq_reset') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: st
      integer(c_int) :: r
    end function
    function c_sq_finalize(st) bind(C, name='sq_finalize') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: st
      integer(c_int) :: r
    end function
    function c_sq_col_int(st, i) bind(C, name='sq_col_int') result(r)
      import :: c_ptr, c_int, c_int64_t
      type(c_ptr), value :: st
      integer(c_int), value :: i
      integer(c_int64_t) :: r
    end function
    function c_sq_col_text(st, i) bind(C, name='sq_col_text') result(p)
      import :: c_ptr, c_int
      type(c_ptr), value :: st
      integer(c_int), value :: i
      type(c_ptr) :: p
    end function
    function c_sq_col_bytes(st, i) bind(C, name='sq_col_bytes') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: st
      integer(c_int), value :: i
      integer(c_int) :: r
    end function
    function c_sq_bind_int(st, i, v) bind(C, name='sq_bind_int') result(r)
      import :: c_ptr, c_int, c_int64_t
      type(c_ptr), value :: st
      integer(c_int), value :: i
      integer(c_int64_t), value :: v
      integer(c_int) :: r
    end function
    function c_sq_bind_text(st, i, p, n) bind(C, name='sq_bind_text') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: st, p
      integer(c_int), value :: i, n
      integer(c_int) :: r
    end function
    function c_sq_changes(db) bind(C, name='sq_changes') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: db
      integer(c_int) :: r
    end function
    function c_sq_errmsg(db) bind(C, name='sq_errmsg') result(p)
      import :: c_ptr
      type(c_ptr), value :: db
      type(c_ptr) :: p
    end function
    function c_sq_wal_frames(db) bind(C, name='sq_wal_frames') result(r)
      import :: c_ptr, c_int64_t
      type(c_ptr), value :: db
      integer(c_int64_t) :: r
    end function
    function c_sq_wal_backfilled(db) bind(C, name='sq_wal_backfilled') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: db
      integer(c_int) :: r
    end function
    function c_sq_wal_truncate(db) bind(C, name='sq_wal_truncate') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: db
      integer(c_int) :: r
    end function
    subroutine c_sq_wal_want_restart(on) bind(C, name='sq_wal_want_restart')
      import :: c_int
      integer(c_int), value :: on
    end subroutine
    ! ---- B-tree
    function c_bt_begin_read(db) bind(C, name='bt_begin_read') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: db
      integer(c_int) :: r
    end function
    function c_bt_cursor_alloc(db, nkey) bind(C, name='bt_cursor_alloc') result(p)
      import :: c_ptr, c_int
      type(c_ptr), value :: db
      integer(c_int), value :: nkey
      type(c_ptr) :: p
    end function
    function c_bt_open(db, kc, root) bind(C, name='bt_open') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: db, kc
      integer(c_int), value :: root
      integer(c_int) :: r
    end function
    subroutine c_bt_close(kc) bind(C, name='bt_close')
      import :: c_ptr
      type(c_ptr), value :: kc
    end subroutine
    function c_bt_last(kc, empty) bind(C, name='bt_last') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: kc
      integer(c_int) :: empty
      integer(c_int) :: r
    end function
    function c_bt_prev(kc) bind(C, name='bt_prev') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: kc
      integer(c_int) :: r
    end function
    function c_bt_next(kc) bind(C, name='bt_next') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: kc
      integer(c_int) :: r
    end function
    function c_bt_moveto(kc, rowid, res) bind(C, name='bt_moveto') result(r)
      import :: c_ptr, c_int, c_int64_t
      type(c_ptr), value :: kc
      integer(c_int64_t), value :: rowid
      integer(c_int) :: res
      integer(c_int) :: r
    end function
    function c_bt_idx_seek_int(kc, key, res) bind(C, name='bt_idx_seek_int') result(r)
      import :: c_ptr, c_int, c_int64_t
      type(c_ptr), value :: kc
      integer(c_int64_t), value :: key
      integer(c_int) :: res
      integer(c_int) :: r
    end function
    function c_bt_eof(kc) bind(C, name='bt_eof') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: kc
      integer(c_int) :: r
    end function
    function c_bt_rowid(kc) bind(C, name='bt_rowid') result(r)
      import :: c_ptr, c_int64_t
      type(c_ptr), value :: kc
      integer(c_int64_t) :: r
    end function
    function c_bt_payload_size(kc) bind(C, name='bt_payload_size') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: kc
      integer(c_int) :: r
    end function
    function c_bt_payload_fetch(kc, amt) bind(C, name='bt_payload_fetch') result(p)
      import :: c_ptr, c_int
      type(c_ptr), value :: kc
      integer(c_int) :: amt
      type(c_ptr) :: p
    end function
    function c_bt_payload(kc, off, n, buf) bind(C, name='bt_payload') result(r)
      import :: c_ptr, c_int
      type(c_ptr), value :: kc, buf
      integer(c_int), value :: off, n
      integer(c_int) :: r
    end function
  end interface

contains

  function table_size() result(n)
    integer :: n
    n = int(sh_table_size())
  end function

  function ring_init(sq, table, nbufs, bufsize) result(r)
    integer, intent(in) :: sq, table, nbufs, bufsize
    integer :: r
    r = int(sh_init(int(sq, c_int), int(table, c_int), int(nbufs, c_int), int(bufsize, c_int)))
  end function

  function ring_open(i) result(r)
    integer, intent(in) :: i
    integer :: r
    r = int(sh_ring_open(int(i, c_int)))
  end function

  function ring_enable_efd(i) result(r)
    integer, intent(in) :: i
    integer :: r
    r = int(sh_ring_enable_efd(int(i, c_int)))
  end function

  function buf_base(i) result(a)
    integer, intent(in) :: i
    integer(ip) :: a
    a = ad(sh_buf_base(int(i, c_int)))
  end function

  subroutine buf_return(i, bid)
    integer, intent(in) :: i, bid
    call sh_buf_return(int(i, c_int), int(bid, c_int))
  end subroutine

  function buf_flush(i) result(n)
    integer, intent(in) :: i
    integer :: n
    n = int(sh_buf_flush(int(i, c_int)))
  end function

  function listen_on(host, port, backlog) result(fd)
    character(len=*), intent(in) :: host
    integer, intent(in) :: port, backlog
    integer :: fd
    fd = int(sh_listen(host // c_null_char, int(port, c_int), int(backlog, c_int)))
  end function

  function submit(i) result(r)
    integer, intent(in) :: i
    integer :: r
    r = int(sh_submit(int(i, c_int)))
  end function

  function wait_one(i) result(r)
    integer, intent(in) :: i
    integer :: r
    r = int(sh_wait_one(int(i, c_int)))
  end function

  function wait_one_batch(i, want, timeout_us) result(r)
    integer, intent(in) :: i, want, timeout_us
    integer :: r
    r = int(sh_wait_one_batch(int(i, c_int), int(want, c_int), int(timeout_us, c_int)))
  end function

  subroutine settle(sleep_us)
    integer, intent(in) :: sleep_us
    call sh_settle(int(sleep_us, c_int))
  end subroutine

  function wait_rings(timeout_ms) result(mask)
    integer, intent(in) :: timeout_ms
    integer :: mask
    mask = int(sh_wait(int(timeout_ms, c_int)))
  end function

  function reap(i, cqes, mx) result(n)
    integer, intent(in) :: i, mx
    type(cqe_t), intent(inout) :: cqes(:)
    integer :: n
    n = int(sh_reap(int(i, c_int), cqes, int(mx, c_int)))
  end function

  function prep_accept(i, lfd, ud) result(r)
    integer, intent(in) :: i, lfd
    integer(i8), intent(in) :: ud
    integer :: r
    r = int(sh_prep_accept(int(i, c_int), int(lfd, c_int), ud))
  end function

  function prep_recv(i, slot, ud) result(r)
    integer, intent(in) :: i, slot
    integer(i8), intent(in) :: ud
    integer :: r
    r = int(sh_prep_recv(int(i, c_int), int(slot, c_int), ud))
  end function

  function prep_read(i, slot, ud) result(r)
    integer, intent(in) :: i, slot
    integer(i8), intent(in) :: ud
    integer :: r
    r = int(sh_prep_read(int(i, c_int), int(slot, c_int), ud))
  end function

  function prep_send(i, slot, buf, n, ud) result(r)
    integer, intent(in) :: i, slot, n
    integer(ip), intent(in) :: buf
    integer(i8), intent(in) :: ud
    integer :: r
    r = int(sh_prep_send(int(i, c_int), int(slot, c_int), cp(buf), int(n, c_int), ud))
  end function

  function prep_close(i, slot, ud_shutdown, ud) result(r)
    integer, intent(in) :: i, slot
    integer(i8), intent(in) :: ud_shutdown, ud
    integer :: r
    r = int(sh_prep_close(int(i, c_int), int(slot, c_int), ud_shutdown, ud))
  end function

  function now_ms() result(t)
    integer(i8) :: t
    t = sh_now_ms()
  end function

  function unix_time() result(t)
    integer(i8) :: t
    t = sh_unix_time()
  end function

  function mem_available_kb() result(kb)
    integer(i8) :: kb
    kb = sh_mem_available_kb()
  end function

  function malloc_bytes(n) result(a)
    integer, intent(in) :: n
    integer(ip) :: a
    a = ad(sh_malloc(int(n, c_size_t)))
  end function

  function realloc_bytes(p, n) result(a)
    integer(ip), intent(in) :: p
    integer, intent(in) :: n
    integer(ip) :: a
    a = ad(sh_realloc(cp(p), int(n, c_size_t)))
  end function

  subroutine free_addr(p)
    integer(ip), intent(in) :: p
    call sh_free(cp(p))
  end subroutine

  function getenv_str(name, default) result(s)
    character(len=*), intent(in) :: name, default
    character(len=:), allocatable :: s
    type(c_ptr) :: p
    p = sh_getenv(name // c_null_char)
    if (.not. c_associated(p)) then
      s = default
    else
      s = c_str(ad(p))
    end if
  end function

  ! An integer environment variable, or default when unset or empty.
  function getenv_int(name, default) result(v)
    character(len=*), intent(in) :: name
    integer, intent(in) :: default
    integer :: v
    character(len=:), allocatable :: s
    s = getenv_str(name, '')
    v = default
    if (len(s) > 0) read (s, *) v
  end function

  subroutine logmsg(s)
    character(len=*), intent(in) :: s
    call sh_log(s // c_null_char)
  end subroutine

  function itoa(v) result(s)
    integer(i8), intent(in) :: v
    character(len=:), allocatable :: s
    character(len=24) :: buf
    write (buf, '(I0)') v
    s = trim(buf)
  end function

  subroutine drop_file_cache(path)
    character(len=*), intent(in) :: path
    call sh_drop_file_cache(path // c_null_char)
  end subroutine

  function lock_code(binary_ok) result(locked)
    integer, intent(out) :: binary_ok
    integer(i8) :: locked
    integer(c_int) :: ok
    locked = int(sh_lock_code(ok), i8)
    binary_ok = int(ok)
  end function

  function checkpointer_start(path, interval_ms) result(r)
    character(len=*), intent(in) :: path
    integer, intent(in) :: interval_ms
    integer :: r
    r = int(sh_checkpointer_start(path // c_null_char, int(interval_ms, c_int)))
  end function

  ! ---- SQLite wrappers (handles as addresses)

  function sq_open(path) result(db)
    character(len=*), intent(in) :: path
    integer(ip) :: db
    db = ad(c_sq_open(path // c_null_char))
  end function

  function sq_open_plain(path) result(db)
    character(len=*), intent(in) :: path
    integer(ip) :: db
    db = ad(c_sq_open_plain(path // c_null_char))
  end function

  function sq_close(db) result(r)
    integer(ip), intent(in) :: db
    integer :: r
    r = int(c_sq_close(cp(db)))
  end function

  function sq_exec(db, sql) result(r)
    integer(ip), intent(in) :: db
    character(len=*), intent(in) :: sql
    integer :: r
    r = int(c_sq_exec(cp(db), sql // c_null_char))
  end function

  function sq_prepare(db, sql) result(st)
    integer(ip), intent(in) :: db
    character(len=*), intent(in) :: sql
    integer(ip) :: st
    st = ad(c_sq_prepare(cp(db), sql // c_null_char))
  end function

  function sq_step(st) result(r)
    integer(ip), intent(in) :: st
    integer :: r
    r = int(c_sq_step(cp(st)))
  end function

  function sq_reset(st) result(r)
    integer(ip), intent(in) :: st
    integer :: r
    r = int(c_sq_reset(cp(st)))
  end function

  function sq_finalize(st) result(r)
    integer(ip), intent(in) :: st
    integer :: r
    r = int(c_sq_finalize(cp(st)))
  end function

  function sq_col_int(st, i) result(v)
    integer(ip), intent(in) :: st
    integer, intent(in) :: i
    integer(i8) :: v
    v = c_sq_col_int(cp(st), int(i, c_int))
  end function

  function sq_col_text(st, i) result(a)
    integer(ip), intent(in) :: st
    integer, intent(in) :: i
    integer(ip) :: a
    a = ad(c_sq_col_text(cp(st), int(i, c_int)))
  end function

  function sq_col_bytes(st, i) result(n)
    integer(ip), intent(in) :: st
    integer, intent(in) :: i
    integer :: n
    n = int(c_sq_col_bytes(cp(st), int(i, c_int)))
  end function

  function sq_bind_int(st, i, v) result(r)
    integer(ip), intent(in) :: st
    integer, intent(in) :: i
    integer(i8), intent(in) :: v
    integer :: r
    r = int(c_sq_bind_int(cp(st), int(i, c_int), v))
  end function

  function sq_bind_text(st, i, p, n) result(r)
    integer(ip), intent(in) :: st, p
    integer, intent(in) :: i, n
    integer :: r
    r = int(c_sq_bind_text(cp(st), int(i, c_int), cp(p), int(n, c_int)))
  end function

  function sq_changes(db) result(n)
    integer(ip), intent(in) :: db
    integer :: n
    n = int(c_sq_changes(cp(db)))
  end function

  function sq_errmsg(db) result(s)
    integer(ip), intent(in) :: db
    character(len=:), allocatable :: s
    s = c_str(ad(c_sq_errmsg(cp(db))))
  end function

  function sq_wal_frames(db) result(n)
    integer(ip), intent(in) :: db
    integer(i8) :: n
    n = c_sq_wal_frames(cp(db))
  end function

  function sq_wal_backfilled(db) result(r)
    integer(ip), intent(in) :: db
    integer :: r
    r = int(c_sq_wal_backfilled(cp(db)))
  end function

  function sq_wal_truncate(db) result(r)
    integer(ip), intent(in) :: db
    integer :: r
    r = int(c_sq_wal_truncate(cp(db)))
  end function

  subroutine sq_wal_want_restart(on)
    integer, intent(in) :: on
    call c_sq_wal_want_restart(int(on, c_int))
  end subroutine

  ! ---- B-tree wrappers

  function bt_begin_read(db) result(r)
    integer(ip), intent(in) :: db
    integer :: r
    r = int(c_bt_begin_read(cp(db)))
  end function

  function bt_cursor_alloc(db, nkey) result(kc)
    integer(ip), intent(in) :: db
    integer, intent(in) :: nkey
    integer(ip) :: kc
    kc = ad(c_bt_cursor_alloc(cp(db), int(nkey, c_int)))
  end function

  function bt_open(db, kc, root) result(r)
    integer(ip), intent(in) :: db, kc
    integer, intent(in) :: root
    integer :: r
    r = int(c_bt_open(cp(db), cp(kc), int(root, c_int)))
  end function

  subroutine bt_close(kc)
    integer(ip), intent(in) :: kc
    call c_bt_close(cp(kc))
  end subroutine

  function bt_last(kc, empty) result(r)
    integer(ip), intent(in) :: kc
    integer, intent(out) :: empty
    integer :: r
    integer(c_int) :: e
    e = 0
    r = int(c_bt_last(cp(kc), e))
    empty = int(e)
  end function

  function bt_prev(kc) result(r)
    integer(ip), intent(in) :: kc
    integer :: r
    r = int(c_bt_prev(cp(kc)))
  end function

  function bt_next(kc) result(r)
    integer(ip), intent(in) :: kc
    integer :: r
    r = int(c_bt_next(cp(kc)))
  end function

  function bt_moveto(kc, rowid, res) result(r)
    integer(ip), intent(in) :: kc
    integer(i8), intent(in) :: rowid
    integer, intent(out) :: res
    integer :: r
    integer(c_int) :: rs
    rs = 0
    r = int(c_bt_moveto(cp(kc), rowid, rs))
    res = int(rs)
  end function

  function bt_idx_seek(kc, key, res) result(r)
    integer(ip), intent(in) :: kc
    integer(i8), intent(in) :: key
    integer, intent(out) :: res
    integer :: r
    integer(c_int) :: rs
    rs = 0
    r = int(c_bt_idx_seek_int(cp(kc), key, rs))
    res = int(rs)
  end function

  function bt_eof(kc) result(e)
    integer(ip), intent(in) :: kc
    logical :: e
    e = c_bt_eof(cp(kc)) /= 0
  end function

  function bt_rowid(kc) result(v)
    integer(ip), intent(in) :: kc
    integer(i8) :: v
    v = c_bt_rowid(cp(kc))
  end function

  function bt_payload_size(kc) result(n)
    integer(ip), intent(in) :: kc
    integer :: n
    n = int(c_bt_payload_size(cp(kc)))
  end function

  function bt_payload_fetch(kc, amt) result(a)
    integer(ip), intent(in) :: kc
    integer, intent(out) :: amt
    integer(ip) :: a
    integer(c_int) :: am
    am = 0
    a = ad(c_bt_payload_fetch(cp(kc), am))
    amt = int(am)
  end function

  function bt_payload(kc, off, n, buf) result(r)
    integer(ip), intent(in) :: kc, buf
    integer, intent(in) :: off, n
    integer :: r
    r = int(c_bt_payload(cp(kc), int(off, c_int), int(n, c_int), cp(buf)))
  end function

end module shim
