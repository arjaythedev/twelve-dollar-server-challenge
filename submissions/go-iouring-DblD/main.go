//go:build linux

// The event loop: one thread, a few io_urings, no goroutine per connection.
// What a request means is in api.go; how it reaches SQLite is in db.go and db.c.
//
// The whole server is this cycle:
//
//  1. wait on ring 0 for a batch of completions (or 1 ms), then collect from the other rings
//  2. drain every ring: a new connection, bytes from a client, a send that finished, a
//     connection that went away. Requests are answered into buffers, inside ONE database
//     transaction for the whole pass
//  3. endPass(): commit that transaction, and only then queue the responses for sending
//  4. hand the other rings their sends; ring 0's go out with the next wait. Go to 1
//
// Step 3 is group commit: if 40 likes arrive in one pass they cost one commit, not 40, and
// rule 6 holds because nothing is sent before the commit has succeeded.
//
// WHY SEVERAL RINGS (stage 4): the process may hold 65,535 file descriptors. Sockets kept in
// an io_uring's own file table ("direct descriptors") do not count against that, but one
// table cannot be larger than the same 65,535. So connections fill ring 0's table, then
// spill into ring 1's, and so on. The multi-ring loop order and the two accept details below
// are the ones go-iouring-davidefrageri measured on the scoring box; credit to that entry.
package main

import (
	"bytes"
	"log"
	"net"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"syscall"
	"time"
	"unsafe"
)

// Each request we give the kernel carries a 64-bit tag that comes back with its result:
// what kind of operation (8 bits), which ring (8), the connection's generation (16), and a
// number (32) that is the connection's slot, or for accepts the listener, or for sends the
// send buffer.
const (
	kindAccept = 1
	kindRecv   = 2
	kindSend   = 3
	kindClose  = 4
	kindIgnore = 5 // shutdowns and cancels: nothing to do when they complete
)

func tag(kind, ri int, gen uint16, n int) uint64 {
	return uint64(kind)<<56 | uint64(ri)<<48 | uint64(gen)<<32 | uint64(uint32(n))
}

const (
	maxRings  = 16
	listeners = 8 // SO_REUSEPORT sockets on the same port: eight accept queues instead of one
	// Receive buffers lent to the kernel, per ring, shared by all its connections. A request
	// is 100 to 600 bytes, so 1 KB holds nearly all of them; a longer one arrives in pieces.
	// There must be more buffers than requests that can arrive on one ring during one pass:
	// with 1,024 the pool ran dry near 190,000 users on the scoring hardware (a 4 ms pass).
	recvBufCount = 8192
	recvBufSize  = 1024

	// Responses being written at one time. A feed of 20 maximum-size posts is about 63 KB of
	// JSON, so 128 KB always fits. This is address space, not memory: the buffers are used
	// most-recently-freed first, so only the first few KB of a handful are ever touched.
	sendBufCount = 8192
	sendBufSize  = 128 << 10

	// Batching: sleep until this many completions are ready, but never longer than the window.
	// Under light load that adds up to 1 ms to a response; in exchange one wake-up, one
	// system call and one commit are shared by everything that arrived in that millisecond.
	waitFor    = 256
	waitWindow = time.Millisecond

	// The answer budget: how many requests one pass answers. Without it a pass is as long as
	// its backlog, and every response waits for the pass's commit. A reply that leaves more
	// than 40 ms after its request costs the kernel an extra acknowledgement packet, so once
	// a pass ran long every request in it and behind it got dearer, and the backlog fed itself:
	// at 200,000 users and 71 % CPU the same build passed or failed from run to run (measured
	// on the scoring hardware: up to 21,700 delayed ACKs a second inside a burst, under 50
	// outside). Requests past the budget wait as small records and are answered newest first,
	// which keeps fresh ones inside the 40 ms; one that has waited oldestMax is given a share
	// of every pass so that none waits without bound. The policy is the one
	// objc-uring-delneg (#326) and fsharp-uring-delneg (#398) describe in their READMEs.
	answerCap = 256
	oldestMax = time.Second

	// The write hold (see db.c). When the checkpointer has copied nearly all of a large WAL,
	// writes wait, for at most holdMax, while it copies the rest, so the WAL can start over.
	// Reads are never held. After a hold, wait holdGap before another: the WAL's size is only
	// known again after the next commit.
	holdMax = 250 * time.Millisecond
	holdGap = 50 * time.Millisecond
	// writeWindow: after a pass that committed writes, the next writes wait this long, while
	// reads are answered as they come. One commit then carries about twenty passes' worth
	// of writes: pages that several of them change (the end of each table and index) go to
	// the WAL once instead of once per pass, so there is less to write, less to checkpoint
	// and fewer commits. A write that arrives with the window open is not delayed at all.
	// 20 ms is fsharp-uring-delneg's value (#398); it stays well under the 40 ms after which
	// a late reply costs the kernel an extra acknowledgement.
	writeWindow = 20 * time.Millisecond
)

