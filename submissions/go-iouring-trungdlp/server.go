package main

// The event loop: one OS thread drives every ring. Each pass submits what the last pass queued,
// waits briefly for completions, answers every request it read inside one SQLite transaction,
// commits, and only then hands the responses to the kernel (rule 6: a 201 means committed).

import (
	"fmt"
	"os"
	"syscall"
	"time"
	"unsafe"
)

const (
	udAccept = iota + 1
	udRecv
	udRead
	udSend
	udShutdown
	udClose
)

const (
	listeners   = 8      // SO_REUSEPORT listeners, each with its own accept queue
	acceptDepth = 512    // single-shot accepts in flight on the accepting ring
	sqEntries   = 4096
	cqEntries   = 65536
	bufSize     = 2048   // receive buffer; a longer request is read in more than one piece
	bufCount    = 1024   // receive buffers per ring
	maxRings    = 16
	waitMin     = 256    // a pass waits for this many completions on ring 0 ...
	waitNs      = 1000000 // ... or this long, whichever comes first
	readsPerPass   = 16384
	answersPerPass = 2048
	outCap      = 8192
	poolMax     = 4096
)

func ud(op, ring int, gen uint16, slot uint32) uint64 {
	return uint64(op)<<56 | uint64(ring)<<48 | uint64(gen)<<32 | uint64(slot)
}

// Connection flags.
const (
	fOpen       = 1 << iota
	fCloseAfter // close once its output drains
	fHeld       // its next request is a write waiting out a checkpoint hold
	fEOF        // the client finished sending while answers were still owed
	fClosing    // shutdown/close submitted
	fSending    // a send is in flight
	fQueued     // on the read queue
	fRecving    // a single-shot read is in flight
	fReady      // on the ready list: bytes read wait in partial for a later pass
	fInBatch    // touched in this pass
	fAnswered   // produced a response in this pass
)

// conn is one direct-descriptor slot. It holds no Go pointers: buffers are pool indexes
// (0 = none), so the garbage collector never scans the table.
type conn struct {
	gen   uint16
	flags uint16
	out   int32 // responses not yet handed to the kernel
	wire  int32 // the send in flight
	part  int32 // bytes read but not yet answered
	sent  uint32
	mark  uint32 // len(out) when the pass first touched it
}

type ring struct {
	u        *uring
	bufs     *bufRing
	conns    []conn
	accepting bool
	full     bool
	accepts  [listeners]uint32
	depth    uint32
	open     uint32
	margin   uint32
	reads    uint32 // single-shot reads in flight (each holds at most one receive buffer)
}

// ref names a connection in a queue: ring, slot, generation.
type ref uint64

func mkref(ri int, slot uint32, gen uint16) ref { return ref(uint64(ri)<<48 | uint64(gen)<<32 | uint64(slot)) }
func (r ref) ring() int    { return int(r >> 48) }
func (r ref) slot() uint32 { return uint32(r) }
func (r ref) gen() uint16  { return uint16(r >> 32) }

// fifo is a queue of refs.
type fifo struct {
	q    []ref
	head int
}

func (f *fifo) push(r ref) { f.q = append(f.q, r) }
func (f *fifo) len() int   { return len(f.q) - f.head }
func (f *fifo) pop() ref {
	r := f.q[f.head]
	f.head++
	if f.head == len(f.q) {
		f.q = f.q[:0]
		f.head = 0
	} else if f.head > 4096 && f.head*2 > len(f.q) {
		n := copy(f.q, f.q[f.head:])
		f.q = f.q[:n]
		f.head = 0
	}
	return r
}

type server struct {
	rings   []*ring
	app     *app
	listen  []int
	slots   uint32
	pool    [][]byte
	free    []int32
	touched []ref
	held    []ref
	rq      fifo // connections to read, first in first out
	ready   fifo // connections with bytes read but not answered, oldest first
	answers int
	req     request
	needRing  bool
	spillAfter time.Time
}

// ---- buffers ------------------------------------------------------------------------------------

