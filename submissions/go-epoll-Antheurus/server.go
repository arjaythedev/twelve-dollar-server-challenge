package main

import (
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"syscall"
	"time"
	"unsafe"
)

const (
	// A connection idle this long may be closed when the fd table runs short. Any client that
	// waits at least this long between requests has already noticed the FIN and dials anew, so
	// the close never races a request in flight.
	idleReapMs  = 2000
	idleMaxMs   = 120_000
	tickMs      = 250
	maxEvents   = 1024
	readBufSize = 64 << 10
	fdHeadroom  = 6000
	// maxConnTable bounds the per-process connection table when the fd limit is far above the
	// 65535 the benchmark service gets.
	maxConnTable = 1 << 18

	// The frozen syscall package defines SO_REUSEPORT for linux/arm64 but not linux/amd64.
	soReusePort = 0xf
)

type conn struct {
	in           []byte // a partial or not-yet-served request; nil whenever none is pending
	out          []byte // an unsent response tail; nil whenever the socket took everything
	lastMs       int64
	open         bool
	closing      bool
	writable     bool // waiting on EPOLLOUT; reading is paused until out drains
	waitWrite    bool // a queued write is not committed yet; later requests wait in in
	sentContinue bool
}

// server is a single-threaded epoll loop: every socket, every query and every commit happens
// on the goroutine that called run, which is locked to its OS thread.
type server struct {
	epfd, lfd int
	listening bool
	conns     []conn
	open      int
	high      int
	fillAt    int // past this many sockets the worker asks for the next one to take new connections
	ctrl      *controller
	rep       *reporter
	sweepPos  int
	events    []syscall.EpollEvent
	ctl       syscall.EpollEvent
	rbuf      []byte
	wbuf      []byte
	nowMs     int64
	dateSec   int64
	date      []byte
	app       *app
}

func newServer(host string, port int, app *app) (*server, error) {
	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err != nil {
		return nil, err
	}
	nofile := min(int(lim.Cur), maxConnTable)
	lfd, err := listen(host, port)
	if err != nil {
		return nil, err
	}
	epfd, err := syscall.EpollCreate1(syscall.EPOLL_CLOEXEC)
	if err != nil {
		return nil, err
	}
	s := &server{
		epfd:  epfd,
		lfd:   lfd,
		conns: make([]conn, nofile),
		high:  max(nofile*3/4, nofile-fdHeadroom),

		events: make([]syscall.EpollEvent, maxEvents),
		rbuf:   make([]byte, readBufSize),
		wbuf:   make([]byte, 0, 256<<10),
		date:   make([]byte, 0, 64),
		app:    app,
	}
	s.fillAt = s.high - s.high/20
	s.resumeListener()
	s.clock()
	log.Printf("pid %d listening on %s:%d, fd limit %d, fills to %d connections", os.Getpid(), host, port, nofile, s.fillAt)
	return s, nil
}

func listen(host string, port int) (int, error) {
	ip := net.ParseIP(host)
	if ip == nil {
		ips, err := net.LookupIP(host)
		if err != nil || len(ips) == 0 {
			return -1, fmt.Errorf("resolve HOST %q: %v", host, err)
		}
		ip = ips[0]
	}
	var sa syscall.Sockaddr
	family := syscall.AF_INET
	if v4 := ip.To4(); v4 != nil {
		a := &syscall.SockaddrInet4{Port: port}
		copy(a.Addr[:], v4)
		sa = a
	} else {
		family = syscall.AF_INET6
		a := &syscall.SockaddrInet6{Port: port}
		copy(a.Addr[:], ip.To16())
		sa = a
	}
	fd, err := syscall.Socket(family, syscall.SOCK_STREAM|syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return -1, err
	}
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		return -1, err
	}
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, soReusePort, 1); err != nil {
		return -1, err
	}
	if err := syscall.Bind(fd, sa); err != nil {
		return -1, fmt.Errorf("bind %s:%d: %w", host, port, err)
	}
	if err := syscall.Listen(fd, 65535); err != nil {
		return -1, err
	}
	return fd, nil
}

