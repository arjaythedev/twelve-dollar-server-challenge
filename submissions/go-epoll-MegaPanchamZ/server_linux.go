//go:build linux

package main

import (
	"bytes"
	"fmt"
	"net"
	"runtime"
	"strconv"
	"syscall"
	"time"
	"unicode/utf8"
	"unsafe"
)

const (
	maxBodyChars = 500
	maxEvents    = 512
	readBufSize  = 64 << 10
	idleGraceMs  = 1500
)

var (
	errNotFound     = []byte(`{"error":"not found"}`)
	errInvalidID    = []byte(`{"error":"invalid post id"}`)
	errMissingAuth  = []byte(`{"error":"missing bearer token"}`)
	errInvalidToken = []byte(`{"error":"invalid or expired token"}`)
	errTokenPayload = []byte(`{"error":"invalid token payload"}`)
	errMalformed    = []byte(`{"error":"malformed JSON body"}`)
	errBodyRequired = []byte(`{"error":"body is required"}`)
	errBodyTooLong  = []byte(`{"error":"body must be at most 500 characters"}`)
	errBadRequest   = []byte(`{"error":"bad request"}`)

	continueResp = []byte("HTTP/1.1 100 Continue\r\n\r\n")

	pathFeed   = []byte("/feed")
	pathHealth = []byte("/health")
	pathPosts  = []byte("/posts")
	prefPosts  = []byte("/posts/")
	suffLike   = []byte("/like")
)

type conn struct {
	in           []byte
	out          []byte
	lastMs       int64
	prev         int32
	next         int32
	open         bool
	closing      bool
	writable     bool
	sentContinue bool
}

type batchItem struct {
	fd    int32
	opIdx int32
}

type server struct {
	db         *db
	jwt        *verifier
	startedSec int64
	nowSec     int64
	nowMs      int64

	epfd       int
	lfd        int
	listening  bool
	conns      []conn
	openConns  int
	softMaxFDs int
	hardMaxFDs int
	lruHead    int32
	lruTail    int32

	events      []syscall.EpollEvent
	ctl         syscall.EpollEvent
	rbuf        []byte
	textScratch []byte
	keyScratch  []byte
	batch       []batchItem
}

func serve(host, portStr string, d *db, secret string) error {
	runtime.LockOSThread()

	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err == nil {
		if lim.Cur < lim.Max {
			lim.Cur = lim.Max
			_ = syscall.Setrlimit(syscall.RLIMIT_NOFILE, &lim)
		}
	}
	nofile := 65535
	if lim.Cur > 0 && lim.Cur < 1<<20 {
		nofile = int(lim.Cur)
	}

	port, err := strconv.Atoi(portStr)
	if err != nil {
		return fmt.Errorf("invalid PORT %q: %w", portStr, err)
	}
	lfd, err := listenSocket(host, port)
	if err != nil {
		return err
	}
	epfd, err := syscall.EpollCreate1(syscall.EPOLL_CLOEXEC)
	if err != nil {
		return err
	}

	softMax := max(nofile-4096, nofile*7/8)
	hardMax := max(nofile-256, nofile*15/16)
	if nofile <= 1024 {
		softMax = nofile - 128
		hardMax = nofile - 64
	}

	now := time.Now()
	s := &server{
		db:          d,
		jwt:         newVerifier(secret),
		startedSec:  now.Unix(),
		nowSec:      now.Unix(),
		nowMs:       now.UnixMilli(),
		epfd:        epfd,
		lfd:         lfd,
		conns:       make([]conn, nofile+64),
		softMaxFDs:  softMax,
		hardMaxFDs:  hardMax,
		lruHead:     -1,
		lruTail:     -1,
		events:      make([]syscall.EpollEvent, maxEvents),
		rbuf:        make([]byte, readBufSize),
		textScratch: make([]byte, 0, 16<<10),
		keyScratch:  make([]byte, 0, 256),
		batch:       make([]batchItem, 0, maxBatchOps),
	}
	for i := range s.conns {
		s.conns[i].prev = -1
		s.conns[i].next = -1
	}

	s.resumeListener()

	for {
		n, err := syscall.EpollWait(epfd, s.events, -1)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return err
		}
		now := time.Now()
		s.nowSec = now.Unix()
		s.nowMs = now.UnixMilli()

		s.db.resetBatch()
		s.batch = s.batch[:0]

		for i := 0; i < n; i++ {
			ev := &s.events[i]
			fd := int(ev.Fd)
			if fd == lfd {
				s.acceptAll()
				continue
			}
			c := &s.conns[fd]
			if !c.open {
				continue
			}
			if ev.Events&(syscall.EPOLLIN|syscall.EPOLLHUP|syscall.EPOLLERR) != 0 {
				s.readConn(fd, c)
			}
			if c.open && c.writable && ev.Events&syscall.EPOLLOUT != 0 {
				s.flushConn(fd, c)
			}
		}

		if len(s.batch) > 0 {
			s.finishBatch()
		}

		if !s.listening && s.openConns < s.hardMaxFDs {
			s.resumeListener()
		}
	}
}

