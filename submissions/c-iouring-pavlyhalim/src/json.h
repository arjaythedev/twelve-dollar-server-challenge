/* json.h: a validating JSON reader that picks a few members out of an object, and a writer
 * for compact output. Both avoid allocation. */
#ifndef JSON_H
#define JSON_H

#include <stdint.h>
#include <string.h>

/* ---------------- reader ---------------- */

enum { J_NONE = 0, J_STRING, J_NUMBER, J_OBJECT, J_ARRAY, J_TRUE, J_FALSE, J_NULL };

struct jval {
  const char *p; /* strings: the raw (still escaped) contents between the quotes */
  size_t n;
  int type;
};
struct jwant {
  const char *key;
  size_t klen;
  struct jval v;
};

struct jp {
  const char *s, *end;
  int depth;
};

static int jp_value(struct jp *p, struct jval *out);

static inline void jp_ws(struct jp *p) {
  while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) p->s++;
}

static inline int jp_hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* p->s points at the opening quote. Validates escapes; leaves p->s after the closing quote. */
static int jp_string(struct jp *p, struct jval *out) {
  p->s++;
  const char *start = p->s;
  while (p->s < p->end) {
    unsigned char c = (unsigned char)*p->s;
    if (c == '"') {
      if (out) { out->p = start; out->n = (size_t)(p->s - start); out->type = J_STRING; }
      p->s++;
      return 0;
    }
    if (c < 0x20) return -1;
    if (c == '\\') {
      if (p->s + 1 >= p->end) return -1;
      char e = p->s[1];
      if (e == 'u') {
        if (p->s + 6 > p->end) return -1;
        for (int i = 2; i < 6; i++)
          if (jp_hex(p->s[i]) < 0) return -1;
        p->s += 6;
        continue;
      }
      if (!(e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't')) return -1;
      p->s += 2;
      continue;
    }
    p->s++;
  }
  return -1;
}

static int jp_number(struct jp *p, struct jval *out) {
  const char *start = p->s;
  if (p->s < p->end && *p->s == '-') p->s++;
  if (p->s >= p->end) return -1;
  if (*p->s == '0') {
    p->s++;
  } else if (*p->s >= '1' && *p->s <= '9') {
    while (p->s < p->end && *p->s >= '0' && *p->s <= '9') p->s++;
  } else {
    return -1;
  }
  if (p->s < p->end && *p->s == '.') {
    p->s++;
    if (p->s >= p->end || *p->s < '0' || *p->s > '9') return -1;
    while (p->s < p->end && *p->s >= '0' && *p->s <= '9') p->s++;
  }
  if (p->s < p->end && (*p->s == 'e' || *p->s == 'E')) {
    p->s++;
    if (p->s < p->end && (*p->s == '+' || *p->s == '-')) p->s++;
    if (p->s >= p->end || *p->s < '0' || *p->s > '9') return -1;
    while (p->s < p->end && *p->s >= '0' && *p->s <= '9') p->s++;
  }
  if (out) { out->p = start; out->n = (size_t)(p->s - start); out->type = J_NUMBER; }
  return 0;
}

static int jp_literal(struct jp *p, const char *lit, size_t n, int type, struct jval *out) {
  if ((size_t)(p->end - p->s) < n || memcmp(p->s, lit, n) != 0) return -1;
  if (out) { out->p = p->s; out->n = n; out->type = type; }
  p->s += n;
  return 0;
}

/* An object: validates every member; `w` receives the first occurrence of each wanted key
 * (only at this level, raw keys compared byte-for-byte, so escaped keys never match). */
static int jp_object(struct jp *p, struct jwant *w, int nw) {
  p->s++;
  if (++p->depth > 32) return -1;
  jp_ws(p);
  if (p->s < p->end && *p->s == '}') { p->s++; p->depth--; return 0; }
  for (;;) {
    jp_ws(p);
    if (p->s >= p->end || *p->s != '"') return -1;
    struct jval key;
    if (jp_string(p, &key) < 0) return -1;
    jp_ws(p);
    if (p->s >= p->end || *p->s != ':') return -1;
    p->s++;
    jp_ws(p);
    struct jval val;
    if (jp_value(p, &val) < 0) return -1;
    for (int i = 0; i < nw; i++)
      if (w[i].v.type == J_NONE && key.n == w[i].klen && memcmp(key.p, w[i].key, key.n) == 0) w[i].v = val;
    jp_ws(p);
    if (p->s >= p->end) return -1;
    if (*p->s == ',') { p->s++; continue; }
    if (*p->s == '}') { p->s++; p->depth--; return 0; }
    return -1;
  }
}

