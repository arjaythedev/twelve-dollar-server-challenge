//go:build linux

package main

import (
	"bytes"
	"net"
	"strconv"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	muxReadTag  = 1
	muxWriteTag = 2
	muxTagMask  = 7

	muxSqEntries = 4096
	muxCqEntries = 8192
)

type muxConn struct {
	id         uint64
	fd         int
	conn       net.Conn
	in         []byte
	out        [][]byte
	outOff     int
	reading    bool
	writing    bool
	busy       bool
	waiting    bool
	closeAfter bool
	closed     bool
}

type MuxHandler func(*httpReq) (packet []byte, closeAfter bool)

type muxJob struct {
	c   *muxConn
	req *httpReq
}

type muxResp struct {
	c     *muxConn
	pkt   []byte
	close bool
}

type uringMux struct {
	handler   MuxHandler
	ring      *rawRing
	br        *readBufRing
	bufs      []byte
	epfd      int
	efd       int
	wakeR     int
	wakeW     int
	addCh     chan net.Conn
	conns     map[uint64]*muxConn
	next      atomic.Uint64
	toSub     uint32
	jobs      chan *muxJob
	responses chan *muxResp
	waiting   []*muxConn
}

func newUringMux(handler MuxHandler, workers int) (*uringMux, error) {
	ring, err := newRawRing(muxSqEntries, muxCqEntries)
	if err != nil {
		return nil, err
	}
	mem, err := syscall.Mmap(-1, 0, readBufCount*16, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE|syscall.MAP_ANONYMOUS)
	if err != nil {
		syscall.Close(ring.fd)
		return nil, err
	}
	bufs := make([]byte, readBufCount*readBufLen)
	br, err := newReadBufRing(ring, mem, bufs)
	if err != nil {
		syscall.Munmap(mem)
		syscall.Close(ring.fd)
		return nil, err
	}
	efd, err := rawEventfd()
	if err != nil {
		syscall.Close(ring.fd)
		return nil, err
	}
	if _, _, errno := syscall.Syscall6(ioUringRegister, uintptr(ring.fd), registerEventfd, uintptr(unsafe.Pointer(&efd)), 1, 0, 0); errno != 0 {
		syscall.Close(efd)
		syscall.Close(ring.fd)
		return nil, errno
	}
	epfd, err := syscall.EpollCreate1(0)
	if err != nil {
		syscall.Close(efd)
		syscall.Close(ring.fd)
		return nil, err
	}
	ev := syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(efd)}
	if err := syscall.EpollCtl(epfd, syscall.EPOLL_CTL_ADD, efd, &ev); err != nil {
		syscall.Close(epfd)
		syscall.Close(efd)
		syscall.Close(ring.fd)
		return nil, err
	}
	var pipe [2]int
	if err := syscall.Pipe(pipe[:]); err != nil {
		syscall.Close(epfd)
		syscall.Close(efd)
		syscall.Close(ring.fd)
		return nil, err
	}
	_ = syscall.SetNonblock(pipe[0], true)
	_ = syscall.SetNonblock(pipe[1], true)
	wev := syscall.EpollEvent{Events: syscall.EPOLLIN, Fd: int32(pipe[0])}
	if err := syscall.EpollCtl(epfd, syscall.EPOLL_CTL_ADD, pipe[0], &wev); err != nil {
		syscall.Close(epfd)
		syscall.Close(efd)
		syscall.Close(ring.fd)
		return nil, err
	}
	if workers <= 0 {
		workers = 64
	}
	m := &uringMux{
		handler:   handler,
		ring:      ring,
		br:        br,
		bufs:      bufs,
		epfd:      epfd,
		efd:       efd,
		wakeR:     pipe[0],
		wakeW:     pipe[1],
		addCh:     make(chan net.Conn, 4096),
		conns:     make(map[uint64]*muxConn),
		jobs:      make(chan *muxJob, workers*4),
		responses: make(chan *muxResp, 1024),
	}
	for i := 0; i < workers; i++ {
		go m.worker()
	}
	return m, nil
}

func (m *uringMux) worker() {
	for j := range m.jobs {
		pkt, closeAfter := m.handler(j.req)
		m.responses <- &muxResp{c: j.c, pkt: pkt, close: closeAfter}
		m.wake()
	}
}

func (m *uringMux) serve(host, port string) error {
	ln, err := net.Listen("tcp", host+":"+port)
	if err != nil {
		return err
	}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				continue
			}
			m.addCh <- c
			var b [1]byte
			_, _ = syscall.Write(m.wakeW, b[:])
		}
	}()
	m.loop()
	return nil
}

func (m *uringMux) wake() {
	var b [1]byte
	_, _ = syscall.Write(m.wakeW, b[:])
}

func (m *uringMux) drainWake() {
	var b [256]byte
	for {
		n, err := syscall.Read(m.wakeR, b[:])
		if n <= 0 || err != nil {
			return
		}
	}
}

func (m *uringMux) flush() {
	for m.toSub > 0 {
		rc, err := m.ring.enter(m.toSub, 0, 0)
		if err != nil || rc <= 0 {
			return
		}
		m.toSub -= uint32(rc)
	}
}

