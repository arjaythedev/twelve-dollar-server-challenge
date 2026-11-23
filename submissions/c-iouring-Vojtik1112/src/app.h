/* app.h: HTTP route dispatch, JWT authentication, zero-malloc parsed request representation,
 * and zero-copy arena response framing. */
#ifndef APP_H
#define APP_H

#include "db.h"
#include "http.h"
#include "jwt.h"

#define HDR_ROOM 128
#define RESP_MAX (128u * 1024u)

enum {
    RT_STATIC_ERR = 0,
    RT_FEED = 1,
    RT_POST,
    RT_HEALTH,
    RT_LIKE,
    RT_CREATE
};

struct qarena {
    uint8_t *buf;
    uint32_t used;
    uint32_t cap;
};

struct parsed_req {
    uint8_t route;
    uint8_t close;
    uint8_t is_write;
    uint8_t pad;
    uint32_t name_off;
    uint32_t name_len;
    uint32_t body_off;
    uint32_t body_len;
    int64_t id;
    int64_t user_id;
};

struct app {
    struct db db;
    struct hmac_key key;
    int64_t now;
    int64_t start;
    uint64_t n_req, n_feed, n_post, n_create, n_like, n_err;
};

struct resp {
    uint8_t *p;
    unsigned n;
    uint8_t close;
    uint8_t db;
    uint8_t is_static;
    uint32_t advance; /* bytes consumed from caller scratch free buffer */
};

#define STATIC_RESP 256
enum {
    SR_404 = 0,
    SR_400_ID,
    SR_404_POST,
    SR_401_MISSING,
    SR_401_INVALID,
    SR_401_PAYLOAD,
    SR_400_JSON,
    SR_400_BODY,
    SR_400_LONG,
    SR_500,
    SR_400_BAD,
    SR_503,
    SR_COUNT
};

static uint8_t S_RESP[SR_COUNT][2][STATIC_RESP];
static unsigned S_LEN[SR_COUNT][2];

static unsigned mk_static_resp(uint8_t *dst, const char *status, const char *body, int close) {
    return (unsigned)snprintf((char *)dst, STATIC_RESP,
                              "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s\r\n%s",
                              status, strlen(body), close ? "Connection: close\r\n" : "", body);
}

static void app_init_responses(void) {
    static const struct {
        const char *st;
        const char *body;
    } defs[SR_COUNT] = {
        [SR_404]         = {"404 Not Found", "{\"error\":\"not found\"}"},
        [SR_400_ID]      = {"400 Bad Request", "{\"error\":\"invalid post id\"}"},
        [SR_404_POST]    = {"404 Not Found", "{\"error\":\"post not found\"}"},
        [SR_401_MISSING] = {"401 Unauthorized", "{\"error\":\"missing bearer token\"}"},
        [SR_401_INVALID] = {"401 Unauthorized", "{\"error\":\"invalid or expired token\"}"},
        [SR_401_PAYLOAD] = {"401 Unauthorized", "{\"error\":\"invalid token payload\"}"},
        [SR_400_JSON]    = {"400 Bad Request", "{\"error\":\"malformed JSON body\"}"},
        [SR_400_BODY]    = {"400 Bad Request", "{\"error\":\"body is required\"}"},
        [SR_400_LONG]    = {"400 Bad Request", "{\"error\":\"body must be at most 500 characters\"}"},
        [SR_500]         = {"500 Internal Server Error", "{\"error\":\"internal server error\"}"},
        [SR_400_BAD]     = {"400 Bad Request", "{\"error\":\"bad request\"}"},
        [SR_503]         = {"503 Service Unavailable",
                            "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"select failed\"}"},
    };
    for (int i = 0; i < SR_COUNT; i++) {
        for (int cl = 0; cl < 2; cl++) {
            S_LEN[i][cl] = mk_static_resp(S_RESP[i][cl], defs[i].st, defs[i].body, cl);
        }
    }
}

static int app_init(struct app *a, const char *db_path, const char *secret, int cache_kib) {
    memset(a, 0, sizeof *a);
    app_init_responses();
    hmac_key_init(&a->key, (const uint8_t *)secret, strlen(secret));
    a->start = a->now = (int64_t)time(NULL);
    return db_open(&a->db, db_path, cache_kib);
}

