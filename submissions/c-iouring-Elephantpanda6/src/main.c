#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "sqlite_layer.h"
#include "uring_net.h"

#define MAX_ANSWERS 2048
#define PRE_CONNS 320000

#define DEFER_CAP 4096
#define CONN_SET_SIZE 8192

#define FEED_JSON_CAP (4u * 1024u * 1024u)
#define POST_JSON_CAP (256u * 1024u)

enum defer_kind {
  DEFER_CREATE = 1,
  DEFER_LIKE = 2,
  DEFER_DB_ERROR = 3
};

struct deferred_resp {
  uint32_t conn_id;
  uint8_t kind;
  uint8_t first_for_conn;
  uint8_t close_after;
  uint8_t already_liked;
  int64_t post_id;
  const uint8_t *body;
  size_t body_len;
  const uint8_t *author;
  size_t author_len;
  char created[40];
  size_t created_len;
};

static struct net_ctx *g_ctx;
static int64_t g_start_ns;
static volatile sig_atomic_t g_stop;

static int g_writes_allowed;
static int g_post_commit_phase;

static uint8_t g_feed_json[FEED_JSON_CAP];
static uint8_t g_post_json[POST_JSON_CAP];
static uint8_t g_uname[1024];
static uint8_t g_trimmed[4096];
static char g_created[64];

static struct deferred_resp g_defers[DEFER_CAP];
static int g_defer_n;
static uint32_t g_defer_keys[CONN_SET_SIZE];
static uint32_t g_blocked_keys[CONN_SET_SIZE];

static uint32_t g_held_conns[DEFER_CAP];
static int g_held_n = 0;

static uint32_t conn_hash(uint32_t id) {
  id ^= id >> 16;
  id *= 0x7feb352dU;
  id ^= id >> 15;
  id *= 0x846ca68bU;
  id ^= id >> 16;
  return id & (CONN_SET_SIZE - 1);
}

static void conn_set_reset(uint32_t *set) {
  memset(set, 0xFF, CONN_SET_SIZE * sizeof(set[0]));
}

static int conn_set_has(const uint32_t *set, uint32_t id) {
  uint32_t i = conn_hash(id);
  for (;;) {
    uint32_t k = set[i];
    if (k == UINT32_MAX) return 0;
    if (k == id) return 1;
    i = (i + 1) & (CONN_SET_SIZE - 1);
  }
}

static void conn_set_add(uint32_t *set, uint32_t id) {
  uint32_t i = conn_hash(id);
  for (;;) {
    uint32_t k = set[i];
    if (k == UINT32_MAX) {
      set[i] = id;
      return;
    }
    if (k == id) return;
    i = (i + 1) & (CONN_SET_SIZE - 1);
  }
}