func (m *uringMux) add(c net.Conn) {
	tc, ok := c.(*net.TCPConn)
	if !ok {
		_ = c.Close()
		return
	}
	_ = tc.SetNoDelay(true)
	sc, ok := c.(syscall.Conn)
	if !ok {
		_ = c.Close()
		return
	}
	raw, err := sc.SyscallConn()
	if err != nil {
		_ = c.Close()
		return
	}
	var fd int
	if err := raw.Control(func(f uintptr) { fd = int(f) }); err != nil || fd < 0 {
		_ = c.Close()
		return
	}
	_ = syscall.SetNonblock(fd, true)
	id := m.next.Add(1)
	mc := &muxConn{id: id, fd: fd, conn: c}
	m.conns[id] = mc
	m.arm(mc)
}

func (m *uringMux) arm(c *muxConn) {
	if c.closed || c.reading {
		return
	}
	for {
		sqe := m.ring.sqe()
		if sqe == nil {
			m.flush()
			continue
		}
		*sqe = rawSQE{}
		sqe.Opcode = rawOpRecv
		sqe.Flags = iosqeBufSel
		sqe.IOPrio = recvMultishot
		sqe.FD = int32(c.fd)
		sqe.Len = 0
		sqe.BufIndex = readBufGroup
		sqe.UserData = c.id<<3 | muxReadTag
		c.reading = true
		m.toSub++
		return
	}
}

func (m *uringMux) close(c *muxConn) {
	if c == nil || c.closed {
		return
	}
	c.closed = true
	delete(m.conns, c.id)
	_ = c.conn.Close()
}

func (m *uringMux) send(c *muxConn, p []byte, closeAfter bool) {
	if c.closed {
		return
	}
	c.out = append(c.out, p)
	if closeAfter {
		c.closeAfter = true
	}
}

func (m *uringMux) submitSends() {
	for _, c := range m.conns {
		if c.closed || c.writing || len(c.out) == 0 {
			continue
		}
		for {
			sqe := m.ring.sqe()
			if sqe == nil {
				m.flush()
				continue
			}
			p := c.out[0]
			*sqe = rawSQE{}
			sqe.Opcode = 26 // IORING_OP_SEND
			sqe.FD = int32(c.fd)
			sqe.Addr = uint64(uintptr(unsafe.Pointer(&p[c.outOff])))
			sqe.Len = uint32(len(p) - c.outOff)
			sqe.RWFlags = syscall.MSG_NOSIGNAL
			sqe.UserData = c.id<<3 | muxWriteTag
			c.writing = true
			m.toSub++
			break
		}
	}
	m.flush()
}

func (m *uringMux) loop() {
	events := make([]syscall.EpollEvent, 16)
	for {
		for {
			select {
			case c := <-m.addCh:
				m.add(c)
			default:
				goto ready
			}
		}
	ready:
		for {
			select {
			case r := <-m.responses:
				m.finishJob(r)
			default:
				goto drained
			}
		}
	drained:
		m.dispatchWaiting()
		m.submitSends()
		n, err := syscall.EpollWait(m.epfd, events, 50)
		if err != nil && err != syscall.EINTR {
			return
		}
		for i := 0; i < n; i++ {
			if int(events[i].Fd) == m.wakeR {
				m.drainWake()
			}
			if int(events[i].Fd) == m.efd {
				var b [8]byte
				_, _ = syscall.Read(m.efd, b[:])
			}
		}
		m.drainCQEs()
	}
}

func (m *uringMux) drainCQEs() {
	m.ring.drain(func(cqe *rawCQE) {
		id := cqe.UserData >> 3
		tag := int32(cqe.UserData & muxTagMask)
		c := m.conns[id]
		if c == nil || c.closed {
			if tag == muxReadTag && cqe.Flags&cqeBuffer != 0 {
				bid := int((cqe.Flags >> 16) & 0xffff)
				if bid < readBufCount {
					m.br.put(m.bufs, bid)
				}
			}
			return
		}
		if tag == muxWriteTag {
			m.finishWrite(c, cqe)
			return
		}
		m.finishRead(c, cqe)
	})
}

func (m *uringMux) finishRead(c *muxConn, cqe *rawCQE) {
	res := cqe.Res
	flags := cqe.Flags
	hasBuf := flags&cqeBuffer != 0
	bid := -1
	if hasBuf {
		bid = int((flags >> 16) & 0xffff)
	}
	if res > 0 && hasBuf && bid < readBufCount {
		data := append([]byte(nil), m.bufs[bid*readBufLen:bid*readBufLen+int(res)]...)
		m.br.put(m.bufs, bid)
		m.handleData(c, data)
		if flags&cqeMore == 0 {
			c.reading = false
			m.arm(c)
		}
		return
	}
	if hasBuf && bid >= 0 && bid < readBufCount {
		m.br.put(m.bufs, bid)
	}
	if res == -int32(syscall.ENOBUFS) || res == -int32(syscall.EAGAIN) || res == -int32(syscall.EINTR) {
		c.reading = false
		m.arm(c)
		return
	}
	c.reading = false
	m.close(c)
}