static int jp_array(struct jp *p) {
  p->s++;
  if (++p->depth > 32) return -1;
  jp_ws(p);
  if (p->s < p->end && *p->s == ']') { p->s++; p->depth--; return 0; }
  for (;;) {
    jp_ws(p);
    if (jp_value(p, NULL) < 0) return -1;
    jp_ws(p);
    if (p->s >= p->end) return -1;
    if (*p->s == ',') { p->s++; continue; }
    if (*p->s == ']') { p->s++; p->depth--; return 0; }
    return -1;
  }
}

static int jp_value(struct jp *p, struct jval *out) {
  if (p->s >= p->end) return -1;
  const char *start = p->s;
  int rc;
  switch (*p->s) {
  case '"': return jp_string(p, out);
  case '{':
    rc = jp_object(p, NULL, 0);
    if (rc == 0 && out) { out->p = start; out->n = (size_t)(p->s - start); out->type = J_OBJECT; }
    return rc;
  case '[':
    rc = jp_array(p);
    if (rc == 0 && out) { out->p = start; out->n = (size_t)(p->s - start); out->type = J_ARRAY; }
    return rc;
  case 't': return jp_literal(p, "true", 4, J_TRUE, out);
  case 'f': return jp_literal(p, "false", 5, J_FALSE, out);
  case 'n': return jp_literal(p, "null", 4, J_NULL, out);
  default: return jp_number(p, out);
  }
}

/* Parses a whole document. Returns the top-level type, or -1 if it is not valid JSON.
 * When the document is an object, the wanted members are filled in. */
static int json_doc(const char *s, size_t n, struct jwant *w, int nw) {
  struct jp p = {s, s + n, 0};
  for (int i = 0; i < nw; i++) w[i].v.type = J_NONE;
  jp_ws(&p);
  if (p.s >= p.end) return -1;
  int type;
  if (*p.s == '{') {
    if (jp_object(&p, w, nw) < 0) return -1;
    type = J_OBJECT;
  } else {
    struct jval v;
    if (jp_value(&p, &v) < 0) return -1;
    type = v.type;
  }
  jp_ws(&p);
  return p.s == p.end ? type : -1;
}

