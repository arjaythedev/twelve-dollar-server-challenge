! SQLite record format: header of varint serial types, then the values.
! https://www.sqlite.org/fileformat2.html#record_format
module record
  use bytes
  implicit none
  private
  public :: rec_t, rec_decode, rec_int, rec_ptr, rec_len, varint

  ! Decoded positions of the first four columns of one record.
  type :: rec_t
    integer(ip) :: a = 0
    integer :: ncols = 0
    integer(i8) :: t(0:3) = 0
    integer :: o(0:3) = 0
  end type

contains

  function varint(p, i) result(v)
    integer(u8), intent(in) :: p(0:)
    integer, intent(inout) :: i
    integer(i8) :: v
    integer :: k, b
    v = 0
    do k = 1, 8
      b = ub(p(i))
      i = i + 1
      v = ior(shiftl(v, 7), int(iand(b, 127), i8))
      if (b < 128) return
    end do
    v = ior(shiftl(v, 8), int(ub(p(i)), i8))
    i = i + 1
  end function

  pure function serial_len(t) result(n)
    integer(i8), intent(in) :: t
    integer :: n
    if (t < 0) then
      n = 0                        ! a serial type past 2^63: only a corrupt record
    else if (t >= 12) then
      n = int(shiftr(t - 12, 1))
    else if (t == 0 .or. t >= 8) then
      n = 0
    else if (t <= 4) then
      n = int(t)
    else if (t == 5) then
      n = 6
    else
      n = 8
    end if
  end function

  function read_int(a, off, t) result(v)
    integer(ip), intent(in) :: a
    integer, intent(in) :: off
    integer(i8), intent(in) :: t
    integer(i8) :: v
    integer(u8), pointer, contiguous :: p(:)
    integer :: n, k, bits
    if (t == 8) then
      v = 0
      return
    end if
    if (t == 9) then
      v = 1
      return
    end if
    n = serial_len(t)
    v = 0
    if (n == 0) return
    call bytes_at(a + off, n, p)
    do k = 0, n - 1
      v = ior(shiftl(v, 8), int(ub(p(k)), i8))
    end do
    bits = n * 8
    if (bits < 64) then
      if (iand(shiftr(v, bits - 1), 1_i8) == 1) v = v - shiftl(1_i8, bits)
    end if
  end function

  ! Decode up to 4 columns of the record at a.
  subroutine rec_decode(r, a)
    type(rec_t), intent(inout) :: r
    integer(ip), intent(in) :: a
    integer(u8), pointer, contiguous :: p(:)
    integer :: i, hdr, off, n
    integer(i8) :: t
    r%a = a
    call bytes_at(a, 65536, p)
    i = 0
    hdr = int(varint(p, i))
    off = hdr
    n = 0
    do while (i < hdr .and. n < 4)
      t = varint(p, i)
      r%t(n) = t
      r%o(n) = off
      off = off + serial_len(t)
      n = n + 1
    end do
    r%ncols = n
  end subroutine

  function rec_int(r, col) result(v)
    type(rec_t), intent(in) :: r
    integer, intent(in) :: col
    integer(i8) :: v
    v = read_int(r%a, r%o(col), r%t(col))
  end function

  function rec_ptr(r, col) result(a)
    type(rec_t), intent(in) :: r
    integer, intent(in) :: col
    integer(ip) :: a
    a = r%a + r%o(col)
  end function

  function rec_len(r, col) result(n)
    type(rec_t), intent(in) :: r
    integer, intent(in) :: col
    integer :: n
    n = serial_len(r%t(col))
  end function

end module record
