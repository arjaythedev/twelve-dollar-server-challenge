/* HTTP/1.1 parsing, the http_requests virtual table, the SQL functions server.sql calls, and running one
 * batch. No socket code: server.c (epoll) and harness.c (tests without a network) both use this file. */
#include "sqlite3.h"
#include "jwt.h"
#include <stdio.h>
#include <stdlib.h>

/* A connection as the app sees it. server.c owns the socket; the app reads `in` and appends to `out`. */
typedef struct conn {
  int fd;
  uint32_t gen;             /* bumped on close, so a stale reference is ignored */
  char *in;  uint32_t in_len, in_cap;
  char *out; uint32_t out_len, out_off, out_cap;
  uint8_t open, busy, close_after, sent_continue;
  struct conn *prev, *next; /* idle list (server.c) */
} conn;

enum { R_HEALTH, R_FEED, R_POST, R_CREATE, R_LIKE, R_NOTFOUND };
static const char *const ROUTE_NAME[] = {"health", "feed", "post", "create", "like", "notfound"};

/* One request in the current batch. auth and body point into the batch arena. */
typedef struct {
  conn *c; uint32_t gen;
  uint8_t route, answered;
  int64_t post_id;          /* 0 = not a valid id */
  const char *auth; int auth_len;
  const char *body; int body_len;
  uint32_t out_mark;        /* c->out_len before this reply, to undo it if the commit fails */
} request;

enum { BATCH_MAX = 256, ARENA_SIZE = 4 << 20 };
static request BATCH[BATCH_MAX];
static int NBATCH;
static char *ARENA;
static size_t ARENA_USED;

static void out_reserve(conn *c, uint32_t more) {
  if (c->out_len + more <= c->out_cap) return;
  uint32_t cap = c->out_cap ? c->out_cap : 4096;
  while (cap < c->out_len + more) cap *= 2;
  c->out = realloc(c->out, cap);
  c->out_cap = cap;
}
static void out_append(conn *c, const char *p, uint32_t n) {
  out_reserve(c, n);
  memcpy(c->out + c->out_len, p, n);
  c->out_len += n;
}

static void reply(conn *c, int status, const char *body, int len) {
  const char *text = status == 200 ? "OK" : status == 201 ? "Created" : status == 400 ? "Bad Request"
                   : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : status == 503 ? "Service Unavailable"
                   : "Internal Server Error";
  char head[160];
  int hl = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n%s\r\n",
                    status, text, len, c->close_after ? "Connection: close\r\n" : "");
  out_append(c, head, (uint32_t)hl);
  out_append(c, body, (uint32_t)len);
}
#define REPLY_CONST(c, status, s) reply(c, status, s, (int)sizeof s - 1)

/* ---- HTTP/1.1 ---- */
static int ieq(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return 0;
  }
  return 1;
}
static int64_t parse_id(const char *s, int n) {
  if (n < 1 || n > 15) return 0;
  int64_t v = 0;
  for (int i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return 0; v = v * 10 + (s[i] - '0'); }
  return v;
}
static void route_of(const char *m, int ml, const char *p, int pl, request *r) {
  const char *q = memchr(p, '?', (size_t)pl);
  if (q) pl = (int)(q - p);
  r->route = R_NOTFOUND; r->post_id = 0;
  int get = ml == 3 && memcmp(m, "GET", 3) == 0, post = ml == 4 && memcmp(m, "POST", 4) == 0;
  if (pl == 7 && memcmp(p, "/health", 7) == 0) { if (get) r->route = R_HEALTH; return; }
  if (pl == 5 && memcmp(p, "/feed", 5) == 0) { if (get) r->route = R_FEED; return; }
  if (pl == 6 && memcmp(p, "/posts", 6) == 0) { if (post) r->route = R_CREATE; return; }
  if (pl > 7 && memcmp(p, "/posts/", 7) == 0) {
    const char *id = p + 7;
    int il = pl - 7;
    if (il > 5 && memcmp(id + il - 5, "/like", 5) == 0) {
      if (post && !memchr(id, '/', (size_t)(il - 5))) { r->route = R_LIKE; r->post_id = parse_id(id, il - 5); }
      return;
    }
    if (get && !memchr(id, '/', (size_t)il)) { r->route = R_POST; r->post_id = parse_id(id, il); }
  }
}
static char *arena_copy(const char *p, int n) {
  if (ARENA_USED + (size_t)n + 1 > ARENA_SIZE) return 0;
  char *d = ARENA + ARENA_USED;
  memcpy(d, p, (size_t)n); d[n] = 0;
  ARENA_USED += (size_t)n + 1;
  return d;
}

