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
func epollLoop(listener int, a *app, w *writer, stop <-chan os.Signal, onStop func() error, g *memGuard) error {
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
			_, n, e := parseRequestCheck(c.input)
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
	// memBlocked is the memory/hard-cap/stale gate (owning the pool and idle
	// release); listenerGated additionally masks the listener for the duration
	// of the gradual re-admission budget, so the accept path cannot spin while
	// re-admission tokens refill.
	memBlocked := false
	listenerGated := false
	// applyGuard evaluates the memory guard and applies the accept gate. It runs
	// at least once per loop iteration — before the wait and again after each
	// event batch — so a watermark or cap crossed mid-batch takes effect in the
	// same iteration. On engage the process pool is dropped and idle
	// connections release their input buffers (reallocated lazily); while
	// shedding it also closes idle connections in bounded batches.
	applyGuard := func() error {
		now := time.Now()
		blocked := g.update(len(connections), now)
		if blocked != memBlocked {
			memBlocked = blocked
			pool.setBlocked(blocked)
			if blocked {
				for _, c := range connections {
					c.releaseIdle(&pool)
				}
			}
		}
		gate := blocked || g.reAdmitGated(now)
		if gate != listenerGated {
			listenerGated = gate
			interest := uint32(unix.EPOLLIN)
			if gate {
				interest = 0
			}
			if e := unix.EpollCtl(ep, unix.EPOLL_CTL_MOD, listener, &unix.EpollEvent{Events: interest, Fd: int32(listener)}); e != nil {
				return e
			}
		}
		// Emergency shedding: close idle keep-alive connections in bounded
		// batches. Active connections (response in flight or writer job queued)
		// are never touched here; they are closed on their own event once the
		// in-flight response has been fully sent (see the per-connection path).
		if g.shedSweepDue(now) {
			shedSweepIdle(connections, memGuardShedBatch, closeConn, func() { g.noteShedClose(true) })
		}
		return nil
	}
	for {
		select {
		case <-stop:
			return onStop()
		default:
		}
		// Memory guard: drop or restore EPOLLIN interest on the listener. With
		// interest removed the kernel stops delivering accept readiness and new
		// clients wait in the SYN/accept backlog (bounded, tiny per-connection
		// cost) instead of allocating server memory; clients see connect
		// timeouts rather than resets. Established connections are unaffected.
		if e = applyGuard(); e != nil {
			return e
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
				if listenerGated {
					// A readiness event queued before the interest change.
					continue
				}
				for accepted := 0; accepted < 128; accepted++ {
					// Re-evaluate the guard before every accept against the
					// newest published sample (one atomic load), not just the
					// precomputed gate, so a watermark crossed by this very
					// batch stops it. admit additionally charges the gradual
					// re-admission token bucket; when it is empty the batch
					// stops for this tick and the listener is masked.
					if g.acceptBlocked(len(connections), now) || !g.admit(now) {
						break
					}
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
			// Emergency shedding: a connection whose in-flight response has
			// now been fully sent (no pending writer job, no buffered output,
			// no partial request) is closed instead of being re-armed for recv.
			if g.shedding() && c.shedIdle() {
				g.noteShedClose(false)
				closeConn(c)
				continue
			}
			if err = update(c); err != nil {
				closeConn(c)
			}
		}
		// Re-evaluate after the batch: an accept batch or a wave of closes may
		// have moved the open count across the cap, or the sampler may have
		// crossed a watermark; apply the gate before the next blocking wait.
		if e = applyGuard(); e != nil {
			return e
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
	// Registered before any SQLite owner starts: later defers close the loop,
	// drain/join the writer and join its checkpointer before the training dump.
	// The default and profile-use builds compile this call to a no-op.
	if e := startSQLiteProfile(); e != nil {
		return e
	}
	defer func() {
		if e := dumpSQLiteProfile(); e != nil {
			err = errors.Join(err, e)
		}
	}()
	if e := configureSQLiteMemoryStatus(); e != nil {
		return e
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
	profiles, e := startProfiles(os.Getenv("CPU_PROFILE"), os.Getenv("HEAP_PROFILE"), os.Stderr)
	if e != nil {
		return e
	}
	defer func() {
		if e := profiles.Close(); e != nil && !errors.Is(err, e) {
			err = errors.Join(err, e)
		}
	}()
	fd, e := listenSocket(env("HOST", "127.0.0.1"), env("PORT", "3000"))
	if e != nil {
		return e
	}
	defer unix.Close(fd)
	g, e := startMemGuard(os.Stderr)
	if e != nil {
		return e
	}
	defer g.stop()
	err = loop(fd, a, w, stop, func() error {
		// Snapshot the serving heap before writer and loop resources are released.
		// A profile error must not skip the existing writer drain.
		e := profiles.Close()
		w.shutdown()
		return e
	}, g)
	return err
}
func main() {
	if e := run(); e != nil {
		log.Print(e)
		os.Exit(1)
	}
}
