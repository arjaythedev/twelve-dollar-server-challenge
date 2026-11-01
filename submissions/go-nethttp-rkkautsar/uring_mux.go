//go:build linux

package main

import (
	"bytes"
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	muxReadTag   = 1
	muxWriteTag  = 2
	muxAcceptTag = 3
	muxWakeTag   = 4
	muxTagMask   = 7

	muxSqEntries = 4096
	muxCqEntries = 8192
)

type muxConn struct {
	id         uint64
	fd         int
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
	c     *muxConn
	req   *httpReq
	feed  bool
	post  bool
	write bool
	conns []*muxConn
	ids   []int64
	reqs  []*httpReq
}

type muxResp struct {
	c        *muxConn
	pkt      []byte
	close    bool
	feedEnd  bool
	postEnd  bool
	writeEnd bool
}

type uringMux struct {
	handler     MuxHandler
	feed        func() []byte
	postBatch   func([]int64) map[int64][]byte
	writeBatch  func([]*httpReq) []readResp
	ring        *rawRing
	br          *readBufRing
	bufs        []byte
	wakeR       int
	wakeW       int
	wakeBuf     []byte
	wakeArmed   bool
	listenFD    int
	listener    net.Listener
	acceptArmed bool
	conns       map[uint64]*muxConn
	next        atomic.Uint64
	toSub       uint32
	jobs        chan *muxJob
	responses   chan *muxResp
	waiting     []*muxConn
	feedPend    []*muxConn
	feedBusy    bool
	postPend    []*muxConn
	postIDs     []int64
	postBusy    bool
	writePend   []*muxConn
	writeReqs   []*httpReq
	writeBusy   bool
}

func newUringMux(handler MuxHandler, feed func() []byte, postBatch func([]int64) map[int64][]byte, writeBatch func([]*httpReq) []readResp, workers int) (*uringMux, error) {
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
	var pipe [2]int
	if err := syscall.Pipe(pipe[:]); err != nil {
		syscall.Close(ring.fd)
		return nil, err
	}
	_ = syscall.SetNonblock(pipe[0], true)
	_ = syscall.SetNonblock(pipe[1], true)
	if workers <= 0 {
		workers = 64
	}
	m := &uringMux{
		handler:    handler,
		feed:       feed,
		postBatch:  postBatch,
		writeBatch: writeBatch,
		ring:       ring,
		br:         br,
		bufs:       bufs,
		wakeR:      pipe[0],
		wakeW:      pipe[1],
		wakeBuf:    make([]byte, 1),
		conns:      make(map[uint64]*muxConn),
		jobs:       make(chan *muxJob, workers*4),
		responses:  make(chan *muxResp, 1024),
	}
	m.armWake()
	for i := 0; i < workers; i++ {
		go m.worker()
	}
	return m, nil
}

