/* app.h: the five endpoints. One request in, one complete HTTP response out, written straight
 * into the batch arena. Every error response is a constant prepared at startup. */
#ifndef APP_H
#define APP_H

#include "db.h"
#include "http.h"
#include "jwt.h"

#define HDR_ROOM 128      /* space kept in front of a body for its headers */
#define RESP_MAX (64 * 1024)

struct app {
  struct db db;
  struct hmac_key key;
  int64_t now;      /* seconds, refreshed per batch */
  int64_t start;
  uint64_t n_req, n_feed, n_post, n_create, n_like, n_err;
};

struct resp {
  uint8_t *p;
  unsigned n;
  uint8_t close;  /* close the connection after sending */
  uint8_t db;     /* touched the database: becomes a 500 if the batch rolls back */
  uint8_t held;   /* a write while the gate is closed: not answered, try again later */
};

/* constant responses */
#define STATIC_RESP 256
static uint8_t R_404[STATIC_RESP], R_400_ID[STATIC_RESP], R_404_POST[STATIC_RESP], R_401_MISSING[STATIC_RESP],
    R_401_INVALID[STATIC_RESP], R_401_PAYLOAD[STATIC_RESP], R_400_JSON[STATIC_RESP], R_400_BODY[STATIC_RESP],
    R_400_LONG[STATIC_RESP], R_500[STATIC_RESP], R_400_BAD[STATIC_RESP], R_503[STATIC_RESP];
static unsigned L_404, L_400_ID, L_404_POST, L_401_MISSING, L_401_INVALID, L_401_PAYLOAD, L_400_JSON, L_400_BODY,
    L_400_LONG, L_500, L_400_BAD, L_503;

static unsigned mk_resp(uint8_t *dst, const char *status, const char *body, int close) {
  return (unsigned)snprintf((char *)dst, STATIC_RESP,
                            "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s\r\n%s", status,
                            strlen(body), close ? "Connection: close\r\n" : "", body);
}

static void app_init_responses(void) {
  L_404 = mk_resp(R_404, "404 Not Found", "{\"error\":\"not found\"}", 0);
  L_400_ID = mk_resp(R_400_ID, "400 Bad Request", "{\"error\":\"invalid post id\"}", 0);
  L_404_POST = mk_resp(R_404_POST, "404 Not Found", "{\"error\":\"post not found\"}", 0);
  L_401_MISSING = mk_resp(R_401_MISSING, "401 Unauthorized", "{\"error\":\"missing bearer token\"}", 0);
  L_401_INVALID = mk_resp(R_401_INVALID, "401 Unauthorized", "{\"error\":\"invalid or expired token\"}", 0);
  L_401_PAYLOAD = mk_resp(R_401_PAYLOAD, "401 Unauthorized", "{\"error\":\"invalid token payload\"}", 0);
  L_400_JSON = mk_resp(R_400_JSON, "400 Bad Request", "{\"error\":\"malformed JSON body\"}", 0);
  L_400_BODY = mk_resp(R_400_BODY, "400 Bad Request", "{\"error\":\"body is required\"}", 0);
  L_400_LONG = mk_resp(R_400_LONG, "400 Bad Request", "{\"error\":\"body must be at most 500 characters\"}", 0);
  L_500 = mk_resp(R_500, "500 Internal Server Error", "{\"error\":\"internal server error\"}", 0);
  L_400_BAD = mk_resp(R_400_BAD, "400 Bad Request", "{\"error\":\"bad request\"}", 1);
  L_503 = mk_resp(R_503, "503 Service Unavailable", "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"select failed\"}", 0);
}

static int app_init(struct app *a, const char *db_path, const char *secret, long long mmap_bytes, int cache_kib) {
  memset(a, 0, sizeof *a);
  app_init_responses();
  hmac_key_init(&a->key, (const uint8_t *)secret, strlen(secret));
  a->start = a->now = (int64_t)time(NULL);
  return db_open(&a->db, db_path, mmap_bytes, cache_kib);
}

static inline void resp_static(struct resp *out, uint8_t *free, const uint8_t *r, unsigned n) {
  memcpy(free, r, n);
  out->p = free;
  out->n = n;
}

/* Puts the status line and headers in front of a body that was written at free+HDR_ROOM. */
static void resp_frame(struct resp *out, uint8_t *free, const char *status, unsigned status_len, unsigned body_n,
                       int close) {
  char hdr[HDR_ROOM];
  unsigned h = 0;
  memcpy(hdr + h, "HTTP/1.1 ", 9); h += 9;
  memcpy(hdr + h, status, status_len); h += status_len;
  memcpy(hdr + h, "\r\nContent-Type: application/json\r\nContent-Length: ", 50); h += 50;
  h += u64_to_str(hdr + h, body_n);
  if (close) { memcpy(hdr + h, "\r\nConnection: close", 19); h += 19; }
  memcpy(hdr + h, "\r\n\r\n", 4); h += 4;
  out->p = free + HDR_ROOM - h;
  memcpy(out->p, hdr, h);
  out->n = h + body_n;
}

static inline int path_is(const struct req *r, const char *lit, unsigned n) {
  return r->path_len == n && memcmp(r->path, lit, n) == 0;
}

