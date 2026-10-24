#include "app.h"

#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "json.h"
#include "jwt.h"
#include "util.h"

#define HEAD_RESERVE   128 /* room for the status line and headers before the body */
#define BODY_MAX_CHARS 500
#define BODY_MAX_BYTES 2000

enum write_kind { W_CREATE, W_LIKE };

struct pending_write {
    struct app_resp *resp;
    enum write_kind kind;
    int64_t user_id;
    int64_t post_id;
    uint32_t body_len;
    uint32_t username_len;
    char body[BODY_MAX_BYTES];
    char username[JWT_USERNAME_MAX];
};

struct app {
    const struct config *cfg;
    struct db *db;
    struct hmac_key key;
    int64_t start_ns;
    int read_open;
    struct pending_write *pending;
    uint32_t npending, pending_cap;
    char *body_scratch; /* max_body + 1 bytes for the unescaped request body */
    struct app_stats stats;
};

struct app *app_new(const struct config *cfg, struct db *db) {
    struct app *app = calloc(1, sizeof *app);
    if (!app) return NULL;
    app->cfg = cfg;
    app->db = db;
    hmac_key_init(&app->key, cfg->jwt_secret, strlen(cfg->jwt_secret));
    app->start_ns = now_ns();
    app->pending_cap = cfg->work_budget;
    app->pending = calloc(app->pending_cap, sizeof *app->pending);
    app->body_scratch = malloc((size_t)cfg->max_body + 1);
    if (!app->pending || !app->body_scratch) {
        app_free(app);
        return NULL;
    }
    return app;
}

void app_free(struct app *app) {
    if (!app) return;
    free(app->pending);
    free(app->body_scratch);
    free(app);
}

/* ------------------------------------------------------------------ responses */

static void finish(struct app_resp *resp, int status, size_t body_len) {
    char head[HEAD_RESERVE];
    size_t hl = http_write_head(head, sizeof head, status, body_len, resp->close_after);
    resp->off = HEAD_RESERVE - hl;
    memcpy(resp->buf + resp->off, head, hl);
    resp->len = hl + body_len;
}

static char *body_start(struct app_resp *resp) {
    return resp->buf + HEAD_RESERVE;
}
static size_t body_cap(struct app_resp *resp) {
    return resp->cap - HEAD_RESERVE;
}

static void error_response(struct app_resp *resp, int status, const char *message) {
    char *b = body_start(resp);
    size_t ml = strlen(message);
    memcpy(b, "{\"error\":\"", 10);
    memcpy(b + 10, message, ml);
    memcpy(b + 10 + ml, "\"}", 2);
    finish(resp, status, 12 + ml);
}

void app_error_response(struct app_resp *resp, int status, const char *message) {
    error_response(resp, status, message);
}

static void count_status(struct app *app, int status) {
    if (status < 300) {
        app->stats.status_2xx++;
    } else if (status < 500) {
        app->stats.status_4xx++;
    } else {
        app->stats.status_5xx++;
    }
}

/* ------------------------------------------------------------------ helpers */

static int parse_post_id(const char *s, size_t len, int64_t *out) {
    uint64_t v;
    if (parse_u64(s, len, &v) || v == 0) return -1;
    *out = (int64_t)v;
    return 0;
}

/* Returns 0 with the claims filled, or the status/message to answer with. */
static int authenticate(struct app *app, const struct http_req *req, struct jwt_claims *claims, const char **msg) {
    if (!req->auth || req->auth_len < 7 || memcmp(req->auth, "Bearer ", 7) != 0) {
        *msg = "missing bearer token";
        return 401;
    }
    enum jwt_status st = jwt_verify(&app->key, req->auth + 7, req->auth_len - 7, wall_s(), claims);
    if (st == JWT_INVALID) {
        *msg = "invalid or expired token";
        return 401;
    }
    if (st == JWT_PAYLOAD) {
        *msg = "invalid token payload";
        return 401;
    }
    return 0;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Validates the create body. Returns 0 with [*out, *out+*out_len) pointing into
 * the scratch buffer, or the status to answer with. */
static int validate_body(struct app *app, const struct http_req *req, const char **out, size_t *out_len,
                         const char **msg) {
    struct json_field f = {.name = "body", .buf = app->body_scratch, .cap = (size_t)app->cfg->max_body + 1};
    enum json_status js = json_parse_object(req->body, req->body_len, &f, 1);
    if (js == JSON_MALFORMED) {
        *msg = "malformed JSON body";
        return 400;
    }
    if (js == JSON_NOT_OBJECT || f.kind != JSON_STRING || f.truncated) {
        *msg = "body is required";
        return 400;
    }
    const char *b = app->body_scratch;
    size_t len = f.len;
    while (len && is_space(b[0])) {
        b++;
        len--;
    }
    while (len && is_space(b[len - 1])) len--;
    if (len == 0) {
        *msg = "body is required";
        return 400;
    }
    size_t chars = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)b[i] & 0xc0) != 0x80) chars++;
    }
    if (chars > BODY_MAX_CHARS || len > BODY_MAX_BYTES) {
        *msg = "body must be at most 500 characters";
        return 400;
    }
    *out = b;
    *out_len = len;
    return 0;
}

