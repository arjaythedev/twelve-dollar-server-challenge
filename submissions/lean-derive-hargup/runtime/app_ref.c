#include "rt.h"
/* Hand-written reference for the generated application layer (gen/app_gen.c). It fixes the
 * interface the Lean emitter targets and serves as a differential oracle in tests. */

#include <stdio.h>

/* ---- wire constants --------------------------------------------------------------------- */
#define HDR(status) "HTTP/1.1 " status "\r\nContent-Type: application/json\r\nContent-Length: "
static const u8 HDR_200[] = HDR("200 OK");
static const u8 HDR_201[] = HDR("201 Created");
static const u8 HDR_503[] = HDR("503 Service Unavailable");
#define ERR(status, msg) "HTTP/1.1 " status "\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n{\"error\":\"" msg "\"}"

typedef struct { const u8 *p; u32 n; } bytes;
static u8 err_mem[4096];
static bytes E_NOT_FOUND, E_BAD_ID, E_NO_POST, E_NO_BEARER, E_BAD_TOKEN, E_BAD_PAYLOAD, E_BAD_JSON, E_NO_BODY, E_LONG_BODY, E_INTERNAL;
static void err_init(void) {
  u8 *m = err_mem;
  struct { bytes *b; const char *status; const char *msg; } t[] = {
    {&E_NOT_FOUND, "404 Not Found", "not found"},
    {&E_BAD_ID, "400 Bad Request", "invalid post id"},
    {&E_NO_POST, "404 Not Found", "post not found"},
    {&E_NO_BEARER, "401 Unauthorized", "missing bearer token"},
    {&E_BAD_TOKEN, "401 Unauthorized", "invalid or expired token"},
    {&E_BAD_PAYLOAD, "401 Unauthorized", "invalid token payload"},
    {&E_BAD_JSON, "400 Bad Request", "malformed JSON body"},
    {&E_NO_BODY, "400 Bad Request", "body is required"},
    {&E_LONG_BODY, "400 Bad Request", "body must be at most 500 characters"},
    {&E_INTERNAL, "500 Internal Server Error", "internal server error"},
  };
  for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
    char body[128];
    int bl = snprintf(body, sizeof body, "{\"error\":\"%s\"}", t[i].msg);
    int n = sprintf((char *)m, "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", t[i].status, bl, body);
    t[i].b->p = m; t[i].b->n = (u32)n;
    m += n + 1;
  }
}
static inline obuf *const_resp(bytes b) { return obuf_static(b.p, b.n); }

/* ---- schema bindings -------------------------------------------------------------------- */
static Pgno ROOT_posts, ROOT_users, ROOT_likes_post_id_idx;
static KeyInfo *KI_likes_post_id_idx;
static cursor C_posts, C_users, C_likes_post_id_idx;
static sqlite3_stmt *Q_feed_sql, *Q_health, *Q_like_insert, *Q_post_insert;
static i64 started_at;

/* Global invariant (Feed.Inv): post ids are 1..n, created_at is non-decreasing in id, and
 * every author exists. Established by app_check_inv() at startup and re-checked after every
 * insert this process performs (it is the only writer: exclusive locking). While it holds,
 * the feed is the last 20 rows by rowid (Feed.feed_eq_tail). */
static int inv_ok;

enum { K_NONE = 0, K_HEALTH, K_FEED, K_POST, K_LIKE, K_CREATE };

