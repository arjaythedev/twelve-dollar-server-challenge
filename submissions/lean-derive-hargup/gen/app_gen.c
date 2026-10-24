/* GENERATED from the Lean specification in spec/ (Feed.Emit.appGen). Do not edit:
 * change the specification and run `lake exe derive`. */
#include "rt.h"
#include <stdio.h>

/* ---- wire constants: Feed.Contract --------------------------------------------------- */
static const u8 HDR_200[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
static const u8 HDR_201[] = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ";
static const u8 HDR_503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ";

static const u8 E_NOT_FOUND[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}";  /* 404 not found */
static const u8 E_BAD_ID[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"invalid post id\"}";  /* 400 invalid post id */
static const u8 E_NO_POST[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 26\r\n\r\n{\"error\":\"post not found\"}";  /* 404 post not found */
static const u8 E_NO_BEARER[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"error\":\"missing bearer token\"}";  /* 401 missing bearer token */
static const u8 E_BAD_TOKEN[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"error\":\"invalid or expired token\"}";  /* 401 invalid or expired token */
static const u8 E_BAD_PAYLOAD[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"invalid token payload\"}";  /* 401 invalid token payload */
static const u8 E_BAD_JSON[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\n\r\n{\"error\":\"malformed JSON body\"}";  /* 400 malformed JSON body */
static const u8 E_NO_BODY[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 28\r\n\r\n{\"error\":\"body is required\"}";  /* 400 body is required */
static const u8 E_LONG_BODY[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 47\r\n\r\n{\"error\":\"body must be at most 500 characters\"}";  /* 400 body must be at most 500 characters */
static const u8 E_INTERNAL[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";  /* 500 internal server error */
#define ERR(name) obuf_static(E_##name, sizeof E_##name - 1)


/* ---- schema bindings: Feed.Schema ------------------------------------------------------ */
static Pgno ROOT_posts, ROOT_users, ROOT_likes_post_id_idx;
static KeyInfo *KI_likes_post_id_idx;
static cursor C_posts, C_users, C_likes_post_id_idx;
static sqlite3_stmt *Q_feed_sql, *Q_post_sql, *Q_post_exists, *Q_health, *Q_like_insert, *Q_post_insert;
static i64 started_at;

static int schema_ok(void) {
  int ok = 1;
  ok &= table_layout("users", "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0");
  ok &= table_layout("posts", "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0");
  ok &= table_layout("likes", "user_id:INTEGER:0,post_id:INTEGER:0,created_at:TEXT:0");
  ok &= index_layout("likes_post_id_idx", "post_id");
  return ok;
}

/* native_ok: the stored layout is the one the B-tree plans were derived for (schema_ok), and no
 * other connection has written since startup (db_foreign_write). Otherwise every read uses SQL.
 * inv_ok: Feed.Db.Inv holds for the current database. Established by app_check_inv
 * (Feed.Db.checkInv, sound by Feed.Db.checkInv_sound) and kept by the check after every insert
 * (Feed.Db.insertOk, Feed.Db.inv_addPost); that covers every write while this process is the
 * only writer, which native_ok confirms each batch. While it holds, GET /feed uses feed_tail
 * (Feed.Db.feedPlan, equal to the feed by Feed.Db.feedPlan_eq). */
static int native_ok, inv_ok;

enum { K_NONE = 0, K_HEALTH, K_FEED, K_POST, K_LIKE, K_CREATE };

static void app_init(void) {
  ROOT_posts = db_rootpage("posts");
  ROOT_users = db_rootpage("users");
  ROOT_likes_post_id_idx = db_rootpage("likes_post_id_idx");
  KI_likes_post_id_idx = db_keyinfo(1, 1);
  Q_feed_sql = db_prepare("SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  Q_post_sql = db_prepare("SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = \?1");
  Q_post_exists = db_prepare("SELECT 1 FROM posts WHERE id = \?1");
  Q_health = db_prepare("SELECT 1");
  Q_like_insert = db_prepare("INSERT INTO likes (user_id, post_id) VALUES (\?1, \?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  Q_post_insert = db_prepare("INSERT INTO posts (user_id, body) VALUES (\?1, \?2) RETURNING id, created_at");
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

/* text order of two stored strings (SQLite BINARY collation) */
static inline int text_le(const u8 *a, u32 an, const u8 *b, u32 bn) {
  u32 m = an < bn ? an : bn;
  int c = memcmp(a, b, m);
  return c < 0 || (c == 0 && an <= bn);
}

/* Feed.Db.checkFrom over the posts B-tree in rowid order. */
static void app_check_inv(void) {
  if (!schema_ok()) {
    native_ok = inv_ok = 0;
    fprintf(stderr, "schema differs from Feed.Schema: native plans disabled\n");
    return;
  }
  native_ok = 1;
  db_batch_begin();
  cursors_open();
  int ok = 1;
  i64 expect = 1;
  u8 prev[256]; u32 prev_n = 0; int have_prev = 0;
  int rc = cur_first(&C_posts);
  while (ok && rc == 0) {
    rec_t r; col_t c[4];
    if (cur_rowid(&C_posts) != expect || cur_record(&C_posts, &r) || rec_cols(&r, 4, c) ||
        !(col_is_int(&c[1])) || !(col_is_text(&c[3])) || col_len(&c[3]) > sizeof prev) { ok = 0; break; }
    const u8 *cr = c[3].p; u32 cl = col_len(&c[3]);
    if (have_prev && !text_le(prev, prev_n, cr, cl)) { ok = 0; break; }
    memcpy(prev, cr, cl); prev_n = cl; have_prev = 1;
    if (cur_seek(&C_users, col_int(&c[1])) != 0) { ok = 0; break; }
    expect++;
    rc = cur_next(&C_posts);
  }
  if (rc < 0) ok = 0;
  cursors_close();
  db_batch_end();
  inv_ok = ok;
  fprintf(stderr, "Feed.Db.Inv: %s (%lld posts checked)\n", ok ? "holds" : "does not hold, feed uses SQL", (long long)(expect - 1));
}

/* ---- request decoding: Feed.Contract.routes ------------------------------------------- */

/* the ":id" segment: a positive decimal integer. 1 ok, 0 invalid, -1 beyond any rowid */
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

/* request data that must outlive the receive buffer until the batch completes */
static u8 arena[1 << 22];
static u32 arena_used;
static u8 *arena_get(u32 n) {
  if (arena_used + n > sizeof arena) return 0;
  u8 *p = arena + arena_used;
  arena_used += (n + 7) & ~7u;
  return p;
}

static int authenticate(const req_t *rq, job_t *j) {
  if (!rq->has_auth || rq->auth_len < 7 || memcmp(rq->auth, "Bearer ", 7)) { j->resp = ERR(NO_BEARER); return 0; }
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

/* String.prototype.trim whitespace */
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
  if (rq->method == M_GET) {
    const u8 *s = p, *e = p + n, *id_p = 0; u32 id_n = 0;
    (void)id_p; (void)id_n;
    if (path_lit(&s, e, "/health", 7) && s == e) {
      
      j->kind = K_HEALTH; j->is_write = 0;
      return;
    }
  }
  if (rq->method == M_GET) {
    const u8 *s = p, *e = p + n, *id_p = 0; u32 id_n = 0;
    (void)id_p; (void)id_n;
    if (path_lit(&s, e, "/feed", 5) && s == e) {
      
      j->kind = K_FEED; j->is_write = 0;
      return;
    }
  }
  if (rq->method == M_GET) {
    const u8 *s = p, *e = p + n, *id_p = 0; u32 id_n = 0;
    (void)id_p; (void)id_n;
    if (path_lit(&s, e, "/posts", 6) && path_param(&s, e, &id_p, &id_n) && s == e) {
      { int ok = parse_id(id_p, id_n, &j->id); if (ok == 0) { j->resp = ERR(BAD_ID); return; } if (ok < 0) { j->resp = ERR(NO_POST); return; } }
      j->kind = K_POST; j->is_write = 0;
      return;
    }
  }
  if (rq->method == M_POST) {
    const u8 *s = p, *e = p + n, *id_p = 0; u32 id_n = 0;
    (void)id_p; (void)id_n;
    if (path_lit(&s, e, "/posts", 6) && s == e) {
      if (!authenticate(rq, j)) return;
      if (!decode_create(rq, j)) return;
      j->kind = K_CREATE; j->is_write = 1;
      return;
    }
  }
  if (rq->method == M_POST) {
    const u8 *s = p, *e = p + n, *id_p = 0; u32 id_n = 0;
    (void)id_p; (void)id_n;
    if (path_lit(&s, e, "/posts", 6) && path_param(&s, e, &id_p, &id_n) && path_lit(&s, e, "/like", 5) && s == e) {
      if (!authenticate(rq, j)) return;
      { int ok = parse_id(id_p, id_n, &j->id); if (ok == 0) { j->resp = ERR(BAD_ID); return; } if (ok < 0) { j->resp = ERR(NO_POST); return; } }
      j->kind = K_LIKE; j->is_write = 1;
      return;
    }
  }
  j->resp = ERR(NOT_FOUND);
}

/* ---- serializers: Feed.Contract shapes ------------------------------------------------- */
/* Feed.Contract.postShape */
static inline void put_post(out_t *o, i64 id, const u8 *body, u32 body_len, const u8 *created, u32 created_len, const u8 *author, u32 author_len, i64 likes) {
  out_need(o, 93 + JSTR_ROOM(body_len) + JSTR_ROOM(created_len) + JSTR_ROOM(author_len));
  out_lit(o, "{\"id\":", 6);
  out_i64(o, id);
  out_lit(o, ",\"body\":", 8);
  out_jstr(o, body, body_len);
  out_lit(o, ",\"created_at\":", 14);
  out_jstr(o, created, created_len);
  out_lit(o, ",\"author\":", 10);
  out_jstr(o, author, author_len);
  out_lit(o, ",\"like_count\":", 14);
  out_i64(o, likes);
  out_lit(o, "}", 1);
}

/* ---- read plans: Feed.Plan ------------------------------------------------------------- */

/* Feed.Db.user? : username of a user, by rowid. 0 found, 1 absent, -1 error */
static int user_name(i64 uid, const u8 **author, u32 *author_len) {
  int rc = cur_seek(&C_users, uid);
  if (rc) return rc;
  rec_t r; col_t c[3];
  if (cur_record(&C_users, &r) || rec_cols(&r, 3, c) || !(col_is_text(&c[1]))) return -1;
  *author = c[1].p;
  *author_len = col_len(&c[1]);
  return 0;
}
/* Feed.Db.rangeCounts: like counts for post ids lo..hi from one scan of likes_post_id_idx */
static int range_counts(i64 lo, i64 hi, i64 *counts) {
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

/* GET /feed without the invariant: the definition, in SQL */
static int feed_sql(out_t *o) {
  out_need(o, 9);
  out_lit(o, "{\"posts\":", 9);
  out_lit(o, "[", 1);
  int i = 0, rc;
  while ((rc = sqlite3_step(Q_feed_sql)) == SQLITE_ROW) {
    if (i++) { out_need(o, 1); out_lit(o, ",", 1); }
    put_post(o, sqlite3_column_int64(Q_feed_sql, 0),
             sqlite3_column_text(Q_feed_sql, 1), (u32)sqlite3_column_bytes(Q_feed_sql, 1),
             sqlite3_column_text(Q_feed_sql, 2), (u32)sqlite3_column_bytes(Q_feed_sql, 2),
             sqlite3_column_text(Q_feed_sql, 3), (u32)sqlite3_column_bytes(Q_feed_sql, 3),
             sqlite3_column_int64(Q_feed_sql, 4));
  }
  sqlite3_reset(Q_feed_sql);
  if (rc != SQLITE_DONE) return -1;
  out_need(o, 1);
  out_lit(o, "]", 1);
  out_need(o, 1);
  out_lit(o, "}", 1);
  return 0;
}

/* GET /feed under Feed.Db.Inv: Feed.Db.feedPlan */
static int feed_tail(out_t *o) {
  out_need(o, 9);
  out_lit(o, "{\"posts\":", 9);
  out_lit(o, "[", 1);
  int rc = cur_last(&C_posts);
  if (rc < 0) return -1;
  if (rc == 0) {
    i64 hi = cur_rowid(&C_posts);
    i64 lo = hi > 20 ? hi - 19 : 1;      /* Feed.Db.window */
    i64 counts[20];
    if (range_counts(lo, hi, counts)) return -1;
    for (i64 k = 0; k <= hi - lo; k++) {                         /* Feed.Db.tailRows */
      rec_t r; col_t c[4];
      if (cur_record(&C_posts, &r) || rec_cols(&r, 4, c) || !(col_is_int(&c[1]) && col_is_text(&c[2]) && col_is_text(&c[3]))) return -1;
      i64 id = cur_rowid(&C_posts);
      if (id != hi - k) return -1;                               /* Inv.dense, checked */
      const u8 *author; u32 author_len;
      if (user_name(col_int(&c[1]), &author, &author_len)) return -1;
      if (k) { out_need(o, 1); out_lit(o, ",", 1); }
      put_post(o, id, c[2].p, col_len(&c[2]), c[3].p, col_len(&c[3]),
               author, author_len, counts[id - lo]);
      if (k < hi - lo && cur_prev(&C_posts)) return -1;
    }
  }
  out_need(o, 1);
  out_lit(o, "]", 1);
  out_need(o, 1);
  out_lit(o, "}", 1);
  return 0;
}

/* GET /posts/:id: Feed.Db.postPlan. 0 ok, 1 not found, -1 error */
static int post_by_id(out_t *o, i64 id) {
  int rc = cur_seek(&C_posts, id);
  if (rc) return rc;
  rec_t r; col_t c[4];
  if (cur_record(&C_posts, &r) || rec_cols(&r, 4, c) || !(col_is_int(&c[1]) && col_is_text(&c[2]) && col_is_text(&c[3]))) return -1;
  const u8 *author; u32 author_len;
  rc = user_name(col_int(&c[1]), &author, &author_len);
  if (rc) return rc;
  i64 likes;
  if (range_counts(id, id, &likes)) return -1;
  out_need(o, 8);
  out_lit(o, "{\"post\":", 8);
  put_post(o, id, c[2].p, col_len(&c[2]), c[3].p, col_len(&c[3]), author, author_len, likes);
  out_need(o, 1);
  out_lit(o, "}", 1);
  return 0;
}

/* GET /posts/:id without native plans: the definition, in SQL. 0 ok, 1 not found, -1 error */
static int post_sql(out_t *o, i64 id) {
  sqlite3_bind_int64(Q_post_sql, 1, id);
  int rc = sqlite3_step(Q_post_sql);
  if (rc == SQLITE_ROW) {
    out_need(o, 8);
  out_lit(o, "{\"post\":", 8);
    put_post(o, sqlite3_column_int64(Q_post_sql, 0),
             sqlite3_column_text(Q_post_sql, 1), (u32)sqlite3_column_bytes(Q_post_sql, 1),
             sqlite3_column_text(Q_post_sql, 2), (u32)sqlite3_column_bytes(Q_post_sql, 2),
             sqlite3_column_text(Q_post_sql, 3), (u32)sqlite3_column_bytes(Q_post_sql, 3),
             sqlite3_column_int64(Q_post_sql, 4));
    out_need(o, 1);
  out_lit(o, "}", 1);
  }
  sqlite3_reset(Q_post_sql);
  return rc == SQLITE_ROW ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
/* Does the post exist? 0 yes, 1 no, -1 error */
static int post_exists(i64 id) {
  if (native_ok) return cur_seek(&C_posts, id);
  sqlite3_bind_int64(Q_post_exists, 1, id);
  int rc = sqlite3_step(Q_post_exists);
  sqlite3_reset(Q_post_exists);
  return rc == SQLITE_ROW ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

static void exec_read(job_t *j) {
  out_t o0, *o = &o0;
  out_begin(o);
  int rc;
  switch (j->kind) {
  case K_FEED:
    rc = native_ok && inv_ok ? feed_tail(o) : -1;
    if (rc) { out_abort(o); out_begin(o); rc = feed_sql(o); }
    if (rc) { out_abort(o); j->resp = ERR(INTERNAL); return; }
    j->resp = out_finish(o, HDR_200, sizeof HDR_200 - 1);
    return;
  case K_POST:
    rc = native_ok ? post_by_id(o, j->id) : -1;
    if (rc < 0) { out_abort(o); out_begin(o); rc = post_sql(o, j->id); }
    if (rc) { out_abort(o); j->resp = rc > 0 ? ERR(NO_POST) : ERR(INTERNAL); return; }
    j->resp = out_finish(o, HDR_200, sizeof HDR_200 - 1);
    return;
  case K_HEALTH:
    rc = sqlite3_step(Q_health);
    sqlite3_reset(Q_health);
    if (rc == SQLITE_ROW) {
      i64 uptime = now_s - started_at;
      out_need(o, 57);
  out_lit(o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36);
  out_i64(o, uptime);
  out_lit(o, "}", 1);
      j->resp = out_finish(o, HDR_200, sizeof HDR_200 - 1);
    } else {
      const u8 *msg = (const u8 *)sqlite3_errmsg(db);
      u32 msg_len = (u32)strlen((const char *)msg);
      out_need(o, 49 + JSTR_ROOM(msg_len));
  out_lit(o, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":", 48);
  out_jstr(o, msg, msg_len);
  out_lit(o, "}", 1);
      j->resp = out_finish(o, HDR_503, sizeof HDR_503 - 1);
    }
    return;
  }
}

/* ---- writes ---------------------------------------------------------------------------- */

static void exec_write(job_t *j) {
  out_t o0, *o = &o0;
  int rc;
  switch (j->kind) {
  case K_LIKE: {                                                 /* Feed.Db.like */
    rc = post_exists(j->id);
    if (rc) { j->resp = rc > 0 ? ERR(NO_POST) : ERR(INTERNAL); return; }
    sqlite3_bind_int64(Q_like_insert, 1, j->uid);
    sqlite3_bind_int64(Q_like_insert, 2, j->id);
    rc = sqlite3_step(Q_like_insert);
    sqlite3_reset(Q_like_insert);
    if (rc != SQLITE_DONE) { j->resp = ERR(INTERNAL); return; }
    int already = sqlite3_changes(db) != 1;
    i64 id = j->id;
    out_begin(o);
    out_need(o, 67);
  out_lit(o, "{\"liked\":true,\"already_liked\":", 30);
  if (already) out_lit(o, "true", 4); else out_lit(o, "false", 5);
  out_lit(o, ",\"post_id\":", 11);
  out_i64(o, id);
  out_lit(o, "}", 1);
    j->resp = already ? out_finish(o, HDR_200, sizeof HDR_200 - 1) : out_finish(o, HDR_201, sizeof HDR_201 - 1);
    return;
  }
  case K_CREATE: {                                               /* Feed.Db.newPost */
    i64 prev_id = 0;
    u8 prev_cr[256]; u32 prev_cl = 0; int have_prev = 0;
    if (inv_ok) {                    /* the newest row before the insert, for Feed.Db.insertOk */
      rc = cur_last(&C_posts);
      if (rc == 0) {
        rec_t r; col_t c[4];
        if (cur_record(&C_posts, &r) || rec_cols(&r, 4, c) || !(col_is_text(&c[3])) || col_len(&c[3]) > sizeof prev_cr) inv_ok = 0;
        else { prev_id = cur_rowid(&C_posts); prev_cl = col_len(&c[3]); memcpy(prev_cr, c[3].p, prev_cl); have_prev = 1; }
      } else if (rc < 0) inv_ok = 0;
    }
    sqlite3_bind_int64(Q_post_insert, 1, j->uid);
    sqlite3_bind_text(Q_post_insert, 2, (const char *)j->text, (int)j->text_len, SQLITE_STATIC);
    rc = sqlite3_step(Q_post_insert);
    if (rc != SQLITE_ROW) { sqlite3_reset(Q_post_insert); j->resp = ERR(INTERNAL); return; }
    i64 id = sqlite3_column_int64(Q_post_insert, 0);
    const u8 *created = sqlite3_column_text(Q_post_insert, 1);
    u32 created_len = (u32)sqlite3_column_bytes(Q_post_insert, 1);
    if (inv_ok && !(id == prev_id + 1 && (!have_prev || text_le(prev_cr, prev_cl, created, created_len)) &&
                    cur_seek(&C_users, j->uid) == 0)) {             /* Feed.Db.insertOk */
      inv_ok = 0;
      fprintf(stderr, "Feed.Db.Inv no longer holds after post %lld; feed uses SQL\n", (long long)id);
    }
    out_begin(o);
    out_need(o, 8);
  out_lit(o, "{\"post\":", 8);
    put_post(o, id, j->text, j->text_len, created, created_len, j->name, j->name_len, 0);
    out_need(o, 1);
  out_lit(o, "}", 1);
    sqlite3_reset(Q_post_insert);
    j->resp = out_finish(o, HDR_201, sizeof HDR_201 - 1);
    return;
  }
  }
}

/* One batch = one transaction: reads (snapshot at batch start), then writes, then COMMIT.
 * No response leaves before the batch has committed. */
static void app_batch(job_t *js, int n) {
  int reads = 0, writes = 0;
  for (int i = 0; i < n; i++) if (js[i].kind) { if (js[i].is_write) writes++; else reads++; }
  if (reads || writes) {
    db_batch_begin();
    if (db_foreign_write) native_ok = inv_ok = 0;
    cursors_open();
    for (int i = 0; i < n; i++) if (js[i].kind && !js[i].is_write) exec_read(&js[i]);
    for (int i = 0; i < n; i++) if (js[i].kind && js[i].is_write) exec_write(&js[i]);
    cursors_close();
    if (db_batch_end() && writes) {
      for (int i = 0; i < n; i++)
        if (js[i].kind && js[i].is_write) { if (js[i].resp) obuf_release(js[i].resp); js[i].resp = ERR(INTERNAL); }
      inv_ok = 0;
    }
  }
  arena_used = 0;
}
