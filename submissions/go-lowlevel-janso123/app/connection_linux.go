package main

import (
	"golang.org/x/sys/unix"
	"time"
)

const clientTimeout = 75 * time.Second

// Owned only by the loop. The bounded free list is shared between all sockets.
type bufferPool struct {
	free [][]byte
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
	return b[:0]
}
func (p *bufferPool) put(b []byte) {
	if b != nil && cap(b) <= maxInput && len(p.free) < 64 {
		p.free = append(p.free, b[:0])
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
		req, n, parseErr := parseRequest(c.input)
		if parseErr == nil && req.expect && !c.continued {
			c.continued = true
			c.output = append(p.take(), "HTTP/1.1 100 Continue\r\n\r\n"...)
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
