//go:build linux

package main

import (
	"fmt"
	"io"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	ioUringRegister = 427
	rawOpRecv       = 27
	iosqeBufSel     = 1 << 5
	recvMultishot   = 1 << 1
	cqeBuffer       = 1 << 0
	cqeMore         = 1 << 1

	eventfdSyscall     = 290
	registerEventfd    = 4
	registerPbufRing   = 22
	ioringOffPbufRing  = 0x80000000
	ioringOffPbufShift = 16

	readSqEntries = 4096
	readCqEntries = 8192
	readBufCount  = 8192
	readBufLen    = 2048
	readBufGroup  = 0
)

type readBuf struct {
	Addr uint64
	Len  uint32
	BID  uint16
	Resv uint16
}

type readBufReg struct {
	RingAddr uint64
	RingEnts uint32
	BGID     uint16
	Flags    uint16
	MinLeft  uint32
	Reserved [5]uint32
}

type readBufRing struct {
	mem     []byte
	entries uint32
	mask    uint32
	tail    uint32
}

type uringConn struct {
	id     uint64
	fd     int
	ch     chan []byte
	done   chan struct{}
	cur    []byte
	closed bool
}

func (c *uringConn) Read(p []byte) (int, error) {
	for len(c.cur) == 0 {
		b, ok := <-c.ch
		if !ok {
			return 0, io.EOF
		}
		c.cur = b
	}
	n := copy(p, c.cur)
	c.cur = c.cur[n:]
	return n, nil
}

type uringReader struct {
	ring     *rawRing
	efd      int
	epfd     int
	wakeR    int
	wakeW    int
	br       *readBufRing
	bufs     []byte
	addCh    chan *uringConn
	dropCh   chan *uringConn
	conns    map[uint64]*uringConn
	next     atomic.Uint64
	toSubmit uint32
	once     sync.Once
}

func rawEventfd() (int, error) {
	fd, _, errno := syscall.Syscall(eventfdSyscall, 0, uintptr(0x800|0x80000), 0)
	if errno != 0 {
		return -1, errno
	}
	return int(fd), nil
}

func newReadBufRing(ring *rawRing, mem []byte, bufs []byte) (*readBufRing, error) {
	reg := readBufReg{
		RingAddr: uint64(uintptr(unsafe.Pointer(&mem[0]))),
		RingEnts: readBufCount,
		BGID:     readBufGroup,
	}
	if _, _, errno := syscall.Syscall6(ioUringRegister, uintptr(ring.fd), registerPbufRing, uintptr(unsafe.Pointer(&reg)), 1, 0, 0); errno != 0 {
		return nil, errno
	}
	for i := 0; i < readBufCount; i++ {
		b := (*readBuf)(unsafe.Pointer(&mem[i*16]))
		b.Addr = uint64(uintptr(unsafe.Pointer(&bufs[i*readBufLen])))
		b.Len = readBufLen
		b.BID = uint16(i)
	}
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&mem[12])), uint32(readBufCount)<<16)
	return &readBufRing{
		mem:     mem,
		entries: readBufCount,
		mask:    readBufCount - 1,
		tail:    readBufCount,
	}, nil
}

func (b *readBufRing) put(base []byte, id int) {
	slot := b.tail & b.mask
	e := (*readBuf)(unsafe.Pointer(&b.mem[int(slot)*16]))
	e.Addr = uint64(uintptr(unsafe.Pointer(&base[id*readBufLen])))
	e.Len = readBufLen
	e.BID = uint16(id)
	e.Resv = 0
	b.tail = (b.tail + 1) & 0xffff
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&b.mem[12])), b.tail<<16)
}

