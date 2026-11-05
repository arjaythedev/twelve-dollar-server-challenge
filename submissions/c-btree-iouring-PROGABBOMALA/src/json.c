#include "rt.h"

typedef struct { const uint8_t *p, *end; int depth; } jscan;

static void js_ws(jscan *s) {
  while (s->p < s->end) {
    uint8_t c = *s->p;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    s->p++;
  }
}
static int hexv(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (uint8_t)(c | 0x20);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
static int utf8_len(const uint8_t *p, const uint8_t *end) {
  uint8_t c = p[0];
  if (c < 0x80) return 1;
  if (c < 0xC2) return 0;
  if (c < 0xE0) return (end - p >= 2 && (p[1] & 0xC0) == 0x80) ? 2 : 0;
  if (c < 0xF0) {
    if (end - p < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;
    if (c == 0xED && p[1] >= 0xA0) return 0;
    return 3;
  }
  if (c < 0xF5) {
    if (end - p < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0;
    if (c == 0xF4 && p[1] >= 0x90) return 0;
    return 4;
  }
  return 0;
}
static int js_string(jscan *s, const uint8_t **v, uint32_t *n, uint8_t *escaped) {
  const uint8_t *p = s->p + 1, *start = p;
  uint8_t esc = 0;
  for (;;) {
    if (p >= s->end) return -1;
    uint8_t c = *p;
    if (c == '"') break;
    if (c == '\\') {
      esc = 1;
      if (p + 1 >= s->end) return -1;
      uint8_t e = p[1];
      if (e == 'u') {
        if (s->end - p < 6) return -1;
        for (int i = 2; i < 6; i++) if (hexv(p[i]) < 0) return -1;
        p += 6;
      } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
        p += 2;
      } else return -1;
      continue;
    }
    if (c < 0x20) return -1;
    if (c < 0x80) { p++; continue; }
    int l = utf8_len(p, s->end);
    if (!l) return -1;
    p += l;
  }
  if (v) { *v = start; *n = (uint32_t)(p - start); *escaped = esc; }
  s->p = p + 1;
  return 0;
}
static int js_number(jscan *s) {
  const uint8_t *p = s->p;
  if (p < s->end && *p == '-') p++;
  if (p >= s->end) return -1;
  if (*p == '0') p++;
  else if (*p >= '1' && *p <= '9') { while (p < s->end && *p >= '0' && *p <= '9') p++; }
  else return -1;
  if (p < s->end && *p == '.') {
    p++;
    if (p >= s->end || *p < '0' || *p > '9') return -1;
    while (p < s->end && *p >= '0' && *p <= '9') p++;
  }
  if (p < s->end && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < s->end && (*p == '+' || *p == '-')) p++;
    if (p >= s->end || *p < '0' || *p > '9') return -1;
    while (p < s->end && *p >= '0' && *p <= '9') p++;
  }
  s->p = p;
  return 0;
}
static int js_value(jscan *s, uint8_t *type, const uint8_t **v, uint32_t *n, uint8_t *escaped);
static int js_lit(jscan *s, const char *lit, uint32_t len) {
  if ((uint32_t)(s->end - s->p) < len || memcmp(s->p, lit, len)) return -1;
  s->p += len;
  return 0;
}
static int js_container(jscan *s, int object) {
  if (++s->depth > 32) return -1;
  s->p++;
  js_ws(s);
  uint8_t close = object ? '}' : ']';
  if (s->p < s->end && *s->p == close) { s->p++; s->depth--; return 0; }
  for (;;) {
    if (object) {
      if (s->p >= s->end || *s->p != '"' || js_string(s, 0, 0, 0)) return -1;
      js_ws(s);
      if (s->p >= s->end || *s->p != ':') return -1;
      s->p++;
      js_ws(s);
    }
    uint8_t t; const uint8_t *v; uint32_t n; uint8_t e;
    if (js_value(s, &t, &v, &n, &e)) return -1;
    js_ws(s);
    if (s->p >= s->end) return -1;
    if (*s->p == ',') { s->p++; js_ws(s); continue; }
    if (*s->p == close) { s->p++; s->depth--; return 0; }
    return -1;
  }
}
static int js_value(jscan *s, uint8_t *type, const uint8_t **v, uint32_t *n, uint8_t *escaped) {
  if (s->p >= s->end) return -1;
  uint8_t c = *s->p;
  const uint8_t *start = s->p;
  switch (c) {
  case '"': *type = JT_STRING; return js_string(s, v, n, escaped);
  case '{': *type = JT_OTHER; return js_container(s, 1);
  case '[': *type = JT_OTHER; return js_container(s, 0);
  case 't': *type = JT_OTHER; return js_lit(s, "true", 4);
  case 'f': *type = JT_OTHER; return js_lit(s, "false", 5);
  case 'n': *type = JT_OTHER; return js_lit(s, "null", 4);
  default:
    *type = JT_NUMBER;
    if (js_number(s)) return -1;
    if (v) { *v = start; *n = (uint32_t)(s->p - start); }
    return 0;
  }
}

int json_members(const uint8_t *p, uint32_t len, jmember *m, int nm) {
  for (int i = 0; i < nm; i++) m[i].type = JT_NONE;
  jscan s = {p, p + len, 0};
  js_ws(&s);
  if (s.p >= s.end) return -1;
  if (*s.p != '{') {
    uint8_t t; const uint8_t *v; uint32_t n; uint8_t e;
    if (js_value(&s, &t, &v, &n, &e)) return -1;
    js_ws(&s);
    return s.p == s.end ? 1 : -1;
  }
  s.p++;
  s.depth = 1;
  js_ws(&s);
  if (s.p < s.end && *s.p == '}') { s.p++; goto done; }
  for (;;) {
    const uint8_t *k; uint32_t kn; uint8_t kesc = 0;
    if (s.p >= s.end || *s.p != '"' || js_string(&s, &k, &kn, &kesc)) return -1;
    js_ws(&s);
    if (s.p >= s.end || *s.p != ':') return -1;
    s.p++;
    js_ws(&s);
    uint8_t t; const uint8_t *v = 0; uint32_t n = 0; uint8_t e = 0;
    if (js_value(&s, &t, &v, &n, &e)) return -1;
    uint8_t tmp[64];
    const uint8_t *kk = k;
    uint32_t klen = kn;
    if (kesc) {
      if (kn > sizeof tmp) klen = 0;
      else { klen = json_unescape(k, kn, tmp); kk = tmp; }
    }
    if (klen) {
      for (int i = 0; i < nm; i++)
        if (m[i].key_len == klen && !memcmp(m[i].key, kk, klen)) {
          m[i].type = t; m[i].v = v; m[i].v_len = n; m[i].escaped = e;
        }
    }
    js_ws(&s);
    if (s.p >= s.end) return -1;
    if (*s.p == ',') { s.p++; js_ws(&s); continue; }
    if (*s.p == '}') { s.p++; break; }
    return -1;
  }
done:
  js_ws(&s);
  return s.p == s.end ? 0 : -1;
}

static uint8_t *put_utf8(uint8_t *d, uint32_t cp) {
  if (cp < 0x80) *d++ = (uint8_t)cp;
  else if (cp < 0x800) { *d++ = (uint8_t)(0xC0 | cp >> 6); *d++ = (uint8_t)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    *d++ = (uint8_t)(0xE0 | cp >> 12);
    *d++ = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    *d++ = (uint8_t)(0x80 | (cp & 0x3F));
  } else {
    *d++ = (uint8_t)(0xF0 | cp >> 18);
    *d++ = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    *d++ = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    *d++ = (uint8_t)(0x80 | (cp & 0x3F));
  }
  return d;
}
uint32_t json_unescape(const uint8_t *s, uint32_t n, uint8_t *dst) {
  uint8_t *d = dst;
  const uint8_t *end = s + n;
  while (s < end) {
    if (*s != '\\') { *d++ = *s++; continue; }
    uint8_t e = s[1];
    if (e != 'u') {
      uint8_t m = e;
      if (e == 'b') m = 8;
      else if (e == 'f') m = 12;
      else if (e == 'n') m = 10;
      else if (e == 'r') m = 13;
      else if (e == 't') m = 9;
      *d++ = m;
      s += 2;
      continue;
    }
    uint32_t cp = (uint32_t)(hexv(s[2]) << 12 | hexv(s[3]) << 8 | hexv(s[4]) << 4 | hexv(s[5]));
    s += 6;
    if (cp >= 0xD800 && cp < 0xDC00) {
      if (end - s >= 6 && s[0] == '\\' && s[1] == 'u') {
        uint32_t lo = (uint32_t)(hexv(s[2]) << 12 | hexv(s[3]) << 8 | hexv(s[4]) << 4 | hexv(s[5]));
        if (lo >= 0xDC00 && lo < 0xE000) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          s += 6;
        } else cp = 0xFFFD;
      } else cp = 0xFFFD;
    } else if (cp >= 0xDC00 && cp < 0xE000) cp = 0xFFFD;
    d = put_utf8(d, cp);
  }
  return (uint32_t)(d - dst);
}
