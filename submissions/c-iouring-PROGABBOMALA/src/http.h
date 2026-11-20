#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <emmintrin.h>

#define MAX_HEAD 16384u
#define MAX_BODY (1u << 20)

static inline char *put_lit(char *d, const char *s, size_t n) {
  memcpy(d, s, n);
  return d + n;
}
#define PUT(d, lit) put_lit((d), (lit), sizeof(lit) - 1)

static inline char *put_u64(char *d, uint64_t v) {
  char tmp[20];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) *d++ = tmp[--n];
  return d;
}

static inline char *put_i64(char *d, int64_t v) {
  if (v < 0) {
    *d++ = '-';
    return put_u64(d, (uint64_t)0 - (uint64_t)v);
  }
  return put_u64(d, (uint64_t)v);
}

static inline char *put_esc_byte(char *d, unsigned char c) {
  static const char hex[] = "0123456789abcdef";
  switch (c) {
    case '"':  *d++ = '\\'; *d++ = '"';  break;
    case '\\': *d++ = '\\'; *d++ = '\\'; break;
    case '\b': *d++ = '\\'; *d++ = 'b';  break;
    case '\f': *d++ = '\\'; *d++ = 'f';  break;
    case '\n': *d++ = '\\'; *d++ = 'n';  break;
    case '\r': *d++ = '\\'; *d++ = 'r';  break;
    case '\t': *d++ = '\\'; *d++ = 't';  break;
    default:
      if (c < 0x20) {
        d[0] = '\\'; d[1] = 'u'; d[2] = '0'; d[3] = '0';
        d[4] = hex[c >> 4]; d[5] = hex[c & 15];
        d += 6;
      } else {
        *d++ = (char)c;
      }
  }
  return d;
}

static inline char *put_jstr(char *d, const uint8_t *s, size_t n) {
  const __m128i quote = _mm_set1_epi8('"');
  const __m128i slash = _mm_set1_epi8('\\');
  const __m128i ctl = _mm_set1_epi8(0x1f);
  while (n >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(const void *)s);
    __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, slash)),
                             _mm_cmpeq_epi8(_mm_min_epu8(v, ctl), v));
    unsigned mask = (unsigned)_mm_movemask_epi8(m);
    _mm_storeu_si128((__m128i *)(void *)d, v);
    if (!mask) {
      d += 16; s += 16; n -= 16;
      continue;
    }
    unsigned k = (unsigned)__builtin_ctz(mask);
    d = put_esc_byte(d + k, s[k]);
    s += k + 1;
    n -= k + 1;
  }
  while (n--) d = put_esc_byte(d, *s++);
  return d;
}

enum { JV_NONE = 0, JV_STRING, JV_NUMBER, JV_OTHER };

struct jmember {
  const char *key;
  uint32_t klen;
  uint8_t type;
  uint8_t esc;
  const uint8_t *v;
  uint32_t vlen;
};

struct jreader {
  const uint8_t *p, *e;
  int depth;
};