static inline void qarena_append(struct qarena *qa, const uint8_t *p, uint32_t n, uint32_t *off_out) {
    if (unlikely(qa->used + n > qa->cap)) {
        uint32_t ncap = qa->cap ? qa->cap * 2u : (64u * 1024u);
        while (ncap < qa->used + n) ncap *= 2u;
        uint8_t *nb = (uint8_t *)realloc(qa->buf, ncap);
        if (!nb) { perror("realloc qarena"); exit(1); }
        qa->buf = nb;
        qa->cap = ncap;
    }
    *off_out = qa->used;
    if (n > 0) {
        memcpy(qa->buf + qa->used, p, n);
        qa->used += n;
    }
}

static inline void resp_static(struct resp *out, int id) {
    int cl = out->close ? 1 : 0;
    out->p = S_RESP[id][cl];
    out->n = S_LEN[id][cl];
    out->db = 0;
    out->is_static = 1;
    out->advance = 0;
}

/* Prepend HTTP status line and headers directly in front of body at free_buf + HDR_ROOM. */
static inline void resp_frame(struct resp *out, uint8_t *free_buf, const char *status,
                              unsigned status_len, unsigned body_n) {
    unsigned cl_digits = dec_len((uint64_t)body_n);
    unsigned close_len = unlikely(out->close) ? 19u : 0u;
    unsigned h = 9u + status_len + 50u + cl_digits + close_len + 4u;
    uint8_t *dst = free_buf + HDR_ROOM - h;
    out->p = dst;
    memcpy(dst, "HTTP/1.1 ", 9); dst += 9;
    memcpy(dst, status, status_len); dst += status_len;
    memcpy(dst, "\r\nContent-Type: application/json\r\nContent-Length: ", 50); dst += 50;
    dst += u64_to_dec((uint64_t)body_n, (char *)dst);
    if (close_len) {
        memcpy(dst, "\r\nConnection: close", 19);
        dst += 19;
    }
    memcpy(dst, "\r\n\r\n", 4);
    out->n = h + body_n;
    out->is_static = 0;
    out->advance = HDR_ROOM + body_n;
}

static inline int path_is(const struct req *r, const char *lit, unsigned n) {
    return r->path_len == n && memcmp(r->path, lit, n) == 0;
}

static int app_auth_err(const struct app *a, const struct req *r, struct claims *c,
                        uint8_t *namebuf, uint32_t namecap) {
    int rc = jwt_verify(&a->key, r->auth, r->auth_len, a->now, c, namebuf, namecap);
    if (likely(rc == JWT_OK)) return -1;
    if (rc == JWT_MISSING) return SR_401_MISSING;
    if (rc == JWT_INVALID) return SR_401_INVALID;
    return SR_401_PAYLOAD;
}

struct post_body_scan {
    struct jv body;
};

static int on_post_member(void *ud, const uint8_t *k, uint32_t kn, int kesc, struct jv *v) {
    struct post_body_scan *s = (struct post_body_scan *)ud;
    if (!kesc && kn == 4 && memcmp(k, "body", 4) == 0) s->body = *v;
    return 0;
}

/* Parses route, validates auth and JSON/params completely without touching SQLite.
 * Returns 1 if ready for app_exec_parsed(), 0 if immediate static error was placed in err_out
 * (with pr->route = RT_STATIC_ERR and pr->id = error_id so caller can queue it if needed). */
