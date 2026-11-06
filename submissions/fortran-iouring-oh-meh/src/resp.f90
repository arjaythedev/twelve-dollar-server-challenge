! Response arenas. A batch writes its responses back to back into an arena; each
! send points into it (no copy) and holds a reference until its completion
! arrives. An arena is reused once nothing in flight points into it.
module resp
  use bytes
  implicit none
  private
  public :: resp_t, arenas_init, arena_begin, arena_finish, arena_release, const_resp
  public :: POST_MAX, FEED_MAX, CREATE_MAX

  integer, parameter :: HDR_RESERVE = 128
  integer, parameter :: ARENA_SIZE = 1024 * 1024
  ! Upper bounds for the variable-size responses, so an arena is reserved tightly:
  ! a post object is at most ~3 KB (500 code points escaped as \u00XX) plus a
  ! username; the feed is 20 of them; a created post echoes a token username.
  integer, parameter :: POST_MAX = 32 * 1024, FEED_MAX = 96 * 1024, CREATE_MAX = 64 * 1024

  type :: arena_t
    integer(ip) :: base = 0
    integer :: used = 0
    integer :: inflight = 0
  end type

  type :: resp_t
    integer(ip) :: ptr = 0     ! address of the first byte to send
    integer :: len = 0
    integer :: arena = -1      ! index, or -1 for a constant response
  end type

  type(arena_t), allocatable :: arenas(:)
  integer :: n_arenas = 0, cur = 0
  integer(ip) :: hdr_scratch = 0

contains

  subroutine put_status_line(o, status)
    type(out_t), intent(inout) :: o
    integer, intent(in) :: status
    select case (status)
    case (200)
      call put_str(o, 'HTTP/1.1 200 OK')
    case (201)
      call put_str(o, 'HTTP/1.1 201 Created')
    case (400)
      call put_str(o, 'HTTP/1.1 400 Bad Request')
    case (401)
      call put_str(o, 'HTTP/1.1 401 Unauthorized')
    case (404)
      call put_str(o, 'HTTP/1.1 404 Not Found')
    case (413)
      call put_str(o, 'HTTP/1.1 413 Content Too Large')
    case (503)
      call put_str(o, 'HTTP/1.1 503 Service Unavailable')
    case default
      call put_str(o, 'HTTP/1.1 500 Internal Server Error')
    end select
    call put_str(o, achar(13) // achar(10))
  end subroutine

  ! Write the header for a body of body_len ending exactly at base + body_start.
  ! Returns the header's start offset. scratch is HDR_RESERVE bytes of workspace.
  function write_header(base, body_start, status, body_len, scratch) result(start)
    integer(ip), intent(in) :: base, scratch
    integer, intent(in) :: body_start, status, body_len
    integer :: start
    type(out_t) :: tmp
    call out_init(tmp, scratch, HDR_RESERVE)
    call put_status_line(tmp, status)
    call put_str(tmp, 'Content-Type: application/json' // achar(13) // achar(10) // 'Content-Length: ')
    call put_int(tmp, int(body_len, i8))
    call put_str(tmp, achar(13) // achar(10) // achar(13) // achar(10))
    start = body_start - tmp%n
    call bcopy(base + start, scratch, tmp%n)
  end function

  ! A constant response, built once, never freed.
  function const_resp(status, body) result(r)
    integer, intent(in) :: status
    character(len=*), intent(in) :: body
    type(resp_t) :: r
    integer(ip) :: base
    type(out_t) :: o
    integer :: n, start
    n = len(body)
    base = balloc(HDR_RESERVE + n)
    call out_init(o, base + HDR_RESERVE, n)
    call put_str(o, body)
    start = write_header(base, HDR_RESERVE, status, n, balloc(HDR_RESERVE))
    r%ptr = base + start
    r%len = HDR_RESERVE + n - start
    r%arena = -1
  end function

  subroutine arenas_init()
    allocate(arenas(16))
    n_arenas = 1
    arenas(1)%base = balloc(ARENA_SIZE)
    arenas(1)%used = 0
    arenas(1)%inflight = 0
    cur = 1
    hdr_scratch = balloc(HDR_RESERVE)
  end subroutine

  ! Make sure the current arena has room for need more bytes.
  subroutine ensure(need)
    integer, intent(in) :: need
    integer :: i
    type(arena_t), allocatable :: more(:)
    if (arenas(cur)%used + need <= ARENA_SIZE) return
    do i = 1, n_arenas
      if (i /= cur .and. arenas(i)%inflight == 0) then
        arenas(i)%used = 0
        cur = i
        return
      end if
    end do
    if (n_arenas == size(arenas)) then
      allocate(more(2 * size(arenas)))
      more(1:n_arenas) = arenas(1:n_arenas)
      call move_alloc(more, arenas)
    end if
    n_arenas = n_arenas + 1
    arenas(n_arenas)%base = balloc(ARENA_SIZE)
    arenas(n_arenas)%used = 0
    arenas(n_arenas)%inflight = 0
    cur = n_arenas
  end subroutine

  ! Start a response body: o is positioned at the body start.
  subroutine arena_begin(max_body, o)
    integer, intent(in) :: max_body
    type(out_t), intent(out) :: o
    call ensure(HDR_RESERVE + max_body)
    call out_init(o, arenas(cur)%base + arenas(cur)%used + HDR_RESERVE, max_body)
  end subroutine

  ! Finish it: write the header just before the body, account the send.
  function arena_finish(status, o) result(r)
    integer, intent(in) :: status
    type(out_t), intent(in) :: o
    type(resp_t) :: r
    integer :: body_start, start
    body_start = int(o%base - arenas(cur)%base)
    start = write_header(arenas(cur)%base, body_start, status, o%n, hdr_scratch)
    arenas(cur)%used = body_start + o%n
    arenas(cur)%inflight = arenas(cur)%inflight + 1
    r%ptr = arenas(cur)%base + start
    r%len = body_start + o%n - start
    r%arena = cur
  end function

  subroutine arena_release(arena)
    integer, intent(in) :: arena
    if (arena >= 1) arenas(arena)%inflight = arenas(arena)%inflight - 1
  end subroutine

end module resp