func (s *server) run() {
	nextTick := s.nowMs + tickMs
	for {
		timeout := tickMs
		if len(s.app.writes) > 0 {
			timeout = 1
		}
		n, err := syscall.EpollWait(s.epfd, s.events, timeout)
		if err != nil && err != syscall.EINTR {
			log.Fatalf("epoll_wait: %v", err)
		}
		s.clock()
		for i := 0; i < n; i++ {
			ev := &s.events[i]
			switch fd := int(ev.Fd); {
			case fd == s.lfd:
				s.acceptAll()
			case s.ctrl != nil && s.ctrl.owns(fd):
				s.ctrl.handle(fd)
			default:
				s.handle(int(ev.Fd), ev.Events)
			}
		}
		for len(s.app.writes) > 0 && s.app.commitWrites(s) {
		}
		if s.nowMs >= nextTick {
			s.reap(s.open-s.high, idleReapMs)
			if !s.listening && s.open < s.high {
				s.resumeListener()
			}
			if s.ctrl != nil {
				s.ctrl.tick()
			}
			nextTick = s.nowMs + tickMs
		}
	}
}

const dateLayout = "Mon, 02 Jan 2006 15:04:05 GMT"

func (s *server) clock() {
	now := time.Now()
	s.nowMs = now.UnixMilli()
	if sec := now.Unix(); sec != s.dateSec {
		s.dateSec = sec
		s.date = append(s.date[:0], "Date: "...)
		s.date = now.UTC().AppendFormat(s.date, dateLayout)
		s.date = append(s.date, "\r\n"...)
	}
}

func (s *server) acceptAll() {
	for {
		r, _, errno := syscall.RawSyscall6(syscall.SYS_ACCEPT4, uintptr(s.lfd), 0, 0,
			syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0, 0)
		switch errno {
		case 0:
		case syscall.EAGAIN:
			return
		case syscall.EINTR, syscall.ECONNABORTED, syscall.EPROTO:
			continue
		case syscall.EMFILE, syscall.ENFILE, syscall.ENOBUFS, syscall.ENOMEM:
			if s.reap(1024, idleReapMs) == 0 {
				s.pauseListener()
			}
			return
		default:
			log.Printf("accept: %v", errno)
			return
		}
		fd := int(r)
		if fd >= len(s.conns) {
			sysClose(fd)
			continue
		}
		syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
		s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN | syscall.EPOLLRDHUP, Fd: int32(fd)}
		if err := syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_ADD, fd, &s.ctl); err != nil {
			sysClose(fd)
			continue
		}
		s.conns[fd] = conn{lastMs: s.nowMs, open: true}
		s.open++
		if s.open == s.fillAt {
			s.filled()
		}
	}
}

// filled hands new connections to the next worker the moment this one reaches its fill mark;
// waiting for the housekeeping tick let a burst push a worker hundreds of sockets past it.
func (s *server) filled() {
	switch {
	case s.ctrl != nil && s.ctrl.active == 0:
		s.ctrl.advance()
	case s.rep != nil && !s.rep.full:
		s.rep.full = true
		s.rep.send(msgFull)
	}
}

func (s *server) pauseListener() {
	if s.listening {
		syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_DEL, s.lfd, nil)
		s.listening = false
	}
}

func (s *server) resumeListener() {
	s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(s.lfd)}
	if err := syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_ADD, s.lfd, &s.ctl); err == nil {
		s.listening = true
	}
}

func (s *server) watch(fd int) {
	s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(fd)}
	syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_ADD, fd, &s.ctl)
}

func (s *server) unwatch(fd int) {
	syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_DEL, fd, nil)
}

func (s *server) setInterest(fd int, events uint32) {
	s.ctl = syscall.EpollEvent{Events: events, Fd: int32(fd)}
	syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_MOD, fd, &s.ctl)
}