// conn is everything we remember about a connection: 8 bytes, no pointers.
type conn struct {
	gen     uint16 // incremented each time the slot is reused, to recognise stale completions
	closing bool
	held    bool  // request bytes are parked in server.partial for this connection
	queued  bool  // its next request is parsed and waiting in server.backlog or server.heldWrites
	waiting bool  // ...because there was no free send buffer (it is on server.sendWait)
	out     int32 // send buffer in use + 1, or 0. One response in flight per connection.
}

type ringState struct {
	u     *ring
	recv  *bufPool
	conns []conn // indexed by direct-descriptor slot
	open  int
	full  bool // too close to the end of its table to accept more
	armed [listeners]bool

	// Connections whose receive stopped because the buffer pool was empty. They are re-armed
	// at the end of a pass, when the pass's buffers are back with the kernel, and never more
	// of them than there are buffers. Re-arming at once (the first version) spins: the new
	// receive finds the pool still empty and fails again, 1.9 million times in 10 s when it
	// happened on the scoring hardware.
	starved     []parkedConn
	starvedHead int
}

type server struct {
	rings     []*ringState
	accepting int // the ring new connections currently go to
	lfds      [listeners]int
	slots     int // direct-descriptor slots per ring
	margin    int
	started   time.Time
	secret    []byte
	verify    verifier

	json []byte // the JSON body being built for it

	// Send buffers: a slab outside the Go heap, with a free list of indexes.
	sendMem  []byte
	sendFree []int32
	sendLen  []int32 // bytes in the buffer
	sendOff  []int32 // bytes already written (a send can be partial)
	sendSlot []int32 // the connection slot the buffer is being sent on

	// Request bytes that could not be handled yet, per connection: a request split across
	// packets, or one that arrived while the previous response was still being sent.
	partial map[uint64][]byte

	// The current pass: whether its transaction is open, and the responses waiting for its commit.
	inTxn   bool
	pending []pendingSend

	// The write hold.
	holding     bool
	holdStart   time.Time
	holdBase    int // db_backfills() when the hold began
	holdNotTill time.Time

	// The current pass's remaining answers, and the requests waiting for a later pass.
	budget     int
	nowNano    int64
	backlog    deque     // past the budget: answered newest first
	heldWrites deque     // writes that wait for a write hold or the write window to end: answered oldest first
	writesWait bool      // this pass does not execute writes (a hold, or the window is shut)
	wrote      bool      // this pass executed a write
	nextWrites time.Time // when the write window opens again
	creates    []createReq
	createFree []int
	parkedMax  int           // largest backlog since the last stats line
	waitMax    time.Duration // longest a request waited in it
	sendWait   []parkedConn  // connections with a request held because no send buffer was free

	passes, requests, commits, noBufs, sendWaits int // since the last stats line
	holds, holdTimeouts                          int
	holdLongest, passLongest                     time.Duration
	enterLongest                                 time.Duration // longest wait-and-collect of the window, and the completions it brought
	enterCount, passCount                        int           // ...and the requests answered in the longest pass
	warmed                                       uint32        // see warm
}

type parkedConn struct {
	ri, slot int
	gen      uint16
}

type pendingSend struct{ ri, slot, buf int }

func key(ri, slot int) uint64 { return uint64(ri)<<32 | uint64(uint32(slot)) }

