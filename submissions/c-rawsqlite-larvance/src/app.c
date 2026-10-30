#define _GNU_SOURCE
#include "rt.h"
#include "raw_sqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const u8 HDR_200[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
static const u8 HDR_201[] = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ";

static const u8 E_NOT_FOUND[]   = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}";
static const u8 E_BAD_ID[]      = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"invalid post id\"}";
static const u8 E_NO_POST[]     = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 26\r\n\r\n{\"error\":\"post not found\"}";
static const u8 E_NO_BEARER[]   = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"error\":\"missing bearer token\"}";
static const u8 E_BAD_TOKEN[]   = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"error\":\"invalid or expired token\"}";
static const u8 E_BAD_PAYLOAD[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"invalid token payload\"}";
static const u8 E_BAD_JSON[]    = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\n\r\n{\"error\":\"malformed JSON body\"}";
static const u8 E_NO_BODY[]     = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 28\r\n\r\n{\"error\":\"body is required\"}";
static const u8 E_LONG_BODY[]   = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 47\r\n\r\n{\"error\":\"body must be at most 500 characters\"}";
static const u8 E_INTERNAL[]    = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";

#define ERR(name) obuf_static(E_##name, sizeof E_##name - 1)

enum { K_NONE = 0, K_HEALTH, K_FEED, K_POST, K_LIKE, K_CREATE };

static i64 started_at = 0;

static int parse_id(const u8 *s, u32 n, i64 *out) {
  if (!n) return 0;
  i64 v = 0;
  for (u32 i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    if (v > 99999999999999999LL) return -1;
    v = v * 10 + (s[i] - '0');
  }
  if (v <= 0) return 0;
  *out = v;
  return 1;
}

static u8 arena[1 << 22];
static u32 arena_used = 0;
static u8 *arena_get(u32 n) {
  if (UNLIKELY(arena_used + n > sizeof arena)) return 0;
  u8 *p = arena + arena_used;
  arena_used += (n + 7) & ~7u;
  return p;
}

static int authenticate(const req_t *rq, job_t *j) {
  if (!rq->has_auth || rq->auth_len < 7 || memcmp(rq->auth, "Bearer ", 7)) {
    j->resp = ERR(NO_BEARER); return 0;
  }
  claims_t c;
  int rc = jwt_verify(rq->auth + 7, rq->auth_len - 7, now_s, &c);
  if (rc == JWT_INVALID) { j->resp = ERR(BAD_TOKEN); return 0; }
  if (rc == JWT_PAYLOAD) { j->resp = ERR(BAD_PAYLOAD); return 0; }
  u8 *name = arena_get(c.name_len + 1);
  if (!name) { j->resp = ERR(INTERNAL); return 0; }
  memcpy(name, c.name, c.name_len);
  j->uid = c.uid;
  j->name = name;
  j->name_len = c.name_len;
  return 1;
}

static int js_space(u32 cp) {
  return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

static u32 cp_at(const u8 *p, u32 *len) {
  u8 c = p[0];
  if (c < 0x80) { *len = 1; return c; }
  if (c < 0xE0) { *len = 2; return (u32)(c & 0x1F) << 6 | (p[1] & 0x3F); }
  if (c < 0xF0) { *len = 3; return (u32)(c & 0x0F) << 12 | (u32)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); }
  *len = 4;
  return (u32)(c & 0x07) << 18 | (u32)(p[1] & 0x3F) << 12 | (u32)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
}

static int decode_create(const req_t *rq, job_t *j) {
  jmember m[1] = {{"body", 4, 0, 0, 0, 0}};
  int rc = json_members(rq->body, rq->body_len, m, 1);
  if (rc < 0) { j->resp = ERR(BAD_JSON); return 0; }
  if (rc > 0 || m[0].type != JT_STRING) { j->resp = ERR(NO_BODY); return 0; }
  u8 *t = arena_get(m[0].v_len + 1);
  if (!t) { j->resp = ERR(INTERNAL); return 0; }
  u32 n = m[0].escaped ? json_unescape(m[0].v, m[0].v_len, t) : (memcpy(t, m[0].v, m[0].v_len), m[0].v_len);
  u32 a = 0, b = n, l;
  while (a < b && js_space(cp_at(t + a, &l))) a += l;
  while (b > a) {
    u32 k = b - 1;
    while (k > a && (t[k] & 0xC0) == 0x80) k--;
    if (!js_space(cp_at(t + k, &l))) break;
    b = k;
  }
  if (a == b) { j->resp = ERR(NO_BODY); return 0; }
  u32 chars = 0;
  for (u32 i = a; i < b; i++) chars += (t[i] & 0xC0) != 0x80;
  if (chars > 500) { j->resp = ERR(LONG_BODY); return 0; }
  j->text = t + a;
  j->text_len = b - a;
  return 1;
}

static void app_decode(const req_t *rq, job_t *j) {
  const u8 *p = rq->path;
  u32 n = rq->path_len;
  const u8 *e = p + n;
  if (rq->method == M_GET) {
    if (n == 5 && !memcmp(p, "/feed", 5)) {
      j->kind = K_FEED; j->is_write = 0; return;
    }
    if (n == 7 && !memcmp(p, "/health", 7)) {
      j->kind = K_HEALTH; j->is_write = 0; return;
    }
    const u8 *s = p, *id_p = 0; u32 id_n = 0;
    if (path_lit(&s, e, "/posts", 6) && path_param(&s, e, &id_p, &id_n) && s == e) {
      int ok = parse_id(id_p, id_n, &j->id);
      if (ok == 0) { j->resp = ERR(BAD_ID); return; }
      if (ok < 0) { j->resp = ERR(NO_POST); return; }
      j->kind = K_POST; j->is_write = 0; return;
    }
  } else if (rq->method == M_POST) {
    if (n == 6 && !memcmp(p, "/posts", 6)) {
      if (!authenticate(rq, j)) return;
      if (!decode_create(rq, j)) return;
      j->kind = K_CREATE; j->is_write = 1; return;
    }
    const u8 *s = p, *id_p = 0; u32 id_n = 0;
    if (path_lit(&s, e, "/posts", 6) && path_param(&s, e, &id_p, &id_n) && path_lit(&s, e, "/like", 5) && s == e) {
      if (!authenticate(rq, j)) return;
      int ok = parse_id(id_p, id_n, &j->id);
      if (ok == 0) { j->resp = ERR(BAD_ID); return; }
      if (ok < 0) { j->resp = ERR(NO_POST); return; }
      j->kind = K_LIKE; j->is_write = 1; return;
    }
  }
  j->resp = ERR(NOT_FOUND);
}

static inline void put_post(out_t *o, i64 id, const u8 *body, u32 body_len,
                           const u8 *created, u32 created_len,
                           const u8 *author, u32 author_len, i64 likes) {
  out_need(o, 128 + JSTR_ROOM(body_len) + created_len + JSTR_ROOM(author_len));
  out_lit(o, "{\"id\":", 6);
  out_i64(o, id);
  out_lit(o, ",\"body\":", 8);
  out_jstr(o, body, body_len);
  out_lit(o, ",\"created_at\":", 14);
  out_raw_jstr(o, created, created_len);
  out_lit(o, ",\"author\":", 10);
  out_jstr(o, author, author_len);
  out_lit(o, ",\"like_count\":", 14);
  out_i64(o, likes);
  out_lit(o, "}", 1);
}

static void exec_read(job_t *j) {
  out_t o0, *o = &o0;
  out_begin(o);
  switch (j->kind) {
    case K_HEALTH: {
      i64 uptime = now_s - started_at;
      out_need(o, 64);
      out_lit(o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36);
      out_i64(o, uptime);
      out_lit(o, "}", 1);
      j->resp = out_finish(o, HDR_200, sizeof HDR_200 - 1);
      return;
    }
    case K_FEED: {
      out_need(o, 10);
      out_lit(o, "{\"posts\":[", 10);
      raw_table_cursor_t cur;
      if (LIKELY(raw_last_table(&cur, ROOT_POSTS) == 0)) {
        i64 hi = cur.rowid;
        i64 lo = hi > 20 ? hi - 19 : 1;
        i64 counts[20];
        raw_range_counts(lo, hi, counts);
        for (i64 k = 0; k <= hi - lo; k++) {
          i64 uid, id = cur.rowid;
          const u8 *body, *created, *author;
          u32 body_len, created_len, author_len;
          parse_post_cols(cur.payload, &uid, &body, &body_len, &created, &created_len);
          raw_get_username(uid, &author, &author_len);
          if (k) { out_need(o, 1); out_lit(o, ",", 1); }
          put_post(o, id, body, body_len, created, created_len, author, author_len, counts[id - lo]);
          if (k < hi - lo && raw_prev_table(&cur) != 0) break;
        }
      }
      out_need(o, 2);
      out_lit(o, "]}", 2);
      j->resp = out_finish(o, HDR_200, sizeof HDR_200 - 1);
      return;
    }
    case K_POST: {
      raw_table_cursor_t cur;
      if (raw_seek_table(&cur, ROOT_POSTS, j->id) != 0) {
        out_abort(o);
        j->resp = ERR(NO_POST);
        return;
      }
      i64 uid, likes = 0;
      const u8 *body, *created, *author;
      u32 body_len, created_len, author_len;
      parse_post_cols(cur.payload, &uid, &body, &body_len, &created, &created_len);
      raw_get_username(uid, &author, &author_len);
      raw_range_counts(j->id, j->id, &likes);
      out_need(o, 8);
      out_lit(o, "{\"post\":", 8);
      put_post(o, j->id, body, body_len, created, created_len, author, author_len, likes);
      out_need(o, 1);
      out_lit(o, "}", 1);
      j->resp = out_finish(o, HDR_200, sizeof HDR_200 - 1);
      return;
    }
  }
}

static time_t cached_sec = 0;
static char cached_time_prefix[21];

static inline void format_iso8601(char out[32], int *out_len) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  if (UNLIKELY(ts.tv_sec != cached_sec)) {
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(cached_time_prefix, sizeof cached_time_prefix, "%04d-%02d-%02dT%02d:%02d:%02d.",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    cached_sec = ts.tv_sec;
  }
  memcpy(out, cached_time_prefix, 20);
  int ms = (int)(ts.tv_nsec / 1000000);
  if (ms < 0) ms = 0;
  if (ms > 999) ms = 999;
  out[20] = (char)('0' + (ms / 100));
  out[21] = (char)('0' + ((ms / 10) % 10));
  out[22] = (char)('0' + (ms % 10));
  out[23] = 'Z';
  out[24] = '\0';
  *out_len = 24;
}

static void exec_write(job_t *j) {
  out_t o0, *o = &o0;
  switch (j->kind) {
    case K_LIKE: {
      if (raw_post_exists(j->id) != 0) {
        j->resp = ERR(NO_POST);
        return;
      }
      char created_str[32];
      int clen = 0;
      format_iso8601(created_str, &clen);
      bool already_liked = false;
      raw_insert_like(j->uid, j->id, (const u8 *)created_str, (u32)clen, &already_liked);
      out_begin(o);
      out_need(o, 67);
      out_lit(o, "{\"liked\":true,\"already_liked\":", 30);
      if (already_liked) out_lit(o, "true", 4); else out_lit(o, "false", 5);
      out_lit(o, ",\"post_id\":", 11);
      out_i64(o, j->id);
      out_lit(o, "}", 1);
      j->resp = already_liked ? out_finish(o, HDR_200, sizeof HDR_200 - 1) : out_finish(o, HDR_201, sizeof HDR_201 - 1);
      return;
    }
    case K_CREATE: {
      char created_str[32];
      int clen = 0;
      format_iso8601(created_str, &clen);
      raw_post_view_t pv;
      raw_insert_post(j->uid, j->text, j->text_len, (const u8 *)created_str, (u32)clen,
                      j->name, j->name_len, &pv);
      out_begin(o);
      out_need(o, 8);
      out_lit(o, "{\"post\":", 8);
      put_post(o, pv.id, pv.body, pv.body_len, pv.created_at, pv.created_at_len,
               pv.author, pv.author_len, 0);
      out_need(o, 1);
      out_lit(o, "}", 1);
      j->resp = out_finish(o, HDR_201, sizeof HDR_201 - 1);
      return;
    }
  }
}

static void app_batch(job_t *js, int n) {
  int reads = 0, writes = 0;
  for (int i = 0; i < n; i++) {
    if (js[i].kind) {
      if (js[i].is_write) writes++; else reads++;
    }
  }
  if (reads || writes) {
    raw_sqlite_batch_begin();
    for (int i = 0; i < n; i++) {
      if (js[i].kind && !js[i].is_write) exec_read(&js[i]);
    }
    for (int i = 0; i < n; i++) {
      if (js[i].kind && js[i].is_write) exec_write(&js[i]);
    }
    raw_sqlite_batch_end();
  }
  arena_used = 0;
}

static void app_init(void) {
  started_at = time(0);
}
