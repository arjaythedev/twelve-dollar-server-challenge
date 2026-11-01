#include "server_uring.h"

#include <errno.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fatal.h"
#include "memory.h"
#include "pacer.h"
#include "server_loop.h"
#include "session.h"

/*
 * Ring memory counts against RLIMIT_MEMLOCK: about 0.4 MB per ring with these sizes, so up to 2 MB for five rings.
 * Where the limit is smaller (containers often allow 64 KB) open_ring fails and the server uses epoll.
 */
enum {
	MAX_RINGS = 5,
	SLOT_BITS = 16,                 /* connection id = ring << SLOT_BITS | slot */
	MAX_SLOTS_PER_RING = 65535,     /* the kernel limits a file table to RLIMIT_NOFILE entries */
	SUBMISSION_ENTRIES = 2048,
	COMPLETION_ENTRIES = 8192,
	BUFFER_COUNT = 4096,            /* receive buffers per ring; a buffer is held only while one request is parsed */
	BUFFER_SIZE = 2048,
	BUFFER_PUBLISH_BATCH = 256,
	WAIT_FOR_COMPLETIONS = 32,      /* when busy: wake up after this many completions, or after one pacer window */
	MAX_ACCEPT_MARGIN = 8192,       /* room a ring keeps free; see accept_margin */
	RING_RETRY_SECONDS = 5,         /* after a ring failed to open */
	ACCEPT_RETRY_SECONDS = 1,       /* after an accept failed for an unexpected reason */
	IDLE_POLL_MS_SEVERAL_RINGS = 20,
};

/* Who a completion belongs to. Pointers to heap memory are 8-byte aligned, so a send's user_data is its pointer. */
enum tag { TAG_SEND = 0, TAG_RECEIVE = 1, TAG_ACCEPT = 2, TAG_CLOSE = 3, TAG_IGNORE = 4, TAG_MASK = 7 };

/* A multishot accept on one listener, on one ring. */
enum accept_state { ACCEPT_IDLE, ACCEPT_ARMED, ACCEPT_CANCELLING };

typedef struct ring_state {
	struct io_uring ring;
	struct io_uring_buf_ring *buffers;
	char *buffer_memory;
	unsigned unpublished_buffers;
	int slot_count; /* direct descriptors registered in this ring */
	int in_use;     /* direct descriptors that hold an open socket (including ones still being closed) */
	enum accept_state accept[LISTENER_COUNT];
	time_t accept_retry_at[LISTENER_COUNT]; /* after an accept failed for another reason than a full table */
} ring_state;

typedef struct uring_state {
	ring_state rings[MAX_RINGS];
	int ring_count;
	const int *listeners;
	int listener_count;
	bool accept_paused;
	unsigned slots_per_ring;
	time_t next_ring_attempt; /* do not try to open a ring before this time */
} uring_state;

/* A response on its way out. It outlives the connection if the connection closes while it is in flight. */
typedef struct send_op {
	int id;
	uint32_t generation;
	uint32_t length;
	uint32_t offset;
	char data[];
} send_op;

static uring_state state;

/* ---- ids and user_data ---- */

static int make_id(int ring, int slot) { return ring << SLOT_BITS | slot; }
static int ring_of(int id) { return id >> SLOT_BITS; }
static int slot_of(int id) { return id & ((1 << SLOT_BITS) - 1); }

static uint64_t connection_data(enum tag tag, int id, uint32_t generation)
{
	return (uint64_t)generation << 32 | (uint64_t)id << 3 | tag;
}

static uint64_t accept_data(int ring, int listener) { return (uint64_t)listener << 16 | (uint64_t)ring << 3 | TAG_ACCEPT; }
static uint64_t close_data(int ring) { return (uint64_t)ring << 3 | TAG_CLOSE; }

/* ---- submission ---- */

static struct io_uring_sqe *get_sqe(ring_state *r)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);

	/* The submission queue is full: hand what is queued to the kernel and try again. */
	for (int attempt = 0; sqe == NULL && attempt < 100; attempt++) {
		int submitted = io_uring_submit(&r->ring);

		if (submitted < 0 && submitted != -EAGAIN && submitted != -EBUSY && submitted != -EINTR)
			fatal("io_uring_submit: %s", strerror(-submitted));
		sqe = io_uring_get_sqe(&r->ring);
	}
	if (sqe == NULL)
		fatal("io_uring submission queue stays full");
	return sqe;
}

