// A feed API server for the $12 server challenge: Go, io_uring, SQLite. See README.md.
package main

import (
	"fmt"
	"net"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"syscall"
	"time"
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func die(f string, a ...any) {
	fmt.Fprintf(os.Stderr, f+"\n", a...)
	os.Exit(1)
}

// listener opens one SO_REUSEPORT listening socket; accepted sockets inherit TCP_NODELAY.
func listener(ip net.IP, port int) (int, error) {
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return -1, err
	}
	syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, 0xf /* SO_REUSEPORT */, 1); err != nil {
		return -1, err
	}
	syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
	var a syscall.SockaddrInet4
	a.Port = port
	copy(a.Addr[:], ip.To4())
	if err := syscall.Bind(fd, &a); err != nil {
		return -1, err
	}
	if err := syscall.Listen(fd, 65535); err != nil {
		return -1, err
	}
	return fd, nil
}

// slots raises the soft descriptor limit to the hard one: a ring's direct-descriptor table may be
// as large as it.
func slots() uint32 {
	var rl syscall.Rlimit
	syscall.Getrlimit(syscall.RLIMIT_NOFILE, &rl)
	rl.Cur = rl.Max
	syscall.Setrlimit(syscall.RLIMIT_NOFILE, &rl)
	syscall.Getrlimit(syscall.RLIMIT_NOFILE, &rl)
	return uint32(min(rl.Cur, 1<<20))
}

func main() {
	// One thread does all the work and the hot path allocates nothing, so the collector only
	// runs if the heap ever passes the limit.
	runtime.LockOSThread()
	debug.SetGCPercent(-1)
	debug.SetMemoryLimit(512 << 20)

	path := env("SQLITE_PATH", "feed.db")
	host := env("HOST", "127.0.0.1")
	port, err := strconv.Atoi(env("PORT", "3000"))
	if err != nil {
		die("PORT: %v", err)
	}
	ip := net.ParseIP(host).To4()
	if ip == nil {
		die("HOST must be an IPv4 address")
	}
	if err := dbOpen(path); err != nil {
		die("database: %v", err)
	}
	s := &server{app: newApp([]byte(os.Getenv("JWT_SECRET"))), slots: slots()}
	s.app.held = writesHeld()
	for i := 0; i < listeners; i++ {
		fd, err := listener(ip, port)
		if err != nil {
			die("listen %s:%d: %v", host, port, err)
		}
		s.listen = append(s.listen, fd)
	}
	r, err := newRing(s.slots)
	if err != nil {
		die("io_uring: %v", err)
	}
	s.rings = []*ring{r}
	r.accepting = true
	s.fillAccepts(0)
	s.spillAfter = time.Now()
	fmt.Fprintf(os.Stderr, "listening on %s:%d; io_uring, %d direct descriptors per ring\n", host, port, s.slots)
	s.run()
}
