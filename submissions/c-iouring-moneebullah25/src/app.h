/* Request handling: routing, auth, validation and the batch (group commit)
 * protocol. The network layer parses bytes into http_req, hands each request
 * to app_handle() inside a batch, and sends nothing until app_end_batch()
 * has committed every write of the batch. */
#ifndef APP_H
#define APP_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "db.h"
#include "http.h"

struct app;

/* A response buffer. The body is written first at a fixed offset and the
 * status line is placed just before it, so [off, off+len) is the wire data. */
struct app_resp {
    char *buf;
    size_t cap;
    size_t off;
    size_t len;
    uint8_t close_after;        /* in: the request asked to close; out: forced by the app on protocol errors */
    uint8_t announce_keepalive; /* in: HTTP/1.0 request that asked for keep-alive */
};

enum app_result {
    APP_DONE,     /* resp is complete */
    APP_DEFERRED, /* a write; resp is completed by app_end_batch() */
    APP_HELD,     /* a write while writes are held: retry the request later */
};

struct app_stats {
    uint64_t health, feed, post_get, post_create, like;
    uint64_t status_2xx, status_4xx, status_5xx;
    uint64_t write_batches, writes, commit_failures, held;
};

struct app *app_new(const struct config *cfg, struct db *db);
void app_free(struct app *app);

void app_begin_batch(struct app *app);
enum app_result app_handle(struct app *app, const struct http_req *req, struct app_resp *resp);
void app_end_batch(struct app *app);

/* Writes a complete error or protocol response (used by the network layer for
 * 400/413/500 outside a batch). */
void app_error_response(struct app_resp *resp, int status, const char *message);

/* 1 when writes should not be attempted right now. */
int app_writes_held(struct app *app);
void app_get_stats(struct app *app, struct app_stats *out);

#endif
