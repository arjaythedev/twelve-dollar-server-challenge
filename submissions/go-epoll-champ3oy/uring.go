package main

/*
#cgo LDFLAGS: -luring
#include <stdlib.h>
#include "uring.h"
*/
import "C"

import (
	"fmt"
	"log"
	"syscall"
	"unsafe"
)

// The io_uring backend. Client sockets are accepted as direct descriptors: they live in a
// ring's registered file table and never take a file descriptor. The kernel caps each table at
// RLIMIT_NOFILE, but the cap is per table, so two rings on one thread hold twice as many
// connections as the process could open file descriptors (2 x 65,535 on the benchmark box).
//
// It also saves system calls: accepts and receives are multishot, received bytes land in a
// shared pool of provided buffers (idle connections hold none), and everything one loop
// iteration queued is submitted with one io_uring_enter.

const (
	uringRings   = 2
	uringBufs    = 8192 // per ring, a power of two
	uringBufSize = 2048
	uringBatch   = 4096 // completions handled per iteration
	acceptGuard  = 1024 // move accepting to another ring this close to a full table
	sendBlock    = 16 << 10
)

// CQE flags (linux/io_uring.h).
const (
	cqeFBuffer     = 1 << 0
	cqeFMore       = 1 << 1
	cqeBufferShift = 16
)

// user_data: kind in the top byte, then the 24-bit generation, then the connection id (for
// sends, the send buffer index; for accepts, the ring).
const (
	kAccept = iota + 1
	kRecv
	kSend
	kClose
	kPipe
)

func ud(kind int, gen, id int32) C.uint64_t {
	return C.uint64_t(uint64(kind)<<56 | uint64(uint32(gen)&genMask)<<32 | uint64(uint32(id)))
}

// sendBuf holds one response in C memory until the kernel has sent all of it.
type sendBuf struct {
	p          unsafe.Pointer
	data       []byte
	off, n     int
	conn, gen  int32
	standalone bool // larger than sendBlock, freed after use
}

type uringIO struct {
	s          *server
	slots      int32
	bufs       [uringRings][]byte
	cqes       *C.ur_cqe
	open       [uringRings]int
	acceptRing int
	sends      []sendBuf
	freeStd    []int32 // sends entries holding a sendBlock-sized buffer
	freeEmpty  []int32 // sends entries holding nothing
	rearm      []connRef
}

type connRef struct{ id, gen int32 }

func newUringIO(s *server) (*uringIO, error) {
	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err != nil {
		return nil, err
	}
	slots := int32(min(lim.Cur, 1<<16))
	if rc := C.ur_setup(uringRings, C.int(slots), uringBufs, uringBufSize); rc < 0 {
		return nil, fmt.Errorf("io_uring setup: %v", syscall.Errno(-rc))
	}
	// io_uring parks the accept on the listener itself; a blocking socket keeps it from
	// returning EAGAIN instead.
	if err := syscall.SetNonblock(s.lfd, false); err != nil {
		return nil, err
	}
	u := &uringIO{s: s, slots: slots}
	for r := range u.bufs {
		u.bufs[r] = unsafe.Slice((*byte)(unsafe.Pointer(C.ur_bufs(C.int(r)))), uringBufs*uringBufSize)
	}
	u.cqes = (*C.ur_cqe)(C.malloc(C.size_t(uringBatch) * C.size_t(unsafe.Sizeof(C.ur_cqe{}))))
	s.conns = make([]conn, uringRings*int(slots))
	u.armAccept(0)
	C.ur_poll(0, C.int(s.checkpointPipe), ud(kPipe, 0, 0))
	log.Printf("io_uring: %d rings x %d connection slots", uringRings, slots)
	return u, nil
}

func (u *uringIO) name() string { return "io_uring" }

func (u *uringIO) armAccept(ring int) {
	u.acceptRing = ring
	C.ur_accept(C.int(ring), C.int(u.s.lfd), ud(kAccept, 0, int32(ring)))
}

func (u *uringIO) split(id int32) (C.int, C.int) {
	return C.int(id / u.slots), C.int(id % u.slots)
}

func (u *uringIO) run() {
	s := u.s
	cqes := unsafe.Slice(u.cqes, uringBatch)
	for {
		n := C.ur_wait(u.cqes, uringBatch)
		if n < 0 {
			log.Fatalf("io_uring wait: %v", syscall.Errno(-n))
		}
		for i := range int(n) {
			e := &cqes[i]
			v := uint64(e.ud)
			kind, gen, id := int(v>>56), int32(v>>32)&genMask, int32(uint32(v))
			res, flags := int32(e.res), uint32(e.flags)
			switch kind {
			case kRecv:
				u.onRecv(id, gen, res, flags)
			case kSend:
				u.onSend(id, res)
			case kAccept:
				u.onAccept(int(id), res, flags)
			case kClose:
				u.open[id/u.slots]--
			case kPipe:
				s.checkpointDone()
				if flags&cqeFMore == 0 {
					C.ur_poll(0, C.int(s.checkpointPipe), ud(kPipe, 0, 0))
				}
			}
		}
		// Receives that stopped because the buffer pool ran dry; this iteration returned buffers.
		for _, r := range u.rearm {
			if c := &s.conns[r.id]; c.open && c.gen == r.gen {
				ring, slot := u.split(r.id)
				C.ur_recv(ring, slot, ud(kRecv, c.gen, c.id))
			}
		}
		u.rearm = u.rearm[:0]
		s.commitPending()
	}
}