// reap closes up to want connections idle for at least minIdle ms, plus any idle past idleMaxMs.
// It walks from where the last pressure reap stopped, so no fd range is always the one closed.
func (s *server) reap(want int, minIdle int64) int {
	closed, n := 0, len(s.conns)
	last := -1
	for k := 0; k < n; k++ {
		fd := (s.sweepPos + k) % n
		c := &s.conns[fd]
		if !c.open || c.writable || c.waitWrite {
			continue
		}
		idle := s.nowMs - c.lastMs
		if idle >= idleMaxMs || (closed < want && idle >= minIdle && len(c.in) == 0) {
			s.closeConn(fd)
			closed++
			last = fd
		}
	}
	if last >= 0 && want > 0 {
		s.sweepPos = (last + 1) % n
	}
	return closed
}

func (s *server) closeConn(fd int) {
	c := &s.conns[fd]
	if !c.open {
		return
	}
	sysClose(fd)
	*c = conn{}
	s.open--
}

func (s *server) handle(fd int, events uint32) {
	c := &s.conns[fd]
	if !c.open || c.waitWrite {
		return
	}
	if c.writable {
		if events&(syscall.EPOLLOUT|syscall.EPOLLERR|syscall.EPOLLHUP) == 0 || !s.flush(fd, c) {
			return
		}
		s.drained(fd, c)
		return
	}
	n, errno := sysRead(fd, s.rbuf)
	if errno == syscall.EAGAIN {
		return
	}
	if errno != 0 || n <= 0 {
		s.closeConn(fd)
		return
	}
	c.lastMs = s.nowMs
	if len(c.in) > 0 {
		c.in = append(c.in, s.rbuf[:n]...)
		s.process(fd, c, c.in, true)
	} else {
		s.process(fd, c, s.rbuf[:n], false)
	}
}

// drained runs once a connection has no output pending: it closes, or serves what is buffered.
func (s *server) drained(fd int, c *conn) {
	if c.writable {
		c.writable = false
		s.setInterest(fd, syscall.EPOLLIN|syscall.EPOLLRDHUP)
	}
	switch {
	case c.closing:
		s.closeConn(fd)
	case len(c.in) > 0:
		s.process(fd, c, c.in, true)
	}
}

var (
	continueResp = []byte("HTTP/1.1 100 Continue\r\n\r\n")
	errBadReq    = []byte(`{"error":"bad request"}`)
	errTooLarge  = []byte(`{"error":"request too large"}`)
	errChunked   = []byte(`{"error":"chunked request bodies are not supported"}`)
)

// process serves the complete requests at the front of data. owned says data is c.in itself.
// A request that writes is queued for the batch's commit, and nothing after it on the same
// connection is served until its response has gone out, so responses keep request order.
func (s *server) process(fd int, c *conn, data []byte, owned bool) {
	s.wbuf = s.wbuf[:0]
	pos := 0
	var r request
	for pos < len(data) && !c.closing && !c.waitWrite {
		switch parseRequest(data[pos:], &r) {
		case parseIncomplete:
			if r.expectContinue && !c.sentContinue {
				s.wbuf = append(s.wbuf, continueResp...)
				c.sentContinue = true
			}
			goto done
		case parseOK:
			c.sentContinue = false
			if s.app.serve(s, &r, fd) {
				c.waitWrite = true
			}
			pos += r.size
			if !r.keepAlive {
				c.closing = true
			}
		case parseTooLarge:
			s.respond(413, errTooLarge, false, false)
			c.closing = true
		case parseUnsupported:
			s.respond(501, errChunked, false, false)
			c.closing = true
		default:
			s.respond(400, errBadReq, false, false)
			c.closing = true
		}
	}
done:
	rest := data[pos:]
	if c.closing {
		rest = nil
	}
	switch {
	case len(rest) == 0:
		c.in = nil
	case owned:
		c.in = c.in[:copy(c.in, rest)]
	default:
		c.in = append([]byte(nil), rest...)
	}
	if len(s.wbuf) > 0 {
		s.send(fd, c, s.wbuf)
	} else if c.closing && !c.waitWrite {
		s.closeConn(fd)
	}
}

