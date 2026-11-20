! SHA-256 and HMAC-SHA256, written here so JWT verification is Fortran end to end.
! The HMAC keeps the inner and outer pad states hashed once, so verifying a token
! costs the message blocks plus one outer block. No allocation per call. 32-bit
! words are integer(4) with wrap-around arithmetic (-fwrapv); shifts are logical.
module sha256
  use bytes
  implicit none
  private
  public :: sha_init, sha_hash, sha_set_key, sha_hmac

  integer(i4), parameter :: K(0:63) = [ &
    int(z'428A2F98', i4), int(z'71374491', i4), int(z'B5C0FBCF', i4), int(z'E9B5DBA5', i4), &
    int(z'3956C25B', i4), int(z'59F111F1', i4), int(z'923F82A4', i4), int(z'AB1C5ED5', i4), &
    int(z'D807AA98', i4), int(z'12835B01', i4), int(z'243185BE', i4), int(z'550C7DC3', i4), &
    int(z'72BE5D74', i4), int(z'80DEB1FE', i4), int(z'9BDC06A7', i4), int(z'C19BF174', i4), &
    int(z'E49B69C1', i4), int(z'EFBE4786', i4), int(z'0FC19DC6', i4), int(z'240CA1CC', i4), &
    int(z'2DE92C6F', i4), int(z'4A7484AA', i4), int(z'5CB0A9DC', i4), int(z'76F988DA', i4), &
    int(z'983E5152', i4), int(z'A831C66D', i4), int(z'B00327C8', i4), int(z'BF597FC7', i4), &
    int(z'C6E00BF3', i4), int(z'D5A79147', i4), int(z'06CA6351', i4), int(z'14292967', i4), &
    int(z'27B70A85', i4), int(z'2E1B2138', i4), int(z'4D2C6DFC', i4), int(z'53380D13', i4), &
    int(z'650A7354', i4), int(z'766A0ABB', i4), int(z'81C2C92E', i4), int(z'92722C85', i4), &
    int(z'A2BFE8A1', i4), int(z'A81A664B', i4), int(z'C24B8B70', i4), int(z'C76C51A3', i4), &
    int(z'D192E819', i4), int(z'D6990624', i4), int(z'F40E3585', i4), int(z'106AA070', i4), &
    int(z'19A4C116', i4), int(z'1E376C08', i4), int(z'2748774C', i4), int(z'34B0BCB5', i4), &
    int(z'391C0CB3', i4), int(z'4ED8AA4A', i4), int(z'5B9CCA4F', i4), int(z'682E6FF3', i4), &
    int(z'748F82EE', i4), int(z'78A5636F', i4), int(z'84C87814', i4), int(z'8CC70208', i4), &
    int(z'90BEFFFA', i4), int(z'A4506CEB', i4), int(z'BEF9A3F7', i4), int(z'C67178F2', i4)]

  integer(i4), parameter :: H0(0:7) = [ &
    int(z'6A09E667', i4), int(z'BB67AE85', i4), int(z'3C6EF372', i4), int(z'A54FF53A', i4), &
    int(z'510E527F', i4), int(z'9B05688C', i4), int(z'1F83D9AB', i4), int(z'5BE0CD19', i4)]

  integer(i4) :: w(0:63), state(0:7)
  integer(i4) :: inner(0:7), outer(0:7)   ! HMAC: states after key^ipad and key^opad
  integer(ip) :: blk = 0                   ! 128 bytes: the padded tail of a message
  integer(ip) :: ih = 0                    ! 32 bytes: HMAC inner hash