func listenSocket(host string, port int) (int, error) {
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
	_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
	if err := syscall.Bind(fd, sa); err != nil {
		return -1, err
	}
	if err := syscall.Listen(fd, 65535); err != nil {
		return -1, err
	}
	return fd, nil
}

func (s *server) pauseListener() {
	if s.listening {
		_ = syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_DEL, s.lfd, nil)
		s.listening = false
	}
}

func (s *server) resumeListener() {
	s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(s.lfd)}
	if syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_ADD, s.lfd, &s.ctl) == nil {
		s.listening = true
	}
}

func (s *server) lruAdd(fd int32) {
	c := &s.conns[fd]
	c.prev = s.lruTail
	c.next = -1
	if s.lruTail >= 0 {
		s.conns[s.lruTail].next = fd
	} else {
		s.lruHead = fd
	}
	s.lruTail = fd
}

func (s *server) lruRemove(fd int32) {
	c := &s.conns[fd]
	if c.prev >= 0 {
		s.conns[c.prev].next = c.next
	} else if s.lruHead == fd {
		s.lruHead = c.next
	}
	if c.next >= 0 {
		s.conns[c.next].prev = c.prev
	} else if s.lruTail == fd {
		s.lruTail = c.prev
	}
	c.prev = -1
	c.next = -1
}

func (s *server) lruTouch(fd int32) {
	if s.lruTail == fd {
		return
	}
	s.lruRemove(fd)
	s.lruAdd(fd)
}

func (s *server) evictIdle(maxCount int, force bool) int {
	evicted := 0
	scanned := 0
	limit := s.openConns
	cur := s.lruHead
	for cur >= 0 && evicted < maxCount && scanned < limit {
		scanned++
		c := &s.conns[cur]
		next := c.next
		if len(c.in) == 0 && !c.writable && !c.closing {
			if force || (s.nowMs-c.lastMs) >= idleGraceMs {
				s.closeConn(int(cur))
				evicted++
			} else {
				s.lruTouch(cur)
			}
		}
		cur = next
	}
	return evicted
}

func (s *server) acceptAll() {
	for {
		if s.openConns >= s.hardMaxFDs {
			if s.evictIdle(64, false) == 0 && s.evictIdle(16, true) == 0 {
				s.pauseListener()
				return
			}
		}
		r, _, errno := syscall.RawSyscall6(syscall.SYS_ACCEPT4, uintptr(s.lfd), 0, 0,
			syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0, 0)
		switch errno {
		case 0:
		case syscall.EAGAIN:
			return
		case syscall.EINTR, syscall.ECONNABORTED, syscall.EPROTO:
			continue
		case syscall.EMFILE, syscall.ENFILE, syscall.ENOBUFS, syscall.ENOMEM:
			if s.evictIdle(64, false) == 0 && s.evictIdle(16, true) == 0 {
				s.pauseListener()
			}
			return
		default:
			return
		}
		fd := int(r)
		if fd >= len(s.conns) {
			sysClose(fd)
			continue
		}
		_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
		s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(fd)}
		if syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_ADD, fd, &s.ctl) != nil {
			sysClose(fd)
			continue
		}
		s.conns[fd] = conn{lastMs: s.nowMs, prev: -1, next: -1, open: true}
		s.openConns++
		s.lruAdd(int32(fd))
	}
}

