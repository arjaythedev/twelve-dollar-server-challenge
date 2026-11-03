/*
 * conn: the table of client connections, indexed by connection id.
 *
 * A connection id is the file descriptor under epoll and (ring, slot) under io_uring; the backend
 * (conn_epoll.c, server_uring.c) moves the bytes, this file keeps what is the same for both.
 *
 * A connection owns heap memory only while it needs it: `carry` while it holds input that is not
 * processed yet, `unsent` while the socket buffer is full. An idle keep-alive connection costs one
 * table entry (about 50 bytes) plus the kernel's socket.
 */
#ifndef CONN_H
#define CONN_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "strview.h"

typedef enum conn_flag {
	CONN_OPEN = 1 << 0,
	CONN_AWAITING_BATCH = 1 << 1,   /* a request is queued in the batch; parse nothing more until it is answered */
	CONN_SEND_PENDING = 1 << 2,     /* a response is on its way out: bytes wait for the socket (epoll) or a send is in flight (io_uring) */
	CONN_CLOSE_AFTER_SEND = 1 << 3, /* close once everything is sent */
	CONN_SENT_CONTINUE = 1 << 4,    /* "100 Continue" went out for the request being received */
	CONN_DRAINING = 1 << 5,         /* the response is sent; waiting for the client to close first (see conn_drain_start) */
	CONN_ABORTIVE = 1 << 6,         /* close at once after sending: a protocol error, nothing to wait for */
} conn_flag;

typedef struct conn {
	char *carry;         /* input received but not processed: a partial request, or requests behind a queued one */
	uint32_t carry_len;
	uint32_t carry_cap;
	char *unsent;        /* response bytes the socket did not accept yet (epoll) or that wait behind a send in flight (io_uring) */
	uint32_t unsent_len;
	uint32_t unsent_off;
	uint32_t last_active; /* seconds, wall clock */
	uint32_t generation;  /* counts how often this id was reused; survives conn_close */
	uint8_t flags;
} conn;

/* A connection that waits for its client to close, and the time at which the server gives up waiting. */
typedef struct drain_entry {
	int id;
	uint32_t generation;
	uint64_t deadline_ns;
} drain_entry;

typedef struct conn_table conn_table;

/* What differs between epoll and io_uring. */
typedef struct conn_backend {
	/* Sends `data` after anything already on its way, as one response. */
	void (*send)(conn_table *table, int id, strview data);
	/* Hands the socket back to the kernel. The table entry is already reset when this runs. */
	void (*release)(conn_table *table, int id);
} conn_backend;

struct conn_table {
	conn *entries;
	int id_space;   /* ids are 0 .. id_space - 1 */
	int capacity;   /* connections the backend can hold at most: the descriptor limit under epoll */
	int highest_id; /* highest id ever registered; bounds the idle sweep */
	int open_count; /* connections currently open */
	int evict_cursor; /* where conn_evict_idle continues */
	const conn_backend *backend;
	void *backend_data;
	drain_entry *drains; /* FIFO, oldest deadline first */
	size_t drain_head, drain_tail, drain_cap;
};

/* How long a connection that answered "Connection: close" waits for the client to close it. */
enum { CONN_DRAIN_TIMEOUT_MS = 250 };

void conn_table_init(conn_table *table, int id_space, int capacity, const conn_backend *backend, void *backend_data);

static inline conn *conn_get(conn_table *table, int id)
{
	return &table->entries[id];
}

static inline bool conn_has(const conn *c, unsigned flags)
{
	return (c->flags & flags) != 0;
}

static inline void conn_set(conn *c, unsigned flags)
{
	c->flags |= (uint8_t)flags;
}

static inline void conn_clear(conn *c, unsigned flags)
{
	c->flags &= (uint8_t)~flags;
}

/* Connections worth of room kept free for the database, the listeners, accept bursts and connections that are draining. */
static inline int conn_reserve(int capacity)
{
	int reserve = capacity / 16;

	return reserve < 32 ? 32 : reserve > 4096 ? 4096 : reserve;
}

/*
 * True when the table is nearly out of descriptors. The server then answers with
 * "Connection: close", so more users than descriptors can be served: a user that finds no
 * persistent connection reconnects for each request.
 */
static inline bool conn_table_is_crowded(const conn_table *table)
{
	return table->open_count >= table->capacity - conn_reserve(table->capacity);
}

/* Records an accepted connection. The backend has already taken the socket (epoll_ctl, or a direct descriptor). */
void conn_register(conn_table *table, int id, time_t now);

/* Releases everything the connection owns and gives the socket back to the kernel. */
void conn_close(conn_table *table, int id);

/* Sends `data` through the backend. */
static inline void conn_send(conn_table *table, int id, strview data)
{
	table->backend->send(table, id, data);
}

/* Replaces the carried input with a copy of `data` (which may overlap the carry). */
void conn_carry_set(conn *c, const char *data, size_t len);
void conn_carry_clear(conn *c);

/* Appends `len` bytes to the carried input. */
void conn_carry_append(conn *c, const char *data, size_t len);

/*
 * Called once the response of a connection that must close is sent. The client should close first:
 * whoever closes first keeps the socket in TIME_WAIT for 60 seconds, and a server that closes 9,000
 * connections a second overflows the kernel's TIME_WAIT table, which makes it drop state early,
 * reset late segments and cause retransmission storms. A client such as Go's HTTP client closes as
 * soon as it has read a "Connection: close" response. If it has not after CONN_DRAIN_TIMEOUT_MS,
 * conn_drain_expire closes the connection.
 */
void conn_drain_start(conn_table *table, int id, uint64_t now_ns);

/* Closes the draining connections whose time ran out. */
void conn_drain_expire(conn_table *table, uint64_t now_ns);

static inline bool conn_drain_pending(const conn_table *table)
{
	return table->drain_head != table->drain_tail;
}

/*
 * Emergency relief when memory is almost gone: closes up to `max` connections that have been idle for at least
 * `min_idle_seconds` and have nothing pending, going round the table from where it stopped last time. A client
 * whose request crosses the close loses it; that is the price of not being killed. Returns how many it closed.
 */
unsigned conn_evict_idle(conn_table *table, time_t now, unsigned min_idle_seconds, unsigned max);

/* Closes connections that have been idle longer than `max_idle_seconds`. */
void conn_close_idle(conn_table *table, time_t now, unsigned max_idle_seconds);

#endif
