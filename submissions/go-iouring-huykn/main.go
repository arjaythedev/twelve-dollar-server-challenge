package main

import (
	"fmt"
	"net"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"syscall"
)

func env(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func run() error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	runtime.GOMAXPROCS(1)
	debug.SetGCPercent(-1)
	debug.SetMemoryLimit(512 << 20)
	host := env("HOST", "127.0.0.1")
	port, err := strconv.Atoi(env("PORT", "3000"))
	if err != nil || port < 1 || port > 65535 {
		return fmt.Errorf("invalid PORT")
	}
	ip := net.ParseIP(host).To4()
	if ip == nil {
		return fmt.Errorf("HOST must be an IPv4 address")
	}
	if err := dbOpen(env("SQLITE_PATH", "feed.db")); err != nil {
		return err
	}
	defer dbClose()
	slots, err := descriptorSlots()
	if err != nil {
		return err
	}
	s := server{app: newApp([]byte(env("JWT_SECRET", "twelve-dollar-challenge"))), slots: slots}
	defer s.close()
	for i := 0; i < listeners; i++ {
		fd, err := listener(ip, port)
		if err != nil {
			return err
		}
		s.listen = append(s.listen, fd)
	}
	r, err := newRing(slots)
	if err != nil {
		return err
	}
	s.rings = []*ring{r}
	r.accepting = true
	s.fillAccepts(0)
	fmt.Fprintf(os.Stderr, "listening on %s:%d (direct io_uring, %d slots per ring, C SQLite)\n", host, port, slots)
	return s.run()
}

func descriptorSlots() (uint32, error) {
	var limit syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &limit); err != nil {
		return 0, err
	}
	limit.Cur = limit.Max
	if err := syscall.Setrlimit(syscall.RLIMIT_NOFILE, &limit); err != nil {
		return 0, err
	}
	return uint32(min(limit.Cur, 1<<20)), nil
}

// Direct descriptors cannot use setsockopt: accepted sockets inherit TCP_NODELAY.
func listener(ip net.IP, port int) (int, error) {
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return -1, err
	}
	bound := false
	defer func() {
		if !bound {
			syscall.Close(fd)
		}
	}()
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		return -1, err
	}
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, 0xf /* SO_REUSEPORT */, 1); err != nil {
		return -1, err
	}
	if err := syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1); err != nil {
		return -1, err
	}
	addr := &syscall.SockaddrInet4{Port: port}
	copy(addr.Addr[:], ip)
	if err := syscall.Bind(fd, addr); err != nil {
		return -1, err
	}
	if err := syscall.Listen(fd, 65535); err != nil {
		return -1, err
	}
	bound = true
	return fd, nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
