#ifndef FEED_URING_SERVER_H
#define FEED_URING_SERVER_H
#include <stdint.h>

/* io_uring HTTP transport: direct (registered) sockets in several rings, multishot receives
 * from provided buffer rings, and bounded per-connection transmit queues. All rings are
 * SINGLE_ISSUER|DEFER_TASKRUN, so every function except feed_uring_stop must be called from
 * the thread that called feed_uring_open. That thread is the only owner; nothing is locked. */
typedef struct feed_uring feed_uring;

enum {
    FEED_EVENT_DATA = 1,     /* bytes received; data stays valid until the next poll */
    FEED_EVENT_CLOSED = 2,   /* the connection is gone; forget its state */
    FEED_EVENT_WRITABLE = 3, /* the transmit queue drained below its high-water mark */
    FEED_EVENT_EOF = 4       /* the peer finished sending; earlier data was delivered first */
};

typedef struct feed_uring_event {
    uint64_t connection;
    const unsigned char *data;
    int32_t length, kind;
} feed_uring_event;

typedef struct feed_uring_response {
    uint64_t connection;
    int32_t offset, length, close_after;
    int32_t result; /* out: 0, or a negative errno when the response was not queued */
} feed_uring_response;

/* 1 when a SINGLE_ISSUER|DEFER_TASKRUN ring can be created (Linux 6.1+, io_uring not blocked)
 * and RLIMIT_NOFILE covers `slots` registered descriptors per ring; otherwise 0. */
int32_t feed_uring_available(int32_t slots);
/* Binds and listens on host_ipv4:port. out_error receives a positive errno on failure. */
feed_uring *feed_uring_open(const char *host_ipv4, int32_t port, int32_t rings,
    int32_t slots_per_ring, int32_t *out_error);
/* Returns the number of events (0 on timeout) or a negative errno; -ECANCELED after stop. */
int32_t feed_uring_poll(feed_uring *, feed_uring_event *events, int32_t max_events, int32_t timeout_ms);
int32_t feed_uring_send(feed_uring *, uint64_t connection, const void *data, int32_t length,
    int32_t close_after);
/* Queues `count` (<= 256) responses whose bytes are slices of `data`. */
int32_t feed_uring_send_batch(feed_uring *, const void *data, int32_t length,
    feed_uring_response *entries, int32_t count);
/* Bytes still queued for transmission, or -ENOTCONN. */
int64_t feed_uring_pending_tx(feed_uring *, uint64_t connection);
int32_t feed_uring_close(feed_uring *, uint64_t connection);
/* Close after the queued responses have been sent. */
int32_t feed_uring_finish(feed_uring *, uint64_t connection);
/* Safe from any thread (for example a signal handler); the next poll returns -ECANCELED. */
void feed_uring_stop(feed_uring *);
void feed_uring_free(feed_uring *);
#endif