func (s *server) take() int32 {
	if n := len(s.free); n > 0 {
		i := s.free[n-1]
		s.free = s.free[:n-1]
		return i
	}
	s.pool = append(s.pool, make([]byte, 0, outCap))
	return int32(len(s.pool) - 1)
}

func (s *server) give(i int32) {
	if i == 0 {
		return
	}
	b := s.pool[i-1]
	if cap(b) > 4*outCap {
		s.pool[i-1] = make([]byte, 0, outCap)
	} else {
		s.pool[i-1] = b[:0]
	}
	s.free = append(s.free, i-1)
}

func (s *server) buf(i int32) []byte { return s.pool[i-1] }

// ---- rings --------------------------------------------------------------------------------------

func newRing(slots uint32) (*ring, error) {
	u, err := newUring(sqEntries, cqEntries)
	if err != nil {
		return nil, fmt.Errorf("io_uring_setup: %w", err)
	}
	if err := u.registerSparse(slots); err != nil {
		return nil, fmt.Errorf("register files: %w", err)
	}
	b, err := newBufRing(u, bufCount, bufSize)
	if err != nil {
		return nil, fmt.Errorf("register buffer ring: %w", err)
	}
	depth := min(max(slots/16, listeners), acceptDepth) / listeners * listeners
	margin := min(max(slots/8, depth+1), 2*acceptDepth, slots/4)
	return &ring{u: u, bufs: b, conns: make([]conn, slots), depth: depth, margin: margin}, nil
}

func (s *server) fillAccepts(ri int) {
	r := s.rings[ri]
	for li, fd := range s.listen {
		for r.accepts[li] < r.depth/listeners {
			r.u.accept(fd, ud(udAccept, ri, 0, uint32(li)))
			r.accepts[li]++
		}
	}
}

// spill moves accepting to a ring with room, adding one if needed.
func (s *server) spill() bool {
	for _, r := range s.rings {
		if r.accepting {
			return true
		}
	}
	for ri, r := range s.rings {
		if !r.full {
			r.accepting = true
			s.fillAccepts(ri)
			return true
		}
	}
	if len(s.rings) >= maxRings {
		fmt.Fprintf(os.Stderr, "connection limit reached (%d rings)\n", len(s.rings))
		return false
	}
	r, err := newRing(s.slots)
	if err != nil {
		fmt.Fprintf(os.Stderr, "cannot add ring: %v\n", err)
		return false
	}
	s.rings = append(s.rings, r)
	r.accepting = true
	s.fillAccepts(len(s.rings) - 1)
	fmt.Fprintf(os.Stderr, "ring %d added (%d slots each)\n", len(s.rings)-1, s.slots)
	return true
}

// ---- per-connection helpers ---------------------------------------------------------------------

func (s *server) mark(ri int, slot uint32, c *conn) {
	if c.flags&fInBatch == 0 {
		c.flags |= fInBatch
		c.mark = 0
		if c.out != 0 {
			c.mark = uint32(len(s.buf(c.out)))
		}
		s.touched = append(s.touched, mkref(ri, slot, c.gen))
	}
}

func (s *server) queueRead(ri int, slot uint32, c *conn) {
	if c.flags&fQueued == 0 {
		c.flags |= fQueued
		s.rq.push(mkref(ri, slot, c.gen))
	}
}

// finish: the client is done sending. Owed answers go out first, then it closes.
func (s *server) finish(ri int, slot uint32, c *conn) {
	if c.flags&(fHeld|fReady) != 0 {
		c.flags |= fEOF
	} else {
		c.flags |= fCloseAfter
		s.mark(ri, slot, c)
	}
}

// stash keeps bytes on the connection behind any it already has.
func (s *server) stash(c *conn, data []byte) {
	if c.part == 0 {
		c.part = s.take() + 1
	}
	s.pool[c.part-1] = append(s.pool[c.part-1], data...)
}

// received: answered now if this pass has answers left and nothing older waits, else kept.
func (s *server) received(ri int, slot uint32, c *conn, data []byte) {
	if c.flags&fCloseAfter != 0 {
		return
	}
	if c.flags&(fHeld|fReady) != 0 || s.answers == 0 {
		s.stash(c, data)
		if c.flags&(fHeld|fReady) == 0 {
			c.flags |= fReady
			s.ready.push(mkref(ri, slot, c.gen))
		}
		return
	}
	s.answers--
	s.serve(ri, slot, c, data)
}

