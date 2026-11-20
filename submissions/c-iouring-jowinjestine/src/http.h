/* http.h: HTTP/1.1 request framing. Only what the API needs is looked at: the request line,
 * Authorization, Content-Length, Connection and Transfer-Encoding. */
#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <string.h>

#define HTTP_MAX_HEAD (16 * 1024)
#define HTTP_MAX_BODY (1024 * 1024)

enum { M_GET = 1, M_POST = 2, M_OTHER = 3 };

struct req {
  uint8_t method;
  uint8_t close; /* Connection: close */
  const uint8_t *path;
  uint32_t path_len;
  const uint8_t *auth; /* the Authorization header's value, or NULL */
  uint32_t auth_len;
  const uint8_t *body;
  uint32_t body_len;
  uint32_t total_len; /* bytes of input this request used */
};

static inline int ieq(const uint8_t *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if ((a[i] | 0x20) != (uint8_t)b[i]) return 0;
  return 1;
}

/* Finds "\r\n\r\n" in p[0..n). */
static inline const uint8_t *find_head_end(const uint8_t *p, size_t n) {
  const uint8_t *end = p + n;
  const uint8_t *q = p;
  while ((q = memchr(q, '\r', (size_t)(end - q))) != NULL) {
    if (end - q < 4) return NULL;
    if (q[1] == '\n' && q[2] == '\r' && q[3] == '\n') return q;
    q++;
  }
  return NULL;
}

/* 1: a complete request in r. 0: incomplete (need more bytes). -1: malformed. */
static int http_parse(const uint8_t *p, uint32_t n, struct req *r) {
  const uint8_t *he = find_head_end(p, n > HTTP_MAX_HEAD ? HTTP_MAX_HEAD : n);
  if (!he) return n >= HTTP_MAX_HEAD ? -1 : 0;
  const uint8_t *head_end = he + 4; /* first body byte */
  memset(r, 0, sizeof *r);
  /* request line */
  const uint8_t *q = p;
  if (he - q >= 4 && q[0] == 'G' && q[1] == 'E' && q[2] == 'T' && q[3] == ' ') {
    r->method = M_GET;
    q += 4;
  } else if (he - q >= 5 && q[0] == 'P' && q[1] == 'O' && q[2] == 'S' && q[3] == 'T' && q[4] == ' ') {
    r->method = M_POST;
    q += 5;
  } else {
    const uint8_t *sp = memchr(q, ' ', (size_t)(he - q));
    if (!sp) return -1;
    r->method = M_OTHER;
    q = sp + 1;
  }
  const uint8_t *sp = memchr(q, ' ', (size_t)(he - q));
  if (!sp) return -1;
  r->path = q;
  r->path_len = (uint32_t)(sp - q);
  q = sp + 1;
  const uint8_t *eol = memchr(q, '\n', (size_t)(he + 2 - q));
  if (!eol || eol - q < 9 || memcmp(q, "HTTP/1.", 7) != 0) return -1;
  q = eol + 1;
  /* headers */
  int64_t clen = -1;
  while (q < he + 2) {
    const uint8_t *le = memchr(q, '\n', (size_t)(he + 2 - q));
    if (!le) break;
    const uint8_t *line_end = le > q && le[-1] == '\r' ? le - 1 : le;
    const uint8_t *colon = memchr(q, ':', (size_t)(line_end - q));
    if (colon) {
      size_t nl = (size_t)(colon - q);
      const uint8_t *v = colon + 1;
      while (v < line_end && (*v == ' ' || *v == '\t')) v++;
      const uint8_t *ve = line_end;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      if (nl == 13 && ieq(q, "authorization", 13)) {
        r->auth = v;
        r->auth_len = (uint32_t)(ve - v);
      } else if (nl == 14 && ieq(q, "content-length", 14)) {
        int64_t x = 0;
        if (ve == v) return -1;
        for (const uint8_t *d = v; d < ve; d++) {
          if (*d < '0' || *d > '9') return -1;
          x = x * 10 + (*d - '0');
          if (x > HTTP_MAX_BODY) return -1;
        }
        clen = x;
      } else if (nl == 10 && ieq(q, "connection", 10)) {
        if (ve - v == 5 && ieq(v, "close", 5)) r->close = 1;
      } else if (nl == 17 && ieq(q, "transfer-encoding", 17)) {
        return -1; /* chunked bodies are out of scope */
      }
    }
    q = le + 1;
  }
  uint32_t head_len = (uint32_t)(head_end - p);
  if (clen < 0) clen = 0;
  if ((uint64_t)head_len + (uint64_t)clen > n) return 0;
  r->body = head_end;
  r->body_len = (uint32_t)clen;
  r->total_len = head_len + (uint32_t)clen;
  return 1;
}

#endif