static inline void j_ws(struct jreader *j) {
  while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static inline int j_hex(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c |= 0x20;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int j_string(struct jreader *j, const uint8_t **s, uint32_t *n, uint8_t *esc) {
  const uint8_t *p = j->p + 1, *e = j->e;
  const uint8_t *start = p;
  uint8_t has_esc = 0;
  for (;;) {
    if (p >= e) return -1;
    unsigned char c = *p;
    if (c == '"') break;
    if (c < 0x20) return -1;
    if (c == '\\') {
      has_esc = 1;
      if (p + 1 >= e) return -1;
      unsigned char x = p[1];
      if (x == 'u') {
        if (p + 6 > e) return -1;
        for (int i = 2; i < 6; i++)
          if (j_hex(p[i]) < 0) return -1;
        p += 6;
        continue;
      }
      if (x != '"' && x != '\\' && x != '/' && x != 'b' && x != 'f' && x != 'n' && x != 'r' && x != 't')
        return -1;
      p += 2;
      continue;
    }
    p++;
  }
  *s = start;
  *n = (uint32_t)(p - start);
  *esc = has_esc;
  j->p = p + 1;
  return 0;
}

static int j_number(struct jreader *j) {
  const uint8_t *p = j->p, *e = j->e;
  if (p < e && *p == '-') p++;
  if (p >= e) return -1;
  if (*p == '0') {
    p++;
  } else if (*p >= '1' && *p <= '9') {
    while (p < e && *p >= '0' && *p <= '9') p++;
  } else {
    return -1;
  }
  if (p < e && *p == '.') {
    p++;
    if (p >= e || *p < '0' || *p > '9') return -1;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p >= e || *p < '0' || *p > '9') return -1;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  j->p = p;
  return 0;
}

static int j_value(struct jreader *j);

static int j_literal(struct jreader *j, const char *lit, size_t n) {
  if ((size_t)(j->e - j->p) < n || memcmp(j->p, lit, n) != 0) return -1;
  j->p += n;
  return 0;
}

static int j_container(struct jreader *j, int object) {
  if (++j->depth > 64) return -1;
  unsigned char close = object ? '}' : ']';
  j->p++;
  j_ws(j);
  if (j->p < j->e && *j->p == close) {
    j->p++;
    j->depth--;
    return 0;
  }
  for (;;) {
    j_ws(j);
    if (object) {
      const uint8_t *s;
      uint32_t n;
      uint8_t esc;
      if (j->p >= j->e || *j->p != '"' || j_string(j, &s, &n, &esc)) return -1;
      j_ws(j);
      if (j->p >= j->e || *j->p != ':') return -1;
      j->p++;
    }
    if (j_value(j)) return -1;
    j_ws(j);
    if (j->p >= j->e) return -1;
    if (*j->p == ',') {
      j->p++;
      continue;
    }
    if (*j->p == close) {
      j->p++;
      j->depth--;
      return 0;
    }
    return -1;
  }
}

static int j_value(struct jreader *j) {
  j_ws(j);
  if (j->p >= j->e) return -1;
  switch (*j->p) {
    case '{': return j_container(j, 1);
    case '[': return j_container(j, 0);
    case '"': {
      const uint8_t *s;
      uint32_t n;
      uint8_t esc;
      return j_string(j, &s, &n, &esc);
    }
    case 't': return j_literal(j, "true", 4);
    case 'f': return j_literal(j, "false", 5);
    case 'n': return j_literal(j, "null", 4);
    default: return j_number(j);
  }
}

static inline char *u8_put(char *d, uint32_t cp) {
  if (cp < 0x80) {
    *d++ = (char)cp;
  } else if (cp < 0x800) {
    *d++ = (char)(0xC0 | (cp >> 6));
    *d++ = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *d++ = (char)(0xE0 | (cp >> 12));
    *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *d++ = (char)(0x80 | (cp & 0x3F));
  } else {
    *d++ = (char)(0xF0 | (cp >> 18));
    *d++ = (char)(0x80 | ((cp >> 12) & 0x3F));
    *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *d++ = (char)(0x80 | (cp & 0x3F));
  }
  return d;
}

static uint32_t json_unescape(const uint8_t *s, uint32_t n, uint8_t *dst) {
  const uint8_t *e = s + n;
  char *d = (char *)dst;
  while (s < e) {
    unsigned char c = *s++;
    if (c != '\\') {
      *d++ = (char)c;
      continue;
    }
    c = *s++;
    switch (c) {
      case 'b': *d++ = '\b'; break;
      case 'f': *d++ = '\f'; break;
      case 'n': *d++ = '\n'; break;
      case 'r': *d++ = '\r'; break;
      case 't': *d++ = '\t'; break;
      case 'u': {
        uint32_t cp = (uint32_t)((j_hex(s[0]) << 12) | (j_hex(s[1]) << 8) | (j_hex(s[2]) << 4) | j_hex(s[3]));
        s += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
          uint32_t lo = (uint32_t)((j_hex(s[2]) << 12) | (j_hex(s[3]) << 8) | (j_hex(s[4]) << 4) | j_hex(s[5]));
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            s += 6;
          }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* lone surrogate */
        d = u8_put(d, cp);
        break;
      }
      default: *d++ = (char)c; /* " \ / */
    }
  }
  return (uint32_t)((uint8_t *)d - dst);
}

static int json_object(const uint8_t *p, uint32_t n, struct jmember *m, int nm) {
  struct jreader j = {p, p + n, 0};
  for (int i = 0; i < nm; i++) {
    m[i].type = JV_NONE;
    m[i].esc = 0;
    m[i].v = 0;
    m[i].vlen = 0;
  }
  j_ws(&j);
  if (j.p >= j.e) return -1;
  if (*j.p != '{') {
    if (j_value(&j)) return -1;
    j_ws(&j);
    return j.p == j.e ? 1 : -1;
  }
  j.p++;
  j.depth = 1;
  j_ws(&j);
  if (j.p < j.e && *j.p == '}') {
    j.p++;
  } else {
    for (;;) {
      const uint8_t *ks;
      uint32_t kn;
      uint8_t kesc;
      j_ws(&j);
      if (j.p >= j.e || *j.p != '"' || j_string(&j, &ks, &kn, &kesc)) return -1;
      uint8_t kbuf[64];
      if (kesc) {
        if (kn <= sizeof kbuf) {
          kn = json_unescape(ks, kn, kbuf);
          ks = kbuf;
        } else {
          kn = 0; /* longer than any key we look for */
        }
      }
      struct jmember *hit = 0;
      for (int i = 0; i < nm; i++)
        if (m[i].klen == kn && memcmp(m[i].key, ks, kn) == 0) hit = &m[i];
      j_ws(&j);
      if (j.p >= j.e || *j.p != ':') return -1;
      j.p++;
      j_ws(&j);
      if (j.p >= j.e) return -1;
      const uint8_t *vs = j.p;
      if (*j.p == '"') {
        const uint8_t *s;
        uint32_t sn;
        uint8_t esc;
        if (j_string(&j, &s, &sn, &esc)) return -1;
        if (hit) {
          hit->type = JV_STRING;
          hit->esc = esc;
          hit->v = s;
          hit->vlen = sn;
        }
      } else {
        unsigned char c = *j.p;
        int is_num = c == '-' || (c >= '0' && c <= '9');
        if (j_value(&j)) return -1;
        if (hit) {
          hit->type = is_num ? JV_NUMBER : JV_OTHER;
          hit->esc = 0;
          hit->v = vs;
          hit->vlen = (uint32_t)(j.p - vs);
        }
      }
      j_ws(&j);
      if (j.p >= j.e) return -1;
      if (*j.p == ',') {
        j.p++;
        continue;
      }
      if (*j.p == '}') {
        j.p++;
        break;
      }
      return -1;
    }
  }
  j_ws(&j);
  return j.p == j.e ? 0 : -1;
}

/* The whitespace String.prototype.trim removes. */
static inline int js_space(uint32_t cp) {
  return (cp >= 9 && cp <= 13) || cp == 0x20 || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
         cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

static inline uint32_t u8_get(const uint8_t *p, const uint8_t *e, uint32_t *len) {
  unsigned char c = p[0];
  if (c < 0x80) { *len = 1; return c; }
  if (c >= 0xC0 && c < 0xE0 && e - p >= 2) {
    *len = 2;
    return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
  }
  if (c >= 0xE0 && c < 0xF0 && e - p >= 3) {
    *len = 3;
    return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  }
  if (c >= 0xF0 && c < 0xF8 && e - p >= 4) {
    *len = 4;
    return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
  }
  *len = 1;
  return 0xFFFD;
}

static void js_trim(const uint8_t **ps, uint32_t *pn) {
  const uint8_t *a = *ps, *b = a + *pn;
  uint32_t l;
  while (a < b && js_space(u8_get(a, b, &l))) a += l;
  while (b > a) {
    const uint8_t *k = b - 1;
    while (k > a && (*k & 0xC0) == 0x80) k--;
    if (!js_space(u8_get(k, b, &l))) break;
    b = k;
  }
  *ps = a;
  *pn = (uint32_t)(b - a);
}

static inline uint32_t u8_chars(const uint8_t *s, uint32_t n) {
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++) c += (s[i] & 0xC0) != 0x80;
  return c;
}

enum http_route {
  ROUTE_NOT_FOUND = 0,
  ROUTE_HEALTH,
  ROUTE_FEED,
  ROUTE_GET_POST,
  ROUTE_CREATE_POST,
  ROUTE_LIKE_POST,
  ROUTE_BAD_POST_ID
};

struct http_req {
  uint8_t route;
  uint8_t is_post;
  uint8_t keep_alive;
  int64_t post_id;
  const char *auth; /* Authorization value, NULL when the header is absent */
  uint32_t auth_len;
  const char *body;
  uint32_t body_len;
};

static inline int ieq(const char *a, const char *lower, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)a[i];
    if (c >= 'A' && c <= 'Z') c |= 0x20;
    if (c != (unsigned char)lower[i]) return 0;
  }
  return 1;
}

