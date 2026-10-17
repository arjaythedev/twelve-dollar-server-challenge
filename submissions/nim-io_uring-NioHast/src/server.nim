## server.nim: the io_uring HTTP transport (the only one).
##
## Reuses the frozen `api.nim`/`db.nim`/`config.nim` modules unchanged, the raw
## io_uring bindings in `iouring.nim`, and the shared HTTP parser/framing in
## `http.nim`. The socket scaffold - the fd-limit bump, the listening-socket
## setup and the `processRequests` loop - lives in this same module.
##
## Why io_uring here
## -----------------
## The one thing this transport does that a readiness API cannot is *registered
## (fixed) file descriptors*. Accepted connections live in the ring's
## fixed-file table and every subsequent RECV/SEND addresses them by slot index
## with `IOSQE_FIXED_FILE`. The fixed table holds its own reference, so a
## connection no longer consumes a process `RLIMIT_NOFILE` slot. The registered
## table is what the famous 300k-connection C entry rides.
##
## Two io_uring best-practices are layered on top of the basic transport:
##
##  1. **Ring setup flags** (`IORING_SETUP_SINGLE_ISSUER | DEFER_TASKRUN`, plus
##     `COOP_TASKRUN`): the recommended set for a single-threaded, per-thread
##     ring.  Less cross-CPU locking, and completion (task) work is deferred to
##     `io_uring_enter`, which the loop already calls every pass.  If the kernel
##     rejects the flags with `-EINVAL` the ring is re-created without them and
##     startup proceeds; the mode is logged.
##
##  2. **Direct multishot accept** (`IORING_ACCEPT_MULTISHOT` with the SQE's
##     `file_index = IORING_FILE_INDEX_ALLOC`): the kernel installs each accepted
##     socket straight into the first free fixed-file slot, so no
##     `IORING_REGISTER_FILES_UPDATE` and no per-accept fd allocation are needed.
##     The accept CQE's `res` is the slot index.  If the kernel rejects the
##     direct variant (`-EINVAL`/`-EOPNOTSUPP`), we transparently fall back to
##     the register-per-accept path.  Closing a connection still frees its slot
##     with `FILES_UPDATE(-1)`, so the slot is reused by the next direct accept.
##
## Fidelity
## --------
## The HTTP/1.1 parser, response framing, keep-alive, pipelining,
## `Content-Length` handling and malformed-framing policy live in the shared
## `http.nim`, so the emitted bytes are pinned by construction. The socket is
## single-threaded; the event source is the io_uring completion queue.

import std/[posix, os, strutils, times, nativesockets, oserrors]

import iouring, config, db, api, http

# ---------------------------------------------------------------------------
# transport-agnostic socket scaffold: the per-process fd-limit bump, the
# listening-socket setup, the optional per-connection TCP tuning, and the HTTP
# request/response loop. Nothing here touches the ring or a connection struct;
# the io_uring loop adapts its own `Conn` to these routines.

# syscall surface std/posix does not expose (or that we want verbatim)
var SOCK_NONBLOCK {.importc, header: "<sys/socket.h>".}: cint
# TCP_DEFER_ACCEPT (value in seconds) is in <netinet/tcp.h>.
var TCP_DEFER_ACCEPT {.importc, header: "<netinet/tcp.h>".}: cint
# TCP_NODELAY set on the listener is inherited by accepted sockets on Linux, so
# the send path needs no per-connection setsockopt (which would also defeat the
# direct-accept path; see the gate in main).
var TCP_NODELAY {.importc, header: "<netinet/tcp.h>".}: cint
# Per-connection TCP options (applied to an accepted socket; see applyTcpTuning).
var TCP_QUICKACK {.importc, header: "<netinet/tcp.h>".}: cint
var TCP_NOTSENT_LOWAT {.importc, header: "<netinet/tcp.h>".}: cint

proc envIntDef(name: string, dflt: int): int =
  try:
    parseInt(getEnv(name, $dflt))
  except ValueError:
    dflt

proc tcpNotsentLowatBytes*(): int =
  ## TCP_NOTSENT_LOWAT value in bytes; 0 disables it (the default).
  result = envIntDef("TCP_NOTSENT_LOWAT_BYTES", 0)
  if result < 0: result = 0

proc tcpQuickAck*(): bool =
  ## Whether to set TCP_QUICKACK on accepted sockets (default off).
  getEnv("TCP_QUICKACK", "0") == "1"

proc applyTcpTuning*(fd: cint) =
  ## Optional, env-gated per-connection TCP tuning (both default OFF, so the
  ## default build is unchanged):
  ##   * TCP_NOTSENT_LOWAT_BYTES > 0 caps the unsent write queue (lower kernel
  ##     memory per socket and a shorter socket-lock hold under backlog);
  ##   * TCP_QUICKACK=1 disables delayed ACKs (lower latency, more ACK traffic).
  let lowat = tcpNotsentLowatBytes()
  if lowat > 0:
    var v: cint = cint(lowat)
    discard posix.setsockopt(SocketHandle(fd), posix.IPPROTO_TCP,
                             TCP_NOTSENT_LOWAT, addr v, SockLen(sizeof(v)))
  if tcpQuickAck():
    var one: cint = 1
    discard posix.setsockopt(SocketHandle(fd), posix.IPPROTO_TCP,
                             TCP_QUICKACK, addr one, SockLen(sizeof(one)))

# Fixed-file table bound and fd headroom. Declared ahead of `raiseNofile`
# because that probe reports the unlimited hard limit as the table cap plus this
# reserve, and Nim resolves module-level constants in source order.
const
  CAP        = 65536
  FdsReserve = 64

proc raiseNofile*(): int =
  ## Raise this process's soft RLIMIT_NOFILE up to its hard limit and return the
  ## *hard* limit (`rlim_max`) as an `int`. Per-process only (no kernel/sysctl
  ## change), and a no-op when the soft limit already equals the hard limit, which
  ## is the case when started under `ulimit -n N` / `prlimit --nofile=N:N`.
  ##
  ## The return value is the process's real fd ceiling: the caller sizes the
  ## registered fixed-file table from it (the table needs roughly one entry per
  ## connection plus headroom).
  var rl: RLimit
  discard getrlimit(RLIMIT_NOFILE, rl)
  let soft0 = rl.rlim_cur
  let hard = rl.rlim_max
  if soft0 < hard:
    var want: RLimit
    want.rlim_cur = hard
    want.rlim_max = hard
    if setrlimit(RLIMIT_NOFILE, want) == 0:
      var after: RLimit
      discard getrlimit(RLIMIT_NOFILE, after)
      stderr.writeLine("RLIMIT_NOFILE raised: soft " & $soft0 & " -> " &
                       $after.rlim_cur & " (hard " & $after.rlim_max & ")")
    else:
      stderr.writeLine("setrlimit(RLIMIT_NOFILE) failed: errno=" &
                       $(osLastError().int) & " (soft=" & $soft0 &
                       " hard=" & $hard & ")")
  else:
    stderr.writeLine("RLIMIT_NOFILE soft==hard==" & $soft0 & " (nothing to raise)")
  # Report the hard limit both when we raised and when soft==hard. An unlimited
  # hard limit (RLIM_INFINITY) is rlim_t all-ones, which Nim's signed `int`
  # rlim field reads back as negative; returning that would silently size the
  # fixed-file table down to the floor instead of the cap. Report the cap plus
  # the reserved headroom so the caller's min(hardNofile - FdsReserve, CAP)
  # lands exactly on CAP. Finite limits are returned unchanged.
  if hard < 0:
    result = CAP + FdsReserve
  else:
    result = int(hard)

