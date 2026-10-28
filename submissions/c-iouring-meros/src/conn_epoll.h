/* conn_epoll: the epoll backend of the connection table. A connection id is its file descriptor. */
#ifndef CONN_EPOLL_H
#define CONN_EPOLL_H

#include "conn.h"

typedef struct conn_epoll_state {
	int epoll_fd;
} conn_epoll_state;

extern const conn_backend conn_epoll_backend;

/* Starts watching an accepted socket for input and records the connection. False if the descriptor cannot be used. */
bool conn_epoll_adopt(conn_table *table, int fd, time_t now);

/* Continues sending what the socket did not take. Returns true when nothing is left; a socket error closes the connection. */
bool conn_epoll_resume_send(conn_table *table, int fd);

static inline int conn_epoll_fd(const conn_table *table)
{
	return ((const conn_epoll_state *)table->backend_data)->epoll_fd;
}

#endif
