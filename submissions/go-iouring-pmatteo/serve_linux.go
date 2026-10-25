//go:build linux

package main

import (
	"bytes"
	"fmt"
	"math"
	"net"
	"os"
	"runtime"
	"strconv"
	"strings"
	"syscall"
)

// maxRingSlots caps the registered file table this server asks for per ring. The kernel's own
// ceiling is far higher (IORING_MAX_FIXED_FILES is 1<<20 on 6.x), and nothing here needs a
// 16-bit index: user_data carries a 32-bit one. This is a memory choice. The connection table is
// rings*slots entries, and eight rings at this cap is already 25 MB of it.
const maxRingSlots = 65535

// Loop sizing that nothing varies. Each was an environment knob, but the rig and the box only
// ever ran the measured value, so they are constants: a knob nothing turns is one more way for
// the shipped configuration to differ from the measured one.
const (
	sqEntriesPerRing = 1024
	cqEntriesPerRing = 8192
	// Response buffers are only held while a send is in flight, so this bounds concurrent
	// in-flight responses, not connections. Since outSize covers the largest response the spec
	// allows (maxRespBytes, ~68 KB), the pool is the biggest single item in the arena: 1024 of
	// them was 69 MB of prefaulted memory to cover a burst one thread cannot produce.
	// outExhausted counts the case where this is too small.
	outBufCount  = 512
	pendBufCount = 256
	// Deep enough that an arrival waits here for the microseconds a close takes to free a slot.
	listenBacklog = 4096
)

// memLimit reports how many bytes this process may use in total, so the loop can work out how
// many connections that allows rather than how many descriptor slots it has. Zero means unknown,
// and the loop then falls back to the slot count.
//
// The cgroup limit is the number that matters, because it is what the OOM killer acts on and what
// the benchmark reports, and it is what SLAB_ACCOUNT charges socket memory against.
func memLimit() int {
	if b, err := os.ReadFile("/sys/fs/cgroup/memory.max"); err == nil {
		// "max" means the cgroup is unlimited, so the machine's memory is the real bound.
		if n, err := strconv.ParseInt(strings.TrimSpace(string(b)), 10, 64); err == nil && n > 0 {
			return int(n)
		}
	}
	b, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(b), "\n") {
		if !strings.HasPrefix(line, "MemTotal:") {
			continue
		}
		f := strings.Fields(line)
		if len(f) < 2 {
			break
		}
		if kb, err := strconv.ParseInt(f[1], 10, 64); err == nil {
			return int(kb) * 1024
		}
		break
	}
	return 0
}

// raiseNOFILE lifts the soft descriptor limit to the hard one, which needs no privilege.
//
// Capacity here is the size of the registered file table, and io_sqe_files_register checks the
// requested size against the *soft* RLIMIT_NOFILE (io_uring/rsrc.c). So whatever soft limit the
// supervisor happened to set decides how many connections this server can hold: started from a
// stock systemd unit, which ships 1024 soft against 524288 hard, it would quietly serve a
// thousand per ring instead of tens of thousands and nothing would say so.
//
// It buys nothing under the scoring harness, which starts the service with LimitNOFILE=65535 -
// a single systemd value sets soft and hard to the same number, and lifting the hard limit needs
// CAP_SYS_RESOURCE. That is the point: this makes capacity depend on the hard limit alone, so
// the server behaves the same whether it is started by that unit, a stock unit, or a shell.
func raiseNOFILE() {
	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err != nil {
		return
	}
	if lim.Cur >= lim.Max {
		return
	}
	lim.Cur = lim.Max
	_ = syscall.Setrlimit(syscall.RLIMIT_NOFILE, &lim)
}