/* Parses one request from c->in. Returns bytes consumed and fills *r, 0 if incomplete, -1 if the
 * connection must close (malformed or too big). Sends 100 Continue when asked and the body is pending. */
static int parse_request(conn *c, request *r) {
  const char *b = c->in, *end = b + c->in_len;
  const char *he = 0;
  for (const char *p = b; p + 3 < end; p++)
    if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') { he = p; break; }
  if (!he) return c->in_len > 16384 ? -1 : 0;
  const char *sp1 = memchr(b, ' ', (size_t)(he - b));
  if (!sp1) return -1;
  const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(he - sp1 - 1));
  if (!sp2) return -1;
  const char *eol = memchr(b, '\r', (size_t)(he + 2 - b));
  int http10 = eol - sp2 - 1 == 8 && memcmp(sp2 + 1, "HTTP/1.0", 8) == 0;
  long clen = 0;
  int conn_close = http10, conn_keep = 0, expect = 0;
  const char *auth = 0; int alen = 0;
  for (const char *l = eol + 2; l < he + 2;) {
    const char *le = memchr(l, '\r', (size_t)(he + 2 - l));
    const char *colon = memchr(l, ':', (size_t)(le - l));
    if (colon) {
      int nl = (int)(colon - l);
      const char *v = colon + 1;
      while (v < le && (*v == ' ' || *v == '\t')) v++;
      const char *ve = le;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      int vl = (int)(ve - v);
      if (nl == 14 && ieq(l, "content-length", 14)) { clen = 0; for (int i = 0; i < vl; i++) { if (v[i] < '0' || v[i] > '9' || clen > 1 << 20) return -1; clen = clen * 10 + (v[i] - '0'); } }
      else if (nl == 13 && ieq(l, "authorization", 13)) { auth = v; alen = vl; }
      else if (nl == 10 && ieq(l, "connection", 10)) { if (vl == 5 && ieq(v, "close", 5)) conn_close = 1; else if (vl == 10 && ieq(v, "keep-alive", 10)) conn_keep = 1; }
      else if (nl == 6 && ieq(l, "expect", 6) && vl == 12 && ieq(v, "100-continue", 12)) expect = 1;
    }
    l = le + 2;
  }
  if (clen > 65536) return -1;
  size_t total = (size_t)(he + 4 - b) + (size_t)clen;
  if ((size_t)c->in_len < total) {
    if (expect && !c->sent_continue) { c->sent_continue = 1; static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n"; out_append(c, cont, sizeof cont - 1); }
    return 0;
  }
  route_of(b, (int)(sp1 - b), sp1 + 1, (int)(sp2 - sp1 - 1), r);
  r->auth = auth ? arena_copy(auth, alen) : 0; r->auth_len = alen;
  r->body = arena_copy(he + 4, (int)clen); r->body_len = (int)clen;
  if ((auth && !r->auth) || !r->body) return -2;  /* arena full: run the batch first */
  c->close_after = conn_close && !(http10 && conn_keep);
  return (int)total;
}