static int app_parse_and_auth(struct app *a, const struct req *r, struct parsed_req *pr,
                              struct qarena *qa, struct resp *err_out) {
    pr->close = r->close;
    pr->is_write = 0;

    if (likely(r->method == M_GET)) {
        if (likely(path_is(r, "/feed", 5))) {
            pr->route = RT_FEED;
            return 1;
        }
        if (likely(r->path_len > 7 && memcmp(r->path, "/posts/", 7) == 0)) {
            int64_t id = 0;
            if (unlikely(!parse_positive(r->path + 7, r->path_len - 7, &id))) {
                pr->route = RT_STATIC_ERR;
                pr->id = SR_400_ID;
                goto ret_err;
            }
            pr->route = RT_POST;
            pr->id = id;
            return 1;
        }
        if (path_is(r, "/health", 7)) {
            pr->route = RT_HEALTH;
            return 1;
        }
        pr->route = RT_STATIC_ERR;
        pr->id = SR_404;
        goto ret_err;
    }

    if (r->method == M_POST) {
        int is_create = path_is(r, "/posts", 6);
        int is_like = !is_create && r->path_len > 12 &&
                      memcmp(r->path, "/posts/", 7) == 0 &&
                      memcmp(r->path + r->path_len - 5, "/like", 5) == 0;
        if (!is_create && !is_like) {
            pr->route = RT_STATIC_ERR;
            pr->id = SR_404;
            goto ret_err;
        }
        struct claims c;
        uint8_t namebuf[1024];
        int aerr = app_auth_err(a, r, &c, namebuf, sizeof(namebuf));
        if (unlikely(aerr >= 0)) {
            pr->route = RT_STATIC_ERR;
            pr->id = aerr;
            goto ret_err;
        }

        if (is_like) {
            int64_t id = 0;
            if (!parse_positive(r->path + 7, r->path_len - 12, &id)) {
                pr->route = RT_STATIC_ERR;
                pr->id = SR_400_ID;
                goto ret_err;
            }
            pr->route = RT_LIKE;
            pr->is_write = 1;
            pr->user_id = c.user_id;
            pr->id = id;
            return 1;
        }

        /* POST /posts */
        struct jp jcheck = {r->body, r->body + r->body_len, 0};
        jp_ws(&jcheck);
        if (jcheck.p >= jcheck.end) {
            pr->route = RT_STATIC_ERR;
            pr->id = SR_400_JSON;
            goto ret_err;
        }
        struct post_body_scan ps;
        memset(&ps, 0, sizeof ps);
        if (*jcheck.p != '{') {
            pr->route = RT_STATIC_ERR;
            pr->id = (jp_value(&jcheck, NULL) != 0 || (jp_ws(&jcheck), jcheck.p != jcheck.end))
                     ? SR_400_JSON : SR_400_BODY;
            goto ret_err;
        }
        if (json_parse_object(r->body, r->body_len, on_post_member, &ps) != 0) {
            pr->route = RT_STATIC_ERR;
            pr->id = SR_400_JSON;
            goto ret_err;
        }
        if (ps.body.type != JV_STRING) {
            pr->route = RT_STATIC_ERR;
            pr->id = SR_400_BODY;
            goto ret_err;
        }

        const uint8_t *trimmed = ps.body.p;
        uint32_t trimmed_n = ps.body.n;
        uint8_t sbuf[4096];
        uint8_t *heap_u = NULL;
        if (ps.body.escaped) {
            uint8_t *dst = sbuf;
            if (ps.body.n > sizeof(sbuf)) {
                heap_u = (uint8_t *)malloc(ps.body.n);
                if (!heap_u) {
                    a->n_err++;
                    pr->route = RT_STATIC_ERR;
                    pr->id = SR_500;
                    goto ret_err;
                }
                dst = heap_u;
            }
            trimmed_n = json_unescape(ps.body.p, ps.body.n, dst);
            trimmed = dst;
        }
        js_trim(&trimmed, &trimmed_n);
        if (trimmed_n == 0) {
            free(heap_u);
            pr->route = RT_STATIC_ERR;
            pr->id = SR_400_BODY;
            goto ret_err;
        }
        if (utf8_count(trimmed, trimmed_n) > 500u) {
            free(heap_u);
            pr->route = RT_STATIC_ERR;
            pr->id = SR_400_LONG;
            goto ret_err;
        }

        pr->route = RT_CREATE;
        pr->is_write = 1;
        pr->user_id = c.user_id;
        pr->name_len = c.username_len;
        qarena_append(qa, c.username, c.username_len, &pr->name_off);
        pr->body_len = trimmed_n;
        qarena_append(qa, trimmed, trimmed_n, &pr->body_off);
        free(heap_u);
        return 1;
    }

    pr->route = RT_STATIC_ERR;
    pr->id = SR_404;
ret_err:
    if (err_out) {
        err_out->close = pr->close;
        resp_static(err_out, (int)pr->id);
    }
    return 0;
}

