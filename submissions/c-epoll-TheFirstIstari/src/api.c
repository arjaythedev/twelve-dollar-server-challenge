#include "api.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

// ---------- tiny writers (no snprintf on hot path) ----------
typedef struct { char *p; char *e; int ok; } wr_t;
static void wr_raw(wr_t *w, const char *s, size_t n) {
  if ((size_t)(w->e - w->p) < n) { w->ok = 0; return; }
  memcpy(w->p, s, n); w->p += n;
}
static void wr_cstr(wr_t *w, const char *s) { wr_raw(w, s, strlen(s)); }
static void wr_u64(wr_t *w, uint64_t v) {
  char tmp[20]; int n = 0;
  if (v == 0) tmp[n++] = '0';
  else { char rev[20]; int m = 0; while (v) { rev[m++] = (char)('0' + v % 10); v /= 10; }
         while (m) tmp[n++] = rev[--m]; }
  wr_raw(w, tmp, (size_t)n);
}
static void wr_esc(wr_t *w, const char *s, size_t n) {
  // Bodies are schema-capped at 500 chars; usernames/timestamps are tiny.
  // Anything bigger than the stack tmp flags overflow -> caller emits 500.
  char tmp[4096];
  if (6 * n + 1 > sizeof tmp) { w->ok = 0; return; }
  int k = json_escape(s, n, tmp, sizeof tmp);
  if (k < 0) { w->ok = 0; return; }
  wr_raw(w, tmp, (size_t)k);
}

// Serialize one post row (cols 0..3: id, body, created_at, author).
// Statement must be positioned on a valid row; like count passed in.
static void wr_post(wr_t *w, sqlite3_stmt *s, int64_t likes) {
  int64_t id = sqlite3_column_int64(s, 0);
  const char *body = (const char *)sqlite3_column_text(s, 1);
  int body_n = sqlite3_column_bytes(s, 1);
  const char *ts = (const char *)sqlite3_column_text(s, 2);
  int ts_n = sqlite3_column_bytes(s, 2);
  const char *au = (const char *)sqlite3_column_text(s, 3);
  int au_n = sqlite3_column_bytes(s, 3);
  if (!body || !ts || !au) { w->ok = 0; return; }
  wr_cstr(w, "{\"id\":"); wr_u64(w, (uint64_t)id);
  wr_cstr(w, ",\"body\":\""); wr_esc(w, body, (size_t)body_n);
  wr_cstr(w, "\",\"created_at\":\""); wr_raw(w, ts, (size_t)ts_n);
  wr_cstr(w, "\",\"author\":\""); wr_esc(w, au, (size_t)au_n);
  wr_cstr(w, "\",\"like_count\":"); wr_u64(w, (uint64_t)likes);
  wr_cstr(w, "}");
}

static resp_t err(resp_t *r, char *dst, size_t cap, int status, const char *msg) {
  char *p = dst;
  if (cap < 32) { r->status = 500; r->body = ""; r->body_len = 0; return *r; }
  size_t n = strlen(msg);
  // {"error":"<msg>"}: messages are constant ASCII, no escaping needed
  memcpy(p, "{\"error\":\"", 10);
  memcpy(p + 10, msg, n);
  memcpy(p + 10 + n, "\"}", 2);
  r->status = status; r->body = p; r->body_len = 12 + n;
  return *r;
}

// Parse a post id segment: all digits, value > 0. Returns 1 ok.
static int parse_id(const char *s, size_t n, uint64_t *out) {
  if (n == 0 || n > 19) return 0;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    v = v * 10 + (uint64_t)(s[i] - '0');
    if (v > 9007199254740991ULL) return 0; // beyond 2^53: out of scope, reject
  }
  if (v == 0) return 0;
  *out = v;
  return 1;
}

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

