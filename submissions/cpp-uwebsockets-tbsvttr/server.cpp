#define FIO_STR
#define FIO_TIME
#define FIO_SHA2
#define FIO_NO_TLS
#include "fio-stl.h"
#include "sqlite3.h"
#include "App.h"
#include <sys/resource.h>

using Response = uWS::HttpResponse<false>;
using View = std::string_view;
static fio_str_info_s info(View text) {
  return FIO_STR_INFO2(const_cast<char *>(text.data()), text.size());
}
#include "unicode.h"

static sqlite3 *db;
static sqlite3_stmt *feed, *post, *create, *like, *exists, *health, *valid, *body;
static const char *secret;
static int64_t started;
static char *out;

static sqlite3_stmt *prepare(const char *sql) {
  sqlite3_stmt *s = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
    fprintf(stderr, "%s\n", sqlite3_errmsg(db));
    exit(1);
  }
  return s;
}

static fio_str_info_s column(sqlite3_stmt *s, int n) {
  const char *text = (const char *)sqlite3_column_text(s, n);
  return FIO_STR_INFO2((char *)text, (size_t)sqlite3_column_bytes(s, n));
}

static int strict_json(fio_str_info_s s) {
  if (!s.len || memchr(s.buf, 0, s.len) || !utf8_valid(s)) return 0;
  sqlite3_bind_text(valid, 1, s.buf, (int)s.len, SQLITE_STATIC);
  int ok = sqlite3_step(valid) == SQLITE_ROW && sqlite3_column_int(valid, 0);
  sqlite3_reset(valid);
  sqlite3_clear_bindings(valid);
  return ok;
}

static sqlite3_int64 positive_id(fio_str_info_s s) {
  sqlite3_int64 n = 0;
  if (!s.len) return 0;
  for (size_t i = 0; i < s.len; ++i) {
    if (s.buf[i] < '0' || s.buf[i] > '9' || n > 900719925474099LL) return 0;
    n = n * 10 + s.buf[i] - '0';
  }
  return n <= 9007199254740991LL ? n : 0;
}

#include "auth.h"

#define WRITE(s) (out = fio_bstr_write(out, (s), sizeof(s) - 1))
static void number(sqlite3_int64 n) { out = fio_bstr_write_i(out, n); }
static void string(fio_str_info_s s) {
  WRITE("\"");
  out = fio_bstr_write_escape(out, s.buf, s.len);
  WRITE("\"");
}

static void row(sqlite3_stmt *s) {
  WRITE("{\"id\":"); number(sqlite3_column_int64(s, 0));
  WRITE(",\"body\":"); string(column(s, 1));
  WRITE(",\"created_at\":"); string(column(s, 2));
  WRITE(",\"author\":"); string(column(s, 3));
  WRITE(",\"like_count\":"); number(sqlite3_column_int64(s, 4));
  WRITE("}");
}

static void respond(Response *h, int status) {
  const char *text = status == 200 ? "200 OK" : status == 201 ? "201 Created" :
    status == 400 ? "400 Bad Request" : status == 401 ? "401 Unauthorized" :
    status == 404 ? "404 Not Found" : status == 413 ? "413 Payload Too Large" :
    status == 503 ? "503 Service Unavailable" : "500 Internal Server Error";
  h->writeStatus(text)->writeHeader("Content-Type", "application/json");
  h->end(View(out, fio_bstr_len(out)), status == 413); // Copies bytes queued by backpressure.
}

static void error(Response *h, int status, const char *message) {
  out = fio_bstr_len_set(out, 0);
  WRITE("{\"error\":"); string(FIO_STR_INFO1((char *)message)); WRITE("}");
  respond(h, status);
}

static int equal(fio_str_info_s s, const char *text) {
  size_t n = strlen(text);
  return s.len == n && !memcmp(s.buf, text, n);
}

static void read_posts(Response *h, sqlite3_stmt *s, int single) {
  WRITE("{\""); WRITE("post");
  if (single) WRITE("\":"); else WRITE("s\":[");
  int count = 0, rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    if (count++) WRITE(",");
    row(s);
  }
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) return error(h, 500, "internal server error");
  if (single && !count) return error(h, 404, "post not found");
  if (!single) WRITE("]");
  WRITE("}");
  respond(h, 200);
}

static void write_post(Response *h, sqlite3_int64 user_id, fio_str_info_s username, fio_str_info_s input) {
  if (!strict_json(input)) return error(h, 400, "malformed JSON body");
  sqlite3_bind_text(body, 1, input.buf, (int)input.len, SQLITE_STATIC);
  int rc = sqlite3_step(body);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(body);
    sqlite3_clear_bindings(body);
    return error(h, 500, "internal server error");
  }
  fio_str_info_s text = column(body, 0);
  int count = equal(column(body, 1), "text") ? utf8_trim(&text) : 0;
  if (count <= 0) error(h, 400, "body is required");
  else if (count > 500) error(h, 400, "body must be at most 500 characters");
  else {
    sqlite3_bind_int64(create, 1, user_id);
    sqlite3_bind_text(create, 2, text.buf, (int)text.len, SQLITE_STATIC);
    rc = sqlite3_step(create);
    if (rc == SQLITE_ROW) {
      WRITE("{\"post\":{\"id\":"); number(sqlite3_column_int64(create, 0));
      WRITE(",\"body\":"); string(text);
      WRITE(",\"created_at\":"); string(column(create, 1));
      WRITE(",\"author\":"); string(username);
      WRITE(",\"like_count\":0}}");
      rc = sqlite3_step(create); // RETURNING must reach DONE before acknowledging the commit.
    }
    sqlite3_reset(create);
    sqlite3_clear_bindings(create);
    if (rc == SQLITE_DONE) respond(h, 201);
    else error(h, 500, "internal server error");
  }
  sqlite3_reset(body);
  sqlite3_clear_bindings(body);
}

