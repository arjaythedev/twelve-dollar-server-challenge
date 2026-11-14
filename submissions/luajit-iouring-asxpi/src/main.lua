-- The event loop: io_uring driven straight from LuaJIT's FFI (no liburing, no C helpers).
--
-- One thread. Accepted sockets are io_uring direct descriptors, so they don't count against the
-- process's file-descriptor limit; a ring's table holds up to 65,535 of them and more rings are
-- added as they fill. Each connection has one multishot receive into a provided-buffer ring.
-- A pass of the loop is one SQLite transaction: no response of a pass is sent before its COMMIT.
local SRC = arg[0]:match("^(.*)/[^/]*$") or "."
package.path = SRC .. "/?.lua;" .. package.path
local ffi = require("ffi")
local bit = require("bit")
local C = require("cdefs")
local app = require("app")
local band, bor, bnot, rshift, tobit = bit.band, bit.bor, bit.bnot, bit.rshift, bit.tobit

local function env(name, default)
  local v = os.getenv(name)
  if v == nil or v == "" then return default end
  return tonumber(v) or v
end

local HOST = env("HOST", "127.0.0.1")
local PORT = env("PORT", 3000)
local SQLITE_PATH = env("SQLITE_PATH", "feed.db")
local JWT_SECRET = tostring(env("JWT_SECRET", "twelve-dollar-challenge"))

-- tuning knobs (all optional)
local STATS = env("TD_STATS", 0) ~= 0             -- one line of counters per second on stderr
local MAX_RINGS = env("TD_MAX_RINGS", 16)
local PRE_RINGS = env("TD_RINGS", 6)              -- rings made at startup (60,535 connections each)
local SQ_ENTRIES = env("TD_SQ_ENTRIES", 4096)
local CQ_ENTRIES = env("TD_CQ_ENTRIES", 32768)
local NBUF = env("TD_NBUF", 4096)                 -- provided receive buffers per ring (power of 2)
local BUFSZ = env("TD_BUFSZ", 1024)
local MAX_CQE = env("TD_MAX_CQE", 16384)          -- completions handled per ring per pass
local ANSWER_CAP = env("TD_ANSWER_CAP", 2048)     -- requests answered per pass; the rest are read and queued
local CKPT_BOOST = env("TD_CKPT_BOOST", 16384)    -- WAL frames past which the checkpoint thread gets normal priority
local BATCH = env("TD_BATCH", 4096)                -- completions to wait for ...
local WAIT_US = env("TD_WAIT_US", 2000)           -- ... or this long, whichever comes first. At 265k users 2, 4 and 8 ms
                                                  -- cost about the same CPU; the WAL peaked at 23k, 48k and 65k frames
local CACHE_KB = env("TD_CACHE_KB", 65536)
local AUTOCKPT = env("TD_AUTOCKPT", 1000)
local BACKLOG = env("TD_BACKLOG", 4096)
local CKPT_FRAMES = env("TD_CKPT_FRAMES", 4096)   -- WAL frames (4 KB each) that start a checkpoint
local HOLD_MS = env("TD_HOLD_MS", 250)            -- how long writes wait for a checkpoint to finish, at first
local HOLD_MAX_MS = env("TD_HOLD_MAX_MS", 2000)   -- ... and at most, after holds that were too short
local CKPT_FORCE = env("TD_CKPT_FORCE", 65536)    -- WAL frames past which writes are held until the log restarts
local MARGIN = env("TD_MARGIN", 5000)             -- free slots at which a ring stops accepting

local function die(msg)
  io.stderr:write("fatal: ", msg, "\n")
  os.exit(1)
end
local function errstr() return ffi.string(C.strerror(ffi.errno())) end

---------------------------------------------------------------------------------------------------
-- constants

local SYS_SETUP, SYS_ENTER, SYS_REGISTER = 425, 426, 427
local SETUP_CQSIZE, SETUP_CLAMP, SETUP_SUBMIT_ALL = 8, 16, 128
local SETUP_SINGLE_ISSUER, SETUP_DEFER_TASKRUN = 4096, 8192
local ENTER_GETEVENTS, ENTER_EXT_ARG = 1, 8
local OFF_CQ_RING, OFF_SQES = 0x8000000, 0x10000000
local REGISTER_FILES2, REGISTER_PBUF_RING = 13, 22
local OP_ACCEPT, OP_CANCEL, OP_CLOSE, OP_SEND, OP_RECV, OP_SHUTDOWN = 13, 14, 19, 26, 27, 34
local SQE_FIXED_FILE, SQE_BUFFER_SELECT = 1, 32
local CQE_F_BUFFER, CQE_F_MORE = 1, 2
local ENOBUFS, ETIME, EINTR, EAGAIN, EBUSY = 105, 62, 4, 11, 16
local MSG_NOSIGNAL = 0x4000

-- user_data: low half = kind * 65536 + slot, high half = the connection's generation
local K_ACCEPT, K_RECV, K_SEND, K_CLOSE, K_IGNORE = 1, 2, 3, 4, 5

local F_OPEN, F_RECV, F_SENDING, F_CLOSING, F_QUEUED, F_SHUT, F_CLOSE_SENT, F_HELD = 1, 2, 4, 8, 16, 32, 64, 128
local F_REARMQ, F_READYQ = 256, 512
local QMASK = 65535                               -- queues hold a connection at most once: 65,536 is enough

