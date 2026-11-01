// Optional batched socket writes through raw io_uring syscalls. One writer
// goroutine owns the ring; connection goroutines submit complete response
// packets and wait for their send completion. Only prebuilt read packets use
// this path.
package main

import (
	"errors"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	ioUringSetup    = 425
	ioUringEnter    = 426
	ioringOffSqRing = 0
	ioringOffCqRing = 0x8000000
	ioringOffSqes   = 0x10000000
	ioringGetEvents = 1
	uringBatch      = 64
	uringEntries    = 256
)

type rawSQE struct {
	Opcode      uint8
	Flags       uint8
	IOPrio      uint16
	FD          int32
	Off         uint64
	Addr        uint64
	Len         uint32
	RWFlags     uint32
	UserData    uint64
	BufIndex    uint16
	Personality uint16
	SpliceFDIn  int32
	Addr3       uint64
	Pad         uint64
}

type rawCQE struct {
	UserData uint64
	Res      int32
	Flags    uint32
}

type rawRing struct {
	fd      int
	sqRing  []byte
	cqRing  []byte
	sqes    []byte
	sqHead  *uint32
	sqTail  *uint32
	sqMask  *uint32
	sqArray *uint32
	cqHead  *uint32
	cqTail  *uint32
	cqMask  *uint32
	cqes    *rawCQE
	entries uint32
}

func newRawRing(entries uint32) (*rawRing, error) {
	var params [120]byte
	fd, _, errno := syscall.Syscall(ioUringSetup, uintptr(entries), uintptr(unsafe.Pointer(&params[0])), 0)
	if errno != 0 {
		return nil, errno
	}
	r := &rawRing{fd: int(fd)}
	u32 := func(off int) uint32 { return *(*uint32)(unsafe.Pointer(&params[off])) }
	r.entries = u32(0)
	sqArrayOff := u32(40 + 6*4)
	cqEntries := u32(4)
	cqHeadOff := u32(80)
	cqTailOff := u32(80 + 4)
	cqMaskOff := u32(80 + 8)
	cqCQEsOff := u32(80 + 5*4)
	sqRingLen := int(sqArrayOff + r.entries*4)
	cqRingLen := int(cqCQEsOff + cqEntries*16)
	sqeLen := int(r.entries * 64)
	sqRing, err := syscall.Mmap(int(fd), ioringOffSqRing, sqRingLen, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		syscall.Close(int(fd))
		return nil, err
	}
	cqRing, err := syscall.Mmap(int(fd), ioringOffCqRing, cqRingLen, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		syscall.Munmap(sqRing)
		syscall.Close(int(fd))
		return nil, err
	}
	sqes, err := syscall.Mmap(int(fd), ioringOffSqes, sqeLen, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		syscall.Munmap(sqRing)
		syscall.Munmap(cqRing)
		syscall.Close(int(fd))
		return nil, err
	}
	r.sqRing, r.cqRing, r.sqes = sqRing, cqRing, sqes
	r.sqHead = (*uint32)(unsafe.Pointer(&sqRing[u32(40)]))
	r.sqTail = (*uint32)(unsafe.Pointer(&sqRing[u32(40+4)]))
	r.sqMask = (*uint32)(unsafe.Pointer(&sqRing[u32(40+8)]))
	r.sqArray = (*uint32)(unsafe.Pointer(&sqRing[sqArrayOff]))
	r.cqHead = (*uint32)(unsafe.Pointer(&cqRing[cqHeadOff]))
	r.cqTail = (*uint32)(unsafe.Pointer(&cqRing[cqTailOff]))
	r.cqMask = (*uint32)(unsafe.Pointer(&cqRing[cqMaskOff]))
	r.cqes = (*rawCQE)(unsafe.Pointer(&cqRing[cqCQEsOff]))
	return r, nil
}

func (r *rawRing) sqe() *rawSQE {
	tail := atomic.LoadUint32(r.sqTail)
	head := atomic.LoadUint32(r.sqHead)
	if tail-head >= r.entries {
		return nil
	}
	idx := tail & atomic.LoadUint32(r.sqMask)
	*(*uint32)(unsafe.Pointer(uintptr(unsafe.Pointer(r.sqArray)) + uintptr(idx)*4)) = idx
	sqe := (*rawSQE)(unsafe.Pointer(&r.sqes[uintptr(idx)*64]))
	atomic.StoreUint32(r.sqTail, tail+1)
	return sqe
}