/* ------------------------------------------------------------------ handlers */

static enum app_result handle_health(struct app *app, struct app_resp *resp) {
    app->stats.health++;
    char err[256];
    char *b = body_start(resp);
    if (db_health(app->db, err, sizeof err)) {
        size_t o = 0;
        memcpy(b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":", 48);
        o = 48;
        o += json_write_string(b + o, body_cap(resp) - o, err, strlen(err));
        b[o++] = '}';
        finish(resp, 503, o);
        count_status(app, 503);
        return APP_DONE;
    }
    int64_t up = (now_ns() - app->start_ns) / 1000000000;
    memcpy(b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36);
    size_t o = 36 + fmt_i64(b + 36, up);
    b[o++] = '}';
    finish(resp, 200, o);
    count_status(app, 200);
    return APP_DONE;
}

static void open_read(struct app *app) {
    if (!app->read_open) {
        db_read_begin(app->db);
        app->read_open = 1;
    }
}

static enum app_result handle_feed(struct app *app, struct app_resp *resp) {
    app->stats.feed++;
    open_read(app);
    size_t n = db_feed_json(app->db, body_start(resp), body_cap(resp));
    if (!n) {
        error_response(resp, 500, "internal server error");
        count_status(app, 500);
        return APP_DONE;
    }
    finish(resp, 200, n);
    count_status(app, 200);
    return APP_DONE;
}

static enum app_result handle_post_get(struct app *app, int64_t id, struct app_resp *resp) {
    app->stats.post_get++;
    open_read(app);
    size_t n = db_post_json(app->db, id, body_start(resp), body_cap(resp));
    if (n == 0) {
        error_response(resp, 404, "post not found");
        count_status(app, 404);
    } else if (n == (size_t)-1) {
        error_response(resp, 500, "internal server error");
        count_status(app, 500);
    } else {
        finish(resp, 200, n);
        count_status(app, 200);
    }
    return APP_DONE;
}

static struct pending_write *defer(struct app *app, struct app_resp *resp, enum write_kind kind,
                                   const struct jwt_claims *claims) {
    if (app->npending >= app->pending_cap) return NULL;
    struct pending_write *w = &app->pending[app->npending++];
    w->resp = resp;
    w->kind = kind;
    w->user_id = claims->sub;
    w->username_len = (uint32_t)claims->username_len;
    memcpy(w->username, claims->username, claims->username_len);
    w->body_len = 0;
    w->post_id = 0;
    return w;
}

static enum app_result handle_create(struct app *app, const struct http_req *req, struct app_resp *resp) {
    app->stats.post_create++;
    struct jwt_claims claims;
    const char *msg;
    int status = authenticate(app, req, &claims, &msg);
    if (status) goto fail;
    const char *body;
    size_t body_len;
    status = validate_body(app, req, &body, &body_len, &msg);
    if (status) goto fail;
    if (db_writes_held(app->db)) {
        app->stats.held++;
        return APP_HELD;
    }
    struct pending_write *w = defer(app, resp, W_CREATE, &claims);
    if (!w) {
        status = 500;
        msg = "internal server error";
        goto fail;
    }
    memcpy(w->body, body, body_len);
    w->body_len = (uint32_t)body_len;
    return APP_DEFERRED;
fail:
    error_response(resp, status, msg);
    count_status(app, status);
    return APP_DONE;
}

static enum app_result handle_like(struct app *app, const struct http_req *req, const char *id_s, size_t id_len,
                                   struct app_resp *resp) {
    app->stats.like++;
    struct jwt_claims claims;
    const char *msg;
    int64_t id;
    /* auth before id validation, as the SPEC requires */
    int status = authenticate(app, req, &claims, &msg);
    if (status) goto fail;
    if (parse_post_id(id_s, id_len, &id)) {
        status = 400;
        msg = "invalid post id";
        goto fail;
    }
    if (db_writes_held(app->db)) {
        app->stats.held++;
        return APP_HELD;
    }
    struct pending_write *w = defer(app, resp, W_LIKE, &claims);
    if (!w) {
        status = 500;
        msg = "internal server error";
        goto fail;
    }
    w->post_id = id;
    return APP_DEFERRED;
fail:
    error_response(resp, status, msg);
    count_status(app, status);
    return APP_DONE;
}

/* ------------------------------------------------------------------ routing */

