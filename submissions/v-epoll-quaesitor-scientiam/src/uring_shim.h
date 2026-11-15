// Thin, non-inline wrappers over liburing so the V backend never has to describe
// liburing's structures. One us_ring per thread. All functions return 0 or -errno
// unless stated otherwise.
#ifndef URING_SHIM_H
#define URING_SHIM_H

typedef struct us_ring us_ring;

typedef struct us_cqe {
	int res;
	unsigned flags;
	unsigned long long user_data;
} us_cqe;

// us_new creates a ring with sq_entries submission slots, cq_entries completion slots, a
// sparse direct-descriptor table of `files` slots and a provided-buffer ring of buf_count
// buffers of buf_size bytes (group 0). Returns NULL and sets *err on failure.
us_ring* us_new(unsigned sq_entries, unsigned cq_entries, unsigned files, unsigned buf_count,
	unsigned buf_size, int* err);
void us_free(us_ring* r);

int us_accept_multishot(us_ring* r, int listen_fd, unsigned long long ud);
int us_recv_multishot(us_ring* r, int slot, unsigned long long ud);
int us_send(us_ring* r, int slot, const void* p, unsigned n, unsigned long long ud);
int us_close(us_ring* r, int slot, unsigned long long ud);
// us_cancel asks the kernel to cancel the operation tagged target_ud (its completion then
// carries -ECANCELED); the cancel itself completes with tag ud.
int us_cancel(us_ring* r, unsigned long long target_ud, unsigned long long ud);
// us_cancel_slot cancels every operation pending on direct descriptor slot (a close does not
// cancel them by itself: the socket would stay open until they complete). Linux 6.0+; on an
// older kernel the cancel completes with -EINVAL and does nothing.
int us_cancel_slot(us_ring* r, int slot, unsigned long long ud);

// us_submit_wait submits pending entries and waits for at least wait_nr completions or
// timeout_us microseconds. Returns the number of completions ready (>= 0) or -errno.
int us_submit_wait(us_ring* r, unsigned wait_nr, int timeout_us);
// us_peek copies up to max ready completions into out without consuming them.
unsigned us_peek(us_ring* r, us_cqe* out, unsigned max);
// us_advance consumes n completions.
void us_advance(us_ring* r, unsigned n);

// us_buf returns the address of provided buffer bid; us_buf_return hands it back to the
// kernel. A recv completion with (flags & US_F_BUFFER) carries bid = flags >> 16.
unsigned char* us_buf(us_ring* r, unsigned bid);
void us_buf_return(us_ring* r, unsigned bid);

#define US_F_BUFFER 1u
#define US_F_MORE 2u

#endif
