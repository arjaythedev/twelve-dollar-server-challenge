package main

// The event loop. One OS thread drives every ring. A pass submits what the previous pass queued,
// waits briefly for completions, parses every request it read and answers the ones that need no
// database at once; the rest become entries of one batch that the C side runs inside a single
// SQLite transaction at the end of the pass (td_batch). Only after COMMIT are the responses
// handed to the kernel (rule 6: a 201 means committed).

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
	listeners      = 8    // SO_REUSEPORT listeners, each with its own accept queue
	sqEntries      = 4096 //
	cqEntries      = 65536
	bufSize        = 2048 // receive buffer; a longer request is read in more than one piece
	bufCount       = 1024 // receive buffers per ring
	maxRings       = 16
	waitMin        = 256   // a pass waits for this many completions on ring 0 ...
	readsPerPass   = 16384 // single-shot reads issued per pass after a burst
	answersPerPass = 2048  // responses produced per pass; the rest wait, oldest first
	maxReqs        = answersPerPass + 64
	outCap         = 16384 // a response buffer: a feed is ~5 KB
	partCap        = 512   // bytes read but not yet answered (a request is a few hundred bytes)
	feedReserve    = 15360 // room a feed must find in its response buffer
	smallReserve   = 2048  // room any other database response must find
	arenaCap       = 4 << 20
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
	fReady      // on the ready list: bytes read wait in part for a later pass
	fInBatch    // touched in this pass
	fPending    // a database response of this pass will land in out
)

// conn is one direct-descriptor slot. It holds no pointers: buffers are pool indexes (0 = none).
type conn struct {
	gen   uint16
	flags uint16
	out   int32 // responses not yet handed to the kernel
	wire  int32 // the send in flight
	part  int32 // bytes read but not yet answered
	sent  uint32
}

type ring struct {
	u         *uring
	bufs      *bufRing
	conns     []conn
	accepting bool
	full      bool
	armed     [listeners]bool
	open      uint32
	margin    uint32
	reads     uint32 // single-shot reads in flight (each holds at most one receive buffer)
}

// ref names a connection in a queue: ring, slot, generation.
type ref uint64

func mkref(ri int, slot uint32, gen uint16) ref {
	return ref(uint64(ri)<<48 | uint64(gen)<<32 | uint64(slot))
}
func (r ref) ring() int    { return int(r >> 48) }
func (r ref) slot() uint32 { return uint32(r) }
func (r ref) gen() uint16  { return uint16(r >> 32) }

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
func (f *fifo) popBack() ref {
	r := f.q[len(f.q)-1]
	f.q = f.q[:len(f.q)-1]
	if f.head == len(f.q) {
		f.q = f.q[:0]
		f.head = 0
	}
	return r
}

// pend ties a batch entry to the connection and buffer its response lands in.
type pend struct {
	r   ref
	out int32
	off int32
}

type server struct {
	rings      []*ring
	listen     []int
	slots      uint32
	jwt        *verifier
	started    time.Time
	now        int64 // unix seconds, refreshed every pass
	heldW      bool  // writes wait for a checkpoint (shim.c), refreshed between passes
	lifo       bool  // answer the ready list newest first
	window     timespec
	pool       [][]byte
	freeOut    []int32
	freePart   []int32
	dead       []int32
	slabOut    []byte
	slabPart   []byte
	touched    []ref
	held       []ref
	rq         fifo // connections to read, first in first out
	ready      fifo // connections with bytes read but not answered
	skipped    []ref
	answers    int
	req        request
	body       []byte // the unescaped post body of the request being parsed
	arena      []byte // per-pass copies of bodies and names handed to the batch
	pend       []pend
	nreq       int
	needRing   bool
	spillAfter time.Time
	nextLog    time.Time
	logEvery   time.Duration
	requests   uint64
}

// ---- buffers ------------------------------------------------------------------------------------

// Buffers live outside the Go heap (mmap) so their addresses can be handed to C, and are named
// by pool index + 1 (0 = none). Responses take 16 KB buffers; bytes read but not yet answered
// take 512-byte ones, so a backlog of tens of thousands of requests costs megabytes.

func (s *server) carve(slab *[]byte, size, chunk int) []byte {
	if len(*slab) < size {
		*slab = mmapAnon(chunk)
	}
	b := (*slab)[:0:size]
	*slab = (*slab)[size:]
	return b
}

func (s *server) takeOut() int32  { return s.takeKind(&s.freeOut, outCap) }
func (s *server) takePart() int32 { return s.takeKind(&s.freePart, partCap) }

func (s *server) takeKind(free *[]int32, size int) int32 {
	if n := len(*free); n > 0 {
		i := (*free)[n-1]
		*free = (*free)[:n-1]
		return i + 1
	}
	var b []byte
	if size == outCap {
		b = s.carve(&s.slabOut, outCap, 256*outCap)
	} else {
		b = s.carve(&s.slabPart, partCap, 4096*partCap)
	}
	return s.adopt(b)
}