func (u *uringIO) onAccept(ring int, res int32, flags uint32) {
	if flags&cqeFMore == 0 && ring == u.acceptRing {
		u.armAccept(ring) // the multishot accept ended (an error, or a full table): re-arm it
	}
	if res < 0 {
		return
	}
	s := u.s
	id := int32(ring)*u.slots + res
	c := &s.conns[id]
	*c = conn{id: id, gen: s.newGen(), open: true}
	u.open[ring]++
	slotRing, slot := u.split(id)
	C.ur_recv(slotRing, slot, ud(kRecv, c.gen, id))

	// Steer new connections to the emptiest ring before this one's table fills up.
	if ring == u.acceptRing && u.open[ring] >= int(u.slots)-acceptGuard {
		next := 0
		for r := range u.open {
			if u.open[r] < u.open[next] {
				next = r
			}
		}
		if next != ring {
			C.ur_cancel_accept(C.int(ring), C.int(s.lfd))
			u.armAccept(next)
		}
	}
}

func (u *uringIO) onRecv(id, gen, res int32, flags uint32) {
	s := u.s
	c := &s.conns[id]
	ring := id / u.slots
	if flags&cqeFBuffer != 0 {
		bid := int(flags >> cqeBufferShift)
		if res > 0 && c.open && c.gen == gen {
			off := bid * uringBufSize
			s.receive(c, u.bufs[ring][off:off+int(res)])
		}
		C.ur_buf_return(C.int(ring), C.int(bid))
	}
	if !c.open || c.gen != gen {
		return
	}
	switch {
	case res == -int32(syscall.ENOBUFS):
		u.rearm = append(u.rearm, connRef{id, gen})
	case res <= 0:
		u.close(c) // the client hung up, or the socket failed
	case flags&cqeFMore == 0 && c.open && c.gen == gen:
		r, slot := u.split(id)
		C.ur_recv(r, slot, ud(kRecv, gen, id))
	}
}

// send hands out to the kernel. One send per connection is in flight at a time, which keeps
// responses in order; anything produced meanwhile waits in c.out.
func (u *uringIO) send(c *conn, out []byte) {
	if c.sending > 0 {
		c.out = append(c.out, out...)
		return
	}
	u.submitSend(c, out)
}

func (u *uringIO) submitSend(c *conn, out []byte) {
	idx := u.allocSend(len(out))
	b := &u.sends[idx]
	copy(b.data, out)
	b.off, b.n, b.conn, b.gen = 0, len(out), c.id, c.gen
	ring, slot := u.split(c.id)
	if C.ur_send(ring, slot, b.p, C.int(b.n), ud(kSend, 0, idx)) < 0 {
		u.releaseSend(idx)
		u.close(c)
		return
	}
	c.sending++
}

func (u *uringIO) onSend(idx, res int32) {
	s := u.s
	b := &u.sends[idx]
	c := &s.conns[b.conn]
	live := c.open && c.gen == b.gen
	if res < 0 || !live {
		u.releaseSend(idx)
		if live {
			c.sending--
			u.close(c)
		}
		return
	}
	b.off += int(res)
	if b.off < b.n { // a short send: queue the rest
		ring, slot := u.split(c.id)
		C.ur_send(ring, slot, unsafe.Add(b.p, b.off), C.int(b.n-b.off), ud(kSend, 0, idx))
		return
	}
	u.releaseSend(idx)
	c.sending--
	switch {
	case len(c.out) > 0:
		out := c.out
		c.out = nil
		u.submitSend(c, out)
	case c.closeAfter && !c.blocked:
		u.close(c)
	}
}

func (u *uringIO) allocSend(n int) int32 {
	if n <= sendBlock && len(u.freeStd) > 0 {
		idx := u.freeStd[len(u.freeStd)-1]
		u.freeStd = u.freeStd[:len(u.freeStd)-1]
		return idx
	}
	var idx int32
	if len(u.freeEmpty) > 0 {
		idx = u.freeEmpty[len(u.freeEmpty)-1]
		u.freeEmpty = u.freeEmpty[:len(u.freeEmpty)-1]
	} else {
		u.sends = append(u.sends, sendBuf{})
		idx = int32(len(u.sends) - 1)
	}
	size := max(n, sendBlock)
	p := C.malloc(C.size_t(size))
	u.sends[idx] = sendBuf{p: p, data: unsafe.Slice((*byte)(p), size), standalone: size > sendBlock}
	return idx
}

func (u *uringIO) releaseSend(idx int32) {
	b := &u.sends[idx]
	if b.standalone {
		C.free(b.p)
		*b = sendBuf{}
		u.freeEmpty = append(u.freeEmpty, idx)
		return
	}
	u.freeStd = append(u.freeStd, idx)
}

func (u *uringIO) outPending(c *conn) bool { return c.sending > 0 || len(c.out) > 0 }

// close cancels the connection's requests and closes its slot. Sends still in flight are
// cancelled too, so callers that want the output delivered wait for outPending to clear.
func (u *uringIO) close(c *conn) {
	if !c.open {
		return
	}
	ring, slot := u.split(c.id)
	C.ur_close(ring, slot, ud(kClose, c.gen, c.id))
	*c = conn{id: c.id, gen: c.gen}
}
