#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Error bodies are the strings test/test.sh compares, with Content-Length and no extra newline. */
static const char E_NOT_FOUND[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}";
static const char E_BAD_ID[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"invalid post id\"}";
static const char E_NO_POST[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 26\r\n\r\n{\"error\":\"post not found\"}";
static const char E_NO_BEARER[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"error\":\"missing bearer token\"}";
static const char E_BAD_TOKEN[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"error\":\"invalid or expired token\"}";
static const char E_BAD_PAYLOAD[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"invalid token payload\"}";
static const char E_BAD_JSON[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\n\r\n{\"error\":\"malformed JSON body\"}";
static const char E_NO_BODY[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 28\r\n\r\n{\"error\":\"body is required\"}";
static const char E_LONG_BODY[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 47\r\n\r\n{\"error\":\"body must be at most 500 characters\"}";
static const char E_INTERNAL[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";

#define ERR(name) obuf_static((const uint8_t *)E_##name, (uint32_t)(sizeof E_##name - 1))

static int64_t started_at;

/* Bytes copied out of the receive buffer. Reset after the batch has built its responses. */
static uint8_t arena[1 << 22];
static uint32_t arena_used;

static uint8_t *arena_get(uint32_t n) {
  if (arena_used + n > sizeof arena) return 0;
  uint8_t *p = arena + arena_used;
  arena_used += (n + 7u) & ~7u;
  return p;
}

void app_init(void) { started_at = (int64_t)time(0); }

/* 1 = positive integer, 0 = invalid id, -1 = too big to be a rowid (respond 404). */
static int parse_id(const uint8_t *s, uint32_t n, int64_t *out) {
  if (!n) return 0;
  int64_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    if (v > 99999999999999999LL) return -1;
    v = v * 10 + (s[i] - '0');
  }
  if (v <= 0) return 0;
  *out = v;
  return 1;
}

static int authenticate(const req_t *rq, job_t *j) {
  if (!rq->has_auth || rq->auth_len < 7 || memcmp(rq->auth, "Bearer ", 7) != 0) {
    j->resp = ERR(NO_BEARER);
    return 0;
  }
  claims_t c;
  int rc = jwt_verify(rq->auth + 7, rq->auth_len - 7, now_s, &c);
  if (rc == JWT_INVALID) { j->resp = ERR(BAD_TOKEN); return 0; }
  if (rc != JWT_OK) { j->resp = ERR(BAD_PAYLOAD); return 0; }
  uint8_t *name = arena_get(c.name_len);
  if (!name) { j->resp = ERR(INTERNAL); return 0; }
  memcpy(name, c.name, c.name_len);
  j->uid = c.uid;
  j->name = name;
  j->name_len = c.name_len;
  return 1;
}

/* String.prototype.trim whitespace, including the Unicode space separators. */
static int js_space(uint32_t cp) {
  return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}
static uint32_t cp_at(const uint8_t *p, uint32_t *len) {
  uint8_t c = p[0];
  if (c < 0x80) { *len = 1; return c; }
  if (c < 0xE0) { *len = 2; return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F); }
  if (c < 0xF0) { *len = 3; return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
  *len = 4;
  return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
         ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

static int decode_create(const req_t *rq, job_t *j) {
  jmember m[1] = {{"body", 4, 0, 0, 0, 0}};
  int rc = json_members(rq->body, rq->body_len, m, 1);
  if (rc < 0) { j->resp = ERR(BAD_JSON); return 0; }
  if (rc > 0 || m[0].type != JT_STRING) { j->resp = ERR(NO_BODY); return 0; }
  uint8_t *t = arena_get(m[0].v_len + 1);
  if (!t) { j->resp = ERR(INTERNAL); return 0; }
  uint32_t n = m[0].escaped ? json_unescape(m[0].v, m[0].v_len, t) : (memcpy(t, m[0].v, m[0].v_len), m[0].v_len);
  uint32_t a = 0, b = n, l = 1;
  while (a < b && js_space(cp_at(t + a, &l))) a += l;
  while (b > a) {
    uint32_t k = b - 1;
    while (k > a && (t[k] & 0xC0) == 0x80) k--;
    if (!js_space(cp_at(t + k, &l))) break;
    b = k;
  }
  if (a == b) { j->resp = ERR(NO_BODY); return 0; }
  uint32_t chars = 0;
  for (uint32_t i = a; i < b; i++) chars += (t[i] & 0xC0) != 0x80;
  if (chars > 500) { j->resp = ERR(LONG_BODY); return 0; }
  j->text = t + a;
  j->text_len = b - a;
  return 1;
}

void app_decode(const req_t *rq, job_t *j) {
  const uint8_t *p = rq->path;
  uint32_t n = rq->path_len;
  if (rq->method == M_GET) {
    const uint8_t *s = p, *e = p + n;
    if (path_lit(&s, e, "/health", 7) && s == e) { j->kind = K_HEALTH; return; }
    s = p;
    if (path_lit(&s, e, "/feed", 5) && s == e) { j->kind = K_FEED; return; }
    s = p;
    const uint8_t *id_p = 0;
    uint32_t id_n = 0;
    if (path_lit(&s, e, "/posts", 6) && path_param(&s, e, &id_p, &id_n) && s == e) {
      int ok = parse_id(id_p, id_n, &j->id);
      if (ok == 0) { j->resp = ERR(BAD_ID); return; }
      if (ok < 0) { j->resp = ERR(NO_POST); return; }
      j->kind = K_POST;
      return;
    }
  } else if (rq->method == M_POST) {
    const uint8_t *s = p, *e = p + n;
    if (path_lit(&s, e, "/posts", 6) && s == e) {
      if (!authenticate(rq, j)) return;
      if (!decode_create(rq, j)) return;
      j->kind = K_CREATE;
      j->is_write = 1;
      return;
    }
    s = p;
    const uint8_t *id_p = 0;
    uint32_t id_n = 0;
    /* Auth before the id check: POST /posts/abc/like with no token is 401, not 400. */
    if (path_lit(&s, e, "/posts", 6) && path_param(&s, e, &id_p, &id_n) && path_lit(&s, e, "/like", 5) && s == e) {
      if (!authenticate(rq, j)) return;
      int ok = parse_id(id_p, id_n, &j->id);
      if (ok == 0) { j->resp = ERR(BAD_ID); return; }
      if (ok < 0) { j->resp = ERR(NO_POST); return; }
      j->kind = K_LIKE;
      j->is_write = 1;
      return;
    }
  }
  j->resp = ERR(NOT_FOUND);
}

static void fail_kind(job_t *js, int n) {
  for (int i = 0; i < n; i++) {
    if (!js[i].kind) continue;
    if (js[i].resp) obuf_release(js[i].resp);
    js[i].resp = ERR(INTERNAL);
    js[i].kind = K_NONE;
    js[i].is_write = 0;
  }
}

static void exec_read(job_t *j) {
  out_t o;
  if (j->kind == K_HEALTH) {
    out_begin(&o);
    if (db_health()) {
      int64_t up = now_s - started_at;
      if (up < 0) up = 0;
      out_lit(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36);
      out_i64(&o, up);
      out_lit(&o, "}", 1);
      j->resp = out_finish(&o, "200 OK", j->close_after);
    } else {
      out_lit(&o, "{\"status\":\"degraded\",\"db\":\"unreachable\"}", 40);
      j->resp = out_finish(&o, "503 Service Unavailable", j->close_after);
    }
    return;
  }
  if (j->kind == K_FEED) {
    if (db_write_feed(&o)) j->resp = ERR(INTERNAL);
    else j->resp = out_finish(&o, "200 OK", j->close_after);
    return;
  }
  if (j->kind == K_POST) {
    int rc = db_write_post(&o, j->id);
    if (rc == 0) j->resp = out_finish(&o, "200 OK", j->close_after);
    else j->resp = rc > 0 ? ERR(NO_POST) : ERR(INTERNAL);
  }
}

static void exec_create(job_t *j) {
  uint8_t created[40];
  uint32_t clen = 0;
  int64_t id = 0;
  if (db_insert_post(j->uid, j->text, j->text_len, &id, created, &clen)) {
    j->resp = ERR(INTERNAL);
    return;
  }
  out_t o;
  out_begin(&o);
  out_lit(&o, "{\"post\":", 8);
  put_post(&o, id, j->text, j->text_len, created, clen, j->name, j->name_len, 0);
  out_lit(&o, "}", 1);
  j->resp = out_finish(&o, "201 Created", j->close_after);
}

static void exec_like(job_t *j) {
  int ex = db_post_exists(j->id);
  if (ex) { j->resp = ex > 0 ? ERR(NO_POST) : ERR(INTERNAL); return; }
  int already = 0;
  if (db_insert_like(j->uid, j->id, &already)) { j->resp = ERR(INTERNAL); return; }
  out_t o;
  out_begin(&o);
  out_lit(&o, "{\"liked\":true,\"already_liked\":", 30);
  out_lit(&o, already ? "true" : "false", already ? 4u : 5u);
  out_lit(&o, ",\"post_id\":", 11);
  out_i64(&o, j->id);
  out_lit(&o, "}", 1);
  j->resp = out_finish(&o, already ? "200 OK" : "201 Created", j->close_after);
}

/* One SQLite transaction per tick. Responses are built here and sent only after commit. */
void app_batch(job_t *js, int n) {
  int work = 0;
  for (int i = 0; i < n; i++) if (js[i].kind) work++;
  if (work) {
    if (db_batch_begin()) {
      fail_kind(js, n);
    } else {
      int opened = db_cursors_open() == 0;
      if (!opened) {
        db_cursors_close();
        db_native_off();
      }
      for (int i = 0; i < n; i++) if (js[i].kind && !js[i].is_write) exec_read(&js[i]);
      if (opened) db_cursors_close();
      for (int i = 0; i < n; i++) if (js[i].kind == K_CREATE) exec_create(&js[i]);
      for (int i = 0; i < n; i++) if (js[i].kind == K_LIKE) exec_like(&js[i]);
      if (db_batch_end()) {
        for (int i = 0; i < n; i++) {
          if (!js[i].is_write) continue;
          if (js[i].resp) obuf_release(js[i].resp);
          js[i].resp = ERR(INTERNAL);
        }
      }
    }
  }
  arena_used = 0;
}
