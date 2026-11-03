/*
 * server_uring: the event loop on io_uring, with every connection in a "direct descriptor".
 *
 * Why: the service gets LimitNOFILE=65535, one file descriptor per connection, so a server on plain
 * descriptors serves more users than that by closing connections after each response, and at about
 * 9,000 new connections a second in a one-vCPU box that churn collapses the tail latency. A direct
 * descriptor lives in a table registered with a ring and does not take a slot in the process's
 * descriptor table, so one ring holds up to 65,535 connections and the server uses up to five rings
 * (327,675 connections), all driven by this one thread.
 *
 * Everything else is the same as the epoll server: the same parser, router, batch and store.
 */
#ifndef SERVER_URING_H
#define SERVER_URING_H

#include "app.h"

/*
 * Serves on the listening sockets until a stop is requested. Returns 0 afterwards. Returns a negative
 * errno, before having served anything, when the kernel does not support what this needs (Linux 6.1
 * or later, with io_uring enabled): the caller then uses the epoll server.
 */
int server_run_uring(app *a, const int *listeners, int listener_count);

#endif