---------------------------------------------------------------------------------------------------
-- stats

local st = {
  accepts = 0, closes = 0, reqs = 0, passes = 0, cqes = 0, enobufs = 0, sends = 0, short_sends = 0,
  stash = 0, commits = 0, commit_max = 0, commit_sum = 0, pass_max = 0, busy = 0, flushes = 0,
  enter = 0, errors = 0, max_batch = 0, held = 0, holds = 0, hold_max = 0, ckpts = 0, enter_max = 0, reap_max = 0, flush_max = 0, deferred = 0, rearms = 0, hold_fail = 0,
}

local ts = ffi.new("td_timespec")
local function mono()
  C.clock_gettime(1, ts)
  return tonumber(ts.sec) + tonumber(ts.nsec) * 1e-9
end
local function cputime()                      -- user + system time of this process
  C.clock_gettime(2, ts)
  return tonumber(ts.sec) + tonumber(ts.nsec) * 1e-9
end
local function wall()
  C.clock_gettime(0, ts)
  return tonumber(ts.sec)
end

---------------------------------------------------------------------------------------------------
-- output buffers: a pool of 8 KB chunks (a feed response is ~4.5 KB), malloc for anything bigger

local CHUNK = 8192
local POOL_MAX = 4096
local pool = ffi.new("uint8_t *[?]", POOL_MAX)
local npool = 0

local function out_release(r, slot)
  local c = r.conns[slot]
  if c.outb ~= nil then
    if c.outcap == CHUNK and npool < POOL_MAX then
      pool[npool] = c.outb; npool = npool + 1
    else
      C.free(c.outb)
    end
    c.outb = nil
  end
  c.outlen = 0; c.outsent = 0; c.outcap = 0
end

local function out_append(r, slot, src, n)
  local c = r.conns[slot]
  local need = c.outlen + n
  if need > c.outcap then
    local cap = CHUNK
    while cap < need do cap = cap * 2 end
    local nb
    if cap == CHUNK and npool > 0 then
      npool = npool - 1; nb = pool[npool]
    else
      nb = ffi.cast("uint8_t *", C.malloc(cap))
      if nb == nil then die("out of memory") end
    end
    if c.outb ~= nil then
      ffi.copy(nb, c.outb, c.outlen)
      local len = c.outlen
      out_release(r, slot)
      c.outlen = len
    end
    c.outb = nb; c.outcap = cap
  end
  ffi.copy(c.outb + c.outlen, src, n)
  c.outlen = need
end

-- input left over from a receive (a request split across packets, or pipelined behind a send)
local function stash_append(r, slot, p, n)
  local c = r.conns[slot]
  local need = c.inlen + n
  if need > c.incap then
    local cap = 512
    while cap < need do cap = cap * 2 end
    local nb = ffi.cast("uint8_t *", C.realloc(c.inb, cap))
    if nb == nil then die("out of memory") end
    c.inb = nb; c.incap = cap
  end
  ffi.copy(c.inb + c.inlen, p, n)
  c.inlen = need
  st.stash = st.stash + 1
end

local function stash_free(r, slot)
  local c = r.conns[slot]
  if c.inb ~= nil then C.free(c.inb); c.inb = nil end
  c.inlen = 0; c.incap = 0
end

---------------------------------------------------------------------------------------------------
-- rings

local rings = {}
local nrings = 0
local LISTENERS = env("TD_LISTENERS", 8)          -- SO_REUSEPORT sockets, each with its own accept queue
local listen_fds = {}
local table_size = 0
local accept_ring = nil          -- the ring whose multishot accept is armed
local params = ffi.new("td_uring_params")

