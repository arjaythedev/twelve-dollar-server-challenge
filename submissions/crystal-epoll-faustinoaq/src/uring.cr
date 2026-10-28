require "c/sys/mman"
require "c/sys/eventfd"

# io_uring through raw syscalls (no liburing). Accepted sockets are fixed (direct)
# descriptors: they live in the ring's file table, not the process fd table, so
# LimitNOFILE=65535 does not cap connections. A table can't exceed RLIMIT_NOFILE either,
# so under that limit there are 8 rings of 65,535 slots, all driven by this one thread.
# Per ring: a multishot recv per connection into kernel-picked provided buffers, and
# sends that point straight into the batch arena. One ring at a time accepts, ring 0
# first, so until it is full the loop is the plain single-ring one: one io_uring_enter
# per batch that submits the last batch's sends and waits for the next completions.
lib LibUring
  struct Sqe
    opcode : UInt8
    flags : UInt8
    ioprio : UInt16
    fd : Int32
    off : UInt64
    addr : UInt64
    len : UInt32
    op_flags : UInt32
    user_data : UInt64
    buf_group : UInt16
    personality : UInt16
    file_index : Int32
    addr3 : UInt64
    pad : UInt64
  end

  struct Cqe
    user_data : UInt64
    res : Int32
    flags : UInt32
  end

  struct SqOff
    head : UInt32
    tail : UInt32
    ring_mask : UInt32
    ring_entries : UInt32
    flags : UInt32
    dropped : UInt32
    array : UInt32
    resv1 : UInt32
    user_addr : UInt64
  end

  struct CqOff
    head : UInt32
    tail : UInt32
    ring_mask : UInt32
    ring_entries : UInt32
    overflow : UInt32
    cqes : UInt32
    flags : UInt32
    resv1 : UInt32
    user_addr : UInt64
  end

  struct Params
    sq_entries : UInt32
    cq_entries : UInt32
    flags : UInt32
    sq_thread_cpu : UInt32
    sq_thread_idle : UInt32
    features : UInt32
    wq_fd : UInt32
    resv : UInt32[3]
    sq_off : SqOff
    cq_off : CqOff
  end

  struct RsrcReg
    nr : UInt32
    flags : UInt32
    resv2 : UInt64
    data : UInt64
    tags : UInt64
  end

  struct BufReg
    ring_addr : UInt64
    ring_entries : UInt32
    bgid : UInt16
    flags : UInt16
    resv : UInt64[3]
  end
end

