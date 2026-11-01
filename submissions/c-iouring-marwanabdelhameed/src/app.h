#ifndef APP_H
#define APP_H
#include <stddef.h>
#include <stdint.h>

/* One client connection. The I/O backend owns fd and the flags marked (io); app.c owns the rest. */
typedef struct conn {
    int fd;
    uint8_t dead;     /* (io) fd closed, struct freed at the end of the batch */
    uint8_t sending;  /* (io) epoll only: waiting for EPOLLOUT */
    uint8_t hold;     /* a write is queued in the open transaction; the response must not be sent yet */
    uint8_t closing;  /* close after the queued output is sent */
    char *in;
    uint32_t in_len, in_cap;
    char *out;
    uint32_t out_len, out_off, out_cap;
    uint32_t hold_mark; /* out_len before the held response */
} conn;

int app_init(const char *db_path, const char *jwt_secret);

/* Parses and answers every complete request in c->in, one at a time. A write answers into c->out and sets
 * c->hold, and the response is released by app_commit. Stops at an incomplete request. */
void app_process(conn *c);

int app_has_pending(void);

/* Commits the open transaction (group commit) and returns the connections whose responses may now be sent.
 * The array stays valid until the next call. A failed commit rewrites each held response to a 500. */
int app_commit(conn ***released);

#endif
