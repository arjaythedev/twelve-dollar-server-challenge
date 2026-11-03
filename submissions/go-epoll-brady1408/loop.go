package main

import (
	"log"
	"net"
	"os"
	"runtime"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	idleTimeout     = 75 // seconds
	sweepInterval   = 10 * time.Second
	readBufSize     = 4096
	maxReadBuf      = 32768 // headers (8K) + body (16K) always fit
	maxConnBudget   = 60000
	fdReserve       = 128
	maxEvents       = 1024
	waitCap         = 10 // ms; longest the loop keeps the only P in epoll_wait
	maxReqsPerRound = 32 // pipelined requests one connection may run per loop round
	poolLimit       = 2048
	listenBacklog   = 4096
	walLimitBytes   = 64 << 20        // WAL file size at which every bulk pass ends in a restart (see checkpointTick)
	checkpointEvery = 2 * time.Second // how often the loop asks the checkpointer for a bulk pass
	pauseLimit      = 5 * time.Second // never keep writes parked longer than this (checkpointer stuck)
	evRead          = unix.EPOLLIN | unix.EPOLLRDHUP | unix.EPOLLET
	evReadWrite     = evRead | unix.EPOLLOUT
)

const (
	connOpen       uint8 = 1 << iota // slot is live
	connWantWrite                    // EPOLLOUT armed; reads paused until wbuf drains
	connCloseAfter                   // close once wbuf drains
	connHeld                         // wbuf[holdFrom:] waits for commitBatch
	connSent100                      // 100 Continue already sent for the pending request
	connPaused                       // hit maxReqsPerRound; queued on l.again for the next round
	connDraining                     // final response sent and SHUT_WR done; discarding input until the peer's FIN
	connPeerFin                      // EPOLLRDHUP seen: read past a short read until read returns 0
)

// conn is one client socket. Buffers are nil while the connection is idle.
type conn struct {
	fd       int32
	flags    uint8
	lastSeen uint32 // seconds since loop start
	holdFrom int32  // first byte of wbuf that awaits commit
	rbuf     []byte
	wbuf     []byte
}

type loop struct {
	srv          *server
	epfd         int
	lfd          int
	stopfd       int // eventfd; any write wakes the loop and shuts it down
	wakefd       int // eventfd; the checkpointer pokes it so a done signal is seen at once
	spare        int // /dev/null, closed and reopened to drain accepts when fds run out
	conns        []*conn
	open         int
	budget       int
	start        time.Time
	nowSec       uint32
	nowUnix      int64
	events       []unix.EpollEvent
	dirty        []*conn
	held         []*conn
	again        []*conn // paused conns with buffered input, serviced next round
	againNx      []*conn
	ckptReq      chan<- struct{} // ask the checkpointer for a pass (cap 1)
	ckptDone     <-chan struct{} // the checkpointer finished a pass (cap 1)
	ckptState    uint8
	nextCkpt     time.Time
	walPath      string
	writesPaused bool // a finishing pass is running: POSTs are parked, GETs served
	pausedSince  time.Time
	writeWait    []*conn // conns parked on a write
	rpool        [][]byte
	wpool        [][]byte
	reads        uint64        // read syscalls on client sockets
	done         chan struct{} // closed when run returns
}

