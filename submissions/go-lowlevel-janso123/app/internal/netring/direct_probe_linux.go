//go:build linux && (amd64 || arm64)

package netring

import (
	"fmt"
	"golang.org/x/sys/unix"
	"time"
)

// ProbeDirect runs before the application listener accepts traffic. A disposable
// table exercises the actual direct variants, since REGISTER_PROBE only reports
// opcodes and cannot distinguish fixed-file or multishot flags.
func ProbeDirect() (err error) {
	r, e := Open(64, 4)
	if e != nil {
		return e
	}
	defer func() { err = joinProbe(err, r.Close()) }()
	if e = r.RegisterDirect(2); e != nil {
		return e
	}
	listen, e := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if e != nil {
		return e
	}
	defer unix.Close(listen)
	if e = unix.Bind(listen, &unix.SockaddrInet4{Addr: [4]byte{127, 0, 0, 1}}); e != nil {
		return e
	}
	if e = unix.Listen(listen, 2); e != nil {
		return e
	}
	address, e := unix.Getsockname(listen)
	if e != nil {
		return e
	}
	client, e := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_CLOEXEC, 0)
	if e != nil {
		return e
	}
	defer unix.Close(client)
	r.Accept(listen)
	if e = unix.Connect(client, address); e != nil {
		return e
	}
	wait := func(tag uint64) (Completion, error) {
		for turn := 0; turn < 20; turn++ {
			es, e := r.Wait(20 * time.Millisecond)
			if e != nil {
				return Completion{}, e
			}
			for _, c := range es {
				if c.Tag == tag {
					return c, nil
				}
				if c.HasBuffer() {
					r.Release(c)
				}
			}
		}
		return Completion{}, fmt.Errorf("direct probe tag %d timed out", tag)
	}
	accepted, e := wait(AcceptTag)
	if e != nil {
		return e
	}
	if accepted.Result < 0 || !accepted.More() {
		return fmt.Errorf("direct multishot accept: %+v", accepted)
	}
	slot := int(accepted.Result)
	r.Recv(slot, 11)
	if _, e = unix.Write(client, []byte("ping")); e != nil {
		return e
	}
	received, e := wait(11)
	if e != nil {
		return e
	}
	if received.Result != 4 || string(r.Bytes(received)) != "ping" {
		return fmt.Errorf("direct recv: %+v", received)
	}
	r.Release(received)
	r.Cancel(11, 12)
	terminal := !received.More()
	for turn := 0; turn < 20 && !terminal; turn++ {
		es, e := r.Wait(20 * time.Millisecond)
		if e != nil {
			return e
		}
		for _, c := range es {
			if c.HasBuffer() {
				r.Release(c)
			}
			if c.Tag == 11 && !c.More() {
				terminal = true
			}
		}
	}
	if !terminal {
		return fmt.Errorf("direct recv cancel timed out")
	}
	if _, e = r.Send(slot, []byte("pong"), 13); e != nil {
		return e
	}
	sent, e := wait(13)
	if e != nil {
		return e
	}
	r.FinishSend(13)
	if sent.Result != 4 {
		return fmt.Errorf("direct send: %+v", sent)
	}
	var out [4]byte
	if n, e := unix.Read(client, out[:]); e != nil || n != 4 || string(out[:]) != "pong" {
		return fmt.Errorf("direct peer read: %d %q %v", n, out, e)
	}
	r.ShutdownDirect(slot, unix.SHUT_WR, 14)
	shut, e := wait(14)
	if e != nil {
		return e
	}
	if shut.Result != 0 {
		return fmt.Errorf("direct shutdown: %+v", shut)
	}
	r.CloseDirect(slot, 15)
	closed, e := wait(15)
	if e != nil {
		return e
	}
	if closed.Result != 0 {
		return fmt.Errorf("direct close: %+v", closed)
	}
	return nil
}
