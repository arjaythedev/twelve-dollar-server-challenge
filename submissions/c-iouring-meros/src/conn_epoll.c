#include "conn_epoll.h"

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "memory.h"

static void watch(const conn_table *table, int fd, int operation, uint32_t events)
{
	struct epoll_event event = { .events = events, .data.u32 = (uint32_t)fd };

	epoll_ctl(conn_epoll_fd(table), operation, fd, &event);
}

bool conn_epoll_adopt(conn_table *table, int fd, time_t now)
{
	struct epoll_event event = { .events = EPOLLIN, .data.u32 = (uint32_t)fd };

	if (fd >= table->id_space || epoll_ctl(conn_epoll_fd(table), EPOLL_CTL_ADD, fd, &event) < 0)
		return false;
	conn_register(table, fd, now);
	return true;
}

static void append_unsent(conn *c, const char *data, size_t len)
{
	c->unsent = xrealloc(c->unsent, (size_t)c->unsent_len + len);
	memcpy(c->unsent + c->unsent_len, data, len);
	c->unsent_len += (uint32_t)len;
}

/* Queues `data` after anything already unsent, writing as much as the socket takes right now. */
static void epoll_send(conn_table *table, int fd, strview data)
{
	conn *c = conn_get(table, fd);

	if (conn_has(c, CONN_SEND_PENDING)) { /* earlier bytes are still waiting: keep the order */
		append_unsent(c, data.ptr, data.len);
		return;
	}
	ssize_t written = write(fd, data.ptr, data.len);

	if (written == (ssize_t)data.len)
		return;
	if (written < 0) {
		if (errno != EAGAIN && errno != EINTR) { /* peer is gone */
			conn_set(c, CONN_CLOSE_AFTER_SEND | CONN_ABORTIVE);
			return;
		}
		written = 0;
	}
	append_unsent(c, data.ptr + written, data.len - (size_t)written);
	conn_set(c, CONN_SEND_PENDING);
	watch(table, fd, EPOLL_CTL_MOD, EPOLLOUT); /* stop reading until the client has taken the response */
}

static void epoll_release(conn_table *table, int fd)
{
	(void)table;
	close(fd); /* also removes the descriptor from the epoll set */
}

const conn_backend conn_epoll_backend = { .send = epoll_send, .release = epoll_release };

bool conn_epoll_resume_send(conn_table *table, int fd)
{
	conn *c = conn_get(table, fd);

	while (c->unsent_off < c->unsent_len) {
		ssize_t written = write(fd, c->unsent + c->unsent_off, c->unsent_len - c->unsent_off);

		if (written < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return false;
			conn_close(table, fd);
			return false;
		}
		c->unsent_off += (uint32_t)written;
	}
	free(c->unsent);
	c->unsent = NULL;
	c->unsent_len = c->unsent_off = 0;
	conn_clear(c, CONN_SEND_PENDING);
	watch(table, fd, EPOLL_CTL_MOD, EPOLLIN);
	return true;
}