proc setupListenSocket*(host: string; port, backlog, deferSec: int): cint =
  ## Create, bind and listen a non-blocking IPv4 socket, returning its fd (or -1,
  ## after logging, on any failure). `deferSec > 0` enables TCP_DEFER_ACCEPT
  ## before `listen`: the kernel withholds a completed handshake from `accept`
  ## until the client sends its first byte (HTTP clients send at once, so accept
  ## still fires immediately, but a silent handshake never wakes the loop).
  ## TCP_NODELAY is always set on the listener, which the kernel then inherits
  ## onto every accepted socket, so the send path installs no per-connection
  ## option and the direct-accept fast path stays usable.
  result = cint(posix.socket(posix.AF_INET,
                             posix.SOCK_STREAM or SOCK_NONBLOCK, 0))
  if result < 0:
    stderr.writeLine("socket() failed: " & $(osLastError().int))
    return -1
  var one: cint = 1
  discard posix.setsockopt(SocketHandle(result), posix.SOL_SOCKET,
                           posix.SO_REUSEADDR, addr one, SockLen(sizeof(one)))
  # Listener-level send tuning: TCP_NODELAY removes Nagle on each accepted
  # socket (inherited by the kernel at accept time), byte-for-byte the same
  # effect as a per-connection setsockopt without the per-accept syscall.
  discard posix.setsockopt(SocketHandle(result), posix.IPPROTO_TCP,
                           TCP_NODELAY, addr one, SockLen(sizeof(one)))
  if deferSec > 0:
    var dv: cint = cint(deferSec)
    discard posix.setsockopt(SocketHandle(result), posix.IPPROTO_TCP,
                             TCP_DEFER_ACCEPT, addr dv, SockLen(sizeof(dv)))
  var sa: Sockaddr_in
  sa.sin_family = TSa_Family(posix.AF_INET)
  sa.sin_port = nativesockets.htons(uint16(port))
  if posix.inet_pton(posix.AF_INET, host.cstring, addr sa.sin_addr) != 1:
    stderr.writeLine("invalid HOST: " & host)
    discard posix.close(result)
    return -1
  if posix.bindSocket(SocketHandle(result), cast[ptr SockAddr](addr sa),
                      SockLen(sizeof(sa))) != 0:
    stderr.writeLine("bind() failed on " & host & ":" & $port & ": " &
                     $(osLastError().int))
    discard posix.close(result)
    return -1
  if posix.listen(SocketHandle(result), cint(backlog)) != 0:
    stderr.writeLine("listen() failed: " & $(osLastError().int))
    discard posix.close(result)
    return -1

# ---------------------------------------------------------------------------
# request processing (the transport-agnostic core is api.handleRequest)

proc processRequests*(inbuf, outbuf: var string; keepAlive, closing: var bool;
                      outOff: int; batchFrom, batchCount: var int;
                      database: Db; secret: string;
                      maxInBuf, maxOutBuf: int): bool =
  ## Parse and answer every complete pipelined request currently in `inbuf`,
  ## appending the response bytes to `outbuf`. Returns false when the framing is
  ## malformed (or non-conforming) and the connection must close after flushing.
  ##
  ## `maxOutBuf` stops the loop once that many response bytes are buffered,
  ## leaving the remainder for the send-drain to finish.
  #
  # Advance with a local offset and compact once at the end; front-deleting per
  # request was O(n^2) on a large pipelined buffer.
  var pos = 0
  while true:
    if outbuf.len - outOff >= maxOutBuf:
      break
    let hend = inbuf.find("\r\n\r\n", pos)
    if hend < 0:
      if inbuf.len - pos > MaxHeader:
        return false
      break
    if hend - pos > MaxHeader:
      return false

    let head = parseHead(inbuf, pos, hend, maxInBuf)
    if not head.ok:
      return false
    if head.bad:
      # Non-conforming framing (bad/conflicting Content-Length, Transfer-
      # Encoding, ...): answer 400 and close, rather than silently treating it
      # as CL=0 and re-parsing the body as a fresh request.
      outbuf.addResponse(400, """{"error":"bad request"}""", false)
      closing = true
      return false
    let totalLen = (hend - pos) + 4 + head.contentLen
    if inbuf.len - pos < totalLen:
      break                  # body not fully arrived
    var rawBody = ""
    if head.contentLen > 0:
      let b = hend + 4
      rawBody = inbuf[b ..< b + head.contentLen]
    pos += totalLen

    var resp: ApiResponse
    try:
      resp = handleRequest(database, secret, head.httpMethod, head.path,
                           head.auth, rawBody)
    except Exception as ex:
      # Exception covers Defect as well (OverflowDefect, IndexDefect, ...): no
      # request may ever abort the single-threaded process.
      stderr.writeLine("request handler error: " & ex.msg)
      resp = ApiResponse(status: 500, body: """{"error":"internal server error"}""")

    keepAlive = head.keepAlive
    let respStart = outbuf.len
    outbuf.addResponse(resp.status, resp.body, head.keepAlive)
    # Record responses produced while a group-commit batch is open: they must not
    # be sent until that batch commits, and become 500 if it fails to commit.
    if database != nil and database.batchOpen():
      if batchCount == 0:
        batchFrom = respStart
      inc batchCount
    if not head.keepAlive:
      closing = true
      break

  if pos > 0:
    inbuf.delete(0 ..< pos)
  true


# ---------------------------------------------------------------------------
# syscall surface std/posix and iouring.nim do not expose for us
# (raw `syscall`, never liburing)

proc c_syscall(number: clong): clong
  {.importc: "syscall", header: "<unistd.h>", varargs.}
var SYS_io_uring_register {.importc, header: "<sys/syscall.h>".}: clong

# SOCK_NONBLOCK is declared with the socket scaffold above; SOCK_CLOEXEC is
# only needed here.
var SOCK_CLOEXEC  {.importc, header: "<sys/socket.h>".}: cint

# ---------------------------------------------------------------------------
# io_uring constants not already exported by iouring.nim (mirrored from
# <linux/io_uring.h>)

