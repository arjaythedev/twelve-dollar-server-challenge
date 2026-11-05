package main

import (
	"bytes"
	"fmt"
	"net"
	"strconv"
	"syscall"
	"time"
)

const (
	readBufSize   = 64 << 10
	maxHeaderSize = 16 << 10
	maxBodySize   = 1 << 20
	idleTimeout   = 120 // seconds. SPEC: keep idle connections open for at least 65 s
)

// conn is one client connection. Requests are parsed straight out of the loop's read buffer, so
// an idle connection holds no buffers at all.
type conn struct {
	fd        int
	in        []byte // start of a request that hasn't fully arrived yet
	pending   []byte // response bytes the socket didn't accept yet
	closing   bool   // close once pending is written
	continued bool   // sent "100 Continue" for the request in `in`
	last      int64  // unix time of the last read
}

// request points into the read buffer; it is only valid while it is being handled.
type request struct {
	method, path, auth, body []byte
	close                    bool // close the connection after the response
	keepAlive10              bool // HTTP/1.0 client asked for keep-alive
	expect                   bool // Expect: 100-continue
}

// loop is the epoll event loop. It owns the listener, every connection and the SQLite connection,
// and runs each request inline from parse to write: on one vCPU, handing work to other threads
// would only add context switches.
type loop struct {
	ep, lfd int
	paused  bool    // out of file descriptors: not accepting until a connection closes
	conns   []*conn // indexed by fd
	buf     []byte  // read buffer shared by all connections
	out     []byte  // responses for the connection being served
	body    []byte  // response body being built
	ev      syscall.EpollEvent
	now     int64
	date    []byte // "Date: ...\r\n" for the current second
	api
}

func listen(host, port string) (int, error) {
	ip := net.ParseIP(host)
	if ip == nil {
		ips, err := net.LookupIP(host)
		if err != nil || len(ips) == 0 {
			return -1, fmt.Errorf("resolve HOST %q: %v", host, err)
		}
		ip = ips[0]
	}
	p, err := strconv.Atoi(port)
	if err != nil {
		return -1, fmt.Errorf("bad PORT %q", port)
	}
	family, sa := syscall.AF_INET6, syscall.Sockaddr(&syscall.SockaddrInet6{Port: p, Addr: [16]byte(ip.To16())})
	if ip4 := ip.To4(); ip4 != nil {
		family, sa = syscall.AF_INET, &syscall.SockaddrInet4{Port: p, Addr: [4]byte(ip4)}
	}
	fd, err := syscall.Socket(family, syscall.SOCK_STREAM|syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return -1, err
	}
	syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
	if err := syscall.Bind(fd, sa); err != nil {
		return -1, fmt.Errorf("bind %s:%s: %v", host, port, err)
	}
	if err := syscall.Listen(fd, 65535); err != nil { // the kernel caps it at somaxconn
		return -1, err
	}
	return fd, nil
}

func newLoop(lfd int, a api) (*loop, error) {
	ep, err := syscall.EpollCreate1(syscall.EPOLL_CLOEXEC)
	if err != nil {
		return nil, err
	}
	l := &loop{ep: ep, lfd: lfd, conns: make([]*conn, 1024), buf: make([]byte, readBufSize), api: a}
	if err := l.ctl(syscall.EPOLL_CTL_ADD, lfd, syscall.EPOLLIN); err != nil {
		return nil, err
	}
	l.tick()
	return l, nil
}

func (l *loop) ctl(op, fd int, events uint32) error {
	l.ev = syscall.EpollEvent{Events: events, Fd: int32(fd)}
	return syscall.EpollCtl(l.ep, op, fd, &l.ev)
}

func (l *loop) run() {
	events := make([]syscall.EpollEvent, 512)
	lastSweep := l.now
	for {
		n, err := syscall.EpollWait(l.ep, events, 1000)
		if err != nil && err != syscall.EINTR {
			panic(fmt.Sprintf("epoll_wait: %v", err))
		}
		l.tick()
		for i := 0; i < n; i++ {
			fd := int(events[i].Fd)
			if fd == l.lfd {
				l.accept()
				continue
			}
			c := l.conns[fd]
			if c == nil {
				continue
			}
			if c.pending != nil {
				l.flush(c) // only EPOLLOUT is armed while a response is pending
			} else {
				l.read(c)
			}
		}
		if l.now-lastSweep >= 10 {
			lastSweep = l.now
			l.sweep()
		}
	}
}

func (l *loop) tick() {
	now := time.Now()
	if s := now.Unix(); s != l.now {
		l.now = s
		l.date = now.UTC().AppendFormat(append(l.date[:0], "Date: "...), "Mon, 02 Jan 2006 15:04:05 GMT")
		l.date = append(l.date, "\r\n"...)
	}
}

