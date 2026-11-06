! HTTP/1.1 request parsing: request line, the three headers the API needs,
! Content-Length body. Everything else is skipped.
module http
  use bytes
  implicit none
  private
  public :: req_t, parse_request, M_OTHER, M_GET, M_POST, P_INCOMPLETE, P_COMPLETE, P_BAD

  integer, parameter :: M_OTHER = 0, M_GET = 1, M_POST = 2
  integer, parameter :: P_INCOMPLETE = 0, P_COMPLETE = 1, P_BAD = 2

  type :: req_t
    integer :: status = P_INCOMPLETE
    integer :: method = M_OTHER
    integer(ip) :: path = 0
    integer :: path_len = 0
    logical :: auth_present = .false.   ! any Authorization header
    logical :: bearer = .false.         ! it starts with "Bearer "
    integer(ip) :: tok = 0
    integer :: tok_len = 0
    integer(ip) :: body = 0
    integer :: body_len = 0
    logical :: wclose = .false.
    integer :: consumed = 0             ! total bytes of this request
  end type

contains

  subroutine parse_request(a, n, r)
    integer(ip), intent(in) :: a
    integer, intent(in) :: n
    type(req_t), intent(inout) :: r
    integer(u8), pointer, contiguous :: p(:)
    integer :: eol, sp1, sp2, qm, vlen, i, e, colon, k, name_len, vs, ve, ts
    integer(i8) :: content_length
    logical :: http10
    r%status = P_INCOMPLETE
    eol = find_crlf(a, n, 0)
    if (eol < 0) then
      if (n > 8192) r%status = P_BAD
      return
    end if
    call bytes_at(a, n, p)
    sp1 = find_byte(a, eol, 0, 32)
    sp2 = -1
    if (sp1 >= 0) sp2 = find_byte(a, eol, sp1 + 1, 32)
    if (sp1 < 0 .or. sp2 < 0) then
      r%status = P_BAD
      return
    end if
    if (eqlit(a, sp1, 'GET')) then
      r%method = M_GET
    else if (eqlit(a, sp1, 'POST')) then
      r%method = M_POST
    else
      r%method = M_OTHER
    end if
    r%path = a + sp1 + 1
    r%path_len = sp2 - sp1 - 1
    ! drop a query string
    qm = find_byte(r%path, r%path_len, 0, 63)
    if (qm >= 0) r%path_len = qm
    vlen = eol - sp2 - 1
    http10 = eqlit(a + sp2 + 1, vlen, 'HTTP/1.0')
    if (.not. http10) then
      if (.not. eqlit(a + sp2 + 1, vlen, 'HTTP/1.1')) then
        r%status = P_BAD
        return
      end if
    end if
    r%wclose = http10
    r%auth_present = .false.
    r%bearer = .false.
    r%body_len = 0
    content_length = 0
    i = eol + 2
    do
      e = find_crlf(a, n, i)
      if (e < 0) then
        if (n > 8192) r%status = P_BAD
        return
      end if
      if (e == i) then
        i = i + 2
        exit
      end if
      colon = -1
      do k = i, e - 1
        if (p(k) == 58) then
          colon = k
          exit
        end if
      end do
      if (colon < 0) then
        r%status = P_BAD
        return
      end if
      name_len = colon - i
      vs = colon + 1
      do while (vs < e)
        if (p(vs) /= 32 .and. p(vs) /= 9) exit
        vs = vs + 1
      end do
      ve = e
      do while (ve > vs)
        if (p(ve - 1) /= 32 .and. p(ve - 1) /= 9) exit
        ve = ve - 1
      end do
      if (name_len == 13 .and. eqlit_ci(a + i, 13, 'authorization')) then
        r%auth_present = .true.
        if (ve - vs >= 7) then
          if (eqlit_ci(a + vs, 7, 'bearer ')) then
            r%bearer = .true.
            ts = vs + 7
            do while (ts < ve)
              if (p(ts) /= 32) exit
              ts = ts + 1
            end do
            r%tok = a + ts
            r%tok_len = ve - ts
          end if
        end if
      else if (name_len == 14 .and. eqlit_ci(a + i, 14, 'content-length')) then
        content_length = parse_uint(a + vs, ve - vs)
        if (content_length < 0) then
          r%status = P_BAD
          return
        end if
      else if (name_len == 10 .and. eqlit_ci(a + i, 10, 'connection')) then
        if (eqlit_ci(a + vs, ve - vs, 'close')) then
          r%wclose = .true.
        else if (eqlit_ci(a + vs, ve - vs, 'keep-alive')) then
          r%wclose = .false.
        end if
      else if (name_len == 17 .and. eqlit_ci(a + i, 17, 'transfer-encoding')) then
        r%status = P_BAD
        return
      end if
      i = e + 2
    end do
    if (content_length > 16384) then
      r%status = P_BAD
      return
    end if
    if (n - i < int(content_length)) return
    r%body = a + i
    r%body_len = int(content_length)
    r%consumed = i + int(content_length)
    r%status = P_COMPLETE
  end subroutine

end module http
