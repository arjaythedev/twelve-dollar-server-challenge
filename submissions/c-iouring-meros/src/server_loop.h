/* server_loop: what the epoll loop and the io_uring loop share. */
#ifndef SERVER_LOOP_H
#define SERVER_LOOP_H

#include <signal.h>
#include <stdint.h>

#include "app.h"

enum {
	LISTENER_COUNT = 4, /* sockets on the same port (SO_REUSEPORT); each has its own accept queue of up to 4096 */
	SWEEP_INTERVAL_SECONDS = 30,
	IDLE_WAIT_MS = 1000, /* an idle loop wakes up this often, so memwatch and the timers keep running */
	IDLE_TIMEOUT_SECONDS = 600, /* the benchmark's longest think time is 15 s; Nginx expects >= 65 s */
};

extern volatile sig_atomic_t server_stop_requested;

uint64_t server_monotonic_ns(void);

/*
 * Answers everything queued in the batch. Resuming a connection can queue its next request, so this
 * repeats until nothing is left. Resumed connections are processed only while the batch has room.
 */
void server_settle(app *a);

/* Each event adds at most one request to the batch, so one free slot is enough. */
void server_ensure_batch_room(app *a);

/*
 * A round that takes long delays every connection waiting in the kernel's accept queue, and a full
 * queue drops connection attempts. Says so on stderr (at most a few lines a second), with the phases.
 */
void server_report_slow_round(const app *a, int events, uint64_t events_done_ns, uint64_t settled_ns, uint64_t end_ns);

/* When memory is critical, evicts idle connections (at most once a second, a bounded number). */
void server_relieve_memory_if_critical(app *a);

/* Closes connections that have been idle for too long, at most once every SWEEP_INTERVAL_SECONDS. */
void server_sweep_idle_if_due(app *a, time_t *last_sweep);

#endif