static void app_exec_parsed(struct app *a, const struct parsed_req *pr, const struct qarena *qa,
                            uint8_t *free_buf, struct resp *out) {
    out->close = pr->close;
    out->db = 0;
    out->is_static = 0;
    out->advance = 0;
    a->n_req++;

    if (unlikely(pr->route == RT_STATIC_ERR)) {
        resp_static(out, (int)pr->id);
        return;
    }

    uint8_t *body_dst = free_buf + HDR_ROOM;
    struct ob o;
    ob_init(&o, body_dst, RESP_MAX - HDR_ROOM);

    switch (pr->route) {
    case RT_FEED:
        a->n_feed++;
        out->db = 1;
        if (unlikely(db_feed(&a->db, &o) < 0 || o.err)) goto err500;
        resp_frame(out, free_buf, "200 OK", 6, (unsigned)o.n);
        return;
    case RT_POST: {
        a->n_post++;
        out->db = 1;
        int rc = db_post(&a->db, &o, pr->id);
        if (unlikely(rc < 0 || o.err)) goto err500;
        if (rc == 0) {
            resp_static(out, SR_404_POST);
            return;
        }
        resp_frame(out, free_buf, "200 OK", 6, (unsigned)o.n);
        return;
    }
    case RT_HEALTH:
        if (db_health(&a->db) < 0) {
            resp_static(out, SR_503);
            return;
        }
        ob_lit(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        ob_u64(&o, (uint64_t)(a->now >= a->start ? a->now - a->start : 0));
        ob_char(&o, '}');
        resp_frame(out, free_buf, "200 OK", 6, (unsigned)o.n);
        return;
    case RT_LIKE: {
        a->n_like++;
        out->db = 1;
        int ex = db_post_exists(&a->db, pr->id);
        if (unlikely(ex < 0)) goto err500;
        if (ex == 0) {
            resp_static(out, SR_404_POST);
            return;
        }
        int rc = db_insert_like(&a->db, pr->user_id, pr->id, NULL);
        if (unlikely(rc < 0)) goto err500;
        if (rc == 1) {
            resp_static(out, SR_404_POST);
            return;
        }
        ob_lit(&o, "{\"liked\":true,\"already_liked\":");
        if (rc == 2) ob_lit(&o, "true,\"post_id\":");
        else ob_lit(&o, "false,\"post_id\":");
        ob_u64(&o, (uint64_t)pr->id);
        ob_char(&o, '}');
        if (rc == 0) resp_frame(out, free_buf, "201 Created", 11, (unsigned)o.n);
        else resp_frame(out, free_buf, "200 OK", 6, (unsigned)o.n);
        return;
    }
    case RT_CREATE: {
        a->n_create++;
        out->db = 1;
        const uint8_t *uname = qa->buf + pr->name_off;
        const uint8_t *body = qa->buf + pr->body_off;
        int64_t new_id = 0;
        char created[25];
        int rc = db_insert_post(&a->db, pr->user_id, (const char *)body, (int)pr->body_len,
                                &new_id, created);
        if (rc != 0) {
            if (rc == 1) resp_static(out, SR_400_LONG);
            else goto err500;
            return;
        }
        ob_lit(&o, "{\"post\":");
        ob_post_obj(&o, new_id, body, pr->body_len, (const uint8_t *)created, 24,
                    uname, pr->name_len, 0);
        ob_char(&o, '}');
        if (unlikely(o.err)) goto err500;
        resp_frame(out, free_buf, "201 Created", 11, (unsigned)o.n);
        return;
    }
    default:
        resp_static(out, SR_404);
        return;
    }

err500:
    a->n_err++;
    resp_static(out, SR_500);
}

static struct qarena g_bench_qa = {NULL, 0, 0};

static inline void app_handle(struct app *a, const struct req *r, uint8_t *free_buf,
                              struct resp *out) {
    struct parsed_req pr;
    g_bench_qa.used = 0;
    if (!app_parse_and_auth(a, r, &pr, &g_bench_qa, out)) {
        a->n_req++;
        return;
    }
    app_exec_parsed(a, &pr, &g_bench_qa, free_buf, out);
    g_bench_qa.used = 0;
}

#endif
