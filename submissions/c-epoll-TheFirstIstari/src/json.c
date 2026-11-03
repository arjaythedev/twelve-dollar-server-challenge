#include "json.h"
#include <string.h>

static const char *ws(const char *p, const char *e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

// Forward declarations for recursive descent (depth-capped at 64).
static const char *value(const char *p, const char *e, int depth,
                         const char **body, size_t *body_len, int top);

static const char *str_end(const char *p, const char *e) { // p at '"'
  p++;
  while (p < e) {
    unsigned char ch = (unsigned char)*p;
    if (ch == '\\') { p += 2; continue; }
    if (ch == '"') return p + 1;
    if (ch < 0x20) return NULL;
    p++;
  }
  return NULL;
}

static int hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// Validate escapes inside s[0..n) (string content, no quotes). 0 ok, -1 bad.
static int check_escapes(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] != '\\') continue;
    if (++i >= n) return -1;
    switch (s[i]) {
      case '"': case '\\': case '/': case 'b': case 'f':
      case 'n': case 'r': case 't': break;
      case 'u': {
        if (i + 4 >= n) return -1;
        unsigned cp = 0;
        for (int k = 1; k <= 4; k++) {
          int h = hexval(s[i + k]);
          if (h < 0) return -1;
          cp = (cp << 4) | (unsigned)h;
        }
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (i + 6 >= n || s[i+1] != '\\' || s[i+2] != 'u') return -1;
          unsigned lo = 0;
          for (int k = 3; k <= 6; k++) {
            int h = hexval(s[i + k]);
            if (h < 0) return -1;
            lo = (lo << 4) | (unsigned)h;
          }
          if (lo < 0xDC00 || lo > 0xDFFF) return -1;
          i += 6;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) return -1;
        break;
      }
      default: return -1;
    }
  }
  return 0;
}

static const char *number_end(const char *p, const char *e) {
  const char *s = p;
  if (p < e && *p == '-') p++;
  if (p >= e) return NULL;
  if (*p == '0') p++;
  else if (*p >= '1' && *p <= '9') { while (p < e && *p >= '0' && *p <= '9') p++; }
  else return NULL;
  if (p < e && *p == '.') {
    p++;
    if (p >= e || *p < '0' || *p > '9') return NULL;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p >= e || *p < '0' || *p > '9') return NULL;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  return (p == s || (p == s + 1 && *s == '-')) ? NULL : p;
}

static const char *lit(const char *p, const char *e, const char *w) {
  size_t n = 0;
  while (w[n]) n++;
  if ((size_t)(e - p) < n || memcmp(p, w, n) != 0) return NULL;
  return p + n;
}

static const char *value(const char *p, const char *e, int depth,
                         const char **body, size_t *body_len, int top) {
  if (depth > 64) return NULL;
  p = ws(p, e);
  if (p >= e) return NULL;
  char ch = *p;
  if (ch == '"') {
    const char *q = str_end(p, e);
    if (!q) return NULL;
    if (check_escapes(p + 1, (size_t)(q - p - 2)) != 0) return NULL;
    return q;
  }
  if (ch == '{') {
    p++;
    p = ws(p, e);
    if (p < e && *p == '}') return p + 1;
    for (;;) {
      p = ws(p, e);
      if (p >= e || *p != '"') return NULL;
      const char *ks = p;
      p = str_end(p, e);
      if (!p) return NULL;
      size_t klen = (size_t)(p - ks - 2);
      int isbody = top && klen == 4 && memcmp(ks + 1, "body", 4) == 0;
      p = ws(p, e);
      if (p >= e || *p != ':') return NULL;
      p++;
      if (isbody) {
        const char *vs = ws(p, e);
        if (vs < e && *vs == '"') {
          const char *ve = str_end(vs, e);
          if (!ve || check_escapes(vs + 1, (size_t)(ve - vs - 2)) != 0) return NULL;
          *body = vs + 1; *body_len = (size_t)(ve - vs - 2);
          p = ve;
        } else {
          p = value(p, e, depth + 1, body, body_len, 0);
          if (!p) return NULL;
        }
      } else {
        p = value(p, e, depth + 1, body, body_len, 0);
        if (!p) return NULL;
      }
      p = ws(p, e);
      if (p >= e) return NULL;
      if (*p == ',') { p++; continue; }
      if (*p == '}') return p + 1;
      return NULL;
    }
  }
  if (ch == '[') {
    p++;
    p = ws(p, e);
    if (p < e && *p == ']') return p + 1;
    for (;;) {
      p = value(p, e, depth + 1, body, body_len, 0);
      if (!p) return NULL;
      p = ws(p, e);
      if (p >= e) return NULL;
      if (*p == ',') { p++; continue; }
      if (*p == ']') return p + 1;
      return NULL;
    }
  }
  if (ch == 't') return lit(p, e, "true");
  if (ch == 'f') return lit(p, e, "false");
  if (ch == 'n') return lit(p, e, "null");
  if (ch == '-' || (ch >= '0' && ch <= '9')) return number_end(p, e);
  return NULL;
}

