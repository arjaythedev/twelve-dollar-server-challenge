/* http.h: HTTP/1.1 request framing with fast GET /feed and header scan. */
#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <string.h>

#define HTTP_MAX_HEAD (16 * 1024)
#define HTTP_MAX_BODY (1024 * 1024)

enum { M_GET = 1, M_POST = 2, M_OTHER = 3 };

struct req {
  uint8_t method;
  uint8_t close;
  const uint8_t *path;
  uint32_t path_len;
  const uint8_t *auth;
  uint32_t auth_len;
  const uint8_t *body;
  uint32_t body_len;
  uint32_t total_len;
};

static inline int ieq(const uint8_t *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if ((a[i] | 0x20) != (uint8_t)b[i]) return 0;
  return 1;
}

static int http_parse(const uint8_t *p, uint32_t n, struct req *r) {
  uint32_t max_h = n > HTTP_MAX_HEAD ? HTTP_MAX_HEAD : n;
  const uint8_t *end = p + max_h;
  const uint8_t *eol = (const uint8_t *)memchr(p, '\n', max_h);
  if (!eol) return n >= HTTP_MAX_HEAD ? -1 : 0;
  const uint8_t *rl_end = (eol > p && eol[-1] == '\r') ? eol - 1 : eol;
  memset(r, 0, sizeof *r);

  const uint8_t *q = p;
  if (rl_end - q == 18 && memcmp(q, "GET /feed HTTP/1.", 17) == 0) {
    r->method = M_GET;
    r->path = q + 4;
    r->path_len = 5;
  } else {
    if (rl_end - q >= 4 && memcmp(q, "GET ", 4) == 0) {
      r->method = M_GET;
      q += 4;
    } else if (rl_end - q >= 5 && memcmp(q, "POST ", 5) == 0) {
      r->method = M_POST;
      q += 5;
    } else {
      const uint8_t *sp = (const uint8_t *)memchr(q, ' ', (size_t)(rl_end - q));
      if (!sp) return -1;
      r->method = M_OTHER;
      q = sp + 1;
    }
    const uint8_t *sp = (const uint8_t *)memchr(q, ' ', (size_t)(rl_end - q));
    if (!sp) return -1;
    r->path = q;
    r->path_len = (uint32_t)(sp - q);
    q = sp + 1;
    if (rl_end - q < 8 || memcmp(q, "HTTP/1.", 7) != 0) return -1;
  }

  q = eol + 1;
  int64_t clen = -1;
  for (;;) {
    if (q >= end) return n >= HTTP_MAX_HEAD ? -1 : 0;
    if (q + 1 < end && q[0] == '\r' && q[1] == '\n') {
      q += 2;
      break;
    }
    if (q[0] == '\n') {
      q += 1;
      break;
    }
    const uint8_t *le = (const uint8_t *)memchr(q, '\n', (size_t)(end - q));
    if (!le) return n >= HTTP_MAX_HEAD ? -1 : 0;
    const uint8_t *line_end = (le > q && le[-1] == '\r') ? le - 1 : le;
    uint8_t c0 = q[0] | 0x20;
    if (c0 == 'a' || c0 == 'c' || c0 == 't') {
      size_t ll = (size_t)(line_end - q);
      if (c0 == 'a' && ll >= 14 && q[13] == ':' && ieq(q, "authorization", 13)) {
        const uint8_t *v = q + 14;
        while (v < line_end && (*v == ' ' || *v == '\t')) v++;
        const uint8_t *ve = line_end;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        r->auth = v;
        r->auth_len = (uint32_t)(ve - v);
      } else if (c0 == 'c') {
        if (ll >= 15 && q[14] == ':' && ieq(q, "content-length", 14)) {
          const uint8_t *v = q + 15;
          while (v < line_end && (*v == ' ' || *v == '\t')) v++;
          const uint8_t *ve = line_end;
          while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
          if (ve == v) return -1;
          int64_t x = 0;
          for (const uint8_t *d = v; d < ve; d++) {
            if (*d < '0' || *d > '9') return -1;
            x = x * 10 + (*d - '0');
            if (x > HTTP_MAX_BODY) return -1;
          }
          clen = x;
        } else if (ll >= 11 && q[10] == ':' && ieq(q, "connection", 10)) {
          const uint8_t *v = q + 11;
          while (v < line_end && (*v == ' ' || *v == '\t')) v++;
          const uint8_t *ve = line_end;
          while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
          if (ve - v == 5 && ieq(v, "close", 5)) r->close = 1;
        }
      } else if (c0 == 't' && ll >= 18 && q[17] == ':' && ieq(q, "transfer-encoding", 17)) {
        return -1;
      }
    }
    q = le + 1;
  }

  uint32_t head_len = (uint32_t)(q - p);
  if (clen < 0) clen = 0;
  if ((uint64_t)head_len + (uint64_t)clen > n) return 0;
  r->body = q;
  r->body_len = (uint32_t)clen;
  r->total_len = head_len + (uint32_t)clen;
  return 1;
}

#endif
