//go:build linux && (amd64 || arm64)

package netring

import (
	"fmt"
	"golang.org/x/sys/unix"
	"time"
)

// Probe exercises multishot variants (REGISTER_PROBE only reports opcodes),
// provided buffer selection and ordinary SEND before the app listener is used.
func Probe() (err error) {
	r, e := Open(64, 4)
	if e != nil {
		return e
	}
	defer func() { err = joinProbe(err, r.Close()) }()
	// The writer's eventfd uses a one-shot poll. Check it on the disposable
	// ring so an unsupported poll path falls back before serving user traffic.
	wake, e := unix.Eventfd(0, unix.EFD_NONBLOCK|unix.EFD_CLOEXEC)
	if e != nil {
		return e
	}
	defer unix.Close(wake)
	r.Poll(wake, unix.POLLIN, 6)
	if _, e = unix.Write(wake, []byte{1, 0, 0, 0, 0, 0, 0, 0}); e != nil {
		return e
	}
	polled := false
	for turn := 0; turn < 10 && !polled; turn++ {
		completions, waitErr := r.Wait(20 * time.Millisecond)
		if waitErr != nil {
			return waitErr
		}
		for _, c := range completions {
			if c.Tag != 6 || c.Result&unix.POLLIN == 0 {
				return fmt.Errorf("writer eventfd poll probe: %+v", c)
			}
			polled = true
		}
	}
	if !polled {
		return fmt.Errorf("writer eventfd poll probe timed out")
	}
	listen, e := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if e != nil {
		return e
	}
	defer unix.Close(listen)
	if e = unix.Bind(listen, &unix.SockaddrInet4{Addr: [4]byte{127, 0, 0, 1}}); e != nil {
		return e
	}
	if e = unix.Listen(listen, 4); e != nil {
		return e
	}
	address, e := unix.Getsockname(listen)
	if e != nil {
		return e
	}
	r.Accept(listen)
	var accepted, clients []int
	// Kernel owns file references independently; sync cancellation in the deferred
	// Ring.Close precedes unmapping. These temporary fds never reach the app.
	defer func() {
		for _, fd := range accepted {
			unix.Close(fd)
		}
		for _, fd := range clients {
			unix.Close(fd)
		}
	}()
	for i := 0; i < 2; i++ {
		fd, e := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_CLOEXEC, 0)
		if e != nil {
			return e
		}
		clients = append(clients, fd)
		if e = unix.Connect(fd, address); e != nil {
			return e
		}
	}
	deadline := time.Now().Add(time.Second)
	for len(accepted) < 2 && time.Now().Before(deadline) {
		es, e := r.Wait(20 * time.Millisecond)
		if e != nil {
			return e
		}
		for _, c := range es {
			if c.Tag != 1 || c.Result < 0 {
				return fmt.Errorf("multishot accept probe: %+v", c)
			}
			accepted = append(accepted, int(c.Result))
			if !c.More() {
				return fmt.Errorf("multishot accept did not remain armed")
			}
		}
	}
	if len(accepted) != 2 {
		return fmt.Errorf("multishot accept probe timed out")
	}
	r.Recv(accepted[0], 2)
	for i := 0; i < 2; i++ {
		if _, e = unix.Write(clients[0], []byte{byte('a' + i)}); e != nil {
			return e
		}
		found := false
		for turn := 0; turn < 10 && !found; turn++ {
			es, e := r.Wait(20 * time.Millisecond)
			if e != nil {
				return e
			}
			for _, c := range es {
				if c.Tag != 2 || c.Result != 1 || !c.More() || !c.HasBuffer() {
					return fmt.Errorf("multishot recv probe: %+v", c)
				}
				if r.Bytes(c)[0] != byte('a'+i) {
					return fmt.Errorf("recv probe data mismatch")
				}
				r.Release(c)
				found = true
			}
		}
		if !found {
			return fmt.Errorf("multishot recv probe timed out")
		}
	}
	if _, e = r.Send(accepted[0], []byte("ok"), 3); e != nil {
		return e
	}
	sent := false
	for turn := 0; turn < 10 && !sent; turn++ {
		es, e := r.Wait(20 * time.Millisecond)
		if e != nil {
			return e
		}
		for _, c := range es {
			if c.Tag != 3 || c.Result != 2 {
				return fmt.Errorf("send probe: %+v", c)
			}
			r.FinishSend(3)
			sent = true
		}
	}
	if !sent {
		return fmt.Errorf("send probe timed out")
	}
	r.Cancel(1, 4)
	r.Cancel(2, 5)
	finals := 0
	for turn := 0; turn < 10 && finals < 4; turn++ {
		es, e := r.Wait(20 * time.Millisecond)
		if e != nil {
			return e
		}
		for _, c := range es {
			if c.HasBuffer() {
				r.Release(c)
			}
			if c.Tag == 4 || c.Tag == 5 {
				if c.Result != 0 && c.Result != -int32(unix.ENOENT) {
					return fmt.Errorf("cancel probe: %+v", c)
				}
				finals++
			} else if (c.Tag == 1 || c.Tag == 2) && !c.More() {
				finals++
			}
		}
	}
	if finals != 4 {
		return fmt.Errorf("cancel probe timed out")
	}
	return nil
}
func joinProbe(a, b error) error {
	if a == nil {
		return b
	}
	if b == nil {
		return a
	}
	return fmt.Errorf("%w; cleanup: %v", a, b)
}
