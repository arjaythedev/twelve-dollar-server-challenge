package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const pollIn = 0x1 // EPOLLIN

// user_data carries the operation kind, the connection index and the connection's generation.
// The generation is what makes a late completion for a closed connection safe to drop.
const (
	udKindShift = 56
	udGenShift  = 32
	udGenMask   = 0xFFFFFF
)

const (
	kindAccept uint8 = 1
	kindRecv   uint8 = 2
	kindSend   uint8 = 3
	kindClose  uint8 = 4
	kindCancel uint8 = 5
	kindPoll   uint8 = 6
	kindTick   uint8 = 7
	// kindStray closes a slot with no conn entry behind it. It carries no index, so it must not
	// reach onClose.
	kindStray uint8 = 8
)

func ud(kind uint8, idx, gen uint32) uint64 {
	return uint64(kind)<<udKindShift | uint64(gen&udGenMask)<<udGenShift | uint64(idx)
}

func udKind(v uint64) uint8 { return uint8(v >> udKindShift) }
func udGen(v uint64) uint32 { return uint32(v>>udGenShift) & udGenMask }
func udIdx(v uint64) uint32 { return uint32(v) }

// Connection states. A closing connection still owns its slot until the close completes.
const (
	connFree uint8 = iota
	connOpen
	// connLinger is open, answered, and waiting for the client's FIN. It is a state rather
	// than a flag beside connOpen because the distinction matters at every site that asks
	// about a connection: a lingering one is not served, but it still owns its slot, still
	// takes completions, and is still swept, re-armed and closed. Spelled as a bool, "open"
	// meant two things at eight sites and two defects followed - a lingering connection
	// answered requests after being told the connection was ending, and its idle deadline
	// moved every time it sent one, so the bound on it could never fire.
	connLinger
	connClosing
)

// conn is everything the loop knows about one connection: 48 bytes, and no pointers, so the
// whole table is one arena region the garbage collector never walks.
//
// A connection holds no buffers while it is idle. It borrows a receive buffer from the kernel
// only while data is being parsed, a response buffer only while a send is in flight, and a
// pending buffer only when bytes arrived that could not be answered yet.
type conn struct {
	gen      uint32 // bumped on open and on close
	lastSeen uint32 // engine.now when this connection last carried a request
	slot     uint32 // registered file index within its ring

	// Links in the idle wheel: the connection before and after this one in its bucket, -1 for
	// none, and which bucket that is. Valid while the connection is alive, which is exactly
	// when it is linked.
	next   int32
	prev   int32
	bucket uint8

	out      int32 // response buffer index, -1 when none is held
	outStart int32 // offset in that buffer of the first unsent byte
	outEnd   int32 // offset one past the last byte to send
	pend     int32 // pending buffer index, -1 when none is held
	pendLen  int32 // bytes of unparsed stream in it

	state     uint8
	keepAlive bool
	sentCont  bool // a 100 Continue has gone out for the request being read
	recvArmed bool
	// sendArmed means the kernel may still read this connection's response buffer, so the
	// buffer must not go back to the pool yet. closeAcked means the close completion has
	// arrived. A connection is freed only when the close is acked and no send is in flight.
	sendArmed  bool
	closeAcked bool
	// rearmQueued keeps a connection out of engine.rearm twice: the queue holds one entry per
	// connection, and in one pass both the receive and the send path can queue the same one.
	rearmQueued bool
}

// touch records that the connection carried traffic, which is what the idle sweep measures
// from. A lingering connection is not touched: its lastSeen is when it was told the connection
// was ending, and lingerTimeout bounds the wait from there. A client that keeps sending instead
// of closing would otherwise push its own deadline out and hold the slot for the full idle
// timeout, which is the one case the bound exists for.
// It reports whether the deadline actually moved, because moving it means the connection
// changes bucket in the idle wheel. lastSeen has one-second resolution, so a connection served
// many times in one second answers false after the first and relinks once.
func (cn *conn) touch(now uint32) bool {
	if cn.state == connLinger || cn.lastSeen == now {
		return false
	}
	cn.lastSeen = now
	return true
}

// deadline is when the idle sweep will close this connection if nothing more arrives on it. A
// lingering connection is waiting on one round trip, not on a user's think time.
func (cn *conn) deadline() uint32 {
	if cn.state == connLinger {
		return cn.lastSeen + lingerTimeout
	}
	return cn.lastSeen + idleTimeout
}

// serving reports whether the loop will answer requests on this connection.
func (cn *conn) serving() bool { return cn.state == connOpen }

// alive reports whether the connection still owns its slot and still takes completions, which
// is true while it is being served and while it is lingering. Every site asking "is this slot
// in use" means this; every site asking "should this be answered" means serving().
func (cn *conn) alive() bool { return cn.state == connOpen || cn.state == connLinger }

type ringState struct {
	r         *ring
	bufs      *bufRing
	idx       int
	slots     uint32
	used      uint32
	staged    bool // buffers were returned but not yet published
	pollArmed bool
}

type stats struct {
	accepted, closed            uint64
	requests, handoffs          uint64
	acceptErr, closeErr         uint64
	enobufs, sqFull, outGrew    uint64
	outExhausted, pendExhausted uint64
	closeLost, stray            uint64
	acceptStalled               uint64
	iters, drained, enterErr    uint64

	// Why connections end. A connection that goes away for any reason other than the peer
	// closing it is a defect until proven otherwise, so each reason is counted separately.
	closeEOF, closeRecvErr, closeSendErr uint64
	closeIdle                            uint64
	lingers, lingerTimeouts              uint64
	admitHeld                            uint64
	shedClose, shedSweeps                uint64
	highTicks, critTicks                 uint64
	lateHandoffs                         uint64
	polls, pollErr, armPolls             uint64

	// checkpointFrames is what says the log is being bounded, not merely checkpointed.
	checkpoints, checkpointFrames, checkpointErr uint64
}