func main() {
	// io_uring's single-issuer mode requires that one OS thread does all the submitting,
	// so pin this goroutine to its thread before creating a ring.
	runtime.LockOSThread()
	// The loop allocates almost nothing, so collecting on the usual schedule (every time the
	// heap doubles) would be pauses for no benefit. Instead: no schedule, and a ceiling. The
	// write path still allocates a little per request, and when that garbage reaches the
	// ceiling it is collected. The ceiling is small because memory is the scarce thing here:
	// measured at 200,000 users, a 512 MB ceiling left 533 MB free on the box.
	debug.SetGCPercent(-1)
	debug.SetMemoryLimit(64 << 20)

	host, port := mustEnv("HOST"), mustEnv("PORT")
	must(dbOpen(mustEnv("SQLITE_PATH")))

	s := &server{started: time.Now(), partial: map[uint64][]byte{}, backlog: newDeque(), heldWrites: newDeque(), secret: []byte(mustEnv("JWT_SECRET")), verify: newVerifier([]byte(mustEnv("JWT_SECRET"))),
		json:    make([]byte, 0, sendBufSize),
		sendMem: mmapAnon(sendBufCount * sendBufSize), sendLen: make([]int32, sendBufCount), sendOff: make([]int32, sendBufCount), sendSlot: make([]int32, sendBufCount)}
	for i := sendBufCount - 1; i >= 0; i-- {
		s.sendFree = append(s.sendFree, int32(i))
	}
	// A ring's table can be as large as our descriptor limit, so take all of it.
	s.slots = raiseFileLimit()
	// The margin, the loop order below, the tag layout and the listener and ring counts follow
	// go-iouring-davidefrageri (#354) closely: same structure, our names.
	s.margin = min(max(s.slots/8, 64), 1024, s.slots/4)
	for i := range s.lfds {
		s.lfds[i] = listen(host, port)
	}
	must(s.addRing())
	s.armAccepts(0)
	blockKB, hugeKB := dbPageMemory()
	log.Printf("listening on %s:%s, %d slots per ring, direct B-tree reads: %v, page cache memory: %d MB of which %d MB on 2 MB pages", host, port, s.slots, dbReadsDirect(), blockKB>>10, hugeKB>>10)

	lastStats := time.Now()
	for {
		// Wait on ring 0 FIRST, then visit the others: what arrived on them during the wait is
		// then handled in this pass rather than the next one. With requests parked there is
		// work to do already, so collect what is ready and do not sleep.
		before := time.Now()
		if s.waiting() {
			s.rings[0].u.poll()
		} else {
			s.rings[0].u.wait(uint32(max(waitFor/len(s.rings), 32)), waitWindow)
		}
		for _, r := range s.rings[1:] {
			r.u.poll()
		}
		now := time.Now()
		entered, reqBefore := now.Sub(before), s.requests
		s.nowNano, s.budget = now.UnixNano(), answerCap
		s.checkHold(now)
		// The write window: after a pass that committed writes, writes wait for writeWindow
		// before the next one, so one commit carries many of them (see writeWindow).
		s.writesWait = s.holding || now.Before(s.nextWrites)
		s.retrySends()
		s.answerOldest()
		completions := 0
		for ri, r := range s.rings {
			s.warm(r)
			completions += r.u.drain(func(c *cqe) { s.complete(ri, r, c) })
		}
		if entered > s.enterLongest { // the system calls that collected this pass's completions
			s.enterLongest, s.enterCount = entered, completions
		}
		s.answerParked()
		s.endPass()
		if s.wrote {
			s.wrote, s.nextWrites = false, now.Add(writeWindow)
		}
		for _, r := range s.rings[1:] {
			r.u.submit()
		}
		for ri, r := range s.rings {
			s.rearmStarved(ri, r)
		}
		if d := time.Since(now); d > s.passLongest {
			s.passLongest, s.passCount = d, s.requests-reqBefore
		}
		if now.Sub(lastStats) >= 10*time.Second {
			s.logStats(now.Sub(lastStats))
			lastStats = now
		}
	}
}

