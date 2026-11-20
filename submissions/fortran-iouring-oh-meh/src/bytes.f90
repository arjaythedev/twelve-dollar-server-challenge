! Raw byte helpers. Everything on the request path works on C-owned memory
! (provided buffers, SQLite pages, buffers leaked once at startup) addressed by
! integer addresses; a Fortran pointer view is laid over a region when its bytes
! are read or written. Bytes are integer(1): signed, so `ub` gives the unsigned
! value where it matters.
module bytes
  use iso_c_binding
  implicit none
  private
  public :: ip, i8, i4, u8, ub, tob, cp, ad, bytes_at, bcopy, bmove, bzero, bcmp, find_byte, find_crlf
  public :: eqlit, eqlit_ci, is_digit, is_ws, parse_uint, parse_pos_int, hex_digit, lower_b
  public :: balloc, calloc_bytes, strlen_c, c_str
  public :: out_t, out_init, put, put_bytes, put_str, put_int, put_json

  integer, parameter :: ip = c_intptr_t, i8 = c_int64_t, i4 = c_int32_t, u8 = c_int8_t

  interface
    function c_memcpy(dst, src, n) bind(C, name='memcpy') result(r)
      import :: c_ptr, c_size_t
      type(c_ptr), value :: dst, src
      integer(c_size_t), value :: n
      type(c_ptr) :: r
    end function
    function c_memmove(dst, src, n) bind(C, name='memmove') result(r)
      import :: c_ptr, c_size_t
      type(c_ptr), value :: dst, src
      integer(c_size_t), value :: n
      type(c_ptr) :: r
    end function
    function c_memset(p, c, n) bind(C, name='memset') result(r)
      import :: c_ptr, c_size_t, c_int
      type(c_ptr), value :: p
      integer(c_int), value :: c
      integer(c_size_t), value :: n
      type(c_ptr) :: r
    end function
    function c_memcmp(a, b, n) bind(C, name='memcmp') result(r)
      import :: c_ptr, c_size_t, c_int
      type(c_ptr), value :: a, b
      integer(c_size_t), value :: n
      integer(c_int) :: r
    end function
    function c_memchr(p, c, n) bind(C, name='memchr') result(r)
      import :: c_ptr, c_size_t, c_int
      type(c_ptr), value :: p
      integer(c_int), value :: c
      integer(c_size_t), value :: n
      type(c_ptr) :: r
    end function
    function c_calloc(n, sz) bind(C, name='calloc') result(r)
      import :: c_ptr, c_size_t
      integer(c_size_t), value :: n, sz
      type(c_ptr) :: r
    end function
    function c_malloc(n) bind(C, name='malloc') result(r)
      import :: c_ptr, c_size_t
      integer(c_size_t), value :: n
      type(c_ptr) :: r
    end function
  end interface

  ! Sequential writer over a byte region: p is a 0-based view of cap bytes at base.
  type :: out_t
    integer(ip) :: base = 0
    integer :: n = 0
    integer :: cap = 0
    integer(u8), pointer, contiguous :: p(:) => null()
  end type

