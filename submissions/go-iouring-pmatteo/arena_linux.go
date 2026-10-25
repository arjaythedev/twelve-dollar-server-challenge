package main

// Off-heap memory.
//
// Two things make the Go heap unusable on the serving path. The kernel reads send buffers
// asynchronously, after io_uring_enter has already returned, so a buffer the GC may move or
// collect is not safe to hand it. And SQLite holds pointers across calls. So every region the
// kernel or SQLite can see is mmapped at startup and addressed through unsafe.Slice: the ring
// memory, the provided buffers, the output buffers, the connection table and the DB result arena.
//
// The Go heap then holds nothing the hot path touches, so it does not grow while the server runs
// and the collector has nothing to walk.

import (
	"fmt"
	"syscall"
)

const pageSize = 4096

// arena is a bump allocator over one anonymous mapping. There is no free: every region it hands
// out lives for the process's lifetime, which is exactly the lifetime of everything it is used
// for.
type arena struct {
	mem  []byte
	used int
}

// newArena maps size bytes, rounded up to a page, and prefaults them. MAP_POPULATE costs startup
// time and buys the absence of a page fault on the first touch of every buffer, which would
// otherwise land in the middle of the first requests served.
func newArena(size int) (*arena, error) {
	size = roundUp(size, pageSize)
	mem, err := syscall.Mmap(-1, 0, size,
		syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_PRIVATE|syscall.MAP_ANONYMOUS|syscall.MAP_POPULATE)
	if err != nil {
		return nil, fmt.Errorf("mmap arena (%d bytes): %w", size, err)
	}
	return &arena{mem: mem}, nil
}

// alloc returns n bytes aligned to align, which must be a power of two.
func (a *arena) alloc(n, align int) ([]byte, error) {
	start := roundUp(a.used, align)
	end := start + n
	if end > len(a.mem) {
		return nil, fmt.Errorf("arena exhausted: need %d bytes at offset %d, have %d",
			n, start, len(a.mem))
	}
	a.used = end
	return a.mem[start:end:end], nil
}

// allocPages returns n bytes starting on a page boundary. Memory registered with the kernel, such
// as a provided buffer ring, has to be page aligned.
func (a *arena) allocPages(n int) ([]byte, error) { return a.alloc(n, pageSize) }

func (a *arena) free() error {
	if a.mem == nil {
		return nil
	}
	m := a.mem
	a.mem, a.used = nil, 0
	return syscall.Munmap(m)
}

func roundUp(v, align int) int { return (v + align - 1) & ^(align - 1) }