func newLoop(srv *server, host string, port int) (*loop, error) {
	var rl unix.Rlimit
	if err := unix.Getrlimit(unix.RLIMIT_NOFILE, &rl); err != nil {
		return nil, err
	}
	if rl.Cur < rl.Max {
		rl.Cur = rl.Max
		unix.Setrlimit(unix.RLIMIT_NOFILE, &rl) // best effort; the budget below uses whatever we got
		unix.Getrlimit(unix.RLIMIT_NOFILE, &rl)
	}
	budget := maxConnBudget
	if int(rl.Cur)-fdReserve < budget {
		budget = int(rl.Cur) - fdReserve
	}
	lfd, err := listen(host, port)
	if err != nil {
		return nil, err
	}
	epfd, err := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
	if err != nil {
		return nil, err
	}
	stopfd, err := unix.Eventfd(0, unix.EFD_CLOEXEC|unix.EFD_NONBLOCK)
	if err != nil {
		return nil, err
	}
	wakefd, err := unix.Eventfd(0, unix.EFD_CLOEXEC|unix.EFD_NONBLOCK)
	if err != nil {
		return nil, err
	}
	spare, err := unix.Open("/dev/null", unix.O_RDONLY|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	l := &loop{
		srv: srv, epfd: epfd, lfd: lfd, stopfd: stopfd, wakefd: wakefd, spare: spare,
		conns: make([]*conn, rl.Cur), budget: budget, start: time.Now(),
		events: make([]unix.EpollEvent, maxEvents), done: make(chan struct{}),
	}
	// Listener is level-triggered so a round we could not fully accept is retried.
	if err := unix.EpollCtl(epfd, unix.EPOLL_CTL_ADD, lfd, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(lfd)}); err != nil {
		return nil, err
	}
	if err := unix.EpollCtl(epfd, unix.EPOLL_CTL_ADD, stopfd, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(stopfd)}); err != nil {
		return nil, err
	}
	if err := unix.EpollCtl(epfd, unix.EPOLL_CTL_ADD, wakefd, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(wakefd)}); err != nil {
		return nil, err
	}
	return l, nil
}

func listen(host string, port int) (int, error) {
	ip := net.ParseIP(host).To4()
	if ip == nil {
		return 0, &net.AddrError{Err: "HOST must be an IPv4 address", Addr: host}
	}
	fd, err := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		return 0, err
	}
	unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEADDR, 1)
	sa := &unix.SockaddrInet4{Port: port}
	copy(sa.Addr[:], ip)
	if err := unix.Bind(fd, sa); err != nil {
		unix.Close(fd)
		return 0, err
	}
	if err := unix.Listen(fd, listenBacklog); err != nil {
		unix.Close(fd)
		return 0, err
	}
	return fd, nil
}

// port returns the bound port (useful when PORT=0 in tests).
func (l *loop) port() int {
	sa, err := unix.Getsockname(l.lfd)
	if err != nil {
		return 0
	}
	return sa.(*unix.SockaddrInet4).Port
}

// stop wakes the loop and makes run return. Safe from any goroutine.
func (l *loop) stop() {
	var one [8]byte
	one[0] = 1
	unix.Write(l.stopfd, one[:])
}

// wake makes the loop run one round now. Safe from any goroutine.
func (l *loop) wake() {
	var one [8]byte
	one[0] = 1
	unix.Write(l.wakefd, one[:])
}

func (l *loop) run() {
	runtime.LockOSThread()
	defer close(l.done)
	nextSweep := time.Now().Add(sweepInterval)
	for {
		timeout := int(time.Until(nextSweep) / time.Millisecond)
		if timeout < 0 || len(l.again) > 0 {
			timeout = 0
		}
		if l.writesPaused && timeout > 100 {
			timeout = 100 // so an overrunning finishing pass is noticed
		}
		n := l.wait(timeout)
		l.nowSec = uint32(time.Since(l.start) / time.Second)
		l.nowUnix = time.Now().Unix()
		for i := 0; i < n; i++ {
			ev := &l.events[i]
			switch fd := int(ev.Fd); fd {
			case l.lfd:
				l.acceptAll()
			case l.stopfd:
				l.shutdown()
				return
			case l.wakefd:
				var buf [8]byte
				unix.Read(l.wakefd, buf[:])
			default:
				if c := l.conns[fd]; c != nil {
					l.handleEvent(c, ev.Events)
				}
			}
		}
		l.serviceAgain()
		l.finishBatch()
		l.checkpointTick()
		if now := time.Now(); !now.Before(nextSweep) {
			l.sweep()
			nextSweep = now.Add(sweepInterval)
		}
	}
}