type engine struct {
	s        *server
	a        *arena
	rings    []*ringState
	listenFD int

	conns        []conn
	slotsPerRing uint32

	// used is open connections across every ring, and the two thresholds are where the server
	// starts giving them back. Above shedSoft an answer carries Connection: close so keep-alive
	// clients cycle out on their own; above shedHard the idle sweep stops waiting out the
	// keep-alive timeout and evicts anything idle longer than a think time.
	//
	// This is what makes capacity elastic instead of a cliff. The load model has a user idle
	// 3-15 s between requests, so at any instant only a small fraction of users are actually
	// mid-request: the slot count bounds concurrent *connections*, not concurrent *users*, and
	// without shedding the table fills with users who are doing nothing and accepts get refused.
	used     uint32
	shedSoft uint32
	shedHard uint32
	// The idle wheel: one bucket per second of deadline, and the second last swept.
	wheel    [wheelSize]int32
	wheelNow uint32

	// live counts the connections shedding can still act on: open, and not already lingering
	// on their way out. used counts slots occupied, which is the right number for whether a
	// table has room for an accept and the wrong one for whether to shed - a lingering
	// connection holds a slot but has already been told to go, so counting it would make the
	// shed decision react to its own output and stay latched while lingerers keep arriving at
	// the rate shedding creates them.
	live uint32

	// The memory guard. memHigh and memCrit are unreclaimable-byte thresholds and memUsed is
	// the last sample, so which of them is crossed is a comparison rather than a third field.
	// Both pressures drive the same two actions, through shedding() and urgent(): above either
	// soft threshold an answer carries Connection: close, above either hard one the idle sweep
	// stops waiting out the full timeout.
	mem     *memProbe
	memHigh int
	memCrit int
	memUsed int

	// req is the one parsed request the loop reuses. parseRequest fills it in place and clears
	// it first, so nothing carries over between requests; holding it here rather than on the
	// stack of process() keeps its address out of escape analysis entirely, so no future change
	// to a callee can turn it into a per-request heap allocation.
	req parsed

	outBufs  []byte
	outSize  int
	outFree  []int32
	outTop   int
	pendBufs []byte
	pendSize int
	pendFree []int32
	pendTop  int

	// rearm is the queue of connections whose multishot receive ended and has to be submitted
	// again. It is drained after returned buffers are published, which is the whole reason it
	// exists: an ENOBUFS re-arm before the buffers are visible would fail the same way.
	rearm    []uint32
	rearmLen int

	accepting  int // index of the ring with an armed accept, -1 when none
	acceptFrom int // where the next search for a ring with free slots starts
	// acceptMargin is how many slots are left spare when accepting moves to the next ring. A
	// table that fills under an armed accept costs the client whose connection the kernel had
	// already accepted and could not place: it is closed, and that client sees a reset. The
	// margin covers the sockets the kernel accepts between the handoff being queued and the
	// cancel landing.
	//
	// It has to stay well inside what shedding leaves free, or it can never be satisfied and
	// every accept lands in a table that is about to fill. Shedding holds back slots/20 per
	// ring, so this reserves an eighth of that. Sized at a quarter of the table instead, the
	// steady state sat above the margin on every ring at once, and a 5-minute hold took one
	// client reset per handoff: 1,566 of them, every single handoff forced by ENFILE.
	acceptMargin uint32
	tickArmed    bool

	ts     *timespec
	tickNS int64
	start  time.Time
	now    uint32

	stats     stats
	logEvery  uint32
	lastStats uint32
}

// timespec is struct __kernel_timespec. It lives in the arena: the kernel reads it after
// io_uring_enter has returned.
type timespec struct {
	sec, nsec int64
}

type engineConfig struct {
	rings      int
	slots      uint32
	sqEntries  uint32
	cqEntries  uint32
	bufCount   uint16
	bufSize    int
	outCount   int
	pendCount  int
	rearmSlots int
	logEvery   uint32
	tickNS     int64
	memLimit   int // bytes the process may use in total; 0 means unknown
}

func newEngine(s *server, listenFD int, cfg engineConfig) (*engine, error) {
	if err := checkEndianness(); err != nil {
		return nil, err
	}
	e := &engine{
		s:            s,
		listenFD:     listenFD,
		slotsPerRing: cfg.slots,
		outSize:      hdrReserve + maxRespBytes,
		pendSize:     maxReqBytes,
		accepting:    -1,
		start:        time.Now(),
		logEvery:     cfg.logEvery,
		acceptMargin: max(32, cfg.slots/256),
	}
	// Empty buckets hold -1: the zero value would make connection 0 the head of all of them.
	for i := range e.wheel {
		e.wheel[i] = -1
	}
	// One mapping for everything the kernel and SQLite can see, sized before anything is
	// allocated out of it so an exhausted arena is a startup error, never a serving one.
	total := cfg.rings*(int(cfg.bufCount)*cfg.bufSize+int(cfg.bufCount)*16+2*pageSize) +
		cfg.outCount*e.outSize + cfg.pendCount*e.pendSize +
		cfg.rings*int(cfg.slots)*int(unsafe.Sizeof(conn{})) +
		cfg.outCount*4 + cfg.pendCount*4 + cfg.rearmSlots*4 + 16*pageSize
	e.setShedThresholds(cfg)

	a, err := newArena(total)
	if err != nil {
		return nil, err
	}
	e.a = a

	if e.conns, err = allocSlice[conn](a, cfg.rings*int(cfg.slots)); err != nil {
		return nil, fmt.Errorf("connection table: %w", err)
	}
	for i := range e.conns {
		e.conns[i].out, e.conns[i].pend = -1, -1
	}
	if e.outBufs, err = a.alloc(cfg.outCount*e.outSize, 64); err != nil {
		return nil, fmt.Errorf("response buffers: %w", err)
	}
	if e.outFree, err = allocSlice[int32](a, cfg.outCount); err != nil {
		return nil, err
	}
	for i := range e.outFree {
		e.outFree[i] = int32(i)
	}
	e.outTop = cfg.outCount
	if e.pendBufs, err = a.alloc(cfg.pendCount*e.pendSize, 64); err != nil {
		return nil, fmt.Errorf("pending buffers: %w", err)
	}
	if e.pendFree, err = allocSlice[int32](a, cfg.pendCount); err != nil {
		return nil, err
	}
	for i := range e.pendFree {
		e.pendFree[i] = int32(i)
	}
	e.pendTop = cfg.pendCount
	if e.rearm, err = allocSlice[uint32](a, cfg.rearmSlots); err != nil {
		return nil, err
	}
	tsMem, err := a.alloc(int(unsafe.Sizeof(timespec{})), 8)
	if err != nil {
		return nil, err
	}
	e.ts = (*timespec)(unsafe.Pointer(&tsMem[0]))
	e.ts.sec, e.ts.nsec = cfg.tickNS/1e9, cfg.tickNS%1e9
	e.tickNS = cfg.tickNS

	for i := 0; i < cfg.rings; i++ {
		rs, err := e.newRingState(i, cfg)
		if err != nil {
			return nil, err
		}
		e.rings = append(e.rings, rs)
	}

	// Ring 0 is the wait ring. Every other ring's completion queue is watched through a
	// multishot poll on its ring fd, which reports readable whenever that queue is non-empty.
	for i := 1; i < len(e.rings); i++ {
		e.armPoll(i)
	}
	e.armTick()
	return e, nil
}

// setShedThresholds decides where the server starts giving connections back.
//
// There are two ceilings and they are in different units, so there are two thresholds rather than
// one fused number. Slots are the smaller problem: eight rings offer about 524,000 registered
// descriptors and a 2 GB box runs out of memory long before that, so the slot thresholds are only
// the backstop for a box whose memory limit cannot be read.
//
// Memory is the real ceiling, and it is sampled rather than modelled. An earlier version divided a
// per-connection cost into a memory budget, which needed two constants nothing could calibrate:
// the published entry this one is measured against held 264,000 connections at 2.44 KB each, but
// the cost near the edge is not the cost at a comfortable load, and the budget has to cover a
// write-ahead log and a page cache that grow with the length of the run and not with the number of
// connections. Reading the cgroup every tick needs neither constant and reacts to whatever is
// actually consuming the box.
func (e *engine) setShedThresholds(cfg engineConfig) {
	// Never shed is the safe default: a zero threshold would mean always shed, so every answer
	// would carry Connection: close and every request would pay for a new connection. The memory
	// thresholds default the same way, because a sample is compared against them directly.
	e.shedSoft, e.shedHard = math.MaxUint32, math.MaxUint32
	e.memHigh, e.memCrit = math.MaxInt, math.MaxInt

	capacity := uint32(cfg.rings) * cfg.slots
	if capacity == 0 {
		return
	}
	// 95% and 98%. The thresholds keep the process alive at its ceiling; they are not the
	// ceiling. The score is connections held, so a connection shed below the real limit is one
	// the box could have served: at 75% of capacity the first rung to reach shedSoft pinned the
	// connection count at the threshold for the whole hold and never used the last quarter of
	// the machine.
	e.shedSoft = capacity - capacity/20
	e.shedHard = capacity - capacity/50

	if cfg.memLimit <= 0 {
		return
	}
	e.mem = newMemProbe()
	if e.mem == nil {
		return
	}
	// 75% and 87.5% of the limit. The gap below the limit is what the page cache lives in, and
	// it is also the headroom the sampler needs: connections arrive at perhaps a thousand a
	// second, so a sample once a second has hundreds of megabytes of warning.
	e.memHigh = cfg.memLimit - cfg.memLimit/4
	e.memCrit = cfg.memLimit - cfg.memLimit/8
}

