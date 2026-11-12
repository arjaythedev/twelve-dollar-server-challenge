/* json.h: an output builder (compact JSON straight into the response buffer) and a validating
 * reader for request bodies and token payloads. */
#ifndef JSON_H
#define JSON_H

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

/* ---------- output ---------- */

struct ob {
  uint8_t *p;
  size_t n, cap;
  int err; /* set when the buffer ran out; the output is then unusable */
};

static inline void ob_init(struct ob *o, uint8_t *p, size_t cap) {
  o->p = p;
  o->n = 0;
  o->cap = cap;
  o->err = 0;
}
static inline int ob_room(struct ob *o, size_t k) {
  if (o->n + k > o->cap) { o->err = 1; return 0; }
  return 1;
}
static inline void ob_bytes(struct ob *o, const void *s, size_t k) {
  if (!ob_room(o, k)) return;
  memcpy(o->p + o->n, s, k);
  o->n += k;
}
#define ob_lit(o, s) ob_bytes((o), (s), sizeof(s) - 1)
static inline void ob_char(struct ob *o, char c) {
  if (!ob_room(o, 1)) return;
  o->p[o->n++] = (uint8_t)c;
}

static const char DIGITS2[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static inline unsigned dec_len(uint64_t v) {
  unsigned n = 1;
  for (;;) {
    if (v < 10) return n;
    if (v < 100) return n + 1;
    if (v < 1000) return n + 2;
    if (v < 10000) return n + 3;
    v /= 10000;
    n += 4;
  }
}
/* Writes the decimal digits of v at out, right to left from the known length; returns the count. */
static inline unsigned u64_to_dec(uint64_t v, char *out) {
  unsigned n = dec_len(v), i = n;
  while (v >= 100) {
    unsigned d = (unsigned)(v % 100);
    v /= 100;
    i -= 2;
    out[i] = DIGITS2[d * 2];
    out[i + 1] = DIGITS2[d * 2 + 1];
  }
  if (v >= 10) {
    out[0] = DIGITS2[v * 2];
    out[1] = DIGITS2[v * 2 + 1];
  } else {
    out[0] = (char)('0' + v);
  }
  return n;
}
static inline void ob_u64(struct ob *o, uint64_t v) {
  if (!ob_room(o, 20)) return;
  o->n += u64_to_dec(v, (char *)o->p + o->n);
}
static inline void ob_i64(struct ob *o, int64_t v) {
  if (v < 0) {
    ob_char(o, '-');
    ob_u64(o, (uint64_t)(-(v + 1)) + 1);
  } else {
    ob_u64(o, (uint64_t)v);
  }
}

/* Bytes that JSON strings must escape: control characters, '"' and '\\'. Scanned 32 at a time. */
static inline int json_needs_escape(const uint8_t *s, size_t n) {
  size_t i = 0;
#ifdef __AVX2__
  const __m256i lo = _mm256_set1_epi8(0x1f), q = _mm256_set1_epi8('"'), b = _mm256_set1_epi8('\\');
  for (; i + 32 <= n; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, b));
    /* unsigned v <= 0x1f  <=>  max(v, 0x1f) == 0x1f */
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(_mm256_max_epu8(v, lo), lo));
    if (_mm256_movemask_epi8(m)) return 1;
  }
#endif
  for (; i < n; i++) {
    uint8_t c = s[i];
    if (c < 0x20 || c == '"' || c == '\\') return 1;
  }
  return 0;
}

static void ob_json_str_slow(struct ob *o, const uint8_t *s, size_t n) {
  static const char HEX[] = "0123456789abcdef";
  if (!ob_room(o, 6 * n + 2)) return;
  uint8_t *d = o->p + o->n;
  *d++ = '"';
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') {
      *d++ = c;
      continue;
    }
    *d++ = '\\';
    switch (c) {
    case '"': *d++ = '"'; break;
    case '\\': *d++ = '\\'; break;
    case '\n': *d++ = 'n'; break;
    case '\r': *d++ = 'r'; break;
    case '\t': *d++ = 't'; break;
    case '\b': *d++ = 'b'; break;
    case '\f': *d++ = 'f'; break;
    default:
      *d++ = 'u'; *d++ = '0'; *d++ = '0';
      *d++ = (uint8_t)HEX[c >> 4];
      *d++ = (uint8_t)HEX[c & 15];
    }
  }
  *d++ = '"';
  o->n = (size_t)(d - o->p);
}