static enum app_result not_found(struct app *app, struct app_resp *resp) {
    error_response(resp, 404, "not found");
    count_status(app, 404);
    return APP_DONE;
}

enum app_result app_handle(struct app *app, const struct http_req *req, struct app_resp *resp) {
    const char *p = req->path;
    size_t n = req->path_len;
    if (req->method == HTTP_GET) {
        if (n == 7 && memcmp(p, "/health", 7) == 0) return handle_health(app, resp);
        if (n == 5 && memcmp(p, "/feed", 5) == 0) return handle_feed(app, resp);
        if (n > 7 && memcmp(p, "/posts/", 7) == 0 && !memchr(p + 7, '/', n - 7)) {
            int64_t id;
            if (parse_post_id(p + 7, n - 7, &id)) {
                error_response(resp, 400, "invalid post id");
                count_status(app, 400);
                return APP_DONE;
            }
            return handle_post_get(app, id, resp);
        }
        return not_found(app, resp);
    }
    if (req->method == HTTP_POST) {
        if (n == 6 && memcmp(p, "/posts", 6) == 0) return handle_create(app, req, resp);
        if (n > 12 && memcmp(p, "/posts/", 7) == 0 && memcmp(p + n - 5, "/like", 5) == 0 &&
            !memchr(p + 7, '/', n - 12)) {
            return handle_like(app, req, p + 7, n - 12, resp);
        }
        return not_found(app, resp);
    }
    return not_found(app, resp);
}

/* ------------------------------------------------------------------ batches */

void app_begin_batch(struct app *app) {
    app->npending = 0;
    app->read_open = 0;
}

static void fail_all(struct app *app) {
    for (uint32_t i = 0; i < app->npending; i++) {
        error_response(app->pending[i].resp, 500, "internal server error");
    }
    app->stats.status_5xx += app->npending;
}

static void write_like_response(struct app *app, struct pending_write *w, int rc) {
    struct app_resp *resp = w->resp;
    char *b = body_start(resp);
    size_t o;
    if (rc == 0) {
        error_response(resp, 404, "post not found");
        count_status(app, 404);
        return;
    }
    if (rc == 2) {
        memcpy(b, "{\"liked\":true,\"already_liked\":false,\"post_id\":", 46);
        o = 46;
    } else {
        memcpy(b, "{\"liked\":true,\"already_liked\":true,\"post_id\":", 45);
        o = 45;
    }
    o += fmt_i64(b + o, w->post_id);
    b[o++] = '}';
    finish(resp, rc == 2 ? 201 : 200, o);
    count_status(app, rc == 2 ? 201 : 200);
}

static void write_create_response(struct app *app, struct pending_write *w, int64_t id, const char *created_at) {
    struct app_resp *resp = w->resp;
    char *b = body_start(resp);
    memcpy(b, "{\"post\":", 8);
    size_t o = 8;
    size_t n = db_write_post_json(b + o, body_cap(resp) - o, id, w->body, w->body_len, created_at, strlen(created_at),
                                  w->username, w->username_len, 0);
    if (!n) {
        error_response(resp, 500, "internal server error");
        count_status(app, 500);
        return;
    }
    o += n;
    b[o++] = '}';
    finish(resp, 201, o);
    count_status(app, 201);
}

void app_end_batch(struct app *app) {
    if (app->read_open) {
        db_read_end(app->db);
        app->read_open = 0;
    }
    if (!app->npending) return;
    app->stats.write_batches++;
    app->stats.writes += app->npending;
    if (db_write_begin(app->db)) {
        app->stats.commit_failures++;
        fail_all(app);
        app->npending = 0;
        return;
    }
    for (uint32_t i = 0; i < app->npending; i++) {
        struct pending_write *w = &app->pending[i];
        int rc;
        if (w->kind == W_LIKE) {
            rc = db_like(app->db, w->user_id, w->post_id);
            if (rc >= 0) {
                write_like_response(app, w, rc);
            }
        } else {
            int64_t id;
            char created_at[32];
            rc = db_create_post(app->db, w->user_id, w->body, w->body_len, &id, created_at);
            if (rc == 0) write_create_response(app, w, id, created_at);
        }
        if (rc < 0) {
            if (!db_txn_active(app->db)) {
                /* the error rolled the transaction back: nothing in this batch is committed */
                app->stats.commit_failures++;
                fail_all(app);
                app->npending = 0;
                return;
            }
            error_response(w->resp, 500, "internal server error");
            count_status(app, 500);
        }
    }
    /* Nothing above is sent before this returns: rule 6. */
    if (db_write_commit(app->db)) {
        app->stats.commit_failures++;
        fail_all(app);
    }
    app->npending = 0;
}

int app_writes_held(struct app *app) {
    return db_writes_held(app->db);
}

void app_get_stats(struct app *app, struct app_stats *out) {
    *out = app->stats;
}
