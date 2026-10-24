//go:build linux

package main

import (
	"fmt"
	"log"
	"net"
	"strconv"
	"syscall"
)

// conn holds only what must survive between reads, so an idle connection costs under 100 bytes.
type conn struct {
	fd      int
	in      []byte // an incomplete request carried over to the next read
	out     []byte // response bytes the socket has not taken yet
	closing bool
}

type server struct {
	app          *app
	epfd         int
	lfd          int
	acceptPaused bool
	conns        []*conn // indexed by file descriptor
	rbuf         []byte
	wbuf         []byte    // every response of the current batch
	batch        []pending // which part of wbuf goes to which connection
}

// pending is one connection's responses in wbuf, waiting for the batch commit.
type pending struct {
	c                 *conn
	start, end, count int
}

var (
	badRequest        = []byte(`{"error":"bad request"}`)
	internalErrorBody = []byte(`{"error":"internal server error"}`)
)

// serve runs one epoll loop that accepts, reads, handles and writes every connection.
func serve(host, port string, a *app) error {
	ip := net.ParseIP(host).To4()
	if ip == nil {
		return fmt.Errorf("HOST %q is not an IPv4 address", host)
	}
	p, err := strconv.Atoi(port)
	if err != nil {
		return fmt.Errorf("PORT %q: %w", port, err)
	}
	lfd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return err
	}
	if err := syscall.SetsockoptInt(lfd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		return err
	}
	sa := &syscall.SockaddrInet4{Port: p}
	copy(sa.Addr[:], ip)
	if err := syscall.Bind(lfd, sa); err != nil {
		return err
	}
	if err := syscall.Listen(lfd, 4096); err != nil {
		return err
	}
	epfd, err := syscall.EpollCreate1(syscall.EPOLL_CLOEXEC)
	if err != nil {
		return err
	}
	s := &server{app: a, epfd: epfd, lfd: lfd, rbuf: make([]byte, 64<<10)}
	if err := s.control(syscall.EPOLL_CTL_ADD, lfd, syscall.EPOLLIN); err != nil {
		return err
	}
	events := make([]syscall.EpollEvent, 256)
	for {
		n, err := syscall.EpollWait(epfd, events, -1)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return err
		}
		for _, ev := range events[:n] {
			fd := int(ev.Fd)
			if fd == lfd {
				s.accept()
				continue
			}
			c := s.conns[fd]
			if c != nil && ev.Events&(syscall.EPOLLIN|syscall.EPOLLHUP|syscall.EPOLLERR) != 0 {
				s.read(c)
			}
			if c != nil && s.conns[fd] == c && ev.Events&syscall.EPOLLOUT != 0 {
				s.flush(c)
			}
		}
		s.finishBatch()
	}
}

// finishBatch commits the batch's writes and only then sends its responses, so no write is
// acknowledged before it is durable and no read is answered with data that may be rolled back.
func (s *server) finishBatch() {
	if err := s.app.db.commitBatch(); err != nil {
		log.Print(err)
		for i := range s.batch {
			p := &s.batch[i]
			p.start = len(s.wbuf)
			for j := range p.count {
				s.wbuf = appendResponse(s.wbuf, 500, internalErrorBody, p.c.closing && j == p.count-1)
			}
			p.end = len(s.wbuf)
		}
	}
	for _, p := range s.batch {
		if s.conns[p.c.fd] == p.c {
			s.send(p.c, s.wbuf[p.start:p.end])
		}
	}
	s.batch, s.wbuf = s.batch[:0], s.wbuf[:0]
	// Checkpointing here keeps its page copies and fsyncs off the batch's response time.
	if err := s.app.db.checkpointIfDue(); err != nil {
		log.Print(err)
	}
}

func (s *server) control(op, fd int, events uint32) error {
	return syscall.EpollCtl(s.epfd, op, fd, &syscall.EpollEvent{Events: events, Fd: int32(fd)})
}

func (s *server) accept() {
	for {
		fd, _, err := syscall.Accept4(s.lfd, syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC)
		switch err {
		case nil:
		case syscall.EINTR, syscall.ECONNABORTED:
			continue
		case syscall.EMFILE, syscall.ENFILE:
			// Out of descriptors: the listener stays readable, so stop watching it until a connection closes.
			s.acceptPaused = s.control(syscall.EPOLL_CTL_DEL, s.lfd, 0) == nil
			return
		default:
			return
		}
		_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
		if err := s.control(syscall.EPOLL_CTL_ADD, fd, syscall.EPOLLIN); err != nil {
			_ = syscall.Close(fd)
			continue
		}
		for fd >= len(s.conns) {
			s.conns = append(s.conns, nil)
		}
		s.conns[fd] = &conn{fd: fd}
	}
}

func (s *server) read(c *conn) {
	n, err := syscall.Read(c.fd, s.rbuf)
	if err == syscall.EAGAIN || err == syscall.EINTR {
		return
	}
	if n <= 0 {
		s.close(c)
		return
	}
	data := s.rbuf[:n]
	if len(c.in) > 0 {
		c.in = append(c.in, data...)
		data = c.in
	}
	start, count := len(s.wbuf), 0
	for len(data) > 0 && !c.closing {
		r, used := parseRequest(data)
		if used == 0 {
			break
		}
		count++
		if used < 0 {
			s.wbuf = appendResponse(s.wbuf, 400, badRequest, true)
			c.closing = true
			break
		}
		s.wbuf = s.app.handle(&r, s.wbuf)
		data = data[used:]
		c.closing = r.close
	}
	if len(data) > 0 && !c.closing {
		c.in = append(c.in[:0], data...)
	} else {
		c.in = nil
	}
	if count > 0 {
		s.batch = append(s.batch, pending{c, start, len(s.wbuf), count})
	}
}

func (s *server) send(c *conn, out []byte) {
	if len(c.out) > 0 {
		c.out = append(c.out, out...)
		return
	}
	if len(out) == 0 {
		return
	}
	n, err := syscall.Write(c.fd, out)
	if err != nil && err != syscall.EAGAIN && err != syscall.EINTR {
		s.close(c)
		return
	}
	n = max(n, 0)
	if n == len(out) {
		if c.closing {
			s.close(c)
		}
		return
	}
	// Stop reading until the client takes what it already asked for.
	c.out = append(c.out[:0], out[n:]...)
	if err := s.control(syscall.EPOLL_CTL_MOD, c.fd, syscall.EPOLLOUT); err != nil {
		s.close(c)
	}
}

func (s *server) flush(c *conn) {
	n, err := syscall.Write(c.fd, c.out)
	if err != nil && err != syscall.EAGAIN && err != syscall.EINTR {
		s.close(c)
		return
	}
	c.out = c.out[max(n, 0):]
	if len(c.out) > 0 {
		return
	}
	c.out = nil
	if c.closing {
		s.close(c)
		return
	}
	if err := s.control(syscall.EPOLL_CTL_MOD, c.fd, syscall.EPOLLIN); err != nil {
		s.close(c)
	}
}

func (s *server) close(c *conn) {
	_ = syscall.Close(c.fd)
	s.conns[c.fd] = nil
	if s.acceptPaused && s.control(syscall.EPOLL_CTL_ADD, s.lfd, syscall.EPOLLIN) == nil {
		s.acceptPaused = false
	}
}
