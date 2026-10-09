/* Generic C ABI adapters: HTTP transport, crypto, strings, and SQLite ownership.
 * Routing, SQL, JWT claims, JSON responses and transaction policy are in Fortran. */
#define FIO_STR
#define FIO_SHA2
#define FIO_NO_TLS
#include "fio-stl.h"
#include "sqlite3.h"
#include <microhttpd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>

extern void app_request(const char *, const char *, const char *, const char *, int, void *);
extern int app_flush(void);
struct request {
  struct MHD_Connection *connection;
  char *body;
  size_t length;
  int answered, too_large, status;
  struct MHD_Response *response;
  struct request *next;
};
static struct request *pending;
static unsigned connections, keepalive_limit = 60000;
static volatile sig_atomic_t stopping;
static void stop(int signal) { (void)signal; stopping = 1; }
static void connection_changed(void *cls, struct MHD_Connection *c, void **state,
                               enum MHD_ConnectionNotificationCode reason) {
  (void)cls; (void)c; (void)state;
  if (reason == MHD_CONNECTION_NOTIFY_STARTED) ++connections;
  else if (reason == MHD_CONNECTION_NOTIFY_CLOSED) --connections;
}
static void complete(void *cls, struct MHD_Connection *c, void **state,
                     enum MHD_RequestTerminationCode reason) {
  (void)cls; (void)c; (void)reason;
  struct request *r = *state;
  if (r) { free(r->body); free(r); *state = NULL; }
}
void http_reply(void *opaque, int status, const char *body, int length, int defer) {
  struct request *r = opaque;
  r->answered = 1;
  r->status = status;
  r->response = MHD_create_response_from_buffer((size_t)length, (void *)body, MHD_RESPMEM_MUST_COPY);
  if (!r->response) abort();
  MHD_add_response_header(r->response, "Content-Type", "application/json");
  if (connections >= keepalive_limit)
    MHD_add_response_header(r->response, "Connection", "close");
  if (defer) {
    MHD_suspend_connection(r->connection);
    r->next = pending;
    pending = r;
  } else {
    MHD_queue_response(r->connection, (unsigned)status, r->response);
    MHD_destroy_response(r->response);
    r->response = NULL;
  }
}
static enum MHD_Result access_handler(void *cls, struct MHD_Connection *c,
    const char *url, const char *method, const char *version, const char *upload,
    size_t *length, void **state) {
  (void)cls; (void)version;
  struct request *r = *state;
  if (!r) {
    r = calloc(1, sizeof(*r));
    if (!r) return MHD_NO;
    r->connection = c;
    *state = r;
    return MHD_YES;
  }
  if (r->answered) return MHD_YES;
  if (*length) {
    if (*length > 16384 - r->length) r->too_large = 1;
    if (!r->too_large) {
      char *buffer = realloc(r->body, r->length + *length);
      if (!buffer) return MHD_NO;
      r->body = buffer;
      memcpy(r->body + r->length, upload, *length);
      r->length += *length;
    }
    *length = 0;
    return MHD_YES;
  }
  if (r->too_large) {
    static const char error[] = "{\"error\":\"request body too large\"}";
    http_reply(r, 413, error, sizeof(error)-1, 0);
  } else {
    const char *auth = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization");
    app_request(method, url, auth ? auth : "", r->body ? r->body : "", (int)r->length, r);
  }
  return MHD_YES;
}
int http_run(const char *host, int port) {
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  if (port < 1 || port > 65535 || inet_pton(AF_INET, host, &address.sin_addr) != 1) return 1;
  signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGPIPE, SIG_IGN);
  struct rlimit limit;
  if (!getrlimit(RLIMIT_NOFILE, &limit)) {
    limit.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &limit)) return 1;
    if (limit.rlim_cur < keepalive_limit + 128)
      keepalive_limit = limit.rlim_cur > 128 ? (unsigned)limit.rlim_cur - 128 : 1;
  }
  struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_EPOLL | MHD_ALLOW_SUSPEND_RESUME,
      (uint16_t)port, NULL, NULL, access_handler, NULL,
      MHD_OPTION_SOCK_ADDR, &address,
      MHD_OPTION_CONNECTION_LIMIT, (unsigned)65000,
      MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)16384,
      MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)75,
      MHD_OPTION_NOTIFY_CONNECTION, connection_changed, NULL,
      MHD_OPTION_NOTIFY_COMPLETED, complete, NULL, MHD_OPTION_END);
  if (!daemon) return 1;
  while (!stopping) {
    if (MHD_run_wait(daemon, 100) != MHD_YES) break;
    int ok = app_flush();
    while (pending) {
      struct request *r = pending;
      pending = r->next;
      if (!ok) {
        MHD_destroy_response(r->response);
        static char error[] = "{\"error\":\"internal server error\"}";
        r->response = MHD_create_response_from_buffer(sizeof(error)-1, error, MHD_RESPMEM_PERSISTENT);
        if (!r->response) abort();
        MHD_add_response_header(r->response, "Content-Type", "application/json");
        r->status = 500;
      }
      MHD_queue_response(r->connection, (unsigned)r->status, r->response);
      MHD_destroy_response(r->response);
      r->response = NULL;
      MHD_resume_connection(r->connection);
    }
  }
  MHD_stop_daemon(daemon);
  return 0;
}
int db_open(const char *path, sqlite3 **db) {
  return sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
}
int db_exec(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }
void *db_prepare(sqlite3 *db, const char *sql) {
  sqlite3_stmt *s = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
    fprintf(stderr, "%s\n", sqlite3_errmsg(db)); exit(1);
  }
  return s;
}
void db_text(sqlite3_stmt *s, int index, const char *text, int length) {
  if (sqlite3_bind_text(s, index, text, length, SQLITE_TRANSIENT) != SQLITE_OK) abort();
}
void db_integer(sqlite3_stmt *s, int index, int64_t value) {
  if (sqlite3_bind_int64(s, index, value) != SQLITE_OK) abort();
}
void db_reset(sqlite3_stmt *s) { sqlite3_reset(s); sqlite3_clear_bindings(s); }
int crypto_verify(const char *secret, int secret_length, const char *message,
                  int message_length, const char *signature, int signature_length) {
  if (signature_length != 43) return 0;
  char decoded[48];
  fio_str_info_s out = {.buf = decoded, .capa = sizeof(decoded)};
  if (fio_string_write_base64dec(&out, NULL, signature, (size_t)signature_length)) return 0;
  fio_u256 expected = fio_sha256_hmac(secret, (size_t)secret_length, message, (size_t)message_length);
  return fio_ct_is_eq(decoded, expected.u8, 32);
}
int base64_decode(const char *encoded, int length, char *decoded) {
  fio_str_info_s out = {.buf = decoded, .capa = (size_t)length + 4};
  if (!length || length % 4 == 1 || fio_string_write_base64dec(&out, NULL, encoded, (size_t)length)) return -1;
  return length * 3 / 4;
}
double wall_time(void) {
  struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
double monotonic_time(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