// wait blocks in epoll_pwait through a raw syscall, so the scheduler never
// sees the loop as blocked. Through unix.EpollWait, sysmon retakes the P from a
// loop blocked for more than 20 µs and the loop buys it back with a futex
// hand-off on every return: thousands of extra syscalls a second under load,
// for nothing, since nobody else wants this P (main gives the checkpointer and
// the runtime a second one). The wait is still capped at waitCap and the loop
// yields when it expires, so that with a single P the checkpointer, the garbage
// collector and signal delivery never wait longer than that while the loop is
// idle.
func (l *loop) wait(timeout int) int {
	if timeout < 0 || timeout > waitCap {
		timeout = waitCap
	}
	n, _, errno := unix.RawSyscall6(unix.SYS_EPOLL_PWAIT, uintptr(l.epfd),
		uintptr(unsafe.Pointer(&l.events[0])), uintptr(len(l.events)), uintptr(timeout), 0, 0)
	switch errno {
	case 0:
		if n == 0 && timeout == waitCap {
			runtime.Gosched()
		}
	case unix.EINTR:
		// Usually the scheduler asking for a preemption point, so give it one:
		// back in the loop the next call honours it. Retrying here would spin
		// in the syscall while a stop-the-world waits on this P.
		return 0
	default:
		log.Fatalf("epoll_wait: %v", errno)
	}
	return int(n)
}

// handleEvent services one epoll event on a client socket.
func (l *loop) handleEvent(c *conn, events uint32) {
	if events&(unix.EPOLLERR|unix.EPOLLHUP) != 0 {
		l.closeConn(c)
		return
	}
	if events&unix.EPOLLOUT != 0 {
		l.flush(c)
	}
	if events&unix.EPOLLRDHUP != 0 {
		c.flags |= connPeerFin
	}
	if c.flags&connOpen != 0 && events&(unix.EPOLLIN|unix.EPOLLRDHUP) != 0 {
		l.readAll(c)
	}
}

func (l *loop) acceptAll() {
	for {
		fd, _, err := unix.Accept4(l.lfd, unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC)
		switch err {
		case nil:
		case unix.EAGAIN:
			return
		case unix.EINTR, unix.ECONNABORTED:
			continue
		case unix.EMFILE, unix.ENFILE:
			// Out of descriptors: use the spare to accept and immediately drop one
			// connection, so the backlog drains instead of waking us forever.
			unix.Close(l.spare)
			if fd, _, err := unix.Accept4(l.lfd, unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC); err == nil {
				unix.Close(fd)
			}
			l.spare, _ = unix.Open("/dev/null", unix.O_RDONLY|unix.O_CLOEXEC, 0)
			return
		default:
			log.Printf("accept: %v", err)
			return
		}
		if fd >= len(l.conns) {
			unix.Close(fd)
			continue
		}
		unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1)
		c := &conn{fd: int32(fd), flags: connOpen, lastSeen: l.nowSec, holdFrom: -1}
		if err := unix.EpollCtl(l.epfd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: evRead, Fd: int32(fd)}); err != nil {
			unix.Close(fd)
			continue
		}
		l.conns[fd] = c
		l.open++
	}
}

// serviceAgain gives paused connections their next slice of work. A conn that
// is also waiting on EPOLLOUT stays paused; flush re-queues it when drained.
func (l *loop) serviceAgain() {
	l.again, l.againNx = l.againNx[:0], l.again
	for _, c := range l.againNx {
		if c.flags&connOpen == 0 || c.flags&connPaused == 0 {
			continue
		}
		if c.flags&connWantWrite != 0 {
			continue
		}
		c.flags &^= connPaused
		l.process(c)
		l.readAll(c)
	}
}