func (s *server) adopt(b []byte) int32 {
	if n := len(s.dead); n > 0 {
		i := s.dead[n-1]
		s.dead = s.dead[:n-1]
		s.pool[i] = b
		return i + 1
	}
	s.pool = append(s.pool, b)
	return int32(len(s.pool))
}

func (s *server) give(i int32) {
	if i == 0 {
		return
	}
	i--
	b := s.pool[i]
	var free *[]int32
	switch cap(b) {
	case outCap:
		free = &s.freeOut
	case partCap:
		free = &s.freePart
	default: // a grown buffer: its own mapping
		syscall.Munmap(b[:cap(b)])
		s.pool[i] = nil
		s.dead = append(s.dead, i)
		return
	}
	// Spare buffers stay in their slab (never unmapped): the pool is sized by the largest
	// backlog seen, which is what the next burst needs.
	s.pool[i] = b[:0]
	*free = append(*free, i)
}

func (s *server) buf(i int32) []byte { return s.pool[i-1] }

// grow makes buffer i hold at least need more bytes, moving its content to a bigger mapping.
func (s *server) grow(i int32, need int) {
	b := s.pool[i-1]
	if cap(b)-len(b) >= need {
		return
	}
	n := (len(b) + need + 4095) &^ 4095
	nb := mmapAnon(n)[:len(b)]
	copy(nb, b)
	s.pool[i-1] = nb
	switch cap(b) {
	case outCap:
		s.freeOut = append(s.freeOut, s.adopt(b[:0])-1)
	case partCap:
		s.freePart = append(s.freePart, s.adopt(b[:0])-1)
	default:
		syscall.Munmap(b[:cap(b)])
	}
}

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
	margin := min(max(slots/8, 64), 1024, slots/4)
	return &ring{u: u, bufs: b, conns: make([]conn, slots), margin: margin}, nil
}