func (m *uringMux) finishWrite(c *muxConn, cqe *rawCQE) {
	c.writing = false
	if cqe.Res < 0 {
		m.close(c)
		return
	}
	if cqe.Res == 0 {
		m.close(c)
		return
	}
	c.outOff += int(cqe.Res)
	if c.outOff < len(c.out[0]) {
		return
	}
	c.out = c.out[1:]
	c.outOff = 0
	if len(c.out) > 0 {
		return
	}
	if c.closeAfter {
		m.close(c)
	}
}

func (m *uringMux) handleData(c *muxConn, data []byte) {
	if c.closed {
		return
	}
	c.in = append(c.in, data...)
	m.pump(c)
}

func (m *uringMux) pump(c *muxConn) {
	if c.closed || c.busy || c.waiting {
		return
	}
	req, used, incomplete, err := parseEventRequest(c.in)
	if err != nil {
		m.close(c)
		return
	}
	if incomplete {
		return
	}
	select {
	case m.jobs <- &muxJob{c: c, req: req}:
		c.in = c.in[used:]
		c.busy = true
	default:
		c.waiting = true
		m.waiting = append(m.waiting, c)
	}
}

func (m *uringMux) finishJob(r *muxResp) {
	c := r.c
	if c.closed || !c.busy {
		return
	}
	c.busy = false
	if r.pkt != nil {
		m.send(c, r.pkt, r.close)
	}
	if !r.close {
		m.pump(c)
	}
}

func (m *uringMux) dispatchWaiting() {
	if len(m.waiting) == 0 {
		return
	}
	keep := m.waiting[:0]
	for _, c := range m.waiting {
		if c.closed {
			continue
		}
		c.waiting = false
		m.pump(c)
		if c.waiting {
			keep = append(keep, c)
		}
	}
	m.waiting = keep
}

func encodeMuxResponse(c *capture, closeAfter bool) []byte {
	if c.packet != nil && !closeAfter {
		return c.packet
	}
	body := c.body
	var hdr [256]byte
	h := hdr[:0]
	h = append(h, "HTTP/1.1 "...)
	h = strconv.AppendInt(h, int64(c.status), 10)
	h = append(h, ' ')
	h = append(h, muxStatusText(c.status)...)
	h = append(h, "\r\nContent-Type: application/json\r\nContent-Length: "...)
	h = strconv.AppendInt(h, int64(len(body)), 10)
	if closeAfter {
		h = append(h, "\r\nConnection: close"...)
	}
	h = append(h, "\r\n\r\n"...)
	out := make([]byte, 0, len(h)+len(body))
	out = append(out, h...)
	out = append(out, body...)
	if c.buf != nil {
		bufPool.Put(c.buf)
		c.buf = nil
	}
	return out
}

func muxStatusText(code int) string {
	switch code {
	case 200:
		return "OK"
	case 201:
		return "Created"
	case 400:
		return "Bad Request"
	case 401:
		return "Unauthorized"
	case 404:
		return "Not Found"
	case 500:
		return "Internal Server Error"
	case 503:
		return "Service Unavailable"
	}
	return "Unknown"
}

func parseEventRequest(b []byte) (*httpReq, int, bool, error) {
	he := bytes.Index(b, []byte("\r\n\r\n"))
	if he < 0 {
		if len(b) > 16<<10 {
			return nil, 0, false, errBadRequest
		}
		return nil, 0, true, nil
	}
	head := b[:he]
	lines := bytes.Split(head, []byte("\r\n"))
	if len(lines) == 0 {
		return nil, 0, false, errBadRequest
	}
	parts := bytes.SplitN(lines[0], []byte(" "), 3)
	if len(parts) != 3 {
		return nil, 0, false, errBadRequest
	}
	target := parts[1]
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	req := &httpReq{
		path:  string(target),
		close: bytes.Equal(parts[2], []byte("HTTP/1.0")),
	}
	clen := 0
	for _, line := range lines[1:] {
		if len(line) == 0 {
			continue
		}
		colon := bytes.IndexByte(line, ':')
		if colon < 0 {
			continue
		}
		name := bytes.TrimSpace(line[:colon])
		value := bytes.TrimSpace(line[colon+1:])
		switch {
		case bytes.EqualFold(name, []byte("authorization")):
			req.auth = string(value)
		case bytes.EqualFold(name, []byte("content-length")):
			n, err := strconv.Atoi(string(value))
			if err != nil || n < 0 {
				return nil, 0, false, errBadRequest
			}
			clen = n
		case bytes.EqualFold(name, []byte("connection")):
			if bytes.Contains(bytes.ToLower(value), []byte("close")) {
				req.close = true
			}
		}
	}
	end := he + 4
	if clen > 0 {
		if len(b) < end+clen {
			return nil, 0, true, nil
		}
		req.body = append([]byte(nil), b[end:end+clen]...)
		end += clen
	}
	return req, end, false, nil
}
