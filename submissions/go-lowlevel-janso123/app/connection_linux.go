package main

import (
	"golang.org/x/sys/unix"
	"time"
)

const clientTimeout = 75 * time.Second
const bufferPoolMaxBytes = 8 << 20

// Owned only by the loop. The bounded free list is shared between all sockets.
type bufferPool struct {
	free      [][]byte
	freeBytes int
	// blocked is set by the memory guard while it is engaged: the pool then
	// neither grows (put is a no-op) nor retains already-pooled buffers, so the
	// heaviest userland retention in the process is released instead of being
	// held while the kernel is short of memory. Buffers are reallocated lazily.
	blocked bool
	// Nil hooks preserve epoll's direct socket syscalls. The ring hook retains
	// output until a send CQE advances offset on the loop thread.
	write    func(*connection, []byte) (int, error)
	drain    func(*connection) (bool, error)
	shutdown func(*connection) error
}

func (p *bufferPool) take() []byte {
	n := len(p.free)
	if n == 0 {
		return make([]byte, 0, 4096)
	}
	b := p.free[n-1]
	p.free[n-1] = nil
	p.free = p.free[:n-1]
	p.freeBytes -= cap(b)
	return b[:0]
}
func (p *bufferPool) put(b []byte) {
	if b == nil || p.blocked {
		return
	}
	if cap(b) <= maxInput && cap(b) <= bufferPoolMaxBytes-p.freeBytes {
		p.free = append(p.free, b[:0])
		p.freeBytes += cap(b)
	}
}

// drop releases every pooled buffer to the GC. The pool's free list is the only
// place the process can voluntarily give memory back on demand.
func (p *bufferPool) drop() {
	for i := range p.free {
		p.free[i] = nil
	}
	p.free = p.free[:0]
	p.freeBytes = 0
}

// setBlocked toggles pressure-relief mode. On engage it drops what the pool is
// already holding; while blocked put is a no-op so it cannot grow again.
// Releases (unblocked) restore the normal pooled behaviour exactly.
func (p *bufferPool) setBlocked(blocked bool) {
	p.blocked = blocked
	if blocked {
		p.drop()
	}
}

// Loop-owned state; pending writes retain only transport flags, never a job pointer.
type connection struct {
	input, output                []byte
	offset                       int
	deadline                     int64
	fd                           int32
	generation                   uint32
	closing, continued, draining bool
	pending                      bool
	pendingProto10               bool
}

func (c *connection) receive(b []byte, p *bufferPool) {
	if len(b) == 0 {
		return
	}
	if c.input == nil {
		c.input = p.take()
	}
	c.input = append(c.input, b...)
}
func (c *connection) release(p *bufferPool) {
	p.put(c.input)
	p.put(c.output)
	c.input, c.output = nil, nil
	c.offset = 0
}

// releaseIdle returns the buffer of a connection that holds no request state
// (no partial request buffered, no response being built, no pending send, not
// draining). Such a connection is idle and keeps no per-connection buffer while
// the guard is engaged; the buffer is reallocated lazily on the next read. Only
// truly idle connections are touched, so no in-flight data is ever dropped.
func (c *connection) releaseIdle(p *bufferPool) {
	if c.pending || c.draining || c.output != nil || c.offset != 0 || len(c.input) != 0 {
		return
	}
	if c.input != nil {
		p.put(c.input)
		c.input = nil
	}
}

// shedIdle reports whether a connection holds no in-flight work and may be
// closed immediately by the emergency shedder: no partial request buffered, no
// response being built, no pending send, no in-flight writer/DB job, not
// draining. Durability: an acknowledged write is only produced once the writer
// job completes (which clears pending), so shedIdle is never true for a
// connection whose write has not committed; closing here can only drop work the
// server never acknowledged.
func (c *connection) shedIdle() bool {
	return !c.pending && !c.draining && c.output == nil && c.offset == 0 && len(c.input) == 0
}

// shedSweepIdle closes at most budget idle connections from conns, calling
// closer then counted for each. It never selects a connection with in-flight
// work (see shedIdle), so a response being written or a writer job in the queue
// is left for its own completion path. It returns the number closed. Mutating
// the map in closer (as the epoll backend does) is safe during the range.
func shedSweepIdle(conns map[int32]*connection, budget int, closer func(*connection), counted func()) int {
	closed := 0
	for _, c := range conns {
		if closed >= budget {
			break
		}
		if !c.shedIdle() {
			continue
		}
		counted()
		closer(c)
		closed++
	}
	return closed
}

