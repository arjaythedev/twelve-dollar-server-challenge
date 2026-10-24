#define _GNU_SOURCE
#include "uring_shim.h"
#include <errno.h>
#include <liburing.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

struct us_ring {
	struct io_uring ring;
	struct io_uring_buf_ring* br;
	unsigned char* bufs;
	unsigned buf_count;
	unsigned buf_size;
};

us_ring* us_new(unsigned sq_entries, unsigned cq_entries, unsigned files, unsigned buf_count,
	unsigned buf_size, int* err) {
	us_ring* r = calloc(1, sizeof(*r));
	if (!r) {
		*err = -ENOMEM;
		return NULL;
	}
	struct io_uring_params p;
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN
		| IORING_SETUP_CQSIZE;
	p.cq_entries = cq_entries;
	int rc = io_uring_queue_init_params(sq_entries, &r->ring, &p);
	if (rc < 0) {
		// older kernels: retry without the task-run flags
		memset(&p, 0, sizeof(p));
		p.flags = IORING_SETUP_CQSIZE;
		p.cq_entries = cq_entries;
		rc = io_uring_queue_init_params(sq_entries, &r->ring, &p);
	}
	if (rc < 0) {
		*err = rc;
		free(r);
		return NULL;
	}
	rc = io_uring_register_files_sparse(&r->ring, files);
	if (rc < 0) {
		*err = rc;
		io_uring_queue_exit(&r->ring);
		free(r);
		return NULL;
	}
	io_uring_register_ring_fd(&r->ring);
	r->br = io_uring_setup_buf_ring(&r->ring, buf_count, 0, 0, &rc);
	if (!r->br) {
		*err = rc;
		io_uring_queue_exit(&r->ring);
		free(r);
		return NULL;
	}
	r->buf_count = buf_count;
	r->buf_size = buf_size;
	r->bufs = aligned_alloc(4096, (size_t)buf_count * buf_size);
	if (!r->bufs) {
		*err = -ENOMEM;
		io_uring_free_buf_ring(&r->ring, r->br, buf_count, 0);
		io_uring_queue_exit(&r->ring);
		free(r);
		return NULL;
	}
	int mask = io_uring_buf_ring_mask(buf_count);
	for (unsigned i = 0; i < buf_count; i++) {
		io_uring_buf_ring_add(r->br, r->bufs + (size_t)i * buf_size, buf_size, i, mask, (int)i);
	}
	io_uring_buf_ring_advance(r->br, (int)buf_count);
	*err = 0;
	return r;
}

void us_free(us_ring* r) {
	if (!r) return;
	io_uring_free_buf_ring(&r->ring, r->br, r->buf_count, 0);
	io_uring_queue_exit(&r->ring);
	free(r->bufs);
	free(r);
}

static struct io_uring_sqe* get_sqe(us_ring* r) {
	struct io_uring_sqe* sqe = io_uring_get_sqe(&r->ring);
	if (!sqe) {
		io_uring_submit(&r->ring);
		sqe = io_uring_get_sqe(&r->ring);
	}
	return sqe;
}

int us_accept_multishot(us_ring* r, int listen_fd, unsigned long long ud) {
	struct io_uring_sqe* sqe = get_sqe(r);
	if (!sqe) return -EBUSY;
	io_uring_prep_multishot_accept_direct(sqe, listen_fd, NULL, NULL, 0);
	io_uring_sqe_set_data64(sqe, ud);
	return 0;
}

int us_recv_multishot(us_ring* r, int slot, unsigned long long ud) {
	struct io_uring_sqe* sqe = get_sqe(r);
	if (!sqe) return -EBUSY;
	io_uring_prep_recv_multishot(sqe, slot, NULL, 0, 0);
	sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
	sqe->buf_group = 0;
	io_uring_sqe_set_data64(sqe, ud);
	return 0;
}

int us_send(us_ring* r, int slot, const void* p, unsigned n, unsigned long long ud) {
	struct io_uring_sqe* sqe = get_sqe(r);
	if (!sqe) return -EBUSY;
	io_uring_prep_send(sqe, slot, p, n, MSG_NOSIGNAL);
	sqe->flags |= IOSQE_FIXED_FILE;
	io_uring_sqe_set_data64(sqe, ud);
	return 0;
}

int us_close(us_ring* r, int slot, unsigned long long ud) {
	struct io_uring_sqe* sqe = get_sqe(r);
	if (!sqe) return -EBUSY;
	io_uring_prep_close_direct(sqe, (unsigned)slot);
	io_uring_sqe_set_data64(sqe, ud);
	return 0;
}

int us_submit_wait(us_ring* r, unsigned wait_nr, int timeout_us) {
	struct __kernel_timespec ts;
	ts.tv_sec = timeout_us / 1000000;
	ts.tv_nsec = (long)(timeout_us % 1000000) * 1000;
	struct io_uring_cqe* cqe = NULL;
	int rc = io_uring_submit_and_wait_timeout(&r->ring, &cqe, wait_nr, &ts, NULL);
	if (rc < 0 && rc != -ETIME && rc != -EINTR) return rc;
	return (int)io_uring_cq_ready(&r->ring);
}

unsigned us_peek(us_ring* r, us_cqe* out, unsigned max) {
	struct io_uring_cqe* cqe;
	unsigned head, n = 0;
	io_uring_for_each_cqe(&r->ring, head, cqe) {
		if (n == max) break;
		out[n].res = cqe->res;
		out[n].flags = 0;
		if (cqe->flags & IORING_CQE_F_BUFFER) out[n].flags |= US_F_BUFFER | (cqe->flags & 0xffff0000u);
		if (cqe->flags & IORING_CQE_F_MORE) out[n].flags |= US_F_MORE;
		out[n].user_data = cqe->user_data;
		n++;
	}
	return n;
}

void us_advance(us_ring* r, unsigned n) {
	io_uring_cq_advance(&r->ring, n);
}

unsigned char* us_buf(us_ring* r, unsigned bid) {
	return r->bufs + (size_t)bid * r->buf_size;
}

void us_buf_return(us_ring* r, unsigned bid) {
	io_uring_buf_ring_add(r->br, r->bufs + (size_t)bid * r->buf_size, r->buf_size, (unsigned short)bid,
		io_uring_buf_ring_mask(r->buf_count), 0);
	io_uring_buf_ring_advance(r->br, 1);
}

int us_cancel_slot(us_ring* r, int slot, unsigned long long ud) {
	struct io_uring_sqe* sqe = get_sqe(r);
	if (!sqe) return -EBUSY;
	io_uring_prep_cancel_fd(sqe, slot, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED);
	io_uring_sqe_set_data64(sqe, ud);
	return 0;
}

int us_cancel(us_ring* r, unsigned long long target_ud, unsigned long long ud) {
	struct io_uring_sqe* sqe = get_sqe(r);
	if (!sqe) return -EBUSY;
	io_uring_prep_cancel64(sqe, target_ud, 0);
	io_uring_sqe_set_data64(sqe, ud);
	return 0;
}
