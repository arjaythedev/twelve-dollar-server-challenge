#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

enum route_kind {
  ROUTE_NONE = 0,
  ROUTE_HEALTH,
  ROUTE_FEED,
  ROUTE_POST_GET,
  ROUTE_POST_CREATE,
  ROUTE_POST_LIKE,
  ROUTE_NOT_FOUND
};

struct http_req {
  enum route_kind route;
  int64_t post_id;
  int keep_alive;
  size_t consumed;
  const uint8_t *auth;
  size_t auth_len;
  const uint8_t *body;
  size_t body_len;
};

enum jwt_rc {
  JWT_OK = 0,
  JWT_ERR_MISSING = 1,
  JWT_ERR_INVALID = 2,
  JWT_ERR_PAYLOAD = 3
};

enum json_body_rc {
  JSON_BODY_OK = 0,
  JSON_BODY_MALFORMED = 1,
  JSON_BODY_REQUIRED = 2,
  JSON_BODY_TOO_LONG = 3
};

void proto_init(const char *jwt_secret);

ssize_t http_parse_req(const uint8_t *buf, size_t len, struct http_req *out);

enum jwt_rc jwt_verify_hs256(const uint8_t *auth, size_t auth_len, int64_t now_sec,
                             int64_t *uid_out, uint8_t *uname_buf, size_t uname_cap,
                             size_t *uname_len_out);

enum json_body_rc json_parse_post_body(const uint8_t *body, size_t len,
                                       uint8_t *trimmed_out, size_t trimmed_cap,
                                       size_t *trimmed_len_out);

size_t json_escape_append(uint8_t *dst, size_t off, size_t cap, const uint8_t *src, size_t len);

size_t resp_write_error(uint8_t *dst, size_t cap, int status, const char *msg, int keep_alive);
size_t resp_write_bad_request(uint8_t *dst, size_t cap);
size_t resp_write_health(uint8_t *dst, size_t cap, int ok, int64_t uptime_s, const char *err, int keep_alive);
size_t resp_write_json_body(uint8_t *dst, size_t cap, int status, const char *reason,
                            const uint8_t *json_body, size_t json_len, int keep_alive);
size_t resp_write_post_created(uint8_t *dst, size_t cap, int64_t id,
                               const uint8_t *body, size_t body_len,
                               const char *created_at, size_t created_len,
                               const uint8_t *author, size_t author_len, int keep_alive);
size_t resp_write_like(uint8_t *dst, size_t cap, int already_liked, int64_t post_id, int keep_alive);

#endif
