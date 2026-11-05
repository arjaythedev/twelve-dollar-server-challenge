/* Minimal JSON output buffer used by B-tree and SQL renderers. */
#ifndef TD_OB_H
#define TD_OB_H

#include <stdint.h>
#include <string.h>

struct ob {
  char *p;
  int n, cap;
  int err;
};

static inline void ob_init(struct ob *o, char *buf, int cap) {
  o->p = buf;
  o->n = 0;
  o->cap = cap;
  o->err = 0;
}

static inline void ob_need(struct ob *o, int k) {
  if (o->err) return;
  if (o->n + k > o->cap) o->err = 1;
}

static inline void ob_char(struct ob *o, char c) {
  ob_need(o, 1);
  if (!o->err) o->p[o->n++] = c;
}

static inline void ob_lit(struct ob *o, const char *s) {
  int k = (int)strlen(s);
  ob_need(o, k);
  if (!o->err) {
    memcpy(o->p + o->n, s, (size_t)k);
    o->n += k;
  }
}

static inline void ob_bytes(struct ob *o, const void *s, int k) {
  ob_need(o, k);
  if (!o->err) {
    memcpy(o->p + o->n, s, (size_t)k);
    o->n += k;
  }
}

static const char TD_HEX[] = "0123456789abcdef";

static inline void ob_esc(struct ob *o, const unsigned char *s, int n) {
  ob_need(o, 6 * n);
  if (o->err) return;
  for (int i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') {
      o->p[o->n++] = (char)c;
      continue;
    }
    o->p[o->n++] = '\\';
    switch (c) {
    case '"':
      o->p[o->n++] = '"';
      break;
    case '\\':
      o->p[o->n++] = '\\';
      break;
    case '\n':
      o->p[o->n++] = 'n';
      break;
    case '\r':
      o->p[o->n++] = 'r';
      break;
    case '\t':
      o->p[o->n++] = 't';
      break;
    case '\b':
      o->p[o->n++] = 'b';
      break;
    case '\f':
      o->p[o->n++] = 'f';
      break;
    default:
      o->p[o->n++] = 'u';
      o->p[o->n++] = '0';
      o->p[o->n++] = '0';
      o->p[o->n++] = TD_HEX[c >> 4];
      o->p[o->n++] = TD_HEX[c & 15];
    }
  }
}

static inline void ob_int(struct ob *o, int64_t v) {
  char tmp[24];
  int n = 0;
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do {
    tmp[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  ob_need(o, n + (v < 0));
  if (o->err) return;
  if (v < 0) o->p[o->n++] = '-';
  while (n) o->p[o->n++] = tmp[--n];
}

static inline void ob_post_obj(struct ob *o, int64_t id, const unsigned char *body, int blen,
                               const unsigned char *created, int clen, const unsigned char *author,
                               int alen, int64_t likes) {
  ob_lit(o, "{\"id\":");
  ob_int(o, id);
  ob_lit(o, ",\"body\":\"");
  ob_esc(o, body, blen);
  ob_lit(o, "\",\"created_at\":\"");
  ob_esc(o, created, clen);
  ob_lit(o, "\",\"author\":\"");
  ob_esc(o, author, alen);
  ob_lit(o, "\",\"like_count\":");
  ob_int(o, likes);
  ob_char(o, '}');
}

#endif
