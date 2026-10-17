## iouring.nim: self-contained raw io_uring bindings for Nim.
##
## This module talks to the io_uring kernel interface directly through the
## three raw syscalls.  It does **not** link or include liburing in any way;
## the only headers it touches are the UAPI header (`<sys/syscall.h>` for the
## syscall numbers) and plain libc entry points (`syscall`, `mmap`, `munmap`).
##
## ================================ mmap layout ================================
##
## io_uring_setup(2) returns a ring file descriptor and fills an
## `IoUringParams` describing the rings.  The application obtains three (or
## two) memory regions by mmap(2)-ing the ring fd at magic offsets:
##
##   IORING_OFF_SQ_RING   0x0000_0000   SQ ring: head/tail/mask/entries/flags/
##                                      dropped + the sq_array of SQE indices
##   IORING_OFF_CQ_RING   0x0800_0000   CQ ring: head/tail/mask/entries/
##                                      overflow + the CQE array
##   IORING_OFF_SQES      0x1000_0000   the SQE array itself (sq_entries * 64B)
##
## If the kernel advertises `IORING_FEAT_SINGLE_MMAP`, the SQ and CQ rings are
## mapped by a *single* mmap at IORING_OFF_SQ_RING; the CQ fields then live at
## `IoUringParams.cq_off.*` inside that same mapping.  We honour that.
##
## Every field's *byte offset inside its mapping* comes from the kernel via
## `sq_off`/`cq_off` in `IoUringParams`; we never hardcode them.  What this
## module does hardcode (and statically asserts) is the in-memory layout of the
## three ABI structs, mirrored byte-for-byte from `<linux/io_uring.h>`.
##
## ============================== SQE/CQE protocol =============================
##
##  1. Caller reserves a slot with `getSqe`.  We zero the 64-byte slot, write
##     its index into `sq_array[sq_tail & ring_mask]`, and advance the shared
##     `sq_tail`.  Advancing the tail before `enter` is safe because the ring is
##     not in SQPOLL mode: the kernel only observes SQEs at `io_uring_enter`.
##  2. `submit` flushes `to_submit` SQEs via
##     `io_uring_enter(fd, to_submit, min_complete=0, flags=0, ...)`.
##     `submitAndWait` additionally sets `IORING_ENTER_GETEVENTS` and blocks
##     until `min_complete` completions are ready.
##  3. Completions are read with `peekCqe` (returns nil when none) and consumed
##     with `cqeSeen`, which advances the kernel-visible `cq_head`.
##
## `io_uring_sqe` is exactly 64 bytes and `io_uring_cqe` exactly 16 bytes; both
## are asserted below at compile time.
##
## Requires: Linux kernel with io_uring (5.1+).  On x86_64 the syscall numbers
## are 425/426/427 as provided by `<sys/syscall.h>`.

import std/oserrors

# ---------------------------------------------------------------------------
# libc entry points
# ---------------------------------------------------------------------------

proc c_syscall(number: clong): clong
  {.importc: "syscall", header: "<unistd.h>", varargs.}

proc c_mmap(address: pointer, length: csize_t, prot: cint, flags: cint,
            fd: cint, offset: clong): pointer
  {.importc: "mmap", header: "<sys/mman.h>".}

proc c_munmap(address: pointer, length: csize_t): cint
  {.importc: "munmap", header: "<sys/mman.h>".}

proc c_close(fd: cint): cint
  {.importc: "close", header: "<unistd.h>".}

# Syscall numbers, resolved by the C preprocessor from <sys/syscall.h>.
var SYS_io_uring_setup {.importc, header: "<sys/syscall.h>".}: clong
var SYS_io_uring_enter {.importc, header: "<sys/syscall.h>".}: clong
var SYS_io_uring_register {.importc, header: "<sys/syscall.h>".}: clong

# ---------------------------------------------------------------------------
# ABI structures: mirrored from <linux/io_uring.h>
# ---------------------------------------------------------------------------