func (e *engine) newRingState(i int, cfg engineConfig) (*ringState, error) {
	// No COOP_TASKRUN and no DEFER_TASKRUN: both hold completions until the owning task next
	// enters the kernel, which would mean a ring's queue never becomes readable while the loop
	// is blocked on ring 0 waiting for exactly that. SINGLE_ISSUER is safe: one thread submits.
	r, err := newRing(cfg.sqEntries, cfg.cqEntries, setupSingleIssuer|setupSubmitAll)
	if err != nil {
		return nil, fmt.Errorf("ring %d: %w", i, err)
	}
	if _, err := registerSparseFiles(r, cfg.slots); err != nil {
		r.close()
		return nil, fmt.Errorf("ring %d: %w", i, err)
	}
	bufs, err := newBufRing(r, e.a, uint16(i), cfg.bufCount, cfg.bufSize)
	if err != nil {
		r.close()
		return nil, fmt.Errorf("ring %d: %w", i, err)
	}
	return &ringState{r: r, bufs: bufs, idx: i, slots: cfg.slots}, nil
}

// allocSlice carves a typed, pointer-free slice out of the arena.
func allocSlice[T any](a *arena, n int) ([]T, error) {
	var zero T
	size := int(unsafe.Sizeof(zero))
	mem, err := a.alloc(n*size, 64)
	if err != nil {
		return nil, err
	}
	return unsafe.Slice((*T)(unsafe.Pointer(&mem[0])), n), nil
}

func (e *engine) run() error {
	runtime.LockOSThread()
	for {
		if e.accepting < 0 {
			e.armAccept()
		}
		if !e.tickArmed {
			e.armTick()
		}
		for i := 1; i < len(e.rings); i++ {
			if !e.rings[i].pollArmed {
				e.armPoll(i)
			}
		}
		// Every ring but ring 0 is submitted here; ring 0's batch goes out with the wait.
		for _, rs := range e.rings[1:] {
			if rs.r.toSubmit > 0 {
				rs.r.submit()
			}
		}
		if _, err := e.rings[0].r.enter(1, enterGetEvents); err != nil {
			switch err {
			case syscall.EINTR, syscall.EAGAIN, syscall.EBUSY, syscall.ETIME:
				// EBUSY means the kernel wants the completion queue drained before it takes
				// more submissions, which is what happens next anyway.
			default:
				e.stats.enterErr++
				return fmt.Errorf("io_uring_enter: %w", err)
			}
		}
		e.stats.iters++
		for _, rs := range e.rings {
			e.stats.drained += uint64(e.drain(rs))
		}
		for _, rs := range e.rings {
			if rs.staged {
				rs.bufs.advance()
				rs.staged = false
			}
		}
		e.drainRearm()
	}
}

// drain processes every queued completion on one ring.
//
// The loop is open-coded rather than going through ring.forEachCQE so that nothing on this path
// can take a closure, so draining completions does not grow the heap.
func (e *engine) drain(rs *ringState) int {
	r := rs.r
	head := *r.cqHead
	tail := atomic.LoadUint32(r.cqTail)
	n := 0
	for ; head != tail; head++ {
		c := &r.cqes[head&r.cqMask]
		switch udKind(c.userData) {
		case kindRecv:
			e.onRecv(rs, c)
		case kindSend:
			e.onSend(rs, c)
		case kindAccept:
			e.onAccept(rs, c)
		case kindClose:
			e.onClose(rs, c)
		case kindPoll:
			// A ring's queue became readable. Nothing to do: every ring is drained each
			// iteration, so the wakeup itself was the message.
			e.stats.polls++
			if c.res < 0 {
				e.stats.pollErr++
			}
			if c.flags&cqeFMore == 0 {
				e.rings[udIdx(c.userData)].pollArmed = false
			}
		case kindTick:
			e.tickArmed = false
			e.onTick()
		case kindStray:
			// A slot closed with nothing behind it. Nothing to free.
			if c.res < 0 {
				e.stats.closeErr++
			}
		case kindCancel:
			// The linked close carries the outcome; a cancel that found nothing is normal.
		}
		n++
	}
	if n > 0 {
		atomic.StoreUint32(r.cqHead, head)
	}
	return n
}

// ---- accept ----------------------------------------------------------------------------------

// armAccept arms the multishot accept on a ring with room for it, unless one is armed already.
//
// Idempotence is the point. Several completion paths lead here and more than one of them can run
// while accept is live on another ring: a planned handoff arms the next ring and then cancels
// this one, so the cancel completes with accept already armed elsewhere, and a terminating
// accept completion can arrive after openConn has handed off. Arming a second multishot on the
// same listening socket leaves an accept that no handoff can reach, because the handoff test in
// openConn only fires for the ring in e.accepting. That orphan runs until its own table fills,
// resets the client whose socket the kernel could not place, and the ENFILE path then arms
// another: a cascade rather than a one-off, in which nearly every handoff is forced by ENFILE.
func (e *engine) armAccept() {
	if e.accepting >= 0 {
		return
	}
	if e.memPressed() {
		// Admission control. Slots may be free, but memory is what the OOM killer acts on and
		// closing connections does not give it back, so the only lever left is to stop taking
		// new ones. Arrivals wait in the listening backlog; the connections already here go on
		// being served.
		return
	}
	n := len(e.rings)
	for i := 0; i < n; i++ {
		rs := e.rings[(e.acceptFrom+i)%n]
		if !e.hasRoom(rs) {
			continue
		}
		if !e.sqeSpace(rs, 1) {
			// This ring cannot take the submission now. Another may, and if none can, the top of
			// run() tries again on the next iteration.
			continue
		}
		s := rs.r.getSQE()
		s.prepAcceptMultishotDirect(int32(e.listenFD), 0, ud(kindAccept, uint32(rs.idx), 0))
		e.accepting = rs.idx
		return
	}
	// No ring can take an accept. Arming anyway, into the last slots of the fullest table, is
	// what the kernel answers with ENFILE after it has already accepted the socket, so the
	// client gets a reset. Leaving accept disarmed instead leaves the arrival in the listening
	// backlog until a close frees a slot, which costs it queueing time and no error. Shedding is
	// already running by then, so slots do free. onTick counts the seconds spent here.
}

