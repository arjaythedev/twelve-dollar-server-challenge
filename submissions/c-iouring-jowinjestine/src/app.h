/* app.h: the five endpoints. Each handler builds one complete HTTP response in the region it
 * is given: the body first, then the status line and headers placed right in front of it, so
 * nothing is copied and there is no Content-Length placeholder to patch. */
#ifndef APP_H
#define APP_H

#include "db.h"
#include "http.h"
#include "jwt.h"

#define RESP_MAX (160 * 1024)
#define HDR_MAX 160
#define MAX_BODY_CHARS 500

struct app {
  struct db db;
  struct hmac_key key;
  int64_t now; /* seconds since the epoch, refreshed every batch */
  uint64_t started_s;
  uint64_t n_err;
};

struct resp {
  uint8_t *p;
  uint32_t n;
  uint8_t db;    /* touched the database: becomes a 500 if the batch's commit fails */
  uint8_t close; /* close the connection after sending */
  uint8_t held;  /* a write during a checkpoint hold: not answered now, kept on the connection */
};

static const char *status_line(int s, unsigned *len) {
  const char *t;
  switch (s) {
  case 200: t = "HTTP/1.1 200 OK\r\n"; break;
  case 201: t = "HTTP/1.1 201 Created\r\n"; break;
  case 400: t = "HTTP/1.1 400 Bad Request\r\n"; break;
  case 401: t = "HTTP/1.1 401 Unauthorized\r\n"; break;
  case 404: t = "HTTP/1.1 404 Not Found\r\n"; break;
  case 503: t = "HTTP/1.1 503 Service Unavailable\r\n"; break;
  default: t = "HTTP/1.1 500 Internal Server Error\r\n"; break;
  }
  *len = (unsigned)strlen(t);
  return t;
}

/* The body sits at region + HDR_MAX, body_n bytes long: writes the head in front of it. */
static void finish(uint8_t *region, int status, uint32_t body_n, int close, struct resp *out) {
  char h[HDR_MAX];
  unsigned sl;
  const char *s = status_line(status, &sl);
  unsigned n = 0;
  memcpy(h, s, sl);
  n += sl;
  memcpy(h + n, "Content-Type: application/json\r\nContent-Length: ", 48);
  n += 48;
  n += u64_to_dec(body_n, h + n);
  if (close) { memcpy(h + n, "\r\nConnection: close\r\n\r\n", 23); n += 23; }
  else { memcpy(h + n, "\r\n\r\n", 4); n += 4; }
  uint8_t *p = region + HDR_MAX - n;
  memcpy(p, h, n);
  out->p = p;
  out->n = n + body_n;
}

static void error_resp(uint8_t *region, int status, const char *msg, struct resp *out) {
  struct ob o;
  ob_init(&o, region + HDR_MAX, RESP_MAX - HDR_MAX);
  ob_lit(&o, "{\"error\":\"");
  ob_bytes(&o, msg, strlen(msg));
  ob_lit(&o, "\"}");
  finish(region, status, (uint32_t)o.n, out->close, out);
}

static int app_init(struct app *a, const char *db_path, const char *secret, int cache_kib) {
  memset(a, 0, sizeof *a);
  hmac_key_init(&a->key, (const uint8_t *)secret, strlen(secret));
  a->started_s = (uint64_t)time(NULL);
  a->now = (int64_t)a->started_s;
  return db_open(&a->db, db_path, cache_kib);
}

/* /posts/<id>[/like]: the id part, which must not contain a slash (then it is some other path). */
static inline int route_id(const uint8_t *p, uint32_t n, const uint8_t **id, uint32_t *idn) {
  if (n < 8 || memcmp(p, "/posts/", 7)) return 0;
  *id = p + 7;
  *idn = n - 7;
  return memchr(*id, '/', *idn) == NULL;
}

struct body_scan {
  struct jv body;
};
static int body_member(void *ud, const uint8_t *k, uint32_t kn, int kesc, struct jv *v) {
  struct body_scan *s = ud;
  if (kesc) { /* an escaped spelling of "body" still counts (JSON.parse decodes keys) */
    uint8_t tmp[64];
    if (kn > sizeof tmp) return 0;
    uint32_t m = json_unescape(k, kn, tmp);
    if (m == 4 && !memcmp(tmp, "body", 4)) s->body = *v;
    return 0;
  }
  if (kn == 4 && !memcmp(k, "body", 4)) s->body = *v; /* the last duplicate wins */
  return 0;
}

static uint8_t body_buf[HTTP_MAX_BODY];
static uint8_t name_buf[4096];