func (l *loop) readAll(c *conn) {
	if c.flags&connDraining != 0 {
		l.drain(c)
		return
	}
	for c.flags&connOpen != 0 {
		if c.flags&(connWantWrite|connPaused|connCloseAfter) != 0 {
			return // backpressure, round cap, or a close already decided: stop reading
		}
		if c.rbuf == nil {
			c.rbuf = l.getRead()
		}
		if len(c.rbuf) == cap(c.rbuf) {
			if cap(c.rbuf) >= maxReadBuf {
				l.closeConn(c) // parseRequest limits make this unreachable; defensive
				return
			}
			nb := make([]byte, len(c.rbuf), cap(c.rbuf)*2)
			copy(nb, c.rbuf)
			c.rbuf = nb
		}
		want := cap(c.rbuf) - len(c.rbuf)
		n, err := unix.Read(int(c.fd), c.rbuf[len(c.rbuf):cap(c.rbuf)])
		l.reads++
		if err == unix.EAGAIN {
			break
		}
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			l.closeConn(c)
			return
		}
		if n == 0 {
			// Peer half-closed. Anything already queued (including a held 201)
			// is still delivered; the socket closes once it drains.
			if len(c.wbuf) > 0 {
				c.flags |= connCloseAfter
				l.markDirty(c)
			} else {
				l.closeConn(c)
			}
			return
		}
		c.rbuf = c.rbuf[:len(c.rbuf)+n]
		c.lastSeen = l.nowSec
		l.process(c)
		if n < want && c.flags&connPeerFin == 0 {
			// The receive queue was empty when read returned, so a further read
			// would only cost an EAGAIN. A packet arriving later raises a new
			// edge. A FIN does not: once RDHUP has been seen, keep reading until
			// the read that returns 0.
			break
		}
	}
	if c.flags&connOpen != 0 && c.rbuf != nil && len(c.rbuf) == 0 {
		l.putRead(c.rbuf)
		c.rbuf = nil
	}
}

// process handles every complete request at the front of c.rbuf.
func (l *loop) process(c *conn) {
	handled := 0
	for c.flags&connOpen != 0 && c.flags&connCloseAfter == 0 && len(c.rbuf) > 0 {
		if handled == maxReqsPerRound {
			c.flags |= connPaused
			l.again = append(l.again, c)
			return
		}
		req, n, st := parseRequest(c.rbuf)
		switch st {
		case parseNeedMore:
			if req.expectContinue && c.flags&connSent100 == 0 {
				c.flags |= connSent100
				c.wbuf = append(c.wbuf, continue100...)
				l.markDirty(c)
			}
			return
		case parseBad:
			l.respondAndClose(c, 400, "bad request")
			return
		case parseHeadersTooLarge:
			l.respondAndClose(c, 431, "request header fields too large")
			return
		case parseBodyTooLarge:
			l.respondAndClose(c, 413, "request body too large")
			return
		}
		if l.writesPaused && req.method == methodPOST {
			// A finishing checkpoint is in flight; this write waits for it
			// (its bytes stay in rbuf) so the WAL can restart. Reads go on.
			c.flags |= connPaused
			l.writeWait = append(l.writeWait, c)
			return
		}
		closeConn := req.connClose || l.open > l.budget
		if c.wbuf == nil {
			c.wbuf = l.getWrite()
		}
		before := len(c.wbuf)
		var held bool
		c.wbuf, held = l.srv.handle(&req, c.wbuf, closeConn, l.nowUnix)
		handled++
		c.flags &^= connSent100
		c.rbuf = c.rbuf[:copy(c.rbuf, c.rbuf[n:])]
		if held && c.flags&connHeld == 0 {
			c.flags |= connHeld
			c.holdFrom = int32(before)
			l.held = append(l.held, c)
		}
		l.markDirty(c)
		if closeConn || req.chunked {
			c.flags |= connCloseAfter
			return
		}
	}
}

func (l *loop) respondAndClose(c *conn, status int, msg string) {
	c.wbuf = appendResponse(c.wbuf, status, appendError(nil, msg), true)
	c.flags |= connCloseAfter
	c.rbuf = c.rbuf[:0]
	l.markDirty(c)
}