static int parse_post_id(const char *s, size_t n, int64_t *out) {
  if (!n) return 0;
  uint64_t v = 0;
  int big = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned d = (unsigned)(s[i] - '0');
    if (d > 9) return 0;
    if (v > 922337203685477579ull) big = 1;
    else v = v * 10 + d;
  }
  if (big || v > (uint64_t)INT64_MAX) {
    *out = INT64_MAX;
    return 1;
  }
  if (v == 0) return 0;
  *out = (int64_t)v;
  return 1;
}

static void route_request(struct http_req *q, int is_get, const char *path, size_t n) {
  const char *qm = memchr(path, '?', n);
  if (qm) n = (size_t)(qm - path);
  if (is_get) {
    if (n == 5 && memcmp(path, "/feed", 5) == 0) { q->route = ROUTE_FEED; return; }
    if (n > 7 && memcmp(path, "/posts/", 7) == 0 && !memchr(path + 7, '/', n - 7)) {
      q->route = parse_post_id(path + 7, n - 7, &q->post_id) ? ROUTE_GET_POST : ROUTE_BAD_POST_ID;
      return;
    }
    if (n == 7 && memcmp(path, "/health", 7) == 0) { q->route = ROUTE_HEALTH; return; }
    return;
  }
  if (n == 6 && memcmp(path, "/posts", 6) == 0) { q->route = ROUTE_CREATE_POST; return; }
  if (n >= 12 && memcmp(path, "/posts/", 7) == 0 && memcmp(path + n - 5, "/like", 5) == 0 &&
      !memchr(path + 7, '/', n - 12)) {
    q->route = parse_post_id(path + 7, n - 12, &q->post_id) ? ROUTE_LIKE_POST : ROUTE_BAD_POST_ID;
  }
}

