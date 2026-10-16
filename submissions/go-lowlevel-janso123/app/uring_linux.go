//go:build linux && (amd64 || arm64)

package main

import (
	"errors"
	"fmt"
	"golang.org/x/sys/unix"
	"os"
	"time"
	"lowlevel/internal/netring"
)

const (
	acceptTag       = uint64(1)
	writerWakeTag   = uint64(2)
	recvKind        = uint64(2)
	sendKind        = uint64(3)
	cancelRecvKind  = uint64(4)
	cancelSendKind  = uint64(5)
	shutdownKind    = uint64(6)
	closeKind       = uint64(7)
	acceptCancelTag = uint64(3)
	directRingBit   = int32(1 << 28)
	directSlotMask  = int32((1 << 28) - 1)
)

// IDs combine a non-wrapping generation, a descriptor and an operation kind.
// A descriptor stays open until both original operations' terminal CQEs arrive;
// cancel acknowledgements never advance offsets or free send memory.
func tag(c *connection, kind uint64) uint64 {
	return uint64(c.generation)<<32 | uint64(uint32(c.fd))<<3 | kind
}

type uringConn struct {
	c                                            *connection
	r                                            *netring.Ring
	ringIndex                                    int
	pending                                      []netring.Completion // held provided buffers: bounded globally by ring size
	recvLive, recvCancel, sendLive, closing, eof bool
	shutdownLive, closeLive                      bool
	sendBytes                                    int
}
type uringBackend struct {
	r            *netring.Ring
	other        *netring.Ring
	direct       bool
	active       [2]int
	accepts      [2]uint64
	conns        map[int32]*uringConn
	pool         bufferPool
	generation   uint32
	acceptLive   [2]bool
	acceptCancel [2]bool
	acceptRetry  [2]time.Time
	// A fixed slot cannot be handed to a later accept until its CLOSE CQE has
	// been consumed. Stop the multishot accept while closes are in flight.
	closeWaiting    map[*uringConn]bool
	waitingClose    [2]int
	closePending    [2]int
	dirty           map[*uringConn]bool
	stale           uint64
	handle          func(job) reply
	w               *writer
	current         *connection
	work            []*uringConn
	waiting         map[*uringConn]bool
}

