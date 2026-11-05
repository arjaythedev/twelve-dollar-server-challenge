#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define HDR_ROOM 160

enum { M_OTHER = 0, M_GET = 1, M_POST = 2 };

typedef struct {
  uint8_t method;
  uint8_t close;
  uint8_t has_auth;
  const uint8_t *path;
  uint32_t path_len;
  const uint8_t *auth;
  uint32_t auth_len;
  const uint8_t *body;
  uint32_t body_len;
} req_t;

typedef struct obuf {
  struct obuf *next;
  const uint8_t *data;
  uint32_t len;
  uint32_t cap;
  uint8_t *mem;
} obuf;

typedef struct {
  uint8_t *p, *end, *start;
  obuf *b;
} out_t;

enum { K_NONE = 0, K_HEALTH, K_FEED, K_POST, K_LIKE, K_CREATE };

typedef struct job {
  uint8_t kind;
  uint8_t is_write;
  uint8_t close_after;
  uint32_t conn;
  uint32_t gen;
  int64_t id;
  int64_t uid;
  const uint8_t *name;
  uint32_t name_len;
  uint8_t *text;
  uint32_t text_len;
  obuf *resp;
} job_t;

extern int64_t now_s;

obuf *obuf_static(const uint8_t *data, uint32_t len);
void obuf_release(obuf *b);
void out_module_init(void);
void out_begin(out_t *o);
void out_abort(out_t *o);
void out_need(out_t *o, uint32_t n);
void out_lit(out_t *o, const char *s, uint32_t n);
void out_i64(out_t *o, int64_t v);
void out_jstr(out_t *o, const uint8_t *s, uint32_t n);
obuf *out_finish(out_t *o, const char *status, int close_conn);
void put_post(out_t *o, int64_t id, const uint8_t *body, uint32_t body_len,
              const uint8_t *created, uint32_t created_len,
              const uint8_t *author, uint32_t author_len, int64_t likes);

enum { JT_NONE = 0, JT_STRING, JT_NUMBER, JT_OTHER };
typedef struct {
  const char *key;
  uint32_t key_len;
  uint8_t type;
  const uint8_t *v;
  uint32_t v_len;
  uint8_t escaped;
} jmember;
int json_members(const uint8_t *p, uint32_t len, jmember *m, int nm);
uint32_t json_unescape(const uint8_t *s, uint32_t n, uint8_t *dst);

enum { JWT_OK = 0, JWT_INVALID = 1, JWT_PAYLOAD = 2 };
typedef struct {
  int64_t uid;
  uint8_t name[256];
  uint32_t name_len;
} claims_t;
void jwt_fast_init(const char *secret, size_t secret_len);
int jwt_verify(const uint8_t *token, uint32_t token_len, int64_t now, claims_t *c);

int http_parse(const uint8_t *p, uint32_t n, req_t *r);
int path_lit(const uint8_t **s, const uint8_t *e, const char *lit, uint32_t n);
int path_param(const uint8_t **s, const uint8_t *e, const uint8_t **v, uint32_t *vn);

void db_open(const char *path);
void db_check_inv(void);
int db_batch_begin(void);
int db_batch_end(void);
int db_fast_feed(void);
int db_health(void);
const char *db_errmsg(void);
int db_write_feed(out_t *o);
int db_write_post(out_t *o, int64_t id);
int db_post_exists(int64_t id);
int db_insert_post(int64_t uid, const uint8_t *body, uint32_t blen, int64_t *id,
                   uint8_t *created, uint32_t *clen);
int db_insert_like(int64_t uid, int64_t post_id, int *already);
int db_cursors_open(void);
void db_cursors_close(void);
void db_native_off(void);

void app_init(void);
void app_decode(const req_t *rq, job_t *j);
void app_batch(job_t *jobs, int n);

void transport_init(const char *host, int port);
void transport_run(void);