// checkHold starts a write hold when the WAL is large and ends it when the checkpointer has
// copied all of it (or holdMax has passed). Ending it re-runs the requests that waited.
func (s *server) checkHold(now time.Time) {
	if !s.holding {
		if now.After(s.holdNotTill) && dbHoldWanted() {
			s.holding, s.holdStart, s.holdBase = true, now, dbBackfills()
			s.holds++
			dbUrgent(true)
		}
		return
	}
	done, late := dbBackfills() > s.holdBase, now.Sub(s.holdStart) >= holdMax
	if !done && !late {
		return
	}
	s.holding = false
	dbUrgent(false)
	s.holdLongest = max(s.holdLongest, now.Sub(s.holdStart))
	s.holdNotTill = now.Add(holdGap)
	if !done {
		s.holdTimeouts++
		s.holdNotTill = now.Add(holdMax)
	}
}

func (s *server) addRing() error {
	u, err := newRing(4096, 65536)
	if err != nil {
		return err
	}
	if err := u.registerFileTable(uint32(s.slots)); err != nil {
		return err
	}
	pool, err := u.newBufPool(0, recvBufCount, recvBufSize)
	if err != nil {
		return err
	}
	s.rings = append(s.rings, &ringState{u: u, recv: pool, conns: make([]conn, s.slots)})
	return nil
}

// txn opens the pass's transaction if this is the first request of the pass to need the
// database. Reads use it too: one transaction for 30 reads is cheaper than 30 separate ones.
func (s *server) txn() bool {
	if !s.inTxn {
		s.inTxn = dbBegin()
	}
	return s.inTxn
}

// endPass commits, then releases the pass's responses. If the commit fails, no client may be
// told its write succeeded, so every connection answered in this pass is dropped instead. A
// read in the pass may have seen a write that was then rolled back; it is dropped with it.
func (s *server) endPass() {
	s.passes++
	ok := true
	if s.inTxn {
		ok = dbCommit()
		s.inTxn = false
		s.commits++
		if !ok {
			log.Printf("commit failed, dropping %d responses: %v", len(s.pending), dbErr())
		}
	}
	for _, p := range s.pending {
		if s.rings[p.ri].conns[p.slot].closing { // the client left during this pass
			s.freeSend(p.ri, p.slot, p.buf)
		} else if ok {
			s.queueSend(p.ri, p.slot, p.buf)
		} else {
			s.freeSend(p.ri, p.slot, p.buf)
			s.closeConn(p.ri, p.slot)
		}
	}
	s.pending = s.pending[:0]
}

func (s *server) logStats(over time.Duration) {
	wal, walMax, ckpts := dbStats()
	perPass, open := 0.0, 0
	if s.passes > 0 {
		perPass = float64(s.requests) / float64(s.passes)
	}
	for _, r := range s.rings {
		open += r.open
	}
	log.Printf("stats conns=%d rings=%d req/s=%.0f req/pass=%.2f commits/s=%.0f wal_pages=%d wal_max=%d checkpoints=%d holds=%d hold_timeouts=%d hold_longest=%s nobufs=%d sendwait=%d enter_longest=%s/%d pass_longest=%s/%d parked_max=%d parked_wait_max=%s",
		open, len(s.rings), float64(s.requests)/over.Seconds(), perPass, float64(s.commits)/over.Seconds(), wal, walMax, ckpts,
		s.holds, s.holdTimeouts, s.holdLongest.Round(time.Millisecond), s.noBufs, s.sendWaits, s.enterLongest.Round(100*time.Microsecond), s.enterCount, s.passLongest.Round(100*time.Microsecond), s.passCount, s.parkedMax, s.waitMax.Round(time.Millisecond))
	s.passes, s.requests, s.commits, s.noBufs, s.sendWaits, s.passLongest = 0, 0, 0, 0, 0, 0
	s.parkedMax, s.waitMax, s.enterLongest, s.enterCount, s.passCount = 0, 0, 0, 0, 0
	s.holds, s.holdTimeouts, s.holdLongest = 0, 0, 0
}

