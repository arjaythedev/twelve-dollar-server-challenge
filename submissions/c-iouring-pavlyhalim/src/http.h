/* http.h: HTTP/1.1 request framing. Finds the few headers the API uses and nothing else. */
#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <string.h>

#define HTTP_MAX_HEAD (16 * 1024)
#define HTTP_MAX_BODY (1024 * 1024)

enum { M_GET = 1, M_POST = 2, M_OTHER = 3 };

struct req {
  int method;
  const char *path;
  unsigned path_len;
  const char *auth; /* Authorization value, or NULL */
  unsigned auth_len;
  const char *body;
  unsigned body_len;
  unsigned total_len; /* bytes consumed: head + body */
  uint8_t keep_alive;
  uint8_t expect_continue;
};

static inline int ieq(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (x != y) return 0;
  }
  return 1;
}

/* Returns 1 when a whole request is in buf[0..n), 0 when more bytes are needed, -1 when the
 * request is malformed (the connection is then answered 400 and closed). */
static int http_parse(const uint8_t *buf, unsigned n, struct req *r) {
  const char *s = (const char *)buf;
  const char *end = memmem(s, n, "\r\n\r\n", 4);
  if (!end) return n > HTTP_MAX_HEAD ? -1 : 0;
  unsigned head_len = (unsigned)(end + 4 - s);
  memset(r, 0, sizeof *r);
  r->keep_alive = 1;

  /* request line */
  const char *p = s, *le = memchr(s, '\n', head_len);
  if (!le) return -1;
  if (head_len >= 4 && memcmp(p, "GET ", 4) == 0) { r->method = M_GET; p += 4; }
  else if (head_len >= 5 && memcmp(p, "POST ", 5) == 0) { r->method = M_POST; p += 5; }
  else {
    const char *sp = memchr(p, ' ', (size_t)(le - p));
    if (!sp) return -1;
    r->method = M_OTHER;
    p = sp + 1;
  }
  const char *sp = memchr(p, ' ', (size_t)(le - p));
  if (!sp) return -1;
  r->path = p;
  r->path_len = (unsigned)(sp - p);
  p = sp + 1;
  if (le - p >= 9 && memcmp(p, "HTTP/1.", 7) == 0) {
    if (p[7] == '0') r->keep_alive = 0;
    else if (p[7] != '1') return -1;
  } else {
    return -1;
  }

  /* headers */
  p = le + 1;
  const char *hend = s + head_len - 2; /* points at the final CRLF */
  while (p < hend) {
    const char *eol = memchr(p, '\n', (size_t)(hend + 2 - p));
    if (!eol) return -1;
    const char *colon = memchr(p, ':', (size_t)(eol - p));
    if (!colon) return -1;
    size_t nl = (size_t)(colon - p);
    const char *v = colon + 1;
    const char *ve = eol;
    if (ve > v && ve[-1] == '\r') ve--;
    while (v < ve && (*v == ' ' || *v == '\t')) v++;
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    size_t vl = (size_t)(ve - v);
    switch (nl) {
    case 13:
      if (ieq(p, "authorization", 13)) { r->auth = v; r->auth_len = (unsigned)vl; }
      break;
    case 14:
      if (ieq(p, "content-length", 14)) {
        if (vl == 0 || vl > 9) return -1;
        unsigned cl = 0;
        for (size_t i = 0; i < vl; i++) {
          if (v[i] < '0' || v[i] > '9') return -1;
          cl = cl * 10 + (unsigned)(v[i] - '0');
        }
        if (cl > HTTP_MAX_BODY) return -1;
        r->body_len = cl;
      }
      break;
    case 10:
      if (ieq(p, "connection", 10)) {
        if (vl == 5 && ieq(v, "close", 5)) r->keep_alive = 0;
        else if (vl == 10 && ieq(v, "keep-alive", 10)) r->keep_alive = 1;
      }
      break;
    case 17:
      if (ieq(p, "transfer-encoding", 17)) return -1; /* not supported: 400 and close */
      break;
    case 6:
      if (ieq(p, "expect", 6) && vl == 12 && ieq(v, "100-continue", 12)) r->expect_continue = 1;
      break;
    default: break;
    }
    p = eol + 1;
  }
  if (head_len + r->body_len > n) return 0;
  r->body = s + head_len;
  r->total_len = head_len + r->body_len;
  return 1;
}

/* A positive integer id in decimal: "0", "-1", "abc", "1.5" and leading zeros are invalid. */
static inline int parse_id(const char *s, unsigned n, uint64_t *out) {
  if (n == 0 || n > 18) return 0;
  if (s[0] == '0') return 0;
  uint64_t v = 0;
  for (unsigned i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    v = v * 10 + (uint64_t)(s[i] - '0');
  }
  *out = v;
  return 1;
}

#endif