func newUringReader() (*uringReader, error) {
	ring, err := newRawRing(readSqEntries, readCqEntries)
	if err != nil {
		return nil, fmt.Errorf("setup: %w", err)
	}
	mem, err := syscall.Mmap(-1, 0, readBufCount*16, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE|syscall.MAP_ANONYMOUS)
	if err != nil {
		syscall.Close(ring.fd)
		return nil, fmt.Errorf("mmap buffer ring: %w", err)
	}
	bufs := make([]byte, readBufCount*readBufLen)
	br, err := newReadBufRing(ring, mem, bufs)
	if err != nil {
		syscall.Munmap(mem)
		syscall.Close(ring.fd)
		return nil, fmt.Errorf("register buffer ring: %w", err)
	}
	efd, err := rawEventfd()
	if err != nil {
		syscall.Close(ring.fd)
		return nil, fmt.Errorf("eventfd: %w", err)
	}
	if _, _, errno := syscall.Syscall6(ioUringRegister, uintptr(ring.fd), registerEventfd, uintptr(unsafe.Pointer(&efd)), 1, 0, 0); errno != 0 {
		syscall.Close(efd)
		syscall.Close(ring.fd)
		return nil, fmt.Errorf("register eventfd: %w", errno)
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
	r := &uringReader{
		ring:   ring,
		efd:    efd,
		epfd:   epfd,
		wakeR:  pipe[0],
		wakeW:  pipe[1],
		br:     br,
		bufs:   bufs,
		addCh:  make(chan *uringConn, 4096),
		dropCh: make(chan *uringConn, 4096),
		conns:  make(map[uint64]*uringConn),
	}
	go r.loop()
	return r, nil
}

func (r *uringReader) wake() {
	var b [1]byte
	_, _ = syscall.Write(r.wakeW, b[:])
}

func (r *uringReader) drainWake() {
	var b [256]byte
	for {
		n, err := syscall.Read(r.wakeR, b[:])
		if n <= 0 || err != nil {
			return
		}
	}
}

func (r *uringReader) add(fd int) *uringConn {
	c := &uringConn{
		id:   r.next.Add(1),
		fd:   fd,
		ch:   make(chan []byte, 16),
		done: make(chan struct{}),
	}
	r.addCh <- c
	r.wake()
	return c
}

func (r *uringReader) drop(c *uringConn) {
	if c == nil {
		return
	}
	r.dropCh <- c
	r.wake()
}

func (r *uringReader) flush() {
	for r.toSubmit > 0 {
		rc, err := r.ring.enter(r.toSubmit, 0, 0)
		if err != nil {
			return
		}
		if rc <= 0 {
			return
		}
		r.toSubmit -= uint32(rc)
	}
}

func (r *uringReader) arm(c *uringConn) {
	if c.closed {
		return
	}
	for {
		sqe := r.ring.sqe()
		if sqe == nil {
			r.flush()
			continue
		}
		*sqe = rawSQE{}
		sqe.Opcode = rawOpRecv
		sqe.Flags = iosqeBufSel
		sqe.IOPrio = recvMultishot
		sqe.FD = int32(c.fd)
		sqe.Len = 0
		sqe.BufIndex = readBufGroup
		sqe.UserData = c.id
		r.toSubmit++
		return
	}
}

func (r *uringReader) closeConn(c *uringConn) {
	if c == nil || c.closed {
		return
	}
	c.closed = true
	delete(r.conns, c.id)
	close(c.ch)
	close(c.done)
}

func (r *uringReader) loop() {
	events := make([]syscall.EpollEvent, 16)
	for {
		for {
			select {
			case c := <-r.addCh:
				r.conns[c.id] = c
				r.arm(c)
			case c := <-r.dropCh:
				r.closeConn(c)
			default:
				goto ready
			}
		}
	ready:
		r.flush()
		n, err := syscall.EpollWait(r.epfd, events, 50)
		if err != nil && err != syscall.EINTR {
			return
		}
		for i := 0; i < n; i++ {
			if int(events[i].Fd) == r.wakeR {
				r.drainWake()
			}
			if int(events[i].Fd) == r.efd {
				var b [8]byte
				_, _ = syscall.Read(r.efd, b[:])
			}
		}
		r.drainCQEs()
		r.flush()
	}
}

func (r *uringReader) drainCQEs() {
	r.ring.drain(func(cqe *rawCQE) {
		c := r.conns[cqe.UserData]
		res := cqe.Res
		flags := cqe.Flags
		hasBuf := flags&cqeBuffer != 0
		var bid int
		if hasBuf {
			bid = int((flags >> 16) & 0xffff)
		}
		if c == nil || c.closed {
			if hasBuf && bid >= 0 && bid < readBufCount {
				r.br.put(r.bufs, bid)
			}
			return
		}
		if res > 0 && hasBuf && bid < readBufCount {
			data := append([]byte(nil), r.bufs[bid*readBufLen:bid*readBufLen+int(res)]...)
			r.br.put(r.bufs, bid)
			select {
			case c.ch <- data:
			case <-c.done:
			}
			if flags&cqeMore == 0 {
				r.arm(c)
			}
			return
		}
		if res == -int32(syscall.ENOBUFS) || res == -int32(syscall.EAGAIN) || res == -int32(syscall.EINTR) {
			if hasBuf && bid < readBufCount {
				r.br.put(r.bufs, bid)
			}
			r.arm(c)
			return
		}
		if hasBuf && bid < readBufCount {
			r.br.put(r.bufs, bid)
		}
		r.closeConn(c)
	})
}