// serve answers every complete request in the connection's kept bytes plus data. During a
// checkpoint hold a write stops it: that request and the rest stay on the connection.
func (s *server) serve(ri int, slot uint32, c *conn, data []byte) {
	if c.flags&fCloseAfter != 0 {
		return
	}
	buf := data
	if c.part != 0 {
		s.pool[c.part-1] = append(s.pool[c.part-1], data...)
		buf = s.pool[c.part-1]
	}
	off := 0
	for off < len(buf) {
		used, st := parse(buf[off:], &s.req)
		if st == parsedPartial {
			break
		}
		if st == parsedBad {
			s.mark(ri, slot, c)
			c.flags |= fAnswered | fCloseAfter
			if c.out == 0 {
				c.out = s.take() + 1
			}
			s.pool[c.out-1] = errorResp(s.pool[c.out-1], 400, "bad request")
			off = len(buf)
			break
		}
		if s.app.held && string(s.req.method) == "POST" {
			if c.flags&fHeld == 0 {
				c.flags |= fHeld
				s.held = append(s.held, mkref(ri, slot, c.gen))
			}
			break
		}
		s.mark(ri, slot, c)
		c.flags |= fAnswered
		if c.out == 0 {
			c.out = s.take() + 1
		}
		s.pool[c.out-1] = s.app.handle(&s.req, s.pool[c.out-1])
		off += used
	}
	rest := buf[off:]
	if c.part != 0 {
		p := s.pool[c.part-1]
		if len(rest) == 0 {
			s.give(c.part)
			c.part = 0
		} else if off > 0 {
			n := copy(p, rest)
			s.pool[c.part-1] = p[:n]
		}
	} else if len(rest) > 0 {
		c.part = s.take() + 1
		s.pool[c.part-1] = append(s.pool[c.part-1], rest...)
	}
}

// ---- completions --------------------------------------------------------------------------------

func (s *server) drain(ri int) int {
	r := s.rings[ri]
	u := r.u
	head := *u.cqHead
	n := 0
	for {
		tail := atomicLoad(u.cqTail)
		if head == tail {
			break
		}
		for ; head != tail; head++ {
			e := u.cqes[head&u.cqMask]
			n++
			s.complete(ri, r, e.userData, e.res, e.flags)
		}
		atomicStore(u.cqHead, head)
	}
	r.bufs.publish()
	if r.accepting {
		if r.full {
			r.accepting = false
			s.needRing = true
		} else {
			s.fillAccepts(ri)
		}
	}
	return n
}