/* A JSON string, scanned and copied in one pass, 32 bytes at a time (the last chunk overlaps
 * the previous one, which needs 32 bytes of slack past the string's end in the buffer). The slow
 * path takes over, from the start, at the first byte that needs escaping (the usual case has
 * none). */
static inline void ob_json_str(struct ob *o, const uint8_t *s, size_t n) {
  if (!ob_room(o, n + 2 + 32)) return;
  uint8_t *d = o->p + o->n;
  d[0] = '"';
  d++;
  size_t i = 0;
#ifdef __AVX2__
  const __m256i lo = _mm256_set1_epi8(0x1f), q = _mm256_set1_epi8('"'), bs = _mm256_set1_epi8('\\');
  for (; i + 32 <= n; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(_mm256_max_epu8(v, lo), lo));
    if (_mm256_movemask_epi8(m)) { ob_json_str_slow(o, s, n); return; }
    _mm256_storeu_si256((__m256i *)(d + i), v);
  }
  if (i < n && n >= 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + n - 32));
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(_mm256_max_epu8(v, lo), lo));
    if (_mm256_movemask_epi8(m)) { ob_json_str_slow(o, s, n); return; }
    _mm256_storeu_si256((__m256i *)(d + n - 32), v);
    i = n;
  } else if (n >= 16) { /* 16..31 bytes (timestamps, names): two overlapping 16-byte steps */
    const __m128i lo16 = _mm_set1_epi8(0x1f), q16 = _mm_set1_epi8('"'), bs16 = _mm_set1_epi8('\\');
    for (size_t k = 0; k < 2; k++) {
      size_t at = k ? n - 16 : 0;
      __m128i v = _mm_loadu_si128((const __m128i *)(s + at));
      __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, q16), _mm_cmpeq_epi8(v, bs16));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(_mm_max_epu8(v, lo16), lo16));
      if (_mm_movemask_epi8(m)) { ob_json_str_slow(o, s, n); return; }
      _mm_storeu_si128((__m128i *)(d + at), v);
    }
    i = n;
  }
#endif
  for (; i < n; i++) {
    uint8_t c = s[i];
    if (c < 0x20 || c == '"' || c == '\\') { ob_json_str_slow(o, s, n); return; }
    d[i] = c;
  }
  d[n] = '"';
  o->n += n + 2;
}

/* ---------- input ---------- */

/* A member value of the top-level object. */
enum { JV_NONE = 0, JV_STRING, JV_NUMBER, JV_OTHER };
struct jv {
  int type;
  const uint8_t *p; /* string: content between the quotes (still escaped); number: the digits */
  uint32_t n;
  int escaped; /* string: contains backslash escapes */
};

struct jp {
  const uint8_t *p, *end;
  int depth;
};

