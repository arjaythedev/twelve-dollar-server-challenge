package main

// The listening socket, created by hand.
//
// net.Listen would register the descriptor with the Go runtime's network poller, which has no
// part to play here: the only thing that ever touches this socket is a multishot accept.

import (
	"fmt"
	"net"
	"syscall"
)

func listenTCP(host, port string, backlog int) (int, error) {
	addr, err := net.ResolveTCPAddr("tcp", net.JoinHostPort(host, port))
	if err != nil {
		return -1, fmt.Errorf("resolve %s:%s: %w", host, port, err)
	}

	domain := syscall.AF_INET
	var sa syscall.Sockaddr
	if ip4 := addr.IP.To4(); ip4 != nil || addr.IP == nil {
		s := &syscall.SockaddrInet4{Port: addr.Port}
		copy(s.Addr[:], ip4)
		sa = s
	} else {
		domain = syscall.AF_INET6
		s := &syscall.SockaddrInet6{Port: addr.Port}
		copy(s.Addr[:], addr.IP.To16())
		sa = s
	}

	fd, err := syscall.Socket(domain, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return -1, fmt.Errorf("socket: %w", err)
	}
	fail := func(what string, err error) (int, error) {
		syscall.Close(fd)
		return -1, fmt.Errorf("%s: %w", what, err)
	}
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		return fail("SO_REUSEADDR", err)
	}
	// Non-blocking so a multishot accept always goes through io_uring's internal poll rather
	// than being punted to a kernel worker thread.
	if err := syscall.SetNonblock(fd, true); err != nil {
		return fail("O_NONBLOCK", err)
	}
	if err := syscall.Bind(fd, sa); err != nil {
		return fail(fmt.Sprintf("bind %s", addr), err)
	}
	if err := syscall.Listen(fd, backlog); err != nil {
		return fail("listen", err)
	}
	return fd, nil
}
