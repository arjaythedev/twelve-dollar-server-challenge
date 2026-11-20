! The server: one thread, one event loop over several io_uring rings.
!
! A batch is one wake-up: answer what earlier passes parked, reap every ring,
! run bounded reads for connections whose multishot receive ended, answer reads
! straight from the batch's SQLite transaction, queue writes, run them, COMMIT,
! then submit all sends. No write's response leaves before its batch commits.
!
! After a stall every connection has a request waiting and the ring's provided
! buffers run out at once; the receives that lost end with ENOBUFS. Those
! connections are not re-armed together (that only repeats the shortage, pass
! after pass, while the requests sit in 3-4 KB kernel socket buffers). Each goes
! on its ring's read queue and is read with one single-shot, non-waiting receive
! per pass, never more at once than the ring has buffers; a short read or EAGAIN
! means the socket is drained and the multishot receive is armed again. A pass
! reads up to READ_MAX requests but answers at most ANSWER_MAX, parking the rest
! in the connection's own buffer (a copy the size of the request) on a ready
! list that later passes serve oldest first, so sockets drain as fast as reads
! can run and replies go out while the backlog is still being read.
module srv
  use iso_c_binding
  use bytes
  use shim
  use resp
  use db
  use http
  use jwt
  use json
  implicit none
  private
  public :: srv_init, srv_run, table, max_rings, CKPT_INTERVAL_MS

  ! user_data layout: op(4) | ring(4) | gen(24) | slot(32). The 4-bit ring field is
  ! why RINGS_MAX is 16 here and in shim.c.
  integer, parameter :: OP_ACCEPT = 1, OP_RECV = 2, OP_SEND = 3, OP_CLOSE = 4
  integer, parameter :: OP_SHUTDOWN = 5   ! the shutdown linked before a close; its completion is ignored
  integer, parameter :: OP_READ = 6       ! a bounded single-shot read from the read queue
  integer, parameter :: RINGS_MAX = 16

  integer, parameter :: SQ_ENTRIES = 4096
  integer, parameter :: NBUFS_DEFAULT = 2048
  integer, parameter :: BUFSIZE = 2048
  integer, parameter :: CQ_BATCH = 4096
  integer, parameter :: TARGET_CONNS = 1000000
  integer, parameter :: PARTIAL_MIN = 512
  integer, parameter :: PARTIAL_MAX = 16384      ! one incomplete request may grow to this (413 past it)
  integer, parameter :: PARKED_MAX = 262144      ! complete requests parked by the answer cap may grow to this
  integer, parameter :: SCRATCH_CHUNK = 1048576
  ! Accepts in flight bound the accept rate (one is re-armed only after a pass sees it
  ! complete); 64 fell behind a 2,000-connection/s ramp once passes slowed at the memory
  ! wall, the listen queue overflowed and the rest waited out SYN retransmits. Eight
  ! SO_REUSEPORT listeners give eight listen queues (net.core.somaxconn caps each at
  ! 4,096) to hold a ramp or a slow pass; each listener keeps its own share of the
  ! accepts in flight, re-armed where a completion came from, so a listener with a
  ! queue is never left waiting for its turn.
  integer, parameter :: ACCEPT_DEPTH = 512
  integer, parameter :: LISTEN_BACKLOG = 4096
  integer, parameter :: LISTENERS = 8
  integer, parameter :: CKPT_INTERVAL_MS = 250
  ! Drain first: a pass reads up to READ_MAX times (over as many rounds as the
  ! buffers allow) but answers at most ANSWER_MAX requests; the rest wait parked.
  integer, parameter :: READ_MAX = 16384
  integer, parameter :: ANSWER_MAX = 2048
  ! Batch formation: after a pass that handled at least BUSY_MIN completions, the
  ! next wait asks for BATCH_WANT completions or BATCH_US, so one wake-up (and its
  ! BEGIN/COMMIT and submits) is shared by more requests. Costs up to BATCH_US of
  ! latency only while the server is busy.
  integer, parameter :: BUSY_MIN = 8
  integer, parameter :: BATCH_WANT = 32
  integer, parameter :: BATCH_US = 250
  ! While writes are held for a WAL restart the wait is bounded, so the hold ends
  ! (backfilled, or expired) even if no completion arrives to wake the loop.
  integer, parameter :: HOLD_POLL_US = 2000

  ! connection flags
  integer, parameter :: F_OPEN = 1, F_SENDING = 2, F_CLOSE_AFTER = 4, F_CLOSING = 8
  integer, parameter :: F_QUEUED = 16     ! on its ring's read queue (no receive armed)
  integer, parameter :: F_READY = 32      ! a parsed-but-unanswered request waits in `partial`
  integer, parameter :: F_DRAINED = 64    ! read dry by a bounded read; multishot receive re-armed after the flush
  integer, parameter :: F_EOF = 128       ! the peer's EOF (or a receive error) was seen: close once nothing is left to answer

  ! write kinds
  integer, parameter :: W_POST = 1, W_LIKE = 2

  ! Everything the loop knows about one connection, one cache line, in a
  ! calloc'ed table indexed by ring * table + slot (untouched slots cost nothing).
  type, bind(C) :: conn_t
    integer(c_int64_t) :: send_ptr
    integer(c_int64_t) :: partial      ! malloc'd buffer address or 0
    integer(c_int32_t) :: send_len
    integer(c_int32_t) :: send_arena
    integer(c_int32_t) :: plen
    integer(c_int32_t) :: pcap
    integer(c_int32_t) :: q_head       ! node index + 1, 0 = empty
    integer(c_int32_t) :: q_tail
    integer(c_int32_t) :: gen
    integer(c_int32_t) :: flags
    integer(c_int32_t) :: mark         ! last compaction that kept an entry for this slot (dedup)
    integer(c_int32_t) :: pad1
    integer(c_int64_t) :: pad2
  end type

  type :: write_t
    integer :: conn = 0
    integer :: wkind = 0          ! W_POST or W_LIKE
    integer(i8) :: user_id = 0
    integer(i8) :: post_id = 0
    integer(ip) :: name = 0       ! address in scratch (W_POST)
    integer :: name_len = 0
    integer(ip) :: body = 0
    integer :: body_len = 0
    integer :: wres = 0           ! W_LIKE: a LIKE_* code; W_POST: 1 ok / 0 fail
    integer(ip) :: created = 0    ! address of 24 bytes in scratch (W_POST)
    integer :: node = 0           ! the response-queue placeholder reserved at parse time
    integer :: gen = 0            ! the connection's generation then (slot reuse check)
  end type

  ! A growable list of 64-bit integers (addresses, indices).
  type :: ilist_t
    integer(i8), allocatable :: v(:)
    integer :: n = 0
  end type

  ! ---- state
  integer :: table = 0, nrings = 0, max_rings = 0, accept_ring = 0
  logical :: accept_paused = .false.
  integer :: accept_inflight = 0
  integer :: ring_live(0:RINGS_MAX - 1) = 0, ring_inflight(0:RINGS_MAX - 1) = 0
  integer :: lfds(LISTENERS) = -1, nlfds = 0
  integer :: accept_inflight_l(LISTENERS) = 0
  integer(i8) :: start_ms = 0, now = 0
  logical :: single_wait_ok = .true.
  type(conn_t), pointer, contiguous :: conns(:) => null()
  ! response queue nodes (pipelined responses waiting behind a send), 1-based;
  ! ptr 0 is a placeholder for a write whose response is built after COMMIT, -1 an
  ! orphan (the connection dropped its queue while the write was pending)
  type :: node_t
    integer(ip) :: ptr = 0
    integer :: len = 0
    integer :: arena = -1
    integer :: next = 0      ! node index, 0 = none
  end type
  type(node_t), allocatable :: qn(:)
  integer :: nqn = 0
  type(ilist_t) :: qn_free
  type(cqe_t), allocatable :: cqes(:)
  type(write_t), allocatable :: writes(:)
  integer :: nwrites = 0
  type(ilist_t) :: close_retry         ! connections whose close could not get its two SQEs
  ! Read queues, one circular buffer per ring, and the ready list, one for all
  ! rings. Entries are hints: the connection's flag (F_QUEUED, F_READY) is the
  ! truth, checked when the entry is taken, so an entry left behind by a close is
  ! skipped. A connection is on a list at most once while its flag is set; the
  ! buffers hold twice the table for the stale entries a close can leave.
  integer, pointer, contiguous :: rq(:) => null()
  integer :: rq_cap = 0
  integer :: rq_head(0:RINGS_MAX - 1) = 0, rq_n(0:RINGS_MAX - 1) = 0
  integer, pointer, contiguous :: rd(:) => null()
  integer :: rd_cap = 0, rd_head = 0, rd_n = 0
  integer :: nbufs = NBUFS_DEFAULT
  integer :: held(0:RINGS_MAX - 1) = 0   ! per ring: buffers handed to us and not yet given back
  type(ilist_t) :: drained               ! read dry by a bounded read; multishot receive re-armed after the flush
  integer :: answered = 0                ! requests answered in this pass
  logical :: capped = .false.            ! the last consume stopped at ANSWER_MAX with data left
  integer(i8) :: n_enobufs = 0, n_reads = 0, n_parked = 0
  integer :: rq_peak = 0, rd_peak = 0
  logical :: trace = .false.
  integer :: stamp = 0                   ! compaction counter, written to conn%mark
  integer(ip) :: body_buf = 0
  type(req_t) :: req
  ! per-batch bump allocator for bodies and usernames that outlive the receive buffer
  type(ilist_t) :: chunks
  integer :: sc_cur = 1, sc_used = 0
  ! constant responses
  type(resp_t) :: r_not_found, r_bad_id, r_post_missing, r_missing_token, r_bad_token, r_bad_payload
  type(resp_t) :: r_bad_json, r_body_required, r_body_long, r_internal, r_bad_request, r_too_large, r_degraded
  integer(i8) :: last_tick = 0, last_tick_prev = 0
  logical :: efd0_ready = .false.

