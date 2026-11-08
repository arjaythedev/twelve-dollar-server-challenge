package main

import (
	"fmt"
	"net"
	"os"
	"runtime"
	"strconv"
	"syscall"

	"github.com/godzie44/go-uring/uring"
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
	var limit syscall.Rlimit
	if syscall.Getrlimit(syscall.RLIMIT_NOFILE, &limit) == nil {
		limit.Cur = limit.Max
		syscall.Setrlimit(syscall.RLIMIT_NOFILE, &limit)
	}
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return err
	}
	defer syscall.Close(fd)
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		return err
	}
	addr := &syscall.SockaddrInet4{Port: port}
	copy(addr.Addr[:], ip)
	if err := syscall.Bind(fd, addr); err != nil {
		return err
	}
	if err := syscall.Listen(fd, 65535); err != nil {
		return err
	}
	ring, err := uring.New(256)
	if err != nil {
		return fmt.Errorf("io_uring: %w", err)
	}
	defer ring.Close()
	s := server{ring: ring, app: newApp([]byte(env("JWT_SECRET", "twelve-dollar-challenge"))),
		clients: make(map[int]*connection), accept: uring.Accept(uintptr(fd), syscall.SOCK_CLOEXEC)}
	fmt.Fprintf(os.Stderr, "listening on %s:%d (go-uring, C SQLite)\n", host, port)
	return s.run()
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