// hasRoom reports whether a ring's table has room to arm an accept on it. The margin is what
// the handoff needs: the kernel goes on accepting into this table until the cancel lands, and
// those sockets need slots to land in.
func (e *engine) hasRoom(rs *ringState) bool {
	return rs.used+e.acceptMargin < rs.slots
}

// moveAcceptPast records that accept is no longer armed on a ring and that the search for the
// next one starts after it.
func (e *engine) moveAcceptPast(rs *ringState) {
	e.accepting = -1
	e.acceptFrom = rs.idx + 1
	e.stats.handoffs++
}

func (e *engine) onAccept(rs *ringState, c *cqe) {
	if c.res >= 0 {
		e.openConn(rs, uint32(c.res))
	} else if -c.res != int32(syscall.ECANCELED) {
		// A cancel is this loop's own planned handoff, so counting it as an accept error would
		// report one per handoff and hide the real ones among them.
		e.stats.acceptErr++
	}
	if c.flags&cqeFMore != 0 {
		return
	}
	// The multishot ended. A completion that ends it without an error carries a slot index, so
	// it must not reach the error switch below: -c.res would be a negative slot number and fall
	// to the default arm, which would clear an accept that openConn may just have armed on
	// another ring.
	if c.res >= 0 {
		if e.accepting == rs.idx {
			e.accepting = -1
		}
		e.armAccept()
		return
	}
	// A full registered file table reports ENFILE or EMFILE, and that is the handoff: the search
	// for the next ring starts after this one. Any other error is transient, so the same ring is
	// tried again.
	switch -c.res {
	case int32(syscall.ENFILE), int32(syscall.EMFILE):
		// The table filled before the planned handoff got there. The client whose connection
		// the kernel accepted and could not place has already been reset.
		e.moveAcceptPast(rs)
		e.stats.lateHandoffs++
	case int32(syscall.ECANCELED):
		// A planned handoff. accepting and acceptFrom were set when the cancel was queued and
		// the next ring is already armed, so the armAccept below must not arm a second one.
	default:
		e.accepting = -1
		e.acceptFrom = rs.idx
	}
	e.armAccept()
}

func (e *engine) openConn(rs *ringState, slot uint32) {
	if slot >= rs.slots {
		e.stats.stray++
		return
	}
	idx := uint32(rs.idx)*e.slotsPerRing + slot
	cn := &e.conns[idx]
	if cn.state != connFree {
		// The kernel frees the slot at close completion, but finishClose holds the entry until
		// an in-flight send reports back, so an accept can land here. The socket is live and
		// nothing else refers to it, so returning without closing it leaks the slot.
		e.stats.stray++
		e.closeStray(rs, slot)
		return
	}
	// Assigned whole rather than field by field, so a field added to conn later cannot be left
	// holding the previous connection's value. conn is pointer-free and lives in the arena, so
	// this is a store and not an allocation.
	*cn = conn{
		gen:       cn.gen + 1,
		state:     connOpen,
		slot:      slot,
		keepAlive: true,
		out:       -1,
		pend:      -1,
		lastSeen:  e.now,
	}
	e.wheelLink(idx, cn)
	rs.used++
	e.used++
	e.live++
	e.stats.accepted++
	e.armRecv(rs, idx, cn)

	// Move the accept on before this table is full rather than after. A table-full accept is
	// terminated with ENFILE *after* the kernel has already accepted the socket, so it closes
	// it and that client sees a reset. The ENFILE path in onAccept is still the backstop; this
	// is what stops it being the normal case.
	if e.accepting == rs.idx && !e.hasRoom(rs) && len(e.rings) > 1 {
		e.handoffAccept(rs)
	}
}

// closeStray closes a slot with no connection entry behind it.
func (e *engine) closeStray(rs *ringState, slot uint32) {
	if !e.sqeSpace(rs, 1) {
		e.stats.closeLost++
		return
	}
	s := rs.r.getSQE()
	s.prepCloseDirect(slot, ud(kindStray, 0, 0))
	if rs.r.features&featCQESkip != 0 {
		s.flags |= sqeCQESkipSuccess
	}
}

// handoffAccept moves accepting off a ring whose table is nearly full.
//
// The next ring is armed first and this one is cancelled second, so there is no instant with no
// accept armed anywhere. The cancellation is asynchronous, which is what the margin is for: the
// kernel may accept a few more connections before it lands, and they need somewhere to go.
func (e *engine) handoffAccept(rs *ringState) {
	if !e.sqeSpace(rs, 1) {
		return // the ENFILE backstop still applies
	}
	e.moveAcceptPast(rs)
	e.armAccept()

	s := rs.r.getSQE()
	s.prepCancelUserData(ud(kindAccept, uint32(rs.idx), 0), ud(kindCancel, uint32(rs.idx), 0))
	// Submitted now rather than with the rest of the batch. Everything still queued in the
	// listening socket's backlog is accepted into this table while the cancel is outstanding,
	// so the window is what the margin has to cover.
	rs.r.submit()
}

// ---- receive ---------------------------------------------------------------------------------

func (e *engine) armRecv(rs *ringState, idx uint32, cn *conn) {
	if !e.sqeSpace(rs, 1) {
		e.stats.sqFull++
		e.closeConn(rs, idx, cn)
		return
	}
	s := rs.r.getSQE()
	s.prepRecvMultishot(cn.slot, rs.bufs.bgid, ud(kindRecv, idx, cn.gen))
	cn.recvArmed = true
}

func (e *engine) onRecv(rs *ringState, c *cqe) {
	idx := udIdx(c.userData)
	cn := &e.conns[idx]
	// connClosing is stale too: the close is queued and the kernel frees the slot when it
	// completes, so answering here would send into a slot the next accept may already own.
	stale := !cn.alive() || udGen(c.userData) != cn.gen&udGenMask
	if bid, ok := bufIDFromCQE(c); ok && (stale || c.res <= 0) {
		e.recycle(rs, bid)
	}
	if stale {
		return
	}
	if c.flags&cqeFMore == 0 {
		cn.recvArmed = false
	}

	if c.res < 0 {
		switch -c.res {
		case int32(syscall.ENOBUFS):
			// The group ran dry, which ends the multishot but loses nothing: the bytes are
			// still in the socket. The re-arm waits until returned buffers are published.
			e.stats.enobufs++
			e.queueRearm(rs, idx, cn)
		case int32(syscall.ECANCELED):
			// Ours, from closeConn.
		default:
			e.stats.closeRecvErr++
			e.closeConn(rs, idx, cn)
		}
		return
	}
	if c.res == 0 {
		e.stats.closeEOF++ // the peer closed its side
		e.closeConn(rs, idx, cn)
		return
	}
	bid, ok := bufIDFromCQE(c)
	if !ok {
		e.closeConn(rs, idx, cn)
		return
	}
	e.touchConn(idx, cn)
	if cn.state == connLinger {
		// This client has already been told the connection is ending. Whatever it sent after
		// that belongs to the connection it is about to open, not to this one.
		e.recycle(rs, bid)
		if !cn.recvArmed {
			e.queueRearm(rs, idx, cn)
		}
		return
	}
	e.onData(rs, idx, cn, rs.bufs.buf(bid)[:c.res])
	e.recycle(rs, bid)
	if !cn.recvArmed && cn.state == connOpen {
		e.queueRearm(rs, idx, cn)
	}
}

