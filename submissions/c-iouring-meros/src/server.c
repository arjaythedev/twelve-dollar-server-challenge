#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>

#include "app.h"
#include "fatal.h"
#include "http_response.h"
#include "server_epoll.h"
#include "server_loop.h"
#include "server_uring.h"

enum {
	LISTEN_BACKLOG = 4096,
	DEFER_ACCEPT_SECONDS = 5,
};

static void request_stop(int signal_number)
{
	(void)signal_number;
	server_stop_requested = 1;
}

static void install_signal_handlers(void)
{
	struct sigaction action = { .sa_handler = request_stop };

	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* Raises the descriptor limit to the hard maximum and returns the resulting soft limit. */
static int raise_descriptor_limit(void)
{
	struct rlimit limit;

	if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
		fatal("getrlimit failed");
	limit.rlim_cur = limit.rlim_max;
	setrlimit(RLIMIT_NOFILE, &limit);
	getrlimit(RLIMIT_NOFILE, &limit);
	return limit.rlim_cur > 0x7fffffff ? 0x7fffffff : (int)limit.rlim_cur;
}

static int open_listener(const config *cfg)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int enable = 1, defer_seconds = DEFER_ACCEPT_SECONDS;
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(cfg->port), .sin_addr = cfg->bind_address };

	if (fd < 0)
		fatal("socket: %s", strerror(errno));
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable);
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof enable); /* every listener of the group needs it */
	/* Accepted sockets inherit TCP_NODELAY. Every response leaves in one write, so Nagle only adds delay. */
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof enable);
	/* Wake up on the first request, not on the handshake: one fewer round per connection. */
	setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer_seconds, sizeof defer_seconds);
	if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0)
		fatal("cannot bind port %u: %s", cfg->port, strerror(errno));
	if (listen(fd, LISTEN_BACKLOG) < 0)
		fatal("listen: %s", strerror(errno));
	return fd;
}

static void initialize(app *a, const config *cfg)
{
	http_response_init_errors();
	jwt_verifier_init(&a->jwt, cfg->jwt_secret);
	a->pending = batch_create();
	a->now = a->started_at = time(NULL);
	memwatch_init(&a->memory);
	a->db = store_open(cfg->database_path);
}

/*
 * io_uring with direct descriptors when the kernel allows it, epoll otherwise. SERVER_IO=epoll or
 * SERVER_IO=uring forces one of them; it exists for tests and comparisons, the benchmark never sets it.
 */
static void run_event_loop(app *a, const int *listeners)
{
	const char *forced = getenv("SERVER_IO");

	if (forced == NULL || strcmp(forced, "epoll") != 0) {
		int error = server_run_uring(a, listeners, LISTENER_COUNT);

		if (error == 0)
			return;
		if (forced != NULL)
			fatal("SERVER_IO=%s, but io_uring is unavailable: %s", forced, strerror(-error));
		fprintf(stderr, "server: io_uring unavailable (%s), using epoll\n", strerror(-error));
	}
	fprintf(stderr, "server: epoll\n");
	server_run_epoll(a, listeners, LISTENER_COUNT);
}

void server_run(const config *cfg)
{
	static app a; /* static: the shared buffers are ~300 KB */
	int listeners[LISTENER_COUNT];

	install_signal_handlers();
	a.fd_limit = raise_descriptor_limit();
	initialize(&a, cfg);
	for (int i = 0; i < LISTENER_COUNT; i++)
		listeners[i] = open_listener(cfg);

	run_event_loop(&a, listeners);

	store_checkpoint(a.db);
	store_close(a.db);
}