// arm puts a multishot accept on every listener of the accepting ring.
func (s *server) arm(ri int) {
	r := s.rings[ri]
	for li, fd := range s.listen {
		if !r.armed[li] {
			r.u.acceptMulti(fd, ud(udAccept, ri, 0, uint32(li)))
			r.armed[li] = true
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
			s.arm(ri)
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
	s.arm(len(s.rings) - 1)
	fmt.Fprintf(os.Stderr, "ring %d added (%d slots each)\n", len(s.rings)-1, s.slots)
	return true
}

// ---- per-connection helpers ---------------------------------------------------------------------

func (s *server) mark(ri int, slot uint32, c *conn) {
	if c.flags&fInBatch == 0 {
		c.flags |= fInBatch
		s.touched = append(s.touched, mkref(ri, slot, c.gen))
	}
}

func (s *server) queueRead(ri int, slot uint32, c *conn) {
	if c.flags&fQueued == 0 {
		c.flags |= fQueued
		s.rq.push(mkref(ri, slot, c.gen))
	}
}

func (s *server) setReady(ri int, slot uint32, c *conn) {
	if c.flags&(fHeld|fReady) == 0 {
		c.flags |= fReady
		s.ready.push(mkref(ri, slot, c.gen))
	}
}

// finish: the client is done sending. Owed answers go out first, then it closes.
func (s *server) finish(ri int, slot uint32, c *conn) {
	if c.flags&(fHeld|fReady|fPending) != 0 {
		c.flags |= fEOF
	} else {
		c.flags |= fCloseAfter
		s.mark(ri, slot, c)
	}
}

// stash keeps bytes on the connection behind any it already has.
func (s *server) stash(c *conn, data []byte) {
	if c.part == 0 {
		c.part = s.takePart()
	}
	s.grow(c.part, len(data))
	s.pool[c.part-1] = append(s.pool[c.part-1], data...)
}

// received: answered now if this pass has answers left and nothing older waits, else kept.
func (s *server) received(ri int, slot uint32, c *conn, data []byte) {
	if c.flags&fCloseAfter != 0 {
		return
	}
	if c.flags&(fHeld|fReady|fPending) != 0 || s.answers == 0 {
		s.stash(c, data)
		s.setReady(ri, slot, c)
		return
	}
	s.serve(ri, slot, c, data)
}

// errorNow appends a complete error response to the connection's output.
func (s *server) errorNow(ri int, slot uint32, c *conn, status int, msg string) {
	s.mark(ri, slot, c)
	if c.out == 0 {
		c.out = s.takeOut()
	}
	s.grow(c.out, 256)
	s.pool[c.out-1] = errorResp(s.pool[c.out-1], status, msg)
}

// serve answers every complete request in the connection's kept bytes plus data, until one
// needs the database (its response is produced by the batch; the rest wait for the next pass),
// a write meets a checkpoint hold, or the pass runs out of answers.
func (s *server) serve(ri int, slot uint32, c *conn, data []byte) {
	if c.flags&fCloseAfter != 0 {
		return
	}
	buf := data
	if c.part != 0 {
		if len(data) > 0 {
			s.grow(c.part, len(data))
			s.pool[c.part-1] = append(s.pool[c.part-1], data...)
		}
		buf = s.pool[c.part-1]
	}
	off := 0
	stopped := false
	for off < len(buf) && s.answers > 0 {
		used, st := parse(buf[off:], &s.req)
		if st == parsedPartial {
			break
		}
		if st == parsedBad {
			s.errorNow(ri, slot, c, 400, "bad request")
			c.flags |= fCloseAfter
			off = len(buf)
			break
		}
		if s.heldW && s.req.method == mPOST {
			if c.flags&fHeld == 0 {
				c.flags |= fHeld
				s.held = append(s.held, mkref(ri, slot, c.gen))
			}
			stopped = true
			break
		}
		s.answers--
		s.requests++
		rc := s.handle(ri, slot, c, &s.req)
		if rc == hFull { // the batch or the arena is full: this request waits for the next pass
			s.answers = 0
			stopped = true
			break
		}
		off += used
		if s.req.closeAfter {
			c.flags |= fCloseAfter
			s.mark(ri, slot, c)
			off = len(buf)
			break
		}
		if rc == hDeferred {
			stopped = true
			break
		}
	}
	if off < len(buf) && (stopped || s.answers == 0) {
		stopped = true // complete requests may remain: come back next pass
	} else {
		stopped = false
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
		c.part = s.takePart()
		s.grow(c.part, len(rest))
		s.pool[c.part-1] = append(s.pool[c.part-1], rest...)
	}
	if stopped && c.flags&fCloseAfter == 0 {
		s.setReady(ri, slot, c)
	}
}

// ---- the batch ----------------------------------------------------------------------------------

const (
	hDone     = iota // the response is in out
	hDeferred        // the batch will write the response into out
	hFull            // nothing done: no room in this pass's batch
)

// deferReq reserves room in the connection's output for a database response and adds the
// request to the batch. nil if the batch is full.
func (s *server) deferReq(ri int, slot uint32, c *conn, kind int, need int) *reqT {
	if s.nreq >= maxReqs {
		return nil
	}
	s.mark(ri, slot, c)
	if c.out == 0 {
		c.out = s.takeOut()
	}
	s.grow(c.out, need)
	b := s.pool[c.out-1]
	r := reqAt(s.nreq)
	*r = reqT{}
	r.kind = uint8(kind)
	r.out = unsafe.Pointer(unsafe.SliceData(b[len(b):cap(b)]))
	r.cap = int32(cap(b) - len(b))
	s.pend[s.nreq] = pend{r: mkref(ri, slot, c.gen), out: c.out, off: int32(len(b))}
	s.nreq++
	c.flags |= fPending
	return r
}

// keep copies bytes into the pass's arena (they must outlive the receive buffer). nil if full.
func (s *server) keep(b []byte) []byte {
	if len(s.arena)+len(b) > cap(s.arena) {
		return nil
	}
	n := len(s.arena)
	s.arena = append(s.arena, b...)
	return s.arena[n : n+len(b) : n+len(b)]
}

// applyBatch finishes every response the batch produced. With committed false no response of
// the batch may claim success: each becomes a 500 and its connection closes (rule 6).
func (s *server) applyBatch(committed bool) {
	for i := 0; i < s.nreq; i++ {
		p := &s.pend[i]
		r := reqAt(i)
		c := &s.rings[p.r.ring()].conns[p.r.slot()]
		if c.gen != p.r.gen() || c.flags&fOpen == 0 || c.out != p.out {
			s.give(p.out) // the connection went away during the pass; the buffer was kept for C
			continue
		}
		c.flags &^= fPending
		b := s.pool[c.out-1][:p.off]
		switch {
		case !committed:
			b = errorResp(b, 500, "internal server error")
			c.flags |= fCloseAfter
		case r.status&spillFlag != 0:
			s.pool[c.out-1] = b
			s.grow(c.out, int(r.n))
			b = s.pool[c.out-1]
			b = append(b, unsafe.Slice((*byte)(r.out), int(r.n))...)
		default:
			b = b[:int(p.off)+int(r.n)]
		}
		s.pool[c.out-1] = b
		if c.flags&fEOF != 0 && c.flags&(fHeld|fReady) == 0 {
			c.flags |= fCloseAfter
		}
	}
	s.nreq = 0
	s.arena = s.arena[:0]
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
			s.arm(ri)
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
		if flags&cqeFMore == 0 {
			r.armed[slot] = false // re-armed by drain while this ring accepts
		}
		if res >= 0 {
			c := &r.conns[res]
			g := c.gen + 1
			*c = conn{gen: g, flags: fOpen}
			r.u.recvMulti(uint32(res), ud(udRecv, ri, g, uint32(res)))
			r.open++
			if r.open+r.margin >= uint32(len(r.conns)) {
				r.full = true
			}
		} else if res == -int32(syscall.ENFILE) || res == -int32(syscall.ENOSPC) {
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
			if c.flags&fPending == 0 {
				s.give(c.out)
				c.out = 0
			}
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
			if c.flags&fPending == 0 {
				s.give(c.out) // else applyBatch frees it: C may still be writing into it
			}
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

// endPass runs the batch, commits, then hands every response to the kernel.
func (s *server) endPass() {
	committed := true
	if s.nreq > 0 {
		committed = dbBatch(s.nreq)
	}
	s.applyBatch(committed)
	dbTick() // no transaction is open: the only point where a checkpoint hold starts or ends
	s.heldW = writesHeld()
	for _, t := range s.touched {
		r := s.rings[t.ring()]
		slot := t.slot()
		c := &r.conns[slot]
		c.flags &^= fInBatch
		if c.gen != t.gen() || c.flags&fOpen == 0 || c.flags&(fClosing|fSending) != 0 {
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
		if c.flags&(fReady|fPending) != 0 {
			continue
		}
		s.serve(t.ring(), t.slot(), c, nil)
		if c.flags&fEOF != 0 && c.flags&(fHeld|fReady|fPending) == 0 {
			c.flags |= fCloseAfter
			s.mark(t.ring(), t.slot(), c)
		}
	}
	if s.held == nil {
		s.held = list[:0]
	}
}

// runReady answers kept bytes while the pass has answers left: oldest first, or newest first
// with ANSWER_ORDER=lifo.
func (s *server) runReady() {
	for s.answers > 0 && s.ready.len() > 0 {
		var t ref
		if s.lifo {
			t = s.ready.popBack()
		} else {
			t = s.ready.pop()
		}
		c := &s.rings[t.ring()].conns[t.slot()]
		if c.gen != t.gen() || c.flags&fOpen == 0 || c.flags&fClosing != 0 {
			continue
		}
		if c.flags&fPending != 0 {
			// Its earlier request is in this pass's batch: the rest of its bytes wait for the
			// next pass (responses must leave in order). It stays on the list.
			s.skipped = append(s.skipped, t)
			continue
		}
		c.flags &^= fReady
		if c.flags&fHeld != 0 {
			continue // releaseHeld serves it
		}
		s.serve(t.ring(), t.slot(), c, nil)
		if c.flags&fEOF != 0 && c.flags&(fHeld|fReady|fPending) == 0 {
			c.flags |= fCloseAfter
			s.mark(t.ring(), t.slot(), c)
		}
	}
	for _, t := range s.skipped {
		s.ready.push(t)
	}
	s.skipped = s.skipped[:0]
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
				r.u.poll()
			}
		}
		for ri := range s.rings {
			s.drain(ri)
		}
	}
}

func (s *server) log(now time.Time) {
	var open uint32
	for _, r := range s.rings {
		open += r.open
	}
	var st [24]int64
	dbStats(st[:])
	fmt.Fprintf(os.Stderr, "%s conns=%d rings=%d requests=%d feed=%d post=%d like=%d insert=%d fallback=%d spill=%d "+
		"wal_writes=%d wal_frames=%d trunc=%d/%d evict=%d fetch=%d ckpt=%d rounds=%d bulk_ms=%d hold_ms=%d/max %d tail_ms=%d/max %d inline=%d tail_full=%d/partial %d restarts=%d ck_rc=%d pool=%d backlog=%d\n",
		now.Format("15:04:05"), open, len(s.rings), s.requests, st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7],
		st[9], st[8], st[10], st[11], st[12], st[23], st[22], st[13], st[14], st[20], st[21], st[15], st[16], st[17], st[18], st[19], len(s.pool), s.ready.len())
}

func (s *server) run() {
	for {
		busy := s.rq.len() > 0 || s.ready.len() > 0
		for ri := len(s.rings) - 1; ri >= 0; ri-- {
			u := s.rings[ri].u
			if ri == 0 && !busy {
				u.enter(enterGetEvents, waitMin, &s.window)
			} else {
				u.poll()
			}
		}
		now := time.Now()
		s.now = now.Unix()
		s.answers = answersPerPass
		for ri := range s.rings {
			s.drain(ri)
		}
		s.runReady()
		s.readRounds()
		if len(s.held) > 0 && !s.heldW {
			s.releaseHeld()
		}
		s.endPass()
		if now.After(s.nextLog) {
			s.nextLog = now.Add(s.logEvery)
			s.log(now)
		}
	}
}
