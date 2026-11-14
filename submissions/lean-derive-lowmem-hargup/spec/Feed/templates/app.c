/* GENERATED from the Lean specification in spec/ (Feed.Emit.appGen). Do not edit:
 * change the specification and run `lake exe derive`. */
#include "rt.h"
#include <stdio.h>

/* ---- wire constants: Feed.Contract --------------------------------------------------- */
@{HEADERS}
@{ERRORS}

/* ---- schema bindings: Feed.Schema ------------------------------------------------------ */
static Pgno ROOT_posts, ROOT_users, ROOT_likes_post_id_idx;
static KeyInfo *KI_likes_post_id_idx;
static cursor C_posts, C_users, C_likes_post_id_idx;
static sqlite3_stmt *Q_feed_sql, *Q_post_sql, *Q_post_exists, *Q_health, *Q_like_insert, *Q_post_insert;
static i64 started_at;

@{SCHEMA_CHECK}
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
  ROOT_posts = db_rootpage(@{ROOT_POSTS});
  ROOT_users = db_rootpage(@{ROOT_USERS});
  ROOT_likes_post_id_idx = db_rootpage(@{ROOT_LIKES_BY_POST});
  KI_likes_post_id_idx = db_keyinfo(@{LIKES_BY_POST_KEYS}, 1);
  Q_feed_sql = db_prepare(@{SQL_FEED});
  Q_post_sql = db_prepare(@{SQL_POST});
  Q_post_exists = db_prepare(@{SQL_POST_EXISTS});
  Q_health = db_prepare("SELECT 1");
  Q_like_insert = db_prepare(@{SQL_LIKE});
  Q_post_insert = db_prepare(@{SQL_CREATE});
  started_at = time(0);
}
static void cursors_open(void) {
  cursor_open(&C_posts, ROOT_posts, 0);
  cursor_open(&C_users, ROOT_users, 0);
  cursor_open(&C_likes_post_id_idx, ROOT_likes_post_id_idx, KI_likes_post_id_idx);
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
    rec_t r; col_t c[@{POSTS_NCOLS}];
    if (cur_rowid(&C_posts) != expect || cur_record(&C_posts, &r) || rec_cols(&r, @{POSTS_NCOLS}, c) ||
        !(@{POSTS_USER_CHECK}) || !(@{POSTS_CREATED_CHECK}) || col_len(&@{POSTS_CREATED}) > sizeof prev) { ok = 0; break; }
    const u8 *cr = @{POSTS_CREATED}.p; u32 cl = col_len(&@{POSTS_CREATED});
    if (have_prev && !text_le(prev, prev_n, cr, cl)) { ok = 0; break; }
    memcpy(prev, cr, cl); prev_n = cl; have_prev = 1;
    if (cur_seek(&C_users, col_int(&@{POSTS_USER})) != 0) { ok = 0; break; }
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
static _Alignas(8) u8 arena[1 << 22];
static u32 arena_used;
static u8 *arena_get(u32 n) {
  if (arena_used + n > sizeof arena) return 0;
  u8 *p = arena + arena_used;
  arena_used += (n + 7) & ~7u;
  return p;
}

typedef struct { u32 head; job_t *tail; } job_stream;
static job_stream stream_health = {UINT32_MAX, 0}, stream_feed = {UINT32_MAX, 0},
                  stream_post = {UINT32_MAX, 0}, stream_writes = {UINT32_MAX, 0};
static void stream_append(job_stream *s, job_t *j, u32 slot) {
  j->next = UINT32_MAX;
  if (s->tail) s->tail->next = slot; else s->head = slot;
  s->tail = j;
}
static void app_reset(void) {
  stream_health = stream_feed = stream_post = stream_writes = (job_stream){UINT32_MAX, 0};
  arena_used = 0;
}
static inline __attribute__((always_inline)) int authenticate(const req_t *rq, job_t *j, claims_t *c) {
  if (!rq->has_auth || rq->auth_len < 7 || memcmp(rq->auth, "Bearer ", 7)) { j->resp = ERR(@{ERR_NO_BEARER}); return 0; }
  int rc = jwt_verify(rq->auth + 7, rq->auth_len - 7, now_s, c);
  if (rc == JWT_INVALID) { j->resp = ERR(@{ERR_BAD_TOKEN}); return 0; }
  if (rc == JWT_PAYLOAD) { j->resp = ERR(@{ERR_BAD_PAYLOAD}); return 0; }
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
static int decode_create(const req_t *rq, job_t *j, const u8 **text, u32 *text_len, u8 *text_plain) {
  jmember m[1] = {{"body", 4, 0, 0, 0, 0}};
  int rc = json_members(rq->body, rq->body_len, m, 1);
  if (rc < 0) { j->resp = ERR(@{ERR_BAD_JSON}); return 0; }
  if (rc > 0 || m[0].type != JT_STRING) { j->resp = ERR(@{ERR_NO_BODY}); return 0; }
  u8 *t = arena_get(m[0].v_len + 1);
  if (!t) { j->resp = ERR(@{ERR_INTERNAL}); return 0; }
  u32 n = m[0].escaped ? json_unescape(m[0].v, m[0].v_len, t) : (memcpy(t, m[0].v, m[0].v_len), m[0].v_len);
  u32 a = 0, b = n, l;
  while (a < b && js_space(cp_at(t + a, &l))) a += l;
  while (b > a) {
    u32 k = b - 1;
    while (k > a && (t[k] & 0xC0) == 0x80) k--;
    if (!js_space(cp_at(t + k, &l))) break;
    b = k;
  }
  if (a == b) { j->resp = ERR(@{ERR_NO_BODY}); return 0; }
  u32 chars = 0;
  for (u32 i = a; i < b; i++) chars += (t[i] & 0xC0) != 0x80;
  if (chars > @{MAX_BODY_CHARS}) { j->resp = ERR(@{ERR_LONG_BODY}); return 0; }
  *text = t + a;
  *text_len = b - a;
  *text_plain = !m[0].escaped; /* validated substring contains no JSON escape bytes */
  return 1;
}

@{DECODE}
/* ---- serializers: Feed.Contract shapes ------------------------------------------------- */
@{PUT_POST}
/* ---- read plans: Feed.Plan ------------------------------------------------------------- */

/* Feed.Db.user? : username of a user, by rowid. 0 found, 1 absent, -1 error */
static int user_name(i64 uid, const u8 **author, u32 *author_len) {
  int rc = cur_seek(&C_users, uid);
  if (rc) return rc;
  rec_t r;
  if (cur_record(&C_users, &r)) return -1;
  @{USERS_NAME_PROJECTION}
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
  @{FEED_BEFORE}
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
  @{FEED_AFTER}
  return 0;
}

/* GET /feed under Feed.Db.Inv: Feed.Db.feedPlan */
static int feed_tail(out_t *o) {
  @{FEED_BEFORE}
  out_lit(o, "[", 1);
  int rc = cur_last(&C_posts);
  if (rc < 0) return -1;
  if (rc == 0) {
    i64 hi = cur_rowid(&C_posts);
    i64 lo = hi > @{FEED_SIZE} ? hi - @{WINDOW_SPAN} : 1;      /* Feed.Db.window */
    i64 counts[@{FEED_SIZE}];
    if (range_counts(lo, hi, counts)) return -1;
    for (i64 k = 0; k <= hi - lo; k++) {                         /* Feed.Db.tailRows */
      rec_t r; col_t c[@{POSTS_NCOLS}];
      if (cur_record(&C_posts, &r) || rec_cols(&r, @{POSTS_NCOLS}, c) || !(@{POSTS_CHECKS})) return -1;
      i64 id = cur_rowid(&C_posts);
      if (id != hi - k) return -1;                               /* Inv.dense, checked */
      const u8 *author; u32 author_len;
      if (user_name(col_int(&@{POSTS_USER}), &author, &author_len)) return -1;
      if (k) { out_need(o, 1); out_lit(o, ",", 1); }
      put_post(o, id, @{POSTS_BODY}.p, col_len(&@{POSTS_BODY}), @{POSTS_CREATED}.p, col_len(&@{POSTS_CREATED}),
               author, author_len, counts[id - lo]);
      if (k < hi - lo && cur_prev(&C_posts)) return -1;
    }
  }
  out_need(o, 1);
  out_lit(o, "]", 1);
  @{FEED_AFTER}
  return 0;
}

/* GET /posts/:id: Feed.Db.postPlan. 0 ok, 1 not found, -1 error */
static int post_by_id(out_t *o, i64 id) {
  int rc = cur_seek(&C_posts, id);
  if (rc) return rc;
  rec_t r; col_t c[@{POSTS_NCOLS}];
  if (cur_record(&C_posts, &r) || rec_cols(&r, @{POSTS_NCOLS}, c) || !(@{POSTS_CHECKS})) return -1;
  const u8 *author; u32 author_len;
  rc = user_name(col_int(&@{POSTS_USER}), &author, &author_len);
  if (rc) return rc;
  i64 likes;
  if (range_counts(id, id, &likes)) return -1;
  @{POST_BEFORE}
  put_post(o, id, @{POSTS_BODY}.p, col_len(&@{POSTS_BODY}), @{POSTS_CREATED}.p, col_len(&@{POSTS_CREATED}), author, author_len, likes);
  @{POST_AFTER}
  return 0;
}

/* GET /posts/:id without native plans: the definition, in SQL. 0 ok, 1 not found, -1 error */
static int post_sql(out_t *o, i64 id) {
  sqlite3_bind_int64(Q_post_sql, 1, id);
  int rc = sqlite3_step(Q_post_sql);
  if (rc == SQLITE_ROW) {
    @{POST_BEFORE}
    put_post(o, sqlite3_column_int64(Q_post_sql, 0),
             sqlite3_column_text(Q_post_sql, 1), (u32)sqlite3_column_bytes(Q_post_sql, 1),
             sqlite3_column_text(Q_post_sql, 2), (u32)sqlite3_column_bytes(Q_post_sql, 2),
             sqlite3_column_text(Q_post_sql, 3), (u32)sqlite3_column_bytes(Q_post_sql, 3),
             sqlite3_column_int64(Q_post_sql, 4));
    @{POST_AFTER}
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

static void exec_feed(job_t *j) {
  out_t o0, *o = &o0;
  int rc;
    out_begin(o, @{FEED_CLASS});
    rc = native_ok && inv_ok ? feed_tail(o) : -1;
    if (rc) { out_abort(o); out_begin(o, @{FEED_CLASS}); rc = feed_sql(o); }
    if (rc) { out_abort(o); j->resp = ERR(@{ERR_INTERNAL}); return; }
    j->resp = out_finish(o, @{HDR_OK}, sizeof @{HDR_OK} - 1);
    return;
}

static void exec_post(job_t *j) {
  out_t o0, *o = &o0;
  int rc;
    out_begin(o, @{OBJECT_CLASS});
    rc = native_ok ? post_by_id(o, @{POST_ID}) : -1;
    if (rc < 0) { out_abort(o); out_begin(o, @{OBJECT_CLASS}); rc = post_sql(o, @{POST_ID}); }
    if (rc) { out_abort(o); j->resp = rc > 0 ? ERR(@{ERR_NO_POST}) : ERR(@{ERR_INTERNAL}); return; }
    j->resp = out_finish(o, @{HDR_OK}, sizeof @{HDR_OK} - 1);
    return;
}

static void exec_health(job_t *j) {
  out_t o0, *o = &o0;
  int rc;
    rc = sqlite3_step(Q_health);
    sqlite3_reset(Q_health);
    if (rc == SQLITE_ROW) {
      @{HEALTH_BEGIN}
      i64 uptime = now_s - started_at;
      @{HEALTH_BODY}
      j->resp = out_finish(o, @{HDR_OK}, sizeof @{HDR_OK} - 1);
    } else {
      out_begin(o, @{OBJECT_CLASS});
      const u8 *msg = (const u8 *)sqlite3_errmsg(db);
      u32 msg_len = (u32)strlen((const char *)msg);
      @{DEGRADED_BODY}
      j->resp = out_finish(o, @{HDR_UNAVAILABLE}, sizeof @{HDR_UNAVAILABLE} - 1);
    }
    return;
}

/* ---- writes ---------------------------------------------------------------------------- */

static void exec_write(job_t *j) {
  out_t o0, *o = &o0;
  int rc;
  switch (j->kind) {
  case K_LIKE: {                                                 /* Feed.Db.like */
    rc = post_exists(@{LIKE_ID});
    if (rc) { j->resp = rc > 0 ? ERR(@{ERR_NO_POST}) : ERR(@{ERR_INTERNAL}); return; }
    sqlite3_bind_int64(Q_like_insert, 1, @{LIKE_USER});
    sqlite3_bind_int64(Q_like_insert, 2, @{LIKE_ID});
    rc = sqlite3_step(Q_like_insert);
    sqlite3_reset(Q_like_insert);
    if (rc != SQLITE_DONE) { j->resp = ERR(@{ERR_INTERNAL}); return; }
    int already = sqlite3_changes(db) != 1;
    i64 id = @{LIKE_ID};
    @{LIKE_BEGIN}
    @{LIKE_BODY}
    j->resp = already ? out_finish(o, @{HDR_OK}, sizeof @{HDR_OK} - 1) : out_finish(o, @{HDR_CREATED}, sizeof @{HDR_CREATED} - 1);
    return;
  }
  case K_CREATE: {                                               /* Feed.Db.newPost */
    i64 prev_id = 0;
    u8 prev_cr[256]; u32 prev_cl = 0; int have_prev = 0;
    if (inv_ok) {                    /* the newest row before the insert, for Feed.Db.insertOk */
      rc = cur_last(&C_posts);
      if (rc == 0) {
        rec_t r; col_t c[@{POSTS_NCOLS}];
        if (cur_record(&C_posts, &r) || rec_cols(&r, @{POSTS_NCOLS}, c) || !(@{POSTS_CREATED_CHECK}) || col_len(&@{POSTS_CREATED}) > sizeof prev_cr) inv_ok = 0;
        else { prev_id = cur_rowid(&C_posts); prev_cl = col_len(&@{POSTS_CREATED}); memcpy(prev_cr, @{POSTS_CREATED}.p, prev_cl); have_prev = 1; }
      } else if (rc < 0) inv_ok = 0;
    }
    sqlite3_bind_int64(Q_post_insert, 1, @{CREATE_USER});
    sqlite3_bind_text(Q_post_insert, 2, (const char *)@{CREATE_BODY}, (int)@{CREATE_BODY}_len, SQLITE_STATIC);
    rc = sqlite3_step(Q_post_insert);
    if (rc != SQLITE_ROW) { sqlite3_reset(Q_post_insert); j->resp = ERR(@{ERR_INTERNAL}); return; }
    i64 id = sqlite3_column_int64(Q_post_insert, 0);
    const u8 *created = sqlite3_column_text(Q_post_insert, 1);
    u32 created_len = (u32)sqlite3_column_bytes(Q_post_insert, 1);
    if (inv_ok && !(id == prev_id + 1 && (!have_prev || text_le(prev_cr, prev_cl, created, created_len)) &&
                    cur_seek(&C_users, @{CREATE_USER}) == 0)) {             /* Feed.Db.insertOk */
      inv_ok = 0;
      fprintf(stderr, "Feed.Db.Inv no longer holds after post %lld; feed uses SQL\n", (long long)id);
    }
    out_begin(o, @{OBJECT_CLASS});
    @{CREATE_BEFORE}
    if (@{CREATE_BODY}_plain) put_post_plain(o, id, @{CREATE_BODY}, @{CREATE_BODY}_len, created, created_len, @{CREATE_NAME}, @{CREATE_NAME}_len, 0);
    else put_post(o, id, @{CREATE_BODY}, @{CREATE_BODY}_len, created, created_len, @{CREATE_NAME}, @{CREATE_NAME}_len, 0);
    @{CREATE_AFTER}
    sqlite3_reset(Q_post_insert);
    j->resp = out_finish(o, @{HDR_CREATED}, sizeof @{HDR_CREATED} - 1);
    return;
  }
  }
}

/* One batch = one transaction: reads (snapshot at batch start), then writes, then COMMIT.
 * No response leaves before the batch has committed. */
static void app_batch(job_t *js, int n) {
  (void)n;
  if (stream_health.tail || stream_feed.tail || stream_post.tail || stream_writes.tail) {
    db_batch_begin();
    if (db_foreign_write) native_ok = inv_ok = 0;
    cursors_open();
    for (u32 i = stream_health.head; i != UINT32_MAX; i = js[i].next) exec_health(&js[i]);
    for (u32 i = stream_feed.head; i != UINT32_MAX; i = js[i].next) exec_feed(&js[i]);
    for (u32 i = stream_post.head; i != UINT32_MAX; i = js[i].next) exec_post(&js[i]);
    for (u32 i = stream_writes.head; i != UINT32_MAX; i = js[i].next) exec_write(&js[i]);
    cursors_close();
    if (db_batch_end() && stream_writes.tail) {
      for (u32 i = stream_writes.head; i != UINT32_MAX; i = js[i].next) {
        if (js[i].resp) obuf_release(js[i].resp);
        js[i].resp = ERR(@{ERR_INTERNAL});
      }
      inv_ok = 0;
    }
  }
  app_reset();
}
