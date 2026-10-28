/* app: the state every request handler works with. One instance, owned by the server. */
#ifndef APP_H
#define APP_H

#include <time.h>

#include "batch.h"
#include "conn.h"
#include "id_queue.h"
#include "http_parse.h"
#include "jwt.h"
#include "memwatch.h"
#include "store.h"

enum {
	READ_CHUNK = 32768,       /* bytes read from a socket at once */
	SCRATCH_CAP = 160 * 1024, /* response working space: a feed of 20 maximum-size posts fits */
};

typedef struct app {
	conn_table conns;
	store *db;
	jwt_verifier jwt;
	batch *pending;       /* requests waiting for the database */
	id_queue to_resume;   /* connections to continue after their response went out */
	memwatch memory;      /* MemAvailable, checked once a second */
	int fd_limit;         /* RLIMIT_NOFILE (soft) */
	time_t now;           /* refreshed once per event-loop round */
	uint64_t now_ns;      /* monotonic clock, refreshed with `now` */
	time_t started_at;
	char read_buffer[READ_CHUNK]; /* shared by all connections: bytes are parsed straight from here */
	char scratch[SCRATCH_CAP];    /* shared response working space */
	char decoded_body[HTTP_MAX_REQUEST_BYTES]; /* a request body, decoded: never longer than the request */
} app;

#endif