func serve(cfg config) error {
	// The thread is locked before the database is opened, because the thread that opens it is
	// the only one allowed to enter it afterwards. Everything from here to e.run() stays on it.
	runtime.LockOSThread()

	// Before slots are derived from it: defaultSlots reads the soft limit.
	raiseNOFILE()

	slots := cfg.slots
	if slots == 0 {
		slots = defaultSlots()
	}

	// Bind before opening the database so a port clash fails fast and start.sh does not look
	// healthy for a moment first.
	listenFD, err := listenTCP(cfg.host, cfg.port, listenBacklog)
	if err != nil {
		return err
	}
	s, err := newServerState(cfg)
	if err != nil {
		return err
	}
	memLim := memLimit()
	e, err := newEngine(s, listenFD, engineConfig{
		rings:      cfg.rings,
		slots:      slots,
		sqEntries:  sqEntriesPerRing,
		cqEntries:  cqEntriesPerRing,
		bufCount:   cfg.bufCount,
		bufSize:    cfg.bufSize,
		outCount:   outBufCount,
		pendCount:  pendBufCount,
		rearmSlots: cfg.rings * int(slots),
		logEvery:   cfg.statsEvery,
		tickNS:     int64(cfg.tickMs) * 1e6,
		memLimit:   memLim,
	})
	if err != nil {
		return err
	}

	fmt.Fprintf(os.Stderr, "go-iouring-pmatteo: listening on %s:%s, db %s, GOMAXPROCS=%d\n",
		cfg.host, cfg.port, cfg.sqlitePath, runtime.GOMAXPROCS(0))
	fmt.Fprintf(os.Stderr,
		"io_uring: %d rings x %d slots = %d connections, "+
			"%d x %d B receive buffers per ring, features=%#x\n",
		cfg.rings, slots, cfg.rings*int(slots), cfg.bufCount, cfg.bufSize,
		e.rings[0].r.features)
	if e.shedSoft == math.MaxUint32 {
		fmt.Fprintln(os.Stderr, "shedding: off, no memory limit to derive a ceiling from")
	} else {
		fmt.Fprintf(os.Stderr, "shedding: Connection: close above %d connections, "+
			"idle sweep tightens above %d\n", e.shedSoft, e.shedHard)
	}
	switch {
	case e.mem != nil:
		fmt.Fprintf(os.Stderr, "memory guard: %d MB limit, shedding above %d MB unreclaimable, "+
			"sweeping above %d MB\n", memLim>>20, e.memHigh>>20, e.memCrit>>20)
	case memLim > 0:
		// The limit is known but the probe is not readable, so the guard is off and only the
		// slot thresholds remain. Silence here would look identical to having no limit at all.
		fmt.Fprintln(os.Stderr, "memory guard: off, /sys/fs/cgroup/memory.stat is not readable")
	}
	return e.run()
}

// defaultSlots sizes one ring's registered file table.
//
// The kernel caps a table at RLIMIT_NOFILE entries even though the connections in it never
// become descriptors. That cap is per table, which is the whole reason this server runs more
// than one ring.
func defaultSlots() uint32 {
	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err != nil {
		return 1024
	}
	n := lim.Cur
	if n > maxRingSlots {
		n = maxRingSlots
	}
	// Leave room for the listener, the ring descriptors, the database and stdio.
	if n > 64 {
		n -= 64
	}
	return uint32(n)
}

// ---- from listen_linux.go ------------------------------------------------------------------------------

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

// ---- from mem_linux.go ---------------------------------------------------------------------------------

// memProbe samples the cgroup's unreclaimable memory.
//
// Only unreclaimable memory can kill the process. The largest consumer on this box is page cache
// - the database file, plus the write-ahead log, which the loop's tick checkpoints and nothing
// else bounds - and the kernel reclaims that under pressure instead of invoking the OOM killer.
// Counting it as pressure would make the server shed connections to make room for a cache, which
// is backwards. A 5-minute hold, before that tick checkpoint existed, measured 238 MB of database
// cache and a WAL growing about 1 MB/s against 148 MB of everything else, so the distinction is
// most of the number.
//
// The file is opened once and re-read with pread, because the loop samples it every tick and
// opening a file per tick would allocate under a collector that is switched off.
type memProbe struct {
	fd  int
	buf [8192]byte
}

func newMemProbe() *memProbe {
	fd, err := syscall.Open("/sys/fs/cgroup/memory.stat", syscall.O_RDONLY, 0)
	if err != nil {
		return nil
	}
	return &memProbe{fd: fd}
}

// sample returns the cgroup's unreclaimable bytes, or 0 when the file cannot be read.
func (m *memProbe) sample() int {
	n, err := syscall.Pread(m.fd, m.buf[:], 0)
	if err != nil || n <= 0 {
		return 0
	}
	total := 0
	for b := m.buf[:n]; len(b) > 0; {
		nl := bytes.IndexByte(b, '\n')
		if nl < 0 {
			nl = len(b)
		}
		line := b[:nl]
		b = b[min(nl+1, len(b)):]
		sp := bytes.IndexByte(line, ' ')
		if sp < 0 {
			continue
		}
		// The fields the kernel cannot free on demand: the Go heap and the arena (anon), the
		// socket and file structures (slab_unreclaimable), socket buffers (sock), and the
		// per-connection kernel bookkeeping. Comparing a byte slice against a string constant
		// does not copy it, in a switch exactly as in an ==.
		switch string(line[:sp]) {
		case "anon", "slab_unreclaimable", "sock", "kernel_stack", "pagetables", "percpu":
			total += parseDecimal(line[sp+1:])
		}
	}
	return total
}

// parseDecimal reads an unsigned decimal without allocating. A field that is not one reads as 0,
// which is the right answer for a pressure sample: it understates, so it cannot shed spuriously.
func parseDecimal(b []byte) int {
	v := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return v
		}
		v = v*10 + int(c-'0')
	}
	return v
}