// complete handles one result from the kernel.
// warm reads the record of every connection that has a completion waiting, before any of them
// is handled. The records of 200,000 connections are megabytes, a completion's connection is
// anywhere in them, and so the first look at each is a wait for memory: about 520 cycles per
// request when complete() met them one at a time (measured at 250,000 users on the scoring
// hardware). Nothing here depends on the result of an earlier read, so the processor has
// several of those waits under way at once; with this loop in front they cost about half.
func (s *server) warm(r *ringState) {
	sum := uint32(0)
	r.u.peek(func(c *cqe) {
		kind, n := int(c.userData>>56), int(uint32(c.userData))
		if kind == kindSend { // a send is tagged with its buffer, which knows its connection
			if n >= len(s.sendSlot) {
				return
			}
			n = int(s.sendSlot[n])
		} else if kind != kindRecv && kind != kindClose {
			return
		}
		if n < len(r.conns) {
			sum += uint32(r.conns[n].gen)
		}
	})
	s.warmed += sum // kept, so that the reads are not removed as having no effect
}

func (s *server) complete(ri int, r *ringState, c *cqe) {
	kind, gen, n := int(c.userData>>56), uint16(c.userData>>32), int(uint32(c.userData))
	switch kind {
	case kindAccept:
		li := n
		switch {
		case c.res >= 0:
			slot := int(c.res)
			cn := &r.conns[slot]
			*cn = conn{gen: cn.gen + 1}
			r.open++
			s.armRecv(ri, slot)
			if r.open+s.margin >= s.slots {
				s.ringFull(ri)
			}
		case c.res == -int32(syscall.ENFILE), c.res == -int32(syscall.ENOSPC):
			s.ringFull(ri) // the table had no free slot
		}
		if c.flags&cqeFMore == 0 { // the kernel stopped this multishot accept
			r.armed[li] = false
			if ri == s.accepting && !r.full {
				s.armAccept(ri, li)
			}
		}

	case kindRecv:
		slot := n
		cn := &r.conns[slot]
		hasBuf := c.flags&cqeFBuffer != 0
		id := uint16(c.flags >> 16)
		if cn.gen != gen || cn.closing { // a leftover from a connection that is gone
			if hasBuf {
				r.recv.give(id)
			}
			return
		}
		if c.res <= 0 {
			// -ENOBUFS: this ring's buffer pool was momentarily empty; the connection is fine.
			// Anything else (0 = client closed, or an error) ends the connection.
			if c.res == -int32(syscall.ENOBUFS) {
				s.noBufs++
				r.starved = append(r.starved, parkedConn{ri, slot, cn.gen})
				return
			}
			s.closeConn(ri, slot)
			return
		}
		s.onData(ri, slot, r.recv.data(id, int(c.res)))
		r.recv.give(id) // parsed (or copied aside): the kernel can reuse this buffer now
		if c.flags&cqeFMore == 0 && !cn.closing {
			s.armRecv(ri, slot)
		}

	case kindSend:
		buf := n
		slot := int(s.sendSlot[buf])
		cn := &r.conns[slot]
		live := cn.gen == gen && !cn.closing && cn.out == int32(buf)+1
		if live && c.res > 0 && s.sendOff[buf]+c.res < s.sendLen[buf] {
			s.sendOff[buf] += c.res // the socket took only part: send the rest
			s.queueSend(ri, slot, buf)
			return
		}
		s.sendFree = append(s.sendFree, int32(buf))
		if !live {
			return
		}
		cn.out = 0
		if c.res < 0 {
			s.closeConn(ri, slot)
		} else if cn.held {
			s.onData(ri, slot, nil) // a request was waiting for this response to finish
		}

	case kindClose:
		slot := n
		cn := &r.conns[slot]
		if cn.gen != gen {
			return
		}
		delete(s.partial, key(ri, slot))
		cn.closing, cn.held, cn.queued, cn.waiting, cn.out = false, false, false, false, 0
		cn.gen++ // anything still in flight for the old connection is now stale
		r.open--
		if r.full && r.open+2*s.margin < s.slots {
			r.full = false
			if s.rings[s.accepting].full { // every ring was full: this one can accept again
				s.armAccepts(ri)
			}
		}
	}
}

func (s *server) armAccepts(ri int) {
	s.accepting = ri
	for li := range s.lfds {
		if !s.rings[ri].armed[li] {
			s.armAccept(ri, li)
		}
	}
}