/* Authenticates; on failure writes the 401 and returns 0. */
static int app_auth(struct app *a, const struct req *r, uint8_t *free, struct resp *out, struct jwt_claims *c) {
  if (!r->auth || r->auth_len < 7 || memcmp(r->auth, "Bearer ", 7) != 0) {
    resp_static(out, free, R_401_MISSING, L_401_MISSING);
    return 0;
  }
  int rc = jwt_verify(&a->key, r->auth + 7, r->auth_len - 7, a->now, c);
  if (rc == JWT_OK) return 1;
  if (rc == JWT_INVALID) resp_static(out, free, R_401_INVALID, L_401_INVALID);
  else resp_static(out, free, R_401_PAYLOAD, L_401_PAYLOAD);
  return 0;
}

static inline int is_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* `free` points at RESP_MAX bytes of arena. `writes_ok` is the WAL gate. */
static void app_handle(struct app *a, const struct req *r, uint8_t *free, int writes_ok, struct resp *out) {
  out->close = !r->keep_alive;
  out->db = 0;
  out->held = 0;
  a->n_req++;
  uint8_t *body = free + HDR_ROOM;
  struct ob o;
  ob_init(&o, body, RESP_MAX - HDR_ROOM);

  if (r->method == M_GET) {
    if (path_is(r, "/feed", 5)) {
      a->n_feed++;
      out->db = 1;
      if (db_feed(&a->db, &o) < 0 || o.err) goto err500;
      resp_frame(out, free, "200 OK", 6, (unsigned)o.n, out->close);
      return;
    }
    if (r->path_len > 7 && memcmp(r->path, "/posts/", 7) == 0) {
      a->n_post++;
      uint64_t id;
      if (!parse_id(r->path + 7, r->path_len - 7, &id)) { resp_static(out, free, R_400_ID, L_400_ID); return; }
      out->db = 1;
      int rc = db_post(&a->db, &o, (int64_t)id);
      if (rc < 0 || o.err) goto err500;
      if (rc == 0) { resp_static(out, free, R_404_POST, L_404_POST); return; }
      resp_frame(out, free, "200 OK", 6, (unsigned)o.n, out->close);
      return;
    }
    if (path_is(r, "/health", 7)) {
      if (db_health(&a->db) < 0) { resp_static(out, free, R_503, L_503); return; }
      ob_lit(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
      ob_u64(&o, (uint64_t)(a->now - a->start));
      ob_char(&o, '}');
      resp_frame(out, free, "200 OK", 6, (unsigned)o.n, out->close);
      return;
    }
    resp_static(out, free, R_404, L_404);
    return;
  }

  if (r->method == M_POST) {
    int is_create = path_is(r, "/posts", 6);
    int is_like = r->path_len > 12 && memcmp(r->path, "/posts/", 7) == 0 &&
                  memcmp(r->path + r->path_len - 5, "/like", 5) == 0;
    if (!is_create && !is_like) { resp_static(out, free, R_404, L_404); return; }
    if (!writes_ok) { out->held = 1; a->n_req--; return; }
    struct jwt_claims c;
    if (!app_auth(a, r, free, out, &c)) return;

    if (is_like) {
      a->n_like++;
      uint64_t id;
      if (!parse_id(r->path + 7, r->path_len - 12, &id)) { resp_static(out, free, R_400_ID, L_400_ID); return; }
      out->db = 1;
      int rc = db_like(&a->db, (int64_t)c.sub, (int64_t)id);
      if (rc < 0) goto err500;
      if (rc == 0) { resp_static(out, free, R_404_POST, L_404_POST); return; }
      ob_lit(&o, "{\"liked\":true,\"already_liked\":");
      if (rc == 2) ob_lit(&o, "true,\"post_id\":"); else ob_lit(&o, "false,\"post_id\":");
      ob_u64(&o, id);
      ob_char(&o, '}');
      if (rc == 1) resp_frame(out, free, "201 Created", 11, (unsigned)o.n, out->close);
      else resp_frame(out, free, "200 OK", 6, (unsigned)o.n, out->close);
      return;
    }

    /* create */
    a->n_create++;
    struct jwant w[1] = {{"body", 4, {0}}};
    int type = json_doc((const char *)r->body, r->body_len, w, 1);
    if (type < 0) { resp_static(out, free, R_400_JSON, L_400_JSON); return; }
    if (type != J_OBJECT || w[0].v.type != J_STRING) { resp_static(out, free, R_400_BODY, L_400_BODY); return; }
    uint8_t text[4096];
    long tn = json_unescape(w[0].v.p, w[0].v.n, text, sizeof text);
    if (tn < 0) { resp_static(out, free, R_400_LONG, L_400_LONG); return; }
    uint8_t *t = text, *te = text + tn;
    while (t < te && is_ws(*t)) t++;
    while (te > t && is_ws(te[-1])) te--;
    if (te == t) { resp_static(out, free, R_400_BODY, L_400_BODY); return; }
    unsigned chars = 0;
    for (uint8_t *q = t; q < te; q++) chars += (*q & 0xC0) != 0x80;
    if (chars > 500) { resp_static(out, free, R_400_LONG, L_400_LONG); return; }
    out->db = 1;
    if (db_create(&a->db, &o, (int64_t)c.sub, t, (unsigned)(te - t), c.username, c.username_len) < 0 || o.err) goto err500;
    resp_frame(out, free, "201 Created", 11, (unsigned)o.n, out->close);
    return;
  }

  resp_static(out, free, R_404, L_404);
  return;

err500:
  a->n_err++;
  resp_static(out, free, R_500, L_500);
}

#endif
