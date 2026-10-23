/* server_epoll: the event loop on epoll. Used when io_uring is unavailable, and for comparison. */
#ifndef SERVER_EPOLL_H
#define SERVER_EPOLL_H

#include "app.h"

/* Serves on the listening sockets until a stop is requested. */
void server_run_epoll(app *a, const int *listeners, int listener_count);

#endif
