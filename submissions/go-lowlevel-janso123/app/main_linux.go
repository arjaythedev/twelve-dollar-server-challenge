package main

import (
	"errors"
	"fmt"
	"golang.org/x/sys/unix"
	"log"
	"net"
	"os"
	"os/signal"
	"runtime"
	"strconv"
	"syscall"
	"time"
)

func env(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
func listenSocket(host, port string) (int, error) {
	p, e := strconv.Atoi(port)
	if e != nil || p < 0 || p > 65535 {
		return -1, fmt.Errorf("invalid PORT %q", port)
	}
	ips, e := net.LookupIP(host)
	if e != nil {
		return -1, e
	}
	if len(ips) == 0 {
		return -1, fmt.Errorf("HOST resolved to no addresses")
	}
	ip := ips[0]
	family := unix.AF_INET6
	var addr unix.Sockaddr
	if v4 := ip.To4(); v4 != nil {
		family = unix.AF_INET
		a := &unix.SockaddrInet4{Port: p}
		copy(a.Addr[:], v4)
		addr = a
	} else {
		a := &unix.SockaddrInet6{Port: p}
		copy(a.Addr[:], ip.To16())
		addr = a
	}
	fd, e := unix.Socket(family, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if e != nil {
		return -1, e
	}
	fail := func(e error) (int, error) { unix.Close(fd); return -1, e }
	if e = unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEADDR, 1); e != nil {
		return fail(e)
	}
	// Linux clones TCP_NODELAY from the listening socket into accepted sockets.
	// Direct descriptors have no process fd for setsockopt after accept.
	if e = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1); e != nil {
		return fail(e)
	}
	if e = unix.Bind(fd, addr); e != nil {
		return fail(e)
	}
	if e = unix.Listen(fd, 4096); e != nil {
		return fail(e)
	}
	return fd, nil
}
func epollLoop(listener int, a *app, w *writer, stop <-chan os.Signal, onStop func() error) error {
	ep, e := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
	if e != nil {
		return e
	}
	defer unix.Close(ep)
	if e = unix.EpollCtl(ep, unix.EPOLL_CTL_ADD, listener, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(listener)}); e != nil {
		return e
	}
	if e = unix.EpollCtl(ep, unix.EPOLL_CTL_ADD, w.wake, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(w.wake)}); e != nil {
		return e
	}
	var current *connection
	a.submit = func(q writeRequest) reply {
		q.fd = current.fd
		q.generation = current.generation
		if !w.submit(q) {
			return failure(503, "writer unavailable")
		}
		return reply{}
	}
	connections := make(map[int32]*connection)
	pool := bufferPool{}
	var generation uint32
	defer func() {
		for _, c := range connections {
			c.release(&pool)
			unix.Close(int(c.fd))
		}
	}()
	closeConn := func(c *connection) {
		unix.EpollCtl(ep, unix.EPOLL_CTL_DEL, int(c.fd), nil)
		unix.Close(int(c.fd))
		c.release(&pool)
		delete(connections, c.fd)
	}
	update := func(c *connection) error {
		events := uint32(0)
		if c.pending {
			// A FIN can be a request-side half-close. Wait for commit/output;
			// ERR/HUP remain reported by epoll even with an empty mask.
			events = 0
		} else if c.draining {
			events = unix.EPOLLIN | unix.EPOLLRDHUP
		} else if c.output != nil || len(c.input) > 0 { // writable scheduling also resumes a bounded pipeline
			_, n, e := parseRequest(c.input)
			if c.output != nil || n > 0 || e != nil {
				events |= unix.EPOLLOUT
			} else {
				events |= unix.EPOLLIN | unix.EPOLLRDHUP
			}
		} else {
			events |= unix.EPOLLIN | unix.EPOLLRDHUP
		}
		return unix.EpollCtl(ep, unix.EPOLL_CTL_MOD, int(c.fd), &unix.EpollEvent{Events: events, Fd: c.fd, Pad: int32(c.generation)})
	}
	events := make([]unix.EpollEvent, 1024)
	sweep := time.Now().Add(time.Second)
	for {
		select {
		case <-stop:
			return onStop()
		default:
		}
		// Real syscall releases the P; 100ms cap also bounds signal delivery latency.
		n, e := unix.EpollWait(ep, events, 100)
		if e == unix.EINTR {
			continue
		}
		if e != nil {
			return e
		}
		iterationStart := time.Now()
		now := iterationStart
		for _, ev := range events[:n] {
			select {
			case <-stop:
				return onStop()
			default:
			}
			if int(ev.Fd) == w.wake {
				var value [8]byte
				for {
					_, err := unix.Read(w.wake, value[:])
					if err == unix.EINTR {
						continue
					}
					break
				}
			completions:
				for completed := 0; completed < cap(w.results); completed++ {
					select {
					case result := <-w.results:
						<-w.slots
						c := connections[result.fd]
						if c != nil && c.complete(&pool, result) {
							if err := update(c); err != nil {
								closeConn(c)
							}
						}
					default:
						break completions
					}
				}
				continue
			}
			if int(ev.Fd) == listener {
				for accepted := 0; accepted < 128; accepted++ {
					fd, _, e := unix.Accept4(listener, unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC)
					if e == unix.EAGAIN || e == unix.EWOULDBLOCK {
						break
					}
					if e == unix.EINTR {
						continue
					}
					if e == unix.EMFILE || e == unix.ENFILE { // avoid spinning at the descriptor limit
						time.Sleep(10 * time.Millisecond)
						break
					}
					if e != nil {
						return e
					}
					if e = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1); e != nil {
						unix.Close(fd)
						continue
					}
					generation++
					if generation == 0 {
						generation++
					}
					c := &connection{fd: int32(fd), generation: generation, deadline: now.Add(clientTimeout).UnixNano()}
					if e = unix.EpollCtl(ep, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP, Fd: int32(fd), Pad: int32(generation)}); e != nil {
						unix.Close(fd)
						continue
					}
					connections[int32(fd)] = c
				}
				continue
			}
			c := connections[ev.Fd]
			if c == nil || c.generation != uint32(ev.Pad) {
				continue
			}
			if c.pending {
				if ev.Events&(unix.EPOLLHUP|unix.EPOLLERR) != 0 {
					closeConn(c)
				}
				continue
			}
			current = c
			closed := false
			var err error
			if c.draining {
				closed, err = c.drain()
			} else if ev.Events&unix.EPOLLOUT != 0 {
				closed, err = c.drive(&pool, a.handle, now)
			}
			if !c.draining && !closed && err == nil && c.output == nil && ev.Events&(unix.EPOLLIN|unix.EPOLLRDHUP) != 0 {
				closed, err = c.readReady(&pool, a.handle, now)
			}
			if closed && err == nil && c.closing && !c.draining {
				closed, err = c.beginDrain(&pool)
			}
			if closed || err != nil || ev.Events&(unix.EPOLLERR|unix.EPOLLHUP) != 0 {
				closeConn(c)
				continue
			}
			if err = update(c); err != nil {
				closeConn(c)
			}
		}
		now = time.Now()
		if !now.Before(sweep) {
			for _, c := range connections {
				if now.UnixNano() >= c.deadline {
					closeConn(c)
				}
			}
			sweep = now.Add(time.Second)
		}
	}
}
func run() (err error) {
	procs, e := strconv.Atoi(env("GOMAXPROCS", "2"))
	if e != nil || procs < 1 {
		return errors.New("GOMAXPROCS must be a positive integer")
	}
	runtime.GOMAXPROCS(procs)
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	path, secret := os.Getenv("SQLITE_PATH"), os.Getenv("JWT_SECRET")
	if path == "" || secret == "" {
		return errors.New("SQLITE_PATH and JWT_SECRET are required")
	}
	capacity, e := strconv.Atoi(env("WRITE_QUEUE_CAPACITY", strconv.Itoa(defaultWriteQueue)))
	if e != nil || capacity < 1 || capacity > 65536 {
		return errors.New("WRITE_QUEUE_CAPACITY must be between 1 and 65536")
	}
	stats := &writerMetrics{}
	w, e := startWriter(path, []byte(secret), capacity, os.Stderr, stats)
	if e != nil {
		return e
	}
	defer unix.Close(w.wake)
	defer w.shutdown()
	a := w.a
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(stop)
	fd, e := listenSocket(env("HOST", "127.0.0.1"), env("PORT", "3000"))
	if e != nil {
		return e
	}
	defer unix.Close(fd)
	err = loop(fd, a, w, stop, func() error {
		w.shutdown()
		return nil
	})
	return err
}
func main() {
	if e := run(); e != nil {
		log.Print(e)
		os.Exit(1)
	}
}