const
  IOSQE_FIXED_FILE*        = 1'u8 shl 0

  IORING_OP_TIMEOUT*       = 11'u8
  IORING_OP_ASYNC_CANCEL*  = 14'u8

  IORING_REGISTER_FILES_UPDATE* = 6
  IORING_ASYNC_CANCEL_USERDATA  = 1'u32 shl 4

  # user_data operation tags (low 8 bits) + slot (next 24) + generation (high 32)
  TAG_ACCEPT*  = 1'u64
  TAG_RECV*    = 2'u64
  TAG_SEND*    = 3'u64
  TAG_TIMEOUT* = 4'u64
  TAG_CANCEL*  = 5'u64
  TAG_PROVIDE* = 6'u64

  # ring / table sizing.
  # The registered-file table size is RUNTIME (see `gSlots`): it is derived from
  # the process fd ceiling at startup instead of a compile-time literal, so the
  # server is no longer capped at 32768 connections. `IORING_REGISTER_FILES`
  # counts every entry against RLIMIT_NOFILE, so the table size is
  # `min(hardLimit - FdsReserve, CAP)`: `FdsReserve` reserves headroom for the
  # process's own fds (stdio, the io_uring ring, the listener before it is
  # closed, the sqlite db) - without it, registering 65536 entries fails when
  # RLIMIT_NOFILE==65535. `SlotsFloor` is the smallest table we will register; if
  # the kernel refuses the top size, startup steps DOWN to it instead of exiting.
  RingEntries  = 4096
  # Explicit CQ headroom, requested via IORING_SETUP_CQSIZE (see setupQueue).
  # Without CQSIZE the kernel defaults `cq_entries = 2 * sq_entries` (verified
  # on this host: sq=4096 -> cq=8192).  We ask for CqMultiple x so a burst of
  # completions cannot park enough entries on the kernel CQ-overflow list to
  # stall the loop while the live connection count approaches the ~64k
  # conntrack ceiling: 4 * 4096 = 16384 CQEs against 4096 SQEs.  The kernel
  # fills the size it actually allocated into params.cq_entries and the startup
  # log prints it, so the headroom is observed, not assumed.  Submissions stay
  # bounded by RingEntries: getSqe returns nil on a full SQ and mustSqe()
  # flushes/waits.
  CqMultiple   = 4
  SlotsFloor   = 16384
  ListenSlot   = 0'i32

  # read/parse limits
  MaxInBuf      = 1024 * 1024
  MaxRequest    = MaxInBuf + MaxHeader + 4
  # Output cap: processing stops once this many response bytes are buffered,
  # which leaves output in flight so the drain path resumes the remaining
  # pipelined requests.  The cap bounds how many *pipelined* responses are
  # queued for one connection, not the size of any single response: a response
  # is appended whole and only the top-of-loop check observes the cap, so a body
  # larger than the cap is still fully buffered and handed to the SEND path
  # (partial sends resume from `outOff` on the next completion).  64 KiB keeps a
  # deep pipeline from pinning a multi-MiB heap buffer per connection while
  # staying far above the largest single response this API returns.
  MaxOutBuf     = 64 * 1024
  TickSeconds   = 1.0

  # -------------------------------------------------------------------------
  # Provided-buffer pool ("the turbo").
  #
  # Multishot recv selects its landing buffer from a single SHARED pool instead
  # of every connection owning a private 16 KiB recv buffer. Idle connections
  # therefore hold no recv storage at all (a buffer is taken only while bytes
  # are actually in flight), which is what removes the per-connection RSS that
  # the one-shot design paid at 10k connections.
  #
  # The pool is one lazily-faulted allocation: `alloc` gives untouched pages, so
  # only the buffers the kernel has actually written into become resident. The
  # descriptors live in a page-aligned ring registered once with
  # IORING_REGISTER_PBUF_RING; when the kernel cannot do that we fall back to
  # IORING_OP_PROVIDE_BUFFERS over the exact same buffer memory.
  BufGroup       = 0'u16
  BufSize        = 8 * 1024
  # Default pool 64: chosen by measuring RSS against latency across pool sizes
  # at 2500 simulated users (3 runs per size). VmHWM median fell 46.1 MiB (2048)
  # -> 41.9 (1024) -> 39.2 (256) -> 38.5 (64) while the p99 median stayed at
  # ~12 ms (the quiet baseline run was 12.6 ms), so there was no latency
  # regression. A 5000-user cross-check was also non-regressed (VmHWM 62.9 vs
  # 71.0 MiB, p99 134 vs 681 ms). 64 is the floor the clamp in initPool enforces;
  # IOURING_BUFS can still raise the pool up to BufRingMax. Both values MUST be
  # powers of two (buf-ring requirement). BufSize is left at 8 KiB: requests are
  # well under 4 KiB, so only one page per buffer is ever faulted and shrinking
  # it cannot lower RSS (page granularity) but would add a CQE per request.
  BufRingDefault = 64
  BufRingMax     = 8192

# kernel-side struct for IORING_OP_TIMEOUT
type KernelTimespec {.bycopy.} = object
  tv_sec:  int64
  tv_nsec: int64

# ---------------------------------------------------------------------------
# connection state: one slot per entry in the registered file table

type
  ConnFlag = enum
    ## Per-connection boolean state. Kept in one `set` so the nine one-bit
    ## facts occupy two bytes instead of nine bytes padded out to a full
    ## machine word each time they land between the wider fields.
    cfActive, cfKeepAlive, cfClosing, cfReadPaused, cfRecvArmed, cfSendArmed,
    cfWantFree, cfDeferred

  Conn = object
    gen:        uint32   ## bumped per reuse so stale CQEs are ignored
    st:         set[ConnFlag]
    inbuf:      string
    outbuf:     string
    outOff:     int
    pending:    int      ## in-flight io_uring ops referencing our buffers
    lastActive: float
    batchFrom:  int      ## outbuf offset where this pass's batch output begins
    batchCount: int      ## responses appended while the pass's batch was open

# `Conn.st` accessors. These are templates so the packed set reads and writes
# exactly like the former per-field booleans did: `c.active` stays a boolean
# expression and `c.active = false` stays an assignment. Each expands inline to
# one bit test / incl / excl, so there is no call on the hot path. A pair is
# provided for both a `Conn` value and a `ptr Conn`, matching the two ways the
# connection is addressed (direct index and `addr conns[slot]`).
template active(c: Conn | ptr Conn): bool = cfActive in c.st
template `active=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfActive else: c.st.excl cfActive)

template keepAlive(c: Conn | ptr Conn): bool = cfKeepAlive in c.st
template `keepAlive=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfKeepAlive else: c.st.excl cfKeepAlive)

template closing(c: Conn | ptr Conn): bool = cfClosing in c.st
template `closing=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfClosing else: c.st.excl cfClosing)

template readPaused(c: Conn | ptr Conn): bool = cfReadPaused in c.st
template `readPaused=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfReadPaused else: c.st.excl cfReadPaused)

template recvArmed(c: Conn | ptr Conn): bool = cfRecvArmed in c.st
template `recvArmed=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfRecvArmed else: c.st.excl cfRecvArmed)

template sendArmed(c: Conn | ptr Conn): bool = cfSendArmed in c.st
template `sendArmed=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfSendArmed else: c.st.excl cfSendArmed)