/* ---- the http_requests virtual table (eponymous-only: no CREATE, nothing in the schema) ---- */
typedef struct { sqlite3_vtab_cursor base; int i; } vcur;
static int vt_connect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **out, char **err) {
  int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(slot INTEGER, route TEXT, post_id INTEGER, auth TEXT, body TEXT)");
  if (rc) return rc;
  *out = sqlite3_malloc(sizeof **out);
  memset(*out, 0, sizeof **out);
  return SQLITE_OK;
}
static int vt_disconnect(sqlite3_vtab *v) { sqlite3_free(v); return SQLITE_OK; }
static int vt_best_index(sqlite3_vtab *v, sqlite3_index_info *ii) { ii->estimatedCost = BATCH_MAX; ii->estimatedRows = BATCH_MAX; return SQLITE_OK; }
static int vt_open(sqlite3_vtab *v, sqlite3_vtab_cursor **out) {
  vcur *c = sqlite3_malloc(sizeof *c);
  memset(c, 0, sizeof *c);
  *out = &c->base;
  return SQLITE_OK;
}
static int vt_close(sqlite3_vtab_cursor *c) { sqlite3_free(c); return SQLITE_OK; }
static int vt_filter(sqlite3_vtab_cursor *c, int idx, const char *s, int argc, sqlite3_value **argv) { ((vcur *)c)->i = 0; return SQLITE_OK; }
static int vt_next(sqlite3_vtab_cursor *c) { ((vcur *)c)->i++; return SQLITE_OK; }
static int vt_eof(sqlite3_vtab_cursor *c) { return ((vcur *)c)->i >= NBATCH; }
static int vt_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int col) {
  int i = ((vcur *)cur)->i;
  request *r = &BATCH[i];
  switch (col) {
  case 0: sqlite3_result_int(ctx, i); break;
  case 1: sqlite3_result_text(ctx, ROUTE_NAME[r->route], -1, SQLITE_STATIC); break;
  case 2: if (r->post_id > 0) sqlite3_result_int64(ctx, r->post_id); else sqlite3_result_null(ctx); break;
  case 3: if (r->auth) sqlite3_result_text(ctx, r->auth, r->auth_len, SQLITE_STATIC); else sqlite3_result_null(ctx); break;
  case 4: sqlite3_result_text(ctx, r->body, r->body_len, SQLITE_STATIC); break;
  }
  return SQLITE_OK;
}
static int vt_rowid(sqlite3_vtab_cursor *c, sqlite_int64 *r) { *r = ((vcur *)c)->i; return SQLITE_OK; }
static sqlite3_module HTTP_REQUESTS = {
  .xCreate = 0, .xConnect = vt_connect, .xBestIndex = vt_best_index, .xDisconnect = vt_disconnect,
  .xDestroy = vt_disconnect, .xOpen = vt_open, .xClose = vt_close, .xFilter = vt_filter, .xNext = vt_next,
  .xEof = vt_eof, .xColumn = vt_column, .xRowid = vt_rowid,
};

/* ---- SQL functions ---- */
static time_t STARTED;
static sqlite3_stmt *INS_LIKE, *INS_POST;

/* http_reply(slot, 'NNN body'): queues the response. It leaves only after the statement commits. */
static void fn_http_reply(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  int i = sqlite3_value_int(argv[0]);
  const char *res = (const char *)sqlite3_value_text(argv[1]);
  int n = sqlite3_value_bytes(argv[1]);
  if (i < 0 || i >= NBATCH) { sqlite3_result_error(ctx, "bad slot", -1); return; }
  request *r = &BATCH[i];
  r->out_mark = r->c->out_len;
  if (!res || n < 5 || res[3] != ' ') REPLY_CONST(r->c, 500, "{\"error\":\"internal server error\"}");
  else reply(r->c, (res[0] - '0') * 100 + (res[1] - '0') * 10 + (res[2] - '0'), res + 4, n - 4);
  r->answered = 1;
  sqlite3_result_null(ctx);
}
static void fn_jwt(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  char out[512];
  const char *a = (const char *)sqlite3_value_text(argv[0]);
  int n = jwt_check(a, a ? sqlite3_value_bytes(argv[0]) : 0, out, sizeof out);
  sqlite3_result_text(ctx, out, n, SQLITE_TRANSIENT);
}
static void fn_uptime(sqlite3_context *ctx, int argc, sqlite3_value **argv) { sqlite3_result_int64(ctx, (sqlite3_int64)(time(0) - STARTED)); }
/* write_like(user_id, post_id) -> 1 if a row was added, 0 if it was already there */
static void fn_write_like(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  sqlite3_bind_value(INS_LIKE, 1, argv[0]);
  sqlite3_bind_value(INS_LIKE, 2, argv[1]);
  int rc = sqlite3_step(INS_LIKE);
  int added = sqlite3_changes(sqlite3_context_db_handle(ctx));
  sqlite3_reset(INS_LIKE);
  if (rc != SQLITE_DONE) { sqlite3_result_error(ctx, sqlite3_errstr(rc), -1); return; }
  sqlite3_result_int(ctx, added);
}
/* write_post(user_id, body, author) -> the response JSON, built by the INSERT's RETURNING clause */
static void fn_write_post(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  for (int i = 0; i < 3; i++) sqlite3_bind_value(INS_POST, i + 1, argv[i]);
  int rc = sqlite3_step(INS_POST);
  if (rc != SQLITE_ROW) { sqlite3_reset(INS_POST); sqlite3_result_error(ctx, sqlite3_errstr(rc), -1); return; }
  sqlite3_result_value(ctx, sqlite3_column_value(INS_POST, 0));
  rc = sqlite3_step(INS_POST);
  sqlite3_reset(INS_POST);
  if (rc != SQLITE_DONE) sqlite3_result_error(ctx, sqlite3_errstr(rc), -1);
}