func newUringBackend() (*uringBackend, error) {
	if e := netring.Probe(); e != nil {
		return nil, fmt.Errorf("required-feature probe: %w", e)
	}
	r, e := netring.Open(1024, 256)
	if e != nil {
		return nil, e
	}
	b := &uringBackend{r: r, conns: make(map[int32]*uringConn), dirty: make(map[*uringConn]bool), waiting: make(map[*uringConn]bool)}
	b.pool.write = b.write
	b.pool.drain = func(c *connection) (bool, error) { return b.conns[c.fd].eof, nil }
	return b, nil
}
func newDirectBackend(slots int) (*uringBackend, error) {
	if slots < 1 || slots > int(directSlotMask) {
		return nil, fmt.Errorf("invalid direct table size %d", slots)
	}
	if e := netring.Probe(); e != nil {
		return nil, fmt.Errorf("normal ring feature probe: %w", e)
	}
	if e := netring.ProbeDirect(); e != nil {
		return nil, fmt.Errorf("direct ring feature probe: %w", e)
	}
	first, e := netring.Open(1024, 256)
	if e != nil {
		return nil, e
	}
	if e = first.RegisterDirect(slots); e != nil {
		first.Close()
		return nil, e
	}
	second, e := netring.Open(1024, 256)
	if e != nil {
		first.Close()
		return nil, e
	}
	if e = second.RegisterDirect(slots); e != nil {
		second.Close()
		first.Close()
		return nil, e
	}
	b := &uringBackend{r: first, other: second, direct: true, conns: make(map[int32]*uringConn), dirty: make(map[*uringConn]bool), waiting: make(map[*uringConn]bool), closeWaiting: make(map[*uringConn]bool)}
	b.pool.write = b.write
	b.pool.drain = func(c *connection) (bool, error) { return b.conns[c.fd].eof, nil }
	b.pool.shutdown = b.shutdown
	return b, nil
}
func (b *uringBackend) ring(i int) *netring.Ring {
	if i == 1 {
		return b.other
	}
	return b.r
}
func (b *uringBackend) connRing(s *uringConn) *netring.Ring {
	if s.r != nil {
		return s.r
	}
	return b.r
}
func (b *uringBackend) slot(s *uringConn) int {
	if b.direct {
		return int(s.c.fd & directSlotMask)
	}
	return int(s.c.fd)
}
func (b *uringBackend) shutdown(c *connection) error {
	s := b.conns[c.fd]
	if s == nil || s.closing {
		return unix.EBADF
	}
	b.connRing(s).ShutdownDirect(b.slot(s), unix.SHUT_WR, tag(c, shutdownKind))
	s.shutdownLive = true
	return nil
}
func (b *uringBackend) write(c *connection, data []byte) (int, error) {
	s := b.conns[c.fd]
	if s.closing {
		return 0, unix.EPIPE
	}
	if !s.sendLive {
		n, e := b.connRing(s).Send(b.slot(s), data, tag(c, sendKind))
		if e != nil {
			return 0, e
		}
		s.sendBytes = n
		s.sendLive = true
	}
	return 0, unix.EAGAIN
}
func (b *uringBackend) arm(s *uringConn) {
	if !s.closing && !s.eof && !s.recvLive && !s.recvCancel && len(s.pending) == 0 && s.c.output == nil && !s.c.pending {
		if b.connRing(s).Available() == 0 {
			b.waiting[s] = true
			return
		}
		delete(b.waiting, s)
		b.connRing(s).Recv(b.slot(s), tag(s.c, recvKind))
		s.recvLive = true
	}
}
func (b *uringBackend) pause(s *uringConn) {
	if s.recvLive && !s.recvCancel {
		b.connRing(s).Cancel(tag(s.c, recvKind), tag(s.c, cancelRecvKind))
		s.recvCancel = true
	}
}
func (b *uringBackend) retire(s *uringConn) {
	if b.conns[s.c.fd] != s || !s.closing || s.recvLive || s.sendLive || s.recvCancel || s.shutdownLive || s.closeLive {
		return
	}
	if b.direct {
		if b.acceptLive[s.ringIndex] {
			if !b.closeWaiting[s] {
				b.closeWaiting[s] = true
				b.waitingClose[s.ringIndex]++
			}
			b.pauseAccept(s.ringIndex)
			return
		}
		if b.closeWaiting[s] {
			delete(b.closeWaiting, s)
			b.waitingClose[s.ringIndex]--
		}
		b.connRing(s).CloseDirect(b.slot(s), tag(s.c, closeKind))
		s.closeLive = true
		b.closePending[s.ringIndex]++
		return
	}
	unix.Close(int(s.c.fd))
	s.c.release(&b.pool)
	delete(b.conns, s.c.fd)
	delete(b.dirty, s)
}
func (b *uringBackend) closeConn(s *uringConn) {
	if !s.closing {
		s.closing = true
		delete(b.waiting, s)
		b.pause(s)
		if s.sendLive {
			b.connRing(s).Cancel(tag(s.c, sendKind), tag(s.c, cancelSendKind))
		}
		for _, v := range s.pending {
			b.connRing(s).Release(v)
		}
		s.pending = nil
	}
	b.retire(s)
}
func (b *uringBackend) drive(s *uringConn, now time.Time) {
	c := s.c
	if s.closing {
		b.retire(s)
		return
	}
	if c.draining {
		for _, v := range s.pending {
			b.connRing(s).Release(v)
		}
		s.pending = nil
		if s.eof {
			b.closeConn(s)
		} else {
			b.arm(s)
		}
		return
	}
	if s.sendLive {
		return
	}
	// A receive chunk is consumed only when the shared driver has no outstanding
	// output. Holding selected buffers bounds unread userspace bytes globally.
	for turns := 0; turns < 32; turns++ {
		b.current = c
		closed, e := c.drive(&b.pool, b.handle, now)
		if closed && e == nil && c.closing && !c.draining {
			closed, e = c.beginDrain(&b.pool)
		}
		if closed || e != nil {
			b.closeConn(s)
			return
		}
		if s.sendLive {
			return
		}
		if c.pending {
			// The writer owns the response until COMMIT. Keep any already
			// selected receive buffers bounded and do not parse its pipeline.
			b.pause(s)
			return
		}
		if c.draining {
			b.dirty[s] = true
			return
		}
		if len(s.pending) > 0 {
			v := s.pending[0]
			s.pending[0] = netring.Completion{}
			s.pending = s.pending[1:]
			if len(s.pending) == 0 {
				s.pending = nil
			}
			if c.input == nil {
				c.deadline = now.Add(clientTimeout).UnixNano()
			}
			c.receive(b.connRing(s).Bytes(v), &b.pool)
			b.connRing(s).Release(v)
			continue
		}
		if len(c.input) > 0 {
			_, n, e := parseRequest(c.input)
			if n > 0 || e != nil {
				b.dirty[s] = true
				return
			}
		}
		if s.eof {
			b.closeConn(s)
			return
		}
		b.arm(s)
		return
	}
	b.dirty[s] = true
}

