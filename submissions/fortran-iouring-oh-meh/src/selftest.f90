! Unit checks for the leaf modules, built and run by build.sh: SHA-256 and HMAC
! against the FIPS and RFC 4231 vectors, a JWT signed here and verified, the body
! validator, the request parser and the writer. Exit status 1 on any failure.
program selftest
  use bytes
  use sha256
  use json
  use jwt
  use http
  implicit none
  integer :: fails = 0
  integer(ip) :: dst, msg
  character(len=:), allocatable :: tok, q
  integer :: n, rc, olen
  type(req_t) :: r
  type(out_t) :: o

  dst = balloc(1024)
  call sha_init()
  call sha_hash(lit(''), 0, dst)
  call check('sha256("")', hex(dst, 32) == 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855')
  call sha_hash(lit('abc'), 3, dst)
  call check('sha256("abc")', hex(dst, 32) == 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
  call sha_hash(lit('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'), 56, dst)
  call check('sha256(56 bytes, two blocks)', hex(dst, 32) == '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1')
  call sha_set_key(lit('Jefe'), 4)
  call sha_hmac(lit('what do ya want for nothing?'), 28, dst)
  call check('hmac rfc 4231 #2', hex(dst, 32) == '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843')

  call jwt_init(lit('twelve-dollar-challenge'), 23)
  tok = sign('{"alg":"HS256","typ":"JWT"}', '{"sub":"1","username":"golden_ember_1","iat":1700000000,"exp":1700003600}')
  rc = jwt_verify(lit(tok), len(tok), 1700000001_i8)
  call check('jwt ok', rc == JWT_OK .and. jwt_user_id == 1 .and. eqlit(jwt_name_ptr, jwt_name_len, 'golden_ember_1'))
  rc = jwt_verify(lit(tok), len(tok), 1700003600_i8)
  call check('jwt expired', rc == JWT_INVALID)
  tok(len(tok):len(tok)) = merge('B', 'A', tok(len(tok):len(tok)) == 'A')
  rc = jwt_verify(lit(tok), len(tok), 1700000001_i8)
  call check('jwt bad signature', rc == JWT_INVALID)
  tok = sign('{"alg":"HS256"}', '{"sub":"abc","username":"x","exp":1700003600}')
  call check('jwt sub not an integer', jwt_verify(lit(tok), len(tok), 1700000001_i8) == JWT_PAYLOAD)
  tok = sign('{"alg":"none"}', '{"sub":"1","username":"x","exp":1700003600}')
  call check('jwt alg none', jwt_verify(lit(tok), len(tok), 1700000001_i8) == JWT_INVALID)
  call check('jwt garbage', jwt_verify(lit('x.y'), 3, 1700000001_i8) == JWT_INVALID)

  rc = body('{"body":"  hi  "}')
  call check('body trimmed', rc == BODY_OK .and. olen == 2 .and. eqlit(dst, 2, 'hi'))
  rc = body('{"body":"   "}')
  call check('body empty', rc == BODY_REQUIRED)
  rc = body('{"body":123}')
  call check('body numeric', rc == BODY_REQUIRED)
  rc = body('{"x":"y"}')
  call check('body missing', rc == BODY_REQUIRED)
  rc = body('{"body":"')
  call check('body malformed', rc == BODY_MALFORMED)
  rc = body('{"body":"a"} x')
  call check('body trailing junk', rc == BODY_MALFORMED)
  rc = body('{"body":"\u00e9x"}')
  call check('body unicode escape', rc == BODY_OK .and. olen == 3 .and. hex(dst, 3) == 'c3a978')
  msg = balloc(600)
  call out_init(o, msg, 600)
  call put_str(o, '{"body":"')
  do n = 1, 501
    call put(o, 97)
  end do
  call put_str(o, '"}')
  call check('body 501 chars', validate_body(msg, o%n, dst, 1024, olen) == BODY_TOO_LONG)

  q = 'GET /feed?x=1 HTTP/1.1' // crlf() // 'Host: x' // crlf() // crlf()
  call parse_request(lit(q), len(q), r)
  call check('parse GET', r%status == P_COMPLETE .and. r%method == M_GET .and. eqlit(r%path, r%path_len, '/feed') &
    .and. r%consumed == len(q) .and. .not. r%wclose)
  q = 'POST /posts HTTP/1.0' // crlf() // 'Authorization: Bearer abc' // crlf() // 'Content-Length: 4' // crlf() // crlf() // 'abcd'
  call parse_request(lit(q), len(q), r)
  call check('parse POST body', r%status == P_COMPLETE .and. r%method == M_POST .and. r%bearer &
    .and. eqlit(r%tok, r%tok_len, 'abc') .and. r%body_len == 4 .and. eqlit(r%body, 4, 'abcd') .and. r%consumed == len(q) &
    .and. r%wclose)
  q = 'GET /feed HTTP/1.1' // crlf() // 'Host'
  call parse_request(lit(q), len(q), r)
  call check('parse incomplete', r%status == P_INCOMPLETE)
  q = 'GET /feed HTTP/1.1' // crlf() // 'Transfer-Encoding: chunked' // crlf() // crlf()
  call parse_request(lit(q), len(q), r)
  call check('parse chunked rejected', r%status == P_BAD)

  call out_init(o, msg, 600)
  call put_int(o, -1234567890123_i8)
  call put(o, 32)
  call put_int(o, 0_i8)
  call put(o, 32)
  call put_json(o, lit('a"b\c' // achar(10) // achar(1)), 7)
  call check('writer', eqlit(msg, o%n, '-1234567890123 0 a\"b\\c\n\u0001'))

  if (fails > 0) then
    print '(A,I0,A)', 'selftest: ', fails, ' FAILED'
    error stop 1
  end if
  print '(A)', 'selftest: all checks passed'

contains

  function body(s) result(code)
    character(len=*), intent(in) :: s
    integer :: code
    code = validate_body(lit(s), len(s), dst, 1024, olen)
  end function

  function lit(s) result(a)
    character(len=*), intent(in) :: s
    integer(ip) :: a
    type(out_t) :: w
    a = balloc(len(s) + 1)
    call out_init(w, a, len(s) + 1)
    call put_str(w, s)
  end function

  function crlf() result(s)
    character(len=2) :: s
    s = achar(13) // achar(10)
  end function

  function hex(a, n) result(s)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    character(len=2 * n) :: s
    integer(u8), pointer, contiguous :: p(:)
    integer :: i
    call bytes_at(a, n, p)
    do i = 0, n - 1
      s(2 * i + 1:2 * i + 1) = achar(hex_digit(shiftr(ub(p(i)), 4)))
      s(2 * i + 2:2 * i + 2) = achar(hex_digit(iand(ub(p(i)), 15)))
    end do
  end function

  function b64url(a, n) result(s)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    character(len=:), allocatable :: s
    character(len=64), parameter :: tbl = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
    integer(u8), pointer, contiguous :: p(:)
    integer :: i, v, bits
    integer(i8) :: acc
    call bytes_at(a, n, p)
    s = ''
    acc = 0
    bits = 0
    do i = 0, n - 1
      acc = ior(shiftl(acc, 8), int(ub(p(i)), i8))
      bits = bits + 8
      do while (bits >= 6)
        bits = bits - 6
        v = int(iand(shiftr(acc, bits), 63_i8))
        s = s // tbl(v + 1:v + 1)
      end do
    end do
    if (bits > 0) then
      v = int(iand(shiftl(acc, 6 - bits), 63_i8))
      s = s // tbl(v + 1:v + 1)
    end if
  end function

  ! A token signed with the key jwt_init installed (the test secret).
  function sign(header, payload) result(t)
    character(len=*), intent(in) :: header, payload
    character(len=:), allocatable :: t, hp
    integer(ip) :: mac
    hp = b64url(lit(header), len(header)) // '.' // b64url(lit(payload), len(payload))
    mac = balloc(32)
    call sha_hmac(lit(hp), len(hp), mac)
    t = hp // '.' // b64url(mac, 32)
  end function

  subroutine check(name, ok)
    character(len=*), intent(in) :: name
    logical, intent(in) :: ok
    if (ok) then
      print '(A,A)', '  ok   ', name
    else
      print '(A,A)', '  FAIL ', name
      fails = fails + 1
    end if
  end subroutine

end program selftest