// armAccept asks for connections from one listener, to be placed straight into this ring's
// file table. The completion's result is the slot the kernel chose.
func (s *server) armAccept(ri, li int) {
	r := s.rings[ri]
	e := r.u.next()
	e.opcode, e.fd, e.ioprio, e.fileIndex = opAccept, int32(s.lfds[li]), acceptMultishot, -1 // -1 = fileIndexAlloc
	e.userData = tag(kindAccept, ri, 0, li)
	r.armed[li] = true
}

// ringFull moves accepting to another ring. The full ring's accepts are cancelled: left
// armed, each would take one more connection off the queue, find no slot for it, and the
// client would see a reset.
func (s *server) ringFull(ri int) {
	r := s.rings[ri]
	if r.full {
		return
	}
	r.full = true
	for li := range r.armed {
		if r.armed[li] {
			e := r.u.next()
			e.opcode, e.fd, e.addr, e.userData = opAsyncCancel, -1, tag(kindAccept, ri, 0, li), tag(kindIgnore, ri, 0, 0)
		}
	}
	for i, other := range s.rings {
		if !other.full {
			s.armAccepts(i)
			return
		}
	}
	if len(s.rings) == maxRings {
		log.Printf("all %d rings are full: not accepting", maxRings)
		return
	}
	if err := s.addRing(); err != nil {
		log.Printf("cannot add a ring: %v", err)
		return
	}
	s.armAccepts(len(s.rings) - 1)
	log.Printf("ring %d is full (%d connections), added ring %d", ri, r.open, len(s.rings)-1)
}

// rearmStarved restarts receives that stopped for want of a buffer, oldest first.
func (s *server) rearmStarved(ri int, r *ringState) {
	for n := 0; r.starvedHead < len(r.starved) && n < recvBufCount; r.starvedHead++ {
		p := r.starved[r.starvedHead]
		if cn := &r.conns[p.slot]; cn.gen == p.gen && !cn.closing {
			s.armRecv(ri, p.slot)
			n++
		}
	}
	if r.starvedHead == len(r.starved) {
		r.starved, r.starvedHead = r.starved[:0], 0
	}
}

// retrySends re-runs requests that were held because every send buffer was in use.
func (s *server) retrySends() {
	if len(s.sendWait) == 0 || len(s.sendFree) == 0 {
		return
	}
	waiting := s.sendWait
	s.sendWait = nil
	for i, p := range waiting {
		if len(s.sendFree) == 0 {
			s.sendWait = append(s.sendWait, waiting[i:]...)
			return
		}
		cn := &s.rings[p.ri].conns[p.slot]
		if cn.gen == p.gen && !cn.closing && cn.waiting {
			cn.waiting = false
			s.onData(p.ri, p.slot, nil)
		}
	}
}

func (s *server) armRecv(ri, slot int) {
	r := s.rings[ri]
	e := r.u.next()
	e.opcode, e.fd, e.ioprio, e.flags, e.bufGroup = opRecv, int32(slot), recvMultishot, sqeFixedFile|sqeBufferSelect, r.recv.group
	e.userData = tag(kindRecv, ri, r.conns[slot].gen, slot)
}

// closeConn ends a connection: shut the socket down, which also ends its armed receive,
// then free its slot. The two are linked so they run in that order. The slot is only
// reused after the close has completed (see kindClose).
func (s *server) closeConn(ri, slot int) {
	r := s.rings[ri]
	cn := &r.conns[slot]
	if cn.closing {
		return
	}
	cn.closing = true
	r.u.reserve(2)
	e := r.u.next()
	e.opcode, e.fd, e.flags, e.len = opShutdown, int32(slot), sqeFixedFile|sqeIOHardlink, syscall.SHUT_RDWR
	e.userData = tag(kindIgnore, ri, 0, 0)
	e = r.u.next()
	e.opcode, e.fileIndex = opClose, int32(slot)+1 // closing a direct descriptor: slot + 1, and no fd
	e.userData = tag(kindClose, ri, cn.gen, slot)
}

func (s *server) queueSend(ri, slot, buf int) {
	r := s.rings[ri]
	off := buf*sendBufSize + int(s.sendOff[buf])
	e := r.u.next()
	e.opcode, e.fd, e.flags = opSend, int32(slot), sqeFixedFile
	e.addr = uint64(uintptr(unsafe.Pointer(&s.sendMem[off])))
	e.len = uint32(s.sendLen[buf] - s.sendOff[buf])
	e.opFlags = syscall.MSG_NOSIGNAL
	e.userData = tag(kindSend, ri, r.conns[slot].gen, buf)
}