static inline void jp_ws(struct jp *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static inline int jp_hex(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  c |= 0x20;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* Validates a string starting after its opening quote; leaves p after the closing quote. */
static int jp_string(struct jp *j, struct jv *v) {
  const uint8_t *s = j->p;
  int esc = 0;
  for (;;) {
    if (j->p >= j->end) return -1;
    uint8_t c = *j->p;
    if (c == '"') break;
    if (c < 0x20) return -1;
    if (c == '\\') {
      esc = 1;
      j->p++;
      if (j->p >= j->end) return -1;
      c = *j->p;
      if (c == 'u') {
        if (j->end - j->p < 5) return -1;
        for (int k = 1; k <= 4; k++)
          if (jp_hex(j->p[k]) < 0) return -1;
        j->p += 4;
      } else if (!(c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't')) {
        return -1;
      }
    }
    j->p++;
  }
  if (v) {
    v->type = JV_STRING;
    v->p = s;
    v->n = (uint32_t)(j->p - s);
    v->escaped = esc;
  }
  j->p++; /* closing quote */
  return 0;
}

static int jp_value(struct jp *j, struct jv *v);

static int jp_number(struct jp *j, struct jv *v) {
  const uint8_t *s = j->p;
  if (j->p < j->end && *j->p == '-') j->p++;
  if (j->p >= j->end) return -1;
  if (*j->p == '0') {
    j->p++;
  } else if (*j->p >= '1' && *j->p <= '9') {
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  } else {
    return -1;
  }
  if (j->p < j->end && *j->p == '.') {
    j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return -1;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return -1;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  if (v) {
    v->type = JV_NUMBER;
    v->p = s;
    v->n = (uint32_t)(j->p - s);
    v->escaped = 0;
  }
  return 0;
}

static int jp_object(struct jp *j, int (*on_member)(void *, const uint8_t *, uint32_t, int, struct jv *), void *ud);

static int jp_value(struct jp *j, struct jv *v) {
  jp_ws(j);
  if (j->p >= j->end) return -1;
  uint8_t c = *j->p;
  if (v) v->type = JV_OTHER;
  if (c == '"') {
    j->p++;
    return jp_string(j, v);
  }
  if (c == '{') {
    if (++j->depth > 64) return -1;
    int rc = jp_object(j, NULL, NULL);
    j->depth--;
    return rc;
  }
  if (c == '[') {
    if (++j->depth > 64) return -1;
    j->p++;
    jp_ws(j);
    if (j->p < j->end && *j->p == ']') { j->p++; j->depth--; return 0; }
    for (;;) {
      if (jp_value(j, NULL)) return -1;
      jp_ws(j);
      if (j->p >= j->end) return -1;
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == ']') { j->p++; break; }
      return -1;
    }
    j->depth--;
    return 0;
  }
  if (c == 't' && j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) { j->p += 4; return 0; }
  if (c == 'f' && j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; return 0; }
  if (c == 'n' && j->end - j->p >= 4 && !memcmp(j->p, "null", 4)) { j->p += 4; return 0; }
  if (c == '-' || (c >= '0' && c <= '9')) return jp_number(j, v);
  return -1;
}

/* Parses an object at j->p; calls on_member(ud, key, keylen, key_escaped, value) for each
 * top-level member when given. */
static int jp_object(struct jp *j, int (*on_member)(void *, const uint8_t *, uint32_t, int, struct jv *), void *ud) {
  jp_ws(j);
  if (j->p >= j->end || *j->p != '{') return -1;
  j->p++;
  jp_ws(j);
  if (j->p < j->end && *j->p == '}') { j->p++; return 0; }
  for (;;) {
    jp_ws(j);
    if (j->p >= j->end || *j->p != '"') return -1;
    j->p++;
    struct jv key;
    if (jp_string(j, &key)) return -1;
    jp_ws(j);
    if (j->p >= j->end || *j->p != ':') return -1;
    j->p++;
    struct jv val;
    if (jp_value(j, &val)) return -1;
    if (on_member && on_member(ud, key.p, key.n, key.escaped, &val)) return -1;
    jp_ws(j);
    if (j->p >= j->end) return -1;
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == '}') { j->p++; return 0; }
    return -1;
  }
}

/* Validates a whole document that must be an object; reports its top-level members. 0 ok, -1 malformed. */
static int json_parse_object(const uint8_t *p, uint32_t n, int (*on_member)(void *, const uint8_t *, uint32_t, int, struct jv *),
                             void *ud) {
  struct jp j = {p, p + n, 0};
  if (jp_object(&j, on_member, ud)) return -1;
  jp_ws(&j);
  return j.p == j.end ? 0 : -1;
}

static inline unsigned utf8_put(uint8_t *d, uint32_t cp) {
  if (cp < 0x80) { d[0] = (uint8_t)cp; return 1; }
  if (cp < 0x800) { d[0] = (uint8_t)(0xc0 | cp >> 6); d[1] = (uint8_t)(0x80 | (cp & 0x3f)); return 2; }
  if (cp < 0x10000) {
    d[0] = (uint8_t)(0xe0 | cp >> 12); d[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f)); d[2] = (uint8_t)(0x80 | (cp & 0x3f));
    return 3;
  }
  d[0] = (uint8_t)(0xf0 | cp >> 18); d[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
  d[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f)); d[3] = (uint8_t)(0x80 | (cp & 0x3f));
  return 4;
}