func (l *loop) markDirty(c *conn) {
	if len(l.dirty) == 0 || l.dirty[len(l.dirty)-1] != c {
		l.dirty = append(l.dirty, c)
	}
}

// rewriteHeld replaces the held tail of c.wbuf with a 500 and closes after it.
func rewriteHeld(c *conn) {
	c.wbuf = appendResponse(c.wbuf[:c.holdFrom], 500, appendError(nil, errInternal), true)
	c.flags = (c.flags &^ connHeld) | connCloseAfter
	c.holdFrom = -1
}

// finishBatch commits the batch transaction, then writes every response
// produced in this epoll round. Held responses are sent only after COMMIT.
func (l *loop) finishBatch() {
	if err := l.srv.commitBatch(); err != nil {
		log.Printf("commit failed, failing %d held responses: %v", len(l.held), err)
		for _, c := range l.held {
			if c.flags&connHeld != 0 {
				rewriteHeld(c)
			}
		}
	}
	for _, c := range l.held {
		c.flags &^= connHeld
		c.holdFrom = -1
	}
	l.held = l.held[:0]
	for _, c := range l.dirty {
		if c.flags&connOpen != 0 && c.flags&connWantWrite == 0 {
			l.flush(c)
		}
	}
	l.dirty = l.dirty[:0]
}

func (l *loop) flush(c *conn) {
	for len(c.wbuf) > 0 {
		n, err := unix.Write(int(c.fd), c.wbuf)
		if err == unix.EAGAIN {
			if c.flags&connWantWrite == 0 {
				c.flags |= connWantWrite
				unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, int(c.fd), &unix.EpollEvent{Events: evReadWrite, Fd: c.fd})
			}
			return
		}
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			l.closeConn(c)
			return
		}
		c.wbuf = c.wbuf[:copy(c.wbuf, c.wbuf[n:])]
	}
	l.putWrite(c.wbuf)
	c.wbuf = nil
	if c.flags&connCloseAfter != 0 {
		l.lingerClose(c)
		return
	}
	if c.flags&connWantWrite != 0 {
		c.flags &^= connWantWrite
		unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, int(c.fd), &unix.EpollEvent{Events: evRead, Fd: c.fd})
		// Reads were paused while output was pending; the socket may hold data
		// that edge-triggered epoll will not announce again.
		if c.flags&connPaused != 0 {
			l.again = append(l.again, c)
		} else {
			l.readAll(c)
		}
	}
}

// lingerClose ends a connection after its last response has been written.
// Closing with unread input queued makes the kernel send an RST, which can
// destroy the response still in flight; instead half-close, discard whatever
// the peer sends, and close on its FIN (or at the idle sweep).
func (l *loop) lingerClose(c *conn) {
	l.putRead(c.rbuf)
	c.rbuf = nil
	c.flags |= connDraining
	unix.Shutdown(int(c.fd), unix.SHUT_WR)
	l.drain(c)
}

func (l *loop) drain(c *conn) {
	var sink [4096]byte
	for c.flags&connOpen != 0 {
		n, err := unix.Read(int(c.fd), sink[:])
		if err == unix.EAGAIN {
			return
		}
		if err == unix.EINTR {
			continue
		}
		if err != nil || n == 0 {
			l.closeConn(c)
			return
		}
	}
}

func (l *loop) closeConn(c *conn) {
	if c.flags&connOpen == 0 {
		return
	}
	unix.Close(int(c.fd)) // also removes it from epoll
	l.conns[c.fd] = nil
	l.open--
	l.putRead(c.rbuf)
	l.putWrite(c.wbuf)
	c.rbuf, c.wbuf, c.flags = nil, nil, 0
}

func (l *loop) sweep() {
	for _, c := range l.conns {
		if c != nil && c.flags&connOpen != 0 && l.nowSec-c.lastSeen > idleTimeout {
			l.closeConn(c)
		}
	}
}