func (s *server) freeSend(ri, slot, buf int) {
	s.sendFree = append(s.sendFree, int32(buf))
	s.rings[ri].conns[slot].out = 0
}

var headerEnd = []byte("\r\n\r\n")

// onData parses every complete request in data and prepares one send with all the responses.
func (s *server) onData(ri, slot int, data []byte) {
	cn := &s.rings[ri].conns[slot]
	k := key(ri, slot)
	if cn.held {
		data = append(s.partial[k], data...)
		delete(s.partial, k)
		cn.held = false
	}
	hold := func(rest []byte) {
		if len(rest) > 0 {
			s.partial[k] = append([]byte(nil), rest...) // copy: the kernel's buffer is about to be reused
			cn.held = true
		}
	}
	if cn.out != 0 || cn.queued || cn.waiting { // a response is still being sent, or the request is queued
		hold(data)
		return
	}
	if len(s.sendFree) == 0 {
		// Every send buffer is in use. Keep the request and come back when one is free.
		hold(data)
		if cn.held {
			s.sendWaits++
			cn.waiting = true
			s.sendWait = append(s.sendWait, parkedConn{ri, slot, cn.gen})
		}
		return
	}
	buf := int(s.sendFree[len(s.sendFree)-1])
	out := s.sendMem[buf*sendBufSize : buf*sendBufSize : (buf+1)*sendBufSize]

	for len(data) > 0 {
		end := bytes.Index(data, headerEnd)
		if end < 0 {
			break // headers incomplete: wait for the rest
		}
		head := data[:end]
		bodyLen := contentLength(head)
		if len(data) < end+4+bodyLen {
			break // body incomplete
		}
		if sendBufSize-len(out) < sendBufSize/2 {
			break // no room for another worst-case response: the rest waits for this send
		}
		req := s.parse(head, data[end+4:end+4+bodyLen])
		if s.budget == 0 || (s.writesWait && req.isWrite()) {
			if len(out) > 0 {
				// A response for this connection is already being built in this pass, and a
				// connection has one response in flight at a time. Leave the request in the
				// bytes: it is parsed again when that send has finished. (Pipelining only.)
				s.discard(&req)
				break
			}
			data = data[end+4+bodyLen:]
			s.park(ri, slot, cn, req)
			break
		}
		data = data[end+4+bodyLen:]
		s.budget--
		s.wrote = s.wrote || req.isWrite()
		out = s.execute(out, &req)
		s.requests++
	}
	hold(data)
	if len(out) == 0 {
		return
	}
	s.respondWith(ri, slot, cn, buf, len(out))
}

// respondWith takes send buffer buf, which holds n bytes of response, off the free list and
// queues it for the end of the pass.
func (s *server) respondWith(ri, slot int, cn *conn, buf, n int) {
	s.sendFree = s.sendFree[:len(s.sendFree)-1]
	s.sendLen[buf], s.sendOff[buf] = int32(n), 0
	s.sendSlot[buf] = int32(slot)
	cn.out = int32(buf) + 1
	s.pending = append(s.pending, pendingSend{ri, slot, buf}) // sent by endPass, after the commit
}

// park puts a parsed request aside for a later pass. A connection has at most one parked
// request; whatever it sends after that waits as bytes, as before.
func (s *server) park(ri, slot int, cn *conn, r request) {
	r.ri, r.slot, r.gen, r.arrival = uint8(ri), uint32(slot), cn.gen, s.nowNano
	q := &s.backlog
	if s.writesWait && r.isWrite() {
		q = &s.heldWrites
	}
	q.pushBack(r)
	cn.queued = true
	s.parkedMax = max(s.parkedMax, s.backlog.n+s.heldWrites.n)
}

