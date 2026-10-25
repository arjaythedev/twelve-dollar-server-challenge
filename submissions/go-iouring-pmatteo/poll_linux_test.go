package main

// Can one ring wait on behalf of another?
//
// The multi-ring loop needs a single wait point: the thread blocks on ring 0 and still notices
// work on rings 1..N-1. Two things had to be settled here, and both were got wrong by a test
// that was too easy before these replaced it.
//
// First, the test has to use real socket traffic on a ring this same thread owns. A NOP
// submitted from another goroutine is posted by that other thread, so it wakes the sleeper
// through an ordinary waitqueue and says nothing about the case that matters.
//
// Second, the test has to fire more than once. A multishot poll across rings reports
// IORING_CQE_F_MORE and then delivers exactly one event, whether it watches the ring fd or an
// eventfd the ring is registered with. A one-shot poll re-armed by hand delivers every one.

import (
	"runtime"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func newEventfd(t *testing.T) int {
	t.Helper()
	fd, _, errno := syscall.Syscall(syscall.SYS_EVENTFD2, 0,
		uintptr(syscall.O_CLOEXEC|syscall.O_NONBLOCK), 0)
	if errno != 0 {
		t.Fatal("eventfd2:", errno)
	}
	return int(fd)
}

func registerEventfd(t *testing.T, r *ring, efd int) {
	t.Helper()
	v := int32(efd)
	if _, _, errno := syscall.Syscall6(sysIOUringRegister,
		uintptr(r.fd), regEventfd, uintptr(unsafe.Pointer(&v)), 1, 0, 0); errno != 0 {
		t.Fatal("register eventfd:", errno)
	}
}

// crossRing sets up a waiter ring, a worker ring and a socket whose arrivals complete on the
// worker. It returns the rings, the write end of the socket, and the descriptor to watch.
func crossRing(t *testing.T, useEventfd bool) (waiter, worker *ring, writeFD, watched int) {
	t.Helper()
	waiter = newTestRing(t, 64, 256)
	t.Cleanup(func() { waiter.close() })
	worker = newTestRing(t, 64, 256)
	t.Cleanup(func() { worker.close() })

	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		t.Fatal("socketpair:", err)
	}
	t.Cleanup(func() { syscall.Close(fds[0]); syscall.Close(fds[1]) })

	watched = worker.fd
	if useEventfd {
		efd := newEventfd(t)
		t.Cleanup(func() { syscall.Close(efd) })
		registerEventfd(t, worker, efd)
		watched = efd
	}
	return waiter, worker, fds[1], watched
}

// armRecv queues a plain (not fixed-file) receive on the worker ring.
func armRecv(t *testing.T, worker *ring, fd int, buf []byte, userData uint64) {
	t.Helper()
	s := worker.getSQE()
	s.opcode = opRecv
	s.fd = int32(fd)
	s.addr = uint64(uintptr(unsafe.Pointer(&buf[0])))
	s.length = uint32(len(buf))
	s.userData = userData
	if _, err := worker.submit(); err != nil {
		t.Fatal("worker submit:", err)
	}
}

func TestCrossRingWakeupRepeats(t *testing.T) {
	type variant struct {
		name     string
		oneshot  bool
		eventfd  bool
		level    bool
		required bool
	}
	// The level variants are the control the first version of this test was missing. A multishot
	// poll without IORING_POLL_ADD_LEVEL is edge triggered, so "fires once" is the documented
	// behaviour of the flag combination, not of multishot - and the loop's one-shot re-arm would
	// be working around a flag it never set.
	for _, v := range []variant{
		{"ringfd-multishot", false, false, false, false},
		{"ringfd-multishot-level", false, false, true, false},
		{"ringfd-oneshot", true, false, false, true},
		{"eventfd-multishot", false, true, false, false},
		{"eventfd-multishot-level", false, true, true, false},
		{"eventfd-oneshot", true, true, false, true},
	} {
		t.Run(v.name, func(t *testing.T) {
			runtime.LockOSThread()
			defer runtime.UnlockOSThread()

			waiter, worker, writeFD, watched := crossRing(t, v.eventfd)
			readFD := writeFD - 1 // Socketpair returns adjacent descriptors; fds[0] is the peer

			arm := func() {
				flags := uint32(pollAddMulti)
				if v.oneshot {
					flags = 0
				}
				if v.level {
					flags |= pollAddLevel
				}
				s := waiter.getSQE()
				s.prepPollAdd(int32(watched), pollIn, flags, 0x1111)
				if _, err := waiter.submit(); err != nil {
					t.Fatal("arm poll:", err)
				}
			}
			arm()

			buf := make([]byte, 64)
			const rounds = 5
			woke := 0
			for i := 0; i < rounds; i++ {
				armRecv(t, worker, readFD, buf, uint64(0x7000+i))
				go func() {
					time.Sleep(20 * time.Millisecond)
					syscall.Write(writeFD, []byte("hello"))
				}()

				ts := timespec{nsec: int64(300 * time.Millisecond)}
				tsq := waiter.getSQE()
				tsq.prepTimeout(&ts, 0xDEAD)
				if _, err := waiter.enter(1, enterGetEvents); err != nil && err != syscall.ETIME {
					t.Fatal("waiter enter:", err)
				}

				got := false
				waiter.forEachCQE(func(c *cqe) {
					if c.userData == 0x1111 {
						got = true
					}
				})
				worker.forEachCQE(func(c *cqe) {})
				if !got {
					continue
				}
				woke++
				if v.eventfd {
					var scratch [8]byte
					syscall.Read(watched, scratch[:])
				}
				if v.oneshot {
					arm()
				}
			}

			t.Logf("%s: woke on %d of %d arrivals", v.name, woke, rounds)
			if v.required {
				if woke < rounds {
					t.Errorf("%s misses cross-ring wakeups: %d of %d. The loop has no way to "+
						"notice work on a secondary ring.", v.name, woke, rounds)
				}
				return
			}
			// The negative result asserts too. It is the whole reason armPoll re-arms a one-shot
			// poll by hand, so if a kernel ever starts delivering these, that comment and this
			// loop are what need revisiting - and a passing test would hide it.
			if woke >= rounds {
				t.Errorf("%s now delivers every cross-ring wakeup (%d of %d). Multishot was "+
					"measured to deliver exactly one; armPoll re-arms a one-shot poll because "+
					"of it and can now be simplified.", v.name, woke, rounds)
			}
		})
	}
}