func (c *connection) flush(p *bufferPool) (bool, error) {
	for c.offset < len(c.output) {
		var n int
		var e error
		if p.write == nil {
			n, e = unix.Write(int(c.fd), c.output[c.offset:])
		} else {
			n, e = p.write(c, c.output[c.offset:])
		}
		if n > 0 {
			c.offset += n
		}
		if e == unix.EINTR {
			continue
		}
		if e == unix.EAGAIN || e == unix.EWOULDBLOCK {
			return false, nil
		}
		if e != nil {
			return false, e
		}
		if n == 0 {
			return false, nil
		}
	}
	p.put(c.output)
	c.output = nil
	c.offset = 0
	return true, nil
}
func (c *connection) drive(p *bufferPool, handle func(job) reply, now time.Time) (bool, error) {
	if c.pending {
		return false, nil
	}
	for handled := 0; handled < 32; handled++ {
		complete, e := c.flush(p)
		if e != nil || !complete {
			return false, e
		}
		if c.closing {
			return true, nil
		}
		if len(c.input) == 0 {
			if c.input != nil {
				p.put(c.input)
				c.input = nil
			}
			c.deadline = now.Add(clientTimeout).UnixNano()
			return false, nil
		}
		req, n, parseErr := parseRequestPooled(c.input, p)
		if parseErr == nil && req.expect && !c.continued {
			c.continued = true
			c.output = append(p.take(), "HTTP/1.1 100 Continue\r\n\r\n"...)
			if req.bodyPooled {
				p.put(req.job.body)
			}
			continue
		}
		if parseErr == nil && n == 0 {
			if len(c.input) > maxInput {
				parseErr = &parseError{status: 413}
			} else {
				return false, nil
			}
		}
		var r reply
		protocolError := false
		if parseErr != nil {
			req.close = true
			c.closing = true
			if parseErr.status == 413 {
				r = failure(413, "request body too large")
			} else {
				protocolError = true
				r = reply{parseErr.status, []byte(protocolErrorText(parseErr))}
			}
			p.put(c.input)
			c.input = nil
		} else {
			// status zero means a validated write was admitted; resume on commit completion.
			r = handle(req.job)
			if req.bodyPooled {
				p.put(req.job.body)
			}
			c.closing = req.close
			copy(c.input, c.input[n:])
			c.input = c.input[:len(c.input)-n]
			if len(c.input) == 0 {
				p.put(c.input)
				c.input = nil
			}
		}
		c.continued = false
		if r.status == 0 {
			c.pending = true
			c.pendingProto10 = req.proto10
			c.deadline = now.Add(clientTimeout).UnixNano()
			return false, nil
		}
		c.output = appendResponse(p.take(), r, req, now, protocolError)
		c.deadline = now.Add(clientTimeout).UnixNano()
	}
	// EPOLLOUT remains enabled for buffered pipeline work, even if output is empty.
	return false, nil
}
func (c *connection) readReady(p *bufferPool, handle func(job) reply, now time.Time) (bool, error) {
	var scratch [8192]byte
	for reads := 0; reads < 32; reads++ {
		n, e := unix.Read(int(c.fd), scratch[:])
		if n > 0 {
			if c.input == nil {
				c.deadline = now.Add(clientTimeout).UnixNano()
			}
			c.receive(scratch[:n], p)
			closed, err := c.drive(p, handle, now)
			if closed || err != nil {
				return closed, err
			}
			if c.output != nil || c.pending {
				return false, nil
			}
		}
		if e == unix.EINTR {
			continue
		}
		if e == unix.EAGAIN || e == unix.EWOULDBLOCK {
			return false, nil
		}
		if e != nil {
			return false, e
		}
		if n == 0 {
			return true, nil
		}
	}
	return false, nil
}

// After the response, half-close the write side and discard incoming bytes for
// a short grace period. Immediate close with an unread oversized body sends RST.
// The existing coarse sweep closes peers that never finish; no timer is allocated.
func (c *connection) beginDrain(p *bufferPool) (bool, error) {
	c.release(p)
	var e error
	if p.shutdown == nil {
		e = unix.Shutdown(int(c.fd), unix.SHUT_WR)
	} else {
		e = p.shutdown(c)
	}
	if e != nil {
		return true, e
	}
	c.draining = true
	c.deadline = time.Now().Add(500 * time.Millisecond).UnixNano()
	if p.drain != nil {
		return p.drain(c)
	}
	return c.drain()
}
func (c *connection) drain() (bool, error) {
	var scratch [8192]byte
	for reads := 0; reads < 32; reads++ {
		n, e := unix.Read(int(c.fd), scratch[:])
		if e == unix.EINTR {
			continue
		}
		if e == unix.EAGAIN || e == unix.EWOULDBLOCK {
			return false, nil
		}
		if e != nil {
			return true, e
		}
		if n == 0 {
			return true, nil
		}
	}
	return false, nil
}

// A completion never contains a connection pointer. Reject fd reuse and duplicates.
func (c *connection) complete(p *bufferPool, result writeCompletion) bool {
	if c.fd != result.fd || c.generation != result.generation || !c.pending {
		return false
	}
	c.pending = false
	c.output = appendResponse(p.take(), result.reply, request{close: c.closing, proto10: c.pendingProto10}, time.Now(), false)
	c.deadline = time.Now().Add(clientTimeout).UnixNano()
	return true
}