type
  Sqe* {.bycopy.} = object
    ## struct io_uring_sqe: 64 bytes.
    opcode*: uint8          ## offset 0
    flags*: uint8           ## offset 1
    ioprio*: uint16         ## offset 2
    fd*: int32              ## offset 4
    off*: uint64            ## offset 8  (union off / addr2 / cmd_op)
    `addr`*: uint64         ## offset 16 (union addr / splice_off_in / level)
    len*: uint32            ## offset 24
    rw_flags*: uint32       ## offset 28 (union of many *_flags fields)
    user_data*: uint64      ## offset 32
    buf_index*: uint16      ## offset 40 (union buf_index / buf_group, packed)
    personality*: uint16    ## offset 42
    splice_fd_in*: int32    ## offset 44 (union splice_fd_in / file_index)
    addr3*: uint64          ## offset 48
    pad2*: uint64           ## offset 56

  Cqe* {.bycopy.} = object
    ## struct io_uring_cqe: 16 bytes.
    user_data*: uint64      ## offset 0
    res*: int32             ## offset 8
    flags*: uint32          ## offset 12

  SqrOff* {.bycopy.} = object
    ## struct io_sqring_offsets
    head*: uint32
    tail*: uint32
    ring_mask*: uint32
    ring_entries*: uint32
    flags*: uint32
    dropped*: uint32
    array*: uint32
    resv1*: uint32
    user_addr*: uint64

  CqrOff* {.bycopy.} = object
    ## struct io_cqring_offsets
    head*: uint32
    tail*: uint32
    ring_mask*: uint32
    ring_entries*: uint32
    overflow*: uint32
    cqes*: uint32
    flags*: uint32
    resv1*: uint32
    user_addr*: uint64

  IoUringParams* {.bycopy.} = object
    ## struct io_uring_params: 120 bytes.
    sq_entries*: uint32
    cq_entries*: uint32
    flags*: uint32
    sq_thread_cpu*: uint32
    sq_thread_idle*: uint32
    features*: uint32
    wq_fd*: uint32
    resv*: array[3, uint32]
    sq_off*: SqrOff
    cq_off*: CqrOff

  BufRingEntry* {.bycopy.} = object
    ## struct io_uring_buf: 16 bytes.  `io_uring_buf_ring` is a union of a
    ## header {resv1,resv2,resv3,tail} and `struct io_uring_buf bufs[]`; the
    ## header's `tail` (u16 at byte offset 14) overlays `bufs[0].resv`, so an
    ## array of these entries is layout-identical to the kernel's buf ring and
    ## the userspace producer advances the tail at `ring[0].resv`.
    `addr`*: uint64         ## offset 0
    len*: uint32            ## offset 8
    bid*: uint16            ## offset 12
    resv*: uint16           ## offset 14 (== the ring `tail` for entry 0)

  BufReg* {.bycopy.} = object
    ## struct io_uring_buf_reg: 40 bytes (argument to IORING_REGISTER_PBUF_RING).
    ring_addr*: uint64      ## offset 0
    ring_entries*: uint32   ## offset 8
    bgid*: uint16           ## offset 12
    flags*: uint16          ## offset 14
    resv*: array[3, uint64] ## offset 16

# Compile-time ABI contract.  If the compiler (or the platform) lays these out
# differently than the kernel expects, the module refuses to build.
static:
  doAssert sizeof(Sqe) == 64, "io_uring_sqe must be 64 bytes"
  doAssert sizeof(Cqe) == 16, "io_uring_cqe must be 16 bytes"
  doAssert sizeof(IoUringParams) == 120, "io_uring_params must be 120 bytes"
  doAssert offsetOf(Sqe, opcode) == 0
  doAssert offsetOf(Sqe, flags) == 1
  doAssert offsetOf(Sqe, ioprio) == 2
  doAssert offsetOf(Sqe, fd) == 4
  doAssert offsetOf(Sqe, off) == 8
  doAssert offsetOf(Sqe, `addr`) == 16
  doAssert offsetOf(Sqe, len) == 24
  doAssert offsetOf(Sqe, rw_flags) == 28
  doAssert offsetOf(Sqe, user_data) == 32
  doAssert offsetOf(Sqe, buf_index) == 40
  doAssert offsetOf(Sqe, personality) == 42
  doAssert offsetOf(Sqe, splice_fd_in) == 44
  doAssert offsetOf(Sqe, addr3) == 48
  doAssert offsetOf(Cqe, user_data) == 0
  doAssert offsetOf(Cqe, res) == 8
  doAssert offsetOf(Cqe, flags) == 12
  doAssert sizeof(BufRingEntry) == 16, "io_uring_buf must be 16 bytes"
  doAssert offsetOf(BufRingEntry, `addr`) == 0
  doAssert offsetOf(BufRingEntry, len) == 8
  doAssert offsetOf(BufRingEntry, bid) == 12
  doAssert offsetOf(BufRingEntry, resv) == 14
  doAssert sizeof(BufReg) == 40, "io_uring_buf_reg must be 40 bytes"
  doAssert offsetOf(BufReg, ring_addr) == 0
  doAssert offsetOf(BufReg, ring_entries) == 8
  doAssert offsetOf(BufReg, bgid) == 12
  doAssert offsetOf(BufReg, flags) == 14