func (s *server) readConn(fd int, c *conn) {
	if c.writable {
		return
	}
	n, errno := sysRead(fd, s.rbuf)
	if errno == syscall.EAGAIN || errno == syscall.EINTR {
		return
	}
	if errno != 0 || n <= 0 {
		s.closeConn(fd)
		return
	}
	c.lastMs = s.nowMs

	data := s.rbuf[:n]
	if len(c.in) > 0 {
		c.in = append(c.in, data...)
		data = c.in
	}

	var req request
	pos := 0
	for pos < len(data) && !c.closing && s.db.nOps < maxBatchOps {
		used := parseRequest(data[pos:], &req)
		if used == 0 {
			if req.expectContinue && !c.sentContinue {
				_, _ = sysWrite(fd, continueResp)
				c.sentContinue = true
			}
			break
		}
		if used < 0 {
			opIdx := s.db.pushStatic(400, errBadRequest, true)
			s.batch = append(s.batch, batchItem{fd: int32(fd), opIdx: int32(opIdx)})
			c.closing = true
			pos = len(data)
			break
		}
		c.sentContinue = false
		pos += used

		// Under FD pressure, retire connection gracefully after serving this response
		closeAfter := req.close || (s.openConns >= s.softMaxFDs && pos == len(data))
		if closeAfter {
			c.closing = true
		}

		opIdx := s.route(&req, closeAfter)
		s.batch = append(s.batch, batchItem{fd: int32(fd), opIdx: int32(opIdx)})
	}

	rest := data[pos:]
	if c.closing || len(rest) == 0 {
		c.in = nil
	} else if len(c.in) > 0 {
		c.in = c.in[:copy(c.in, rest)]
	} else {
		c.in = append([]byte(nil), rest...)
	}
}

func (s *server) route(r *request, closeConn bool) int {
	isGet := len(r.method) == 3 && string(r.method) == "GET"
	isPost := len(r.method) == 4 && string(r.method) == "POST"

	switch {
	case isGet && bytes.Equal(r.path, pathFeed):
		return s.db.pushSimple(opFeed, 0, 0, closeConn)
	case isGet && bytes.Equal(r.path, pathHealth):
		return s.db.pushSimple(opHealth, 0, 0, closeConn)
	case isPost && bytes.Equal(r.path, pathPosts):
		return s.handleCreate(r, closeConn)
	case bytes.HasPrefix(r.path, prefPosts):
		rest := r.path[len(prefPosts):]
		if isGet && bytes.IndexByte(rest, '/') < 0 {
			id, ok := parsePositiveID(rest)
			if !ok {
				return s.db.pushStatic(400, errInvalidID, closeConn)
			}
			return s.db.pushSimple(opGetPost, id, 0, closeConn)
		}
		if isPost && bytes.HasSuffix(rest, suffLike) {
			idPart := rest[:len(rest)-len(suffLike)]
			if bytes.IndexByte(idPart, '/') < 0 {
				return s.handleLike(r, idPart, closeConn)
			}
		}
	}
	return s.db.pushStatic(404, errNotFound, closeConn)
}

func (s *server) authenticate(r *request, closeConn bool) (int64, []byte, int, bool) {
	uid, user, res := s.jwt.verify(r.auth, r.hasAuth, s.nowSec)
	switch res {
	case authOK:
		return uid, user, 0, true
	case authMissing:
		return 0, nil, s.db.pushStatic(401, errMissingAuth, closeConn), false
	case authInvalid:
		return 0, nil, s.db.pushStatic(401, errInvalidToken, closeConn), false
	default:
		return 0, nil, s.db.pushStatic(401, errTokenPayload, closeConn), false
	}
}