local function ring_new()
  ffi.fill(params, ffi.sizeof(params))
  params.flags = bor(SETUP_CQSIZE, SETUP_CLAMP, SETUP_SUBMIT_ALL, SETUP_SINGLE_ISSUER, SETUP_DEFER_TASKRUN)
  params.cq_entries = CQ_ENTRIES
  local fd = C.td_sys_setup(SYS_SETUP, SQ_ENTRIES, params)
  if fd < 0 then return nil, "io_uring_setup: " .. errstr() end
  if band(params.features, 1) == 0 then return nil, "io_uring: no single mmap" end
  local sq_sz = params.sq_off.array + params.sq_entries * 4
  local cq_sz = params.cq_off.cqes + params.cq_entries * 16
  local ring_sz = math.max(sq_sz, cq_sz)
  local MAP_FAILED = ffi.cast("void *", -1)
  local rp = C.mmap(nil, ring_sz, 3, 0x8001, fd, 0)
  if rp == MAP_FAILED then return nil, "mmap ring: " .. errstr() end
  local sp = C.mmap(nil, params.sq_entries * 64, 3, 0x8001, fd, OFF_SQES)
  if sp == MAP_FAILED then return nil, "mmap sqes: " .. errstr() end
  local base = ffi.cast("uint8_t *", rp)
  local u32 = function(off) return ffi.cast("uint32_t *", base + off) end
  local arr = u32(params.sq_off.array)
  for i = 0, params.sq_entries - 1 do arr[i] = i end

  -- the table of direct descriptors
  local reg = ffi.new("td_rsrc_register")
  reg.nr = table_size; reg.flags = 1
  if C.td_sys_register(SYS_REGISTER, fd, REGISTER_FILES2, reg, ffi.sizeof(reg)) < 0 then
    return nil, "register files: " .. errstr()
  end

  -- the provided-buffer ring
  local bp = C.mmap(nil, NBUF * 16, 3, 0x22, -1, 0)
  local dp = C.mmap(nil, NBUF * BUFSZ, 3, 0x22, -1, 0)
  if bp == MAP_FAILED or dp == MAP_FAILED then return nil, "mmap buffers: " .. errstr() end
  local bufs = ffi.cast("td_buf *", bp)
  local bufbase = ffi.cast("uint8_t *", dp)
  local breg = ffi.new("td_buf_reg")
  breg.ring_addr = ffi.cast("uint8_t *", bp); breg.ring_entries = NBUF; breg.bgid = 0
  if C.td_sys_register(SYS_REGISTER, fd, REGISTER_PBUF_RING, breg, 1) < 0 then
    -- Ubuntu 6.8.0-136+ rejects zeroed reserved words (Launchpad #2162843, an inverted check)
    breg.resv[0] = 1
    if C.td_sys_register(SYS_REGISTER, fd, REGISTER_PBUF_RING, breg, 1) < 0 then
      return nil, "register buffer ring: " .. errstr()
    end
  end
  for i = 0, NBUF - 1 do
    bufs[i].addr = bufbase + i * BUFSZ; bufs[i].len = BUFSZ; bufs[i].bid = i
  end
  bufs[0].tail = NBUF

  local conns = ffi.cast("td_conn *", C.mmap(nil, table_size * ffi.sizeof("td_conn"), 3, 0x22, -1, 0))
  if conns == ffi.cast("td_conn *", MAP_FAILED) then return nil, "mmap conns: " .. errstr() end

  nrings = nrings + 1
  local r = {
    idx = nrings, fd = fd,
    sqes = ffi.cast("td_sqe *", sp), sq_mask = params.sq_entries - 1, sq_entries = params.sq_entries,
    sq_khead = u32(params.sq_off.head), sq_ktail = u32(params.sq_off.tail),
    sqtail = 0, sqsub = 0,
    cqes = ffi.cast("td_cqe *", base + params.cq_off.cqes), cq_mask = params.cq_entries - 1,
    cq_khead = u32(params.cq_off.head), cq_ktail = u32(params.cq_off.tail),
    bufs = bufs, bufbase = bufbase, btail = NBUF,
    conns = conns, nconn = 0, accepting = false, more = false, acc_n = 0,
    sendq = ffi.new("uint32_t[?]", table_size), nsend = 0,
    heldq = ffi.new("uint32_t[?]", table_size), nheld = 0,
    rearmq = ffi.new("uint32_t[?]", QMASK + 1), rq_head = 0, rq_tail = 0,
    readyq = ffi.new("uint32_t[?]", QMASK + 1), yq_head = 0, yq_tail = 0,
  }
  rings[nrings] = r
  return r
end

local function ring_sync(r)
  local consumed = tobit(r.sq_khead[0] - tobit(r.sqsub))
  r.sqsub = r.sqsub + consumed
end

local garg = ffi.new("td_getevents_arg")
local gts = ffi.new("td_timespec")
garg.ts = gts

-- submits what is queued and collects completions; waits for BATCH of them (or wait_us) if asked
local function ring_enter(r, wait_us, min_complete)
  local to_submit = r.sqtail - r.sqsub
  r.sq_ktail[0] = tobit(r.sqtail)
  local ret
  st.enter = st.enter + 1
  if wait_us > 0 then
    gts.sec = math.floor(wait_us / 1e6); gts.nsec = (wait_us % 1e6) * 1000
    ret = C.td_sys_enter(SYS_ENTER, r.fd, to_submit, min_complete, ENTER_GETEVENTS + ENTER_EXT_ARG, garg, 24)
  else
    ret = C.td_sys_enter(SYS_ENTER, r.fd, to_submit, 0, ENTER_GETEVENTS, nil, 0)
  end
  if ret < 0 then
    local e = ffi.errno()
    if e ~= ETIME and e ~= EINTR and e ~= EAGAIN and e ~= EBUSY then die("io_uring_enter: " .. errstr()) end
  end
  ring_sync(r)
end

local function get_sqe(r)
  if r.sqtail - r.sqsub >= r.sq_entries then
    -- the submission queue is full: hand it to the kernel now
    r.sq_ktail[0] = tobit(r.sqtail)
    st.flushes = st.flushes + 1
    local ret = C.td_sys_enter(SYS_ENTER, r.fd, r.sqtail - r.sqsub, 0, 0, nil, 0)
    if ret < 0 then
      local e = ffi.errno()
      if e ~= EINTR and e ~= EAGAIN and e ~= EBUSY then die("io_uring_enter (flush): " .. errstr()) end
    end
    ring_sync(r)
    if r.sqtail - r.sqsub >= r.sq_entries then die("submission queue stuck") end
  end
  local sqe = r.sqes[band(r.sqtail, r.sq_mask)]
  r.sqtail = r.sqtail + 1
  ffi.fill(sqe, 64)
  return sqe
end

local function arm_accept_one(r, li)
  local sqe = get_sqe(r)
  sqe.opcode = OP_ACCEPT; sqe.fd = listen_fds[li + 1]; sqe.ioprio = 1   -- multishot
  sqe.file_index = 0xffffffff                                          -- allocate a direct descriptor
  sqe.ud_lo = K_ACCEPT * 65536 + li
  r.acc_n = r.acc_n + 1
end

-- one multishot accept per listening socket, all on the ring that is being filled
local function arm_accept(r)
  for li = 0, LISTENERS - 1 do arm_accept_one(r, li) end
  r.accepting = true
  accept_ring = r
end

-- the ring is nearly full (or can take no more): its accepts end, another ring takes over
local function stop_accept(r)
  if not r.accepting then return end
  r.accepting = false
  for li = 0, LISTENERS - 1 do
    local sqe = get_sqe(r)
    sqe.opcode = OP_CANCEL; sqe.addr = ffi.cast("uint8_t *", K_ACCEPT * 65536 + li)
    sqe.ud_lo = K_IGNORE * 65536
  end
end

local function choose_accept_ring()
  if accept_ring then return end
  for i = 1, nrings do
    local r = rings[i]
    if r.nconn < table_size - 2 * MARGIN then arm_accept(r); return end
  end
  if nrings < MAX_RINGS then
    local r, e = ring_new()
    if r then
      arm_accept(r)
    else
      io.stderr:write("cannot add a ring: ", e, "\n")
    end
  end
end


local function arm_recv(r, slot)
  local c = r.conns[slot]
  local sqe = get_sqe(r)
  sqe.opcode = OP_RECV; sqe.fd = slot; sqe.flags = SQE_FIXED_FILE + SQE_BUFFER_SELECT
  sqe.ioprio = 2                                                       -- multishot
  sqe.ud_lo = K_RECV * 65536 + slot; sqe.ud_hi = c.gen
  c.flags = bor(c.flags, F_RECV)
end

local function submit_send(r, slot)
  local c = r.conns[slot]
  local sqe = get_sqe(r)
  sqe.opcode = OP_SEND; sqe.fd = slot; sqe.flags = SQE_FIXED_FILE
  sqe.addr = c.outb + c.outsent; sqe.len = c.outlen - c.outsent
  sqe.op_flags = MSG_NOSIGNAL
  sqe.ud_lo = K_SEND * 65536 + slot; sqe.ud_hi = c.gen
  c.flags = bor(c.flags, F_SENDING)
  st.sends = st.sends + 1
end

-- moves a closing connection one step further: first its response, then the receive, then the fd
local function advance_close(r, slot)
  local c = r.conns[slot]
  local f = c.flags
  if band(f, F_CLOSE_SENT) ~= 0 or c.outlen > 0 then return end
  if band(f, F_RECV) ~= 0 then
    if band(f, F_SHUT) == 0 then
      local sqe = get_sqe(r)
      sqe.opcode = OP_SHUTDOWN; sqe.fd = slot; sqe.flags = SQE_FIXED_FILE; sqe.len = 2
      sqe.ud_lo = K_IGNORE * 65536
      c.flags = bor(f, F_SHUT)
    end
    return
  end
  local sqe = get_sqe(r)
  sqe.opcode = OP_CLOSE; sqe.file_index = slot + 1
  sqe.ud_lo = K_CLOSE * 65536 + slot
  c.flags = bor(f, F_CLOSE_SENT)
end

local function start_close(r, slot)
  local c = r.conns[slot]
  c.flags = bor(c.flags, F_CLOSING)
  advance_close(r, slot)
end

local handle = app.handle
local resp = app.resp
local req = app.req
local STASH_MAX = app.REQ_CAP - 4096
local pass_reqs = 0              -- requests answered in this pass

-- Past ANSWER_CAP answers in a pass, a request is only read: it waits in its connection's buffer
-- and the connection in this first-in, first-out queue, which later passes answer first. Reading
-- is a copy of a few hundred bytes; an unread request holds a 3-4 KB kernel buffer.
local function ready_push(r, slot)
  local c = r.conns[slot]
  if band(c.flags, F_READYQ) == 0 then
    c.flags = bor(c.flags, F_READYQ)
    r.readyq[band(r.yq_tail, QMASK)] = slot; r.yq_tail = r.yq_tail + 1
    st.deferred = st.deferred + 1
  end
end

-- Answers the complete requests in req[0 .. n) and returns how many bytes they took; what is
-- left (an incomplete request, or a held write) is moved to the front of req.
local function process(r, slot, n)
  local c = r.conns[slot]
  local done, len = 0, n
  while len > 0 do
    local used = handle(len)
    if used == 0 then break end
    if used == -2 then
      -- a write during a hold: it stays in the connection's input until the hold ends
      if band(c.flags, F_HELD) == 0 then
        c.flags = bor(c.flags, F_HELD)
        r.heldq[r.nheld] = slot; r.nheld = r.nheld + 1
        st.held = st.held + 1
      end
      break
    end
    out_append(r, slot, resp, app.rlen)
    st.reqs = st.reqs + 1
    pass_reqs = pass_reqs + 1
    if used < 0 or app.close then
      c.flags = bor(c.flags, F_CLOSING)
      done = n
      break
    end
    done = done + used; len = len - used
    if len > 0 then C.memmove(req, req + used, len) end
  end
  if c.outlen > 0 and band(c.flags, F_QUEUED) == 0 then
    c.flags = bor(c.flags, F_QUEUED)
    r.sendq[r.nsend] = slot; r.nsend = r.nsend + 1
  end
  return done
end

local function process_stash(r, slot)
  local c = r.conns[slot]
  local n = c.inlen
  ffi.copy(req, c.inb, n)
  local used = process(r, slot, n)
  if used >= n then
    stash_free(r, slot)
  elseif used > 0 then
    C.memmove(c.inb, c.inb + used, n - used)
    c.inlen = n - used
  end
end

local function on_recv(r, slot, res, fl, gen)
  local c = r.conns[slot]
  local live = gen == c.gen and band(c.flags, F_OPEN) ~= 0
  if band(fl, CQE_F_BUFFER) ~= 0 then
    local bid = rshift(fl, 16)
    if live and res > 0 and band(c.flags, F_CLOSING) == 0 then
      if c.inlen > 0 or band(c.flags, F_SENDING) ~= 0 or c.outlen > 0 then
        if c.inlen + res > STASH_MAX then
          c.flags = bor(c.flags, F_CLOSING)        -- far more input than any request allows
        else
          stash_append(r, slot, r.bufbase + bid * BUFSZ, res)
          if band(c.flags, F_SENDING) == 0 and c.outlen == 0 then
            if pass_reqs >= ANSWER_CAP then ready_push(r, slot) else process_stash(r, slot) end
          end
        end
      elseif pass_reqs >= ANSWER_CAP then
        stash_append(r, slot, r.bufbase + bid * BUFSZ, res)
        ready_push(r, slot)
      else
        ffi.copy(req, r.bufbase + bid * BUFSZ, res)
        local used = process(r, slot, res)
        if used < res then stash_append(r, slot, req, res - used) end
      end
    end
    -- give the buffer back
    local b = r.bufs[band(r.btail, NBUF - 1)]
    b.addr = r.bufbase + bid * BUFSZ; b.len = BUFSZ; b.bid = bid
    r.btail = r.btail + 1
    r.bufs[0].tail = band(r.btail, 65535)
  end
  if not live then return end
  if band(fl, CQE_F_MORE) == 0 then
    c.flags = band(c.flags, bnot(F_RECV))
    if res == -ENOBUFS then
      -- The buffer ring ran dry (a burst after a stall). Arming it again now would only fail
      -- again: it waits in a queue, and each pass arms no more than there are buffers for.
      st.enobufs = st.enobufs + 1
      if band(c.flags, F_CLOSING + F_REARMQ) == 0 then
        c.flags = bor(c.flags, F_REARMQ)
        r.rearmq[band(r.rq_tail, QMASK)] = slot; r.rq_tail = r.rq_tail + 1
      end
    elseif res > 0 then
      if band(c.flags, F_CLOSING) == 0 then arm_recv(r, slot) end
    else
      c.flags = bor(c.flags, F_CLOSING)       -- end of stream or an error
    end
  end
  if band(c.flags, F_CLOSING) ~= 0 then advance_close(r, slot) end
end

local function on_send(r, slot, res, gen)
  local c = r.conns[slot]
  if gen ~= c.gen or band(c.flags, F_OPEN) == 0 then return end
  c.flags = band(c.flags, bnot(F_SENDING))
  if res < 0 then
    out_release(r, slot)
    start_close(r, slot)
    return
  end
  c.outsent = c.outsent + res
  if c.outsent < c.outlen then
    st.short_sends = st.short_sends + 1
    submit_send(r, slot)
    return
  end
  out_release(r, slot)
  if band(c.flags, F_CLOSING) ~= 0 then
    advance_close(r, slot)
  elseif c.inlen > 0 then
    if pass_reqs >= ANSWER_CAP then ready_push(r, slot) else process_stash(r, slot) end
  end
end


-- answers queued requests, oldest first, until the pass's cap
local function drain_ready(r)
  local q, conns = r.readyq, r.conns
  while r.yq_head < r.yq_tail and pass_reqs < ANSWER_CAP do
    local slot = q[band(r.yq_head, QMASK)]
    r.yq_head = r.yq_head + 1
    local c = conns[slot]
    if band(c.flags, F_READYQ) ~= 0 then
      c.flags = band(c.flags, bnot(F_READYQ))
      if band(c.flags, F_OPEN) ~= 0 and band(c.flags, F_CLOSING + F_SENDING + F_HELD) == 0
        and c.inlen > 0 and c.outlen == 0 then
        process_stash(r, slot)
      end
    end
  end
end

-- arms receives that ended for lack of buffers, at most half a buffer ring's worth per pass
local function drain_rearm(r)
  local q, conns = r.rearmq, r.conns
  local budget = rshift(NBUF, 1)
  while r.rq_head < r.rq_tail and budget > 0 do
    local slot = q[band(r.rq_head, QMASK)]
    r.rq_head = r.rq_head + 1
    local c = conns[slot]
    if band(c.flags, F_REARMQ) ~= 0 then
      c.flags = band(c.flags, bnot(F_REARMQ))
      if band(c.flags, F_OPEN) ~= 0 and band(c.flags, F_CLOSING + F_RECV) == 0 then
        arm_recv(r, slot)
        st.rearms = st.rearms + 1
        budget = budget - 1
      end
    end
  end
end

local function on_accept(r, res, fl, li)
  if res >= 0 then
    local c = r.conns[res]
    c.flags = F_OPEN; c.inlen = 0; c.outlen = 0; c.outsent = 0
    r.nconn = r.nconn + 1
    st.accepts = st.accepts + 1
    arm_recv(r, res)
    if r.nconn >= table_size - MARGIN then stop_accept(r) end
  elseif res ~= -125 then                    -- not our own cancel
    st.errors = st.errors + 1
    stop_accept(r)
  end
  if band(fl, CQE_F_MORE) == 0 then
    r.acc_n = r.acc_n - 1
    if r.accepting then
      arm_accept_one(r, li)                  -- ended without being asked to: arm it again
    elseif r.acc_n == 0 then
      if accept_ring == r then accept_ring = nil end
      choose_accept_ring()
    end
  end
end

local function on_close(r, slot)
  local c = r.conns[slot]
  out_release(r, slot)
  stash_free(r, slot)
  c.gen = c.gen + 1
  c.flags = 0
  r.nconn = r.nconn - 1
  st.closes = st.closes + 1
  if not accept_ring then choose_accept_ring() end
end

-- the hold is over: answer the writes that waited
local function release_held(r)
  local q, conns = r.heldq, r.conns
  local n = r.nheld
  r.nheld = 0
  for k = 0, n - 1 do
    local slot = q[k]
    local c = conns[slot]
    if band(c.flags, F_HELD) ~= 0 then
      c.flags = band(c.flags, bnot(F_HELD))
      if band(c.flags, F_OPEN) ~= 0 and band(c.flags, F_CLOSING + F_SENDING) == 0 and c.inlen > 0 and c.outlen == 0 then
        process_stash(r, slot)
      end
    end
  end
end

-- handles up to MAX_CQE completions; returns how many
local function reap(r)
  local head = r.cq_khead[0]
  local n = tobit(r.cq_ktail[0] - head)
  r.more = n > MAX_CQE
  if r.more then n = MAX_CQE end
  local cqes, mask = r.cqes, r.cq_mask
  for k = 0, n - 1 do
    -- plain numbers from here on: no completion or connection reference crosses a call
    local idx = band(head + k, mask)
    local lo = cqes[idx].ud_lo
    local kind = rshift(lo, 16)
    if kind == K_RECV then on_recv(r, band(lo, 65535), cqes[idx].res, cqes[idx].flags, cqes[idx].ud_hi)
    elseif kind == K_SEND then on_send(r, band(lo, 65535), cqes[idx].res, cqes[idx].ud_hi)
    elseif kind == K_ACCEPT then on_accept(r, cqes[idx].res, cqes[idx].flags, band(lo, 65535))
    elseif kind == K_CLOSE then on_close(r, band(lo, 65535))
    end
  end
  r.cq_khead[0] = tobit(head + n)
  st.cqes = st.cqes + n
  return n
end

-- after COMMIT: hand this pass's responses to the kernel
local function flush_sends(r, failed)
  local q, conns = r.sendq, r.conns
  for k = 0, r.nsend - 1 do
    local slot = q[k]
    local c = conns[slot]
    c.flags = band(c.flags, bnot(F_QUEUED))
    if band(c.flags, F_OPEN) ~= 0 and c.outlen > 0 then
      if failed then
        -- the pass's transaction didn't commit: nothing it answered may be claimed
        c.outlen = 0
        out_append(r, slot, app.R_INTERNAL.b, app.R_INTERNAL.n)
        c.flags = bor(c.flags, F_CLOSING)
      end
      if band(c.flags, F_SENDING) == 0 then submit_send(r, slot) end
    end
  end
  r.nsend = 0
end

---------------------------------------------------------------------------------------------------
-- startup

local function listen_sockets()
  local one = ffi.new("int[1]", 1)
  local sa = ffi.new("td_sockaddr_in")
  sa.family = 2; sa.port = C.htons(PORT)
  if C.inet_pton(2, tostring(HOST), ffi.cast("void *", ffi.cast("uint8_t *", sa) + 4)) ~= 1 then
    die("bad HOST " .. tostring(HOST))
  end
  for li = 1, LISTENERS do
    local fd = C.socket(2, 1, 0)
    if fd < 0 then die("socket: " .. errstr()) end
    C.setsockopt(fd, 1, 2, one, 4)           -- SO_REUSEADDR
    C.setsockopt(fd, 1, 15, one, 4)          -- SO_REUSEPORT: the kernel spreads connections over them
    C.setsockopt(fd, 6, 1, one, 4)           -- TCP_NODELAY, inherited by accepted sockets
    -- a restart can find the old process's sockets still being torn down: retry for a while
    local tries = 0
    while C.bind(fd, sa, 16) < 0 do
      tries = tries + 1
      if ffi.errno() ~= 98 or tries > 200 then die("bind " .. tostring(HOST) .. ":" .. PORT .. ": " .. errstr()) end
      C.usleep(100000)
    end
    if C.listen(fd, BACKLOG) < 0 then die("listen: " .. errstr()) end
    listen_fds[li] = fd
  end
end

local function main()
  C.signal(13, ffi.cast("td_sighandler", 1))     -- SIGPIPE: ignore

  -- direct descriptors: a ring's table can't be larger than RLIMIT_NOFILE
  local rl = ffi.new("td_rlimit")
  C.getrlimit(7, rl)
  rl.cur = rl.max
  C.setrlimit(7, rl)
  C.getrlimit(7, rl)
  table_size = math.min(65535, tonumber(rl.cur))
  if MARGIN * 4 > table_size then MARGIN = math.floor(table_size / 4) end

  local shim = ffi.load(SRC .. "/../libtdshim.so")
  if shim.td_open(SQLITE_PATH, CACHE_KB, AUTOCKPT) ~= 0 then
    die("database " .. SQLITE_PATH .. ": " .. ffi.string(shim.td_errmsg()))
  end
  app.init(shim, JWT_SECRET)
  app.started = wall()
  app.now = app.started

  listen_sockets()
  local r0, e = ring_new()
  if not r0 then die(e) end
  arm_accept(r0)
  -- Making a ring takes about 0.2 s (a 65,535-entry descriptor table, queues, buffers). Done in
  -- the middle of a ramp that stalls the loop and starts a backlog, so the rings a full run
  -- needs are made now. More are still added on demand.
  for _ = 2, math.min(PRE_RINGS, MAX_RINGS) do
    local r, e = ring_new()
    if not r then io.stderr:write("cannot add a ring: ", e, "\n"); break end
  end
  io.stderr:write(string.format("listening on %s:%d, %d slots per ring, stats %s\n",
    tostring(HOST), PORT, table_size, STATS and "on" or "off"))

  -- TD_PROFILE=<mode>,<seconds>,<file>: LuaJIT's sampling profiler (jit.p) for the first N seconds
  local prof_until = 0
  local PROFILE = os.getenv("TD_PROFILE")
  if PROFILE then
    local mode, secs, file = PROFILE:match("^([^,]*),([^,]*),(.*)$")
    require("jit.p").start(mode, file)
    prof_until = mono() + tonumber(secs)
  end
  local last_stats = mono()
  local idle = false
  local ck_phase, ck_mark, hold_start = 0, 0, 0
  local ck_boost = false
  local hold_ms = HOLD_MS
  local cpu_prev = 0
  while true do
    -- Ring 1 waits for BATCH completions or WAIT_US, whichever comes first, so a pass answers a
    -- batch of requests with one COMMIT and one system call per ring. The other rings were
    -- entered at the end of the previous pass and are only reaped here.
    local r1 = rings[1]
    local wait = WAIT_US
    if r1.more then wait = 0 end
    for i = 1, nrings do
      local r = rings[i]
      if r.more or r.yq_head < r.yq_tail or r.rq_head < r.rq_tail then wait = 0 end
    end
    -- nothing came in last pass: sleep until the first completion instead of polling every ms
    local batch = BATCH
    if wait > 0 and idle then wait = 1000000; batch = 1 end
    local te = STATS and mono() or 0
    ring_enter(r1, wait, batch)
    local t0 = mono()
    if STATS and wait <= WAIT_US and t0 - te > st.enter_max then st.enter_max = t0 - te end
    app.now = wall()
    pass_reqs = 0
    for i = 1, nrings do drain_ready(rings[i]) end
    local n = 0
    for i = 1, nrings do n = n + reap(rings[i]) end
    if n > st.max_batch then st.max_batch = n end
    if STATS then
      local d = mono() - t0
      if d > st.reap_max then st.reap_max = d end
    end
    idle = (n == 0 and r1.sqtail == r1.sqsub)

    local failed = false
    if app.in_txn then
      local tc = STATS and mono() or 0
      if shim.td_commit() ~= 0 or app.db_error then
        if not app.db_error then io.stderr:write("commit failed: ", ffi.string(shim.td_errmsg()), "\n") end
        failed = app.wrote or not app.db_error
        st.errors = st.errors + 1
      end
      app.in_txn = false; app.wrote = false; app.db_error = false
      if STATS then
        local d = mono() - tc
        st.commits = st.commits + 1
        st.commit_sum = st.commit_sum + d
        if d > st.commit_max then st.commit_max = d end
      end
    end
    local tf = STATS and mono() or 0
    for i = 1, nrings do
      local r = rings[i]
      if r.nsend > 0 then flush_sends(r, failed) end
      if r.rq_head < r.rq_tail then drain_rearm(r) end
      -- ring 1 submits at the top of the loop; the others submit their sends and collect now
      if i > 1 then
        -- a ring with no connections, no accepts armed and nothing queued needs no system call
        if r.nconn > 0 or r.accepting or r.acc_n > 0 or r.sqtail ~= r.sqsub then
          ring_enter(r, 0, 0)
          idle = false
        end
      end
    end
    if STATS then
      local d = mono() - tf
      if d > st.flush_max then st.flush_max = d end
    end

    -- Checkpoints run on the shim's thread, so their fsyncs never block this loop. A round can't
    -- copy the frames written while it runs, and the log only restarts once it is fully copied.
    -- So after the first round, writes are held (reads go on) while a short second round copies
    -- the rest; the next write then restarts the log.
    --
    -- When the loop is busy the thread gets no CPU at idle priority and the rounds do not finish.
    -- Giving up on the hold at a fixed limit then lets the log grow, the next round has more to
    -- copy than the last, and it never catches up (a 280k run reached 258k frames, commits took
    -- 300 ms, and the server ran out of memory). So: the thread has normal priority while a
    -- round runs, a hold that was too short makes the next one longer, and past CKPT_FORCE
    -- frames writes stay held until the log has been copied.
    local wf = shim.td_wal_frames()
    if ck_phase == 0 then
      if wf >= CKPT_FRAMES and shim.td_ckpt_start() == 1 then ck_phase = 1 end
    elseif ck_phase == 3 then
      local f = shim.td_wal_frames()
      if f < ck_mark or f >= ck_mark + CKPT_FRAMES then ck_phase = 0 end
    else
      local done = shim.td_ckpt_running() == 0
      local end_hold = false
      if ck_phase == 2 and wf < CKPT_FORCE and (t0 - hold_start) * 1000 > hold_ms then
        end_hold = true; ck_phase = 1          -- too slow: let writes go on, and hold longer next time
        hold_ms = math.min(hold_ms * 2, HOLD_MAX_MS)
        st.hold_fail = st.hold_fail + 1
      end
      if done then
        st.ckpts = st.ckpts + 1
        -- complete only if nothing was written since the round took its snapshot of the log
        if shim.td_wal_frames() <= shim.td_ckpt_done() then
          if ck_phase == 2 then end_hold = true; hold_ms = HOLD_MS end
          ck_phase = 3; ck_mark = shim.td_wal_frames()
        elseif (ck_phase == 1 and not end_hold) or (ck_phase == 1 and wf >= CKPT_FORCE) then
          end_hold = false
          app.hold = true; hold_start = t0; ck_phase = 2
          st.holds = st.holds + 1
          shim.td_ckpt_start()
        elseif ck_phase == 2 then
          shim.td_ckpt_start()
        else
          ck_phase = 0
        end
      end
      if end_hold and app.hold then
        app.hold = false
        local d = mono() - hold_start
        if d > st.hold_max then st.hold_max = d end
        for i = 1, nrings do release_held(rings[i]) end
      end
    end
    -- Normal priority while a round runs or the log is long, idle priority otherwise. A round is
    -- some tens of milliseconds of CPU every second or two; at idle priority it gets none of it
    -- from a busy loop, and the work only piles up.
    local want = ck_phase == 1 or ck_phase == 2 or wf >= CKPT_BOOST
    if want ~= ck_boost then
      ck_boost = want; shim.td_ckpt_boost(want and 1 or 0)
    end

    st.passes = st.passes + 1
    if STATS then
      local t1 = mono()
      local d = t1 - t0
      st.busy = st.busy + d
      if d > st.pass_max then st.pass_max = d end
      if prof_until > 0 and t1 > prof_until then
        require("jit.p").stop(); prof_until = 0
        io.stderr:write("profile written\n")
      end
      if t1 - last_stats >= 1 then
        local conns = 0
        for i = 1, nrings do conns = conns + rings[i].nconn end
        io.stderr:write(string.format(
          "conns=%d rings=%d req/s=%d (feed %d post %d like %d create %d other %d) passes=%d cqe=%d maxbatch=%d "
          .. "cpu=%.0f%% us/req=%.1f busy=%.0f%% pass_max=%.1fms enter_max=%.1fms reap_max=%.1fms flush_max=%.1fms commits=%d commit_avg=%.0fus commit_max=%.1fms wal=%d "
          .. "ckpt=%d holds=%d held=%d hold_max=%.0fms hold_fail=%d enobufs=%d rearm=%d deferred=%d stash=%d short=%d sqflush=%d acc=%d close=%d err=%d lua_kb=%.0f\n",
          conns, nrings, st.reqs, app.n_feed, app.n_post, app.n_like, app.n_create, app.n_other,
          st.passes, st.cqes, st.max_batch, (cputime() - cpu_prev) / (t1 - last_stats) * 100,
          st.reqs > 0 and (cputime() - cpu_prev) / st.reqs * 1e6 or 0, st.busy / (t1 - last_stats) * 100, st.pass_max * 1000, st.enter_max * 1000, st.reap_max * 1000, st.flush_max * 1000,
          st.commits, st.commits > 0 and st.commit_sum / st.commits * 1e6 or 0, st.commit_max * 1000,
          shim.td_wal_frames(), st.ckpts, st.holds, st.held, st.hold_max * 1000, st.hold_fail, st.enobufs, st.rearms, st.deferred, st.stash, st.short_sends, st.flushes, st.accepts, st.closes,
          st.errors, collectgarbage("count")))
        for k in pairs(st) do st[k] = 0 end
        app.n_feed, app.n_post, app.n_like, app.n_create, app.n_other = 0, 0, 0, 0, 0
        last_stats = t1
        cpu_prev = cputime()
      end
    end
  end
end

main()