func (s *server) complete(ri int, r *ring, u uint64, res int32, flags uint32) {
	op := int(u >> 56)
	gen := uint16(u >> 32)
	slot := uint32(u)
	switch op {
	case udAccept:
		r.accepts[slot]--
		if res >= 0 {
			c := &r.conns[res]
			g := c.gen + 1
			*c = conn{gen: g, flags: fOpen}
			r.u.recvMulti(uint32(res), ud(udRecv, ri, g, uint32(res)))
			r.open++
			if r.open+r.margin >= uint32(len(r.conns)) {
				r.full = true
			}
		} else if res == -int32(syscall.ENFILE) {
			r.full = true
		}
	case udRecv:
		c := &r.conns[slot]
		hasBuf := flags&cqeFBuffer != 0
		bid := uint16(flags >> 16)
		if c.gen != gen || c.flags&fOpen == 0 || c.flags&fClosing != 0 {
			if hasBuf {
				r.bufs.add(bid)
			}
			return
		}
		switch {
		case res > 0:
			if hasBuf {
				s.received(ri, slot, c, r.bufs.get(bid, int(res)))
				r.bufs.add(bid)
			}
			if flags&cqeFMore == 0 {
				// It ended (the buffers ran out behind it): read the rest in bounded single-shot
				// reads, which re-arm it once the socket is drained.
				s.queueRead(ri, slot, c)
			}
		case res == -int32(syscall.ENOBUFS):
			// A burst emptied the buffer ring. Not re-armed now: re-arming every receive at once
			// would only repeat the storm.
			s.queueRead(ri, slot, c)
		default:
			if hasBuf {
				r.bufs.add(bid)
			}
			s.finish(ri, slot, c)
		}
	case udRead:
		r.reads--
		c := &r.conns[slot]
		hasBuf := flags&cqeFBuffer != 0
		bid := uint16(flags >> 16)
		if c.gen != gen || c.flags&fOpen == 0 || c.flags&fClosing != 0 {
			if hasBuf {
				r.bufs.add(bid)
			}
			return
		}
		c.flags &^= fRecving
		switch {
		case res > 0:
			if !hasBuf {
				s.queueRead(ri, slot, c)
				return
			}
			s.received(ri, slot, c, r.bufs.get(bid, int(res)))
			r.bufs.add(bid)
			if int(res) == bufSize {
				s.queueRead(ri, slot, c) // the buffer filled: there may be more
			} else {
				r.u.recvMulti(slot, ud(udRecv, ri, gen, slot))
			}
		case res == -int32(syscall.EAGAIN):
			r.u.recvMulti(slot, ud(udRecv, ri, gen, slot))
		case res == -int32(syscall.ENOBUFS):
			s.queueRead(ri, slot, c)
		default:
			if hasBuf {
				r.bufs.add(bid)
			}
			s.finish(ri, slot, c)
		}
	case udSend:
		c := &r.conns[slot]
		if c.gen != gen || c.flags&fSending == 0 {
			return
		}
		if res < 0 {
			c.flags &^= fSending
			s.give(c.wire)
			c.wire = 0
			s.give(c.out)
			c.out = 0
			c.flags |= fCloseAfter
			s.mark(ri, slot, c)
			return
		}
		c.sent += uint32(res)
		w := s.buf(c.wire)
		if int(c.sent) < len(w) {
			r.u.send(slot, unsafe.Pointer(&w[c.sent]), len(w)-int(c.sent), ud(udSend, ri, c.gen, slot))
			return
		}
		c.flags &^= fSending
		s.give(c.wire)
		c.wire = 0
		if (c.out != 0 && len(s.buf(c.out)) > 0) || c.flags&fCloseAfter != 0 {
			s.mark(ri, slot, c) // sent after this pass commits, with its other responses
		}
	case udClose:
		c := &r.conns[slot]
		if c.gen == gen && c.flags&fOpen != 0 {
			s.give(c.out)
			s.give(c.wire)
			s.give(c.part)
			*c = conn{gen: c.gen}
			r.open--
		}
		if r.full && r.open+2*r.margin < uint32(len(r.conns)) {
			r.full = false
			r.accepting = true
		}
	}
}

// ---- the end of a pass ----------------------------------------------------------------------------

// endPass commits the pass's transaction, then hands every response it produced to the kernel.
func (s *server) endPass() {
	committed := dbCommit()
	s.app.wrote = false
	dbTick() // no transaction is open: the only point where a checkpoint hold starts or ends
	s.app.held = writesHeld()
	for _, t := range s.touched {
		r := s.rings[t.ring()]
		slot := t.slot()
		c := &r.conns[slot]
		if !committed && c.flags&fAnswered != 0 {
			// No response from a rolled-back pass may claim success.
			o := s.pool[c.out-1][:c.mark]
			s.pool[c.out-1] = errorResp(o, 500, "internal server error")
			c.flags |= fCloseAfter
		}
		c.flags &^= fInBatch | fAnswered
		if c.flags&fOpen == 0 || c.flags&(fClosing|fSending) != 0 {
			continue
		}
		if c.out != 0 && len(s.buf(c.out)) > 0 {
			c.wire, c.out = c.out, 0
			c.sent = 0
			c.flags |= fSending
			w := s.buf(c.wire)
			r.u.send(slot, unsafe.Pointer(&w[0]), len(w), ud(udSend, t.ring(), c.gen, slot))
		} else if c.flags&fCloseAfter != 0 {
			c.flags |= fClosing
			r.u.shutdownClose(slot, ud(udShutdown, t.ring(), c.gen, slot), ud(udClose, t.ring(), c.gen, slot))
		}
	}
	s.touched = s.touched[:0]
	if s.needRing && time.Now().After(s.spillAfter) {
		s.needRing = !s.spill()
		if s.needRing {
			s.spillAfter = time.Now().Add(time.Second)
		}
	}
}