/* Decodes the escapes of a validated JSON string into d (at least n bytes). Returns the length.
 * Lone surrogates become U+FFFD, as JSON.parse does. */
static uint32_t json_unescape(const uint8_t *s, uint32_t n, uint8_t *d) {
  uint32_t o = 0;
  for (uint32_t i = 0; i < n;) {
    uint8_t c = s[i++];
    if (c != '\\') { d[o++] = c; continue; }
    c = s[i++];
    switch (c) {
    case 'n': d[o++] = '\n'; break;
    case 'r': d[o++] = '\r'; break;
    case 't': d[o++] = '\t'; break;
    case 'b': d[o++] = '\b'; break;
    case 'f': d[o++] = '\f'; break;
    case 'u': {
      uint32_t cp = (uint32_t)(jp_hex(s[i]) << 12 | jp_hex(s[i + 1]) << 8 | jp_hex(s[i + 2]) << 4 | jp_hex(s[i + 3]));
      i += 4;
      if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u') {
        uint32_t lo = (uint32_t)(jp_hex(s[i + 2]) << 12 | jp_hex(s[i + 3]) << 8 | jp_hex(s[i + 4]) << 4 | jp_hex(s[i + 5]));
        if (lo >= 0xdc00 && lo <= 0xdfff) {
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          i += 6;
        } else {
          cp = 0xfffd;
        }
      } else if (cp >= 0xd800 && cp <= 0xdfff) {
        cp = 0xfffd;
      }
      o += utf8_put(d + o, cp);
      break;
    }
    default: d[o++] = c; /* " \ / */
    }
  }
  return o;
}

/* JavaScript's String.prototype.trim() whitespace, in UTF-8. Returns the length of the prefix
 * that is one whitespace character (0 if none). */
static inline unsigned js_space_len(const uint8_t *s, uint32_t n) {
  if (n == 0) return 0;
  uint8_t c = s[0];
  if (c == ' ' || (c >= 0x09 && c <= 0x0d)) return 1;
  if (c < 0x80) return 0;
  if (n >= 2 && c == 0xc2 && s[1] == 0xa0) return 2;
  if (n >= 3) {
    if (c == 0xe1 && s[1] == 0x9a && s[2] == 0x80) return 3;
    if (c == 0xe2 && s[1] == 0x80 && ((s[2] >= 0x80 && s[2] <= 0x8a) || s[2] == 0xa8 || s[2] == 0xa9 || s[2] == 0xaf)) return 3;
    if (c == 0xe2 && s[1] == 0x81 && s[2] == 0x9f) return 3;
    if (c == 0xe3 && s[1] == 0x80 && s[2] == 0x80) return 3;
    if (c == 0xef && s[1] == 0xbb && s[2] == 0xbf) return 3;
  }
  return 0;
}
/* The same, for the last character. */
static inline unsigned js_space_len_back(const uint8_t *s, uint32_t n) {
  if (n == 0) return 0;
  uint8_t c = s[n - 1];
  if (c == ' ' || (c >= 0x09 && c <= 0x0d)) return 1;
  if (c < 0x80) return 0;
  if (n >= 2 && js_space_len(s + n - 2, 2) == 2) return 2;
  if (n >= 3 && js_space_len(s + n - 3, 3) == 3) return 3;
  return 0;
}
static inline void js_trim(const uint8_t **s, uint32_t *n) {
  unsigned k;
  while ((k = js_space_len(*s, *n)) != 0) { *s += k; *n -= k; }
  while ((k = js_space_len_back(*s, *n)) != 0) *n -= k;
}
/* Code points (bytes that are not UTF-8 continuation bytes). */
static inline uint32_t utf8_count(const uint8_t *s, uint32_t n) {
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++) c += (s[i] & 0xc0) != 0x80;
  return c;
}

#endif