resp_t api_handle(api_t *a, const req_t *req) {
  resp_t r;
  char *sc = a->scratch;
  char *rb = sc + API_RESP_OFF;          // all responses built here...
  const size_t rcap = API_RESP_CAP;      // ...never clobbering JWT/body areas
  db_t *d = a->db;
  int is_get = req->mlen == 3 && memcmp(req->method, "GET", 3) == 0;
  int is_post = req->mlen == 4 && memcmp(req->method, "POST", 4) == 0;

  // ---- GET /health ----
  if (is_get && req->plen == 7 && memcmp(req->path, "/health", 7) == 0) {
    int rc = sqlite3_step(d->st_health);
    sqlite3_reset(d->st_health);
    if (rc == SQLITE_ROW) {
      unsigned long long up = a->now_mono >= a->boot_mono ?
          (unsigned long long)(a->now_mono - a->boot_mono) : 0;
      int n = snprintf(rb, rcap,
                       "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%llu}", up);
      r.status = 200; r.body = rb; r.body_len = (size_t)n;
      return r;
    }
    int n = snprintf(rb, rcap,
                     "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"%s\"}",
                     sqlite3_errmsg(d->db));
    r.status = 503; r.body = rb; r.body_len = (size_t)n;
    return r;
  }

  // ---- GET /feed ----
  if (is_get && req->plen == 5 && memcmp(req->path, "/feed", 5) == 0) {
    wr_t w = { rb, rb + rcap, 1 };
    wr_cstr(&w, "{\"posts\":[");
    int first = 1, rc;
    while ((rc = sqlite3_step(d->st_feed)) == SQLITE_ROW) {
      if (!first) wr_cstr(&w, ",");
      first = 0;
      wr_post(&w, d->st_feed, sqlite3_column_int64(d->st_feed, 4));
      if (!w.ok) break;
    }
    sqlite3_reset(d->st_feed);
    if (rc != SQLITE_DONE || !w.ok)
      return err(&r, rb, rcap, 500, "internal server error");
    wr_cstr(&w, "]}");
    if (!w.ok) return err(&r, rb, rcap, 500, "internal server error");
    r.status = 200; r.body = rb; r.body_len = (size_t)(w.p - rb);
    return r;
  }

  // ---- /posts... routes ----
  if (req->plen >= 7 && memcmp(req->path, "/posts/", 7) == 0) {
    const char *seg = req->path + 7;
    size_t seglen = req->plen - 7;
    int want_like = 0;
    if (seglen > 5 && memcmp(seg + seglen - 5, "/like", 5) == 0) {
      want_like = 1;
      seglen -= 5;
    }
    // Auth BEFORE id on the like route (SPEC).
    auth_t auth;
    int have_auth = 0;
    if (want_like || is_post) {
      if (!req->auth || req->auth_len < 7 || memcmp(req->auth, "Bearer ", 7) != 0)
        return err(&r, rb, rcap, 401, "missing bearer token");
      int ar = jwt_verify(req->auth + 7, req->auth_len - 7,
                          a->jwt_secret, a->secret_len, a->now_s,
                          &auth, sc, API_JWT_SCRATCH);
      if (ar == JWT_BAD_PAYLOAD)
        return err(&r, rb, rcap, 401, "invalid token payload");
      if (ar != JWT_OK)
        return err(&r, rb, rcap, 401, "invalid or expired token");
      have_auth = 1;
    }
    // POST /posts (create): path must be exactly "/posts"... handled below.
    // Here: /posts/:id and /posts/:id/like
    uint64_t pid = 0;
    int id_ok = 0;
    if (!(seglen == 0 && !want_like))  // "/posts/" alone falls through to 404 below
      id_ok = parse_id(seg, seglen, &pid);

    if (is_get && !want_like && id_ok) {
      sqlite3_bind_int64(d->st_post, 1, (int64_t)pid);
      int rc = sqlite3_step(d->st_post);
      if (rc == SQLITE_ROW) {
        wr_t w = { rb, rb + rcap, 1 };
        wr_cstr(&w, "{\"post\":");
        wr_post(&w, d->st_post, sqlite3_column_int64(d->st_post, 4));
        wr_cstr(&w, "}");
        sqlite3_reset(d->st_post);
        sqlite3_clear_bindings(d->st_post);
        if (!w.ok) return err(&r, rb, rcap, 500, "internal server error");
        r.status = 200; r.body = rb; r.body_len = (size_t)(w.p - rb);
        return r;
      }
      sqlite3_reset(d->st_post);
      sqlite3_clear_bindings(d->st_post);
      if (rc != SQLITE_DONE)
        return err(&r, rb, rcap, 500, "internal server error");
      if (!id_ok) return err(&r, rb, rcap, 400, "invalid post id");
      return err(&r, rb, rcap, 404, "post not found");
    }
    // NOTE: GET with bad id falls here too (id_ok==0) -> 400 below.
    if (is_get && !want_like && !id_ok)
      return err(&r, rb, rcap, 400, "invalid post id");

    if (want_like && (is_post)) {
      if (!have_auth) return err(&r, rb, rcap, 401, "missing bearer token");
      if (!id_ok) return err(&r, rb, rcap, 400, "invalid post id");
      // post must exist (explicit check -> clean 404, keeps FK as backstop)
      sqlite3_bind_int64(d->st_exists, 1, (int64_t)pid);
      int erc = sqlite3_step(d->st_exists);
      sqlite3_reset(d->st_exists);
      sqlite3_clear_bindings(d->st_exists);
      if (erc == SQLITE_DONE)
        return err(&r, rb, rcap, 404, "post not found");
      if (erc != SQLITE_ROW)
        return err(&r, rb, rcap, 500, "internal server error");
      if (db_begin(d) != 0)
        return err(&r, rb, rcap, 500, "internal server error");
      sqlite3_bind_int64(d->st_like, 1, (int64_t)auth.uid);
      sqlite3_bind_int64(d->st_like, 2, (int64_t)pid);
      int lrc = sqlite3_step(d->st_like);
      int chg = sqlite3_changes(d->db);
      sqlite3_reset(d->st_like);
      sqlite3_clear_bindings(d->st_like);
      if (lrc != SQLITE_DONE) { db_rollback(d); return err(&r, rb, rcap, 500, "internal server error"); }
      int first_like = (chg == 1);
      int n = snprintf(rb, rcap,
                       "{\"liked\":true,\"already_liked\":%s,\"post_id\":%llu}",
                       first_like ? "false" : "true", (unsigned long long)pid);
      r.status = first_like ? 201 : 200;
      r.body = rb; r.body_len = (size_t)n;
      return r;
    }
    // method/path mismatch inside /posts/...
    if (want_like || id_ok || seglen == 0)
      return err(&r, rb, rcap, 404, "not found");
    return err(&r, rb, rcap, 400, "invalid post id");
  }

  // ---- POST /posts (create) ----
  if (is_post && req->plen == 6 && memcmp(req->path, "/posts", 6) == 0) {
    if (!req->auth || req->auth_len < 7 || memcmp(req->auth, "Bearer ", 7) != 0)
      return err(&r, rb, rcap, 401, "missing bearer token");
    auth_t auth;
    int ar = jwt_verify(req->auth + 7, req->auth_len - 7,
                        a->jwt_secret, a->secret_len, a->now_s,
                        &auth, sc, API_JWT_SCRATCH);
    if (ar == JWT_BAD_PAYLOAD)
      return err(&r, rb, rcap, 401, "invalid token payload");
    if (ar != JWT_OK)
      return err(&r, rb, rcap, 401, "invalid or expired token");
    // body validation
    char *bbuf = sc + API_BODY_OFF;
    size_t bcap = API_BODY_CAP;
    size_t blen = 0;
    int jr = json_extract_body(req->body ? req->body : "", req->body_len,
                               bbuf, bcap, &blen);
    if (jr == JSON_MALFORMED)
      return err(&r, rb, rcap, 400, "malformed JSON body");
    if (jr == JSON_NO_BODY)
      return err(&r, rb, rcap, 400, "body is required");
    // trim ASCII whitespace
    size_t s = 0, e = blen;
    while (s < e && is_space(bbuf[s])) s++;
    while (e > s && is_space(bbuf[e - 1])) e--;
    if (e == s) return err(&r, rb, rcap, 400, "body is required");
    size_t chars = 0;
    if (utf8_count_chars(bbuf + s, e - s, &chars) != 0) chars = e - s; // lenient
    if (chars > 500)
      return err(&r, rb, rcap, 400, "body must be at most 500 characters");
    if (db_begin(d) != 0)
      return err(&r, rb, rcap, 500, "internal server error");
    sqlite3_bind_int64(d->st_create, 1, (int64_t)auth.uid);
    /* STATIC: bbuf lives in the request accumulator, which is freed only after
       this handler returns; the step below consumes the text synchronously. */
    sqlite3_bind_text(d->st_create, 2, bbuf + s, (int)(e - s), SQLITE_STATIC);
    int crc = sqlite3_step(d->st_create);
    sqlite3_reset(d->st_create);
    sqlite3_clear_bindings(d->st_create);
    if (crc != SQLITE_DONE) { db_rollback(d); return err(&r, rb, rcap, 500, "internal server error"); }
    int64_t nid = sqlite3_last_insert_rowid(d->db);
    sqlite3_bind_int64(d->st_sel_ts, 1, nid);
    int trc = sqlite3_step(d->st_sel_ts);
    const char *ts = trc == SQLITE_ROW ? (const char *)sqlite3_column_text(d->st_sel_ts, 0) : NULL;
    int ts_n = trc == SQLITE_ROW ? sqlite3_column_bytes(d->st_sel_ts, 0) : 0;
    char tsbuf[32]; int ts_l = 0;
    if (ts && ts_n > 0 && ts_n < (int)sizeof tsbuf) { memcpy(tsbuf, ts, (size_t)ts_n); ts_l = ts_n; }
    sqlite3_reset(d->st_sel_ts);
    sqlite3_clear_bindings(d->st_sel_ts);
    if (!ts_l) { db_rollback(d); return err(&r, rb, rcap, 500, "internal server error"); }
    // NOTE: commit happens at end of event-loop batch (main.c). The response
    // is not sent before that commit (group commit, rule 6).
    wr_t w = { rb, rb + rcap, 1 };
    wr_cstr(&w, "{\"post\":{\"id\":"); wr_u64(&w, (uint64_t)nid);
    wr_cstr(&w, ",\"body\":\""); wr_esc(&w, bbuf + s, e - s);
    wr_cstr(&w, "\",\"created_at\":\""); wr_raw(&w, tsbuf, (size_t)ts_l);
    wr_cstr(&w, "\",\"author\":\""); wr_esc(&w, auth.uname, auth.ulen);
    wr_cstr(&w, "\",\"like_count\":0}}");
    if (!w.ok) { db_rollback(d); return err(&r, rb, rcap, 500, "internal server error"); }
    r.status = 201; r.body = rb; r.body_len = (size_t)(w.p - rb);
    return r;
  }

  return err(&r, rb, rcap, 404, "not found");
}
