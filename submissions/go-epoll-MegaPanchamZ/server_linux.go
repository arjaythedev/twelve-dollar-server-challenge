//go:build linux

package main

import (
	"bytes"
	"fmt"
	"math/bits"
	"net"
	"runtime"
	"strconv"
	"syscall"
	"time"
	"unicode/utf8"
	"unsafe"
)

const (
	maxBodyChars       = 500
	maxEvents          = 512
	readBufSize        = 64 << 10
	idleGraceMs        = 1500
	maxAcceptsPerTick  = 128
	maxRequestsPerConn = 16
	stalledTimeoutMs   = 120_000
	idleTimeoutMs      = 125_000
	maxConnOutput      = 2 << 20
	maxQueuedOutput    = 32 << 20
	maxBufferedInput   = 32 << 20
	maxPoolBytes       = 8 << 20
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
	in              []byte
	out             []byte
	lastMs          int64
	prev            int32
	next            int32
	readyPrev       int32
	readyNext       int32
	generation      uint32
	pending         int32
	outPos          int
	open            bool
	closing         bool
	writable        bool
	sentContinue    bool
	continuePending bool
	queued          bool
}

type batchItem struct {
	fd         int32
	opIdx      int32
	generation uint32
}

// Only owned transport buffers enter this bounded pool. Database data and
// completed responses are never retained as cached results.
type bufferPool struct {
	free  [8][][]byte
	bytes int
}

func (p *bufferPool) get(n int) []byte {
	if n <= 0 {
		return nil
	}
	shift := max(10, bits.Len(uint(n-1)))
	if shift > 17 {
		return make([]byte, n)
	}
	i := shift - 10
	bucket := p.free[i]
	if len(bucket) > 0 {
		b := bucket[len(bucket)-1]
		bucket[len(bucket)-1] = nil
		p.free[i] = bucket[:len(bucket)-1]
		p.bytes -= cap(b)
		return b[:n]
	}
	return make([]byte, n, 1<<shift)
}

func (p *bufferPool) put(b []byte) {
	n := cap(b)
	if n < 1024 || n > 128<<10 || n&(n-1) != 0 || p.bytes+n > maxPoolBytes {
		return
	}
	i := bits.Len(uint(n)) - 11
	if len(p.free[i]) < 64 {
		p.free[i] = append(p.free[i], b[:0])
		p.bytes += n
	}
}

func (p *bufferPool) append(dst, src []byte) []byte {
	if len(src) <= cap(dst)-len(dst) {
		return append(dst, src...)
	}
	b := p.get(max(len(dst)+len(src), 2*cap(dst)))
	b = b[:len(dst)+len(src)]
	copy(b, dst)
	copy(b[len(dst):], src)
	p.put(dst)
	return b
}

