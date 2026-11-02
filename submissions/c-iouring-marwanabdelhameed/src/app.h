#ifndef APP_H
#define APP_H
#include <stddef.h>
#include <stdint.h>

/* Experiment flags (all off by default, see exp/README.md). V_SMALL_CONN keeps the first CONN_INLINE_IN bytes of
 * the input buffer inside the connection struct, so an idle connection owns one allocation. */
/* V_MULTISHOT_RECV leaves an idle connection with no input buffer at all, so the inline buffer is not used then. */
#if defined(V_SMALL_CONN) && !defined(V_MULTISHOT_RECV)
#define CONN_INLINE_IN 512
#endif

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
#ifdef V_MULTISHOT_RECV
    uint8_t in_ext;     /* (io) c->in points into a provided ring buffer that the loop gives back; app.c copies out before growing */
    uint8_t recv_armed; /* (io) a multishot recv is in flight */
#endif
#ifdef V_ZC_SEND
    int32_t zc_res; /* (io) result of a zero-copy send whose notification has not arrived yet */
#endif
#ifdef CONN_INLINE_IN
    char in0[CONN_INLINE_IN]; /* initial input buffer; c->in points here until a request needs more */
#endif
} conn;

int app_init(const char *db_path, const char *jwt_secret);

/* Parses and answers every complete request in c->in, one at a time. A write answers into c->out and sets
 * c->hold, and the response is released by app_commit. Stops at an incomplete request. */
void app_process(conn *c);

int app_has_pending(void);

#ifdef V_GROUP_COMMIT_N
size_t app_pending_count(void); /* writes waiting for the next group commit */
#endif

/* Commits the open transaction (group commit) and returns the connections whose responses may now be sent.
 * The array stays valid until the next call. A failed commit rewrites each held response to a 500. */
int app_commit(conn ***released);

#endif