static inline int put_utf8(uint8_t *o, uint32_t cp) {
  if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
  if (cp < 0x800) { o[0] = (uint8_t)(0xC0 | (cp >> 6)); o[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (uint8_t)(0xE0 | (cp >> 12)); o[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (uint8_t)(0xF0 | (cp >> 18)); o[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (uint8_t)(0x80 | (cp & 0x3F));
  return 4;
}

/* Unescapes an already-validated string body into `out` (cap bytes). Returns the length, or -1 if
 * it does not fit. */
static long json_unescape(const char *s, size_t n, uint8_t *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n;) {
    if (s[i] != '\\') {
      if (o >= cap) return -1;
      out[o++] = (uint8_t)s[i++];
      continue;
    }
    char e = s[i + 1];
    if (e == 'u') {
      uint32_t cp = (uint32_t)(jp_hex(s[i + 2]) << 12 | jp_hex(s[i + 3]) << 8 | jp_hex(s[i + 4]) << 4 | jp_hex(s[i + 5]));
      i += 6;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u') {
        uint32_t lo = (uint32_t)(jp_hex(s[i + 2]) << 12 | jp_hex(s[i + 3]) << 8 | jp_hex(s[i + 4]) << 4 | jp_hex(s[i + 5]));
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 6;
        }
      }
      if (o + 4 > cap) return -1;
      o += (size_t)put_utf8(out + o, cp);
      continue;
    }
    if (o >= cap) return -1;
    switch (e) {
    case 'b': out[o++] = '\b'; break;
    case 'f': out[o++] = '\f'; break;
    case 'n': out[o++] = '\n'; break;
    case 'r': out[o++] = '\r'; break;
    case 't': out[o++] = '\t'; break;
    default: out[o++] = (uint8_t)e; break; /* " \ / */
    }
    i += 2;
  }
  return (long)o;
}

/* ---------------- writer ---------------- */

struct ob {
  uint8_t *b;
  size_t n, cap;
  int err; /* set when a write did not fit */
};

static inline void ob_init(struct ob *o, uint8_t *b, size_t cap) { o->b = b; o->n = 0; o->cap = cap; o->err = 0; }

static inline void ob_bytes(struct ob *o, const void *p, size_t n) {
  if (o->n + n > o->cap) { o->err = 1; return; }
  memcpy(o->b + o->n, p, n);
  o->n += n;
}
#define ob_lit(o, s) ob_bytes((o), (s), sizeof(s) - 1)
static inline void ob_char(struct ob *o, char c) {
  if (o->n >= o->cap) { o->err = 1; return; }
  o->b[o->n++] = (uint8_t)c;
}

static const char digits2[201] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
                                 "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
                                 "8081828384858687888990919293949596979899";

static inline unsigned u64_to_str(char *dst, uint64_t v) {
  char tmp[20];
  unsigned i = 20;
  while (v >= 100) {
    unsigned r = (unsigned)(v % 100);
    v /= 100;
    i -= 2;
    memcpy(tmp + i, digits2 + r * 2, 2);
  }
  if (v >= 10) {
    i -= 2;
    memcpy(tmp + i, digits2 + v * 2, 2);
  } else {
    tmp[--i] = (char)('0' + v);
  }
  memcpy(dst, tmp + i, 20 - i);
  return 20 - i;
}

static inline void ob_u64(struct ob *o, uint64_t v) {
  if (o->n + 20 > o->cap) { o->err = 1; return; }
  o->n += u64_to_str((char *)o->b + o->n, v);
}

/* Escapes a JSON string body: only `"`, `\` and control characters. Scans 8 bytes at a time and
 * copies clean runs with one memcpy, which is the common case for every post in the seed. */
static void ob_json_str(struct ob *o, const uint8_t *s, size_t n) {
  if (o->n + n + 2 > o->cap) { o->err = 1; return; }
  o->b[o->n++] = '"';
  size_t i = 0;
  while (i < n) {
    size_t run = i;
    while (run + 8 <= n) {
      uint64_t w;
      memcpy(&w, s + run, 8);
      /* any byte < 0x20, == '"' (0x22) or == '\\' (0x5c)? */
      uint64_t lt20 = (w - 0x2020202020202020ULL) & ~w & 0x8080808080808080ULL;
      uint64_t q = w ^ 0x2222222222222222ULL;
      uint64_t isq = (q - 0x0101010101010101ULL) & ~q & 0x8080808080808080ULL;
      uint64_t bs = w ^ 0x5c5c5c5c5c5c5c5cULL;
      uint64_t isbs = (bs - 0x0101010101010101ULL) & ~bs & 0x8080808080808080ULL;
      if (lt20 | isq | isbs) break;
      run += 8;
    }
    while (run < n && s[run] >= 0x20 && s[run] != '"' && s[run] != '\\') run++;
    if (run > i) {
      if (o->n + (run - i) > o->cap) { o->err = 1; return; }
      memcpy(o->b + o->n, s + i, run - i);
      o->n += run - i;
      i = run;
    }
    if (i >= n) break;
    uint8_t c = s[i++];
    if (o->n + 6 > o->cap) { o->err = 1; return; }
    o->b[o->n++] = '\\';
    switch (c) {
    case '"': o->b[o->n++] = '"'; break;
    case '\\': o->b[o->n++] = '\\'; break;
    case '\n': o->b[o->n++] = 'n'; break;
    case '\r': o->b[o->n++] = 'r'; break;
    case '\t': o->b[o->n++] = 't'; break;
    case '\b': o->b[o->n++] = 'b'; break;
    case '\f': o->b[o->n++] = 'f'; break;
    default: {
      static const char hx[] = "0123456789abcdef";
      o->b[o->n++] = 'u'; o->b[o->n++] = '0'; o->b[o->n++] = '0';
      o->b[o->n++] = (uint8_t)hx[c >> 4]; o->b[o->n++] = (uint8_t)hx[c & 15];
    }
    }
  }
  if (o->n >= o->cap) { o->err = 1; return; }
  o->b[o->n++] = '"';
}

#endif