// send writes data, parking whatever the socket does not take in a buffer of the connection's
// own; a connection already parked appends behind what it has.
func (s *server) send(fd int, c *conn, data []byte) {
	if c.writable {
		c.out = append(c.out, data...)
		return
	}
	n, errno := sysWrite(fd, data)
	if errno == syscall.EAGAIN {
		n, errno = 0, 0
	}
	if errno != 0 {
		s.closeConn(fd)
		return
	}
	if n == len(data) {
		if c.closing && !c.waitWrite {
			s.closeConn(fd)
		}
		return
	}
	c.out = append([]byte(nil), data[n:]...)
	c.writable = true
	s.setInterest(fd, syscall.EPOLLOUT|syscall.EPOLLRDHUP)
}

// flush continues a parked write; true means c.out is now empty.
func (s *server) flush(fd int, c *conn) bool {
	n, errno := sysWrite(fd, c.out)
	if errno == syscall.EAGAIN {
		return false
	}
	if errno != 0 {
		s.closeConn(fd)
		return false
	}
	c.out = c.out[n:]
	if len(c.out) > 0 {
		return false
	}
	c.out = nil
	return true
}

// deliverWrite sends a committed write's response and resumes the connection behind it.
func (s *server) deliverWrite(fd int, resp []byte) {
	c := &s.conns[fd]
	if !c.open || !c.waitWrite {
		return
	}
	c.waitWrite = false
	s.send(fd, c, resp)
	if c.open && !c.writable {
		s.drained(fd, c)
	}
}

func statusLine(status int) string {
	switch status {
	case 200:
		return "HTTP/1.1 200 OK\r\n"
	case 201:
		return "HTTP/1.1 201 Created\r\n"
	case 400:
		return "HTTP/1.1 400 Bad Request\r\n"
	case 401:
		return "HTTP/1.1 401 Unauthorized\r\n"
	case 404:
		return "HTTP/1.1 404 Not Found\r\n"
	case 413:
		return "HTTP/1.1 413 Content Too Large\r\n"
	case 501:
		return "HTTP/1.1 501 Not Implemented\r\n"
	case 503:
		return "HTTP/1.1 503 Service Unavailable\r\n"
	}
	return "HTTP/1.1 500 Internal Server Error\r\n"
}

func (s *server) respond(status int, body []byte, keepAlive, http10 bool) {
	s.wbuf = appendResponse(s.wbuf, s.date, status, body, keepAlive, http10)
}

func appendResponse(b, date []byte, status int, body []byte, keepAlive, http10 bool) []byte {
	b = append(b, statusLine(status)...)
	b = append(b, "Content-Type: application/json\r\nContent-Length: "...)
	b = strconv.AppendInt(b, int64(len(body)), 10)
	b = append(b, "\r\n"...)
	b = append(b, date...)
	switch {
	case !keepAlive:
		b = append(b, "Connection: close\r\n"...)
	case http10:
		b = append(b, "Connection: keep-alive\r\n"...)
	}
	b = append(b, "\r\n"...)
	return append(b, body...)
}

// The sockets are non-blocking, so read, write, accept and close never park the thread: raw
// syscalls skip the scheduler hand-off a blocking syscall needs.
func sysRead(fd int, b []byte) (int, syscall.Errno) {
	n, _, e := syscall.RawSyscall(syscall.SYS_READ, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(b))), uintptr(len(b)))
	return int(n), e
}

func sysWrite(fd int, b []byte) (int, syscall.Errno) {
	n, _, e := syscall.RawSyscall(syscall.SYS_WRITE, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(b))), uintptr(len(b)))
	return int(n), e
}

func sysClose(fd int) {
	syscall.RawSyscall(syscall.SYS_CLOSE, uintptr(fd), 0, 0)
}