class Ring
  getter fd : Int32
  getter base : Int32 # connection id of slot 0
  getter slots : Int32
  property to_submit = 0_u32
  property accepting = false
  property used = 0 # slots taken: accepted and not yet closed by the kernel
  property efd = -1

  @sqes : LibUring::Sqe*
  @sq_head : UInt32*
  @sq_tail : UInt32*
  @sq_mask : UInt32
  @sq_entries : UInt32
  @stail : UInt32
  @cq_head : UInt32*
  @cq_tail : UInt32*
  @cq_mask : UInt32
  @cqes : LibUring::Cqe*
  @bring : UInt8*
  @bufs : UInt8*
  @btail : UInt16 = 0
  @retry : UInt64* = Pointer(UInt64).null # receives that ran out of buffers: (gen << 32) | id
  @nretry = 0
  @retry_cap = 0

  # Returns nil when the kernel refuses (no io_uring, or older than 6.1).
  def self.create(base : Int32, slots : Int32, sq : UInt32, cq : UInt32) : Ring?
    pa = LibUring::Params.new
    pa.cq_entries = cq
    pa.flags = Uring::SETUP_CQSIZE | Uring::SETUP_SUBMIT_ALL | Uring::SETUP_SINGLE_ISSUER | Uring::SETUP_DEFER_TASKRUN
    fd = LibC.syscall(Uring::SYS_SETUP, sq, pointerof(pa)).to_i32
    return nil if fd < 0
    r = new(fd, base, slots, pointerof(pa))
    r.init ? r : nil
  end

  def initialize(@fd : Int32, @base : Int32, @slots : Int32, pa : LibUring::Params*)
    so = pa.value.sq_off
    co = pa.value.cq_off
    @sq_entries = pa.value.sq_entries
    sz = {so.array.to_u64 + @sq_entries.to_u64 * 4, co.cqes.to_u64 + pa.value.cq_entries.to_u64 * 16}.max
    rw = LibC::PROT_READ | LibC::PROT_WRITE
    sh = LibC::MAP_SHARED | LibC::MAP_POPULATE
    m = Pointer(UInt8).null
    m = LibC.mmap(Pointer(Void).null, sz, rw, sh, @fd, 0).as(UInt8*) if pa.value.features & Uring::FEAT_SINGLE_MMAP != 0
    m = Pointer(UInt8).null if m.address == ~0_u64
    sq = Pointer(Void).null
    sq = LibC.mmap(Pointer(Void).null, @sq_entries.to_u64 * 64, rw, sh, @fd, Uring::OFF_SQES) unless m.null?
    sq = Pointer(Void).null if sq.address == ~0_u64
    if m.null? || sq.null?
      @sqes = Pointer(LibUring::Sqe).null
      m = LibC.malloc(64).as(UInt8*) # placeholders; init fails
    else
      @sqes = sq.as(LibUring::Sqe*)
    end
    @sq_head = (m + so.head).as(UInt32*)
    @sq_tail = (m + so.tail).as(UInt32*)
    @sq_mask = @sqes.null? ? 0_u32 : (m + so.ring_mask).as(UInt32*).value
    @stail = @sqes.null? ? 0_u32 : @sq_tail.value
    unless @sqes.null?
      array = (m + so.array).as(UInt32*)
      @sq_entries.times { |i| array[i] = i.to_u32 } # identity: slot i holds sqe i
    end
    @cq_head = (m + co.head).as(UInt32*)
    @cq_tail = (m + co.tail).as(UInt32*)
    @cq_mask = @sqes.null? ? 0_u32 : (m + co.ring_mask).as(UInt32*).value
    @cqes = (m + co.cqes).as(LibUring::Cqe*)
    @bring = Pointer(UInt8).null
    @bufs = Pointer(UInt8).null
  end

  # Fixed-file table, provided-buffer ring, first accept.
  def init : Bool
    return false if @sqes.null?
    rr = LibUring::RsrcReg.new
    rr.nr = @slots.to_u32
    rr.flags = 1 # IORING_RSRC_REGISTER_SPARSE
    return false if LibC.syscall(Uring::SYS_REGISTER, @fd, Uring::REGISTER_FILES2, pointerof(rr), sizeof(LibUring::RsrcReg)) < 0
    @bring = LibC.mmap(Pointer(Void).null, (Uring::NBUF * 16).to_u64, LibC::PROT_READ | LibC::PROT_WRITE,
      LibC::MAP_PRIVATE | LibC::MAP_ANONYMOUS, -1, 0).as(UInt8*)
    return false if @bring.address == ~0_u64
    @bufs = LibC.malloc(Uring::NBUF * Uring::BUF_SIZE).as(UInt8*)
    reg = LibUring::BufReg.new
    reg.ring_addr = @bring.address
    reg.ring_entries = Uring::NBUF.to_u32
    return false if LibC.syscall(Uring::SYS_REGISTER, @fd, Uring::REGISTER_PBUF_RING, pointerof(reg), 1) < 0
    Uring::NBUF.times { |i| give_buf(i.to_u16) }
    publish_bufs
    true
  end

  @[AlwaysInline]
  def load_acq(p : UInt32*) : UInt32
    Atomic::Ops.load(p, :acquire, true)
  end

  @[AlwaysInline]
  def store_rel(p : T*, v : T) : Nil forall T
    Atomic::Ops.store(p, v, :release, true)
  end

  # Next free SQE, zeroed. A full queue is submitted first.
  def sqe : LibUring::Sqe*
    enter(0_u32, 0_u32) if @stail &- load_acq(@sq_head) >= @sq_entries
    e = @sqes + (@stail & @sq_mask)
    Intrinsics.memset(e.as(Void*), 0_u8, 64_u64, false)
    @stail &+= 1
    @to_submit &+= 1
    e
  end

  def enter(min : UInt32, flags : UInt32) : Nil
    publish_bufs # buffers returned so far must be visible to what gets submitted now
    store_rel(@sq_tail, @stail)
    n = @to_submit
    @to_submit = 0_u32
    LibC.syscall(Uring::SYS_ENTER, @fd, n, min, flags, Pointer(Void).null, 0)
  end

  @[AlwaysInline]
  def buf(bid : UInt16) : UInt8*
    @bufs + bid.to_i32 &* Uring::BUF_SIZE
  end

  @[AlwaysInline]
  def give_buf(bid : UInt16) : Nil
    e = @bring + ((@btail & (Uring::NBUF &- 1)).to_i32 &* 16)
    e.as(UInt64*).value = buf(bid).address
    (e + 8).as(UInt32*).value = Uring::BUF_SIZE.to_u32
    (e + 12).as(UInt16*).value = bid # bytes 14..15 of entry 0 are the ring tail: untouched
    @btail &+= 1
  end

  # Signals efd whenever completions are pending (with deferred task work: as soon as
  # there is work for this ring), so epoll can wait on several rings at once.
  def watch(ep : Int32) : Nil
    return if @efd >= 0
    @efd = LibC.eventfd(0, 0o4000 | 0o2000000) # EFD_NONBLOCK | EFD_CLOEXEC
    LibC.syscall(Uring::SYS_REGISTER, @fd, Uring::REGISTER_EVENTFD, pointerof(@efd), 1)
    Sys.epoll(ep, Sys::EPOLL_CTL_ADD, @efd, Sys::EPOLLIN | Sys::EPOLLET)
  end

  def publish_bufs : Nil
    store_rel((@bring + 14).as(UInt16*), @btail)
  end

  def defer_recv(id : Int32) : Nil
    if @nretry == @retry_cap
      @retry_cap = @retry_cap == 0 ? 1024 : @retry_cap &* 2
      @retry = LibC.realloc(@retry, @retry_cap &* 8).as(UInt64*)
    end
    @retry[@nretry] = (Server.conn(id).value.gen.to_u64 << 32) | id.to_u32
    @nretry &+= 1
  end

  # Rearms the receives that hit ENOBUFS, now that this batch's buffers are back. Ones that
  # find none again just come back next batch: never a resubmit loop inside one batch.
  def retry_recvs : Nil
    n = @nretry
    @nretry = 0
    n.times do |i|
      v = @retry[i]
      id = v.to_i32!
      c = Server.conn(id)
      Uring.arm_recv(self, id) if c.value.open && c.value.gen == (v >> 32).to_u16!
    end
  end

  def ready? : Bool
    @cq_head.value != load_acq(@cq_tail)
  end

  # Runs every completion that is ready. Returns how many.
  def drain : Int32
    head = @cq_head.value
    tail = load_acq(@cq_tail)
    n = 0
    while head != tail
      while head != tail
        Uring.complete(self, @cqes[head & @cq_mask])
        head &+= 1
        n &+= 1
      end
      store_rel(@cq_head, head)
      tail = load_acq(@cq_tail)
    end
    publish_bufs if n > 0
    n
  end
