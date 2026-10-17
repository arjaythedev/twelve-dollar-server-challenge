package main

import (
	"log"
	"syscall"
)

// The epoll backend: the fallback when io_uring isn't available. Every connection is a file
// descriptor, so it holds at most RLIMIT_NOFILE (65,535 on the benchmark box) connections.
// Input is read into one shared buffer and only a partial request is copied into the
// connection; output only stays on the connection when the socket is full.
type epollIO struct {
	s      *server
	epfd   int
	spare  int // held in reserve so that at the descriptor limit we can still accept and close
	events []syscall.EpollEvent
	rbuf   []byte
}

func newEpollIO(s *server) (*epollIO, error) {
	e := &epollIO{s: s, events: make([]syscall.EpollEvent, 1024), rbuf: make([]byte, 64<<10)}
	var err error
	if e.epfd, err = syscall.EpollCreate1(syscall.EPOLL_CLOEXEC); err != nil {
		return nil, err
	}
	for _, fd := range []int{s.lfd, s.checkpointPipe} {
		ev := syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(fd)}
		if err = syscall.EpollCtl(e.epfd, syscall.EPOLL_CTL_ADD, fd, &ev); err != nil {
			return nil, err
		}
	}
	s.conns = make([]conn, 4096)
	e.spare, _ = syscall.Open("/dev/null", syscall.O_RDONLY|syscall.O_CLOEXEC, 0)
	return e, nil
}

func (e *epollIO) name() string { return "epoll" }

func (e *epollIO) run() {
	s := e.s
	for {
		n, err := syscall.EpollWait(e.epfd, e.events, -1)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			log.Fatalf("epoll_wait: %v", err)
		}
		for i := 0; i < n; i++ {
			ev := e.events[i]
			fd := int(ev.Fd)
			if fd == s.lfd {
				e.accept()
				continue
			}
			if fd == s.checkpointPipe {
				s.checkpointDone()
				continue
			}
			c := &s.conns[fd]
			if !c.open || c.gen != ev.Pad {
				continue // closed earlier in this batch
			}
			if ev.Events&syscall.EPOLLOUT != 0 {
				e.flush(c)
			}
			if c.open && ev.Events&(syscall.EPOLLIN|syscall.EPOLLHUP|syscall.EPOLLERR) != 0 {
				e.readable(c)
			}
		}
		s.commitPending()
	}
}

func (e *epollIO) accept() {
	s := e.s
	for {
		fd, _, err := syscall.Accept4(s.lfd, syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC)
		if err != nil {
			switch err {
			case syscall.EINTR, syscall.ECONNABORTED:
				continue
			case syscall.EMFILE, syscall.ENFILE:
				// Out of descriptors: shed one waiting client rather than spin on the listener.
				if e.spare >= 0 {
					syscall.Close(e.spare)
					if fd, _, err := syscall.Accept4(s.lfd, syscall.SOCK_CLOEXEC); err == nil {
						syscall.Close(fd)
					}
					e.spare, _ = syscall.Open("/dev/null", syscall.O_RDONLY|syscall.O_CLOEXEC, 0)
				}
			}
			return
		}
		if fd >= len(s.conns) {
			grown := make([]conn, max(fd+1, 2*len(s.conns)))
			copy(grown, s.conns)
			s.conns = grown
		}
		c := &s.conns[fd]
		*c = conn{id: int32(fd), gen: s.newGen(), open: true}
		ev := syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(fd), Pad: c.gen}
		if syscall.EpollCtl(e.epfd, syscall.EPOLL_CTL_ADD, fd, &ev) != nil {
			e.close(c)
		}
	}
}

func (e *epollIO) close(c *conn) {
	if !c.open {
		return
	}
	syscall.Close(int(c.id)) // also removes it from the epoll set
	*c = conn{id: c.id, gen: c.gen}
}

func (e *epollIO) outPending(c *conn) bool { return len(c.out) > 0 }

func (e *epollIO) setPollOut(c *conn, on bool) {
	ev := syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: c.id, Pad: c.gen}
	if on {
		ev.Events |= syscall.EPOLLOUT
	}
	c.pollOut = on
	syscall.EpollCtl(e.epfd, syscall.EPOLL_CTL_MOD, int(c.id), &ev)
}

func (e *epollIO) readable(c *conn) {
	n, err := syscall.Read(int(c.id), e.rbuf)
	if err == syscall.EAGAIN || err == syscall.EINTR {
		return
	}
	if n <= 0 || err != nil {
		e.close(c)
		return
	}
	e.s.receive(c, e.rbuf[:n])
}

// send writes out to the socket, keeping whatever doesn't fit until the socket is writable.
func (e *epollIO) send(c *conn, out []byte) {
	if len(c.out) > 0 {
		c.out = append(c.out, out...)
		return
	}
	for len(out) > 0 {
		n, err := syscall.Write(int(c.id), out)
		if err == syscall.EINTR {
			continue
		}
		if err == syscall.EAGAIN {
			break
		}
		if err != nil {
			e.close(c)
			return
		}
		out = out[n:]
	}
	if len(out) > 0 {
		c.out = append([]byte(nil), out...)
		e.setPollOut(c, true)
	}
}

func (e *epollIO) flush(c *conn) {
	for len(c.out) > 0 {
		n, err := syscall.Write(int(c.id), c.out)
		if err == syscall.EINTR {
			continue
		}
		if err == syscall.EAGAIN {
			return
		}
		if err != nil {
			e.close(c)
			return
		}
		c.out = c.out[n:]
	}
	c.out = nil
	if c.pollOut {
		e.setPollOut(c, false)
	}
	if c.closeAfter && !c.blocked {
		e.close(c)
	}
}
