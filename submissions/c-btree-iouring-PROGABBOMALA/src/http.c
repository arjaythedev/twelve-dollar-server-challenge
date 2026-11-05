#include "rt.h"

#define MAX_HEAD 16384
#define MAX_BODY 16384

static int ci_eq(const uint8_t *s, const char *lit, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
    if (c != (uint8_t)lit[i]) return 0;
  }
  return 1;
}

static uint32_t find_head_end(const uint8_t *p, uint32_t n) {
  uint32_t i = 0;
  /* 8-byte scan for CR, then check the CRLFCRLF pattern. */
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    memcpy(&w, p + i, 8);
    uint64_t x = w ^ 0x0d0d0d0d0d0d0d0dULL;
    uint64_t m = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
    if (!m) continue;
    for (uint32_t k = i; k < i + 8 && k + 3 < n; k++)
      if (p[k] == '\r' && p[k + 1] == '\n' && p[k + 2] == '\r' && p[k + 3] == '\n') return k + 4;
  }
  for (; i + 3 < n; i++)
    if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') return i + 4;
  return 0;
}

int http_parse(const uint8_t *p, uint32_t n, req_t *r) {
  uint32_t scan = n < MAX_HEAD ? n : MAX_HEAD;
  uint32_t he = find_head_end(p, scan);
  if (!he) return n >= MAX_HEAD ? -1 : 0;
  memset(r, 0, sizeof *r);
  const uint8_t *e = p + he - 2;
  const uint8_t *q = p;
  if (he >= 4 && memcmp(p, "GET ", 4) == 0) { r->method = M_GET; q = p + 4; }
  else if (he >= 5 && memcmp(p, "POST ", 5) == 0) { r->method = M_POST; q = p + 5; }
  else {
    while (q < e && *q != ' ') q++;
    if (q == p || q >= e) return -1;
    q++;
    r->method = M_OTHER;
  }
  const uint8_t *path = q;
  while (q < e && *q != ' ' && *q != '\r') q++;
  if (q == path || q >= e || *q != ' ') return -1;
  r->path = path;
  r->path_len = (uint32_t)(q - path);
  const uint8_t *qm = memchr(path, '?', r->path_len);
  if (qm) r->path_len = (uint32_t)(qm - path);
  q++;
  if ((uint32_t)(e - q) < 8 || memcmp(q, "HTTP/1.", 7) != 0) return -1;
  int http10 = q[7] == '0';
  q += 8;
  if (q > e || q[0] != '\r' || q[1] != '\n') return -1;
  q += 2;

  int keepalive10 = 0;
  int64_t clen = -1;
  int chunked = 0;
  while (q < e) {
    const uint8_t *ls = q;
    const uint8_t *le = memchr(q, '\r', (size_t)(e - q + 2));
    if (!le || le + 1 > e || le[1] != '\n') return -1;
    const uint8_t *colon = memchr(ls, ':', (size_t)(le - ls));
    if (!colon || colon == ls) return -1;
    uint32_t nl = (uint32_t)(colon - ls);
    const uint8_t *v = colon + 1;
    while (v < le && (*v == ' ' || *v == '\t')) v++;
    const uint8_t *ve = le;
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    uint32_t vl = (uint32_t)(ve - v);
    if (nl == 13 && ci_eq(ls, "authorization", 13)) {
      r->auth = v; r->auth_len = vl; r->has_auth = 1;
    } else if (nl == 14 && ci_eq(ls, "content-length", 14)) {
      if (!vl || vl > 9) return -1;
      int64_t x = 0;
      for (uint32_t i = 0; i < vl; i++) {
        if (v[i] < '0' || v[i] > '9') return -1;
        x = x * 10 + (v[i] - '0');
      }
      if (clen >= 0 && clen != x) return -1;
      clen = x;
    } else if (nl == 10 && ci_eq(ls, "connection", 10)) {
      if (vl == 5 && ci_eq(v, "close", 5)) r->close = 1;
      else if (vl == 10 && ci_eq(v, "keep-alive", 10)) keepalive10 = 1;
    } else if (nl == 17 && ci_eq(ls, "transfer-encoding", 17)) {
      chunked = 1;
    }
    q = le + 2;
  }
  if (http10 && !keepalive10) r->close = 1;
  if (chunked) return -1;
  if (clen < 0) clen = 0;
  if (clen > MAX_BODY) return -1;
  if ((uint64_t)he + (uint64_t)clen > n) return 0;
  r->body = p + he;
  r->body_len = (uint32_t)clen;
  return (int)(he + (uint32_t)clen);
}

int path_lit(const uint8_t **s, const uint8_t *e, const char *lit, uint32_t n) {
  if ((uint32_t)(e - *s) < n || memcmp(*s, lit, n)) return 0;
  if (*s + n < e && (*s)[n] != '/') return 0;
  *s += n;
  return 1;
}
int path_param(const uint8_t **s, const uint8_t *e, const uint8_t **v, uint32_t *vn) {
  if (*s >= e || **s != '/') return 0;
  const uint8_t *a = *s + 1, *b = a;
  while (b < e && *b != '/') b++;
  if (b == a) return 0;
  *v = a;
  *vn = (uint32_t)(b - a);
  *s = b;
  return 1;
}
