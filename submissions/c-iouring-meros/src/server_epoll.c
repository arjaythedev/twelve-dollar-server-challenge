#include "server_epoll.h"

#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "conn_epoll.h"
#include "fatal.h"
#include "memory.h"
#include "pacer.h"
#include "server_loop.h"
#include "session.h"

enum {
	MAX_EVENTS = 256,
	ACCEPTS_PER_WAKEUP = 1024, /* the kernel queue holds 4096: draining it fast is what keeps clients from retrying */
	MAX_TRACKED_FDS = 262144,
};

static const int *listeners;
static int listener_count;
static bool listener_paused;
static bool more_to_accept; /* the last accept pass stopped at its limit: do not sleep before the next */

static void watch_listener(int epoll_fd, int listen_fd)
{
	struct epoll_event event = { .events = EPOLLIN, .data.u32 = (uint32_t)listen_fd };

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) < 0)
		fatal("epoll_ctl: %s", strerror(errno));
}

static bool is_listener(int fd)
{
	for (int i = 0; i < listener_count; i++) {
		if (listeners[i] == fd)
			return true;
	}
	return false;
}

/*
 * Out of descriptors: a level-triggered listener would wake the loop on every round for a
 * connection it cannot take. Stop watching it until connections have closed.
 */
static void pause_listeners(app *a)
{
	if (listener_paused)
		return;
	for (int i = 0; i < listener_count; i++)
		epoll_ctl(conn_epoll_fd(&a->conns), EPOLL_CTL_DEL, listeners[i], NULL);
	listener_paused = true;
}

static void resume_listeners_if_possible(app *a)
{
	if (listener_paused && !conn_table_is_crowded(&a->conns) && a->memory.level != MEMORY_CRITICAL) {
		for (int i = 0; i < listener_count; i++)
			watch_listener(conn_epoll_fd(&a->conns), listeners[i]);
		listener_paused = false;
	}
}

/*
 * Takes connections out of the kernel's accept queue, which holds at most 4096: a full queue drops new
 * attempts, and the client retries only after 1, 3, 7 seconds. So this is cheap on purpose: it registers
 * the socket and nothing else. The first request is already waiting (TCP_DEFER_ACCEPT) and is read by the
 * next ordinary round. Returns true when it stopped at its limit with connections possibly still queued.
 */
static bool accept_connections(app *a, int listen_fd)
{
	for (int i = 0; i < ACCEPTS_PER_WAKEUP; i++) {
		if (a->memory.level == MEMORY_CRITICAL) {
			pause_listeners(a); /* no memory for more sockets: wait in the kernel's queue */
			return false;
		}
		int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (fd < 0) {
			if (errno == EMFILE || errno == ENFILE)
				pause_listeners(a);
			return false; /* EAGAIN: everything waiting was accepted */
		}
		if (!conn_epoll_adopt(&a->conns, fd, a->now))
			close(fd);
	}
	return true;
}

/* Reads once from a connection that has input. */
static void on_readable(app *a, int fd)
{
	conn *c = conn_get(&a->conns, fd);

	if (conn_has(c, CONN_AWAITING_BATCH) || conn_has(c, CONN_SEND_PENDING))
		return; /* the next request waits until this one is answered */

	/* Always read into the shared buffer; session_on_data copies only what has to wait. */
	ssize_t received = read(fd, a->read_buffer, READ_CHUNK);

	if (received < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (received <= 0)
		session_on_eof(a, fd);
	else
		session_on_data(a, fd, a->read_buffer, (size_t)received);
}

static void on_writable(app *a, int fd)
{
	if (conn_epoll_resume_send(&a->conns, fd))
		session_resume(a, fd);
	/* otherwise the socket is still full, or the connection was closed on a write error */
}

static void handle_event(app *a, const struct epoll_event *event)
{
	int fd = (int)event->data.u32;
	uint32_t events = event->events;

	if (is_listener(fd)) {
		more_to_accept |= accept_connections(a, fd);
		return;
	}
	if ((events & (EPOLLERR | EPOLLHUP)) && !(events & EPOLLIN)) {
		if (!conn_has(conn_get(&a->conns, fd), CONN_AWAITING_BATCH))
			conn_close(&a->conns, fd);
		return;
	}
	server_ensure_batch_room(a);
	if (events & EPOLLOUT)
		on_writable(a, fd);
	else if (events & EPOLLIN)
		on_readable(a, fd);
}

static void sleep_for_window(void)
{
	struct timespec window = { .tv_sec = 0, .tv_nsec = PACER_WINDOW_US * 1000L };

	nanosleep(&window, NULL);
}

void server_run_epoll(app *a, const int *listening_sockets, int count)
{
	static conn_epoll_state state;
	struct epoll_event events[MAX_EVENTS];
	pacer pacer_state;
	time_t last_sweep;
	int epoll_fd = epoll_create1(EPOLL_CLOEXEC);

	if (epoll_fd < 0)
		fatal("epoll_create1: %s", strerror(errno));
	listeners = listening_sockets;
	listener_count = count;
	state.epoll_fd = epoll_fd;
	{
		int limit = a->fd_limit > MAX_TRACKED_FDS ? MAX_TRACKED_FDS : a->fd_limit;

		conn_table_init(&a->conns, limit, limit, &conn_epoll_backend, &state);
	}
	for (int i = 0; i < listener_count; i++)
		watch_listener(epoll_fd, listeners[i]);
	last_sweep = a->now;
	pacer_init(&pacer_state, server_monotonic_ns());

	while (!server_stop_requested) {
		/* Busy: look without blocking, then sleep a window so the next round is bigger. Idle: block. */
		bool busy = pacer_is_busy(&pacer_state) && !more_to_accept;

		more_to_accept = false;
		/* A paused listener only comes back when the loop wakes up and sees that descriptors or memory are free. */
		int wait_ms = busy ? 0 : (listener_paused || conn_drain_pending(&a->conns)) ? 50 : IDLE_WAIT_MS;
		int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, wait_ms);

		a->now = time(NULL); /* served by the vDSO: no system call */
		a->now_ns = server_monotonic_ns();
		memwatch_check(&a->memory, a->now);
		server_relieve_memory_if_critical(a);
		conn_drain_expire(&a->conns, a->now_ns);
		for (int i = 0; i < ready; i++)
			handle_event(a, &events[i]);
		uint64_t events_done_ns = server_monotonic_ns();

		server_settle(a);
		resume_listeners_if_possible(a);
		uint64_t settled_ns = server_monotonic_ns();

		store_checkpoint_if_due(a->db);
		uint64_t round_end_ns = server_monotonic_ns();

		pacer_record(&pacer_state, a->now_ns, ready > 0 ? (unsigned)ready : 0);
		server_report_slow_round(a, ready, events_done_ns, settled_ns, round_end_ns);
		if (busy)
			sleep_for_window();
		server_sweep_idle_if_due(a, &last_sweep);
	}
}