// handle is set only while loop runs; tests can use the same driver with a
// synthetic handler without SQLite. It is not an alternate application path.
func (b *uringBackend) complete(v netring.Completion, now time.Time) error {
	return b.completeOn(0, v, now)
}
func (b *uringBackend) completeOn(ringIndex int, v netring.Completion, now time.Time) error {
	if v.Tag == writerWakeTag {
		if ringIndex != 0 {
			return fmt.Errorf("writer wake on ring B")
		}
		if b.w == nil || v.Result < 0 || v.Result&unix.POLLIN == 0 {
			return fmt.Errorf("writer eventfd poll: %+v", v)
		}
		var counter [8]byte
		for {
			_, e := unix.Read(b.w.wake, counter[:])
			if e == unix.EINTR {
				continue
			}
			if e != nil && e != unix.EAGAIN {
				return fmt.Errorf("writer eventfd read: %w", e)
			}
			break
		}
		for completed := 0; completed < cap(b.w.results); completed++ {
			select {
			case result := <-b.w.results:
				<-b.w.slots
				s := b.conns[result.fd]
				if s != nil && s.c.complete(&b.pool, result) {
					b.dirty[s] = true
				}
			default:
				b.r.Poll(b.w.wake, unix.POLLIN, writerWakeTag)
				return nil
			}
		}
		b.r.Poll(b.w.wake, unix.POLLIN, writerWakeTag)
		return nil
	}
	if v.Tag == acceptCancelTag {
		b.acceptCancel[ringIndex] = false
		if v.Result != 0 && v.Result != -int32(unix.ENOENT) && v.Result != -int32(unix.EALREADY) {
			return fmt.Errorf("cancel accept: %d", v.Result)
		}
		return nil
	}
	if v.Tag == acceptTag {
		b.acceptLive[ringIndex] = v.More()
		if b.direct && !v.More() {
			// All accept CQEs for this multishot operation precede its
			// terminal CQE. Only now may queued socket closes be submitted.
			for s := range b.closeWaiting {
				if s.ringIndex == ringIndex {
					b.dirty[s] = true
				}
			}
		}
		if v.Result < 0 {
			e := unix.Errno(-v.Result)
			if e == unix.ECANCELED {
				return nil
			}
			if e == unix.EMFILE || e == unix.ENFILE || e == unix.ENOBUFS || e == unix.ENOMEM {
				b.acceptRetry[ringIndex] = now.Add(50 * time.Millisecond)
				if b.direct && e == unix.ENFILE {
					b.pauseAccept(ringIndex)
				}
				return nil
			}
			if e == unix.EINTR || e == unix.EAGAIN || e == unix.ECONNABORTED || e == unix.EPROTO {
				return nil
			}
			return fmt.Errorf("multishot accept: %w", e)
		}
		fd := int(v.Result)
		if fd >= 1<<28 || b.generation == ^uint32(0) {
			if !b.direct {
				unix.Close(fd)
			}
			// The fixed table is released by Ring.Close on this error path.
			// Never wrap generation or submit an untracked direct CLOSE while
			// a multishot accept could reuse that slot before its close CQE.
			return fmt.Errorf("connection identity exhausted: slot=%d generation=%d", fd, b.generation)
		}
		if !b.direct {
			if e := unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1); e != nil {
				unix.Close(fd)
				return nil
			}
		}
		b.generation++
		if b.direct {
			fd |= ringIndex << 28
			b.active[ringIndex]++
			b.accepts[ringIndex]++
		}
		c := &connection{fd: int32(fd), generation: b.generation, deadline: now.Add(clientTimeout).UnixNano()}
		s := &uringConn{c: c, r: b.ring(ringIndex), ringIndex: ringIndex}
		b.conns[c.fd] = s
		b.arm(s)
		if b.direct && b.active[ringIndex] >= b.ring(ringIndex).DirectSlots() {
			b.pauseAccept(ringIndex)
		}
		return nil
	}
	fd := int32(uint32(v.Tag) >> 3)
	gen := uint32(v.Tag >> 32)
	kind := v.Tag & 7
	s := b.conns[fd]
	if s == nil || s.c.generation != gen {
		b.stale++
		// Old recv CQEs still return their selected buffers; stale tags must never
		// dispatch bytes to a different connection or advance its send offset.
		if v.HasBuffer() {
			b.ring(ringIndex).Release(v)
		}
		return nil
	}
	if s.ringIndex != ringIndex {
		return fmt.Errorf("completion delivered on wrong ring: %+v", v)
	}
	switch kind {
	case recvKind:
		if !s.recvLive {
			return errors.New("recv completion without live receive")
		}
		if !v.More() {
			s.recvLive = false
		}
		if v.Result > 0 {
			if !v.HasBuffer() {
				return errors.New("recv data without selected buffer")
			}
			if s.closing || s.c.draining {
				b.connRing(s).Release(v)
			} else {
				s.pending = append(s.pending, v)
				if s.sendLive || s.c.output != nil || s.c.pending {
					b.pause(s)
				}
			}
		} else {
			if v.HasBuffer() {
				b.connRing(s).Release(v)
			}
			if v.Result == 0 {
				s.eof = true
			} else if v.Result != -int32(unix.ENOBUFS) && v.Result != -int32(unix.ECANCELED) && v.Result != -int32(unix.EAGAIN) && v.Result != -int32(unix.EINTR) {
				b.closeConn(s)
			}
		}
	case sendKind:
		if !s.sendLive {
			return errors.New("send completion without live send")
		}
		b.connRing(s).FinishSend(v.Tag)
		s.sendLive = false
		if v.Result > 0 && int(v.Result) <= s.sendBytes {
			s.c.offset += int(v.Result)
		} else if v.Result != -int32(unix.EAGAIN) && v.Result != -int32(unix.EINTR) {
			b.closeConn(s)
		}
	case cancelRecvKind:
		s.recvCancel = false
		if v.Result != 0 && v.Result != -int32(unix.ENOENT) && v.Result != -int32(unix.EALREADY) {
			return fmt.Errorf("cancel recv: %d", v.Result)
		}
	case cancelSendKind:
		if v.Result != 0 && v.Result != -int32(unix.ENOENT) && v.Result != -int32(unix.EALREADY) {
			return fmt.Errorf("cancel send: %d", v.Result)
		}
	case shutdownKind:
		if !s.shutdownLive {
			return errors.New("shutdown completion without live shutdown")
		}
		s.shutdownLive = false
		if v.Result < 0 && v.Result != -int32(unix.ENOTCONN) {
			b.closeConn(s)
		}
	case closeKind:
		if !s.closeLive {
			return errors.New("close completion without live close")
		}
		if v.Result != 0 {
			return fmt.Errorf("direct close failed: %d", v.Result)
		}
		s.closeLive = false
		b.closePending[ringIndex]--
		s.c.release(&b.pool)
		delete(b.conns, s.c.fd)
		delete(b.dirty, s)
		b.active[ringIndex]--
		return nil
	default:
		return fmt.Errorf("unknown completion kind %d", kind)
	}
	if b.conns[s.c.fd] == s {
		b.dirty[s] = true
	}
	return nil
}
func (b *uringBackend) pauseAccept(i int) {
	if b.acceptLive[i] && !b.acceptCancel[i] {
		b.ring(i).Cancel(acceptTag, acceptCancelTag)
		b.acceptCancel[i] = true
	}
}
func (b *uringBackend) loop(listener int, a *app, w *writer, stop <-chan os.Signal, onStop func() error) (err error) {
	b.handle = a.handle
	b.w = w
	a.submit = func(q writeRequest) reply {
		q.fd = b.current.fd
		q.generation = b.current.generation
		if !w.submit(q) {
			return failure(503, "writer unavailable")
		}
		return reply{}
	}
	b.r.Poll(w.wake, unix.POLLIN, writerWakeTag)
	defer func() {
		// Cancel-and-wait before socket release or mmap reclamation. Queued but
		// unsubmitted SQEs are abandoned by Ring.Close rather than issued on reused fds.
		err = errors.Join(err, b.r.Close())
		if b.other != nil {
			err = errors.Join(err, b.other.Close())
		}
		for _, s := range b.conns {
			if !b.direct {
				unix.Close(int(s.c.fd))
			}
			s.c.release(&b.pool)
		}
	}()
	drain := func(i int, wait time.Duration) (int, error) {
		events, e := b.ring(i).Wait(wait)
		if e != nil {
			return 0, e
		}
		now := time.Now()
		for _, v := range events {
			if e = b.completeOn(i, v, now); e != nil {
				return 0, e
			}
		}
		return len(events), nil
	}
	sweep := time.Now().Add(time.Second)
	for {
		select {
		case <-stop:
			return onStop()
		default:
		}
		now := time.Now()
		ringCount := 1
		if b.direct {
			ringCount = 2
		}
		for i := 0; i < ringCount; i++ {
			if !b.acceptLive[i] && !b.acceptCancel[i] && b.waitingClose[i] == 0 && b.closePending[i] == 0 && !now.Before(b.acceptRetry[i]) && (!b.direct || b.active[i] < b.ring(i).DirectSlots()) {
				b.ring(i).Accept(listener)
				b.acceptLive[i] = true
			}
		}
		b.work = b.work[:0]
		for s := range b.dirty {
			b.work = append(b.work, s)
			delete(b.dirty, s)
		}
		for i, s := range b.work {
			b.drive(s, now)
			b.work[i] = nil
		}
		if !now.Before(sweep) {
			for _, s := range b.conns {
				if now.UnixNano() >= s.c.deadline {
					b.closeConn(s)
				}
			}
			sweep = now.Add(time.Second)
		}
		// ENOBUFS waiters are rearmed only after buffers have actually returned;
		// a stalled reader holding the whole pool must not cause an enter spin.
		for s := range b.waiting {
			if b.connRing(s).Available() > 0 {
				delete(b.waiting, s)
				b.arm(s)
			}
		}
		if !b.direct {
			wait := 100 * time.Millisecond
			if len(b.dirty) > 0 {
				wait = 0
			}
			if _, e := drain(0, wait); e != nil {
				return e
			}
		} else {
			// DEFER_TASKRUN needs enter on each ring. Drain both with a zero
			// timeout before either can sleep. A short timed wait only happens
			// when neither ring produced a CQE or parser work. This bounds the
			// other ring's wake latency without cross-ring poll registration.
			na, e := drain(0, 0)
			if e != nil {
				return e
			}
			nb, e := drain(1, 0)
			if e != nil {
				return e
			}
			if na+nb == 0 && len(b.dirty) == 0 {
				waitingRing := 0
				if b.active[0] >= b.r.DirectSlots() || (!b.acceptLive[0] && b.acceptLive[1]) {
					waitingRing = 1
				}
				if _, e = drain(waitingRing, 5*time.Millisecond); e != nil {
					return e
				}
			}
		}
	}
}