contains

  pure elemental function ub(b) result(v)
    integer(u8), intent(in) :: b
    integer :: v
    v = iand(int(b), 255)
  end function

  ! The byte whose unsigned value is v (0..255).
  pure elemental function tob(v) result(b)
    integer, intent(in) :: v
    integer(u8) :: b
    integer :: t
    t = iand(v, 255)
    if (t >= 128) t = t - 256
    b = int(t, u8)
  end function

  function cp(a) result(p)
    integer(ip), intent(in) :: a
    type(c_ptr) :: p
    p = transfer(a, p)
  end function

  function ad(p) result(a)
    type(c_ptr), intent(in) :: p
    integer(ip) :: a
    a = transfer(p, a)
  end function

  ! A 0-based view of n bytes at address a.
  subroutine bytes_at(a, n, p)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    integer(u8), pointer, contiguous, intent(out) :: p(:)
    integer(u8), pointer, contiguous :: q(:)
    call c_f_pointer(cp(a), q, [max(n, 1)])
    p(0:) => q
  end subroutine

  subroutine bcopy(dst, src, n)
    integer(ip), intent(in) :: dst, src
    integer, intent(in) :: n
    type(c_ptr) :: r
    if (n <= 0) return
    r = c_memcpy(cp(dst), cp(src), int(n, c_size_t))
  end subroutine

  subroutine bmove(dst, src, n)
    integer(ip), intent(in) :: dst, src
    integer, intent(in) :: n
    type(c_ptr) :: r
    if (n <= 0) return
    r = c_memmove(cp(dst), cp(src), int(n, c_size_t))
  end subroutine

  subroutine bzero(p, n)
    integer(ip), intent(in) :: p
    integer, intent(in) :: n
    type(c_ptr) :: r
    if (n <= 0) return
    r = c_memset(cp(p), 0_c_int, int(n, c_size_t))
  end subroutine

  function bcmp(a, b, n) result(r)
    integer(ip), intent(in) :: a, b
    integer, intent(in) :: n
    integer :: r
    r = int(c_memcmp(cp(a), cp(b), int(n, c_size_t)))
  end function

  ! Index of byte c in [start, n) of the region at a, or -1.
  function find_byte(a, n, start, c) result(i)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n, start, c
    integer :: i
    type(c_ptr) :: hit
    i = -1
    if (start >= n) return
    hit = c_memchr(cp(a + start), int(c, c_int), int(n - start, c_size_t))
    if (.not. c_associated(hit)) return
    i = int(ad(hit) - a)
  end function

  ! Index of the first CRLF at or after start, or -1.
  function find_crlf(a, n, start) result(i)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n, start
    integer :: i
    integer(u8), pointer, contiguous :: p(:)
    integer :: k
    i = -1
    if (n < 2) return
    call bytes_at(a, n, p)
    k = start
    do while (k + 1 < n)
      if (p(k) == 13 .and. p(k + 1) == 10) then
        i = k
        return
      end if
      k = k + 1
    end do
  end function

  ! The bytes at a, n bytes, equal the literal s.
  function eqlit(a, n, s) result(eq)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    character(len=*), intent(in) :: s
    logical :: eq
    integer(u8), pointer, contiguous :: p(:)
    integer :: k
    eq = .false.
    if (n /= len(s)) return
    if (n == 0) then
      eq = .true.
      return
    end if
    call bytes_at(a, n, p)
    do k = 0, n - 1
      if (ub(p(k)) /= ichar(s(k + 1:k + 1))) return
    end do
    eq = .true.
  end function

  pure elemental function lower_b(c) result(l)
    integer, intent(in) :: c
    integer :: l
    l = c
    if (c >= 65 .and. c <= 90) l = c + 32
  end function

  ! Case-insensitive compare of n bytes at a with an all-lowercase literal.
  function eqlit_ci(a, n, s) result(eq)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    character(len=*), intent(in) :: s
    logical :: eq
    integer(u8), pointer, contiguous :: p(:)
    integer :: k
    eq = .false.
    if (n /= len(s)) return
    if (n == 0) then
      eq = .true.
      return
    end if
    call bytes_at(a, n, p)
    do k = 0, n - 1
      if (lower_b(ub(p(k))) /= ichar(s(k + 1:k + 1))) return
    end do
    eq = .true.
  end function

  pure elemental function is_digit(c) result(d)
    integer, intent(in) :: c
    logical :: d
    d = c >= 48 .and. c <= 57
  end function

  pure elemental function is_ws(c) result(w)
    integer, intent(in) :: c
    logical :: w
    w = c == 32 .or. c == 9 .or. c == 10 .or. c == 13 .or. c == 11 .or. c == 12
  end function

  pure elemental function hex_digit(d) result(c)
    integer, intent(in) :: d
    integer :: c
    if (d < 10) then
      c = 48 + d
    else
      c = 87 + d
    end if
  end function

  ! Non-negative decimal integer of at most 18 digits (leading zeros allowed); -1 if not digits.
  function parse_uint(a, n) result(v)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    integer(i8) :: v
    integer(u8), pointer, contiguous :: p(:)
    integer :: k, c
    v = -1
    if (n == 0 .or. n > 18) return
    call bytes_at(a, n, p)
    v = 0
    do k = 0, n - 1
      c = ub(p(k))
      if (.not. is_digit(c)) then
        v = -1
        return
      end if
      v = v * 10 + (c - 48)
    end do
  end function

  ! Same, but a positive integer with no sign and no leading zero (a post id).
  function parse_pos_int(a, n) result(v)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    integer(i8) :: v
    integer(u8), pointer, contiguous :: p(:)
    v = -1
    if (n > 0) then
      call bytes_at(a, 1, p)
      if (p(0) == 48) return
    end if
    v = parse_uint(a, n)
  end function

  ! Leaked memory for n bytes (never freed).
  function balloc(n) result(a)
    integer, intent(in) :: n
    integer(ip) :: a
    a = ad(c_malloc(int(max(n, 1), c_size_t)))
  end function

  ! Zeroed, lazily backed memory (calloc): untouched pages cost nothing.
  function calloc_bytes(n, sz) result(a)
    integer(i8), intent(in) :: n
    integer, intent(in) :: sz
    integer(ip) :: a
    a = ad(c_calloc(int(max(n, 1_i8), c_size_t), int(sz, c_size_t)))
  end function

  function strlen_c(a) result(n)
    integer(ip), intent(in) :: a
    integer :: n
    integer(u8), pointer, contiguous :: p(:)
    call bytes_at(a, 1073741823, p)
    n = 0
    do while (p(n) /= 0)
      n = n + 1
    end do
  end function

  ! A Fortran string from a NUL-terminated C string at address a.
  function c_str(a) result(s)
    integer(ip), intent(in) :: a
    character(len=:), allocatable :: s
    integer(u8), pointer, contiguous :: p(:)
    integer :: n, k
    n = strlen_c(a)
    allocate(character(len=n) :: s)
    call bytes_at(a, n, p)
    do k = 0, n - 1
      s(k + 1:k + 1) = achar(ub(p(k)))
    end do
  end function

  ! ---- the sequential writer

  subroutine out_init(o, base, cap)
    type(out_t), intent(out) :: o
    integer(ip), intent(in) :: base
    integer, intent(in) :: cap
    o%base = base
    o%n = 0
    o%cap = cap
    call bytes_at(base, cap, o%p)
  end subroutine

  subroutine put(o, c)
    type(out_t), intent(inout) :: o
    integer, intent(in) :: c
    o%p(o%n) = tob(c)
    o%n = o%n + 1
  end subroutine

  subroutine put_bytes(o, src, n)
    type(out_t), intent(inout) :: o
    integer(ip), intent(in) :: src
    integer, intent(in) :: n
    call bcopy(o%base + o%n, src, n)
    o%n = o%n + n
  end subroutine

  ! A literal's bytes in one copy (transfer of a contiguous slice is a memcpy).
  subroutine put_str(o, s)
    type(out_t), intent(inout) :: o
    character(len=*), intent(in) :: s
    integer :: n
    n = len(s)
    if (n == 0) return
    o%p(o%n:o%n + n - 1) = transfer(s, o%p(0:0), n)
    o%n = o%n + n
  end subroutine

  subroutine put_int(o, v)
    type(out_t), intent(inout) :: o
    integer(i8), intent(in) :: v
    integer(u8) :: tmp(0:19)
    integer(i8) :: x
    integer :: k
    if (v < 0) then
      call put(o, 45)
      x = -v
    else
      x = v
    end if
    k = 20
    do
      k = k - 1
      tmp(k) = tob(48 + int(mod(x, 10_i8)))
      x = x / 10
      if (x == 0) exit
    end do
    o%p(o%n:o%n + 19 - k) = tmp(k:19)
    o%n = o%n + 20 - k
  end subroutine

  ! JSON string body (without quotes): escapes " \ and control characters only.
  ! Runs of clean bytes are copied in one memcpy.
  subroutine put_json(o, src, n)
    type(out_t), intent(inout) :: o
    integer(ip), intent(in) :: src
    integer, intent(in) :: n
    integer(u8), pointer, contiguous :: s(:)
    integer :: i, j, c
    if (n <= 0) return
    call bytes_at(src, n, s)
    i = 0
    do while (i < n)
      j = i
      do while (j < n)
        c = ub(s(j))
        if (c < 32 .or. c == 34 .or. c == 92) exit
        j = j + 1
      end do
      if (j > i) then
        call put_bytes(o, src + i, j - i)
        i = j
      end if
      if (i >= n) exit
      c = ub(s(i))
      select case (c)
      case (34)
        call put(o, 92); call put(o, 34)
      case (92)
        call put(o, 92); call put(o, 92)
      case (10)
        call put(o, 92); call put(o, 110)
      case (13)
        call put(o, 92); call put(o, 114)
      case (9)
        call put(o, 92); call put(o, 116)
      case (8)
        call put(o, 92); call put(o, 98)
      case (12)
        call put(o, 92); call put(o, 102)
      case default
        call put_str(o, '\u00')
        call put(o, hex_digit(shiftr(c, 4)))
        call put(o, hex_digit(iand(c, 15)))
      end select
      i = i + 1
    end do
  end subroutine

end module bytes