static void write_like(Response *h, sqlite3_int64 user_id, sqlite3_int64 id) {
  sqlite3_bind_int64(like, 1, user_id);
  sqlite3_bind_int64(like, 2, id);
  int rc = sqlite3_step(like), inserted = sqlite3_changes(db);
  sqlite3_reset(like);
  if (rc != SQLITE_DONE) return error(h, 500, "internal server error");
  if (!inserted) {
    sqlite3_bind_int64(exists, 1, id);
    rc = sqlite3_step(exists);
    sqlite3_reset(exists);
    if (rc == SQLITE_DONE) return error(h, 404, "post not found");
    if (rc != SQLITE_ROW) return error(h, 500, "internal server error");
  }
  WRITE("{\"liked\":true,\"already_liked\":");
  if (inserted) WRITE("false"); else WRITE("true");
  WRITE(",\"post_id\":"); number(id); WRITE("}");
  respond(h, inserted ? 201 : 200);
}

static void on_http(Response *h, View method_text, View path_text, View authorization, View input) {
  out = fio_bstr_len_set(out, 0);
  fio_str_info_s path = info(path_text), method = info(method_text);
  int get = equal(method, "GET"), submit = equal(method, "POST");
  if (get && equal(path, "/health")) {
    int ok = sqlite3_step(health) == SQLITE_ROW;
    sqlite3_reset(health);
    if (ok) {
      WRITE("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
      number((fio_time_milli() - started) / 1000); WRITE("}");
    } else {
      WRITE("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
      string(FIO_STR_INFO1((char *)sqlite3_errmsg(db))); WRITE("}");
    }
    return respond(h, ok ? 200 : 503);
  }
  if (get && equal(path, "/feed")) return read_posts(h, feed, 0);
  int create_route = submit && equal(path, "/posts");
  int like_route = submit && path.len > 12 && !memcmp(path.buf + path.len - 5, "/like", 5);
  fio_str_info_s id_text = {0};
  if ((get || like_route) && path.len > 7 && !memcmp(path.buf, "/posts/", 7))
    id_text = FIO_STR_INFO2(path.buf + 7, path.len - 7 - (like_route ? 5 : 0));
  if (id_text.len && memchr(id_text.buf, '/', id_text.len)) id_text.len = 0;
  if (!create_route && !id_text.len) return error(h, 404, "not found");
  sqlite3_int64 user_id = 0;
  fio_str_info_s username = {0};
  if (submit) {
    const char *message = authenticate(info(authorization), secret, &user_id, &username);
    if (message) { auth_reset(); return error(h, 401, message); }
  }
  sqlite3_int64 id = positive_id(id_text);
  if (create_route) write_post(h, user_id, username, info(input));
  else if (!id) error(h, 400, "invalid post id");
  else if (like_route) write_like(h, user_id, id);
  else { sqlite3_bind_int64(post, 1, id); read_posts(h, post, 1); }
  if (submit) auth_reset();
}

int main(void) {
  const char *path = getenv("SQLITE_PATH"), *host = getenv("HOST"), *port = getenv("PORT");
  secret = getenv("JWT_SECRET");
  if (!path || !*path || !secret || !*secret) {
    fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1;
  }
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK ||
      sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"
        "PRAGMA cache_size=-65536; PRAGMA mmap_size=268435456;", NULL, NULL, NULL) != SQLITE_OK) {
    fprintf(stderr, "%s\n", sqlite3_errmsg(db)); return 1;
  }
#define POST_SQL "SELECT p.id,p.body,p.created_at,u.username," \
  "(SELECT count(*) FROM likes WHERE post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id"
  feed = prepare(POST_SQL " ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
  post = prepare(POST_SQL " WHERE p.id=?");
  create = prepare("INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at");
  like = prepare("INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS"
    "(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING");
  exists = prepare("SELECT 1 FROM posts WHERE id=?");
  health = prepare("SELECT 1");
  valid = prepare("SELECT json_valid(?)");
  body = prepare("SELECT json_extract(?1,'$.body'),json_type(?1,'$.body')");
  init_auth();
  started = fio_time_milli();
  struct rlimit limit;
  if (!getrlimit(RLIMIT_NOFILE, &limit)) {
    limit.rlim_cur = limit.rlim_max;
    setrlimit(RLIMIT_NOFILE, &limit);
  }
  uWS::App app;
  app.any("/*", [](Response *res, uWS::HttpRequest *req) {
    View method = req->getCaseSensitiveMethod(), path = req->getUrl();
    View auth = req->getHeader("authorization");
    if (method == "POST" && path == "/posts") {
      // Request views expire on return; own data until the body is complete.
      res->onAborted([] {});
      res->onData([res, auth = std::string(auth), data = std::string{}](View chunk, bool last) mutable {
        if (res->hasResponded()) return;
        if (chunk.size() > 16384 - data.size()) {
          res->cork([res] { error(res, 413, "request body too large"); });
          return;
        }
        data.append(chunk);
        if (last) res->cork([&] { on_http(res, "POST", "/posts", auth, data); });
      });
    } else on_http(res, method, path, auth, {});
  });
  bool listening = false;
  app.listen(host ? host : "127.0.0.1", port ? atoi(port) : 3000,
             [&](auto *socket) { listening = socket != nullptr; });
  if (!listening) { fprintf(stderr, "unable to listen\n"); return 1; }
  app.run();
  for (sqlite3_stmt *s; (s = sqlite3_next_stmt(db, NULL));) sqlite3_finalize(s);
  sqlite3_close(db);
  fio_bstr_free(out);
  return 0;
}