static void app_init(void) {
  err_init();
  ROOT_posts = db_rootpage("posts");
  ROOT_users = db_rootpage("users");
  ROOT_likes_post_id_idx = db_rootpage("likes_post_id_idx");
  KI_likes_post_id_idx = db_keyinfo(1, 1);
  Q_feed_sql = db_prepare("SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  Q_health = db_prepare("SELECT 1");
  Q_like_insert = db_prepare("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  Q_post_insert = db_prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  started_at = time(0);
}
static void cursors_open(void) {
  C_posts = cursor_new(ROOT_posts, 0);
  C_users = cursor_new(ROOT_users, 0);
  C_likes_post_id_idx = cursor_new(ROOT_likes_post_id_idx, KI_likes_post_id_idx);
}
static void cursors_close(void) {
  cursor_close(&C_posts);
  cursor_close(&C_users);
  cursor_close(&C_likes_post_id_idx);
}

/* Startup: one pass over posts in rowid order. */
static void app_check_inv(void) {
  db_batch_begin();
  cursors_open();
  int ok = 1;
  i64 expect = 1;
  u8 prev[64]; u32 prev_n = 0;
  int rc = cur_first(&C_posts);
  while (rc == 0) {
    rec_t r; col_t c[4];
    if (cur_rowid(&C_posts) != expect || cur_record(&C_posts, &r) || rec_cols(&r, 4, c) ||
        !col_is_int(&c[1]) || !col_is_text(&c[3]) || col_len(&c[3]) > sizeof prev) { ok = 0; break; }
    u32 n = col_len(&c[3]);
    u32 m = n < prev_n ? n : prev_n;
    int cmp = memcmp(c[3].p, prev, m);
    if (cmp < 0 || (cmp == 0 && n < prev_n)) { ok = 0; break; }
    memcpy(prev, c[3].p, n); prev_n = n;
    if (cur_seek(&C_users, col_int(&c[1])) != 0) { ok = 0; break; }
    expect++;
    rc = cur_next(&C_posts);
  }
  if (rc < 0) ok = 0;
  cursors_close();
  db_batch_end();
  inv_ok = ok;
  fprintf(stderr, "invariant Feed.Inv: %s (%lld posts)\n", ok ? "holds" : "does not hold, feed uses SQL", (long long)(expect - 1));
}

/* ---- request decoding ------------------------------------------------------------------- */

/* "/posts/<id>" id segment: a positive decimal integer. Returns 1 ok, 0 invalid, -1 too big. */
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

/* Arena for decoded request data that lives until the batch completes. */
static u8 arena[1 << 22];
static u32 arena_used;
static u8 *arena_get(u32 n) {
  if (arena_used + n > sizeof arena) return 0;
  u8 *p = arena + arena_used;
  arena_used += (n + 7) & ~7u;
  return p;
}

static int authenticate(const req_t *rq, job_t *j) {
  if (!rq->has_auth || rq->auth_len < 7 || memcmp(rq->auth, "Bearer ", 7)) { j->resp = const_resp(E_NO_BEARER); return 0; }
  claims_t c;
  int rc = jwt_verify(rq->auth + 7, rq->auth_len - 7, now_s, &c);
  if (rc == JWT_INVALID) { j->resp = const_resp(E_BAD_TOKEN); return 0; }
  if (rc == JWT_PAYLOAD) { j->resp = const_resp(E_BAD_PAYLOAD); return 0; }
  u8 *name = arena_get(c.name_len + 1);
  if (!name) { j->resp = const_resp(E_INTERNAL); return 0; }
  memcpy(name, c.name, c.name_len);
  j->uid = c.uid;
  j->name = name;
  j->name_len = c.name_len;
  return 1;
}

/* JavaScript String.prototype.trim whitespace. */
static int js_space(u32 cp) {
  return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
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
  if (rc < 0) { j->resp = const_resp(E_BAD_JSON); return 0; }
  if (rc > 0 || m[0].type != JT_STRING) { j->resp = const_resp(E_NO_BODY); return 0; }
  u8 *t = arena_get(m[0].v_len + 1);
  if (!t) { j->resp = const_resp(E_INTERNAL); return 0; }
  u32 n = m[0].escaped ? json_unescape(m[0].v, m[0].v_len, t) : (memcpy(t, m[0].v, m[0].v_len), m[0].v_len);
  /* trim */
  u32 a = 0, b = n, l;
  while (a < b && js_space(cp_at(t + a, &l))) a += l;
  while (b > a) {
    u32 k = b - 1;
    while (k > a && (t[k] & 0xC0) == 0x80) k--;
    if (!js_space(cp_at(t + k, &l))) break;
    b = k;
  }
  if (a == b) { j->resp = const_resp(E_NO_BODY); return 0; }
  u32 chars = 0;
  for (u32 i = a; i < b; i++) chars += (t[i] & 0xC0) != 0x80;
  if (chars > 500) { j->resp = const_resp(E_LONG_BODY); return 0; }
  j->text = t + a;
  j->text_len = b - a;
  return 1;
}

static void app_decode(const req_t *rq, job_t *j) {
  const u8 *p = rq->path;
  u32 n = rq->path_len;
  if (rq->method == M_GET) {
    if (n == 7 && !memcmp(p, "/health", 7)) { j->kind = K_HEALTH; return; }
    if (n == 5 && !memcmp(p, "/feed", 5)) { j->kind = K_FEED; return; }
    if (n > 7 && !memcmp(p, "/posts/", 7) && !memchr(p + 7, '/', n - 7)) {
      int ok = parse_id(p + 7, n - 7, &j->id);
      if (ok == 0) { j->resp = const_resp(E_BAD_ID); return; }
      if (ok < 0) { j->resp = const_resp(E_NO_POST); return; }
      j->kind = K_POST;
      return;
    }
  } else if (rq->method == M_POST) {
    if (n == 6 && !memcmp(p, "/posts", 6)) {
      if (!authenticate(rq, j) || !decode_create(rq, j)) return;
      j->kind = K_CREATE;
      j->is_write = 1;
      return;
    }
    if (n > 12 && !memcmp(p, "/posts/", 7) && !memcmp(p + n - 5, "/like", 5) && !memchr(p + 7, '/', n - 12)) {
      if (!authenticate(rq, j)) return;
      int ok = parse_id(p + 7, n - 12, &j->id);
      if (ok == 0) { j->resp = const_resp(E_BAD_ID); return; }
      if (ok < 0) { j->resp = const_resp(E_NO_POST); return; }
      j->kind = K_LIKE;
      j->is_write = 1;
      return;
    }
  }
  j->resp = const_resp(E_NOT_FOUND);
}

/* ---- plans ------------------------------------------------------------------------------- */

#define LIT(o, s) out_lit(o, s, sizeof(s) - 1)
#define POST_ROOM(body, created, name) (96u + JSTR_ROOM(body) + JSTR_ROOM(created) + JSTR_ROOM(name))

/* PostView JSON from decoded columns. */
static inline void put_post(out_t *o, i64 id, const u8 *body, u32 bl, const u8 *cr, u32 cl, const u8 *nm, u32 nl, i64 likes) {
  out_need(o, POST_ROOM(bl, cl, nl));
  LIT(o, "{\"id\":");
  out_i64(o, id);
  LIT(o, ",\"body\":");
  out_jstr(o, body, bl);
  LIT(o, ",\"created_at\":");
  out_jstr(o, cr, cl);
  LIT(o, ",\"author\":");
  out_jstr(o, nm, nl);
  LIT(o, ",\"like_count\":");
  out_i64(o, likes);
  *o->p++ = '}';
}
/* username of a user, by rowid. 0 found, 1 absent, -1 error. */
static int user_name(i64 uid, const u8 **nm, u32 *nl) {
  int rc = cur_seek(&C_users, uid);
  if (rc) return rc;
  rec_t r; col_t c[2];
  if (cur_record(&C_users, &r) || rec_cols(&r, 2, c) || !col_is_text(&c[1])) return -1;
  *nm = c[1].p; *nl = col_len(&c[1]);
  return 0;
}
/* Likes per post for post ids lo..hi: one range scan of likes_post_id_idx. */
static int likes_range(i64 lo, i64 hi, i64 *counts) {
  memset(counts, 0, sizeof(i64) * (size_t)(hi - lo + 1));
  int rc = idx_seek_ge(&C_likes_post_id_idx, KI_likes_post_id_idx, lo);
  while (rc == 0) {
    rec_t r; col_t c[1];
    if (cur_record(&C_likes_post_id_idx, &r) || rec_cols(&r, 1, c) || !col_is_int(&c[0])) return -1;
    i64 pid = col_int(&c[0]);
    if (pid > hi) return 0;
    counts[pid - lo]++;
    rc = cur_next(&C_likes_post_id_idx);
  }
  return rc < 0 ? -1 : 0;
}

/* Feed via SQL (used when the invariant does not hold). */
static int feed_sql(out_t *o) {
  LIT(o, "{\"posts\":[");
  int i = 0, rc;
  while ((rc = sqlite3_step(Q_feed_sql)) == SQLITE_ROW) {
    if (i++) *o->p++ = ',';
    put_post(o, sqlite3_column_int64(Q_feed_sql, 0),
             sqlite3_column_text(Q_feed_sql, 1), (u32)sqlite3_column_bytes(Q_feed_sql, 1),
             sqlite3_column_text(Q_feed_sql, 2), (u32)sqlite3_column_bytes(Q_feed_sql, 2),
             sqlite3_column_text(Q_feed_sql, 3), (u32)sqlite3_column_bytes(Q_feed_sql, 3),
             sqlite3_column_int64(Q_feed_sql, 4));
  }
  sqlite3_reset(Q_feed_sql);
  if (rc != SQLITE_DONE) return -1;
  out_need(o, 2);
  LIT(o, "]}");
  return 0;
}
/* Feed under Feed.Inv: the newest 20 posts are rowids hi-19..hi. */
static int feed_tail(out_t *o) {
  int rc = cur_last(&C_posts);
  LIT(o, "{\"posts\":[");
  if (rc == 1) { LIT(o, "]}"); return 0; }
  if (rc < 0) return -1;
  i64 hi = cur_rowid(&C_posts), lo = hi > 20 ? hi - 19 : 1;
  i64 counts[20];
  if (likes_range(lo, hi, counts)) return -1;
  for (i64 k = 0; k < hi - lo + 1; k++) {
    rec_t r; col_t c[4];
    if (cur_record(&C_posts, &r) || rec_cols(&r, 4, c) || !col_is_int(&c[1]) || !col_is_text(&c[2]) || !col_is_text(&c[3])) return -1;
    i64 id = cur_rowid(&C_posts);
    if (id != hi - k) return -1;
    const u8 *nm; u32 nl;
    if (user_name(col_int(&c[1]), &nm, &nl)) return -1;
    if (k) *o->p++ = ',';
    put_post(o, id, c[2].p, col_len(&c[2]), c[3].p, col_len(&c[3]), nm, nl, counts[id - lo]);
    if (k + 1 < hi - lo + 1 && cur_prev(&C_posts)) return -1;
  }
  out_need(o, 2);
  LIT(o, "]}");
  return 0;
}
/* One post by id. 0 ok, 1 not found, -1 error. */
static int post_by_id(out_t *o, i64 id) {
  int rc = cur_seek(&C_posts, id);
  if (rc) return rc;
  rec_t r; col_t c[4];
  if (cur_record(&C_posts, &r) || rec_cols(&r, 4, c) || !col_is_int(&c[1]) || !col_is_text(&c[2]) || !col_is_text(&c[3])) return -1;
  const u8 *nm; u32 nl;
  rc = user_name(col_int(&c[1]), &nm, &nl);
  if (rc) return rc;
  i64 likes;
  if (likes_range(id, id, &likes)) return -1;
  LIT(o, "{\"post\":");
  put_post(o, id, c[2].p, col_len(&c[2]), c[3].p, col_len(&c[3]), nm, nl, likes);
  out_need(o, 1);
  *o->p++ = '}';
  return 0;
}

static void exec_read(job_t *j) {
  out_t o;
  out_begin(&o);
  int rc;
  switch (j->kind) {
  case K_FEED:
    rc = inv_ok ? feed_tail(&o) : -1;
    if (rc) { out_abort(&o); out_begin(&o); rc = feed_sql(&o); }
    if (rc) { out_abort(&o); j->resp = const_resp(E_INTERNAL); return; }
    j->resp = out_finish(&o, HDR_200, sizeof HDR_200 - 1);
    return;
  case K_POST:
    rc = post_by_id(&o, j->id);
    if (rc) { out_abort(&o); j->resp = const_resp(rc > 0 ? E_NO_POST : E_INTERNAL); return; }
    j->resp = out_finish(&o, HDR_200, sizeof HDR_200 - 1);
    return;
  case K_HEALTH:
    rc = sqlite3_step(Q_health);
    sqlite3_reset(Q_health);
    if (rc == SQLITE_ROW) {
      LIT(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
      out_i64(&o, now_s - started_at);
      *o.p++ = '}';
      j->resp = out_finish(&o, HDR_200, sizeof HDR_200 - 1);
    } else {
      const char *msg = sqlite3_errmsg(db);
      u32 ml = (u32)strlen(msg);
      out_need(&o, 64 + JSTR_ROOM(ml));
      LIT(&o, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
      out_jstr(&o, (const u8 *)msg, ml);
      *o.p++ = '}';
      j->resp = out_finish(&o, HDR_503, sizeof HDR_503 - 1);
    }
    return;
  }
}

static void exec_write(job_t *j) {
  out_t o;
  int rc;
  switch (j->kind) {
  case K_LIKE:
    rc = cur_seek(&C_posts, j->id);
    if (rc) { j->resp = const_resp(rc > 0 ? E_NO_POST : E_INTERNAL); return; }
    sqlite3_bind_int64(Q_like_insert, 1, j->uid);
    sqlite3_bind_int64(Q_like_insert, 2, j->id);
    rc = sqlite3_step(Q_like_insert);
    sqlite3_reset(Q_like_insert);
    if (rc != SQLITE_DONE) { j->resp = const_resp(E_INTERNAL); return; }
    {
      int fresh = sqlite3_changes(db) == 1;
      out_begin(&o);
      LIT(&o, "{\"liked\":true,\"already_liked\":");
      if (fresh) LIT(&o, "false"); else LIT(&o, "true");
      LIT(&o, ",\"post_id\":");
      out_i64(&o, j->id);
      *o.p++ = '}';
      j->resp = fresh ? out_finish(&o, HDR_201, sizeof HDR_201 - 1) : out_finish(&o, HDR_200, sizeof HDR_200 - 1);
    }
    return;
  case K_CREATE: {
    /* previous newest row, for the invariant check */
    i64 prev_id = 0;
    u8 prev_cr[64]; u32 prev_cl = 0;
    if (inv_ok) {
      rc = cur_last(&C_posts);
      if (rc == 0) {
        rec_t r; col_t c[4];
        if (cur_record(&C_posts, &r) || rec_cols(&r, 4, c) || !col_is_text(&c[3]) || col_len(&c[3]) > sizeof prev_cr) inv_ok = 0;
        else { prev_id = cur_rowid(&C_posts); prev_cl = col_len(&c[3]); memcpy(prev_cr, c[3].p, prev_cl); }
      } else if (rc < 0) inv_ok = 0;
    }
    sqlite3_bind_int64(Q_post_insert, 1, j->uid);
    sqlite3_bind_text(Q_post_insert, 2, (const char *)j->text, (int)j->text_len, SQLITE_STATIC);
    rc = sqlite3_step(Q_post_insert);
    if (rc != SQLITE_ROW) { sqlite3_reset(Q_post_insert); j->resp = const_resp(E_INTERNAL); return; }
    i64 id = sqlite3_column_int64(Q_post_insert, 0);
    const u8 *cr = sqlite3_column_text(Q_post_insert, 1);
    u32 cl = (u32)sqlite3_column_bytes(Q_post_insert, 1);
    if (inv_ok) {
      u32 m = cl < prev_cl ? cl : prev_cl;
      int cmp = memcmp(cr, prev_cr, m);
      if (id != prev_id + 1 || cmp < 0 || (cmp == 0 && cl < prev_cl) || cur_seek(&C_users, j->uid) != 0) {
        inv_ok = 0;
        fprintf(stderr, "invariant Feed.Inv no longer holds after insert of post %lld; feed uses SQL\n", (long long)id);
      }
    }
    out_begin(&o);
    LIT(&o, "{\"post\":");
    put_post(&o, id, j->text, j->text_len, cr, cl, j->name, j->name_len, 0);
    out_need(&o, 1);
    *o.p++ = '}';
    sqlite3_reset(Q_post_insert);
    j->resp = out_finish(&o, HDR_201, sizeof HDR_201 - 1);
    return;
  }
  }
}

static void app_batch(job_t *js, int n) {
  int reads = 0, writes = 0;
  for (int i = 0; i < n; i++) if (js[i].kind) { if (js[i].is_write) writes++; else reads++; }
  if (reads || writes) {
    db_batch_begin();
    cursors_open();
    for (int i = 0; i < n; i++) if (js[i].kind && !js[i].is_write) exec_read(&js[i]);
    for (int i = 0; i < n; i++) if (js[i].kind && js[i].is_write) exec_write(&js[i]);
    cursors_close();
    if (db_batch_end() && writes) {
      for (int i = 0; i < n; i++)
        if (js[i].kind && js[i].is_write) { if (js[i].resp) obuf_release(js[i].resp); js[i].resp = const_resp(E_INTERNAL); }
      inv_ok = 0;   /* conservative: re-established only at the next start */
    }
  }
  arena_used = 0;
}