func (s *server) handleCreate(r *request, closeConn bool) int {
	uid, user, errOp, ok := s.authenticate(r, closeConn)
	if !ok {
		return errOp
	}
	obj, isObj := newObjectWalker(r.body)
	if !isObj {
		if !validJSON(r.body) {
			return s.db.pushStatic(400, errMalformed, closeConn)
		}
		return s.db.pushStatic(400, errBodyRequired, closeConn)
	}
	var text []byte
	for m, more := obj.next(); more; m, more = obj.next() {
		if string(m.key) == `"body"` || (bytes.IndexByte(m.key, '\\') >= 0 && string(unescape(s.keyScratch[:0], m.key)) == "body") {
			text = nil
			if m.val[0] == '"' {
				text = unescapeRef(s.textScratch[:0], m.val)
			}
		}
	}
	if obj.err {
		return s.db.pushStatic(400, errMalformed, closeConn)
	}
	text = trimSpace(text)
	if len(text) == 0 {
		return s.db.pushStatic(400, errBodyRequired, closeConn)
	}
	if utf8.RuneCount(text) > maxBodyChars {
		return s.db.pushStatic(400, errBodyTooLong, closeConn)
	}
	return s.db.pushCreate(uid, text, user, closeConn)
}

func (s *server) handleLike(r *request, idPart []byte, closeConn bool) int {
	uid, _, errOp, ok := s.authenticate(r, closeConn)
	if !ok {
		return errOp
	}
	postID, ok := parsePositiveID(idPart)
	if !ok {
		return s.db.pushStatic(400, errInvalidID, closeConn)
	}
	return s.db.pushSimple(opLikePost, uid, postID, closeConn)
}

func (s *server) finishBatch() {
	uptime := s.nowSec - s.startedSec
	s.db.execBatch(uptime)

	ops := s.db.ops
	outBuf := s.db.outBuf
	for i := range s.batch {
		item := s.batch[i]
		fd := int(item.fd)
		c := &s.conns[fd]
		if !c.open {
			continue
		}
		op := &ops[item.opIdx]
		resp := outBuf[op.resp_off : op.resp_off+op.resp_len]
		s.sendConn(fd, c, resp)
	}

	s.db.checkpointIfDue()
}

func (s *server) sendConn(fd int, c *conn, data []byte) {
	if len(c.out) > 0 {
		c.out = append(c.out, data...)
		return
	}
	n, errno := sysWrite(fd, data)
	if errno == syscall.EAGAIN || errno == syscall.EINTR {
		n, errno = 0, 0
	}
	if errno != 0 {
		s.closeConn(fd)
		return
	}
	if n == len(data) {
		if c.closing {
			s.closeConn(fd)
		}
		return
	}
	c.out = append(c.out[:0], data[n:]...)
	c.writable = true
	s.ctl = syscall.EpollEvent{Events: syscall.EPOLLOUT, Fd: int32(fd)}
	if syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_MOD, fd, &s.ctl) != nil {
		s.closeConn(fd)
	}
}

func (s *server) flushConn(fd int, c *conn) {
	n, errno := sysWrite(fd, c.out)
	if errno == syscall.EAGAIN || errno == syscall.EINTR {
		return
	}
	if errno != 0 {
		s.closeConn(fd)
		return
	}
	c.out = c.out[n:]
	if len(c.out) > 0 {
		return
	}
	c.out = nil
	c.writable = false
	if c.closing {
		s.closeConn(fd)
		return
	}
	s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(fd)}
	if syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_MOD, fd, &s.ctl) != nil {
		s.closeConn(fd)
	}
}

func (s *server) closeConn(fd int) {
	c := &s.conns[fd]
	if !c.open {
		return
	}
	s.lruRemove(int32(fd))
	sysClose(fd)
	*c = conn{prev: -1, next: -1}
	s.openConns--
}

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