// onData parses and answers everything it can out of one arrival.
//
// The fast path is the one that matters: nothing was buffered and the whole arrival parses, so
// the request bytes live only in the kernel's buffer and are never copied.
func (e *engine) onData(rs *ringState, idx uint32, cn *conn, data []byte) {
	if cn.pend < 0 {
		n := e.process(rs, idx, cn, data)
		if n < len(data) && cn.state == connOpen {
			e.stash(rs, idx, cn, data[n:])
		}
		return
	}
	if !e.appendPend(rs, idx, cn, data) {
		return
	}
	e.drainPend(rs, idx, cn)
}

// process answers as many requests as it can from buf and returns the bytes it consumed.
//
// It stops as soon as a response is in flight: one connection holds at most one response buffer,
// so a pipelined second request waits for the first answer to go out. That is also the path a
// partially arrived request takes, so pipelining needs no code of its own.
func (e *engine) process(rs *ringState, idx uint32, cn *conn, buf []byte) int {
	p := &e.req
	off := 0
	for off < len(buf) {
		if cn.out >= 0 {
			return off
		}
		switch parseRequest(buf[off:], p) {
		case parseIncomplete:
			if p.expect100 && !cn.sentCont {
				cn.sentCont = true
				e.sendLiteral(rs, idx, cn, continue100)
			}
			return off
		case parseFailed:
			// The stream is at an unknown offset, so there is nothing to resynchronise to:
			// answer once and close.
			cn.keepAlive = false
			e.respond(rs, idx, cn, p.status, p.errBody)
			return len(buf)
		}
		off += p.n
		cn.sentCont = false
		cn.keepAlive = p.keepAlive
		if cn.keepAlive && (e.shedding() || e.memCritical()) {
			// Shed rather than refuse. The client reconnects for its next request, which costs
			// a handshake and frees a slot for a user who wants to be served now.
			cn.keepAlive = false
			e.stats.shedClose++
		}
		e.serve(rs, idx, cn, p)
		if cn.state != connOpen {
			return len(buf)
		}
	}
	return off
}

func (e *engine) serve(rs *ringState, idx uint32, cn *conn, p *parsed) {
	ob := e.takeOut()
	if ob < 0 {
		// Every response buffer is in flight. Dropping one connection is the degradation; the
		// loop itself must not stall.
		e.stats.outExhausted++
		e.closeConn(rs, idx, cn)
		return
	}
	buf := e.outBuf(ob)
	status, body := e.s.handle(&p.req, buf[hdrReserve:hdrReserve:len(buf)])
	if len(body) > 0 && unsafe.SliceData(body) != &buf[hdrReserve] {
		// handle outgrew the arena buffer and reallocated onto the Go heap. The kernel reads a
		// send buffer after io_uring_enter returns, so it cannot be heap memory: copy it back.
		//
		// outSize covers every response the spec allows (see maxRespBytes), so this is reached
		// only by data outside those bounds - a username past maxAuthorBytes. It answers 500
		// rather than sending heap memory, and outGrew is the only signal that it happened.
		e.stats.outGrew++
		n := copy(buf[hdrReserve:], body)
		if n < len(body) {
			status = 500
			n = copy(buf[hdrReserve:], errInternal)
		}
		body = buf[hdrReserve : hdrReserve+n]
	}
	e.stats.requests++
	start := frameResponse(buf, len(body), status, cn.keepAlive)
	e.startSend(rs, idx, cn, ob, int32(start), int32(hdrReserve+len(body)))
}

// respond answers with a constant body, for the rejections that never reach the router.
func (e *engine) respond(rs *ringState, idx uint32, cn *conn, status int, body string) {
	ob := e.takeOut()
	if ob < 0 {
		e.stats.outExhausted++
		e.closeConn(rs, idx, cn)
		return
	}
	buf := e.outBuf(ob)
	n := copy(buf[hdrReserve:], body)
	start := frameResponse(buf, n, status, cn.keepAlive)
	e.startSend(rs, idx, cn, ob, int32(start), int32(hdrReserve+n))
}

// sendLiteral writes bytes that are already a complete response, which is only the interim
// 100 Continue.
func (e *engine) sendLiteral(rs *ringState, idx uint32, cn *conn, lit string) {
	ob := e.takeOut()
	if ob < 0 {
		e.stats.outExhausted++
		e.closeConn(rs, idx, cn)
		return
	}
	buf := e.outBuf(ob)
	n := copy(buf[hdrReserve:], lit)
	e.startSend(rs, idx, cn, ob, hdrReserve, int32(hdrReserve+n))
}

// ---- send ------------------------------------------------------------------------------------

func (e *engine) startSend(rs *ringState, idx uint32, cn *conn, ob, start, end int32) {
	cn.out, cn.outStart, cn.outEnd = ob, start, end
	e.armSend(rs, idx, cn)
}

func (e *engine) armSend(rs *ringState, idx uint32, cn *conn) {
	if !e.sqeSpace(rs, 1) {
		e.stats.sqFull++
		e.closeConn(rs, idx, cn)
		return
	}
	buf := e.outBuf(cn.out)
	s := rs.r.getSQE()
	s.prepSend(cn.slot, buf[cn.outStart:cn.outEnd], ud(kindSend, idx, cn.gen))
	cn.sendArmed = true
}

func (e *engine) onSend(rs *ringState, c *cqe) {
	idx := udIdx(c.userData)
	cn := &e.conns[idx]
	if udGen(c.userData) != cn.gen&udGenMask {
		return // a completion from a previous life of this slot
	}
	cn.sendArmed = false
	switch cn.state {
	case connClosing:
		// Closing, and this send is what the response buffer was waiting on. Dropping the
		// completion here is what leaked it, and releasing the buffer in closeConn instead is
		// what corrupted it: the kernel reads a send buffer after io_uring_enter returns.
		e.finishClose(rs, idx, cn)
		return
	case connLinger:
		// The final response is already out and this side is waiting for the client's FIN.
		// There is nothing further to send, and nothing to finish: the close has not started.
		return
	}
	if c.res < 0 {
		if -c.res != int32(syscall.ECANCELED) {
			e.stats.closeSendErr++
			e.closeConn(rs, idx, cn)
		}
		return
	}
	cn.outStart += c.res
	if cn.outStart < cn.outEnd {
		// A short send. There is only ever one send in flight per connection, so the remainder
		// can simply go out as the next one.
		e.armSend(rs, idx, cn)
		return
	}
	e.releaseOut(cn)
	if !cn.keepAlive {
		// Wait for the client's FIN rather than sending one.
		//
		// Closing first makes this side the active closer, and the socket then sits in
		// FIN_WAIT_2 still holding the response it just sent, charged to this cgroup until the
		// client acknowledges. A 5-minute hold that shed 77,000 connections grew socket memory
		// by 300 MB that way, with the connection count flat, and that memory is the
		// unreclaimable kind. Waiting costs a slot in this server's own table for the
		// millisecond a client takes to close, and a slot is not a socket buffer.
		// lastSeen starts the linger clock, so it is set here and never moved again.
		cn.lastSeen = e.now
		cn.state = connLinger
		// The deadline just changed from a think time to a round trip, so the bucket changes
		// with it.
		e.wheelUnlink(idx, cn)
		e.wheelLink(idx, cn)
		e.live--
		// Whatever is half-parsed in the pending buffer will never be answered now, and there
		// are far fewer of those buffers than there are slots.
		e.releasePend(cn)
		e.stats.lingers++
		if !cn.recvArmed {
			e.queueRearm(rs, idx, cn)
		}
		return
	}
	e.drainPend(rs, idx, cn)
	if cn.state == connOpen && !cn.recvArmed {
		e.queueRearm(rs, idx, cn)
	}
}