contains

  subroutine sha_init()
    blk = balloc(128)
    ih = balloc(32)
  end subroutine

  pure function rotr(x, n) result(r)
    integer(i4), intent(in) :: x
    integer, intent(in) :: n
    integer(i4) :: r
    r = ior(shiftr(x, n), shiftl(x, 32 - n))
  end function

  pure function load_be32(p, i) result(v)
    integer(u8), intent(in) :: p(0:)
    integer, intent(in) :: i
    integer(i4) :: v
    v = ior(ior(shiftl(int(ub(p(i)), i4), 24), shiftl(int(ub(p(i + 1)), i4), 16)), &
            ior(shiftl(int(ub(p(i + 2)), i4), 8), int(ub(p(i + 3)), i4)))
  end function

  subroutine store_be32(p, i, v)
    integer(u8), intent(inout) :: p(0:)
    integer, intent(in) :: i
    integer(i4), intent(in) :: v
    p(i) = tob(int(iand(shiftr(v, 24), 255_i4)))
    p(i + 1) = tob(int(iand(shiftr(v, 16), 255_i4)))
    p(i + 2) = tob(int(iand(shiftr(v, 8), 255_i4)))
    p(i + 3) = tob(int(iand(v, 255_i4)))
  end subroutine

  ! One compression round: st (8 words) absorbs the 64 bytes at p(off:).
  subroutine compress(st, p, off)
    integer(i4), intent(inout) :: st(0:7)
    integer(u8), intent(in) :: p(0:)
    integer, intent(in) :: off
    integer(i4) :: a, b, c, d, e, f, g, h, s0, s1, ch, t1, t2, maj, w15, w2
    integer :: i
    do i = 0, 15
      w(i) = load_be32(p, off + i * 4)
    end do
    do i = 16, 63
      w15 = w(i - 15)
      w2 = w(i - 2)
      s0 = ieor(ieor(rotr(w15, 7), rotr(w15, 18)), shiftr(w15, 3))
      s1 = ieor(ieor(rotr(w2, 17), rotr(w2, 19)), shiftr(w2, 10))
      w(i) = w(i - 16) + s0 + w(i - 7) + s1
    end do
    a = st(0); b = st(1); c = st(2); d = st(3)
    e = st(4); f = st(5); g = st(6); h = st(7)
    do i = 0, 63
      s1 = ieor(ieor(rotr(e, 6), rotr(e, 11)), rotr(e, 25))
      ch = ieor(iand(e, f), iand(not(e), g))
      t1 = h + s1 + ch + K(i) + w(i)
      s0 = ieor(ieor(rotr(a, 2), rotr(a, 13)), rotr(a, 22))
      maj = ieor(ieor(iand(a, b), iand(a, c)), iand(b, c))
      t2 = s0 + maj
      h = g; g = f; f = e
      e = d + t1
      d = c; c = b; b = a
      a = t1 + t2
    end do
    st(0) = st(0) + a; st(1) = st(1) + b; st(2) = st(2) + c; st(3) = st(3) + d
    st(4) = st(4) + e; st(5) = st(5) + f; st(6) = st(6) + g; st(7) = st(7) + h
  end subroutine

  ! Hash the n bytes at msg continuing from `start` (which already absorbed
  ! prefix_blocks blocks) into the 32 bytes at dst.
  subroutine finish(start, prefix_blocks, msg, n, dst)
    integer(i4), intent(in) :: start(0:7)
    integer, intent(in) :: prefix_blocks, n
    integer(ip), intent(in) :: msg, dst
    integer(u8), pointer, contiguous :: m(:), b(:), d(:)
    integer :: off, rem, last, i
    integer(i8) :: total_bits
    state = start
    call bytes_at(msg, max(n, 1), m)
    off = 0
    do while (n - off >= 64)
      call compress(state, m, off)
      off = off + 64
    end do
    rem = n - off
    call bzero(blk, 128)
    call bcopy(blk, msg + off, rem)
    call bytes_at(blk, 128, b)
    b(rem) = tob(128)
    total_bits = int(prefix_blocks * 64 + n, i8) * 8
    if (rem < 56) then
      last = 64
    else
      last = 128
    end if
    do i = 0, 7
      b(last - 1 - i) = tob(int(iand(shiftr(total_bits, i * 8), 255_i8)))
    end do
    call compress(state, b, 0)
    if (last == 128) call compress(state, b, 64)
    call bytes_at(dst, 32, d)
    do i = 0, 7
      call store_be32(d, i * 4, state(i))
    end do
  end subroutine

  subroutine sha_hash(msg, n, dst)
    integer(ip), intent(in) :: msg, dst
    integer, intent(in) :: n
    call finish(H0, 0, msg, n, dst)
  end subroutine

  ! The HMAC pad block: key (zero-padded, or hashed if longer than a block) xor x.
  subroutine pad_state(key, n, x, st)
    integer(ip), intent(in) :: key
    integer, intent(in) :: n, x
    integer(i4), intent(out) :: st(0:7)
    integer(ip) :: kb
    integer(u8), pointer, contiguous :: k(:)
    integer :: i
    kb = balloc(64)
    call bzero(kb, 64)
    if (n > 64) then
      call sha_hash(key, n, kb)
    else
      call bcopy(kb, key, n)
    end if
    call bytes_at(kb, 64, k)
    do i = 0, 63
      k(i) = tob(ieor(ub(k(i)), x))
    end do
    st = H0
    call compress(st, k, 0)
  end subroutine

  subroutine sha_set_key(key, n)
    integer(ip), intent(in) :: key
    integer, intent(in) :: n
    call pad_state(key, n, 54, inner)    ! 0x36
    call pad_state(key, n, 92, outer)    ! 0x5C
  end subroutine

  ! HMAC-SHA256(key, msg) -> the 32 bytes at dst.
  subroutine sha_hmac(msg, n, dst)
    integer(ip), intent(in) :: msg, dst
    integer, intent(in) :: n
    call finish(inner, 1, msg, n, ih)
    call finish(outer, 1, ih, 32, dst)
  end subroutine

end module sha256
