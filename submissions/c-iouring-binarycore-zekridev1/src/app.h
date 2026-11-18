#ifndef BINARYCORE_APP_H
#define BINARYCORE_APP_H

#include "database.h"
#include "http.h"
#include "jwt.h"

/* The five endpoints. One request in, one complete HTTP response out, written
 * straight into the batch arena. Every error response is a constant prepared
 * at startup. Modeled on c-iouring-pavlyhalim/src/app.h. */

struct bc_app {
    struct bc_db db;
    struct bc_hmac_key key;
    int64_t now;    /* seconds, refreshed per batch */
    int64_t start;  /* process start time (seconds) */
    /* counters */
    uint64_t n_req, n_feed, n_post, n_create, n_like, n_err;
};

struct bc_resp {
    uint8_t *p;
    unsigned n;
    unsigned char close;
    unsigned char db;
    unsigned char held;
};

#define bc_app_resp_max (64 * 1024)

/* Constant error responses (filled by bc_app_init_responses). */
extern uint8_t bc_r_404[256], bc_r_400_id[256], bc_r_404_post[256],
    bc_r_401_missing[256], bc_r_401_invalid[256], bc_r_401_payload[256],
    bc_r_400_json[256], bc_r_400_body[256], bc_r_400_long[256],
    bc_r_500[256], bc_r_400_bad[256], bc_r_503[256];
extern unsigned bc_l_404, bc_l_400_id, bc_l_404_post, bc_l_401_missing,
    bc_l_401_invalid, bc_l_401_payload, bc_l_400_json, bc_l_400_body,
    bc_l_400_long, bc_l_500, bc_l_400_bad, bc_l_503;

static inline unsigned bc_mk_resp(uint8_t *dst, const char *status,
                                  const char *body, int close) {
    return (unsigned)snprintf((char *)dst, 256,
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s%s",
        status, strlen(body), close ? "Connection: close\r\n" : "", body);
}

void bc_app_init_responses(void);

void bc_app_init(struct bc_app *a, const char *db_path, const char *secret,
                 int cache_kib);

static inline void bc_resp_static(struct bc_resp *out, uint8_t *free,
                                  const uint8_t *r, unsigned n) {
    memcpy(free, r, n);
    out->p = free;
    out->n = n;
}

/* Put the status line and headers in front of a body written at free+hdr_room. */
static inline void bc_resp_frame(struct bc_resp *out, uint8_t *free,
                                 const char *status, unsigned status_len,
                                 unsigned body_n, int close) {
    char hdr[bc_hdr_room];
    unsigned h = 0;
    memcpy(hdr + h, "HTTP/1.1 ", 9); h += 9;
    memcpy(hdr + h, status, status_len); h += status_len;
    memcpy(hdr + h, "\r\nContent-Type: application/json\r\nContent-Length: ", 50);
    h += 50;
    h += bc_u64_to_str(hdr + h, body_n);
    if (close) { memcpy(hdr + h, "\r\nConnection: close", 19); h += 19; }
    memcpy(hdr + h, "\r\n\r\n", 4); h += 4;
    out->p = free + bc_hdr_room - h;
    memcpy(out->p, hdr, h);
    out->n = h + body_n;
}

static inline int bc_path_is(const struct bc_req *r, const char *lit, unsigned n) {
    return r->path_len == n && memcmp(r->path, lit, n) == 0;
}

/* Authenticate. On failure writes the 401 and returns 0. On success fills
 * `claims` and returns 1. */
static inline int bc_app_auth(struct bc_app *a, const struct bc_req *r,
                              uint8_t *free, struct bc_resp *out,
                              struct bc_jwt_claims *claims) {
    if (!r->auth || r->auth_len < 7 ||
        memcmp(r->auth, "Bearer ", 7) != 0) {
        bc_resp_static(out, free, bc_r_401_missing, bc_l_401_missing);
        return 0;
    }
    int rc = bc_jwt_verify(&a->key, r->auth + 7, r->auth_len - 7,
                           a->now, claims);
    if (rc == bc_jwt_ok) return 1;
    if (rc == bc_jwt_invalid)
        bc_resp_static(out, free, bc_r_401_invalid, bc_l_401_invalid);
    else
        bc_resp_static(out, free, bc_r_401_payload, bc_l_401_payload);
    return 0;
}

static inline int bc_is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

/* Handle one request. `free` points at bc_app_resp_max bytes of arena.
 * `writes_ok` is the write-gate flag. `now` is current epoch seconds. */
void bc_app_handle(struct bc_app *a, const struct bc_req *r, uint8_t *free,
                   int writes_ok, struct bc_resp *out);

#endif /* BINARYCORE_APP_H */