static void arm_receive(ring_state *r, int id, uint32_t generation)
{
	struct io_uring_sqe *sqe = get_sqe(r);

	io_uring_prep_recv_multishot(sqe, slot_of(id), NULL, 0, 0);
	sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
	sqe->buf_group = 0;
	sqe->user_data = connection_data(TAG_RECEIVE, id, generation);
}

static void arm_accept(ring_state *r, int ring_index, int listener_index, int listener_fd)
{
	struct io_uring_sqe *sqe = get_sqe(r);

	io_uring_prep_multishot_accept_direct(sqe, listener_fd, NULL, NULL, 0);
	sqe->user_data = accept_data(ring_index, listener_index);
	r->accept[listener_index] = ACCEPT_ARMED;
}

static void submit_send(ring_state *r, int id, uint32_t generation, const char *data, size_t length)
{
	send_op *op = xrealloc(NULL, sizeof *op + length);
	struct io_uring_sqe *sqe = get_sqe(r);

	op->id = id;
	op->generation = generation;
	op->length = (uint32_t)length;
	op->offset = 0;
	memcpy(op->data, data, length);
	io_uring_prep_send(sqe, slot_of(id), op->data, length, MSG_NOSIGNAL);
	sqe->flags |= IOSQE_FIXED_FILE;
	sqe->user_data = (uint64_t)(uintptr_t)op;
}

static void resubmit_send(ring_state *r, send_op *op)
{
	struct io_uring_sqe *sqe = get_sqe(r);

	io_uring_prep_send(sqe, slot_of(op->id), op->data + op->offset, op->length - op->offset, MSG_NOSIGNAL);
	sqe->flags |= IOSQE_FIXED_FILE;
	sqe->user_data = (uint64_t)(uintptr_t)op;
}

/* ---- the connection table's backend ---- */

static void uring_send(conn_table *table, int id, strview data)
{
	uring_state *s = table->backend_data;
	conn *c = conn_get(table, id);

	if (!conn_has(c, CONN_OPEN))
		return; /* closed while a response was being prepared */
	if (conn_has(c, CONN_SEND_PENDING)) { /* one send in flight at a time keeps the bytes in order */
		c->unsent = xrealloc(c->unsent, (size_t)c->unsent_len + data.len);
		memcpy(c->unsent + c->unsent_len, data.ptr, data.len);
		c->unsent_len += (uint32_t)data.len;
		return;
	}
	conn_set(c, CONN_SEND_PENDING);
	submit_send(&s->rings[ring_of(id)], id, c->generation, data.ptr, data.len);
}

/*
 * Closes the socket in direct descriptor `slot`. A multishot receive keeps a reference to the socket, so
 * the requests on the descriptor are cancelled first; the hard link makes the close follow either way.
 */
static void uring_release(conn_table *table, int id)
{
	uring_state *s = table->backend_data;
	ring_state *r = &s->rings[ring_of(id)];
	struct io_uring_sqe *sqe;

	if (io_uring_sq_space_left(&r->ring) < 2)
		io_uring_submit(&r->ring); /* the two entries must be submitted together to stay linked */
	sqe = get_sqe(r);
	io_uring_prep_cancel_fd(sqe, slot_of(id), IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED);
	sqe->flags |= IOSQE_IO_HARDLINK;
	sqe->user_data = TAG_IGNORE;
	sqe = get_sqe(r);
	io_uring_prep_close_direct(sqe, slot_of(id));
	sqe->user_data = close_data(ring_of(id));
}

static const conn_backend uring_backend = { .send = uring_send, .release = uring_release };

/* ---- rings ---- */

/*
 * How much room a ring keeps free: an eighth of its slots, at most MAX_ACCEPT_MARGIN. The kernel accepts
 * without asking us, so the room must cover every connection that can arrive between two rounds of the loop,
 * including after a stall: 8,192 is about a second of the busiest benchmark arrival rate.
 */
static int accept_margin(const ring_state *r)
{
	int margin = r->slot_count / 8;

	return margin < 24 ? 24 : margin > MAX_ACCEPT_MARGIN ? MAX_ACCEPT_MARGIN : margin;
}

static void recycle_buffer(ring_state *r, unsigned buffer_id)
{
	io_uring_buf_ring_add(r->buffers, r->buffer_memory + (size_t)buffer_id * BUFFER_SIZE, BUFFER_SIZE, (unsigned short)buffer_id,
	                      io_uring_buf_ring_mask(BUFFER_COUNT), (int)r->unpublished_buffers++);
	if (r->unpublished_buffers >= BUFFER_PUBLISH_BATCH) {
		io_uring_buf_ring_advance(r->buffers, (int)r->unpublished_buffers);
		r->unpublished_buffers = 0;
	}
}