// ---- close -----------------------------------------------------------------------------------

func (e *engine) closeConn(rs *ringState, idx uint32, cn *conn) {
	if !cn.alive() {
		return
	}
	// Kept because the revert below has to restore what was found, not assume connOpen: a
	// lingering connection brought back as open would be served again after being told the
	// connection was ending.
	was := cn.state
	e.wheelUnlink(idx, cn)
	cn.state = connClosing

	// A pending multishot receive holds its own reference to the socket, so releasing the slot
	// under it is not enough: cancel first, hard-linked to the close so the close still runs
	// when the cancel finds nothing, which is the common case.
	want := uint32(1)
	if cn.recvArmed {
		want = 2
	}
	// Checked before anything is released, because this path reverts: releasing first threw
	// away a response armSend had just framed.
	if !e.sqeSpace(rs, want) {
		// Leaving the slot occupied would leak it, so this is counted rather than ignored. The
		// idle sweep reaches the connection again on the next pass.
		e.stats.closeLost++
		cn.state = was
		e.wheelLink(idx, cn)
		return
	}
	if was == connOpen {
		// A lingering connection left the sheddable population when it began lingering, so
		// only a connection closed while still servable leaves it here. Counted at this
		// transition rather than at the close completion, because connClosing has by then
		// overwritten the state that says which it was.
		e.live--
	}
	// The pending buffer is only ever read by this loop, so it can go back now. The response
	// buffer cannot: while a send is in flight the kernel reads it after io_uring_enter
	// returns, so returning it here let the next connection to take it write its own response
	// into bytes still on their way out. finishClose releases it when that send reports back.
	e.releasePend(cn)
	if !cn.sendArmed {
		e.releaseOut(cn)
	}
	if cn.recvArmed {
		s := rs.r.getSQE()
		s.prepCancelFixedFile(cn.slot, ud(kindCancel, idx, cn.gen))
		s.flags |= sqeIOHardlink
		if rs.r.features&featCQESkip != 0 {
			s.flags |= sqeCQESkipSuccess
		}
		cn.recvArmed = false
	}
	s := rs.r.getSQE()
	s.prepCloseDirect(cn.slot, ud(kindClose, idx, cn.gen))
}

func (e *engine) onClose(rs *ringState, c *cqe) {
	idx := udIdx(c.userData)
	cn := &e.conns[idx]
	if cn.state != connClosing {
		e.stats.stray++
		return
	}
	if c.res < 0 {
		e.stats.closeErr++
	}
	cn.closeAcked = true
	e.finishClose(rs, idx, cn)
}

// finishClose frees the connection once everything that could still touch its buffers has
// reported back. The slot is not reused and the generation is not bumped before that, because
// either would hand this connection's response buffer to the next one while the kernel is still
// reading it.
func (e *engine) finishClose(rs *ringState, idx uint32, cn *conn) {
	if !cn.closeAcked || cn.sendArmed {
		return
	}
	e.releaseOut(cn)
	e.releasePend(cn)
	cn.state = connFree
	cn.gen++
	cn.recvArmed = false
	cn.closeAcked = false
	rs.used--
	e.used--
	e.stats.closed++
}

// ---- buffers ---------------------------------------------------------------------------------

func (e *engine) recycle(rs *ringState, bid uint16) {
	rs.bufs.put(bid)
	rs.staged = true
}

func (e *engine) outBuf(i int32) []byte {
	off := int(i) * e.outSize
	return e.outBufs[off : off+e.outSize : off+e.outSize]
}

func (e *engine) takeOut() int32 {
	if e.outTop == 0 {
		return -1
	}
	e.outTop--
	return e.outFree[e.outTop]
}

func (e *engine) releaseOut(cn *conn) {
	if cn.out < 0 {
		return
	}
	e.outFree[e.outTop] = cn.out
	e.outTop++
	cn.out, cn.outStart, cn.outEnd = -1, 0, 0
}

func (e *engine) pendBuf(i int32) []byte {
	off := int(i) * e.pendSize
	return e.pendBufs[off : off+e.pendSize : off+e.pendSize]
}

func (e *engine) releasePend(cn *conn) {
	if cn.pend < 0 {
		return
	}
	e.pendFree[e.pendTop] = cn.pend
	e.pendTop++
	cn.pend, cn.pendLen = -1, 0
}

// stash copies bytes that could not be answered yet out of the kernel's buffer, so the buffer can
// go straight back to the group.
func (e *engine) stash(rs *ringState, idx uint32, cn *conn, data []byte) {
	if e.pendTop == 0 {
		e.stats.pendExhausted++
		e.closeConn(rs, idx, cn)
		return
	}
	e.pendTop--
	cn.pend = e.pendFree[e.pendTop]
	cn.pendLen = int32(copy(e.pendBuf(cn.pend), data))
	if int(cn.pendLen) < len(data) {
		e.respondAndClose(rs, idx, cn, 431, errHeadTooLarge)
	}
}

func (e *engine) appendPend(rs *ringState, idx uint32, cn *conn, data []byte) bool {
	buf := e.pendBuf(cn.pend)
	n := copy(buf[cn.pendLen:], data)
	cn.pendLen += int32(n)
	if n < len(data) {
		e.respondAndClose(rs, idx, cn, 431, errHeadTooLarge)
		return false
	}
	return true
}

func (e *engine) respondAndClose(rs *ringState, idx uint32, cn *conn, status int, body string) {
	cn.keepAlive = false
	e.releasePend(cn)
	if cn.out < 0 {
		e.respond(rs, idx, cn, status, body)
	} else {
		e.closeConn(rs, idx, cn)
	}
}

// drainPend answers whatever is sitting in the pending buffer and compacts what is left.
func (e *engine) drainPend(rs *ringState, idx uint32, cn *conn) {
	if cn.pend < 0 || cn.pendLen == 0 {
		return
	}
	buf := e.pendBuf(cn.pend)[:cn.pendLen]
	n := e.process(rs, idx, cn, buf)
	if cn.state != connOpen {
		return
	}
	switch {
	case n == int(cn.pendLen):
		e.releasePend(cn)
	case n > 0:
		copy(buf, buf[n:])
		cn.pendLen -= int32(n)
	}
}

// ---- re-arm queue ----------------------------------------------------------------------------

func (e *engine) queueRearm(rs *ringState, idx uint32, cn *conn) {
	if cn.rearmQueued {
		return
	}
	if e.rearmLen == len(e.rearm) {
		// One entry per connection, so the queue is sized for every connection that can be in
		// it. Closing keeps the invariant that an open connection is always being read.
		e.stats.stray++
		e.closeConn(rs, idx, cn)
		return
	}
	e.rearm[e.rearmLen] = idx
	e.rearmLen++
	cn.rearmQueued = true
}