// answer executes a parked request. It reports false if the connection has gone.
func (s *server) answer(r *request) bool {
	ri, slot := int(r.ri), int(r.slot)
	cn := &s.rings[ri].conns[slot]
	if cn.gen != r.gen || cn.closing {
		s.discard(r)
		return false
	}
	cn.queued = false
	s.waitMax = max(s.waitMax, time.Duration(s.nowNano-r.arrival))
	s.wrote = s.wrote || r.isWrite()
	buf := int(s.sendFree[len(s.sendFree)-1])
	out := s.execute(s.sendMem[buf*sendBufSize:buf*sendBufSize:(buf+1)*sendBufSize], r)
	s.respondWith(ri, slot, cn, buf, len(out))
	s.budget--
	s.requests++
	return true
}

// answerOldest gives requests that have waited longer than oldestMax up to half of the pass,
// before anything else is answered. It runs at the start of a pass.
func (s *server) answerOldest() {
	for n := 0; n < answerCap/2 && s.backlog.n > 0 && len(s.sendFree) > 0; n++ {
		if time.Duration(s.nowNano-s.backlog.front().arrival) < oldestMax {
			return
		}
		r := s.backlog.popFront()
		if s.writesWait && r.isWrite() {
			s.heldWrites.pushBack(r)
			continue
		}
		s.answer(&r)
	}
}

// answerParked spends what is left of the pass's budget after its own arrivals: first on
// writes that waited out a hold, oldest first, then on the backlog, newest first.
func (s *server) answerParked() {
	for s.budget > 0 && len(s.sendFree) > 0 {
		var r request
		switch {
		case !s.writesWait && s.heldWrites.n > 0:
			r = s.heldWrites.popFront()
		case s.backlog.n > 0:
			r = s.backlog.popBack()
			if s.writesWait && r.isWrite() {
				s.heldWrites.pushBack(r)
				continue
			}
		default:
			return
		}
		s.answer(&r)
	}
}

// waiting reports whether answerable requests are parked, in which case the loop must not
// sleep in the kernel waiting for more: the budget would become a limit on throughput.
func (s *server) waiting() bool {
	return s.backlog.n > 0 || (!s.writesWait && s.heldWrites.n > 0)
}

// deque is a ring of requests in memory outside the Go heap: nothing to allocate or collect
// while the loop is behind.
type deque struct {
	buf     []request
	head, n int
}

const dequeCap = 1 << 20 // more than the connections 16 rings can hold; 32 MB of address space, used as touched

func newDeque() deque {
	mem := mmapAnon(dequeCap * int(unsafe.Sizeof(request{})))
	return deque{buf: unsafe.Slice((*request)(unsafe.Pointer(&mem[0])), dequeCap)}
}

func (q *deque) pushBack(r request) {
	if q.n == dequeCap {
		panic("request backlog is full")
	}
	q.buf[(q.head+q.n)&(dequeCap-1)] = r
	q.n++
}

func (q *deque) front() *request { return &q.buf[q.head] }

func (q *deque) popFront() request {
	r := q.buf[q.head]
	q.head = (q.head + 1) & (dequeCap - 1)
	q.n--
	return r
}

func (q *deque) popBack() request {
	q.n--
	return q.buf[(q.head+q.n)&(dequeCap-1)]
}

func listen(host, port string) int {
	ip := net.ParseIP(host).To4()
	p, err := strconv.Atoi(port)
	if ip == nil || err != nil {
		log.Fatalf("bad HOST/PORT %q %q", host, port)
	}
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
	must(err)
	must(syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1))
	must(syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, 15 /* SO_REUSEPORT */, 1))
	// Accepted sockets inherit this: send each response at once instead of waiting to batch it.
	must(syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1))
	must(syscall.Bind(fd, &syscall.SockaddrInet4{Port: p, Addr: [4]byte(ip)}))
	must(syscall.Listen(fd, 65535))
	return fd
}

// raiseFileLimit lifts the soft descriptor limit to the hard one and returns it.
func raiseFileLimit() int {
	var l syscall.Rlimit
	must(syscall.Getrlimit(syscall.RLIMIT_NOFILE, &l))
	l.Cur = l.Max
	must(syscall.Setrlimit(syscall.RLIMIT_NOFILE, &l))
	return int(min(l.Cur, 1<<20))
}

func must(err error) {
	if err != nil {
		log.Fatal(err)
	}
}

func mustEnv(k string) string {
	v := os.Getenv(k)
	if v == "" {
		log.Fatalf("%s is not set", k)
	}
	return v
}