type server struct {
	db          *db
	jwt         *verifier
	startedAt   time.Time
	nowSec      int64
	nowMs       int64
	nowMonoMs   int64
	lastSweepMs int64

	epfd          int
	lfd           int
	listening     bool
	conns         []conn
	openConns     int
	softMaxFDs    int
	hardMaxFDs    int
	lruHead       int32
	lruTail       int32
	readyHead     int32
	readyTail     int32
	readyCount    int
	queuedOutput  int
	bufferedInput int
	buffers       bufferPool

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
	if lim.Cur > 0 {
		nofile = int(min(lim.Cur, 1<<20))
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
		sysClose(lfd)
		return err
	}
	defer sysClose(epfd)
	defer sysClose(lfd)

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
		startedAt:   now,
		nowSec:      now.Unix(),
		nowMs:       now.UnixMilli(),
		epfd:        epfd,
		lfd:         lfd,
		conns:       make([]conn, min(nofile+64, 65536)),
		softMaxFDs:  softMax,
		hardMaxFDs:  hardMax,
		lruHead:     -1,
		lruTail:     -1,
		readyHead:   -1,
		readyTail:   -1,
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
		timeout := 10
		if s.readyCount > 0 {
			timeout = 0
		}
		n, errno := epollWait(epfd, s.events, timeout)
		if errno != 0 {
			if errno == syscall.EINTR {
				continue
			}
			return errno
		}
		s.updateTime()

		s.db.resetBatch()
		s.batch = s.batch[:0]
		if s.readyCount > 0 {
			s.drainReady()
		}

		for i := 0; i < n; i++ {
			ev := &s.events[i]
			fd := int(ev.Fd)
			if fd == lfd {
				s.acceptAll()
				continue
			}
			if fd < 0 || fd >= len(s.conns) {
				continue
			}
			c := &s.conns[fd]
			if !c.open || c.generation != uint32(ev.Pad) {
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
			if s.db.creates == len(s.batch) && len(s.batch) < s.openConns && len(s.batch) < 64 {
				s.coalesceCreates()
			}
			s.finishBatch()
		}
		if s.nowMonoMs-s.lastSweepMs >= 5000 {
			s.expireStalled()
			s.lastSweepMs = s.nowMonoMs
		}

		if !s.listening && s.openConns < s.hardMaxFDs {
			s.resumeListener()
		}
	}
}