# ---------------------------------------------------------------------------
# Constants (mirrored from <linux/io_uring.h>)
# ---------------------------------------------------------------------------

const
  # mmap magic offsets
  IORING_OFF_SQ_RING* = 0x00000000'u64
  IORING_OFF_CQ_RING* = 0x08000000'u64
  IORING_OFF_SQES* = 0x10000000'u64

  # mmap(2) bits
  IOURING_PROT_READ = 0x1
  IOURING_PROT_WRITE = 0x2
  IOURING_MAP_SHARED = 0x1
  IOURING_MAP_POPULATE = 0x8000

  # io_uring_setup(2) flags (io_uring_params.flags).  SINGLE_ISSUER | DEFER_TASKRUN
  # (optionally COOP_TASKRUN) is the recommended set for a single-threaded,
  # per-thread ring driven by an event loop: less cross-CPU locking and completion
  # (task) work is deferred to io_uring_enter for better batching/cache locality.
  # DEFER_TASKRUN requires SINGLE_ISSUER and is incompatible with SQPOLL/IOPOLL.
  IORING_SETUP_COOP_TASKRUN*  = 1'u32 shl 8
  IORING_SETUP_SINGLE_ISSUER* = 1'u32 shl 12
  IORING_SETUP_DEFER_TASKRUN* = 1'u32 shl 13
  # IORING_SETUP_CQSIZE: honour io_uring_params.cq_entries as the explicit CQ
  # ring size.  Without it the kernel derives cq_entries = 2 * sq_entries.
  IORING_SETUP_CQSIZE*        = 1'u32 shl 3

  # io_uring_params.features
  IORING_FEAT_SINGLE_MMAP* = 1'u32 shl 0

  # sq_ring->flags (byte offset in io_uring_params.sq_off.flags).  Set by the
  # kernel when a completion could not fit in the CQ ring and was parked on its
  # internal overflow list; cleared once that list has been drained back into
  # the CQ ring.  The application must keep polling/entering while it is set or
  # those completions stay invisible.
  IORING_SQ_CQ_OVERFLOW* = 1'u32 shl 1

  # io_uring_enter(2) flags
  IORING_ENTER_GETEVENTS* = 1'u32 shl 0

  # sqe opcodes
  IORING_OP_NOP* = 0'u8
  IORING_OP_ACCEPT* = 13'u8
  IORING_OP_SEND* = 26'u8
  IORING_OP_RECV* = 27'u8
  IORING_OP_PROVIDE_BUFFERS* = 31'u8

  # sqe->flags (IOSQE_*)
  IOSQE_BUFFER_SELECT* = 1'u8 shl 5

  # sqe->file_index sentinel (union with splice_fd_in at Sqe offset 44).  For a
  # "direct" accept the kernel installs the new socket straight into the first
  # free registered-file slot; the CQE `res` is then that SLOT index, not an OS
  # fd, so no IORING_REGISTER_FILES_UPDATE and no per-accept fd is needed.
  IORING_FILE_INDEX_ALLOC* = 0xFFFFFFFF'u32

  # send/recv flags, passed in sqe->ioprio
  IORING_RECV_MULTISHOT* = 1'u32 shl 1
  # accept flags, passed in sqe->ioprio
  IORING_ACCEPT_MULTISHOT* = 1'u32 shl 0

  # cqe->flags
  IORING_CQE_F_BUFFER* = 1'u32 shl 0
  IORING_CQE_F_MORE* = 1'u32 shl 1
  IORING_CQE_BUFFER_SHIFT* = 16

  # io_uring_register(2) opcodes
  IORING_REGISTER_FILES* = 2
  IORING_UNREGISTER_FILES* = 3
  IORING_REGISTER_PBUF_RING* = 22

  # NB: the provided-buffer ring here is always allocated by the caller
  # (page-aligned mmap) and registered with flags=0; the kernel-managed
  # "mmap the ring for me" register flag is deliberately not used, so no
  # constant for it is declared.

  # errno values we special-case (Linux)
  IOURING_EPERM* = 1
  IOURING_EINVAL* = 22
  IOURING_ENOSYS* = 38

# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

type
  IoRingError* = object of CatchableError
    errno*: cint

proc newIoRingError(op, detail: string, e: cint): ref IoRingError =
  result = newException(IoRingError, op & " failed: errno=" & $e & " (" & detail & ")")
  result.errno = e

# ---------------------------------------------------------------------------
# Ring object
# ---------------------------------------------------------------------------

type
  Ring* = ref object
    fd*: cint
    params*: IoUringParams
    sqRing*: pointer          ## mmap base of the SQ ring
    cqRing*: pointer          ## mmap base of the CQ ring (== sqRing if SINGLE_MMAP)
    sqes*: pointer            ## mmap base of the SQE array
    sqkHead*: ptr uint32
    sqkTail*: ptr uint32
    sqkMask*: ptr uint32
    sqkEntries*: ptr uint32
    sqkFlags*: ptr uint32     ## SQ ring flags; nil if the kernel exposes no offset
    sqArray*: ptr uint32
    cqkHead*: ptr uint32
    cqkTail*: ptr uint32
    cqkMask*: ptr uint32
    cqes*: ptr Cqe
    toSubmit*: uint32
    sqRingSize*: csize_t
    cqRingSize*: csize_t
    requestedFlags*: uint32   ## setup flags asked for in io_uring_setup
    usedFlags*: uint32        ## setup flags the kernel actually accepted
    flagsDropped*: bool       ## true when a flagged setup was refused and retried bare

proc at(base: pointer, off: uint32): pointer {.inline.} =
  cast[pointer](cast[uint](base) + uint(off))

template mmapFailed(p: pointer): bool = cast[int](p) == -1

# ---------------------------------------------------------------------------
# setup / teardown
# ---------------------------------------------------------------------------

