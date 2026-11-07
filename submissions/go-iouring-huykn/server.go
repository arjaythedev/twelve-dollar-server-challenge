package main

import (
	"errors"
	"syscall"
	"time"

	"github.com/godzie44/go-uring/uring"
)

// One operation per connection keeps receive/send buffers alive and unchanged
// until completion. The accept operation also stays reachable while in flight.
type connection struct {
	in, out []byte
	used    int
	sent    int
	sending bool
	closing bool
}

type server struct {
	ring    *uring.Ring
	app     *app
	clients map[int]*connection
	accept  *uring.AcceptOp
	ready   []int
}

func (s *server) queue(op uring.Operation, tag uint64) error {
	err := s.ring.QueueSQE(op, 0, tag)
	if errors.Is(err, uring.ErrSQOverflow) {
		if _, err = s.ring.Submit(); err != nil {
			return err
		}
		err = s.ring.QueueSQE(op, 0, tag)
	}
	return err
}

func (s *server) close(fd int) {
	syscall.Close(fd)
	delete(s.clients, fd)
}

func (s *server) receive(fd int, c *connection) error {
	if c.used == len(c.in) {
		if len(c.in) >= maxHead+maxBody {
			s.close(fd)
			return nil
		}
		buf := make([]byte, min(len(c.in)*2, maxHead+maxBody))
		copy(buf, c.in[:c.used])
		c.in = buf
	}
	c.sending = false
	return s.queue(uring.Recv(uintptr(fd), c.in[c.used:], 0), uint64(fd)+1)
}

func (s *server) send(fd int, c *connection) error {
	c.sending = true
	return s.queue(uring.Send(uintptr(fd), c.out[c.sent:], syscall.MSG_NOSIGNAL), uint64(fd)+1)
}

// Build the complete response before submitting any bytes to the kernel.
// A failed commit replaces the pending response, so no uncommitted success leaks.
func (s *server) respond(fd int, c *connection) error {
	var req request
	n, state := parse(c.in[:c.used], &req)
	if state == parsedPartial {
		return s.receive(fd, c)
	}
	c.out = c.out[:0]
	if state == parsedBad {
		c.out = errorResp(c.out, 400, "malformed HTTP request")
		c.closing = true
	} else {
		s.app.now = time.Now().Unix()
		c.out = s.app.handle(&req, c.out)
		copy(c.in, c.in[n:c.used])
		c.used -= n
	}
	c.sent = 0
	s.ready = append(s.ready, fd)
	return nil
}

func (s *server) complete(tag uint64, result int32) error {
	var err error
	if tag == 0 {
		if result >= 0 {
			fd := int(result)
			syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
			c := &connection{in: make([]byte, 4096), out: make([]byte, 0, 8192)}
			s.clients[fd] = c
			if err := s.receive(fd, c); err != nil {
				return err
			}
		} else if result != -int32(syscall.EINTR) && result != -int32(syscall.ECONNABORTED) {
			return syscall.Errno(-result)
		}
		if err := s.queue(s.accept, 0); err != nil {
			return err
		}
		return nil
	}
	fd := int(tag - 1)
	c := s.clients[fd]
	if result <= 0 {
		s.close(fd)
		return nil
	}
	if c.sending {
		c.sent += int(result)
		if c.sent < len(c.out) {
			err = s.send(fd, c)
		} else if c.closing {
			s.close(fd)
		} else {
			c.out = c.out[:0]
			err = s.respond(fd, c)
		}
	} else {
		c.used += int(result)
		err = s.respond(fd, c)
	}
	if err != nil {
		return err
	}
	return nil
}

func (s *server) run() error {
	defer func() {
		for fd := range s.clients {
			syscall.Close(fd)
		}
	}()
	if err := s.queue(s.accept, 0); err != nil {
		return err
	}
	for {
		// Checkpoints run between transactions, never during a response batch.
		for dbTick(); writesHeld(); dbTick() {
			time.Sleep(time.Millisecond)
		}
		event, err := s.ring.SubmitAndWaitCQEvents(1)
		if errors.Is(err, syscall.EINTR) {
			continue
		}
		if err != nil {
			return err
		}
		s.ready = s.ready[:0]
		// Drain only already-ready completions, bounded to avoid starving sends.
		for count := 0; ; count++ {
			tag, result := event.UserData, event.Res
			s.ring.SeenCQE(event)
			if err := s.complete(tag, result); err != nil {
				return err
			}
			if count == 255 {
				break
			}
			event, err = s.ring.PeekCQE()
			if errors.Is(err, syscall.EAGAIN) {
				break
			}
			if err != nil {
				return err
			}
		}
		// Reads may have seen this batch's writes, so replace every staged response
		// if commit fails. No staged bytes are queued to the kernel before commit.
		committed := dbCommit()
		for _, fd := range s.ready {
			c := s.clients[fd]
			if !committed {
				c.out = errorResp(c.out[:0], 500, "internal server error")
			}
			if err := s.send(fd, c); err != nil {
				return err
			}
		}
	}
}