func (r *rawRing) enter(toSubmit, minComplete, flags uint32) (int, error) {
	for {
		n, _, errno := syscall.Syscall6(ioUringEnter, uintptr(r.fd), uintptr(toSubmit), uintptr(minComplete), uintptr(flags), 0, 0)
		if errno == syscall.EINTR {
			continue
		}
		if errno != 0 {
			return 0, errno
		}
		return int(n), nil
	}
}

func (r *rawRing) drain(fn func(*rawCQE)) int {
	head := atomic.LoadUint32(r.cqHead)
	tail := atomic.LoadUint32(r.cqTail)
	n := 0
	for head != tail {
		cqe := (*rawCQE)(unsafe.Pointer(uintptr(unsafe.Pointer(r.cqes)) + uintptr(head&atomic.LoadUint32(r.cqMask))*unsafe.Sizeof(rawCQE{})))
		fn(cqe)
		head++
		n++
	}
	atomic.StoreUint32(r.cqHead, head)
	return n
}

type uringJob struct {
	fd   int
	buf  []byte
	off  int
	conn net.Conn
	done chan error
}

type uringWriter struct {
	ring *rawRing
	jobs chan *uringJob
	once sync.Once
}

func newUringWriter() (*uringWriter, error) {
	ring, err := newRawRing(uringEntries)
	if err != nil {
		return nil, err
	}
	w := &uringWriter{ring: ring, jobs: make(chan *uringJob, 1024)}
	go w.loop()
	return w, nil
}

func (w *uringWriter) close() {
	w.once.Do(func() {
		_ = syscall.Close(w.ring.fd)
	})
}

func (w *uringWriter) write(fd int, buf []byte, conn net.Conn) error {
	j := &uringJob{fd: fd, buf: buf, conn: conn, done: make(chan error, 1)}
	w.jobs <- j
	return <-j.done
}

func (w *uringWriter) loop() {
	for first := range w.jobs {
		batch := make([]*uringJob, 1, uringBatch)
		batch[0] = first
		for len(batch) < uringBatch {
			select {
			case j := <-w.jobs:
				batch = append(batch, j)
			default:
				goto submit
			}
		}
	submit:
		w.submit(batch)
	}
}

func (w *uringWriter) submit(batch []*uringJob) {
	next := batch
	for len(next) > 0 {
		n := 0
		for n < len(next) {
			sqe := w.ring.sqe()
			if sqe == nil {
				break
			}
			j := next[n]
			*sqe = rawSQE{}
			sqe.Opcode = 26 // IORING_OP_SEND
			sqe.FD = int32(j.fd)
			sqe.Addr = uint64(uintptr(unsafe.Pointer(&j.buf[j.off])))
			sqe.Len = uint32(len(j.buf) - j.off)
			sqe.RWFlags = syscall.MSG_NOSIGNAL
			sqe.UserData = uint64(uintptr(unsafe.Pointer(j)))
			n++
		}
		if n == 0 {
			_, _ = w.ring.enter(0, 0, 0)
			continue
		}
		submitN := n
		for submitN > 0 {
			rc, err := w.ring.enter(uint32(submitN), 0, 0)
			if err != nil {
				for _, j := range next[:n] {
					j.done <- err
				}
				return
			}
			if rc <= 0 {
				break
			}
			submitN -= rc
		}
		remaining := n
		retry := append([]*uringJob(nil), next[n:]...)
		for remaining > 0 {
			if _, err := w.ring.enter(0, 1, ioringGetEvents); err != nil {
				for _, j := range next[:n] {
					j.done <- err
				}
				return
			}
			got := w.ring.drain(func(cqe *rawCQE) {
				remaining--
				j := (*uringJob)(unsafe.Pointer(uintptr(cqe.UserData)))
				if cqe.Res < 0 {
					err := syscall.Errno(-cqe.Res)
					if errors.Is(err, syscall.EAGAIN) {
						if _, werr := j.conn.Write(j.buf[j.off:]); werr != nil {
							j.done <- werr
						} else {
							j.done <- nil
						}
						return
					}
					j.done <- err
					return
				}
				if cqe.Res == 0 {
					j.done <- io.ErrUnexpectedEOF
					return
				}
				j.off += int(cqe.Res)
				if j.off < len(j.buf) {
					retry = append(retry, j)
				} else {
					j.done <- nil
				}
			})
			if got == 0 && remaining > 0 {
				continue
			}
		}
		next = retry
	}
}