proc setupQueue*(entries: cuint, flags: uint32 = 0'u32,
                 cqEntries: cuint = 0'u32): Ring =
  ## Create an io_uring with room for `entries` submissions, requesting the
  ## setup `flags` (e.g. SINGLE_ISSUER | DEFER_TASKRUN).
  ##
  ## `cqEntries`, when non-zero, is requested as the explicit CQ ring size via
  ## `IORING_SETUP_CQSIZE` (must be >= `entries`; the kernel rounds to a power
  ## of two).  Without it the kernel defaults `cq_entries = 2 * sq_entries`.
  ## The size the kernel actually allocated is left in `params.cq_entries`.
  ##
  ## If the kernel rejects a flagged setup with -EINVAL (older kernel / kernel
  ## without that hint), the ring is retried WITHOUT the optimisation flags but
  ## KEEPING the explicit CQ-size request, so startup never fails over a hint and
  ## the requested headroom is not silently lost.  `usedFlags`/`flagsDropped`
  ## record which optimisation set actually took effect.
  ##
  ## Raises `IoRingError` (with `.errno` set) if the kernel refuses the ring
  ## outright, e.g. ENOSYS on a kernel without io_uring or EPERM under seccomp.
  var p: IoUringParams
  p.flags = flags
  if cqEntries > 0'u32:
    p.flags = p.flags or IORING_SETUP_CQSIZE
    p.cq_entries = cqEntries
  var fd = c_syscall(SYS_io_uring_setup, cast[clong](entries), cast[clong](addr p))
  var used = flags
  if fd < 0 and flags != 0'u32 and cint(osLastError().int) == IOURING_EINVAL:
    # Unsupported optimisation flag: retry without it, keeping IORING_SETUP_CQSIZE.
    p = IoUringParams()
    if cqEntries > 0'u32:
      p.flags = IORING_SETUP_CQSIZE
      p.cq_entries = cqEntries
    fd = c_syscall(SYS_io_uring_setup, cast[clong](entries), cast[clong](addr p))
    used = 0'u32
  if fd < 0:
    let e = cint(osLastError().int)
    raise newIoRingError("io_uring_setup", "entries=" & $entries, e)

  result = Ring(fd: cint(fd), params: p, requestedFlags: flags, usedFlags: used,
                flagsDropped: flags != 0'u32 and used == 0'u32)

  var sqRingSize = csize_t(p.sq_off.array) +
                   csize_t(p.sq_entries) * csize_t(sizeof(uint32))
  var cqRingSize = csize_t(p.cq_off.cqes) +
                   csize_t(p.cq_entries) * csize_t(sizeof(Cqe))
  let singleMmap = (p.features and IORING_FEAT_SINGLE_MMAP) != 0'u32

  if singleMmap:
    if cqRingSize > sqRingSize:
      sqRingSize = cqRingSize
    cqRingSize = sqRingSize

  let prot = cint(IOURING_PROT_READ or IOURING_PROT_WRITE)
  let mflags = cint(IOURING_MAP_SHARED or IOURING_MAP_POPULATE)

  let sq = c_mmap(nil, sqRingSize, prot, mflags, result.fd,
                  clong(IORING_OFF_SQ_RING))
  if mmapFailed(sq):
    let e = cint(osLastError().int)
    discard c_close(result.fd)
    raise newIoRingError("mmap(SQ_RING)", "size=" & $sqRingSize, e)
  result.sqRing = sq
  result.sqRingSize = sqRingSize

  if singleMmap:
    result.cqRing = sq
    result.cqRingSize = cqRingSize
  else:
    let cq = c_mmap(nil, cqRingSize, prot, mflags, result.fd,
                    clong(IORING_OFF_CQ_RING))
    if mmapFailed(cq):
      let e = cint(osLastError().int)
      discard c_munmap(sq, sqRingSize)
      discard c_close(result.fd)
      raise newIoRingError("mmap(CQ_RING)", "size=" & $cqRingSize, e)
    result.cqRing = cq
    result.cqRingSize = cqRingSize

  let sqesSize = csize_t(p.sq_entries) * csize_t(sizeof(Sqe))
  let sqes = c_mmap(nil, sqesSize, prot, mflags, result.fd,
                    clong(IORING_OFF_SQES))
  if mmapFailed(sqes):
    let e = cint(osLastError().int)
    if not singleMmap:
      discard c_munmap(result.cqRing, result.cqRingSize)
    discard c_munmap(sq, sqRingSize)
    discard c_close(result.fd)
    raise newIoRingError("mmap(SQES)", "size=" & $sqesSize, e)
  result.sqes = sqes

  # Bind every ring pointer from the kernel-provided byte offsets.
  result.sqkHead = cast[ptr uint32](at(result.sqRing, p.sq_off.head))
  result.sqkTail = cast[ptr uint32](at(result.sqRing, p.sq_off.tail))
  result.sqkMask = cast[ptr uint32](at(result.sqRing, p.sq_off.ring_mask))
  result.sqkEntries = cast[ptr uint32](at(result.sqRing, p.sq_off.ring_entries))
  # `sq_off.flags` did not exist on very old kernels; only bind it when the
  # kernel advertised a non-zero offset (else offset 0 would alias sq_head).
  if p.sq_off.flags != 0'u32:
    result.sqkFlags = cast[ptr uint32](at(result.sqRing, p.sq_off.flags))
  result.sqArray = cast[ptr uint32](at(result.sqRing, p.sq_off.array))

  result.cqkHead = cast[ptr uint32](at(result.cqRing, p.cq_off.head))
  result.cqkTail = cast[ptr uint32](at(result.cqRing, p.cq_off.tail))
  result.cqkMask = cast[ptr uint32](at(result.cqRing, p.cq_off.ring_mask))
  result.cqes = cast[ptr Cqe](at(result.cqRing, p.cq_off.cqes))

proc teardown*(r: Ring) =
  ## Unmap the rings and close the ring fd.
  if r.isNil: return
  if not r.sqes.isNil:
    discard c_munmap(r.sqes, csize_t(r.params.sq_entries) * csize_t(sizeof(Sqe)))
    r.sqes = nil
  if not r.cqRing.isNil and r.cqRing != r.sqRing:
    discard c_munmap(r.cqRing, r.cqRingSize)
    r.cqRing = nil
  if not r.sqRing.isNil:
    discard c_munmap(r.sqRing, r.sqRingSize)
    r.sqRing = nil
  if r.fd >= 0:
    discard c_close(r.fd)
    r.fd = -1

# ---------------------------------------------------------------------------
# Submission
# ---------------------------------------------------------------------------

proc getSqe*(r: Ring): ptr Sqe =
  ## Reserve the next SQE slot (zeroed), or return nil if the SQ is full.
  ## The slot index is published into sq_array and the SQ tail advanced; the
  ## kernel sees nothing until `submit`/`submitAndWait`.
  let head = r.sqkHead[]
  let tail = r.sqkTail[]
  if (tail - head) >= r.sqkEntries[]:
    return nil
  let index = tail and r.sqkMask[]
  cast[ptr UncheckedArray[uint32]](r.sqArray)[index] = index
  r.sqkTail[] = tail + 1'u32
  inc r.toSubmit
  result = cast[ptr Sqe](cast[uint](r.sqes) + uint(index) * uint(sizeof(Sqe)))
  zeroMem(result, sizeof(Sqe))

proc submit*(r: Ring): int =
  ## Push all reserved SQEs to the kernel with io_uring_enter(to_submit).
  ## Returns the syscall's return value (>= 0 on success). `toSubmit` is cleared
  ## only on success: on failure the reserved SQEs are still in the ring, so
  ## keeping the count lets a later submit re-flush them instead of leaving them
  ## uncounted and stranded.
  let to = r.toSubmit
  let ret = c_syscall(SYS_io_uring_enter, cast[clong](r.fd), cast[clong](to),
                      clong(0), clong(0), clong(0), clong(0))
  if ret < 0:
    let e = cint(osLastError().int)
    raise newIoRingError("io_uring_enter", "to_submit=" & $to, e)
  r.toSubmit = 0
  result = int(ret)

proc submitAndWait*(r: Ring, minComplete: cuint = 1): int =
  ## Flush reserved SQEs and block until at least `minComplete` completions
  ## are available (IORING_ENTER_GETEVENTS). `toSubmit` is cleared only on
  ## success so a failed enter leaves the reserved SQEs counted for a re-flush.
  let to = r.toSubmit
  let ret = c_syscall(SYS_io_uring_enter, cast[clong](r.fd), cast[clong](to),
                      cast[clong](minComplete),
                      cast[clong](IORING_ENTER_GETEVENTS),
                      clong(0), clong(0))
  if ret < 0:
    let e = cint(osLastError().int)
    raise newIoRingError("io_uring_enter(wait)", "to_submit=" & $to, e)
  r.toSubmit = 0
  result = int(ret)

proc enterGetEvents*(r: Ring): int =
  ## Non-blocking io_uring_enter with `IORING_ENTER_GETEVENTS` and
  ## `min_complete = 0`: flushes any reserved SQEs, and if the CQ ring overflowed
  ## asks the kernel to move the parked completions back into the CQ ring (the
  ## kernel's `io_cqring_wait` flushes the overflow list before the
  ## `min_events = 0` early return, so this never sleeps).  Use it to drain an
  ## overflow condition without stalling the loop.
  let to = r.toSubmit
  r.toSubmit = 0
  let ret = c_syscall(SYS_io_uring_enter, cast[clong](r.fd), cast[clong](to),
                      clong(0), cast[clong](IORING_ENTER_GETEVENTS),
                      clong(0), clong(0))
  if ret < 0:
    let e = cint(osLastError().int)
    raise newIoRingError("io_uring_enter(getevents)", "to_submit=" & $to, e)
  result = int(ret)

proc sqOverflowed*(r: Ring): bool =
  ## True while `IORING_SQ_CQ_OVERFLOW` is set in the SQ ring flags: at least one
  ## completion did not fit in the CQ ring and is parked on the kernel's overflow
  ## list.  Call `enterGetEvents` + drain until this clears so no completion is
  ## silently left unread.  Always false on a ring that exposes no flags offset.
  result = (not r.sqkFlags.isNil) and
           ((r.sqkFlags[] and IORING_SQ_CQ_OVERFLOW) != 0'u32)

# ---------------------------------------------------------------------------
# Completion
# ---------------------------------------------------------------------------

proc peekCqe*(r: Ring): ptr Cqe =
  ## Return the next completion, or nil if the CQ ring is empty.
  ## The entry stays owned by the kernel until `cqeSeen`.
  let head = r.cqkHead[]
  let tail = r.cqkTail[]
  if head == tail:
    return nil
  result = cast[ptr Cqe](cast[uint](r.cqes) +
                         uint(head and r.cqkMask[]) * uint(sizeof(Cqe)))

proc cqeSeen*(r: Ring) =
  ## Consume the completion previously returned by `peekCqe`.
  inc r.cqkHead[]

# ---------------------------------------------------------------------------
# Fixed files / buffers
# ---------------------------------------------------------------------------

proc registerFiles*(r: Ring, fds: openArray[cint]): int =
  ## IORING_REGISTER_FILES: pin `fds` in the ring's fixed-file table so that
  ## later SQEs can address them by index (IOSQE_FIXED_FILE) without occupying
  ## process fd-table slots.
  var arr = newSeq[cint](fds.len)
  for i in 0 ..< fds.len:
    arr[i] = fds[i]
  let argp = if fds.len > 0: cast[clong](addr arr[0]) else: clong(0)
  let ret = c_syscall(SYS_io_uring_register, cast[clong](r.fd),
                      cast[clong](IORING_REGISTER_FILES), argp,
                      cast[clong](fds.len))
  if ret < 0:
    let e = cint(osLastError().int)
    raise newIoRingError("io_uring_register(FILES)", "nr=" & $fds.len, e)
  result = int(ret)

proc unregisterFiles*(r: Ring): int =
  ## IORING_UNREGISTER_FILES: drop the fixed-file table.
  let ret = c_syscall(SYS_io_uring_register, cast[clong](r.fd),
                      cast[clong](IORING_UNREGISTER_FILES), clong(0), clong(0))
  if ret < 0:
    let e = cint(osLastError().int)
    raise newIoRingError("io_uring_unregister(FILES)", "", e)
  result = int(ret)

proc registerBufRing*(r: Ring, ringAddr: pointer, entries: uint32,
                      bgid: uint16): int =
  ## IORING_REGISTER_PBUF_RING: publish a userspace-managed provided-buffer
  ## ring (`entries` descriptor slots at `ringAddr`) under buffer group `bgid`.
  ## The kernel only reads the descriptors; the caller keeps filling the ring
  ## (advancing `tail`) with `(addr,len,bid)` entries as buffers free up.
  ## `ringAddr` must be page-aligned; `entries` must be a power of two.
  var reg = BufReg(ring_addr: cast[uint64](ringAddr),
                   ring_entries: entries, bgid: bgid, flags: 0'u16)
  let ret = c_syscall(SYS_io_uring_register, cast[clong](r.fd),
                      cast[clong](IORING_REGISTER_PBUF_RING),
                      cast[clong](cast[uint](addr reg)), clong(1))
  if ret < 0:
    let e = cint(osLastError().int)
    raise newIoRingError("io_uring_register(PBUF_RING)",
                         "entries=" & $entries & " bgid=" & $bgid, e)
  result = int(ret)

proc prepProvideBuffers*(sqe: ptr Sqe, bufAddr: uint64, bufLen: uint32,
                         nr: uint32, startBid: uint16, bgid: uint16) =
  ## Fill an SQE with IORING_OP_PROVIDE_BUFFERS: hand `nr` buffers of `bufLen`
  ## bytes at `bufAddr` to buffer group `bgid`, with the first assigned buffer
  ## id `startBid`.  This is the pre-5.19 fallback for provided buffers.
  sqe.opcode = IORING_OP_PROVIDE_BUFFERS
  sqe.fd = int32(nr)
  sqe.`addr` = bufAddr
  sqe.len = bufLen
  sqe.off = uint64(startBid)
  sqe.buf_index = bgid