func (e *engine) drainRearm() {
	for i := 0; i < e.rearmLen; i++ {
		idx := e.rearm[i]
		cn := &e.conns[idx]
		cn.rearmQueued = false
		if !cn.alive() || cn.recvArmed {
			continue
		}
		rs := e.rings[int(idx/e.slotsPerRing)]
		e.armRecv(rs, idx, cn)
	}
	e.rearmLen = 0
}

// ---- clock -----------------------------------------------------------------------------------

func (e *engine) armTick() {
	rs := e.rings[0]
	if !e.sqeSpace(rs, 1) {
		return
	}
	s := rs.r.getSQE()
	s.prepTimeout(e.ts, ud(kindTick, 0, 0))
	e.tickArmed = true
}

// armPoll puts ring i's readiness under ring 0's single wait point.
//
// The poll is one-shot and re-armed after every completion, and that is not a style choice. A
// multishot poll across rings reports IORING_CQE_F_MORE and then delivers exactly one event
// ever: seven secondary rings carrying 160 requests produced three poll completions in total,
// and every one of those requests waited for the clock tick instead. A one-shot poll re-armed by hand delivers every one. All of it is
// measured in poll_linux_test.go: against the ring fd and against a registered eventfd, and with
// and without IORING_POLL_ADD_LEVEL, because without the level flag a multishot poll is edge
// triggered and "fires once" would be a property of the flags rather than of multishot. It is
// not: the level variants deliver one event too. The result is the same in all four, so the ring
// fd is used, no eventfd is needed, and the re-arm is not avoidable by setting a flag.
//
// Re-arming after the drain is what makes it safe: a completion that lands between the drain and
// the re-arm leaves the ring readable, so the poll fires immediately instead of being missed.
func (e *engine) armPoll(i int) {
	rs := e.rings[0]
	if !e.sqeSpace(rs, 1) {
		return
	}
	s := rs.r.getSQE()
	s.prepPollAdd(int32(e.rings[i].r.fd), pollIn, 0, ud(kindPoll, uint32(i), 0))
	e.rings[i].pollArmed = true
	e.stats.armPolls++
}

// onTick is the only place the clock is read. Once a second is enough: lastSeen only feeds the
// idle sweep, and a per-request clock read would cost more than the sweep saves.
func (e *engine) onTick() {
	e.now = uint32(time.Since(e.start) / time.Second)
	e.sampleMemory()
	if e.accepting < 0 {
		// Counted per tick rather than per loop iteration, so the number reads as seconds spent
		// unable to accept rather than as iterations, which is not a unit anyone can use.
		e.stats.acceptStalled++
		if e.memPressed() {
			e.stats.admitHeld++
		}
	}
	e.checkpointWAL()
	e.sweepIdle()
	if e.logEvery > 0 && e.now-e.lastStats >= e.logEvery {
		e.lastStats = e.now
		e.logStats()
	}
}

const (
	idleTimeout = 180 // seconds; k6 users idle at most ~15 s and nginx keeps 65 s
	// lingerTimeout bounds how long a client that was told the connection is ending may hold the
	// slot without closing it. A client that behaves closes within a round trip; this is for one
	// that does not. The rolling sweep reaches a slot every 32 seconds, so this is the floor
	// rather than the actual delay, and under pressure the sweep walks the whole table.
	lingerTimeout = 10
)

// checkpointWAL bounds the write-ahead log, once a tick. wal_autocheckpoint is 0, so nothing
// else does and the log would otherwise grow for the whole run.
//
// A tick rather than a threshold because automatic means it fires inside whichever request
// crosses the threshold, and that one client pays for the whole copy.
func (e *engine) checkpointWAL() {
	frames, err := e.s.db.checkpoint()
	if err != nil {
		e.stats.checkpointErr++
		return
	}
	if frames > 0 {
		e.stats.checkpoints++
		e.stats.checkpointFrames += uint64(frames)
	}
}

// sampleMemory reads the cgroup's unreclaimable bytes. One read of one file per second, off the
// request path. With no probe the thresholds are MaxInt, so no sample can cross them.
func (e *engine) sampleMemory() {
	if e.mem == nil {
		return
	}
	e.memUsed = e.mem.sample()
	switch {
	case e.memUsed >= e.memCrit:
		e.stats.critTicks++
	case e.memUsed >= e.memHigh:
		e.stats.highTicks++
	}
}

// The two pressures act on different things, because the two resources are freed by different
// means.
//
// Slots are freed by closing: a slot given up by an idle user is a slot a waiting one can have,
// so slot pressure drives retention - shedding() sheds keep-alive, urgent() stops the sweep
// waiting out the full timeout.
//
// Memory is not. A shed connection is replaced by the client that reconnects, so shedding holds
// the population level and frees nothing; what it frees is a slot, which was never the scarce
// resource. Memory pressure therefore drives admission instead: memPressed() stops arming the
// accept, which is the only thing in this design that bounds how far the population can grow.
// memCritical() sheds as well, to drain rather than merely stop growing, but it deliberately
// does not tighten the idle sweep: evicting users faster only raises the rate at which they
// reconnect, and each reconnection is a new socket.
func (e *engine) shedding() bool { return e.live >= e.shedSoft }

func (e *engine) urgent() bool { return e.live >= e.shedHard }

func (e *engine) memPressed() bool { return e.memUsed >= e.memHigh }

func (e *engine) memCritical() bool { return e.memUsed >= e.memCrit }

// The idle wheel.
//
// Every alive connection sits in exactly one bucket, chosen by the second it falls due:
// lastSeen plus the timeout for its state. Sweeping a tick is then walking the bucket that comes
// due this second, so the sweep costs what it evicts rather than what the table holds. Walking
// the table instead cost a load and a compare per slot, which at 8 rings of 65,471 slots is
// half a million of each inside one tick, and under pressure that was every tick.
//
// It also makes the pressure path LRU-ordered. The buckets after the one just swept hold the
// connections closest to falling due, so evicting forward from there evicts the most idle
// first - where dropping every connection's timeout to a few seconds at once evicted
// indiscriminately, and a user mid-think-time was as likely to go as one who had vanished.
//
// wheelSize must exceed the longest timeout, or a deadline could wrap onto a bucket that has
// not been swept yet; a power of two makes the bucket index a mask instead of a divide.
const wheelSize = 256

func (e *engine) wheelLinkAt(idx uint32, cn *conn, b uint8) {
	cn.bucket = b
	cn.prev = -1
	cn.next = e.wheel[b]
	if cn.next >= 0 {
		e.conns[cn.next].prev = int32(idx)
	}
	e.wheel[b] = int32(idx)
}

func (e *engine) wheelLink(idx uint32, cn *conn) {
	e.wheelLinkAt(idx, cn, uint8(cn.deadline()&(wheelSize-1)))
}

func (e *engine) wheelUnlink(idx uint32, cn *conn) {
	if cn.prev >= 0 {
		e.conns[cn.prev].next = cn.next
	} else if e.wheel[cn.bucket] == int32(idx) {
		e.wheel[cn.bucket] = cn.next
	}
	if cn.next >= 0 {
		e.conns[cn.next].prev = cn.prev
	}
	cn.next, cn.prev = -1, -1
}

