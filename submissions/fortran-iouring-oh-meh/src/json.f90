! A small JSON scanner, used two ways:
!   - jscan_t: find a top-level key in an object and inspect its value
!     (JWT header/payload fields).
!   - validate_body(): validate a whole document and extract the top-level
!     "body" member as an unescaped UTF-8 string (POST /posts).
module json
  use bytes
  implicit none
  private
  public :: jscan_t, jscan_init, find_key, value_is_string, string_raw_ptr, string_raw_len
  public :: string_unescape, number_int, validate_body
  public :: BODY_OK, BODY_MALFORMED, BODY_REQUIRED, BODY_TOO_LONG

  integer, parameter :: MAX_DEPTH = 64
  integer, parameter :: BODY_OK = 0, BODY_MALFORMED = 1, BODY_REQUIRED = 2, BODY_TOO_LONG = 3
  integer, parameter :: MAX_BODY_CHARS = 500

  ! Cursor over one JSON object: find a top-level key and look at its value.
  type :: jscan_t
    integer(ip) :: a = 0
    integer :: n = 0
    integer :: vstart = -1    ! index of the value found by find_key
    integer :: vend = -1
  end type

contains

  pure function hexval(c) result(v)
    integer, intent(in) :: c
    integer :: v
    if (c >= 48 .and. c <= 57) then
      v = c - 48
    else if (c >= 97 .and. c <= 102) then
      v = c - 97 + 10
    else if (c >= 65 .and. c <= 70) then
      v = c - 65 + 10
    else
      v = -1
    end if
  end function

  ! Four hex digits at p(at:at+3) as an int, or -1.
  function hex4(p, at) result(cp4)
    integer(u8), intent(in) :: p(0:)
    integer, intent(in) :: at
    integer :: cp4, k, h
    cp4 = 0
    do k = 0, 3
      h = hexval(ub(p(at + k)))
      if (h < 0) then
        cp4 = -1
        return
      end if
      cp4 = ior(shiftl(cp4, 4), h)
    end do
  end function

  ! Skip a JSON string starting at p(start) == '"'. Returns the index after the
  ! closing quote, or -1 if invalid. With wr set, writes the unescaped bytes to d
  ! (up to cap) and stores the count in out_len.
  function scan_string(p, n, start, wr, d, cap, out_len) result(idx)
    integer(u8), intent(in) :: p(0:)
    integer, intent(in) :: n, start, cap
    logical, intent(in) :: wr
    integer(u8), intent(inout) :: d(0:)
    integer, intent(out) :: out_len
    integer :: idx, i, o, c, e, b, cpt, lo
    i = start + 1
    o = 0
    out_len = 0
    idx = -1
    do
      if (i >= n) return
      c = ub(p(i))
      if (c == 34) then
        out_len = o
        idx = i + 1
        return
      end if
      if (c < 32) return
      if (c == 92) then
        i = i + 1
        if (i >= n) return
        e = ub(p(i))
        select case (e)
        case (34, 92, 47)
          b = e
        case (98)
          b = 8
        case (102)
          b = 12
        case (110)
          b = 10
        case (114)
          b = 13
        case (116)
          b = 9
        case (117)
          if (i + 4 >= n) return
          cpt = hex4(p, i + 1)
          if (cpt < 0) return
          i = i + 4
          if (cpt >= 55296 .and. cpt <= 56319) then
            ! high surrogate: needs \uDC00-\uDFFF next
            if (i + 6 >= n) return
            if (p(i + 1) /= 92 .or. p(i + 2) /= 117) return
            lo = hex4(p, i + 3)
            if (lo < 56320 .or. lo > 57343) return
            i = i + 6
            cpt = 65536 + shiftl(cpt - 55296, 10) + (lo - 56320)
          else if (cpt >= 56320 .and. cpt <= 57343) then
            return
          end if
          if (wr) then
            if (o + 4 > cap) return
            if (cpt < 128) then
              d(o) = tob(cpt)
              o = o + 1
            else if (cpt < 2048) then
              d(o) = tob(ior(192, shiftr(cpt, 6)))
              d(o + 1) = tob(ior(128, iand(cpt, 63)))
              o = o + 2
            else if (cpt < 65536) then
              d(o) = tob(ior(224, shiftr(cpt, 12)))
              d(o + 1) = tob(ior(128, iand(shiftr(cpt, 6), 63)))
              d(o + 2) = tob(ior(128, iand(cpt, 63)))
              o = o + 3
            else
              d(o) = tob(ior(240, shiftr(cpt, 18)))
              d(o + 1) = tob(ior(128, iand(shiftr(cpt, 12), 63)))
              d(o + 2) = tob(ior(128, iand(shiftr(cpt, 6), 63)))
              d(o + 3) = tob(ior(128, iand(cpt, 63)))
              o = o + 4
            end if
          end if
          i = i + 1
          cycle
        case default
          return
        end select
        if (wr) then
          if (o + 1 > cap) return
          d(o) = tob(b)
          o = o + 1
        end if
        i = i + 1
        cycle
      end if
      if (wr) then
        if (o + 1 > cap) return
        d(o) = p(i)
        o = o + 1
      end if
      i = i + 1
    end do
  end function

  ! Skip a number starting at p(start); returns the index after it or -1.
  function scan_number(p, n, start) result(idx)
    integer(u8), intent(in) :: p(0:)
    integer, intent(in) :: n, start
    integer :: idx, i, dgt
    idx = -1
    i = start
    if (i < n) then
      if (p(i) == 45) i = i + 1
    end if
    if (i >= n) return
    if (p(i) == 48) then
      i = i + 1
    else if (is_digit(ub(p(i)))) then
      do while (i < n)
        if (.not. is_digit(ub(p(i)))) exit
        i = i + 1
      end do
    else
      return
    end if
    if (i < n) then
      if (p(i) == 46) then
        i = i + 1
        dgt = 0
        do while (i < n)
          if (.not. is_digit(ub(p(i)))) exit
          i = i + 1
          dgt = dgt + 1
        end do
        if (dgt == 0) return
      end if
    end if
    if (i < n) then
      if (p(i) == 101 .or. p(i) == 69) then
        i = i + 1
        if (i < n) then
          if (p(i) == 43 .or. p(i) == 45) i = i + 1
        end if
        dgt = 0
        do while (i < n)
          if (.not. is_digit(ub(p(i)))) exit
          i = i + 1
          dgt = dgt + 1
        end do
        if (dgt == 0) return
      end if
    end if
    idx = i
  end function

  function skip_ws(p, n, start) result(i)
    integer(u8), intent(in) :: p(0:)
    integer, intent(in) :: n, start
    integer :: i
    i = start
    do while (i < n)
      if (p(i) /= 32 .and. p(i) /= 9 .and. p(i) /= 10 .and. p(i) /= 13) exit
      i = i + 1
    end do
  end function

  ! Validate any JSON value starting at p(start); returns the index after it or -1.
  recursive function scan_value(p, n, start, depth) result(idx)
    integer(u8), intent(in) :: p(0:)
    integer, intent(in) :: n, start, depth
    integer :: idx, i, c, dummy
    integer(u8) :: nodst(0:0)
    idx = -1
    if (depth > MAX_DEPTH) return
    i = skip_ws(p, n, start)
    if (i >= n) return
    c = ub(p(i))
    if (c == 34) then
      idx = scan_string(p, n, i, .false., nodst, 0, dummy)
      return
    end if
    if (c == 123) then
      i = skip_ws(p, n, i + 1)
      if (i < n) then
        if (p(i) == 125) then
          idx = i + 1
          return
        end if
      end if
      do
        i = skip_ws(p, n, i)
        if (i >= n) return
        if (p(i) /= 34) return
        i = scan_string(p, n, i, .false., nodst, 0, dummy)
        if (i < 0) return
        i = skip_ws(p, n, i)
        if (i >= n) return
        if (p(i) /= 58) return
        i = scan_value(p, n, i + 1, depth + 1)
        if (i < 0) return
        i = skip_ws(p, n, i)
        if (i >= n) return
        if (p(i) == 44) then
          i = i + 1
          cycle
        end if
        if (p(i) == 125) then
          idx = i + 1
          return
        end if
        return
      end do
    end if
    if (c == 91) then
      i = skip_ws(p, n, i + 1)
      if (i < n) then
        if (p(i) == 93) then
          idx = i + 1
          return
        end if
      end if
      do
        i = scan_value(p, n, i, depth + 1)
        if (i < 0) return
        i = skip_ws(p, n, i)
        if (i >= n) return
        if (p(i) == 44) then
          i = i + 1
          cycle
        end if
        if (p(i) == 93) then
          idx = i + 1
          return
        end if
        return
      end do
    end if
    if (c == 116) then
      if (i + 4 <= n) then
        if (p(i + 1) == 114 .and. p(i + 2) == 117 .and. p(i + 3) == 101) idx = i + 4
      end if
      return
    end if
    if (c == 102) then
      if (i + 5 <= n) then
        if (p(i + 1) == 97 .and. p(i + 2) == 108 .and. p(i + 3) == 115 .and. p(i + 4) == 101) idx = i + 5
      end if
      return
    end if
    if (c == 110) then
      if (i + 4 <= n) then
        if (p(i + 1) == 117 .and. p(i + 2) == 108 .and. p(i + 3) == 108) idx = i + 4
      end if
      return
    end if
    idx = scan_number(p, n, i)
  end function

  subroutine jscan_init(s, a, n)
    type(jscan_t), intent(out) :: s
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    s%a = a
    s%n = n
    s%vstart = -1
    s%vend = -1
  end subroutine

  ! True if the document is an object containing key (last occurrence wins).
  function find_key(s, key) result(found)
    type(jscan_t), intent(inout) :: s
    character(len=*), intent(in) :: key
    logical :: found
    integer(u8), pointer, contiguous :: p(:)
    integer :: i, kstart, klen, vs, dummy
    integer(u8) :: nodst(0:0)
    found = .false.
    call bytes_at(s%a, s%n, p)
    i = skip_ws(p, s%n, 0)
    if (i >= s%n) return
    if (p(i) /= 123) return
    i = skip_ws(p, s%n, i + 1)
    if (i < s%n) then
      if (p(i) == 125) return
    end if
    do
      i = skip_ws(p, s%n, i)
      if (i >= s%n) then
        found = .false.
        return
      end if
      if (p(i) /= 34) then
        found = .false.
        return
      end if
      kstart = i + 1
      i = scan_string(p, s%n, i, .false., nodst, 0, dummy)
      if (i < 0) then
        found = .false.
        return
      end if
      klen = i - 1 - kstart
      i = skip_ws(p, s%n, i)
      if (i >= s%n) then
        found = .false.
        return
      end if
      if (p(i) /= 58) then
        found = .false.
        return
      end if
      vs = skip_ws(p, s%n, i + 1)
      i = scan_value(p, s%n, vs, 1)
      if (i < 0) then
        found = .false.
        return
      end if
      if (eqlit(s%a + kstart, klen, key)) then
        s%vstart = vs
        s%vend = i
        found = .true.
      end if
      i = skip_ws(p, s%n, i)
      if (i >= s%n) then
        found = .false.
        return
      end if
      if (p(i) == 44) then
        i = i + 1
        cycle
      end if
      if (p(i) == 125) then
        ! the object must be the whole document
        found = found .and. skip_ws(p, s%n, i + 1) == s%n
        return
      end if
      found = .false.
      return
    end do
  end function

  function value_is_string(s) result(yes)
    type(jscan_t), intent(in) :: s
    logical :: yes
    integer(u8), pointer, contiguous :: p(:)
    yes = .false.
    if (s%vstart < 0) return
    call bytes_at(s%a, s%n, p)
    yes = p(s%vstart) == 34
  end function

  ! Raw (still escaped) string contents.
  function string_raw_ptr(s) result(a)
    type(jscan_t), intent(in) :: s
    integer(ip) :: a
    a = s%a + s%vstart + 1
  end function

  function string_raw_len(s) result(n)
    type(jscan_t), intent(in) :: s
    integer :: n
    n = s%vend - s%vstart - 2
  end function

  function string_unescape(s, dst, cap) result(olen)
    type(jscan_t), intent(in) :: s
    integer(ip), intent(in) :: dst
    integer, intent(in) :: cap
    integer :: olen
    integer(u8), pointer, contiguous :: p(:), d(:)
    integer :: r
    call bytes_at(s%a, s%n, p)
    call bytes_at(dst, cap, d)
    r = scan_string(p, s%n, s%vstart, .true., d, cap, olen)
    if (r < 0) olen = -1
  end function

  ! Integer part of a JSON number value (floors), -1 if not a non-negative number.
  function number_int(s) result(v)
    type(jscan_t), intent(in) :: s
    integer(i8) :: v
    integer(u8), pointer, contiguous :: p(:)
    integer :: i, digits, e
    v = -1
    if (s%vstart < 0) return
    call bytes_at(s%a, s%n, p)
    i = s%vstart
    if (.not. is_digit(ub(p(i)))) return
    v = 0
    digits = 0
    do while (i < s%vend)
      if (.not. is_digit(ub(p(i)))) exit
      if (digits < 18) v = v * 10 + (ub(p(i)) - 48)
      digits = digits + 1
      i = i + 1
    end do
    if (i < s%vend) then
      e = ub(p(i))
      if (e == 101 .or. e == 69) v = -1   ! exponent form: treat as invalid
    end if
  end function

  ! Validate the whole document, extract and trim "body". On BODY_OK the trimmed
  ! UTF-8 bytes are at dst and out_len is their length.
  function validate_body(a, n, dst, cap, out_len) result(code)
    integer(ip), intent(in) :: a, dst
    integer, intent(in) :: n, cap
    integer, intent(out) :: out_len
    integer :: code
    integer(u8), pointer, contiguous :: p(:), d(:)
    type(jscan_t) :: s
    integer :: e, raw, lo, hi, chars, i
    out_len = 0
    call bytes_at(a, n, p)
    e = scan_value(p, n, 0, 0)
    code = BODY_MALFORMED
    if (e < 0) return
    if (skip_ws(p, n, e) /= n) return
    call jscan_init(s, a, n)
    code = BODY_REQUIRED
    if (.not. find_key(s, 'body')) return
    if (.not. value_is_string(s)) return
    raw = string_unescape(s, dst, cap)
    code = BODY_MALFORMED
    if (raw < 0) return
    call bytes_at(dst, cap, d)
    lo = 0
    hi = raw
    do while (lo < hi)
      if (.not. is_ws(ub(d(lo)))) exit
      lo = lo + 1
    end do
    do while (hi > lo)
      if (.not. is_ws(ub(d(hi - 1)))) exit
      hi = hi - 1
    end do
    code = BODY_REQUIRED
    if (lo == hi) return
    chars = 0
    do i = lo, hi - 1
      if (iand(ub(d(i)), 192) /= 128) chars = chars + 1
    end do
    code = BODY_TOO_LONG
    if (chars > MAX_BODY_CHARS) return
    if (lo > 0) then
      do i = 0, hi - lo - 1
        d(i) = d(lo + i)
      end do
    end if
    out_len = hi - lo
    code = BODY_OK
  end function

end module json