static void publish_buffers(ring_state *r)
{
	if (r->unpublished_buffers != 0) {
		io_uring_buf_ring_advance(r->buffers, (int)r->unpublished_buffers);
		r->unpublished_buffers = 0;
	}
}

/* Returns 0, or a negative errno. */
static int open_ring(uring_state *s, int index)
{
	ring_state *r = &s->rings[index];
	struct io_uring_params params = {
		.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE,
		.cq_entries = COMPLETION_ENTRIES,
	};
	int error = io_uring_queue_init_params(SUBMISSION_ENTRIES, &r->ring, &params);

	if (error < 0)
		return error;
	error = io_uring_register_files_sparse(&r->ring, s->slots_per_ring);
	if (error < 0) {
		io_uring_queue_exit(&r->ring);
		return error;
	}
	r->buffers = io_uring_setup_buf_ring(&r->ring, BUFFER_COUNT, 0, 0, &error);
	if (r->buffers == NULL) {
		io_uring_queue_exit(&r->ring);
		return error;
	}
	if (posix_memalign((void **)&r->buffer_memory, 4096, (size_t)BUFFER_COUNT * BUFFER_SIZE) != 0) {
		io_uring_free_buf_ring(&r->ring, r->buffers, BUFFER_COUNT, 0);
		io_uring_queue_exit(&r->ring);
		return -ENOMEM;
	}
	for (unsigned i = 0; i < BUFFER_COUNT; i++)
		recycle_buffer(r, i);
	publish_buffers(r);
	r->slot_count = (int)s->slots_per_ring;
	return 0;
}

/* What a ring can hold: accepting stops when `margin / 2` slots are left. */
static int usable_slots(const ring_state *r)
{
	return r->slot_count - accept_margin(r) / 2;
}

/*
 * The capacity the connection table reports is what the open rings can hold. The "crowded" mode
 * (responses with "Connection: close") starts a reserve below it, so it always begins before accepting
 * stops and new users never wait in the kernel's queue without a way to make room.
 */
static void update_capacity(app *a, const uring_state *s)
{
	int capacity = 0;

	for (int i = 0; i < s->ring_count; i++)
		capacity += usable_slots(&s->rings[i]);
	a->conns.capacity = capacity;
}

/*
 * Decides, ring by ring, whether connections may be accepted into it. This matters: if an accept finds the
 * file table full, the kernel has already taken the socket from the listener and then drops it, and the
 * client sees a reset. So a ring stops accepting while it still has room (free <= margin / 2), starts
 * again only when it has clearly more (free > margin), and the next ring is opened earlier (see below).
 */
static void manage_accepts(uring_state *s, time_t now)
{
	for (int i = 0; i < s->ring_count; i++) {
		ring_state *r = &s->rings[i];
		int free_slots = r->slot_count - r->in_use;
		int margin = accept_margin(r);

		for (int l = 0; l < s->listener_count; l++) {
			if (r->accept[l] == ACCEPT_IDLE && !s->accept_paused && free_slots > margin && now >= r->accept_retry_at[l]) {
				arm_accept(r, i, l, s->listeners[l]);
			} else if (r->accept[l] == ACCEPT_ARMED && (s->accept_paused || free_slots <= margin / 2)) {
				struct io_uring_sqe *sqe = get_sqe(r);

				io_uring_prep_cancel_fd(sqe, s->listeners[l], IORING_ASYNC_CANCEL_ALL);
				sqe->user_data = TAG_IGNORE;
				r->accept[l] = ACCEPT_CANCELLING;
			}
		}
	}
}

/*
 * A new ring joins before the last one fills up, when there is memory for its buffers and sockets. If it
 * cannot open, the connection table's capacity stays as it is and the server's own fallback applies:
 * responses carry "Connection: close" once the open rings are nearly full.
 */
static void open_next_ring_if_needed(app *a, uring_state *s)
{
	const ring_state *last = &s->rings[s->ring_count - 1];

	if (s->ring_count == MAX_RINGS || last->slot_count - last->in_use > 2 * accept_margin(last) || a->memory.level != MEMORY_OK)
		return;
	if (a->now < s->next_ring_attempt)
		return;
	if (open_ring(s, s->ring_count) == 0) {
		s->ring_count++;
		update_capacity(a, s);
	} else {
		s->next_ring_attempt = a->now + RING_RETRY_SECONDS;
	}
}