/* Sets out->p/n and the flags. writes_ok = 0: writes are held (checkpoint tail); they come back later. */
static void app_handle(struct app *a, const struct req *r, uint8_t *region, int writes_ok, struct resp *out) {
  out->db = 0;
  out->close = r->close;
  out->held = 0;
  const uint8_t *p = r->path;
  uint32_t n = r->path_len;
  struct ob o;
  ob_init(&o, region + HDR_MAX, RESP_MAX - HDR_MAX);

  if (r->method == M_GET) {
    if (n == 5 && !memcmp(p, "/feed", 5)) {
      out->db = 1;
      if (db_feed(&a->db, &o) || o.err) { a->n_err++; error_resp(region, 500, "internal server error", out); return; }
      finish(region, 200, (uint32_t)o.n, out->close, out);
      return;
    }
    const uint8_t *id;
    uint32_t idn;
    if (route_id(p, n, &id, &idn)) {
      int64_t pid;
      if (!parse_positive(id, idn, &pid)) { error_resp(region, 400, "invalid post id", out); return; }
      out->db = 1;
      int rc = db_post(&a->db, &o, pid);
      if (rc == 1 && !o.err) { finish(region, 200, (uint32_t)o.n, out->close, out); return; }
      if (rc == 0) { error_resp(region, 404, "post not found", out); return; }
      a->n_err++;
      error_resp(region, 500, "internal server error", out);
      return;
    }
    if (n == 7 && !memcmp(p, "/health", 7)) {
      out->db = 1;
      if (db_health(&a->db) == 0) {
        ob_lit(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        ob_u64(&o, (uint64_t)time(NULL) - a->started_s);
        ob_char(&o, '}');
        finish(region, 200, (uint32_t)o.n, out->close, out);
      } else {
        ob_lit(&o, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
        const char *e = sqlite3_errmsg(a->db.c);
        ob_json_str(&o, (const uint8_t *)e, strlen(e));
        ob_char(&o, '}');
        finish(region, 503, (uint32_t)o.n, out->close, out);
      }
      return;
    }
    error_resp(region, 404, "not found", out);
    return;
  }

  if (r->method != M_POST) { error_resp(region, 404, "not found", out); return; }

  if (n == 6 && !memcmp(p, "/posts", 6)) {
    if (!writes_ok) { out->held = 1; return; }
    struct claims c;
    int rc = jwt_verify(&a->key, r->auth, r->auth_len, a->now, &c, name_buf, sizeof name_buf);
    if (rc == JWT_MISSING) { error_resp(region, 401, "missing bearer token", out); return; }
    if (rc == JWT_INVALID) { error_resp(region, 401, "invalid or expired token", out); return; }
    if (rc != JWT_OK) { error_resp(region, 401, "invalid token payload", out); return; }
    struct body_scan bs;
    memset(&bs, 0, sizeof bs);
    if (json_parse_object(r->body, r->body_len, body_member, &bs)) { error_resp(region, 400, "malformed JSON body", out); return; }
    if (bs.body.type != JV_STRING) { error_resp(region, 400, "body is required", out); return; }
    const uint8_t *text = bs.body.p;
    uint32_t text_n = bs.body.n;
    if (bs.body.escaped) {
      text_n = json_unescape(text, text_n, body_buf);
      text = body_buf;
    }
    js_trim(&text, &text_n);
    if (text_n == 0) { error_resp(region, 400, "body is required", out); return; }
    if (text_n > MAX_BODY_CHARS && utf8_count(text, text_n) > MAX_BODY_CHARS) {
      error_resp(region, 400, "body must be at most 500 characters", out);
      return;
    }
    out->db = 1;
    sqlite3_int64 id;
    uint8_t created[64];
    uint32_t created_n;
    if (db_insert_post(&a->db, c.user_id, text, text_n, &id, created, &created_n)) {
      a->n_err++;
      error_resp(region, 500, "internal server error", out);
      return;
    }
    ob_lit(&o, "{\"post\":");
    ob_post_obj(&o, id, text, text_n, created, created_n, c.username, c.username_len, 0);
    ob_char(&o, '}');
    if (o.err) { a->n_err++; error_resp(region, 500, "internal server error", out); return; }
    finish(region, 201, (uint32_t)o.n, out->close, out);
    return;
  }

  const uint8_t *id;
  uint32_t idn;
  if (n > 12 && !memcmp(p + n - 5, "/like", 5) && route_id(p, n - 5, &id, &idn)) {
    if (!writes_ok) { out->held = 1; return; }
    struct claims c;
    int rc = jwt_verify(&a->key, r->auth, r->auth_len, a->now, &c, name_buf, sizeof name_buf);
    if (rc == JWT_MISSING) { error_resp(region, 401, "missing bearer token", out); return; }
    if (rc == JWT_INVALID) { error_resp(region, 401, "invalid or expired token", out); return; }
    if (rc != JWT_OK) { error_resp(region, 401, "invalid token payload", out); return; }
    int64_t pid;
    if (!parse_positive(id, idn, &pid)) { error_resp(region, 400, "invalid post id", out); return; }
    out->db = 1;
    rc = db_post_exists(&a->db, pid);
    if (rc == 0) { error_resp(region, 404, "post not found", out); return; }
    if (rc < 0) { a->n_err++; error_resp(region, 500, "internal server error", out); return; }
    rc = db_insert_like(&a->db, c.user_id, pid);
    if (rc < 0) { a->n_err++; error_resp(region, 500, "internal server error", out); return; }
    if (rc == 1) ob_lit(&o, "{\"liked\":true,\"already_liked\":false,\"post_id\":");
    else ob_lit(&o, "{\"liked\":true,\"already_liked\":true,\"post_id\":");
    ob_i64(&o, pid);
    ob_char(&o, '}');
    finish(region, rc == 1 ? 201 : 200, (uint32_t)o.n, out->close, out);
    return;
  }
  error_resp(region, 404, "not found", out);
}

#endif