static int64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int64_t uptime_s(void) {
  return (monotonic_ns() - g_start_ns) / 1000000000LL;
}

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void install_signals(void) {
  struct sigaction sa;

  signal(SIGPIPE, SIG_IGN);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

static uint8_t *resp_alloc(struct net_ctx *ctx, size_t cap) {
  if (cap < 256) cap = 256;
  return net_batch_alloc(ctx, cap);
}

static int send_owned(struct net_ctx *ctx, uint32_t conn_id, const uint8_t *data,
                      size_t len, int close_after) {
  if (net_conn_is_dead(ctx, conn_id)) return -1;
  if (net_conn_send(ctx, conn_id, data, len, 0, close_after) < 0) return -1;
  return 0;
}

static int reply_error(struct net_ctx *ctx, uint32_t conn_id, int status,
                       const char *msg, int keep_alive) {
  uint8_t *dst = resp_alloc(ctx, 512);
  size_t n;
  if (!dst) {
    net_conn_close(ctx, conn_id);
    return -1;
  }
  n = resp_write_error(dst, 512, status, msg, keep_alive);
  return send_owned(ctx, conn_id, dst, n, !keep_alive);
}

static int reply_auth_error(struct net_ctx *ctx, uint32_t conn_id, enum jwt_rc jr,
                            int keep_alive) {
  const char *msg;
  switch (jr) {
    case JWT_ERR_MISSING: msg = "missing bearer token"; break;
    case JWT_ERR_PAYLOAD: msg = "invalid token payload"; break;
    case JWT_ERR_INVALID:
    default: msg = "invalid or expired token"; break;
  }
  return reply_error(ctx, conn_id, 401, msg, keep_alive);
}

static enum jwt_rc verify_auth(const struct http_req *req, int64_t now_sec,
                               int64_t *uid_out, size_t *ulen_out) {
  if (!req->auth) return JWT_ERR_MISSING;
  return jwt_verify_hs256(req->auth, req->auth_len, now_sec, uid_out,
                          g_uname, sizeof(g_uname), ulen_out);
}

static int defer_push(struct net_ctx *ctx, const struct deferred_resp *d) {
  int first;
  if (g_defer_n >= DEFER_CAP) {
    net_conn_close(ctx, d->conn_id);
    return -1;
  }
  first = !conn_set_has(g_defer_keys, d->conn_id);
  g_defers[g_defer_n] = *d;
  g_defers[g_defer_n].first_for_conn = (uint8_t)first;
  if (first) conn_set_add(g_defer_keys, d->conn_id);
  g_defer_n++;
  return 0;
}

static int handle_health(struct net_ctx *ctx, uint32_t conn_id,
                         const struct http_req *req) {
  uint8_t *dst;
  char err[256];
  size_t n;
  int ok;

  err[0] = '\0';
  ok = (db_health(err, sizeof(err)) == 0);

  dst = resp_alloc(ctx, 2048);
  if (!dst) {
    net_conn_close(ctx, conn_id);
    return -1;
  }
  n = resp_write_health(dst, 2048, ok, uptime_s(), ok ? NULL : err,
                        req->keep_alive);
  return send_owned(ctx, conn_id, dst, n, !req->keep_alive);
}

static int handle_feed(struct net_ctx *ctx, uint32_t conn_id,
                       const struct http_req *req) {
  size_t jlen = 0;
  size_t cap;
  uint8_t *dst;
  size_t n;

  if (db_read_feed(g_feed_json, sizeof(g_feed_json), &jlen) != 0)
    return reply_error(ctx, conn_id, 500, "internal server error",
                       req->keep_alive);

  cap = jlen + 1024;
  dst = resp_alloc(ctx, cap);
  if (!dst) {
    net_conn_close(ctx, conn_id);
    return -1;
  }
  n = resp_write_json_body(dst, cap, 200, "OK", g_feed_json, jlen,
                           req->keep_alive);
  return send_owned(ctx, conn_id, dst, n, !req->keep_alive);
}

static int handle_post_get(struct net_ctx *ctx, uint32_t conn_id,
                           const struct http_req *req) {
  size_t jlen = 0;
  size_t cap;
  uint8_t *dst;
  size_t n;
  int rc;

  if (req->post_id <= 0)
    return reply_error(ctx, conn_id, 400, "invalid post id", req->keep_alive);

  rc = db_read_post(req->post_id, g_post_json, sizeof(g_post_json), &jlen);
  if (rc == 1)
    return reply_error(ctx, conn_id, 404, "post not found", req->keep_alive);
  if (rc != 0)
    return reply_error(ctx, conn_id, 500, "internal server error",
                       req->keep_alive);

  cap = jlen + 1024;
  dst = resp_alloc(ctx, cap);
  if (!dst) {
    net_conn_close(ctx, conn_id);
    return -1;
  }
  n = resp_write_json_body(dst, cap, 200, "OK", g_post_json, jlen,
                           req->keep_alive);
  return send_owned(ctx, conn_id, dst, n, !req->keep_alive);
}

static int handle_post_create(struct net_ctx *ctx, uint32_t conn_id,
                              const struct http_req *req, int64_t now_sec) {
  int64_t uid = 0;
  size_t ulen = 0;
  enum jwt_rc jr;
  enum json_body_rc jbr;
  size_t tlen = 0;
  const uint8_t *body = req->body ? req->body : (const uint8_t *)"";
  uint8_t *body_copy;
  uint8_t *author_copy;
  int64_t id = 0;
  size_t created_len = 0;
  struct deferred_resp d;

  jr = verify_auth(req, now_sec, &uid, &ulen);
  if (jr != JWT_OK) return reply_auth_error(ctx, conn_id, jr, req->keep_alive);

  jbr = json_parse_post_body(body, req->body_len, g_trimmed,
                             sizeof(g_trimmed), &tlen);
  if (jbr != JSON_BODY_OK) {
    switch (jbr) {
      case JSON_BODY_MALFORMED:
        return reply_error(ctx, conn_id, 400, "malformed JSON body",
                           req->keep_alive);
      case JSON_BODY_REQUIRED:
        return reply_error(ctx, conn_id, 400, "body is required",
                           req->keep_alive);
      case JSON_BODY_TOO_LONG:
      default:
        return reply_error(ctx, conn_id, 400,
                           "body must be at most 500 characters",
                           req->keep_alive);
    }
  }

  body_copy = net_batch_alloc(ctx, tlen ? tlen : 1);
  author_copy = net_batch_alloc(ctx, ulen ? ulen : 1);
  if (!body_copy || !author_copy) {
    net_conn_close(ctx, conn_id);
    return -1;
  }
  if (tlen) memcpy(body_copy, g_trimmed, tlen);
  if (ulen) memcpy(author_copy, g_uname, ulen);

  if (db_insert_post(uid, g_trimmed, tlen, &id, g_created, sizeof(g_created),
                     &created_len) != 0) {
    memset(&d, 0, sizeof(d));
    d.conn_id = conn_id;
    d.kind = DEFER_DB_ERROR;
    d.close_after = 1;
    return defer_push(ctx, &d);
  }

  memset(&d, 0, sizeof(d));
  d.conn_id = conn_id;
  d.kind = DEFER_CREATE;
  d.close_after = (uint8_t)!req->keep_alive;
  d.post_id = id;
  d.body = body_copy;
  d.body_len = tlen;
  d.author = author_copy;
  d.author_len = ulen;
  if (created_len >= sizeof(d.created)) created_len = sizeof(d.created) - 1;
  memcpy(d.created, g_created, created_len);
  d.created_len = created_len;
  return defer_push(ctx, &d);
}

static int handle_post_like(struct net_ctx *ctx, uint32_t conn_id,
                            const struct http_req *req, int64_t now_sec) {
  int64_t uid = 0;
  size_t ulen = 0;
  enum jwt_rc jr;
  int already = 0;
  int rc;
  struct deferred_resp d;

  jr = verify_auth(req, now_sec, &uid, &ulen);
  if (jr != JWT_OK) return reply_auth_error(ctx, conn_id, jr, req->keep_alive);

  if (req->post_id <= 0)
    return reply_error(ctx, conn_id, 400, "invalid post id", req->keep_alive);

  rc = db_insert_like(uid, req->post_id, &already);
  if (rc == 1)
    return reply_error(ctx, conn_id, 404, "post not found", req->keep_alive);
  if (rc != 0) {
    memset(&d, 0, sizeof(d));
    d.conn_id = conn_id;
    d.kind = DEFER_DB_ERROR;
    d.close_after = 1;
    return defer_push(ctx, &d);
  }

  memset(&d, 0, sizeof(d));
  d.conn_id = conn_id;
  d.kind = DEFER_LIKE;
  d.close_after = (uint8_t)!req->keep_alive;
  d.already_liked = (uint8_t)already;
  d.post_id = req->post_id;
  return defer_push(ctx, &d);
}

static int dispatch(struct net_ctx *ctx, uint32_t conn_id,
                    const struct http_req *req, int64_t now_sec) {
  switch (req->route) {
    case ROUTE_HEALTH:
      return handle_health(ctx, conn_id, req);
    case ROUTE_FEED:
      return handle_feed(ctx, conn_id, req);
    case ROUTE_POST_GET:
      return handle_post_get(ctx, conn_id, req);
    case ROUTE_POST_CREATE:
      return handle_post_create(ctx, conn_id, req, now_sec);
    case ROUTE_POST_LIKE:
      return handle_post_like(ctx, conn_id, req, now_sec);
    case ROUTE_NOT_FOUND:
    case ROUTE_NONE:
    default:
      return reply_error(ctx, conn_id, 404, "not found", req->keep_alive);
  }
}

static int process_conn(struct net_ctx *ctx, uint32_t conn_id,
                        int writes_allowed) {
  int64_t now_sec;

  if (net_conn_is_dead(ctx, conn_id)) return 0;

  if (!g_post_commit_phase && conn_set_has(g_defer_keys, conn_id)) return 0;

  if (conn_set_has(g_blocked_keys, conn_id)) return 0;

  now_sec = (int64_t)time(NULL);

  for (;;) {
    const uint8_t *buf = NULL;
    size_t len = 0;
    struct http_req req;
    ssize_t parsed;
    int is_write;
    int rc;

    if (g_stop) return 0;

    net_conn_buf(ctx, conn_id, &buf, &len);
    if (len == 0) {
      if (net_conn_is_eof(ctx, conn_id)) net_conn_close(ctx, conn_id);
      return 0;
    }

    parsed = http_parse_req(buf, len, &req);
    if (parsed == 0) return 0;
    if (parsed < 0) {
      uint8_t *dst = resp_alloc(ctx, 512);
      size_t n;
      if (!dst) {
        net_conn_close(ctx, conn_id);
        return 0;
      }
      n = resp_write_bad_request(dst, 512);
      net_conn_send(ctx, conn_id, dst, n, 0, 1);
      return 0;
    }
    if (req.consumed == 0 || req.consumed > len) {
      net_conn_close(ctx, conn_id);
      return 0;
    }

    is_write = (req.route == ROUTE_POST_CREATE ||
                req.route == ROUTE_POST_LIKE);

    if (is_write && !writes_allowed) {
      if (!conn_set_has(g_blocked_keys, conn_id)) {
        conn_set_add(g_blocked_keys, conn_id);
        if (g_held_n < DEFER_CAP) {

          g_held_conns[g_held_n++] = conn_id;
        } else {

          net_conn_close(ctx, conn_id);
        }
      }
      return 0;
    }

    rc = dispatch(ctx, conn_id, &req, now_sec);
    if (rc < 0) return 0;

    net_conn_consume(ctx, conn_id, req.consumed);

    if (!req.keep_alive) return 0;

    if (is_write && !g_post_commit_phase &&
        conn_set_has(g_defer_keys, conn_id))
      return 0;
  }
}

static void on_ready(struct net_ctx *ctx, uint32_t conn_id, void *user) {
  (void)user;
  (void)process_conn(ctx, conn_id, g_writes_allowed && !g_post_commit_phase);
}

static size_t serialize_deferred(struct net_ctx *ctx,
                                 const struct deferred_resp *d,
                                 uint8_t **out) {
  size_t cap;
  uint8_t *dst;
  size_t n;

  switch (d->kind) {
    case DEFER_CREATE:
      cap = 1024 + d->body_len * 6 + d->author_len * 6;
      break;
    case DEFER_LIKE:
      cap = 512;
      break;
    default:
      cap = 512;
      break;
  }

  dst = resp_alloc(ctx, cap);
  if (!dst) {
    *out = NULL;
    return 0;
  }

  if (d->kind == DEFER_CREATE) {
    n = resp_write_post_created(dst, cap, d->post_id, d->body, d->body_len,
                                d->created, d->created_len, d->author,
                                d->author_len, !d->close_after);
  } else if (d->kind == DEFER_LIKE) {
    n = resp_write_like(dst, cap, d->already_liked, d->post_id,
                        !d->close_after);
  } else {
    n = resp_write_error(dst, cap, 500, "internal server error", 0);
  }

  *out = dst;
  return n;
}

static void flush_deferred(struct net_ctx *ctx) {
  int i;

  for (i = 0; i < g_defer_n; i++) {
    struct deferred_resp *d = &g_defers[i];
    uint8_t *dst = NULL;
    size_t n;

    if (net_conn_is_dead(ctx, d->conn_id)) continue;

    n = serialize_deferred(ctx, d, &dst);
    if (!dst || n == 0) {
      net_conn_close(ctx, d->conn_id);
      continue;
    }
    net_conn_send(ctx, d->conn_id, dst, n, 0, d->close_after);
  }

  g_post_commit_phase = 1;
  for (i = 0; i < g_defer_n; i++) {
    struct deferred_resp *d = &g_defers[i];
    if (!d->first_for_conn) continue;
    if (d->close_after || d->kind == DEFER_DB_ERROR) continue;
    if (net_conn_is_dead(ctx, d->conn_id)) continue;
    (void)process_conn(ctx, d->conn_id, 0);
  }
  g_post_commit_phase = 0;
}

static void fail_deferred(struct net_ctx *ctx) {
  static uint8_t err_resp[256];
  size_t n;
  int i;

  n = resp_write_error(err_resp, sizeof(err_resp), 500,
                       "internal server error", 0);
  for (i = 0; i < g_defer_n; i++) {
    uint32_t conn_id = g_defers[i].conn_id;
    if (net_conn_is_dead(ctx, conn_id)) continue;
    net_conn_send(ctx, conn_id, err_resp, n, 1, 1);
  }
}

static const char *env_or(const char *name, const char *fallback) {
  const char *v = getenv(name);
  return (v && *v) ? v : fallback;
}

int main(void) {
  const char *db_path = getenv("SQLITE_PATH");
  const char *jwt_secret = getenv("JWT_SECRET");
  const char *host;
  const char *port_str;
  long port_long;
  char err[512];
  int last_handled = 0;

  install_signals();

  if (!db_path || !*db_path) {
    fprintf(stderr, "missing SQLITE_PATH\n");
    return 1;
  }
  if (!jwt_secret || !*jwt_secret) {
    fprintf(stderr, "missing JWT_SECRET\n");
    return 1;
  }
  host = env_or("HOST", "0.0.0.0");
  port_str = env_or("PORT", "80");
  port_long = strtol(port_str, NULL, 10);
  if (port_long <= 0 || port_long > 65535) {
    fprintf(stderr, "invalid PORT: %s\n", port_str);
    return 1;
  }

  proto_init(jwt_secret);

  err[0] = '\0';
  if (db_open(db_path, err, sizeof(err)) != 0) {
    fprintf(stderr, "db_open failed: %s\n", err);
    return 1;
  }

  g_ctx = net_init(host, (uint16_t)port_long, PRE_CONNS);
  if (!g_ctx) {
    fprintf(stderr, "net_init failed on %s:%s\n", host, port_str);
    db_close();
    return 1;
  }

  g_start_ns = monotonic_ns();

  while (!g_stop) {
    int wait_us;
    int min_cqes;
    int handled;
    int commit_ok;
    int txn_ok;

    db_tick();

    if (net_has_backlog(g_ctx)) {
      wait_us = 0;
      min_cqes = 1;
    } else if (last_handled >= 8) {
      wait_us = 250;
      min_cqes = 64;
    } else {
      wait_us = 5000;
      min_cqes = 1;
    }

    g_defer_n = 0;
    g_post_commit_phase = 0;
    conn_set_reset(g_defer_keys);
    conn_set_reset(g_blocked_keys);

    txn_ok = (db_begin() == 0);
    g_writes_allowed = txn_ok && !db_writes_held();

    if (g_writes_allowed && g_held_n > 0) {
      int hi;
      for (hi = 0; hi < g_held_n; hi++)
        net_conn_requeue(g_ctx, g_held_conns[hi]);
      g_held_n = 0;
    }

    handled = net_poll_pass(g_ctx, wait_us, min_cqes, MAX_ANSWERS, on_ready,
                            NULL);
    last_handled = handled > 0 ? handled : 0;

    commit_ok = (db_commit() == 0);
    if (commit_ok) {
      flush_deferred(g_ctx);
    } else {
      db_rollback();
      fail_deferred(g_ctx);
    }

    net_end_batch(g_ctx);
  }

  net_destroy(g_ctx);
  g_ctx = NULL;
  db_close();
  return 0;
}
