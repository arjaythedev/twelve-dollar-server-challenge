//go:build linux

package main

// What the cgroup says about memory, read without allocating.

import (
	"bytes"
	"syscall"
)

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