func (l *loop) accept() {
	for {
		fd, _, err := syscall.Accept4(l.lfd, syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC)
		if err != nil {
			switch err {
			case syscall.EINTR, syscall.ECONNABORTED:
				continue
			case syscall.EMFILE, syscall.ENFILE:
				// Out of descriptors (LimitNOFILE). The listener would stay readable and spin the
				// loop, so stop watching it until a connection closes; new clients wait in the
				// kernel's accept queue meanwhile.
				if l.ctl(syscall.EPOLL_CTL_MOD, l.lfd, 0) == nil {
					l.paused = true
				}
			}
			return // EAGAIN: no more pending connections
		}
		syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
		if err := l.ctl(syscall.EPOLL_CTL_ADD, fd, syscall.EPOLLIN); err != nil {
			syscall.Close(fd)
			continue
		}
		for fd >= len(l.conns) {
			l.conns = append(l.conns, make([]*conn, len(l.conns))...)
		}
		l.conns[fd] = &conn{fd: fd, last: l.now}
	}
}

func (l *loop) close(c *conn) {
	syscall.Close(c.fd) // also removes it from epoll
	l.conns[c.fd] = nil
	if l.paused && l.ctl(syscall.EPOLL_CTL_MOD, l.lfd, syscall.EPOLLIN) == nil {
		l.paused = false
	}
}

// sweep closes connections that have been idle (or stuck writing) for longer than idleTimeout.
func (l *loop) sweep() {
	for _, c := range l.conns {
		if c != nil && l.now-c.last > idleTimeout {
			l.close(c)
		}
	}
}

func (l *loop) read(c *conn) {
	n, err := syscall.Read(c.fd, l.buf)
	if n <= 0 {
		if err != syscall.EAGAIN && err != syscall.EINTR {
			l.close(c) // EOF or error
		}
		return
	}
	c.last = l.now
	data, buffered := l.buf[:n], len(c.in) > 0
	if buffered {
		c.in = append(c.in, data...)
		data = c.in
	}
	l.out = l.out[:0]
	used, closing := l.serve(c, data)
	switch rest := data[used:]; {
	case closing || len(rest) == 0:
		c.in = nil
	case buffered:
		c.in = append(c.in[:0], rest...) // overlapping copy to the front is fine
	default:
		c.in = append([]byte(nil), rest...)
	}
	if len(l.out) > 0 {
		l.send(c, l.out, closing)
	} else if closing {
		l.close(c)
	}
}

// serve handles every complete request in data (pipelining works) and appends the responses to
// l.out. It returns how many bytes it used and whether the connection should close.
func (l *loop) serve(c *conn, data []byte) (used int, closing bool) {
	for used < len(data) {
		var r request
		n, status := parse(data[used:], &r)
		if status != 0 {
			l.protocolError(status)
			return used, true
		}
		if n == 0 { // incomplete
			if r.expect && !c.continued {
				l.out = append(l.out, "HTTP/1.1 100 Continue\r\n\r\n"...)
				c.continued = true
			}
			return used, false
		}
		c.continued = false
		used += n
		l.handle(&r)
		if r.close {
			return used, true
		}
	}
	return used, false
}

func (l *loop) send(c *conn, b []byte, closing bool) {
	n, err := syscall.Write(c.fd, b)
	if err != nil {
		if err != syscall.EAGAIN && err != syscall.EINTR {
			l.close(c)
			return
		}
		n = 0
	}
	if n == len(b) {
		if closing {
			l.close(c)
		}
		return
	}
	// The socket buffer is full: keep the rest and stop reading until it's written.
	c.pending = append([]byte(nil), b[n:]...)
	c.closing = closing
	if l.ctl(syscall.EPOLL_CTL_MOD, c.fd, syscall.EPOLLOUT) != nil {
		l.close(c)
	}
}

func (l *loop) flush(c *conn) {
	n, err := syscall.Write(c.fd, c.pending)
	if err != nil {
		if err != syscall.EAGAIN && err != syscall.EINTR {
			l.close(c)
		}
		return
	}
	if c.pending = c.pending[n:]; len(c.pending) > 0 {
		return
	}
	c.pending = nil
	if c.closing || l.ctl(syscall.EPOLL_CTL_MOD, c.fd, syscall.EPOLLIN) != nil {
		l.close(c)
	}
}

var crlfcrlf = []byte("\r\n\r\n")