// releaseHeld: the hold is over; answer the writes it kept, from the bytes on each connection.
func (s *server) releaseHeld() {
	list := s.held
	s.held = nil
	for _, t := range list {
		c := &s.rings[t.ring()].conns[t.slot()]
		if c.gen != t.gen() || c.flags&fOpen == 0 || c.flags&fClosing != 0 {
			continue
		}
		c.flags &^= fHeld
		s.serve(t.ring(), t.slot(), c, nil)
		if c.flags&fEOF != 0 && c.flags&fHeld == 0 {
			c.flags |= fCloseAfter
			s.mark(t.ring(), t.slot(), c)
		}
	}
	if s.held == nil {
		s.held = list[:0]
	}
}

// runReady answers kept bytes, oldest first, while the pass has answers left.
func (s *server) runReady() {
	for s.answers > 0 && s.ready.len() > 0 {
		t := s.ready.pop()
		c := &s.rings[t.ring()].conns[t.slot()]
		if c.gen != t.gen() || c.flags&fOpen == 0 || c.flags&fClosing != 0 {
			continue
		}
		c.flags &^= fReady
		if c.flags&fHeld != 0 {
			continue // releaseHeld answers it
		}
		s.answers--
		s.serve(t.ring(), t.slot(), c, nil)
		if c.flags&fEOF != 0 && c.flags&fHeld == 0 {
			c.flags |= fCloseAfter
			s.mark(t.ring(), t.slot(), c)
		}
	}
}

// issueReads puts one read in flight per queued connection, up to budget and never more on a
// ring than it has receive buffers.
func (s *server) issueReads(budget int) int {
	issued := 0
	for n := s.rq.len(); n > 0 && issued < budget; n-- {
		t := s.rq.pop()
		r := s.rings[t.ring()]
		c := &r.conns[t.slot()]
		if c.gen != t.gen() || c.flags&fOpen == 0 || c.flags&fClosing != 0 {
			continue
		}
		if c.flags&fRecving != 0 || r.reads >= bufCount {
			s.rq.push(t)
			continue
		}
		c.flags &^= fQueued
		c.flags |= fRecving
		r.reads++
		issued++
		r.u.recvOnce(t.slot(), ud(udRead, t.ring(), t.gen(), t.slot()))
	}
	return issued
}

// readRounds reads queued connections in rounds: a MSG_DONTWAIT read completes inside the
// submit, so a round is one non-blocking enter per ring.
func (s *server) readRounds() {
	budget := readsPerPass
	for budget > 0 && s.rq.len() > 0 {
		k := s.issueReads(budget)
		if k == 0 {
			return
		}
		budget -= k
		for _, r := range s.rings {
			if r.u.pending > 0 {
				r.u.enter(enterGetEvents, 0, nil)
			}
		}
		for ri := range s.rings {
			s.drain(ri)
		}
	}
}

func (s *server) run() {
	ts := timespec{nsec: waitNs}
	for {
		busy := s.rq.len() > 0 || s.ready.len() > 0
		for ri := len(s.rings) - 1; ri >= 0; ri-- {
			u := s.rings[ri].u
			if ri == 0 && !busy {
				u.enter(enterGetEvents, waitMin, &ts)
			} else {
				u.enter(enterGetEvents, 0, nil)
			}
		}
		s.app.now = time.Now().Unix()
		s.answers = answersPerPass
		for ri := range s.rings {
			s.drain(ri)
		}
		s.runReady()
		s.readRounds()
		if len(s.held) > 0 && !s.app.held {
			s.releaseHeld()
		}
		s.endPass()
	}
}
