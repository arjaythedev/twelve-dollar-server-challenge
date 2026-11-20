! JWT HS256 verification: base64url, signature check, header and payload checks.
! Result codes match SPEC.md's error table:
!   0 ok, 1 "invalid or expired token", 2 "invalid token payload"
module jwt
  use bytes
  use sha256
  use json
  implicit none
  private
  public :: jwt_init, jwt_verify, jwt_user_id, jwt_name_ptr, jwt_name_len
  public :: JWT_OK, JWT_INVALID, JWT_PAYLOAD

  integer, parameter :: JWT_OK = 0, JWT_INVALID = 1, JWT_PAYLOAD = 2
  integer, parameter :: MAX_TOKEN = 4096

  integer(ip) :: hdr = 0, pay = 0, sig = 0, mac = 0
  integer(i8) :: jwt_user_id = 0
  integer(ip) :: jwt_name_ptr = 0
  integer :: jwt_name_len = 0

contains

  pure function b64url_val(c) result(v)
    integer, intent(in) :: c
    integer :: v
    if (c >= 65 .and. c <= 90) then
      v = c - 65
    else if (c >= 97 .and. c <= 122) then
      v = c - 97 + 26
    else if (c >= 48 .and. c <= 57) then
      v = c - 48 + 52
    else if (c == 45) then
      v = 62
    else if (c == 95) then
      v = 63
    else
      v = -1
    end if
  end function

  ! Decode base64url (padding optional) into dst; returns the byte count or -1.
  function b64url_decode(src, n, dst) result(o)
    integer(ip), intent(in) :: src, dst
    integer, intent(in) :: n
    integer :: o
    integer(u8), pointer, contiguous :: s(:), d(:)
    integer :: m, i, v, bits
    integer(i8) :: acc
    o = -1
    if (n <= 0) then
      o = 0
      return
    end if
    call bytes_at(src, n, s)
    call bytes_at(dst, n, d)
    m = n
    do while (m > 0)
      if (s(m - 1) /= 61) exit
      m = m - 1
    end do
    o = 0
    acc = 0
    bits = 0
    do i = 0, m - 1
      v = b64url_val(ub(s(i)))
      if (v < 0) then
        o = -1
        return
      end if
      acc = ior(shiftl(acc, 6), int(v, i8))
      bits = bits + 6
      if (bits >= 8) then
        bits = bits - 8
        d(o) = tob(int(iand(shiftr(acc, bits), 255_i8)))
        o = o + 1
      end if
    end do
    if (mod(m, 4) == 1) o = -1
  end function

  subroutine jwt_init(secret, n)
    integer(ip), intent(in) :: secret
    integer, intent(in) :: n
    call sha_init()
    call sha_set_key(secret, n)
    hdr = balloc(MAX_TOKEN)
    pay = balloc(MAX_TOKEN)
    sig = balloc(MAX_TOKEN)
    mac = balloc(32)
    jwt_name_ptr = pay
  end subroutine

  function jwt_verify(tok, n, now) result(rc)
    integer(ip), intent(in) :: tok
    integer, intent(in) :: n
    integer(i8), intent(in) :: now
    integer :: rc
    integer :: d1, d2, sn, hn, pn, ulen, i, diff
    integer(u8), pointer, contiguous :: m(:), s(:)
    type(jscan_t) :: hs, ps, ss, us
    integer(i8) :: expiry, uid
    rc = JWT_INVALID
    if (n < 3 .or. n > MAX_TOKEN) return
    d1 = find_byte(tok, n, 0, 46)
    if (d1 <= 0) return
    d2 = find_byte(tok, n, d1 + 1, 46)
    if (d2 <= d1 + 1 .or. d2 >= n - 1) return
    if (find_byte(tok, n, d2 + 1, 46) >= 0) return
    ! Signature first: cheapest rejection of garbage, and never leak header checks on unsigned tokens.
    sn = b64url_decode(tok + d2 + 1, n - d2 - 1, sig)
    if (sn /= 32) return
    call sha_hmac(tok, d2, mac)
    call bytes_at(mac, 32, m)
    call bytes_at(sig, 32, s)
    diff = 0
    do i = 0, 31
      diff = ior(diff, ieor(ub(m(i)), ub(s(i))))
    end do
    if (diff /= 0) return
    ! Header: must be JSON with "alg":"HS256".
    hn = b64url_decode(tok, d1, hdr)
    if (hn < 0) return
    call jscan_init(hs, hdr, hn)
    if (.not. find_key(hs, 'alg')) return
    if (.not. value_is_string(hs)) return
    if (.not. eqlit(string_raw_ptr(hs), string_raw_len(hs), 'HS256')) return
    ! Payload: exp (number, > now), sub (positive integer string), username (string).
    pn = b64url_decode(tok + d1 + 1, d2 - d1 - 1, pay)
    if (pn < 0) return
    call jscan_init(ps, pay, pn)
    if (.not. find_key(ps, 'exp')) return
    expiry = number_int(ps)
    if (expiry < 0 .or. expiry <= now) return
    rc = JWT_PAYLOAD
    call jscan_init(ss, pay, pn)
    if (.not. find_key(ss, 'sub')) return
    if (.not. value_is_string(ss)) return
    uid = parse_pos_int(string_raw_ptr(ss), string_raw_len(ss))
    if (uid <= 0) return
    call jscan_init(us, pay, pn)
    if (.not. find_key(us, 'username')) return
    if (.not. value_is_string(us)) return
    ! Username is used as-is; take the unescaped form.
    ulen = string_unescape(us, sig, MAX_TOKEN)
    if (ulen < 0) return
    jwt_user_id = uid
    jwt_name_ptr = sig
    jwt_name_len = ulen
    rc = JWT_OK
  end function

end module jwt