// touchConn records traffic on a connection and keeps it in the right bucket.
func (e *engine) touchConn(idx uint32, cn *conn) {
	if !cn.touch(e.now) {
		return
	}
	b := uint8(cn.deadline() & (wheelSize - 1))
	if b == cn.bucket {
		return
	}
	e.wheelUnlink(idx, cn)
	e.wheelLinkAt(idx, cn, b)
}

// sweepIdle closes the connections that have fallen due, and under pressure keeps going into the
// buckets that are closest to due.
func (e *engine) sweepIdle() {
	// One bucket per second, catching up if a tick arrived late: skipping a second would leave
	// its bucket unswept until the wheel came round again. wheelSize bounds the catch-up.
	first := e.wheelNow + 1
	if e.now-e.wheelNow >= wheelSize {
		first = e.now - wheelSize + 1
	}
	for s := first; s <= e.now; s++ {
		e.sweepBucket(uint8(s&(wheelSize-1)), s)
	}
	e.wheelNow = e.now

	// Under pressure the sweep is not housekeeping: it is how slots are reclaimed, and a slot
	// reclaimed late is a connection refused. Each step forward takes the next-most-idle
	// bucket, and it stops as soon as the pressure is gone.
	for s := e.now + 1; s <= e.now+wheelSize-1; s++ {
		if !e.urgent() {
			break
		}
		e.stats.shedSweeps++
		e.sweepBucket(uint8(s&(wheelSize-1)), forceDue)
	}
}

// forceDue sweeps a bucket regardless of the deadlines in it, which is what the pressure path
// wants: those connections are the most idle ones left.
const forceDue = ^uint32(0)

// sweepBucket closes every connection in a bucket that is due by the given second. A later
// deadline belongs to a connection the wheel has wrapped onto this bucket, which is left alone.
func (e *engine) sweepBucket(b uint8, due uint32) {
	for i := e.wheel[b]; i >= 0; {
		idx := uint32(i)
		cn := &e.conns[idx]
		// Read before the close, which unlinks this entry.
		i = cn.next
		if due != forceDue && cn.deadline() > due {
			continue
		}
		if cn.state == connLinger {
			e.stats.lingerTimeouts++
		} else {
			e.stats.closeIdle++
		}
		e.closeConn(e.rings[int(idx/e.slotsPerRing)], idx, cn)
		if cn.alive() {
			// The close could not be queued and reverted. Retry on the next tick rather than
			// wait for the wheel to come round to this bucket again.
			e.wheelUnlink(idx, cn)
			e.wheelLinkAt(idx, cn, uint8((e.now+1)&(wheelSize-1)))
		}
	}
}

func (e *engine) logStats() {
	st := &e.stats
	open := uint64(0)
	for _, rs := range e.rings {
		open += uint64(rs.used)
	}
	fmt.Fprintf(os.Stderr,
		"stats t=%ds open=%d live=%d accepted=%d closed=%d requests=%d handoffs=%d "+
			"iters=%d drained=%d eof=%d recverr=%d senderr=%d linger=%d lingerexpired=%d idle=%d late=%d polls=%d pollerr=%d armpolls=%d "+
			"enobufs=%d outbusy=%d pendbusy=%d sqfull=%d outgrew=%d acceptstall=%d admithold=%d "+
			"shedclose=%d shedsweeps=%d memmb=%d highticks=%d critticks=%d "+
			"ckpt=%d ckptframes=%d ckpterr=%d "+
			"stray=%d closelost=%d accepterr=%d closeerr=%d overflow=%d\n",
		e.now, open, e.live, st.accepted, st.closed, st.requests, st.handoffs,
		st.iters, st.drained, st.closeEOF, st.closeRecvErr, st.closeSendErr,
		st.lingers, st.lingerTimeouts, st.closeIdle, st.lateHandoffs, st.polls, st.pollErr, st.armPolls,
		st.enobufs, st.outExhausted, st.pendExhausted, st.sqFull, st.outGrew,
		st.acceptStalled, st.admitHeld, st.shedClose, st.shedSweeps,
		e.memUsed>>20, st.highTicks, st.critTicks,
		st.checkpoints, st.checkpointFrames, st.checkpointErr,
		st.stray, st.closeLost, st.acceptErr, st.closeErr,
		e.rings[0].r.overflowed())
}

// ---- submission ------------------------------------------------------------------------------

// sqeSpace makes room for n submission entries, flushing the batch once if the queue is full.
// Entries reserved after it returns true are contiguous, which a hard-linked pair needs.
func (e *engine) sqeSpace(rs *ringState, n uint32) bool {
	r := rs.r
	if r.sqeTail-atomic.LoadUint32(r.sqHead)+n <= r.sqEntries {
		return true
	}
	r.submit()
	return r.sqeTail-atomic.LoadUint32(r.sqHead)+n <= r.sqEntries
}

// prepTimeout arms a one-shot relative timeout. It is re-armed on every completion rather than
// using a multishot timeout, which is newer than the kernels this has to run on.
func (s *sqe) prepTimeout(ts *timespec, userData uint64) {
	s.opcode = opTimeout
	s.fd = -1
	s.addr = uint64(uintptr(unsafe.Pointer(ts)))
	s.length = 1
	s.off = 0
	s.userData = userData
}

// ---- from arena_linux.go -------------------------------------------------------------------------------

const pageSize = 4096

// arena is a bump allocator over one anonymous mapping. There is no free: every region it hands
// out lives for the process's lifetime, which is exactly the lifetime of everything it is used
// for.
type arena struct {
	mem  []byte
	used int
}

// newArena maps size bytes, rounded up to a page, and prefaults them. MAP_POPULATE costs startup
// time and buys the absence of a page fault on the first touch of every buffer, which would
// otherwise land in the middle of the first requests served.
func newArena(size int) (*arena, error) {
	size = roundUp(size, pageSize)
	mem, err := syscall.Mmap(-1, 0, size,
		syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_PRIVATE|syscall.MAP_ANONYMOUS|syscall.MAP_POPULATE)
	if err != nil {
		return nil, fmt.Errorf("mmap arena (%d bytes): %w", size, err)
	}
	return &arena{mem: mem}, nil
}

// alloc returns n bytes aligned to align, which must be a power of two.
func (a *arena) alloc(n, align int) ([]byte, error) {
	start := roundUp(a.used, align)
	end := start + n
	if end > len(a.mem) {
		return nil, fmt.Errorf("arena exhausted: need %d bytes at offset %d, have %d",
			n, start, len(a.mem))
	}
	a.used = end
	return a.mem[start:end:end], nil
}

// allocPages returns n bytes starting on a page boundary. Memory registered with the kernel, such
// as a provided buffer ring, has to be page aligned.
func (a *arena) allocPages(n int) ([]byte, error) { return a.alloc(n, pageSize) }

func (a *arena) free() error {
	if a.mem == nil {
		return nil
	}
	m := a.mem
	a.mem, a.used = nil, 0
	return syscall.Munmap(m)
}

func roundUp(v, align int) int { return (v + align - 1) & ^(align - 1) }
