#ifndef APP_H
#define APP_H
#include <stddef.h>
#include <stdint.h>

struct out {
    char *p;
    size_t len;
    size_t cap;
    uint8_t small; /* allocated as a compact pending-request buffer, not a pooled response */
};

struct app {
    const unsigned char *secret;
    size_t secret_len;
    long long started_ns;
};

struct http_request;

void app_init(struct app *a, const char *secret, size_t secret_len);
/* Appends one complete HTTP response to o. */
void app_handle(struct app *a, const struct http_request *req, struct out *o);
/* Replaces o with a minimal 500 response (used when a group commit fails). */
void app_500(struct out *o);

/* Output buffer helpers, also used by the server. */
void out_clear(struct out *o);
void out_free(struct out *o);
void out_release(struct out *o); /* returns the buffer to the process-wide pool */
int out_reserve(struct out *o, size_t extra);
/* Compact growable buffer for a connection's pending/incomplete request bytes. Released with
 * out_release (which frees it instead of returning it to the response pool). */
int out_reserve_partial(struct out *o, size_t extra);
void out_lit(struct out *o, const char *s, size_t n);

#endif
