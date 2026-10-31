package main

import "base:runtime"
import "core:fmt"
import "core:mem"
import "core:os"
import "core:sys/linux"

import http "http:."

// Everything the hot path needs is allocated once at startup, aligned to 2 MiB, advised as huge
// pages and touched, so serving never page-faults or grows the heap.

MAX_CONNS           :: 65536
REQUEST_BLOCKS      :: 1024     // 16 KiB each, borrowed only while a request is in flight
SCANNER_BUF_SIZE    :: 1024     // odin-http's INIT_BUF_SIZE
OUT_BUF_SIZE        :: 256 * mem.Kilobyte
SQLITE_CACHE_PAGES  :: 8192
SQLITE_PAGE_SIZE    :: 4096

HUGE_PAGE :: 2 * mem.Megabyte

slab_alloc :: proc(size: int) -> []byte {
	size := mem.align_forward_int(size, HUGE_PAGE)
	raw, errno := linux.mmap(0, uint(size + HUGE_PAGE), {.READ, .WRITE}, {.PRIVATE, .ANONYMOUS})
	if errno != nil {
		fmt.eprintfln("mmap of %M failed: %v", size, errno)
		os.exit(1)
	}
	base := ([^]byte)(uintptr(mem.align_forward_uintptr(uintptr(raw), HUGE_PAGE)))
	linux.madvise(base, uint(size), .HUGEPAGE)
	for i := 0; i < size; i += 4096 {
		(^u8)(&base[i])^ = 0 // fault every page in now
	}
	return base[:size]
}

// A free list of equal-sized slots carved from a slab.
Slab :: struct {
	free:       rawptr,
	start, end: uintptr,
	slot:       int,
}

slab_init :: proc(s: ^Slab, slot, count: int) {
	s.slot = mem.align_forward_int(slot, 64)
	region := slab_alloc(s.slot * count)
	s.start = uintptr(raw_data(region))
	s.end = s.start + uintptr(s.slot * count)
	for i := count - 1; i >= 0; i -= 1 {
		p := rawptr(s.start + uintptr(i * s.slot))
		(^rawptr)(p)^ = s.free
		s.free = p
	}
}

slab_owns :: #force_inline proc(s: ^Slab, p: rawptr) -> bool {
	return uintptr(p) >= s.start && uintptr(p) < s.end
}

// odin-http's connection allocator: connection structs and scanner buffers come from two slabs,
// anything else (or anything once a slab runs dry) from the heap.
Conn_Allocator :: struct {
	conns:   Slab,
	buffers: Slab,
}

conn_allocator_init :: proc(a: ^Conn_Allocator) -> mem.Allocator {
	slab_init(&a.conns, size_of(http.Connection), MAX_CONNS)
	slab_init(&a.buffers, SCANNER_BUF_SIZE, MAX_CONNS)
	return {procedure = conn_allocator_proc, data = a}
}

@(private = "file")
conn_alloc :: proc(a: ^Conn_Allocator, size, alignment: int, zero: bool) -> ([]byte, mem.Allocator_Error) {
	if size <= 0 { return nil, nil }
	if alignment <= 64 {
		small, large := &a.conns, &a.buffers
		if small.slot > large.slot { small, large = large, small }
		s := small if size <= small.slot else large
		if size <= s.slot && s.free != nil {
			p := s.free
			s.free = (^rawptr)(p)^
			if zero { mem.zero(p, size) }
			return mem.byte_slice(p, size), nil
		}
	}
	if zero {
		return mem.alloc_bytes(size, alignment, runtime.heap_allocator())
	}
	return mem.alloc_bytes_non_zeroed(size, alignment, runtime.heap_allocator())
}

@(private = "file")
conn_free :: proc(a: ^Conn_Allocator, p: rawptr) {
	if p == nil { return }
	for s in ([]^Slab{&a.conns, &a.buffers}) {
		if slab_owns(s, p) {
			(^rawptr)(p)^ = s.free
			s.free = p
			return
		}
	}
	mem.free(p, runtime.heap_allocator())
}

@(private = "file")
conn_slot_size :: proc(a: ^Conn_Allocator, p: rawptr) -> int {
	if slab_owns(&a.conns, p) { return a.conns.slot }
	if slab_owns(&a.buffers, p) { return a.buffers.slot }
	return 0
}

conn_allocator_proc :: proc(
	allocator_data: rawptr,
	mode: mem.Allocator_Mode,
	size, alignment: int,
	old_memory: rawptr,
	old_size: int,
	loc := #caller_location,
) -> (bytes: []byte, err: mem.Allocator_Error) {
	a := (^Conn_Allocator)(allocator_data)
	switch mode {
	case .Alloc, .Alloc_Non_Zeroed:
		return conn_alloc(a, size, alignment, mode == .Alloc)
	case .Free:
		conn_free(a, old_memory)
		return nil, nil
	case .Resize, .Resize_Non_Zeroed:
		zero := mode == .Resize
		if old_memory == nil {
			return conn_alloc(a, size, alignment, zero)
		}
		if size <= 0 {
			conn_free(a, old_memory)
			return nil, nil
		}
		if size <= conn_slot_size(a, old_memory) {
			if zero && size > old_size { mem.zero(rawptr(uintptr(old_memory) + uintptr(old_size)), size - old_size) }
			return mem.byte_slice(old_memory, size), nil
		}
		new_memory := conn_alloc(a, size, alignment, false) or_return
		copy(new_memory, mem.byte_slice(old_memory, min(old_size, size)))
		if zero && size > old_size { mem.zero_slice(new_memory[old_size:]) }
		conn_free(a, old_memory)
		return new_memory, nil
	case .Free_All, .Query_Info:
		return nil, .Mode_Not_Implemented
	case .Query_Features:
		set := (^mem.Allocator_Mode_Set)(old_memory)
		if set != nil {
			set^ = {.Alloc, .Alloc_Non_Zeroed, .Free, .Resize, .Resize_Non_Zeroed, .Query_Features}
		}
		return nil, nil
	}
	return nil, nil
}

// Descriptors kept free above `keep_alive_max` for reconnecting users, the database and its WAL.
FD_RESERVE :: 4096

// Open connections above which a response closes its connection (see `send`).
keep_alive_max: int

// Raise the soft descriptor limit to the hard limit (the service gets LimitNOFILE=65535).
raise_nofile_limit :: proc() {
	lim: linux.RLimit
	if linux.getrlimit(.NOFILE, &lim) == nil && lim.cur < lim.max {
		lim.cur = lim.max
		if linux.setrlimit(.NOFILE, &lim) != nil { linux.getrlimit(.NOFILE, &lim) }
	}
	keep_alive_max = int(lim.cur) - FD_RESERVE
}