template wantFree(c: Conn | ptr Conn): bool = cfWantFree in c.st
template `wantFree=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfWantFree else: c.st.excl cfWantFree)

template deferred(c: Conn | ptr Conn): bool = cfDeferred in c.st
template `deferred=`(c: var Conn | ptr Conn, v: bool) =
  (if v: c.st.incl cfDeferred else: c.st.excl cfDeferred)

type
  SlotRef = tuple
    ## A pending slot reference carrying the connection generation at the time it
    ## was queued. Direct accept aggressively reuses fixed slots, so a bare index
    ## could name a different (reused) connection by the time it is consumed; the
    ## generation lets the consumer skip a stale entry.
    slot: cint
    gen:  uint32

var
  gRing:        Ring
  gDb:          Db
  gSecret:      string
  gSlots:       int            # runtime registered-file table size (set in main)
  conns:        seq[Conn]      # indexed by fixed-file slot, never resized
  gFreeSlots:   seq[cint]
  gFreeTop:     int
  gGen:         uint32
  gNow:         float
  gLastCkpt:    float
  gAcceptArmed: bool
  gAcceptPaused: bool

  # ---- turbo toggles (env) -------------------------------------------------
  # IOURING_RECV=multishot|oneshot (default multishot) and
  # IOURING_ACCEPT=multishot|oneshot (default multishot). Both fall back at runtime
  # if the kernel refuses the feature, and `-ENOBUFS`/`-EINVAL` are always
  # handled, so a one-shot build is a single env var away if p99 regresses.
  gRecvMultishot:   bool = true
  gAcceptMultishot: bool = true
  gAcceptMsProven:  bool = false
  # Direct accept: install the accepted socket straight into a fixed slot with
  # IORING_FILE_INDEX_ALLOC (kernel picks the slot; CQE res == slot). IOURING_ACCEPT_DIRECT=0
  # forces the register-per-accept fallback; it also engages automatically if the
  # kernel rejects the direct variant. Only meaningful with multishot accept.
  gAcceptDirect:       bool = true
  gAcceptDirectProven: bool = false

  # ---- provided-buffer pool ------------------------------------------------
  gBufBase:     ptr UncheckedArray[char]   # gBufSlots * BufSize, lazily faulted
  gBufRing:     ptr UncheckedArray[BufRingEntry]  # page-aligned descriptor ring
  gBufTail:     uint16
  gBufSlots:    int = BufRingDefault       # live pool size (IOURING_BUFS, power of 2)
  gBufMask:     int = BufRingDefault - 1
  gUseBufRing:  bool                       # true: registered PBUF_RING; else list
  gBufsReady:   int                        # buffers handed to the kernel, not yet seen
  gConsumedThisPass: int                   # data CQEs that returned a buffer this pass
  gRecvStarved: seq[SlotRef]               # recv ended with -ENOBUFS (slot+gen)
  gSendRetry:   seq[SlotRef]               # send ended with -ENOBUFS (slot+gen)

  # Slots whose SEND was withheld because their responses were produced while a
  # group-commit batch was open. Armed (or rewritten to 500) once the batch has
  # been committed at the end of the completion-queue pass.
  gDeferredSlots: seq[SlotRef]

  # accept scratch (must stay valid until the accept op completes)
  gAcceptSa:    Sockaddr_in
  gAcceptSaLen: SockLen

  # timeout scratch (must stay valid until the timeout op completes)
  gTs:          KernelTimespec

# ---------------------------------------------------------------------------
# user_data packing

proc makeUd(tag: uint64, slot: cint, gen: uint32): uint64 {.inline.} =
  tag or (uint64(uint32(slot)) shl 8) or (uint64(gen) shl 32)

proc udTag(ud: uint64): uint64 {.inline.} = ud and 0xFF'u64
proc udSlot(ud: uint64): cint {.inline.} = cint((ud shr 8) and 0xFFFFFF'u64)
proc udGen(ud: uint64): uint32 {.inline.} = uint32(ud shr 32)

# ---------------------------------------------------------------------------
# HTTP/1.1 parsing and serialisation live in the shared `http` module (imported
# above); this transport only supplies its inbound body cap (MaxInBuf) to
# parseHead.

proc processRequests(c: var Conn): bool {.inline.} =
  ## Thin adapter over the shared request loop above; maxOutBuf caps the
  ## buffered response bytes so a large pipelined burst drains across sends.
  ## keepAlive/closing are now bits in `c.st`, so they are staged through locals
  ## for the `var bool` core and written back; identical in/out semantics.
  var ka = c.keepAlive
  var cl = c.closing
  result = processRequests(c.inbuf, c.outbuf, ka, cl, c.outOff,
                           c.batchFrom, c.batchCount, gDb, gSecret, MaxInBuf,
                           MaxOutBuf)
  c.keepAlive = ka
  c.closing = cl

# ---------------------------------------------------------------------------
# fixed-file table bookkeeping

proc updateSlotRaw(slot: cint, fd: cint): int =
  ## IORING_REGISTER_FILES_UPDATE: install `fd` (or -1 to clear) at `slot`.
  ## Passing -1 drops the table's reference, which closes the socket once any
  ## in-flight op holding its own reference completes.
  var up: array[4, uint64]
  up[0] = uint64(uint32(slot))          # offset(u32) + resv(u32)
  var tmp = fd
  up[1] = uint64(cast[uint](addr tmp))  # pointer to the fd array
  let ret = c_syscall(SYS_io_uring_register, clong(gRing.fd),
                      clong(IORING_REGISTER_FILES_UPDATE),
                      clong(cast[uint](addr up[0])), clong(1))
  if ret < 0:
    result = -cint(osLastError().int)
  else:
    result = int(ret)

proc allocSlot(): cint =
  if gFreeTop <= 0:
    return -1
  dec gFreeTop
  gFreeSlots[gFreeTop]

proc releaseSlot(slot: cint) =
  ## Recv storage is pooled (not per-connection), so returning a slot is just
  ## bookkeeping: no buffer to free, only flags and the free-list push.
  ## In direct-accept mode the *kernel* owns slot allocation (FILES_UPDATE(-1) in
  ## closeConn already frees the slot), so we must NOT also push it onto the
  ## userspace free list - doing so would double-count and could overflow it.
  conns[int(slot)].wantFree = false
  if gAcceptDirect:
    return
  gFreeSlots[gFreeTop] = slot
  inc gFreeTop

proc rebuildFreeList() =
  ## Rebuild the userspace free-slot list from the live connection table. Used
  ## when a running ring downgrades from direct accept to the register-per-accept
  ## path: the list was never consumed in direct mode, so it is stale and must
  ## exclude every slot the kernel handed out directly.
  gFreeTop = 0
  for i in 1 ..< gSlots:
    if not conns[i].active:
      gFreeSlots[gFreeTop] = cint(i)
      inc gFreeTop

proc maybeFreeSlot(slot: cint) =
  if slot < 0:
    return
  if not conns[int(slot)].active and conns[int(slot)].wantFree and
     conns[int(slot)].pending == 0:
    releaseSlot(slot)

# ---------------------------------------------------------------------------
# submission helpers

proc mustSqe(): ptr Sqe =
  ## Reserve an SQE, flushing (and, if needed, waiting for a completion) when
  ## the submission queue is momentarily full.
  result = gRing.getSqe()
  if result.isNil:
    discard gRing.submit()
    result = gRing.getSqe()
  if result.isNil:
    discard gRing.submitAndWait(1)
    result = gRing.getSqe()

# ---------------------------------------------------------------------------
# provided-buffer pool
#
# Buffer `bid` always lives at `gBufBase[bid * BufSize]`; the id is the index,
# so a returned buffer is simply re-published with that same id and no
# allocation bookkeeping is needed at all on the hot path.

proc poolPush(bid: int) =
  ## Hand buffer `bid` back to the kernel. With a registered buf ring this is
  ## pure userspace bookkeeping (write the descriptor, publish `tail`); the
  ## fallback submits one IORING_OP_PROVIDE_BUFFERS per buffer.
  if gUseBufRing:
    let idx = int(gBufTail) and gBufMask
    let e = addr gBufRing[idx]
    e.`addr` = cast[uint64](addr gBufBase[bid * BufSize])
    e.len = uint32(BufSize)
    e.bid = uint16(bid)
    # Publish the producer tail only after the descriptor fields; the kernel
    # loads `tail` with acquire semantics, and on x86 stores are ordered.
    gBufTail = gBufTail + 1'u16
    gBufRing[0].resv = gBufTail
  else:
    let sqe = mustSqe()
    if sqe.isNil:
      return
    prepProvideBuffers(sqe, cast[uint64](addr gBufBase[bid * BufSize]),
                       uint32(BufSize), 1'u32, uint16(bid), BufGroup)
    sqe.user_data = TAG_PROVIDE
  inc gBufsReady

proc poolConsume() {.inline.} =
  dec gBufsReady
  inc gConsumedThisPass

proc armAccept() =
  ## Arm accept. With IORING_ACCEPT_MULTISHOT one SQE serves every pending
  ## connection until the kernel reports it can no longer continue
  ## (IORING_CQE_F_MORE clears); otherwise it is re-armed per completion.
  if gAcceptArmed:
    return
  let sqe = mustSqe()
  if sqe.isNil:
    return
  gAcceptSaLen = SockLen(sizeof(gAcceptSa))
  sqe.opcode = IORING_OP_ACCEPT
  sqe.flags = IOSQE_FIXED_FILE
  sqe.fd = ListenSlot
  sqe.`addr` = cast[uint64](addr gAcceptSa)
  sqe.off = cast[uint64](addr gAcceptSaLen)
  if gAcceptDirect and gAcceptMultishot:
    # Direct descriptor: the kernel installs the accepted socket into the first
    # free registered-file slot and returns that SLOT in the CQE's `res` (field
    # is the splice_fd_in/file_index union at Sqe offset 44). SOCK_CLOEXEC is
    # rejected (-EINVAL) here because a fixed descriptor has no process fd, so
    # only SOCK_NONBLOCK is requested; the non-direct path keeps both.
    sqe.rw_flags = uint32(SOCK_NONBLOCK)
    sqe.splice_fd_in = cast[int32](IORING_FILE_INDEX_ALLOC)
  else:
    sqe.rw_flags = uint32(SOCK_NONBLOCK or SOCK_CLOEXEC)
  if gAcceptMultishot:
    sqe.ioprio = uint16(IORING_ACCEPT_MULTISHOT)
  sqe.user_data = makeUd(TAG_ACCEPT, 0, 0)
  gAcceptArmed = true

proc armRecv(slot: cint) =
  ## Arm (or, in multishot mode, leave armed) a recv that selects its landing
  ## buffer from the shared provided-buffer pool. `IOSQE_BUFFER_SELECT` makes
  ## the kernel pick a buffer; the CQE then carries its id in the high 16 bits
  ## of cqe->flags.
  if slot < 0:
    return
  var c = addr conns[int(slot)]
  if not c.active or c.closing or c.recvArmed:
    return
  let sqe = mustSqe()
  if sqe.isNil:
    return
  sqe.opcode = IORING_OP_RECV
  sqe.flags = IOSQE_FIXED_FILE or IOSQE_BUFFER_SELECT
  sqe.fd = slot
  sqe.buf_index = BufGroup
  sqe.len = uint32(BufSize)
  if gRecvMultishot:
    sqe.ioprio = uint16(IORING_RECV_MULTISHOT)
  sqe.user_data = makeUd(TAG_RECV, slot, c.gen)
  c.recvArmed = true
  inc c.pending

proc rearmStarved() =
  ## Re-arm every recv that ended with -ENOBUFS, now that buffers have returned.
  ## Only called when at least one buffer is available (gBufsReady > 0), so it
  ## cannot spin on a still-empty pool.
  if gRecvStarved.len == 0:
    return
  let starved = gRecvStarved
  gRecvStarved.setLen(0)
  for sr in starved:
    let slot = sr.slot
    if slot < 0 or int(slot) >= conns.len:
      continue
    var c = addr conns[int(slot)]
    if c.gen != sr.gen:
      continue                      # slot reused by another connection: stale
    if c.active and not c.closing and not c.recvArmed and not c.readPaused:
      armRecv(slot)

proc armSend(slot: cint) =
  if slot < 0:
    return
  var c = addr conns[int(slot)]
  if not c.active or c.sendArmed:
    return
  if c.outOff >= c.outbuf.len:
    return
  let sqe = mustSqe()
  if sqe.isNil:
    return
  sqe.opcode = IORING_OP_SEND
  sqe.flags = IOSQE_FIXED_FILE
  sqe.fd = slot
  sqe.`addr` = cast[uint64](addr c.outbuf[c.outOff])
  sqe.len = uint32(c.outbuf.len - c.outOff)
  sqe.rw_flags = uint32(MSG_NOSIGNAL)
  sqe.user_data = makeUd(TAG_SEND, slot, c.gen)
  c.sendArmed = true
  inc c.pending

proc armSendOrDefer(slot: cint) =
  ## Arm a SEND for freshly produced output, unless that output was produced
  ## while a group-commit batch is open: then the response must wait for COMMIT,
  ## so remember the slot and let `flushDeferredSlots` arm the send afterwards.
  if slot < 0:
    return
  var c = addr conns[int(slot)]
  if not c.active or c.sendArmed:
    return
  if c.outOff >= c.outbuf.len:
    return
  if gDb != nil and gDb.batchOpen() and c.batchCount > 0:
    if not c.deferred:
      c.deferred = true
      gDeferredSlots.add (slot: slot, gen: c.gen)
    return
  armSend(slot)

proc cancelOp(slot: cint, tag: uint64, gen: uint32) =
  ## Cancel an in-flight op by its user_data so the socket's last reference can
  ## be dropped and the fixed slot reused.
  let sqe = mustSqe()
  if sqe.isNil:
    return
  sqe.opcode = IORING_OP_ASYNC_CANCEL
  sqe.`addr` = makeUd(tag, slot, gen)
  sqe.rw_flags = IORING_ASYNC_CANCEL_USERDATA
  sqe.user_data = TAG_CANCEL

proc disarmRecv(slot: cint) =
  ## Stop a still-armed (multishot) recv so it cannot keep delivering data after
  ## the connection paused reading or decided to close. The op stays counted in
  ## `pending` until its -ECANCELED completion arrives.
  if slot < 0 or int(slot) >= conns.len:
    return
  var c = addr conns[int(slot)]
  if c.recvArmed:
    c.recvArmed = false
    cancelOp(slot, TAG_RECV, c.gen)

proc armTimeout() =
  let sqe = mustSqe()
  if sqe.isNil:
    return
  gTs.tv_sec = int64(TickSeconds)
  gTs.tv_nsec = 0
  sqe.opcode = IORING_OP_TIMEOUT
  sqe.fd = -1'i32
  sqe.`addr` = cast[uint64](addr gTs)
  sqe.len = 1'u32
  sqe.off = 0
  sqe.user_data = TAG_TIMEOUT

# ---------------------------------------------------------------------------
# connection lifecycle

proc closeConn(slot: cint) =
  if slot < 0 or int(slot) >= conns.len:
    return
  var c = addr conns[int(slot)]
  if not c.active:
    return
  let gen = c.gen
  if c.recvArmed:
    cancelOp(slot, TAG_RECV, gen)
  if c.sendArmed:
    cancelOp(slot, TAG_SEND, gen)
  # Drop the table's reference; the socket closes once in-flight ops finish.
  discard updateSlotRaw(slot, -1'i32)
  # Reset the slot (releasing the buffers via ARC) but KEEP `gen` and `pending`:
  # the ops cancelled above still complete and must (a) look current so they
  # decrement `pending`, and (b) keep the slot off the free list until they do.
  # wantFree arms that release once `pending` hits zero.
  let pend = c.pending
  conns[int(slot)] = Conn(gen: gen, pending: pend, st: {cfWantFree})
  maybeFreeSlot(slot)

proc finalizeConn(slot: cint) =
  if slot < 0:
    return
  var c = addr conns[int(slot)]
  if not c.active:
    return
  if c.closing and not c.sendArmed and c.outOff >= c.outbuf.len:
    closeConn(slot)

# ---------------------------------------------------------------------------
# completion handlers

proc initAcceptedSlot(slot: cint) =
  ## Bring a freshly acquired fixed-file slot into service.  Recv storage is
  ## pooled, so there is nothing per-connection to allocate here.
  inc gGen
  conns[int(slot)] = Conn(gen: gGen, st: {cfActive, cfKeepAlive},
                          lastActive: gNow)

proc onAccept(res: cint, more: bool) =
  ## `more` mirrors IORING_CQE_F_MORE: while set, a multishot accept is still
  ## armed and must NOT be re-submitted.  On success `res` is either an OS fd
  ## (register-per-accept path) or a fixed-file SLOT index (direct path).
  if not more:
    gAcceptArmed = false
  if res >= 0:
    var slot: cint
    if gAcceptDirect:
      # Direct accept: the kernel already installed the socket into a free
      # fixed-file slot, and `res` IS that slot. No allocSlot(), no
      # FILES_UPDATE, no per-accept fd to close.
      slot = cint(res)
      if slot <= 0 or int(slot) >= conns.len or conns[int(slot)].active:
        stderr.writeLine("io_uring: direct accept returned unusable slot " & $slot)
        if slot > 0 and int(slot) < conns.len:
          discard updateSlotRaw(slot, -1'i32)   # drop the kernel-installed file
        if not more:
          armAccept()
        return
      gAcceptDirectProven = true
    else:
      slot = allocSlot()
      if slot < 0:
        discard posix.close(res)
        gAcceptPaused = true          # retry on the next tick
        return
      if updateSlotRaw(slot, res) < 0:
        stderr.writeLine("io_uring: register update failed; dropping accept")
        discard posix.close(res)
        gFreeSlots[gFreeTop] = slot; inc gFreeTop
        if not more:
          armAccept()
        return
      # Registered: the fixed table now holds its own reference, so release the
      # process fd immediately (this is the RLIMIT_NOFILE bypass).
      applyTcpTuning(res)
      discard posix.close(res)

    gAcceptMsProven = true
    initAcceptedSlot(slot)
    armRecv(slot)
    if not more:
      armAccept()
  else:
    if res == -EAGAIN or res == -EINTR:
      armAccept()
    elif res == -EINVAL or res == -EOPNOTSUPP:
      if gAcceptDirect and gAcceptMultishot and not gAcceptDirectProven:
        # Kernel refuses IORING_FILE_INDEX_ALLOC direct accept: keep multishot
        # but go back to registering each accepted fd into a free slot.
        gAcceptDirect = false
        rebuildFreeList()
        stderr.writeLine("io_uring: direct accept (FILE_INDEX_ALLOC) unsupported (" &
                         $res & "); falling back to register-per-accept")
        armAccept()
      elif gAcceptMultishot and not gAcceptMsProven:
        # Kernel refuses IORING_ACCEPT_MULTISHOT: degrade to a re-armed one-shot
        # accept. This is the only way to detect it (accept flags are not
        # covered by IORING_REGISTER_PROBE).
        gAcceptMultishot = false
        gAcceptDirect = false
        stderr.writeLine("io_uring: multishot accept unsupported (" & $res &
                         "); falling back to re-armed accept")
        armAccept()
      else:
        gAcceptPaused = true
    elif res == -ECANCELED:
      discard
    else:
      # EMFILE/ENFILE/ECONNABORTED/... : back off; the tick revives the accept.
      gAcceptPaused = true

proc onRecv(slot: cint, gen: uint32, res: cint, cflags: uint32) =
  ## One recv completion. In multishot mode the same SQE yields many of these
  ## (each with IORING_CQE_F_MORE); only the terminal one clears `pending`. In
  ## both modes the data lives in a provided buffer whose id is in the high 16
  ## bits of `cflags`, and which is returned to the pool before we leave.
  if slot < 0 or int(slot) >= conns.len:
    return
  let hasBuf = (cflags and IORING_CQE_F_BUFFER) != 0'u32
  let more   = (cflags and IORING_CQE_F_MORE) != 0'u32
  var bid = -1
  if hasBuf:
    bid = int((cflags shr uint32(IORING_CQE_BUFFER_SHIFT)) and 0xFFFF'u32)
    poolConsume()

  if conns[int(slot)].gen != gen:
    # Stale completion for a reused slot: only the buffer needs returning.
    if bid >= 0: poolPush(bid)
    return

  var c = addr conns[int(slot)]
  if not more:
    # Terminal completion: this recv op is no longer armed.
    c.recvArmed = false
    dec c.pending

  if not c.active:
    if bid >= 0: poolPush(bid)
    maybeFreeSlot(slot)
    return
  c.lastActive = gNow

  if res > 0:
    if c.closing:
      if bid >= 0: poolPush(bid)      # discard data for a closing conn
      return
    let n = int(res)
    let old = c.inbuf.len
    c.inbuf.setLen(old + n)
    if bid >= 0:
      copyMem(addr c.inbuf[old], addr gBufBase[bid * BufSize], n)
      poolPush(bid)
    # Stop reading (do not close) once the buffer passes the request cap so a
    # pipelined burst drains; only a lone oversized request/header is rejected.
    if c.inbuf.len > MaxRequest:
      if findHeaderEnd(c.inbuf) >= 0:
        c.readPaused = true
      else:
        c.closing = true
    if not c.closing and not c.sendArmed:
      if not processRequests(c[]):
        c.closing = true
      if c.inbuf.len <= MaxRequest:
        c.readPaused = false
      # Defer the SEND when this pass opened a write batch (see armSendOrDefer).
      armSendOrDefer(slot)
    if not c.closing and not c.readPaused:
      armRecv(slot)          # no-op while a multishot op is still armed
    elif c.recvArmed:
      disarmRecv(slot)
    finalizeConn(slot)
    return

  # Non-data completion.
  if bid >= 0: poolPush(bid)
  if res == 0:
    # Peer closed its write side: flush what we have, then close. Defer the send
    # if this pass opened a write batch, so the response still waits for COMMIT.
    c.closing = true
    if not c.sendArmed:
      armSendOrDefer(slot)
    finalizeConn(slot)
    return
  # res < 0
  if res == -ENOBUFS:
    # The pool is momentarily empty. If a buffer is already available, re-arm
    # now; otherwise remember the slot and re-arm once buffers return (end of
    # pass / tick), which prevents a submit-and-immediately-ENOBUFS spin.
    if gBufsReady > 0:
      armRecv(slot)
    else:
      gRecvStarved.add (slot: slot, gen: c.gen)
    return
  if res == -EAGAIN or res == -EINTR:
    armRecv(slot)
    return
  if res == -ECANCELED or res == -ENOENT:
    return
  c.closing = true
  if not c.sendArmed:
    armSendOrDefer(slot)   # wait for COMMIT if a write batch is open
  finalizeConn(slot)

proc onSend(slot: cint, gen: uint32, res: cint) =
  if slot < 0 or int(slot) >= conns.len:
    return
  if conns[int(slot)].gen != gen:
    return
  dec conns[int(slot)].pending
  if not conns[int(slot)].active:
    maybeFreeSlot(slot)
    return
  var c = addr conns[int(slot)]
  c.sendArmed = false
  c.lastActive = gNow

  if res > 0:
    c.outOff += int(res)
  else:
    if res == -EAGAIN:
      armSend(slot)
      return
    if res == -ENOBUFS:
      # Transient kernel allocation failure: retry on the next tick rather than
      # spin, so the response is recovered instead of the connection dropped.
      c.sendArmed = false
      gSendRetry.add (slot: slot, gen: c.gen)
      return
    if res == -ECANCELED or res == -ENOENT:
      return
    closeConn(slot)
    return

  if c.outOff >= c.outbuf.len:
    c.outbuf.setLen(0)
    c.outOff = 0
    # Answer any request that arrived while the previous response was in flight.
    if not c.closing and c.inbuf.len > 0:
      if not processRequests(c[]):
        c.closing = true
      if c.inbuf.len <= MaxRequest:
        c.readPaused = false
      # Defer the SEND when this pass opened a write batch (see armSendOrDefer).
      armSendOrDefer(slot)
    if c.closing and not c.sendArmed and c.outOff >= c.outbuf.len:
      closeConn(slot)
      return
    if not c.closing and not c.readPaused:
      armRecv(slot)
  else:
    armSend(slot)   # partial send: push the remainder

proc sweep()   # forward declaration (defined below)

proc onTimeout() =
  # Idle/stall sweep moved here from the per-pass hot path: walking every
  # registered slot on each completion batch was pure overhead; the 1 s timer
  # is the natural sweep cadence.
  sweep()
  # Retry sends that hit a transient -ENOBUFS (never dropped, never spun on).
  if gSendRetry.len > 0:
    let pend = gSendRetry
    gSendRetry.setLen(0)
    for sr in pend:
      let slot = sr.slot
      if slot >= 0 and int(slot) < conns.len:
        var c = addr conns[int(slot)]
        if c.gen == sr.gen and c.active and not c.sendArmed and
           c.outOff < c.outbuf.len:
          armSend(slot)
  # Re-arm recvs starved of provided buffers.
  if gRecvStarved.len > 0 and gBufsReady > 0:
    rearmStarved()
  if gAcceptPaused:
    gAcceptPaused = false
    armAccept()
  elif not gAcceptArmed:
    armAccept()
  armTimeout()

# ---------------------------------------------------------------------------
# idle / stall sweep

proc sweep() =
  for slot in 0 ..< conns.len:
    if conns[slot].active:
      let pendingOut = conns[slot].sendArmed or
                       conns[slot].outOff < conns[slot].outbuf.len
      let midReq = conns[slot].inbuf.len > 0
      let limit = if pendingOut or midReq: StallTimeout else: IdleKeepAlive
      if gNow - conns[slot].lastActive > limit:
        closeConn(cint(slot))

# ---------------------------------------------------------------------------
# reaping

proc dispatch(ud: uint64, res: cint, cflags: uint32) =
  case udTag(ud)
  of TAG_ACCEPT:  onAccept(res, (cflags and IORING_CQE_F_MORE) != 0'u32)
  of TAG_RECV:    onRecv(udSlot(ud), udGen(ud), res, cflags)
  of TAG_SEND:    onSend(udSlot(ud), udGen(ud), res)
  of TAG_TIMEOUT: onTimeout()
  of TAG_CANCEL:  discard
  of TAG_PROVIDE: discard
  else:           discard

proc flushDeferredSlots(commitOk: bool) =
  ## Release the SENDs withheld during this pass, once the pass's batch has been
  ## committed (or has failed). On a failed COMMIT every response produced against
  ## the rolled-back batch is replaced by a 500, so no success byte for an
  ## uncommitted write can reach the socket.
  for sr in gDeferredSlots:
    let slot = sr.slot
    if slot < 0 or int(slot) >= conns.len:
      continue
    var c = addr conns[int(slot)]
    if c.gen != sr.gen:
      continue                      # slot reused by another connection: stale
    c.deferred = false
    if not c.active:
      continue
    if not commitOk and c.batchCount > 0:
      # Never touch outbuf while a SEND is in flight: the kernel holds
      # `addr c.outbuf[c.outOff]`, so setLen/add would reallocate the storage out
      # from under the op (use-after-free). A deferred slot is only queued while
      # no send is armed, so the UAF cannot occur here.
      c.outbuf.setLen(c.batchFrom)
      for _ in 0 ..< c.batchCount:
        c.outbuf.addResponse(500, """{"error":"internal server error"}""",
                             c.keepAlive)
      c.closing = true
    c.batchFrom = 0
    c.batchCount = 0
    if c.outOff < c.outbuf.len:
      armSend(slot)
    if c.closing and not c.sendArmed and c.outOff >= c.outbuf.len:
      closeConn(slot)
  gDeferredSlots.setLen(0)

proc runLoop() =
  ## One thread, one ring. Per pass we drain the *entire* completion queue in a
  ## single loop, do the send-side work, then loop. The end-of-pass submit is
  ## deliberately omitted: the wait at the top of the next iteration flushes
  ## every SQE reserved during the drain, so the steady-state hot loop costs
  ## exactly ONE io_uring_enter per pass (plus whatever `mustSqe()` is forced to
  ## flush if the SQ genuinely fills mid-drain).
  ##
  ## The wait is the blocking GETEVENTS enter, so the loop sleeps until a
  ## completion is ready. The armed 1 s tick CQE bounds the idle wakeup, and the
  ## completion queue is drained whole, so the tick CQE is processed exactly as
  ## before and the idle/stall sweep keeps its cadence.
  armAccept()
  armTimeout()

  while true:
    try:
      discard gRing.submitAndWait(1)   # flush reserved SQEs + block for >=1 CQE
    except IoRingError as ex:
      if ex.errno == EINTR:
        continue
      stderr.writeLine("io_uring_enter failed: " & ex.msg)
      quit(1)

    gNow = epochTime()
    gConsumedThisPass = 0
    # Drain the *entire* completion queue.  If the kernel reports
    # IORING_SQ_CQ_OVERFLOW it parked completions that did not fit in the CQ ring
    # on its internal overflow list; re-enter (non-blocking GETEVENTS) to move
    # them into the space we just freed, then drain again, until the flag clears.
    # The flag is only set when the CQ actually filled, so the common case costs
    # one `sqOverflowed()` read per pass.  This never blocks: `enterGetEvents`
    # uses min_complete=0, so the loop cannot sleep while overflowed.
    while true:
      while true:
        let cqe = gRing.peekCqe()
        if cqe.isNil:
          break
        let ud = cqe.user_data
        let res = cqe.res
        let fl = cqe.flags
        gRing.cqeSeen()
        dispatch(ud, res, fl)
      if not gRing.sqOverflowed():
        break
      # CQ overflowed: nudge the kernel to flush the overflow list into the
      # CQ space freed above, then drain those completions too.
      try:
        discard gRing.enterGetEvents()
      except IoRingError as ex:
        if ex.errno == EINTR:
          continue
        stderr.writeLine("io_uring_enter failed: " & ex.msg)
        quit(1)

    # End of one completion-queue pass: commit the pass's write batch (if any)
    # and only then arm the SENDs withheld for it (rule 6: durable before send).
    if gDb != nil and gDb.batchOpen():
      flushDeferredSlots(gDb.commitBatch())

    # Checkpoint the WAL off the request path, at most once a second.
    if gNow - gLastCkpt >= 1.0:
      gLastCkpt = gNow
      if gDb != nil:
        gDb.checkpointPassive()

    # Buffers returned during this pass may have un-starved recvs that hit
    # -ENOBUFS earlier in the same pass.
    if gRecvStarved.len > 0 and gConsumedThisPass > 0 and gBufsReady > 0:
      rearmStarved()

# ---------------------------------------------------------------------------
# setup

proc groupCommitEnabled(): bool =
  ## GROUP_COMMIT defaults ON; only "0" (or unparseable -> default) is treated
  ## as a disable request, matching envIntDef semantics.
  try:
    result = parseInt(getEnv("GROUP_COMMIT", "1")) != 0
  except ValueError:
    result = true

proc envIsOneshot(name: string): bool =
  ## IOURING_RECV / IOURING_ACCEPT: default "multishot"; anything else (notably
  ## "oneshot") selects the conservative one-shot path.
  getEnv(name, "multishot").toLowerAscii == "oneshot"

proc initPool() =
  ## Allocate the shared provided-buffer pool and publish it to the kernel.
  ## The data region is one lazily-faulted `alloc`: untouched pages stay out of
  ## RSS, and because buffers are only taken while bytes are in flight, the
  ## resident cost tracks peak concurrent receives rather than connection count.
  # Live pool size (IOURING_BUFS, default BufRingDefault): rounded down to a power
  # of two and clamped, so the buf-ring requirement always holds. Exposed so the
  # -ENOBUFS path can be exercised (a tiny pool) without a rebuild.
  var want = BufRingDefault
  try:
    want = parseInt(getEnv("IOURING_BUFS", $BufRingDefault))
  except ValueError:
    want = BufRingDefault
  if want < 64: want = 64
  if want > BufRingMax: want = BufRingMax
  var p2 = 1
  while p2 * 2 <= want: p2 *= 2
  gBufSlots = p2
  gBufMask = p2 - 1

  let bytes = gBufSlots * BufSize
  gBufBase = cast[ptr UncheckedArray[char]](alloc(bytes))
  if gBufBase.isNil:
    stderr.writeLine("io_uring: buffer pool alloc failed (" & $bytes & " bytes)")
    quit(1)

  # The descriptor ring must be page aligned; mmap gives us that for free.
  let ringBytes = gBufSlots * sizeof(BufRingEntry)
  let p = posix.mmap(nil, ringBytes, PROT_READ or PROT_WRITE,
                     MAP_PRIVATE or MAP_ANONYMOUS, -1, Off(0))
  if p.isNil or p == MAP_FAILED:
    stderr.writeLine("io_uring: buf-ring mmap failed (" & $ringBytes & " bytes)")
    quit(1)
  gBufRing = cast[ptr UncheckedArray[BufRingEntry]](p)

  # IOURING_BUFRING=0 forces the IORING_OP_PROVIDE_BUFFERS path (for testing the
  # degradation); otherwise we try the registered ring and fall back on failure.
  if getEnv("IOURING_BUFRING", "1") == "0":
    gUseBufRing = false
    stderr.writeLine("io_uring: IOURING_BUFRING=0; using IORING_OP_PROVIDE_BUFFERS")
  else:
    try:
      discard gRing.registerBufRing(p, uint32(gBufSlots), BufGroup)
      gUseBufRing = true
    except IoRingError as ex:
      gUseBufRing = false
      stderr.writeLine("io_uring: PBUF_RING unavailable (" & ex.msg &
                       "); falling back to IORING_OP_PROVIDE_BUFFERS")

  gBufTail = 0
  gBufsReady = 0
  if gUseBufRing:
    for bid in 0 ..< gBufSlots:
      poolPush(bid)
  else:
    # One SQE hands the whole list to buffer group BufGroup, ids 0..N-1.
    let sqe = mustSqe()
    if sqe.isNil:
      stderr.writeLine("io_uring: could not submit PROVIDE_BUFFERS")
      quit(1)
    prepProvideBuffers(sqe, cast[uint64](addr gBufBase[0]), uint32(BufSize),
                       uint32(gBufSlots), 0'u16, BufGroup)
    sqe.user_data = TAG_PROVIDE
    gBufsReady = gBufSlots
    discard gRing.submit()

  stderr.writeLine("io_uring pool: " & $gBufSlots & " x " & $BufSize &
                   " B (" & $(bytes div (1024 * 1024)) & " MiB, " &
                   (if gUseBufRing: "registered buf_ring"
                    else: "provide_buffers list") & ")")

# ---------------------------------------------------------------------------
# Startup failure policy.
#
# Every per-feature failure (flagged setup, multishot recv, multishot/direct
# accept, buf_ring) degrades in place with the server staying up. The only
# unrecoverable case is a failure that leaves the transport itself unavailable:
# `io_uring_setup` rejecting the ring (-ENOSYS on a kernel/VM without io_uring,
# -EPERM under seccomp, -ENOMEM for the ring) or the fixed-file registration
# being refused even at the floor. There is exactly one transport, so there is
# no alternative to hand off to: log a clear message and exit non-zero, so the
# failure is immediate and observable rather than a silently degraded server.
proc hardFail(reason: string) {.noreturn.} =
  stderr.writeLine("fatal: io_uring transport unavailable (" & reason &
                   "); no fallback transport is built in")
  quit(1)

proc main() =
  let cfg = loadConfig()
  gSecret = cfg.jwtSecret

  discard posix.signal(SIGPIPE, SIG_IGN)

  # Turbo toggles: multishot by default, one env var away from the safe path.
  gRecvMultishot = not envIsOneshot("IOURING_RECV")
  gAcceptMultishot = not envIsOneshot("IOURING_ACCEPT")
  # Direct accept (kernel-installed fixed slots) defaults ON;
  # IOURING_ACCEPT_DIRECT=0 forces the register-per-accept path so that path can
  # be exercised.
  gAcceptDirect = getEnv("IOURING_ACCEPT_DIRECT", "1") != "0"
  # Direct accept installs the socket kernel-side, so no userspace fd exists to
  # run applyTcpTuning on it. If per-connection TCP tuning is requested, fall
  # back to register-per-accept so the accepted fd is available.
  if not gAcceptMultishot or tcpNotsentLowatBytes() > 0 or tcpQuickAck():
    gAcceptDirect = false

  # Raise the fd ceiling BEFORE registering the fixed-file table (see raiseNofile).
  # The hard limit it returns sizes the registered fixed-file table below.
  let hardNofile = raiseNofile()

  # ---- runtime fixed-file table size --------------------------------------
  # Registering N fixed files counts N against RLIMIT_NOFILE, so the table
  # cannot simply equal the hard limit: reserve headroom for the process's own
  # descriptors (stdio, io_uring ring, the listener before it is closed, the
  # sqlite db) or IORING_REGISTER_FILES fails outright - 65536 entries under a
  # 65535 hard limit is exactly that case. CAP bounds the table; SlotsFloor is
  # the smallest size we will run with.
  if hardNofile > 0:
    gSlots = min(hardNofile - FdsReserve, CAP)
  if gSlots < SlotsFloor:
    gSlots = SlotsFloor

  # ---- the ring -----------------------------------------------------------
  # SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN: the recommended setup for a
  # single-threaded, per-thread ring. Completion (task) work is deferred to
  # io_uring_enter, which runLoop calls every pass. A kernel that rejects the
  # flags is retried bare inside setupQueue, so startup never fails on a hint.
  const RingSetupFlags = IORING_SETUP_SINGLE_ISSUER or
                         IORING_SETUP_DEFER_TASKRUN or
                         IORING_SETUP_COOP_TASKRUN
  try:
    gRing = setupQueue(RingEntries, RingSetupFlags,
                       cuint(RingEntries * CqMultiple))
  except IoRingError as ex:
    # Unrecoverable: the ring itself is unavailable.
    hardFail(ex.msg)
  if gRing.flagsDropped:
    stderr.writeLine("io_uring setup: flags 0x" & toHex(gRing.requestedFlags) &
                     " rejected; running WITHOUT SINGLE_ISSUER|DEFER_TASKRUN")
  else:
    stderr.writeLine("io_uring setup: flags 0x" & toHex(gRing.usedFlags) &
                     " in effect (SINGLE_ISSUER|DEFER_TASKRUN|COOP_TASKRUN)")

  # CQ headroom, observed from the kernel (not assumed): the params the kernel
  # filled in report the SQ/CQ ring sizes it actually allocated.
  stderr.writeLine("io_uring ring: sq_entries=" & $gRing.params.sq_entries &
                   " cq_entries=" & $gRing.params.cq_entries &
                   " (CQSIZE request=" & $(RingEntries * CqMultiple) &
                   ", kernel default=2x sq=" & $(RingEntries * 2) & ")")

  # ---- the provided-buffer pool -------------------------------------------
  initPool()

  # ---- listener -----------------------------------------------------------
  # Shared socket/bind/listen. deferSec=1 enables TCP_DEFER_ACCEPT: the kernel
  # holds a completed handshake back from accept until the client's first byte
  # arrives, so a silent/impatient connect never wakes the loop. HTTP clients
  # send the request immediately, so a real request is accepted with no added
  # latency, and this is a listen-time option only (no per-connection work).
  let lfd = setupListenSocket(cfg.host, cfg.port, ListenBacklog, 1)
  if lfd < 0:
    quit(1)

  # ---- register the fixed-file table: slot 0 = listener, rest empty --------
  # `gSlots` was already sized from RLIMIT_NOFILE above, but the kernel can still
  # refuse the top value (e.g. -EMFILE/-EINVAL). Do NOT quit at the first failure
  # and do NOT halve (halving is how you land back on the removed 32768 cap):
  # walk the size DOWN in small steps until the kernel accepts it, never below
  # `SlotsFloor`. Only if even the floor is refused is the transport unusable.
  while true:
    var fds = newSeq[cint](gSlots)
    for i in 0 ..< gSlots:
      fds[i] = -1'i32
    fds[0] = lfd
    try:
      discard gRing.registerFiles(fds)
      stderr.writeLine("fixed-file table registered: fixed_file_slots=" & $gSlots)
      break
    except IoRingError as ex:
      stderr.writeLine("IORING_REGISTER_FILES(" & $gSlots & ") failed: " & ex.msg)
      if gSlots <= SlotsFloor:
        # Nothing left to give up; the fixed-file transport is unusable here.
        hardFail("IORING_REGISTER_FILES failed at floor " & $SlotsFloor)
      # Small step DOWN (~1.5% or 1024 entries, whichever shrinks more), floored.
      let shrink = max(gSlots div 64, 1024)
      gSlots = max(SlotsFloor, gSlots - shrink)
  # The registered reference keeps the listening socket alive, so the process
  # fd can be released: accepts then address it as fixed slot 0.
  discard posix.close(lfd)

  # ---- database -----------------------------------------------------------
  gDb = openDb(cfg.sqlitePath)
  # Write batching across one completion-queue pass; GROUP_COMMIT=0 restores
  # autocommit. Default ON.
  gDb.setGroupCommit(groupCommitEnabled())
  markStart()

  # ---- slot pools ---------------------------------------------------------
  conns = newSeq[Conn](gSlots)
  gFreeSlots = newSeq[cint](gSlots - 1)
  for i in 1 ..< gSlots:
    gFreeSlots[i - 1] = cint(i)
  gFreeTop = gSlots - 1

  stderr.writeLine("io_uring transport: entries=" & $RingEntries &
                   " fixed_file_slots=" & $gSlots &
                   " recv=" & (if gRecvMultishot: "multishot" else: "oneshot") &
                   " accept=" & (if not gAcceptMultishot: "oneshot"
                                elif gAcceptDirect: "multishot+direct"
                                else: "multishot"))
  stderr.writeLine("slot array: sizeof(conn)=" & $sizeof(Conn) &
                   " slots=" & $gSlots &
                   " bytes=" & $(gSlots * sizeof(Conn)))

  runLoop()

main()