// parse reads one HTTP/1.x request from the start of b. It returns the request's length, or 0 if
// it hasn't fully arrived, or a non-zero status for a request it can't serve.
func parse(b []byte, r *request) (n, status int) {
	start := 0
	for start+1 < len(b) && b[start] == '\r' && b[start+1] == '\n' { // RFC 9112 2.2: skip empty lines
		start += 2
	}
	end := bytes.Index(b[start:], crlfcrlf)
	if end < 0 {
		if len(b) > maxHeaderSize {
			return 0, 431
		}
		return 0, 0
	}
	head := b[start : start+end+2] // request line and headers, each ending in CRLF
	bodyStart := start + end + 4
	if bodyStart > maxHeaderSize {
		return 0, 431
	}

	eol := bytes.IndexByte(head, '\n')
	if eol < 1 || head[eol-1] != '\r' {
		return 0, 400
	}
	line := head[:eol-1]
	sp1, sp2 := bytes.IndexByte(line, ' '), bytes.LastIndexByte(line, ' ')
	if sp1 <= 0 || sp2 <= sp1+1 {
		return 0, 400
	}
	r.method, r.path = line[:sp1], line[sp1+1:sp2]
	if q := bytes.IndexByte(r.path, '?'); q >= 0 {
		r.path = r.path[:q]
	}
	http10 := false
	switch string(line[sp2+1:]) {
	case "HTTP/1.1":
	case "HTTP/1.0":
		http10 = true
	default:
		return 0, 400
	}

	length, keepAlive := -1, false
	for rest := head[eol+1:]; len(rest) > 0; {
		i := bytes.IndexByte(rest, '\n')
		h := rest[:i]
		rest = rest[i+1:]
		if len(h) > 0 && h[len(h)-1] == '\r' {
			h = h[:len(h)-1]
		}
		colon := bytes.IndexByte(h, ':')
		if colon <= 0 {
			return 0, 400
		}
		name, value := h[:colon], bytes.Trim(h[colon+1:], " \t")
		switch len(name) {
		case 6:
			if foldEq(name, "expect") && foldEq(value, "100-continue") {
				r.expect = true
			}
		case 10:
			if foldEq(name, "connection") {
				if foldContains(value, "close") {
					r.close = true
				} else if foldContains(value, "keep-alive") {
					keepAlive = true
				}
			}
		case 13:
			if foldEq(name, "authorization") {
				r.auth = value
			}
		case 14:
			if foldEq(name, "content-length") {
				v, ok := parseDigits(value)
				if !ok || (length >= 0 && v != length) {
					return 0, 400
				}
				length = v
			}
		case 17:
			if foldEq(name, "transfer-encoding") {
				return 0, 501
			}
		}
	}
	if http10 && !r.close {
		r.close, r.keepAlive10 = !keepAlive, keepAlive
	}

	length = max(length, 0)
	if length > maxBodySize {
		return 0, 413
	}
	if len(b) < bodyStart+length {
		return 0, 0
	}
	r.body = b[bodyStart : bodyStart+length]
	return bodyStart + length, 0
}

func parseDigits(b []byte) (int, bool) {
	if len(b) == 0 || len(b) > 9 {
		return 0, false
	}
	v := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int(c-'0')
	}
	return v, true
}

// foldEq reports whether b equals the lower-case ASCII string s, ignoring case.
func foldEq(b []byte, s string) bool {
	if len(b) != len(s) {
		return false
	}
	for i := range len(b) {
		if b[i]|0x20 != s[i] && b[i] != s[i] {
			return false
		}
	}
	return true
}

func foldContains(b []byte, s string) bool {
	for i := 0; i+len(s) <= len(b); i++ {
		if foldEq(b[i:i+len(s)], s) {
			return true
		}
	}
	return false
}

func statusLine(code int) string {
	switch code {
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
	case 405:
		return "HTTP/1.1 405 Method Not Allowed\r\n"
	case 413:
		return "HTTP/1.1 413 Content Too Large\r\n"
	case 431:
		return "HTTP/1.1 431 Request Header Fields Too Large\r\n"
	case 501:
		return "HTTP/1.1 501 Not Implemented\r\n"
	case 503:
		return "HTTP/1.1 503 Service Unavailable\r\n"
	default:
		return "HTTP/1.1 500 Internal Server Error\r\n"
	}
}

// respond appends a complete response with a JSON body to l.out.
func (l *loop) respond(r *request, code int, body []byte) {
	l.out = append(l.out, statusLine(code)...)
	l.out = append(l.out, l.date...)
	l.out = append(l.out, "Content-Type: application/json\r\nContent-Length: "...)
	l.out = strconv.AppendInt(l.out, int64(len(body)), 10)
	if r.close {
		l.out = append(l.out, "\r\nConnection: close"...)
	} else if r.keepAlive10 {
		l.out = append(l.out, "\r\nConnection: keep-alive"...)
	}
	l.out = append(l.out, "\r\n\r\n"...)
	l.out = append(l.out, body...)
}

// protocolError answers a request that couldn't be parsed, then the connection closes.
func (l *loop) protocolError(code int) {
	msg := "bad request"
	switch code {
	case 413:
		msg = "request body too large"
	case 431:
		msg = "request headers too large"
	case 501:
		msg = "chunked request bodies are not supported"
	}
	l.body = appendError(l.body[:0], msg)
	l.respond(&request{close: true}, code, l.body)
}
