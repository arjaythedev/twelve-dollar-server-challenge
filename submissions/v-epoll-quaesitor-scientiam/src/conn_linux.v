module main

// Per-connection state and the buffer pool shared by the epoll and io_uring backends.

const pool_max = 256
const pool_buf_max = u32(16 * 1024)

// Buf is a malloc'd byte range owned by one connection; ptr is nil while idle.
struct Buf {
mut:
	ptr &u8 = unsafe { nil }
	len u32
	cap u32
}

enum CState as u32 {
	free
	open    // reading requests
	parked  // output is backed up: not reading (epoll: waiting for EPOLLOUT; io_uring: receive cancelled until the queue drains)
	closing // flush what is pending, then close
	closed  // io_uring: the close is submitted; waiting for its completion
}

// Conn is the whole per-connection state: 64 bytes, one cache line.
struct Conn {
mut:
	input      Buf // bytes of a request that has not fully arrived
	output     Buf // response bytes not yet handed to the socket
	flight     Buf // io_uring: the buffer a send operation currently owns
	sent       u32 // bytes of flight (io_uring) or output (epoll) already written
	generation u32
	touched    u32 // seconds since start when the connection last made progress
	state      CState
}

// Seg is one response in the batch arena, owned by connection fd (or slot).
struct Seg {
	fd    int
	start int
	end   int
	wrote bool
}

// BufPool recycles freed connection buffers so steady-state traffic does not malloc.
struct BufPool {
mut:
	free []Buf
}

// take returns a buffer of at least need bytes, from the pool when one fits.
fn (mut p BufPool) take(need u32) Buf {
	for i := p.free.len - 1; i >= 0; i-- {
		if p.free[i].cap >= need {
			b := p.free[i]
			// remove entry i (not the last one): move the last entry into its place
			p.free[i] = p.free[p.free.len - 1]
			p.free.delete_last()
			return Buf{
				ptr: b.ptr
				cap: b.cap
			}
		}
	}
	mut cap := u32(4096)
	for cap < need {
		cap *= 2
	}
	return Buf{
		ptr: unsafe { &u8(C.malloc(cap)) }
		cap: cap
	}
}

// give returns a buffer to the pool (or frees it when the pool is full or it is large).
fn (mut p BufPool) give(b Buf) {
	if b.ptr == unsafe { nil } {
		return
	}
	if p.free.len < pool_max && b.cap <= pool_buf_max {
		p.free << Buf{
			ptr: b.ptr
			cap: b.cap
		}
		return
	}
	unsafe { C.free(b.ptr) }
}

// append grows b as needed and appends n bytes from src.
fn (mut p BufPool) append(mut b Buf, src &u8, n int) {
	need := b.len + u32(n)
	if b.ptr == unsafe { nil } {
		nb := p.take(need)
		b.ptr = nb.ptr
		b.cap = nb.cap
		b.len = 0
	} else if need > b.cap {
		mut cap := b.cap
		for cap < need {
			cap *= 2
		}
		b.ptr = unsafe { &u8(C.realloc(b.ptr, cap)) }
		b.cap = cap
	}
	unsafe { vmemcpy(b.ptr + b.len, src, n) }
	b.len = need
}

// drop returns b's storage to the pool and leaves b empty.
fn (mut p BufPool) drop(mut b Buf) {
	p.give(b)
	b.ptr = unsafe { nil }
	b.len = 0
	b.cap = 0
}