contains

  ! ---- small helpers

  subroutine fail(msg)
    character(len=*), intent(in) :: msg
    call logmsg('fatal: ' // msg)
    error stop 1
  end subroutine

  subroutine push(l, x)
    type(ilist_t), intent(inout) :: l
    integer(i8), intent(in) :: x
    integer(i8), allocatable :: more(:)
    if (.not. allocated(l%v)) allocate(l%v(64))
    if (l%n == size(l%v)) then
      allocate(more(2 * size(l%v)))
      more(1:l%n) = l%v(1:l%n)
      call move_alloc(more, l%v)
    end if
    l%n = l%n + 1
    l%v(l%n) = x
  end subroutine

  function pop(l) result(x)
    type(ilist_t), intent(inout) :: l
    integer(i8) :: x
    x = l%v(l%n)
    l%n = l%n - 1
  end function

  function mk_ud(op, ring, gen, slot) result(ud)
    integer, intent(in) :: op, ring, gen, slot
    integer(i8) :: ud
    ud = ior(ior(shiftl(int(op, i8), 60), shiftl(int(ring, i8), 56)), &
             ior(shiftl(iand(int(gen, i8), int(z'FFFFFF', i8)), 32), int(slot, i8)))
  end function

  function has(f, bit) result(yes)
    integer, intent(in) :: f, bit
    logical :: yes
    yes = iand(f, bit) /= 0
  end function

  ! ---- scratch (bump allocator)

  subroutine scratch_reset()
    sc_cur = 1
    sc_used = 0
  end subroutine

  function scratch_take(n) result(a)
    integer, intent(in) :: n
    integer(ip) :: a
    if (sc_used + n > SCRATCH_CHUNK) then
      sc_cur = sc_cur + 1
      if (sc_cur > chunks%n) call push(chunks, int(balloc(SCRATCH_CHUNK), i8))
      sc_used = 0
    end if
    a = int(chunks%v(sc_cur), ip) + sc_used
    sc_used = sc_used + n
  end function

  ! ---- init

  subroutine srv_init(db_path, secret, host, port)
    character(len=*), intent(in) :: db_path, secret, host, port
    integer :: k, lfd, iport
    integer(ip) :: sec, a
    type(conn_t), pointer, contiguous :: ctmp(:)
    integer, pointer, contiguous :: itmp(:)
    read (port, *) iport
    call db_open(db_path)
    call db_startup(db_path)
    sec = balloc(len(secret) + 1)
    block
      type(out_t) :: o
      call out_init(o, sec, len(secret) + 1)
      call put_str(o, secret)
    end block
    call jwt_init(sec, len(secret))
    call arenas_init()
    call push(chunks, int(balloc(SCRATCH_CHUNK), i8))
    ! Test-only knobs: a tiny table makes rings fill after a few connections so the
    ! multi-ring path can be exercised locally; fewer provided buffers per ring let a
    ! few hundred connections run the ring dry (ENOBUFS, the read queue). Never set
    ! on the benchmark box.
    table = getenv_int('FORTRAN_TABLE_OVERRIDE', table_size())
    if (table <= 0) call fail('cannot determine fd limit')
    nbufs = getenv_int('FORTRAN_NBUFS_OVERRIDE', NBUFS_DEFAULT)
    max_rings = (TARGET_CONNS + table - 1) / table
    if (max_rings > RINGS_MAX) max_rings = RINGS_MAX
    if (max_rings < 1) max_rings = 1
    if (ring_init(SQ_ENTRIES, table, nbufs, BUFSIZE) < 0) call fail('ring init failed')
    nrings = 0
    accept_ring = 0
    accept_paused = .false.
    accept_inflight = 0
    nlfds = 0
    do k = 1, LISTENERS
      lfd = listen_on(host, iport, LISTEN_BACKLOG)
      if (lfd < 0) then
        if (nlfds > 0) exit   ! the port allows fewer sharers: serve with what opened
        call fail('listen failed: ' // itoa(int(lfd, i8)))
      end if
      nlfds = nlfds + 1
      lfds(nlfds) = lfd
    end do
    start_ms = now_ms()
    now = unix_time()
    single_wait_ok = .true.
    a = calloc_bytes(int(max_rings, i8) * int(table, i8), 64)
    call c_f_pointer(cp(a), ctmp, [max_rings * table])
    conns(0:) => ctmp
    allocate(cqes(CQ_BATCH))
    allocate(writes(256))
    nwrites = 0
    rq_cap = 2 * table
    a = calloc_bytes(int(max_rings, i8) * int(rq_cap, i8), 4)
    call c_f_pointer(cp(a), itmp, [max_rings * rq_cap])
    rq(0:) => itmp
    rd_cap = 2 * max_rings * table
    a = calloc_bytes(int(rd_cap, i8), 4)
    call c_f_pointer(cp(a), itmp, [rd_cap])
    rd(0:) => itmp
    rd_head = 0
    rd_n = 0
    answered = 0
    capped = .false.
    trace = len(getenv_str('FORTRAN_TRACE', '')) > 0
    stamp = 0
    body_buf = balloc(PARTIAL_MAX * 4)
    r_not_found = const_resp(404, '{"error":"not found"}')
    r_bad_id = const_resp(400, '{"error":"invalid post id"}')
    r_post_missing = const_resp(404, '{"error":"post not found"}')
    r_missing_token = const_resp(401, '{"error":"missing bearer token"}')
    r_bad_token = const_resp(401, '{"error":"invalid or expired token"}')
    r_bad_payload = const_resp(401, '{"error":"invalid token payload"}')
    r_bad_json = const_resp(400, '{"error":"malformed JSON body"}')
    r_body_required = const_resp(400, '{"error":"body is required"}')
    r_body_long = const_resp(400, '{"error":"body must be at most 500 characters"}')
    r_internal = const_resp(500, '{"error":"internal server error"}')
    r_bad_request = const_resp(400, '{"error":"bad request"}')
    r_too_large = const_resp(413, '{"error":"request too large"}')
    r_degraded = const_resp(503, '{"status":"degraded","db":"unreachable","error":"SELECT 1 failed"}')
    last_tick = start_ms
    last_tick_prev = start_ms
    efd0_ready = .false.
    if (ring_open(0) /= 0) call fail('ring 0 open failed')
    nrings = 1
    call pump_accepts()
  end subroutine

  ! ---- accepting

  function ring_free(r) result(n)
    integer, intent(in) :: r
    integer :: n
    n = table - ring_live(r) - ring_inflight(r)
  end function

  ! Pick the ring to accept on: the next one in order, else any with free slots.
  function next_accept_ring() result(ok)
    logical :: ok
    integer :: nr, best, best_free, r, fr
    ok = .false.
    nr = accept_ring + 1
    if (nr < max_rings) then
      if (nr >= nrings) then
        if (ring_open(nr) /= 0) then
          call logmsg('ring open failed; accepting paused')
          return
        end if
        nrings = nr + 1
      end if
      accept_ring = nr
      ok = .true.
      return
    end if
    best = -1
    best_free = 0
    do r = 0, nrings - 1
      fr = ring_free(r)
      if (fr > best_free) then
        best = r
        best_free = fr
      end if
    end do
    if (best < 0) return
    accept_ring = best
    ok = .true.
  end function

  ! Keep up to ACCEPT_DEPTH one-shot accepts outstanding, an equal share per
  ! listener, never more than the accept ring has free slots; move to another
  ! ring when it is full.
  subroutine pump_accepts()
    integer :: depth, per, l, r, fr, want, k
    if (accept_paused) return
    ! an accept in flight reserves a slot: with a test-sized table, keep the
    ! reservation a fraction of the slots so connections can still be accepted
    depth = ACCEPT_DEPTH
    if (depth > table / 4) depth = table / 4
    per = depth / nlfds
    if (per < 1) per = 1
    do l = 1, nlfds
      do while (accept_inflight_l(l) < per)
        r = accept_ring
        fr = ring_free(r)
        if (fr <= 0) then
          if (.not. next_accept_ring()) then
            call logmsg('all rings full; accepting paused')
            accept_paused = .true.
            return
          end if
          cycle
        end if
        want = per - accept_inflight_l(l)
        if (want > fr) want = fr
        do k = 1, want
          if (prep_accept(r, lfds(l), mk_ud(OP_ACCEPT, r, 0, l - 1)) /= 0) return
          accept_inflight = accept_inflight + 1
          accept_inflight_l(l) = accept_inflight_l(l) + 1
          ring_inflight(r) = ring_inflight(r) + 1
        end do
      end do
    end do
  end subroutine

  subroutine on_accept(ring, listener, res)
    integer, intent(in) :: ring, listener, res
    integer :: slot, conn
    accept_inflight = accept_inflight - 1
    accept_inflight_l(listener + 1) = accept_inflight_l(listener + 1) - 1
    ring_inflight(ring) = ring_inflight(ring) - 1
    if (res >= 0) then
      slot = res
      conn = ring * table + slot
      conns(conn)%flags = F_OPEN
      conns(conn)%gen = conns(conn)%gen + 1
      ring_live(ring) = ring_live(ring) + 1
      call arm_recv(ring, slot, conn)
    else if (res /= EAGAIN) then
      call logmsg('accept failed: ' // itoa(int(res, i8)) // ' on ring ' // itoa(int(ring, i8)))
    end if
  end subroutine

  ! ---- the read queue and the ready list

  ! Arm the multishot receive; if the submission queue has no room even after a
  ! submit, the connection goes on the read queue instead, which keeps the retry
  ! (a connection with no receive and on no list would hang).
  subroutine arm_recv(ring, slot, conn)
    integer, intent(in) :: ring, slot, conn
    if (prep_recv(ring, slot, mk_ud(OP_RECV, ring, conns(conn)%gen, slot)) /= 0) call queue_read(conn)
  end subroutine

  ! The read queues and the ready list are circular buffers of connection indices
  ! at buf(base : base + cap - 1), with a head and a count. These three helpers are
  ! all that touches them.

  ! Compact a full buffer: keep one entry per slot whose flag is set (the oldest),
  ! drop the rest (entries left by closed lifetimes of the slot). Afterwards it
  ! holds at most one entry per live slot, half its capacity, so an append never
  ! overwrites. Rare: it needs more closes of queued connections than pops
  ! between two passes.
  subroutine ring_compact(buf, base, hd, n, cap, flag)
    integer, intent(inout) :: buf(0:)
    integer, intent(in) :: base, hd, cap, flag
    integer, intent(inout) :: n
    integer :: kept, i, conn
    stamp = stamp + 1
    kept = 0
    do i = 0, n - 1
      conn = buf(base + mod(hd + i, cap))
      if (has(conns(conn)%flags, flag) .and. conns(conn)%mark /= stamp) then
        conns(conn)%mark = stamp
        buf(base + mod(hd + kept, cap)) = conn
        kept = kept + 1
      end if
    end do
    n = kept
  end subroutine

  ! Append conn, compacting first if full (so an append never overwrites).
  subroutine ring_push(buf, base, hd, n, cap, flag, conn)
    integer, intent(inout) :: buf(0:)
    integer, intent(in) :: base, hd, cap, flag, conn
    integer, intent(inout) :: n
    if (n >= cap) call ring_compact(buf, base, hd, n, cap, flag)
    buf(base + mod(hd + n, cap)) = conn
    n = n + 1
  end subroutine

  function ring_pop(buf, base, hd, n, cap) result(conn)
    integer, intent(in) :: buf(0:)
    integer, intent(in) :: base, cap
    integer, intent(inout) :: hd, n
    integer :: conn
    conn = buf(base + hd)
    hd = mod(hd + 1, cap)
    n = n - 1
  end function

  subroutine queue_read(conn)
    integer, intent(in) :: conn
    integer :: f, r
    f = conns(conn)%flags
    if (has(f, F_QUEUED) .or. .not. has(f, F_OPEN) .or. has(f, F_CLOSING)) return
    r = conn / table
    ! push first: a compaction inside it must see this connection's flag still
    ! clear, so a stale hint for the slot is dropped rather than kept beside this one
    call ring_push(rq, r * rq_cap, rq_head(r), rq_n(r), rq_cap, F_QUEUED, conn)
    conns(conn)%flags = ior(f, F_QUEUED)
    if (rq_n(r) > rq_peak) rq_peak = rq_n(r)
  end subroutine

  subroutine mark_ready(conn)
    integer, intent(in) :: conn
    integer :: f
    f = conns(conn)%flags
    if (has(f, F_READY)) return
    call ring_push(rd, 0, rd_head, rd_n, rd_cap, F_READY, conn)   ! push, then flag (see queue_read)
    conns(conn)%flags = ior(f, F_READY)
    n_parked = n_parked + 1
    if (rd_n > rd_peak) rd_peak = rd_n
  end subroutine

  subroutine mark_drained(conn)
    integer, intent(in) :: conn
    integer :: f
    f = conns(conn)%flags
    if (iand(f, ior(ior(F_DRAINED, F_CLOSING), ior(F_CLOSE_AFTER, F_EOF))) /= 0) return
    conns(conn)%flags = ior(f, F_DRAINED)
    call push(drained, int(conn, i8))
  end subroutine

  function pending_work() result(yes)
    logical :: yes
    integer :: r
    yes = .true.
    if (rd_n > 0 .or. drained%n > 0) return
    do r = 0, nrings - 1
      if (rq_n(r) > 0) return
    end do
    yes = .false.
  end function

  ! ---- sends and closes

  subroutine start_send(ring, slot, conn, ptr, n, arena)
    integer, intent(in) :: ring, slot, conn, n, arena
    integer(ip), intent(in) :: ptr
    integer :: r
    conns(conn)%send_ptr = ptr
    conns(conn)%send_len = n
    conns(conn)%send_arena = arena
    conns(conn)%flags = ior(conns(conn)%flags, F_SENDING)
    r = prep_send(ring, slot, ptr, n, mk_ud(OP_SEND, ring, conns(conn)%gen, slot))
  end subroutine

  subroutine do_close(ring, slot, conn)
    integer, intent(in) :: ring, slot, conn
    integer :: f, g
    f = conns(conn)%flags
    if (has(f, F_CLOSING)) return
    conns(conn)%flags = ior(f, F_CLOSING)
    g = conns(conn)%gen
    if (prep_close(ring, slot, mk_ud(OP_SHUTDOWN, ring, g, slot), mk_ud(OP_CLOSE, ring, g, slot)) /= 0) then
      ! no room even after a submit: try again at the end of the batch
      conns(conn)%flags = f
      call push(close_retry, int(conn, i8))
    end if
  end subroutine

  ! Append a queue node for conn (ptr == 0 is a placeholder for a write whose
  ! response is built after COMMIT). Returns the node index (1-based).
  function enqueue(conn, ptr, n, arena) result(node)
    integer, intent(in) :: conn, n, arena
    integer(ip), intent(in) :: ptr
    integer :: node, tail
    type(node_t), allocatable :: more(:)
    if (qn_free%n > 0) then
      node = int(pop(qn_free))
    else
      if (.not. allocated(qn)) allocate(qn(256))
      if (nqn == size(qn)) then
        allocate(more(2 * size(qn)))
        more(1:nqn) = qn(1:nqn)
        call move_alloc(more, qn)
      end if
      nqn = nqn + 1
      node = nqn
    end if
    qn(node)%ptr = ptr
    qn(node)%len = n
    qn(node)%arena = arena
    qn(node)%next = 0
    tail = conns(conn)%q_tail
    if (tail == 0) then
      conns(conn)%q_head = node
    else
      qn(tail)%next = node
    end if
    conns(conn)%q_tail = node
  end function

  ! Queue a response for a connection: send now if nothing is ahead of it, else
  ! append (pipelined requests are answered in order, one send in flight).
  subroutine respond(ring, slot, conn, r, wclose)
    integer, intent(in) :: ring, slot, conn
    type(resp_t), intent(in) :: r
    logical, intent(in) :: wclose
    integer :: f, node
    f = conns(conn)%flags
    if (has(f, F_CLOSING)) then
      call arena_release(r%arena)   ! a close is already on its way; nothing more goes out
      return
    end if
    if (wclose) then
      f = ior(f, F_CLOSE_AFTER)
      conns(conn)%flags = f
    end if
    if (.not. has(f, F_SENDING) .and. conns(conn)%q_head == 0) then
      call start_send(ring, slot, conn, r%ptr, r%len, r%arena)
      return
    end if
    node = enqueue(conn, r%ptr, r%len, r%arena)
  end subroutine

  ! Start the next queued response; false if none, or if the head is a write still
  ! waiting for its commit.
  function send_next(ring, slot, conn) result(sent)
    integer, intent(in) :: ring, slot, conn
    logical :: sent
    integer :: node, nxt
    sent = .false.
    if (has(conns(conn)%flags, F_CLOSING)) then
      call drop_queue(conn)
      return
    end if
    node = conns(conn)%q_head
    if (node == 0) return
    if (qn(node)%ptr == 0) return
    nxt = qn(node)%next
    conns(conn)%q_head = nxt
    if (nxt == 0) conns(conn)%q_tail = 0
    call start_send(ring, slot, conn, qn(node)%ptr, qn(node)%len, qn(node)%arena)
    call push(qn_free, int(node, i8))
    sent = .true.
  end function

  ! Close a connection that is to close (a close request answered, or the peer's
  ! EOF) once nothing is left: no send in flight, nothing queued, nothing parked.
  subroutine close_if_done(conn)
    integer, intent(in) :: conn
    integer :: f
    f = conns(conn)%flags
    if (iand(f, ior(F_CLOSE_AFTER, F_EOF)) == 0 .or. has(f, F_CLOSING) .or. has(f, F_SENDING)) return
    if (conns(conn)%q_head /= 0) return
    if (has(f, F_READY) .and. .not. has(f, F_CLOSE_AFTER)) return   ! parked requests still to answer (EOF only)
    call do_close(conn / table, mod(conn, table), conn)
  end subroutine

  ! A placeholder got its response: send it if the connection is idle.
  subroutine kick(conn)
    integer, intent(in) :: conn
    integer :: f
    f = conns(conn)%flags
    if (.not. has(f, F_SENDING) .and. .not. has(f, F_CLOSING)) then
      if (.not. send_next(conn / table, mod(conn, table), conn)) call close_if_done(conn)
    end if
  end subroutine

  ! Drop everything queued for a connection that is going away. A placeholder
  ! still waiting for its write is not freed: run_writes owns it until it has
  ! resolved the write, and sees the -1 (orphan) mark then.
  subroutine drop_queue(conn)
    integer, intent(in) :: conn
    integer :: node, hd
    hd = conns(conn)%q_head
    do while (hd /= 0)
      node = hd
      hd = qn(node)%next
      if (qn(node)%ptr == 0) then
        qn(node)%ptr = -1
      else
        call arena_release(qn(node)%arena)
        call push(qn_free, int(node, i8))
      end if
    end do
    conns(conn)%q_head = 0
    conns(conn)%q_tail = 0
  end subroutine

  ! ---- request handling

  subroutine add_write(w)
    type(write_t), intent(in) :: w
    type(write_t), allocatable :: more(:)
    if (nwrites == size(writes)) then
      allocate(more(2 * size(writes)))
      more(1:nwrites) = writes(1:nwrites)
      call move_alloc(more, writes)
    end if
    nwrites = nwrites + 1
    writes(nwrites) = w
  end subroutine

  subroutine handle_request(ring, slot, conn)
    integer, intent(in) :: ring, slot, conn
    logical :: wclose, is_create, is_like
    integer(ip) :: p, name, body
    integer :: n, rc, blen, brc, node
    integer(i8) :: id
    type(out_t) :: o
    type(write_t) :: w
    wclose = req%wclose
    p = req%path
    n = req%path_len
    if (req%method == M_GET) then
      if (eqlit(p, n, '/feed')) then
        call arena_begin(FEED_MAX, o)
        call db_write_feed(o)
        call respond(ring, slot, conn, arena_finish(200, o), wclose)
        return
      end if
      if (n > 7) then
        if (eqlit(p, 7, '/posts/')) then
          id = parse_pos_int(p + 7, n - 7)
          if (id <= 0) then
            call respond(ring, slot, conn, r_bad_id, wclose)
            return
          end if
          call arena_begin(POST_MAX, o)
          if (db_write_post(id, o)) then
            call respond(ring, slot, conn, arena_finish(200, o), wclose)
          else
            call respond(ring, slot, conn, r_post_missing, wclose)
          end if
          return
        end if
      end if
      if (eqlit(p, n, '/health')) then
        if (db_health_ok()) then
          call arena_begin(128, o)
          call put_str(o, '{"status":"ok","db":"ok","uptime_s":')
          call put_int(o, (now_ms() - start_ms) / 1000)
          call put(o, 125)
          call respond(ring, slot, conn, arena_finish(200, o), wclose)
        else
          call respond(ring, slot, conn, r_degraded, wclose)
        end if
        return
      end if
      call respond(ring, slot, conn, r_not_found, wclose)
      return
    end if
    if (req%method == M_POST) then
      is_create = eqlit(p, n, '/posts')
      is_like = .false.
      if (n > 12) is_like = eqlit(p, 7, '/posts/') .and. eqlit(p + n - 5, 5, '/like')
      if (.not. is_create .and. .not. is_like) then
        call respond(ring, slot, conn, r_not_found, wclose)
        return
      end if
      ! auth first
      if (.not. req%bearer) then
        call respond(ring, slot, conn, r_missing_token, wclose)
        return
      end if
      rc = jwt_verify(req%tok, req%tok_len, now)
      if (rc == JWT_INVALID) then
        call respond(ring, slot, conn, r_bad_token, wclose)
        return
      end if
      if (rc == JWT_PAYLOAD) then
        call respond(ring, slot, conn, r_bad_payload, wclose)
        return
      end if
      if (is_like) then
        id = parse_pos_int(p + 7, n - 12)
        if (id <= 0) then
          call respond(ring, slot, conn, r_bad_id, wclose)
          return
        end if
        node = enqueue(conn, 0_ip, 0, -1)
        w%conn = conn; w%wkind = W_LIKE; w%user_id = jwt_user_id; w%post_id = id
        w%name = 0; w%name_len = 0; w%body = 0; w%body_len = 0; w%wres = 0; w%created = 0
        w%node = node; w%gen = conns(conn)%gen
        call add_write(w)
        if (wclose) conns(conn)%flags = ior(conns(conn)%flags, F_CLOSE_AFTER)
        return
      end if
      ! create: validate the JSON body now, copy the trimmed body and the
      ! token's username to scratch (the receive buffer is returned after this)
      brc = validate_body(req%body, req%body_len, body_buf, PARTIAL_MAX * 4, blen)
      if (brc == BODY_MALFORMED) then
        call respond(ring, slot, conn, r_bad_json, wclose)
        return
      end if
      if (brc == BODY_REQUIRED) then
        call respond(ring, slot, conn, r_body_required, wclose)
        return
      end if
      if (brc == BODY_TOO_LONG) then
        call respond(ring, slot, conn, r_body_long, wclose)
        return
      end if
      name = scratch_take(jwt_name_len)
      call bcopy(name, jwt_name_ptr, jwt_name_len)
      body = scratch_take(blen)
      call bcopy(body, body_buf, blen)
      node = enqueue(conn, 0_ip, 0, -1)
      w%conn = conn; w%wkind = W_POST; w%user_id = jwt_user_id; w%post_id = 0
      w%name = name; w%name_len = jwt_name_len; w%body = body; w%body_len = blen
      w%wres = 0; w%created = 0; w%node = node; w%gen = conns(conn)%gen
      call add_write(w)
      if (wclose) conns(conn)%flags = ior(conns(conn)%flags, F_CLOSE_AFTER)
      return
    end if
    call respond(ring, slot, conn, r_not_found, wclose)
  end subroutine

  ! Parse as many complete requests as the n bytes at dat hold, up to the pass's
  ! answer cap; returns bytes consumed, or -1 to close. `capped` says it stopped at the cap.
  function consume(ring, slot, conn, dat, n) result(off)
    integer, intent(in) :: ring, slot, conn, n
    integer(ip), intent(in) :: dat
    integer :: off
    off = 0
    capped = .false.
    do while (off < n)
      if (answered >= ANSWER_MAX) then
        capped = .true.
        exit
      end if
      call parse_request(dat + off, n - off, req)
      if (req%status == P_INCOMPLETE) exit
      if (req%status == P_BAD) then
        call respond(ring, slot, conn, r_bad_request, .true.)
        off = -1
        return
      end if
      call handle_request(ring, slot, conn)
      answered = answered + 1
      off = off + req%consumed
      if (has(conns(conn)%flags, F_CLOSE_AFTER)) then
        off = -1
        return
      end if
    end do
  end function

  ! Make the connection's partial-request buffer hold `need` bytes (realloc keeps
  ! what is already there). False past the cap.
  ! `tight`: size the buffer to the bytes (rounded to 64) rather than doubling from
  ! 512: a parked complete request is read later as it is and never grows, and
  ! after a stall there can be a parked request per connection.
  function grow_partial(conn, need, limit, tight) result(ok)
    integer, intent(in) :: conn, need, limit
    logical, intent(in) :: tight
    logical :: ok
    integer(ip) :: pbuf
    integer :: want
    ok = .false.
    if (need > limit) return
    pbuf = conns(conn)%partial
    ok = .true.
    if (pbuf /= 0 .and. conns(conn)%pcap >= need) return
    if (tight .and. pbuf == 0) then
      want = iand(need + 63, not(63))
    else
      want = PARTIAL_MIN
      do while (want < need)
        want = want * 2
      end do
    end if
    if (want > limit) want = limit
    if (pbuf /= 0) then
      pbuf = realloc_bytes(pbuf, want)
    else
      pbuf = malloc_bytes(want)
    end if
    conns(conn)%partial = pbuf
    conns(conn)%pcap = want
  end function

  subroutine free_partial(conn)
    integer, intent(in) :: conn
    integer(ip) :: pbuf
    pbuf = conns(conn)%partial
    if (pbuf /= 0) then
      call free_addr(pbuf)
      conns(conn)%partial = 0
      conns(conn)%plen = 0
      conns(conn)%pcap = 0
    end if
  end subroutine

  ! A terminal response (413) went out for a connection: whatever was parked is
  ! dropped with it, so nothing is answered after or out of order with it.
  subroutine too_large(ring, slot, conn)
    integer, intent(in) :: ring, slot, conn
    conns(conn)%flags = iand(conns(conn)%flags, not(F_READY))
    call respond(ring, slot, conn, r_too_large, .true.)
  end subroutine

  ! Received bytes for a connection (dat is a provided buffer, given back here):
  ! append to a partial request if one is pending, answer what is complete within
  ! the cap, keep the rest. False if the connection is closing. While the cap
  ! parks complete requests the buffer may grow to PARKED_MAX; one incomplete
  ! request is still limited to PARTIAL_MAX.
  function ingest(ring, slot, conn, dat, n, bid) result(ok)
    integer, intent(in) :: ring, slot, conn, n, bid
    integer(ip), intent(in) :: dat
    logical :: ok
    integer :: consumed, rest, f, limit, have
    integer(ip) :: pbuf
    ok = .false.
    f = conns(conn)%flags
    if (has(f, F_READY) .or. answered >= ANSWER_MAX) then
      limit = PARKED_MAX
    else
      limit = PARTIAL_MAX
    end if
    pbuf = conns(conn)%partial
    if (pbuf /= 0) then
      have = conns(conn)%plen
      if (.not. grow_partial(conn, have + n, limit, .false.)) then
        call buf_return(ring, bid)
        call too_large(ring, slot, conn)
        return
      end if
      pbuf = conns(conn)%partial
      call bcopy(pbuf + have, dat, n)
      call buf_return(ring, bid)
      have = have + n
      conns(conn)%plen = have
      consumed = consume(ring, slot, conn, pbuf, have)
      if (consumed < 0) then
        ! a close request or a bad request: the rest of the input is dropped
        conns(conn)%flags = iand(conns(conn)%flags, not(F_READY))
        call free_partial(conn)
        return
      end if
      rest = have - consumed
      if (rest == 0) then
        call free_partial(conn)
      else if (consumed > 0) then
        call bmove(pbuf, pbuf + consumed, rest)
        conns(conn)%plen = rest
      end if
    else
      consumed = consume(ring, slot, conn, dat, n)
      if (consumed < 0) then
        call buf_return(ring, bid)
        return
      end if
      rest = n - consumed
      if (rest > 0) then
        if (.not. grow_partial(conn, rest, limit, capped)) then
          call buf_return(ring, bid)
          call too_large(ring, slot, conn)
          return
        end if
        call bcopy(conns(conn)%partial, dat + consumed, rest)
        conns(conn)%plen = rest
      end if
      call buf_return(ring, bid)
    end if
    if (rest > 0) then
      if (capped) then
        call mark_ready(conn)
      else if (rest > PARTIAL_MAX) then
        ! what is left is one incomplete request, past the single-request cap
        call too_large(ring, slot, conn)
        return
      end if
    end if
    ok = .true.
  end function

  ! A receive completed: the multishot one (single=false) or a bounded read.
  subroutine on_recv(ring, slot, conn, res, cflags, single)
    integer, intent(in) :: ring, slot, conn, res, cflags
    logical, intent(in) :: single
    integer :: f, bid
    logical :: has_buf
    integer(ip) :: dat
    f = conns(conn)%flags
    has_buf = iand(cflags, CQE_F_BUFFER) /= 0
    bid = shiftr(cflags, CQE_BUFFER_SHIFT)
    if (.not. has(f, F_OPEN) .or. iand(f, ior(F_CLOSING, F_CLOSE_AFTER)) /= 0) then
      ! closed, closing, or closing once its responses are out: after a request
      ! that asked to close, nothing more is read (it would replay what the
      ! buffer still holds)
      if (has_buf) call buf_return(ring, bid)
      return
    end if
    if (res <= 0) then
      if (has_buf) call buf_return(ring, bid)
      if (res == ENOBUFS) then
        ! the ring's buffers are out: read this one later, when they are back
        n_enobufs = n_enobufs + 1
        call queue_read(conn)
        return
      end if
      if (single .and. res == EAGAIN) then
        call mark_drained(conn)   ! back to a multishot receive, once the buffers are back
        return
      end if
      ! EOF or error: everything parked or queued (including writes waiting for
      ! their commit) is still answered first, then the connection closes
      conns(conn)%flags = ior(f, F_EOF)
      call close_if_done(conn)
      return
    end if
    dat = buf_base(ring) + int(bid, ip) * BUFSIZE
    if (.not. ingest(ring, slot, conn, dat, res, bid)) return
    if (single) then
      ! a full buffer may mean more is waiting; a shorter read means drained
      if (res >= BUFSIZE) then
        call queue_read(conn)
      else
        call mark_drained(conn)
      end if
    else if (iand(cflags, CQE_F_MORE) == 0) then
      call queue_read(conn)
    end if
  end subroutine

  subroutine on_send(ring, slot, conn, res)
    integer, intent(in) :: ring, slot, conn, res
    integer :: f, left, r
    integer(ip) :: ptr
    f = conns(conn)%flags
    if (.not. has(f, F_OPEN)) return
    if (res < 0) then
      call arena_release(conns(conn)%send_arena)
      conns(conn)%flags = iand(f, not(F_SENDING))
      call drop_queue(conn)
      call do_close(ring, slot, conn)
      return
    end if
    left = conns(conn)%send_len - res
    if (left > 0) then
      ptr = conns(conn)%send_ptr + res
      conns(conn)%send_ptr = ptr
      conns(conn)%send_len = left
      r = prep_send(ring, slot, ptr, left, mk_ud(OP_SEND, ring, conns(conn)%gen, slot))
      return
    end if
    call arena_release(conns(conn)%send_arena)
    f = iand(f, not(F_SENDING))
    conns(conn)%flags = f
    if (send_next(ring, slot, conn)) return
    ! not sent: either nothing is queued, or the head is a write still waiting
    ! for its commit; only the first case may close
    call close_if_done(conn)
  end subroutine

  subroutine on_close(conn)
    integer, intent(in) :: conn
    call drop_queue(conn)
    conns(conn)%flags = 0
    conns(conn)%gen = conns(conn)%gen + 1
    ring_live(conn / table) = ring_live(conn / table) - 1
    call free_partial(conn)
  end subroutine

  ! ---- the write phase

  subroutine run_writes()
    integer :: i, conn, status
    integer(i8) :: id
    integer(ip) :: created
    logical :: committed, ok
    type(resp_t) :: r
    type(out_t) :: o
    call db_close_cursors()
    do i = 1, nwrites
      if (writes(i)%wkind == W_POST) then
        created = scratch_take(CREATED_LEN)
        ok = db_insert_post(writes(i)%user_id, writes(i)%body, writes(i)%body_len, id, created)
        if (ok) then
          writes(i)%wres = 1
        else
          writes(i)%wres = 0
        end if
        writes(i)%post_id = id
        writes(i)%created = created
      else
        writes(i)%wres = db_like(writes(i)%user_id, writes(i)%post_id)
      end if
    end do
    committed = db_commit()
    do i = 1, nwrites
      conn = writes(i)%conn
      if (qn(writes(i)%node)%ptr == -1) then
        ! the connection dropped its queue (close, send error) since the parse;
        ! the write still ran, but nobody is waiting for the response
        call push(qn_free, int(writes(i)%node, i8))
        cycle
      end if
      if (.not. committed .or. writes(i)%wres < 0 .or. (writes(i)%wkind == W_POST .and. writes(i)%wres == 0)) then
        r = r_internal
      else if (writes(i)%wkind == W_LIKE) then
        if (writes(i)%wres == LIKE_MISSING) then
          r = r_post_missing
        else
          call arena_begin(128, o)
          call put_str(o, '{"liked":true,"already_liked":')
          if (writes(i)%wres == LIKE_DUP) then
            call put_str(o, 'true')
          else
            call put_str(o, 'false')
          end if
          call put_str(o, ',"post_id":')
          call put_int(o, writes(i)%post_id)
          call put(o, 125)
          status = 200
          if (writes(i)%wres == LIKE_NEW) status = 201
          r = arena_finish(status, o)
        end if
      else
        call arena_begin(CREATE_MAX, o)
        call put_str(o, '{"post":')
        call put_post_head(o, writes(i)%post_id, writes(i)%body, writes(i)%body_len, writes(i)%created, CREATED_LEN)
        call put_json(o, writes(i)%name, writes(i)%name_len)
        call put_post_tail(o, 0_i8)
        call put(o, 125)
        r = arena_finish(201, o)
      end if
      qn(writes(i)%node)%ptr = r%ptr
      qn(writes(i)%node)%len = r%len
      qn(writes(i)%node)%arena = r%arena
      call kick(conn)
    end do
    nwrites = 0
  end subroutine

  ! ---- the loop

  function reap_ring(r) result(handled)
    integer, intent(in) :: r
    integer :: handled, n, i
    handled = 0
    do
      n = reap(r, cqes, CQ_BATCH)
      handled = handled + n
      do i = 1, n
        call dispatch(cqes(i))
      end do
      if (n < CQ_BATCH) exit
    end do
  end function

  subroutine flush_bufs(r)
    integer, intent(in) :: r
    integer :: n
    n = buf_flush(r)
    if (n > 0) held(r) = held(r) - n
  end subroutine

  ! Answer parked requests, oldest first, up to the cap.
  subroutine serve_ready()
    integer :: conn, f, have, consumed, rest
    integer(ip) :: pbuf
    do while (rd_n > 0 .and. answered < ANSWER_MAX)
      conn = ring_pop(rd, 0, rd_head, rd_n, rd_cap)
      f = conns(conn)%flags
      if (.not. has(f, F_READY)) cycle
      conns(conn)%flags = iand(f, not(F_READY))
      pbuf = conns(conn)%partial
      if (.not. has(f, F_OPEN) .or. has(f, F_CLOSING) .or. has(f, F_CLOSE_AFTER) .or. pbuf == 0) cycle
      have = conns(conn)%plen
      consumed = consume(conn / table, mod(conn, table), conn, pbuf, have)
      if (consumed < 0) then
        call free_partial(conn)   ! a close or bad request: the rest of the input is dropped
        call close_if_done(conn)
        cycle
      end if
      rest = have - consumed
      if (rest == 0) then
        call free_partial(conn)
      else
        if (consumed > 0) then
          call bmove(pbuf, pbuf + consumed, rest)
          conns(conn)%plen = rest
        end if
        if (capped) then
          call mark_ready(conn)
          cycle
        end if
        if (has(f, F_EOF)) call free_partial(conn)   ! an incomplete request the peer will never finish
      end if
      call close_if_done(conn)
    end do
  end subroutine

  ! Bounded reads for the read queues: per ring, as many single-shot receives as
  ! it has free buffers, submitted and reaped in the same round, in rounds while
  ! the queue and the pass's read budget last.
  subroutine issue_reads()
    integer :: budget, r, fr, want, issued, k, conn, f, slot, got, s
    integer(i8) :: e0
    budget = READ_MAX
    do r = 0, nrings - 1
      do while (rq_n(r) > 0 .and. budget > 0)
        fr = nbufs - held(r)
        if (fr <= 0) exit
        want = rq_n(r)
        if (want > fr) want = fr
        if (want > budget) want = budget
        issued = 0
        do k = 1, want
          conn = ring_pop(rq, r * rq_cap, rq_head(r), rq_n(r), rq_cap)
          f = conns(conn)%flags
          if (.not. has(f, F_QUEUED)) cycle
          conns(conn)%flags = iand(f, not(F_QUEUED))
          if (.not. has(f, F_OPEN) .or. has(f, F_CLOSING)) cycle
          slot = mod(conn, table)
          if (prep_read(r, slot, mk_ud(OP_READ, r, conns(conn)%gen, slot)) /= 0) then
            call queue_read(conn)   ! no SQE even after a submit: next round
            exit
          end if
          issued = issued + 1
        end do
        if (issued == 0) exit
        budget = budget - issued
        n_reads = n_reads + issued
        e0 = n_enobufs
        s = submit(r)
        got = reap_ring(r)
        call flush_bufs(r)
        if (trace) call logmsg('trace: ring ' // itoa(int(r, i8)) // ' issued ' // itoa(int(issued, i8)) // &
          ' free_before ' // itoa(int(fr, i8)) // ' reaped ' // itoa(int(got, i8)) // ' enobufs+' // &
          itoa(n_enobufs - e0) // ' held_after ' // itoa(int(held(r), i8)) // ' rq ' // itoa(int(rq_n(r), i8)) // &
          ' rd ' // itoa(int(rd_n, i8)) // ' answered ' // itoa(int(answered, i8)))
        ! multishot receives took buffers this pass had counted as free:
        ! nothing more can succeed on this ring until the next flush lands
        if (n_enobufs > e0) exit
      end do
    end do
  end subroutine

  ! Re-arm the multishot receive of every connection a bounded read drained. After
  ! the flush, so the re-provided buffers precede these in the submission queue: a
  ! multishot receive selects a buffer at submission, before it looks at the socket,
  ! and with the group empty it would end with ENOBUFS at once. The flag guards
  ! against a slot closed and accepted again since (its new connection was armed
  ! by the accept).
  subroutine arm_drained()
    integer :: i, conn, f
    do i = 1, drained%n
      conn = int(drained%v(i))
      f = conns(conn)%flags
      if (.not. has(f, F_DRAINED)) cycle
      conns(conn)%flags = iand(f, not(F_DRAINED))
      if (has(f, F_OPEN) .and. .not. has(f, F_CLOSING) .and. .not. has(f, F_QUEUED) .and. .not. has(f, F_EOF)) &
        call arm_recv(conn / table, mod(conn, table), conn)
    end do
    drained%n = 0
  end subroutine

  subroutine dispatch(c)
    type(cqe_t), intent(in) :: c
    integer(i8) :: ud
    integer :: op, ring, g, slot, res, conn
    ud = c%user_data
    op = int(shiftr(ud, 60))
    ring = int(iand(shiftr(ud, 56), 15_i8))
    g = int(iand(shiftr(ud, 32), int(z'FFFFFF', i8)))
    slot = int(iand(ud, int(z'FFFFFFFF', i8)))
    res = c%res
    if (op == OP_ACCEPT) then
      call on_accept(ring, slot, res)   ! the slot field carries the listener index
      return
    end if
    if (iand(c%flags, CQE_F_BUFFER) /= 0) held(ring) = held(ring) + 1
    conn = ring * table + slot
    if (iand(conns(conn)%gen, int(z'FFFFFF')) /= g) then
      ! a completion for an earlier life of this slot
      if (iand(c%flags, CQE_F_BUFFER) /= 0) call buf_return(ring, shiftr(c%flags, CQE_BUFFER_SHIFT))
      return
    end if
    select case (op)
    case (OP_SHUTDOWN)
      return
    case (OP_RECV)
      call on_recv(ring, slot, conn, res, c%flags, .false.)
    case (OP_READ)
      call on_recv(ring, slot, conn, res, c%flags, .true.)
    case (OP_SEND)
      call on_send(ring, slot, conn, res)
    case (OP_CLOSE)
      call on_close(conn)
    end select
  end subroutine

  subroutine tick()
    integer(i8) :: t
    t = now_ms()
    if (t - last_tick < 1000) return
    last_tick = t
    ! a line every minute on how the WAL has been kept short (restarts should do it all)
    if ((t - start_ms) / 60000 /= (last_tick_prev - start_ms) / 60000) then
      call logmsg('wal: holds ' // itoa(int(holds, i8)) // ', restarts ' // itoa(int(restarts, i8)) // &
        ', expired ' // itoa(int(holds_expired, i8)) // ', loop truncates ' // itoa(int(truncates, i8)))
      call logmsg('recv: enobufs ' // itoa(n_enobufs) // ', bounded reads ' // itoa(n_reads) // &
        ', parked ' // itoa(n_parked) // ', read queue peak ' // itoa(int(rq_peak, i8)) // &
        ', ready peak ' // itoa(int(rd_peak, i8)) // '; MemAvailable ' // itoa(mem_available_kb() / 1024) // ' MB')
    end if
    last_tick_prev = t
    ! (No accept pause on low MemAvailable: at the wall it only kept the users the
    ! test was ramping waiting in the SYN queue until they timed out. A refused
    ! connection saves nothing.)
    if (accept_paused) then
      if (next_accept_ring()) then
        ! paused because every ring was full: a ring has room again
        accept_paused = .false.
        call pump_accepts()
      end if
    end if
  end subroutine

  ! One ring and nothing paused: submit and wait in a single enter. Otherwise:
  ! submit each ring, then epoll on their eventfds.
  function single_mode() result(single)
    logical :: single
    single = nrings == 1 .and. single_wait_ok .and. .not. accept_paused
  end function

  subroutine srv_run()
    logical :: single, busy, committed
    integer :: r, rc, handled, n, i, conn, m
    integer(i8) :: e0
    type(ilist_t) :: left
    single = single_mode()
    busy = .false.
    if (.not. single) then
      do r = 0, nrings - 1
        rc = submit(r)
      end do
    end if
    do
      if (pending_work()) then
        ! parked requests or queued reads: don't sleep, just submit (and run
        ! any deferred completion work) and go again
        if (single) then
          rc = submit(0)
        else
          m = wait_rings(0)   ! the end of the last pass submitted every ring
        end if
      else if (single) then
        if (writes_held) then
          rc = wait_one_batch(0, 1, HOLD_POLL_US)
        else if (busy) then
          rc = wait_one_batch(0, BATCH_WANT, BATCH_US)
        else
          rc = wait_one(0)
        end if
        if (rc < 0 .and. rc /= EAGAIN .and. rc /= EINTR) then
          call logmsg('wait_one failed: ' // itoa(int(rc, i8)) // '; using epoll waits')
          single_wait_ok = .false.
        end if
      else
        if (.not. efd0_ready) then
          if (ring_enable_efd(0) == 0) then
            efd0_ready = .true.
          else
            call logmsg('eventfd registration failed; epoll waits will time out instead')
          end if
        end if
        if (writes_held) then
          m = wait_rings(HOLD_POLL_US / 1000)
        else if (.not. efd0_ready) then
          m = wait_rings(100)
        else
          m = wait_rings(1000)
        end if
        if (busy) call settle(BATCH_US)
      end if
      now = unix_time()
      call db_release_hold()
      ! held writes keep their names and bodies in scratch across batches
      if (nwrites == 0) call scratch_reset()
      answered = 0
      call serve_ready()
      e0 = n_enobufs
      handled = 0
      do r = 0, nrings - 1
        handled = handled + reap_ring(r)
      end do
      if (trace .and. (handled > 100 .or. n_enobufs > e0)) call logmsg('trace: pass reaped ' // &
        itoa(int(handled, i8)) // ' enobufs+' // itoa(n_enobufs - e0) // ' held ' // itoa(int(held(0), i8)) // &
        ' rq ' // itoa(int(rq_n(0), i8)) // ' rd ' // itoa(int(rd_n, i8)) // ' answered ' // itoa(int(answered, i8)))
      busy = handled >= BUSY_MIN
      ! hand buffers back, then read for the connections whose receive ran out of them
      do r = 0, nrings - 1
        call flush_bufs(r)
      end do
      call issue_reads()
      call arm_drained()
      if (nwrites > 0 .and. .not. writes_held) then
        call run_writes()
      else
        committed = db_commit()
      end if
      if (close_retry%n > 0) then
        n = close_retry%n
        do i = 1, n
          conn = int(close_retry%v(i))
          if (has(conns(conn)%flags, F_OPEN)) call do_close(conn / table, mod(conn, table), conn)
        end do
        ! keep only what do_close re-queued during this pass
        left%n = 0
        do i = n + 1, close_retry%n
          call push(left, close_retry%v(i))
        end do
        close_retry%n = 0
        do i = 1, left%n
          call push(close_retry, left%v(i))
        end do
      end if
      call pump_accepts()
      call tick()
      single = single_mode()
      if (.not. single) then
        do r = 0, nrings - 1
          rc = submit(r)
        end do
      end if
    end do
  end subroutine

end module srv

program server
  use bytes
  use shim
  use db, only: tail_ok
  use srv
  implicit none
  character(len=:), allocatable :: db_path, secret, host, port
  integer :: rc, binary_ok
  integer(i8) :: locked
  db_path = getenv_str('SQLITE_PATH', '')
  if (len(db_path) == 0) then
    call logmsg('SQLITE_PATH is required')
    error stop 1
  end if
  secret = getenv_str('JWT_SECRET', 'twelve-dollar-challenge')
  host = getenv_str('HOST', '127.0.0.1')
  port = getenv_str('PORT', '3000')
  call srv_init(db_path, secret, host, port)
  rc = checkpointer_start(db_path, CKPT_INTERVAL_MS)
  locked = lock_code(binary_ok)
  if (binary_ok /= 0) then
    call logmsg('locked ' // itoa(locked / 1024) // ' KB of code and read-only data')
  else
    call logmsg('locked ' // itoa(locked / 1024) // ' KB of code and read-only data' // &
      ' (the binary itself only partly: RLIMIT_MEMLOCK)')
  end if
  call logmsg('fortran server listening on ' // host // ':' // port // ' (fd table ' // itoa(int(table, i8)) // &
    ' x up to ' // itoa(int(max_rings, i8)) // ' rings, tail plan ' // trim(merge('on ', 'off', tail_ok)) // ')')
  call srv_run()
end program server