func (s *server) coalesceCreates() {
	for empty := 0; len(s.batch) < s.openConns && len(s.batch) < 64 && empty < 24; {
		n, _ := epollWait(s.epfd, s.events, 0)
		if n <= 0 {
			empty++
			continue
		}
		empty = 0
		for i := 0; i < n; i++ {
			ev := &s.events[i]
			fd := int(ev.Fd)
			if fd == s.lfd {
				s.acceptAll()
				continue
			}
			if fd < 0 || fd >= len(s.conns) {
				continue
			}
			c := &s.conns[fd]
			if !c.open || c.generation != uint32(ev.Pad) {
				continue
			}
			if ev.Events&(syscall.EPOLLIN|syscall.EPOLLHUP|syscall.EPOLLERR) != 0 {
				s.readConn(fd, c)
			}
			if c.open && c.writable && ev.Events&syscall.EPOLLOUT != 0 {
				s.flushConn(fd, c)
			}
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
		sysClose(fd)
		return -1, err
	}
	if err := syscall.Listen(fd, 65535); err != nil {
		sysClose(fd)
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

func (s *server) updateTime() {
	now := time.Now()
	s.nowSec = now.Unix()
	s.nowMs = now.UnixMilli()
	s.nowMonoMs = now.Sub(s.startedAt).Milliseconds()
}

func (s *server) queueConn(fd int32) {
	c := &s.conns[fd]
	if c.queued || c.closing || !c.open {
		return
	}
	c.queued = true
	c.readyPrev, c.readyNext = s.readyTail, -1
	if s.readyTail >= 0 {
		s.conns[s.readyTail].readyNext = fd
	} else {
		s.readyHead = fd
	}
	s.readyTail = fd
	s.readyCount++
}

func (s *server) unqueueConn(fd int32) {
	c := &s.conns[fd]
	if !c.queued {
		return
	}
	if c.readyPrev >= 0 {
		s.conns[c.readyPrev].readyNext = c.readyNext
	} else {
		s.readyHead = c.readyNext
	}
	if c.readyNext >= 0 {
		s.conns[c.readyNext].readyPrev = c.readyPrev
	} else {
		s.readyTail = c.readyPrev
	}
	c.queued = false
	c.readyPrev, c.readyNext = -1, -1
	s.readyCount--
}

func (s *server) drainReady() {
	// A connection gets one turn per tick, even if it queues more work.
	for left := s.readyCount; left > 0 && s.db.hasRoom(0); left-- {
		fd := s.readyHead
		c := &s.conns[fd]
		s.unqueueConn(fd)
		if c.open && !c.writable && c.pending == 0 {
			s.parseConn(int(fd), c, c.in)
		}
	}
}

func (s *server) expireStalled() {
	for fd := s.lruHead; fd >= 0; {
		c := &s.conns[fd]
		next := c.next
		timeout := int64(idleTimeoutMs)
		if len(c.in) > 0 || c.writable {
			timeout = stalledTimeoutMs
		}
		if c.pending == 0 && s.nowMonoMs-c.lastMs >= timeout {
			s.closeConn(int(fd))
		}
		fd = next
	}
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
		if len(c.in) == 0 && c.pending == 0 && !c.queued && !c.writable && !c.closing {
			if force || (s.nowMonoMs-c.lastMs) >= idleGraceMs {
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
	for accepted := 0; accepted < maxAcceptsPerTick; {
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
		accepted++
		if fd >= len(s.conns) {
			s.conns = append(s.conns, make([]conn, max(fd+1-len(s.conns), len(s.conns)))...)
		}
		_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
		generation := s.conns[fd].generation + 1
		if generation == 0 {
			generation = 1
		}
		s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(fd), Pad: int32(generation)}
		if syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_ADD, fd, &s.ctl) != nil {
			sysClose(fd)
			continue
		}
		s.conns[fd] = conn{lastMs: s.nowMonoMs, prev: -1, next: -1, readyPrev: -1, readyNext: -1, generation: generation, open: true}
		s.openConns++
		s.lruAdd(int32(fd))
	}
}

func (s *server) readConn(fd int, c *conn) {
	if c.writable || c.closing || c.pending > 0 || c.queued {
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
	c.lastMs = s.nowMonoMs

	data := s.rbuf[:n]
	if len(c.in) > 0 {
		if n > maxBufferedInput-s.bufferedInput {
			s.closeConn(fd)
			return
		}
		c.in = s.buffers.append(c.in, data)
		s.bufferedInput += n
		data = c.in
	}

	s.parseConn(fd, c, data)
}

func (s *server) parseConn(fd int, c *conn, data []byte) {
	var req request
	pos := 0
	requests := 0
	deferred := false
	for pos < len(data) && !c.closing {
		if requests == maxRequestsPerConn || !s.db.hasRoom(0) {
			deferred = true
			break
		}
		used := parseRequest(data[pos:], &req)
		if used == 0 {
			if req.expectContinue && !c.sentContinue {
				c.continuePending = true
			}
			break
		}
		if used < 0 {
			opIdx := s.db.pushStatic(400, errBadRequest, true)
			if opIdx < 0 {
				deferred = true
				break
			}
			s.addBatch(fd, c, opIdx)
			c.closing = true
			pos = len(data)
			break
		}
		// Under FD pressure, retire connection gracefully after serving this response
		closeAfter := req.close || (s.openConns >= s.softMaxFDs && pos+used == len(data))
		opIdx := s.route(&req, closeAfter)
		if opIdx < 0 {
			deferred = true
			break
		}
		c.sentContinue = false
		c.continuePending = false
		pos += used
		requests++
		c.closing = closeAfter
		s.addBatch(fd, c, opIdx)
	}

	rest := data[pos:]
	if c.closing || len(rest) == 0 {
		if len(c.in) > 0 {
			s.bufferedInput -= len(c.in)
			s.buffers.put(c.in)
			c.in = nil
		}
	} else {
		if len(rest)-len(c.in) > maxBufferedInput-s.bufferedInput {
			s.closeConn(fd)
			return
		}
		s.bufferedInput += len(rest) - len(c.in)
		if len(c.in) > 0 {
			c.in = c.in[:copy(c.in, rest)]
		} else {
			c.in = s.buffers.get(len(rest))
			copy(c.in, rest)
		}
		if deferred {
			s.queueConn(int32(fd))
		}
	}
	if c.continuePending {
		s.sendContinue(fd, c)
	}
}

func (s *server) sendContinue(fd int, c *conn) {
	if c.open && c.continuePending && c.pending == 0 && !c.closing {
		c.continuePending = false
		c.sentContinue = true
		// Use the normal output queue to preserve order and handle short writes.
		s.sendConn(fd, c, continueResp)
	}
}

func (s *server) addBatch(fd int, c *conn, opIdx int) {
	c.pending++
	s.batch = append(s.batch, batchItem{fd: int32(fd), opIdx: int32(opIdx), generation: c.generation})
}

func (s *server) route(r *request, closeConn bool) int {
	path := r.path
	if string(r.method) == "GET" {
		if string(path) == "/feed" {
			return s.db.pushSimple(opFeed, 0, 0, closeConn)
		}
		if len(path) > 7 && string(path[:7]) == "/posts/" {
			if id, ok := parsePositiveID(path[7:]); ok {
				return s.db.pushSimple(opGetPost, id, 0, closeConn)
			}
		}
	} else if string(r.method) == "POST" {
		if len(path) > 12 && string(path[:7]) == "/posts/" && string(path[len(path)-5:]) == "/like" {
			idPart := path[7 : len(path)-5]
			if bytes.IndexByte(idPart, '/') < 0 {
				return s.handleLike(r, idPart, closeConn)
			}
		} else if string(path) == "/posts" {
			return s.handleCreate(r, closeConn)
		}
	}
	return s.routeSlow(r, closeConn)
}

func (s *server) routeSlow(r *request, closeConn bool) int {
	path := r.path
	if q := bytes.IndexByte(path, '?'); q >= 0 {
		path = path[:q]
	}
	if string(r.method) == "GET" {
		if string(path) == "/feed" {
			return s.db.pushSimple(opFeed, 0, 0, closeConn)
		}
		if len(path) > 7 && string(path[:7]) == "/posts/" {
			rest := path[7:]
			id, ok := parsePositiveID(rest)
			if !ok {
				if bytes.IndexByte(rest, '/') >= 0 {
					return s.db.pushStatic(404, errNotFound, closeConn)
				}
				return s.db.pushStatic(400, errInvalidID, closeConn)
			}
			return s.db.pushSimple(opGetPost, id, 0, closeConn)
		}
		if string(path) == "/health" {
			return s.db.pushSimple(opHealth, 0, 0, closeConn)
		}
	} else if string(r.method) == "POST" {
		if len(path) > 12 && string(path[:7]) == "/posts/" && string(path[len(path)-5:]) == "/like" {
			idPart := path[7 : len(path)-5]
			if bytes.IndexByte(idPart, '/') < 0 {
				return s.handleLike(r, idPart, closeConn)
			}
		} else if string(path) == "/posts" {
			return s.handleCreate(r, closeConn)
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
	for first := 0; first < len(s.batch); {
		done := s.db.execBatch(first, s.nowMs, s.nowMonoMs/1000)
		if done <= 0 || done > len(s.batch)-first {
			panic("database batch made no progress")
		}
		// Commit and checkpoint I/O may have taken time since readiness was sampled.
		for _, item := range s.batch[first : first+done] {
			fd := int(item.fd)
			c := &s.conns[fd]
			if !c.open || c.generation != item.generation {
				continue
			}
			c.pending--
			op := &s.db.ops[item.opIdx]
			resp := s.db.outBuf[op.resp_off : op.resp_off+op.resp_len]
			if len(c.out) == 0 {
				n, errno := sysWrite(fd, resp)
				if errno == 0 && n == len(resp) {
					c.lastMs = s.nowMonoMs
					if c.closing && c.pending == 0 {
						s.closeConn(fd)
					} else if c.continuePending {
						s.sendContinue(fd, c)
					}
					continue
				}
				s.sendConnPartial(fd, c, resp, n, errno)
			} else {
				s.sendConnQueued(fd, c, resp)
			}
			if c.continuePending {
				s.sendContinue(fd, c)
			}
		}
		first += done
		s.db.checkpointIfDue()
	}
}

func (s *server) sendConn(fd int, c *conn, data []byte) {
	if len(c.out) == 0 {
		n, errno := sysWrite(fd, data)
		if errno == 0 && n == len(data) {
			c.lastMs = s.nowMonoMs
			if c.closing && c.pending == 0 {
				s.closeConn(fd)
			}
			return
		}
		s.sendConnPartial(fd, c, data, n, errno)
		return
	}
	s.sendConnQueued(fd, c, data)
}

func (s *server) sendConnQueued(fd int, c *conn, data []byte) {
	if len(data) > maxConnOutput-(len(c.out)-c.outPos) || len(data) > maxQueuedOutput-s.queuedOutput {
		s.closeConn(fd)
		return
	}
	if c.outPos > 0 {
		c.out = c.out[:copy(c.out, c.out[c.outPos:])]
		c.outPos = 0
	}
	c.out = s.buffers.append(c.out, data)
	s.queuedOutput += len(data)
}

func (s *server) sendConnPartial(fd int, c *conn, data []byte, n int, errno syscall.Errno) {
	if errno == syscall.EAGAIN || errno == syscall.EINTR {
		n, errno = 0, 0
	}
	if errno != 0 {
		s.closeConn(fd)
		return
	}
	if len(data)-n > maxConnOutput || len(data)-n > maxQueuedOutput-s.queuedOutput {
		s.closeConn(fd)
		return
	}
	if n > 0 {
		c.lastMs = s.nowMonoMs
	}
	c.out = s.buffers.get(len(data) - n)
	copy(c.out, data[n:])
	s.queuedOutput += len(c.out)
	c.writable = true
	s.ctl = syscall.EpollEvent{Events: syscall.EPOLLOUT, Fd: int32(fd), Pad: int32(c.generation)}
	if syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_MOD, fd, &s.ctl) != nil {
		s.closeConn(fd)
	}
}

func (s *server) flushConn(fd int, c *conn) {
	n, errno := sysWrite(fd, c.out[c.outPos:])
	if errno == syscall.EAGAIN || errno == syscall.EINTR {
		return
	}
	if errno != 0 {
		s.closeConn(fd)
		return
	}
	c.outPos += n
	s.queuedOutput -= n
	if n > 0 {
		c.lastMs = s.nowMonoMs
	}
	if c.outPos < len(c.out) {
		return
	}
	s.buffers.put(c.out)
	c.out = nil
	c.outPos = 0
	c.writable = false
	if c.closing && c.pending == 0 {
		s.closeConn(fd)
		return
	}
	s.ctl = syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(fd), Pad: int32(c.generation)}
	if syscall.EpollCtl(s.epfd, syscall.EPOLL_CTL_MOD, fd, &s.ctl) != nil {
		s.closeConn(fd)
		return
	}
	if len(c.in) > 0 {
		s.queueConn(int32(fd))
	}
}

func (s *server) closeConn(fd int) {
	c := &s.conns[fd]
	if !c.open {
		return
	}
	s.unqueueConn(int32(fd))
	s.lruRemove(int32(fd))
	sysClose(fd)
	s.queuedOutput -= len(c.out) - c.outPos
	s.bufferedInput -= len(c.in)
	s.buffers.put(c.in)
	s.buffers.put(c.out)
	*c = conn{prev: -1, next: -1, readyPrev: -1, readyNext: -1, generation: c.generation}
	s.openConns--
}

func epollWait(epfd int, events []syscall.EpollEvent, timeout int) (int, syscall.Errno) {
	n, _, e := syscall.RawSyscall6(
		syscall.SYS_EPOLL_PWAIT,
		uintptr(epfd),
		uintptr(unsafe.Pointer(unsafe.SliceData(events))),
		uintptr(len(events)),
		uintptr(timeout), 0, 0,
	)
	return int(n), e
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