const (
	ckptIdle   uint8 = iota
	ckptBulk         // a routine pass is running; the loop keeps writing
	ckptFinish       // a finishing pass is running; writes are parked
)

// checkpointTick drives WAL maintenance from the loop so nothing ever races
// it. Every checkpointEvery it asks the checkpointer for a bulk PASSIVE pass,
// which never blocks anyone. SQLite only restarts a WAL when a writer begins
// while every frame is backfilled and no reader is inside it, and the loop
// commits again before any bulk pass ends, so that never happens by itself.
// Once the file has reached walLimitBytes, the loop therefore parks its
// writes after each bulk pass and asks for one more: with no new frames
// arriving it copies only the few committed during the bulk pass, syncs, and
// reports done; the loop releases the writes and the first of them restarts
// the WAL at frame 0. The file grows to the limit once; journal_size_limit
// then holds it at exactly that size, so from there on every pass ends in a
// restart and the wal-index hash chains that every page lookup walks stay a
// couple of seconds deep. Reads are never held, and the loop itself never
// syncs anything.
func (l *loop) checkpointTick() {
	if l.ckptReq == nil {
		return
	}
	now := time.Now()
	select {
	case <-l.ckptDone:
		switch l.ckptState {
		case ckptBulk:
			if l.walLarge() && l.requestPass() {
				l.ckptState = ckptFinish
				l.writesPaused = true
				l.pausedSince = now
			} else {
				l.ckptState = ckptIdle
			}
		case ckptFinish:
			l.ckptState = ckptIdle
			l.releaseWrites()
		}
	default:
	}
	if l.writesPaused && now.Sub(l.pausedSince) > pauseLimit {
		log.Printf("checkpoint finishing pass overran %s; releasing writes", pauseLimit)
		l.ckptState = ckptIdle
		l.releaseWrites()
	}
	if l.ckptState == ckptIdle && !now.Before(l.nextCkpt) {
		if l.requestPass() {
			l.ckptState = ckptBulk
		}
		l.nextCkpt = now.Add(checkpointEvery)
	}
}

func (l *loop) requestPass() bool {
	select {
	case l.ckptReq <- struct{}{}:
		return true
	default:
		return false
	}
}

func (l *loop) walLarge() bool {
	fi, err := os.Stat(l.walPath)
	return err == nil && fi.Size() >= walLimitBytes
}

// releaseWrites ends a pause: parked connections get serviced next round.
func (l *loop) releaseWrites() {
	l.writesPaused = false
	for _, c := range l.writeWait {
		if c.flags&connOpen != 0 && c.flags&connPaused != 0 {
			l.again = append(l.again, c)
		}
	}
	l.writeWait = l.writeWait[:0]
}

func (l *loop) shutdown() {
	unix.Close(l.lfd)
	for _, c := range l.conns {
		if c != nil {
			l.closeConn(c)
		}
	}
	l.srv.close()
}

func (l *loop) getRead() []byte {
	if n := len(l.rpool); n > 0 {
		b := l.rpool[n-1]
		l.rpool = l.rpool[:n-1]
		return b[:0]
	}
	return make([]byte, 0, readBufSize)
}

func (l *loop) getWrite() []byte {
	if n := len(l.wpool); n > 0 {
		b := l.wpool[n-1]
		l.wpool = l.wpool[:n-1]
		return b[:0]
	}
	return make([]byte, 0, readBufSize)
}

func (l *loop) putRead(b []byte) {
	if b != nil && cap(b) == readBufSize && len(l.rpool) < poolLimit {
		l.rpool = append(l.rpool, b[:0])
	}
}

func (l *loop) putWrite(b []byte) {
	if b != nil && cap(b) <= maxReadBuf && len(l.wpool) < poolLimit {
		l.wpool = append(l.wpool, b[:0])
	}
}