int json_unescape(const char *val, size_t vlen, char *out, size_t cap) {
  const char *s = val; size_t n = vlen;
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char ch = s[i];
    if (ch != '\\') {
      if (o + 1 > cap) return -1;
      out[o++] = ch;
      continue;
    }
    if (++i >= n) return -1;
    char w;
    switch (s[i]) {
      case '"': case '\\': case '/': w = s[i]; if (o+1 > cap) return -1; out[o++] = w; break;
      case 'b': if (o+1 > cap) return -1; out[o++] = '\b'; break;
      case 'f': if (o+1 > cap) return -1; out[o++] = '\f'; break;
      case 'n': if (o+1 > cap) return -1; out[o++] = '\n'; break;
      case 'r': if (o+1 > cap) return -1; out[o++] = '\r'; break;
      case 't': if (o+1 > cap) return -1; out[o++] = '\t'; break;
      case 'u': {
        unsigned cp = 0;
        if (i + 4 >= n) return -1;
        for (int k = 1; k <= 4; k++) {
          int h = hexval(s[i+k]);
          if (h < 0) return -1;
          cp = (cp << 4) | (unsigned)h;
        }
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          unsigned lo = 0;
          if (i + 6 >= n || s[i+1] != '\\' || s[i+2] != 'u') return -1;
          for (int k = 3; k <= 6; k++) {
            int h = hexval(s[i+k]);
            if (h < 0) return -1;
            lo = (lo << 4) | (unsigned)h;
          }
          if (lo < 0xDC00 || lo > 0xDFFF) return -1;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 6;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) return -1;
        char tmp[4]; int k;
        if (cp < 0x80) { tmp[0] = (char)cp; k = 1; }
        else if (cp < 0x800) { tmp[0]=(char)(0xC0|(cp>>6)); tmp[1]=(char)(0x80|(cp&63)); k=2; }
        else if (cp < 0x10000) { tmp[0]=(char)(0xE0|(cp>>12)); tmp[1]=(char)(0x80|((cp>>6)&63)); tmp[2]=(char)(0x80|(cp&63)); k=3; }
        else { tmp[0]=(char)(0xF0|(cp>>18)); tmp[1]=(char)(0x80|((cp>>12)&63)); tmp[2]=(char)(0x80|((cp>>6)&63)); tmp[3]=(char)(0x80|(cp&63)); k=4; }
        if (o + (size_t)k > cap) return -1;
        for (int j = 0; j < k; j++) out[o++] = tmp[j];
        break;
      }
      default: return -1;
    }
  }
  return (int)o;
}

int json_extract_body(const char *p, size_t n, char *out, size_t cap, size_t *body_len) {
  const char *b = NULL;
  size_t bl = 0;
  const char *e = value(p, p + n, 0, &b, &bl, 1);
  if (!e) return JSON_MALFORMED;
  if (ws(e, p + n) != p + n) return JSON_MALFORMED; // trailing garbage
  if (!b) return JSON_NO_BODY;
  int r = json_unescape(b, bl, out, cap);
  if (r < 0) return JSON_MALFORMED;
  *body_len = (size_t)r;
  return JSON_OK;
}

int json_escape(const char *s, size_t n, char *out, size_t cap) {
  static const char *hex = "0123456789abcdef";
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '"' || ch == '\\') {
      if (o + 2 > cap) return -1;
      out[o++] = '\\'; out[o++] = (char)ch;
    } else if (ch < 0x20) {
      if (o + 6 > cap) return -1;
      out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
      out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 15];
    } else {
      if (o + 1 > cap) return -1;
      out[o++] = (char)ch;
    }
  }
  return (int)o;
}

int utf8_count_chars(const char *s, size_t n, size_t *chars) {
  size_t c = 0;
  size_t i = 0;
  while (i < n) {
    unsigned char ch = (unsigned char)s[i];
    size_t need;
    unsigned min;
    if (ch < 0x80) { need = 1; min = 0; }
    else if (ch >= 0xC2 && ch <= 0xDF) { need = 2; min = 0x80; }
    else if (ch >= 0xE0 && ch <= 0xEF) { need = 3; min = 0x800; }
    else if (ch >= 0xF0 && ch <= 0xF4) { need = 4; min = 0x10000; }
    else return -1;
    if (i + need > n) return -1;
    unsigned cp;
    if (need == 1) cp = ch;
    else if (need == 2) cp = (unsigned)(ch & 0x1F);
    else if (need == 3) cp = (unsigned)(ch & 0x0F);
    else cp = (unsigned)(ch & 0x07);
    for (size_t k = 1; k < need; k++) {
      unsigned char cc = (unsigned char)s[i + k];
      if ((cc & 0xC0) != 0x80) return -1;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < min) return -1; // overlong
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1; // surrogate
    if (cp > 0x10FFFF) return -1;
    i += need;
    c++;
  }
  *chars = c;
  return 0;
}