/* ---- completions ---- */

static bool connection_is(app *a, int id, uint32_t generation)
{
	const conn *c = conn_get(&a->conns, id);

	return conn_has(c, CONN_OPEN) && c->generation == generation;
}

static void on_accept(app *a, uring_state *s, int ring_index, int listener, const struct io_uring_cqe *cqe)
{
	ring_state *r = &s->rings[ring_index];

	if (cqe->res >= 0) {
		int id = make_id(ring_index, cqe->res);

		r->in_use++;
		conn_register(&a->conns, id, a->now);
		arm_receive(r, id, conn_get(&a->conns, id)->generation);
	}
	if (!(cqe->flags & IORING_CQE_F_MORE)) {
		r->accept[listener] = ACCEPT_IDLE; /* cancelled, or an error: manage_accepts decides about the next one */
		/* Cancelled by us, or the table was full: normal. Anything else (a denied accept, a bad listener) is retried slowly. */
		if (cqe->res < 0 && cqe->res != -ECANCELED && cqe->res != -ENFILE && cqe->res != -EMFILE)
			r->accept_retry_at[listener] = a->now + ACCEPT_RETRY_SECONDS;
	}
}

static void on_receive(app *a, ring_state *r, int id, uint32_t generation, const struct io_uring_cqe *cqe)
{
	bool has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
	unsigned buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
	int received = cqe->res;

	if (connection_is(a, id, generation)) {
		if (received > 0) {
			server_ensure_batch_room(a);
			if (has_buffer && connection_is(a, id, generation))
				session_on_data(a, id, r->buffer_memory + (size_t)buffer_id * BUFFER_SIZE, (size_t)received);
		} else if (received != -ENOBUFS) { /* end of stream, or an error: the multishot request is over */
			session_on_eof(a, id);
		}
		/* The receive ends by itself when the buffers run out or on an error: start it again. */
		if (!(cqe->flags & IORING_CQE_F_MORE) && (received > 0 || received == -ENOBUFS) && connection_is(a, id, generation))
			arm_receive(r, id, generation);
	}
	if (has_buffer)
		recycle_buffer(r, buffer_id);
}

static void on_send(app *a, ring_state *r, send_op *op, int result)
{
	int id = op->id;

	if (!connection_is(a, id, op->generation)) { /* the connection closed meanwhile */
		free(op);
		return;
	}
	conn *c = conn_get(&a->conns, id);

	if (result < 0) { /* the peer is gone */
		free(op);
		c->unsent_len = 0;
		free(c->unsent);
		c->unsent = NULL;
		conn_clear(c, CONN_SEND_PENDING);
		conn_set(c, CONN_CLOSE_AFTER_SEND | CONN_ABORTIVE);
		server_ensure_batch_room(a);
		session_resume(a, id);
		return;
	}
	op->offset += (uint32_t)result;
	if (op->offset < op->length) { /* the socket took part of it */
		resubmit_send(r, op);
		return;
	}
	free(op);
	if (c->unsent_len != 0) { /* more was queued behind this response */
		submit_send(r, id, c->generation, c->unsent, c->unsent_len);
		free(c->unsent);
		c->unsent = NULL;
		c->unsent_len = 0;
		return;
	}
	conn_clear(c, CONN_SEND_PENDING);
	server_ensure_batch_room(a);
	session_resume(a, id);
}

static void handle_completion(app *a, uring_state *s, int ring_index, const struct io_uring_cqe *cqe)
{
	ring_state *r = &s->rings[ring_index];
	uint64_t data = cqe->user_data;

	switch (data & TAG_MASK) {
	case TAG_SEND:
		on_send(a, r, (send_op *)(uintptr_t)data, cqe->res);
		break;
	case TAG_RECEIVE:
		on_receive(a, r, (int)((data >> 3) & 0x1fffffff), (uint32_t)(data >> 32), cqe);
		break;
	case TAG_ACCEPT:
		on_accept(a, s, (int)((data >> 3) & 7), (int)(data >> 16), cqe);
		break;
	case TAG_CLOSE:
		r->in_use--; /* the slot is free again */
		break;
	default:
		break; /* TAG_IGNORE: a cancel request */
	}
}