end

module Uring
  SYS_SETUP    = 425
  SYS_ENTER    = 426
  SYS_REGISTER = 427

  SETUP_CQSIZE        = 1_u32 << 3
  SETUP_SUBMIT_ALL    = 1_u32 << 7
  SETUP_SINGLE_ISSUER = 1_u32 << 12
  SETUP_DEFER_TASKRUN = 1_u32 << 13
  FEAT_SINGLE_MMAP    = 1_u32 << 0
  ENTER_GETEVENTS     = 1_u32
  REGISTER_EVENTFD    = 4
  REGISTER_FILES2     = 13
  REGISTER_PBUF_RING  = 22
  OFF_SQES            = 0x10000000_i64
  OP_ACCEPT           = 13_u8
  OP_CLOSE            = 19_u8
  OP_SEND             = 26_u8
  OP_RECV             = 27_u8
  OP_SHUTDOWN         = 34_u8
  SQE_FIXED_FILE      = 1_u8
  SQE_IO_HARDLINK     = 1_u8 << 3
  SQE_BUFFER_SELECT   = 1_u8 << 5
  ACCEPT_MULTISHOT    = 1_u16
  RECV_MULTISHOT      = 2_u16
  FILE_INDEX_ALLOC    = ~0_u32
  CQE_F_BUFFER        = 1_u32
  CQE_F_MORE          = 2_u32
  ENOBUFS             = 105

  SQ_ENTRIES = 4096_u32 # largest ring; halved together while it doesn't fit the memlock budget
  CQ_ENTRIES = 32768_u32
  NBUF       = 4096 # provided receive buffers per ring (power of two)
  BUF_SIZE   = 2048
  MAX_RINGS  = 8
  TARGET     = 524288 # connections to make room for

  # user_data: op (8 bits) | arena (8) | gen (16) | connection id (32)
  TAG_ACCEPT = 1_u64
  TAG_RECV   = 2_u64
  TAG_SEND   = 3_u64
  TAG_CLOSE  = 4_u64
  OWNED      = 255 # a send not from an arena: the connection's outbuf, or a constant

  @@rings = Pointer(Ring).null
  @@nrings = 0
  @@slots = 0
  @@sq = 0_u32
  @@cq = 0_u32
  @@lfd = 0
  @@ep = -1
  @@multi = false # some ring other than 0 accepts or holds connections

  def self.setup(lfd : Int32, nofile : UInt64) : Bool
    @@lfd = lfd
    slots = {nofile, TARGET.to_u64}.min.to_i32
    if t = Sys.env("URING_SLOTS") # testing the ring spill-over with few connections
      slots = {slots, t.to_i}.min
    end
    want = {(TARGET &+ slots &- 1) // slots, MAX_RINGS}.min
    # Since Linux 6.13 ring memory counts against RLIMIT_MEMLOCK (8 MB by default, per user):
    # size and count the rings to fit.
    budget = Sys.memlock_budget
    sq = SQ_ENTRIES
    cq = CQ_ENTRIES
    while sq > 256 && ring_cost(sq, cq) > budget
      sq >>= 1
      cq >>= 1
    end
    want = (budget // ring_cost(sq, cq)).clamp(1_u64, want.to_u64).to_i32
    @@rings = LibC.malloc(want * sizeof(Ring)).as(Ring*)
    @@slots = slots
    @@sq = sq
    @@cq = cq
    want.times do |i|
      r = Ring.create(i &* slots, slots, sq, cq)
      break unless r
      @@rings[i] = r
      @@nrings &+= 1
    end
    return false if @@nrings == 0
    Server.alloc_conns(@@nrings.to_u64 &* slots)
    @@ep = LibC.epoll_create1(0)
    arm_accept(@@rings[0])
    true
  end

  def self.describe : String
    "#{@@nrings} ring(s) x #{@@slots} fixed slots, #{@@sq}/#{@@cq} sq/cq entries"
  end

  # Bytes a ring pins: SQ array and CQEs, SQEs, provided-buffer ring.
  private def self.ring_cost(sq : UInt32, cq : UInt32) : UInt64
    sq.to_u64 * 4 + cq.to_u64 * 16 + sq.to_u64 * 64 + NBUF.to_u64 * 16 + 3 * 4096
  end

  @[AlwaysInline]
  def self.ring_of(id : Int32) : Ring
    @@rings[id // @@slots]
  end

  @[AlwaysInline]
  private def self.tag(op : UInt64, arena : Int32, id : Int32) : UInt64
    op | (arena.to_u64 << 8) | (Server.conn(id).value.gen.to_u64 << 16) | (id.to_u64 << 32)
  end

  private def self.arm_accept(r : Ring) : Nil
    r.accepting = true
    @@multi = true if r.base > 0
    e = r.sqe
    e.value.opcode = OP_ACCEPT
    e.value.fd = @@lfd
    e.value.ioprio = ACCEPT_MULTISHOT
    e.value.file_index = FILE_INDEX_ALLOC.to_i32!
    e.value.user_data = TAG_ACCEPT
  end

  def self.arm_recv(r : Ring, id : Int32) : Nil
    e = r.sqe
    e.value.opcode = OP_RECV
    e.value.fd = id &- r.base
    e.value.flags = SQE_FIXED_FILE | SQE_BUFFER_SELECT
    e.value.ioprio = RECV_MULTISHOT
    e.value.user_data = tag(TAG_RECV, 0, id)
  end

  private def self.send(id : Int32, p : UInt8*, n : Int32, arena : Int32) : Nil
    r = ring_of(id)
    c = Server.conn(id)
    c.value.sending = true
    c.value.sptr = p
    c.value.slen = n
    c.value.sarena = arena
    e = r.sqe
    e.value.opcode = OP_SEND
    e.value.fd = id &- r.base
    e.value.flags = SQE_FIXED_FILE
    e.value.addr = p.address
    e.value.len = n.to_u32
    e.value.op_flags = Sys::MSG_NOSIGNAL.to_u32
    e.value.user_data = tag(TAG_SEND, arena, id)
  end

  def self.run : NoReturn
    r0 = @@rings[0]
    evs = LibC.malloc(MAX_RINGS * sizeof(Sys::EpollEvent)).as(Sys::EpollEvent*)
    loop do
      if @@multi && !multi?
        @@multi = false
      end
      if !@@multi
        (1...@@nrings).each do |i| # closes still going out on rings that emptied
          r = @@rings[i]
          r.enter(0_u32, ENTER_GETEVENTS) if r.to_submit > 0
        end
        r0.enter(1_u32, ENTER_GETEVENTS) # submits the last batch's sends, waits for work
      else
        @@nrings.times do |i|
          r = @@rings[i]
          r.watch(@@ep)
          r.enter(0_u32, ENTER_GETEVENTS) # submit, and run the ring's deferred work
        end
        unless ready?
          LibC.epoll_wait(@@ep, evs, MAX_RINGS, -1)
          @@nrings.times { |i| @@rings[i].enter(0_u32, ENTER_GETEVENTS) }
        end
      end
      Server.begin_batch
      @@nrings.times { |i| @@rings[i].drain }
      deliver(Db.finish_batch)
      @@nrings.times { |i| @@rings[i].retry_recvs }
    end
  end

  private def self.multi? : Bool
    (1...@@nrings).any? { |i| @@rings[i].used > 0 || @@rings[i].accepting }
  end

  private def self.ready? : Bool
    @@nrings.times { |i| return true if @@rings[i].ready? }
    false
  end

  def self.complete(r : Ring, cq : LibUring::Cqe) : Nil
    ud = cq.user_data
    op = ud & 0xFF
    return if op == 0 # a shutdown
    if op == TAG_ACCEPT
      on_accept(r, cq.res, cq.flags)
      return
    end
    if op == TAG_CLOSE # a slot came free: resume accepting here if every table was full
      r.used &-= 1
      arm_accept(r) unless accepting?
      return
    end
    id = (ud >> 32).to_i32!
    gen = ((ud >> 16) & 0xFFFF).to_u16!
    c = Server.conn(id)
    live = c.value.open && c.value.gen == gen
    if op == TAG_RECV
      if cq.flags & CQE_F_BUFFER != 0
        bid = (cq.flags >> 16).to_u16!
        Server.on_data(id, r.buf(bid), cq.res) if live && cq.res > 0 && !c.value.closing
        r.give_buf(bid)
      end
      return unless live
      if cq.res == 0 || (cq.res < 0 && cq.res != -ENOBUFS)
        close(id)
      elsif cq.flags & CQE_F_MORE == 0
        if cq.res == -ENOBUFS
          r.defer_recv(id)
        else
          arm_recv(r, id) # multishot ended for another reason: rearm
        end
      end
    else
      arena = ((ud >> 8) & 0xFF).to_i32!
      Server.unref(arena) unless arena == OWNED
      on_sent(id, cq.res) if live
    end
  end

  # res is the fixed-file slot. When the table is full (-ENFILE) accepting moves on to
  # the lowest ring with room; with none left it resumes on the next close.
  private def self.on_accept(r : Ring, res : Int32, flags : UInt32) : Nil
    if res >= 0
      r.used &+= 1
      id = r.base &+ res
      Server.conn(id).value.open = true
      arm_recv(r, id)
    end
    return unless flags & CQE_F_MORE == 0
    r.accepting = false
    if res != -23 # ENFILE
      arm_accept(r)
      return
    end
    @@nrings.times do |i|
      n = @@rings[i]
      if n.used < n.slots
        arm_accept(n)
        return
      end
    end
  end

  private def self.accepting? : Bool
    @@nrings.times { |i| return true if @@rings[i].accepting }
    false
  end

  private def self.on_sent(id : Int32, res : Int32) : Nil
    c = Server.conn(id)
    cv = c.value
    if res < 0
      close(id)
      return
    end
    if res < cv.slen # short send: the rest goes out from the same memory
      arena = cv.sarena
      Server.ref(arena) unless arena == OWNED
      send(id, cv.sptr + res, cv.slen &- res, arena)
      return
    end
    cv.sending = false
    c.value = cv
    if cv.nextlen > 0 # output queued meanwhile becomes the owned buffer in flight
      ob, oc = cv.outbuf, cv.outcap
      cv.outbuf, cv.outcap = cv.nextbuf, cv.nextcap
      cv.nextbuf, cv.nextcap = ob, oc
      n = cv.nextlen
      cv.nextlen = 0
      c.value = cv
      send(id, cv.outbuf, n, OWNED)
    elsif cv.closing
      close(id)
    end
  end

  # Shutdown ends the multishot recv (its last CQE carries a stale generation), then the
  # slot is closed; hard-linked so the close runs even if the shutdown fails.
  private def self.close(id : Int32) : Nil
    r = ring_of(id)
    slot = id &- r.base
    Server.free_conn(id)
    e = r.sqe
    e.value.opcode = OP_SHUTDOWN
    e.value.fd = slot
    e.value.flags = SQE_FIXED_FILE | SQE_IO_HARDLINK
    e.value.len = 2 # SHUT_RDWR
    e.value.user_data = 0_u64
    e = r.sqe
    e.value.opcode = OP_CLOSE
    e.value.file_index = slot &+ 1
    e.value.user_data = TAG_CLOSE
  end

  # After COMMIT: each socket's responses go out as one send from the arena, or are
  # appended to its queue when a send is already in flight (one at a time keeps order).
  # A failed commit turns every response of the batch into a 500 and a close.
  private def self.deliver(ok : Bool) : Nil
    a = Server.cur
    base = Server.arena(a).ptr
    pend = Server.pend
    Server.npend.times do |k|
      pe = pend[k]
      id = pe.fd
      c = Server.conn(id)
      next unless c.value.open && c.value.gen == pe.gen
      if ok
        p = base + pe.start
        n = pe.stop &- pe.start
      else
        p = Resp::INTERNAL.to_unsafe
        n = Resp::INTERNAL.bytesize
      end
      if n > 0
        if c.value.sending
          Server.queue(id, p, n)
        elsif ok
          Server.ref(a)
          send(id, p, n, a)
        else
          send(id, p, n, OWNED)
        end
      end
      if pe.close || !ok
        c.value.closing = true
        close(id) unless c.value.sending
      end
    end
  end
end