static int http_parse(const char *buf, size_t len, struct http_req *q) {
  const char *he = memmem(buf, len, "\r\n\r\n", 4);
  if (!he) return len > MAX_HEAD ? -1 : 0;
  size_t head = (size_t)(he - buf) + 4;
  if (head > MAX_HEAD) return -1;

  const char *eol = memchr(buf, '\r', head);
  const char *sp1 = memchr(buf, ' ', (size_t)(eol - buf));
  if (!sp1) return -1;
  const char *path = sp1 + 1;
  const char *sp2 = memchr(path, ' ', (size_t)(eol - path));
  if (!sp2) return -1;
  size_t mlen = (size_t)(sp1 - buf);
  size_t vlen = (size_t)(eol - sp2 - 1);

  q->route = ROUTE_NOT_FOUND;
  q->is_post = 0;
  q->keep_alive = !(vlen == 8 && memcmp(sp2 + 1, "HTTP/1.0", 8) == 0);
  q->post_id = 0;
  q->auth = 0;
  q->auth_len = 0;
  if (mlen == 3 && memcmp(buf, "GET", 3) == 0) {
    route_request(q, 1, path, (size_t)(sp2 - path));
  } else if (mlen == 4 && memcmp(buf, "POST", 4) == 0) {
    q->is_post = 1;
    route_request(q, 0, path, (size_t)(sp2 - path));
  }

  size_t clen = 0;
  const char *line = eol + 2;
  const char *hend = he + 2; /* one past the last header line's CRLF */
  while (line < hend) {
    const char *le = memchr(line, '\r', (size_t)(hend - line));
    if (!le) return -1;
    const char *colon = memchr(line, ':', (size_t)(le - line));
    if (colon) {
      size_t nl = (size_t)(colon - line);
      const char *v = colon + 1, *ve = le;
      while (v < ve && (*v == ' ' || *v == '\t')) v++;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      if (nl == 13 && ieq(line, "authorization", 13)) {
        q->auth = v;
        q->auth_len = (uint32_t)(ve - v);
      } else if (nl == 14 && ieq(line, "content-length", 14)) {
        if (v == ve) return -1;
        size_t x = 0;
        for (const char *p = v; p < ve; p++) {
          unsigned d = (unsigned)(*p - '0');
          if (d > 9 || x > MAX_BODY) return -1;
          x = x * 10 + d;
        }
        clen = x;
      } else if (nl == 10 && ieq(line, "connection", 10)) {
        if (ve - v == 5 && ieq(v, "close", 5)) q->keep_alive = 0;
      } else if (nl == 17 && ieq(line, "transfer-encoding", 17)) {
        return -1; /* chunked request bodies are not supported */
      }
    }
    line = le + 2;
  }
  if (clen > MAX_BODY) return -1;
  if (len < head + clen) return 0;
  q->body = buf + head;
  q->body_len = (uint32_t)clen;
  return (int)(head + clen);
}

#endif