/* Handles every completion that is ready on one ring. Returns how many there were. */
static unsigned reap(app *a, uring_state *s, int ring_index)
{
	ring_state *r = &s->rings[ring_index];
	struct io_uring_cqe *cqe;
	unsigned head, count = 0;

	io_uring_for_each_cqe(&r->ring, head, cqe) {
		handle_completion(a, s, ring_index, cqe);
		count++;
	}
	io_uring_cq_advance(&r->ring, count);
	publish_buffers(r);
	return count;
}

/*
 * Waits for work. Busy: until a batch of completions is ready or one pacer window passed, so the round is
 * large. Idle: until the first completion. The other rings are polled; they only exist above 64k connections.
 */
static void wait_for_completions(uring_state *s, bool busy, int idle_wait_ms)
{
	struct io_uring_cqe *cqe;
	struct __kernel_timespec timeout = { .tv_sec = 0, .tv_nsec = PACER_WINDOW_US * 1000L };
	unsigned wanted = WAIT_FOR_COMPLETIONS;

	if (!busy) {
		timeout.tv_sec = idle_wait_ms / 1000;
		timeout.tv_nsec = (long)(idle_wait_ms % 1000) * 1000000L;
		wanted = 1;
	}
	if (s->ring_count > 1) {
		/*
		 * Completions can arrive on any ring, and only ring 0 is waited on: poll the others. Busy, the pacer
		 * window bounds the wait anyway. Idle, a short wait keeps latency low without waking up 1,000 times a second.
		 */
		for (int i = 1; i < s->ring_count; i++)
			io_uring_submit_and_get_events(&s->rings[i].ring);
		if (!busy) {
			timeout.tv_sec = 0;
			timeout.tv_nsec = IDLE_POLL_MS_SEVERAL_RINGS * 1000000L;
		}
		wanted = 1;
	}
	io_uring_submit_and_wait_timeout(&s->rings[0].ring, &cqe, wanted, &timeout, NULL);
	for (int i = 1; i < s->ring_count; i++)
		io_uring_submit_and_get_events(&s->rings[i].ring);
}

/* ---- the loop ---- */

static void apply_memory_pressure(app *a, uring_state *s)
{
	/* No memory for more sockets: they wait in the kernel's queue (manage_accepts cancels the accepts). */
	s->accept_paused = a->memory.level == MEMORY_CRITICAL;
}

int server_run_uring(app *a, const int *listeners, int listener_count)
{
	uring_state *s = &state;
	pacer pacer_state;
	time_t last_sweep;
	int error;

	s->listeners = listeners;
	s->listener_count = listener_count;
	s->slots_per_ring = a->fd_limit < MAX_SLOTS_PER_RING ? (unsigned)a->fd_limit : MAX_SLOTS_PER_RING;
	error = open_ring(s, 0);
	if (error < 0)
		return error;
	s->ring_count = 1;
	conn_table_init(&a->conns, MAX_RINGS << SLOT_BITS, 0, &uring_backend, s);
	update_capacity(a, s);
	fprintf(stderr, "server: io_uring with direct descriptors (%u per ring, up to %d rings)\n", s->slots_per_ring, MAX_RINGS);

	last_sweep = a->now;
	pacer_init(&pacer_state, server_monotonic_ns());
	while (!server_stop_requested) {
		bool busy = pacer_is_busy(&pacer_state);
		int idle_wait_ms = (s->accept_paused || conn_drain_pending(&a->conns)) ? 50 : IDLE_WAIT_MS;

		manage_accepts(s, a->now);
		wait_for_completions(s, busy, idle_wait_ms);

		a->now = time(NULL);
		a->now_ns = server_monotonic_ns();
		memwatch_check(&a->memory, a->now);
		server_relieve_memory_if_critical(a);
		apply_memory_pressure(a, s);
		conn_drain_expire(&a->conns, a->now_ns);
		open_next_ring_if_needed(a, s);

		unsigned completions = 0;

		for (int i = 0; i < s->ring_count; i++)
			completions += reap(a, s, i);
		uint64_t events_done_ns = server_monotonic_ns();

		server_settle(a);
		uint64_t settled_ns = server_monotonic_ns();

		store_checkpoint_if_due(a->db);
		uint64_t round_end_ns = server_monotonic_ns();

		pacer_record(&pacer_state, a->now_ns, completions);
		server_report_slow_round(a, (int)completions, events_done_ns, settled_ns, round_end_ns);
		server_sweep_idle_if_due(a, &last_sweep);
	}
	return 0;
}