func (m *uringMux) worker() {
	for j := range m.jobs {
		if j.feed {
			pkt := m.feed()
			for _, c := range j.conns {
				m.responses <- &muxResp{c: c, pkt: pkt}
			}
			m.responses <- &muxResp{feedEnd: true}
			m.wake()
			continue
		}
		if j.post {
			packets := m.postBatch(j.ids)
			for i, c := range j.conns {
				pkt := packets[j.ids[i]]
				m.responses <- &muxResp{c: c, pkt: pkt}
			}
			m.responses <- &muxResp{postEnd: true}
			m.wake()
			continue
		}
		if j.write {
			results := m.writeBatch(j.reqs)
			for i, c := range j.conns {
				r := results[i]
				m.responses <- &muxResp{c: c, pkt: r.packet}
			}
			m.responses <- &muxResp{writeEnd: true}
			m.wake()
			continue
		}
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
	tl, ok := ln.(*net.TCPListener)
	if !ok {
		ln.Close()
		return fmt.Errorf("listener is not TCP")
	}
	raw, err := tl.SyscallConn()
	if err != nil {
		ln.Close()
		return err
	}
	var fd int
	if err := raw.Control(func(f uintptr) { fd = int(f) }); err != nil {
		ln.Close()
		return err
	}
	_ = syscall.SetNonblock(fd, true)
	m.listener = ln
	m.listenFD = fd
	m.armAccept()
	m.loop()
	return nil
}

func (m *uringMux) wake() {
	var b [1]byte
	_, _ = syscall.Write(m.wakeW, b[:])
}

func (m *uringMux) armWake() {
	for {
		sqe := m.ring.sqe()
		if sqe == nil {
			m.flush()
			continue
		}
		*sqe = rawSQE{}
		sqe.Opcode = 22 // IORING_OP_READ
		sqe.FD = int32(m.wakeR)
		sqe.Addr = uint64(uintptr(unsafe.Pointer(&m.wakeBuf[0])))
		sqe.Len = 1
		sqe.UserData = muxWakeTag
		m.wakeArmed = true
		m.toSub++
		return
	}
}

func (m *uringMux) armAccept() {
	for {
		sqe := m.ring.sqe()
		if sqe == nil {
			m.flush()
			continue
		}
		*sqe = rawSQE{}
		sqe.Opcode = 13 // IORING_OP_ACCEPT
		sqe.FD = int32(m.listenFD)
		sqe.IOPrio = 1 // IORING_ACCEPT_MULTISHOT
		sqe.UserData = muxAcceptTag
		m.acceptArmed = true
		m.toSub++
		return
	}
}

func (m *uringMux) addFD(fd int) {
	if fd < 0 {
		return
	}
	_ = syscall.SetNonblock(fd, true)
	_ = syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
	id := m.next.Add(1)
	mc := &muxConn{id: id, fd: fd}
	m.conns[id] = mc
	m.arm(mc)
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
	_ = syscall.Close(c.fd)
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
	for {
		m.dispatchWaiting()
		m.flushPosts()
		m.flushWrites()
		m.submitSends()
		if m.toSub > 0 {
			m.flush()
		}
		if _, err := m.ring.enter(0, 1, 1); err != nil && err != syscall.EINTR {
			return
		}
		m.drainCQEs()
		for {
			select {
			case r := <-m.responses:
				m.finishJob(r)
			default:
				goto drained
			}
		}
	drained:
		m.flushFeeds()
	}
}

func (m *uringMux) finishAccept(cqe *rawCQE) {
	m.acceptArmed = false
	if cqe.Res >= 0 {
		m.addFD(int(cqe.Res))
	}
	if cqe.Flags&2 == 0 && !m.acceptArmed {
		m.armAccept()
	}
}

func (m *uringMux) finishWake() {
	m.wakeArmed = false
	var b [256]byte
	for {
		n, err := syscall.Read(m.wakeR, b[:])
		if n <= 0 || err != nil {
			break
		}
	}
	for {
		select {
		case r := <-m.responses:
			m.finishJob(r)
		default:
			goto armed
		}
	}
armed:
	m.armWake()
}

func (m *uringMux) drainCQEs() {
	m.ring.drain(func(cqe *rawCQE) {
		tag := int32(cqe.UserData & muxTagMask)
		if tag == muxAcceptTag {
			m.finishAccept(cqe)
			return
		}
		if tag == muxWakeTag {
			m.finishWake()
			return
		}
		id := cqe.UserData >> 3
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
	if req.method == "GET" && req.path == "/feed" {
		c.in = c.in[used:]
		c.busy = true
		m.feedPend = append(m.feedPend, c)
		return
	}
	if req.method == "GET" && strings.HasPrefix(req.path, "/posts/") {
		rest := req.path[len("/posts/"):]
		if !strings.HasSuffix(rest, "/like") {
			if id, ok := parsePostID(rest); ok {
				c.in = c.in[used:]
				c.busy = true
				m.postPend = append(m.postPend, c)
				m.postIDs = append(m.postIDs, id)
				return
			}
		}
	}
	if req.method == "POST" && (req.path == "/posts" || strings.HasSuffix(req.path, "/like")) {
		c.in = c.in[used:]
		c.busy = true
		m.writePend = append(m.writePend, c)
		m.writeReqs = append(m.writeReqs, req)
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
	if r.feedEnd {
		m.feedBusy = false
		m.flushFeeds()
		return
	}
	if r.postEnd {
		m.postBusy = false
		m.flushPosts()
		return
	}
	if r.writeEnd {
		m.writeBusy = false
		m.flushWrites()
		return
	}
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

func (m *uringMux) flushFeeds() {
	if m.feedBusy || len(m.feedPend) == 0 {
		return
	}
	batch := m.feedPend
	m.feedPend = nil
	select {
	case m.jobs <- &muxJob{feed: true, conns: batch}:
		m.feedBusy = true
	default:
		m.feedPend = batch
	}
}

func (m *uringMux) flushPosts() {
	if m.postBusy || len(m.postPend) == 0 {
		return
	}
	conns := m.postPend
	ids := m.postIDs
	m.postPend = nil
	m.postIDs = nil
	select {
	case m.jobs <- &muxJob{post: true, conns: conns, ids: ids}:
		m.postBusy = true
	default:
		m.postPend = conns
		m.postIDs = ids
	}
}

func (m *uringMux) flushWrites() {
	if m.writeBusy || len(m.writePend) == 0 {
		return
	}
	conns := m.writePend
	reqs := m.writeReqs
	m.writePend = nil
	m.writeReqs = nil
	select {
	case m.jobs <- &muxJob{write: true, conns: conns, reqs: reqs}:
		m.writeBusy = true
	default:
		m.writePend = conns
		m.writeReqs = reqs
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
		method: string(parts[0]),
		path:   string(target),
		close:  bytes.Equal(parts[2], []byte("HTTP/1.0")),
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