/* ---- setup and one batch ---- */
static sqlite3 *DB;
static sqlite3_stmt *SERVER;

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *s = malloc((size_t)n + 1);
  if (fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return 0; }
  s[n] = 0;
  fclose(f);
  return s;
}
static int app_init(const char *db_path, const char *sql_path, const char *secret) {
  STARTED = time(0);
  hmac_init(secret);
  ARENA = malloc(ARENA_SIZE);
  char *sql = read_file(sql_path);
  if (!sql) { fprintf(stderr, "cannot read %s\n", sql_path); return 1; }
  if (sqlite3_open_v2(db_path, &DB, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) goto fail;
  if (sqlite3_exec(DB, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;"
                       "PRAGMA cache_size=-65536; PRAGMA mmap_size=1073741824; PRAGMA temp_store=MEMORY;", 0, 0, 0)) goto fail;
  if (sqlite3_create_module(DB, "http_requests", &HTTP_REQUESTS, 0)) goto fail;
  if (sqlite3_create_function(DB, "http_reply", 2, SQLITE_UTF8, 0, fn_http_reply, 0, 0)) goto fail;
  if (sqlite3_create_function(DB, "jwt", 1, SQLITE_UTF8, 0, fn_jwt, 0, 0)) goto fail;
  if (sqlite3_create_function(DB, "uptime", 0, SQLITE_UTF8, 0, fn_uptime, 0, 0)) goto fail;
  if (sqlite3_create_function(DB, "write_like", 2, SQLITE_UTF8, 0, fn_write_like, 0, 0)) goto fail;
  if (sqlite3_create_function(DB, "write_post", 3, SQLITE_UTF8, 0, fn_write_post, 0, 0)) goto fail;
  if (sqlite3_prepare_v3(DB, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
                         -1, SQLITE_PREPARE_PERSISTENT, &INS_LIKE, 0)) goto fail;
  if (sqlite3_prepare_v3(DB, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING json_object('post', json_object("
                             "'id', id, 'body', body, 'created_at', created_at, 'author', ?3, 'like_count', 0))",
                         -1, SQLITE_PREPARE_PERSISTENT, &INS_POST, 0)) goto fail;
  if (sqlite3_prepare_v3(DB, sql, -1, SQLITE_PREPARE_PERSISTENT, &SERVER, 0)) goto fail;
  free(sql);
  return 0;
fail:
  fprintf(stderr, "sqlite: %s\n", DB ? sqlite3_errmsg(DB) : "open failed");
  return 1;
}

/* Runs the statement once over BATCH. On success every reply is committed and queued in its conn's out
 * buffer; on failure the transaction is rolled back and every request gets a 500. */
static void run_batch(void) {
  if (!NBATCH) return;
  int rc;
  while ((rc = sqlite3_step(SERVER)) == SQLITE_ROW) {}
  sqlite3_reset(SERVER);
  int ok = rc == SQLITE_DONE && sqlite3_get_autocommit(DB);
  if (!ok) {
    fprintf(stderr, "batch of %d failed: %s\n", NBATCH, sqlite3_errmsg(DB));
    if (!sqlite3_get_autocommit(DB)) sqlite3_exec(DB, "ROLLBACK", 0, 0, 0);
  }
  for (int i = 0; i < NBATCH; i++) {
    request *r = &BATCH[i];
    if (r->c->gen != r->gen) continue;
    if (!ok || !r->answered) {
      if (r->answered) r->c->out_len = r->out_mark;
      REPLY_CONST(r->c, 500, "{\"error\":\"internal server error\"}");
    }
  }
  NBATCH = 0;
  ARENA_USED = 0;
}

/* Parses the next request of c into the batch. 1 queued, 0 nothing complete yet, -1 close the connection,
 * 2 the arena is full: run the batch and call again. The caller also runs the batch when it is full. */
static int queue_request(conn *c) {
  if (c->busy || NBATCH >= BATCH_MAX) return 0;
  request *r = &BATCH[NBATCH];
  memset(r, 0, sizeof *r);
  int n = parse_request(c, r);
  if (n == -2) return 2;
  if (n <= 0) return n;
  memmove(c->in, c->in + n, c->in_len - (uint32_t)n);
  c->in_len -= (uint32_t)n;
  c->sent_continue = 0;
  r->c = c; r->gen = c->gen;
  c->busy = 1;
  NBATCH++;
  return 1;
}
