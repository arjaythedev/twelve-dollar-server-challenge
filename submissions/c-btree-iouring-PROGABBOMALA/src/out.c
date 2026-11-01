#include "rt.h"
#include <stdlib.h>

#define BLOCK 16384
#define JSTR_ROOM(n) (6u * (n) + 8u)

static obuf *obuf_free_list;
static uint8_t *block_free_list;
static uint32_t block_free_count;
static char dig2[200];

static void dig2_init(void) {
  for (int i = 0; i < 100; i++) {
    dig2[i * 2] = (char)('0' + i / 10);
    dig2[i * 2 + 1] = (char)('0' + i % 10);
  }
}

static uint32_t fmt_u64(uint8_t *d, uint64_t v) {
  uint8_t tmp[32];
  int n = 0;
  while (v >= 100) {
    uint64_t q = v / 100;
    unsigned r = (unsigned)(v - q * 100);
    tmp[n++] = (uint8_t)dig2[r * 2 + 1];
    tmp[n++] = (uint8_t)dig2[r * 2];
    v = q;
  }
  if (v >= 10) {
    tmp[n++] = (uint8_t)dig2[v * 2 + 1];
    tmp[n++] = (uint8_t)dig2[v * 2];
  } else {
    tmp[n++] = (uint8_t)('0' + (unsigned)v);
  }
  for (int i = 0; i < n; i++) d[i] = tmp[n - 1 - i];
  return (uint32_t)n;
}

static obuf *obuf_get(void) {
  obuf *b = obuf_free_list;
  if (b) obuf_free_list = b->next;
  else if (!(b = malloc(sizeof *b))) abort();
  b->next = 0;
  return b;
}
static uint8_t *block_get(void) {
  uint8_t *m = block_free_list;
  if (m) {
    block_free_list = *(uint8_t **)m;
    block_free_count--;
    return m;
  }
  if (!(m = malloc(BLOCK))) abort();
  return m;
}
static void block_put(uint8_t *m) {
  if (block_free_count >= 512) { free(m); return; }
  *(uint8_t **)m = block_free_list;
  block_free_list = m;
  block_free_count++;
}

obuf *obuf_static(const uint8_t *data, uint32_t len) {
  obuf *b = obuf_get();
  b->data = data;
  b->len = len;
  b->cap = 0;
  b->mem = 0;
  return b;
}
void obuf_release(obuf *b) {
  if (!b) return;
  if (b->cap == BLOCK) block_put(b->mem);
  else if (b->cap) free(b->mem);
  b->next = obuf_free_list;
  obuf_free_list = b;
}

static void out_grow(out_t *o, uint32_t n) {
  uint32_t used = (uint32_t)(o->p - o->start);
  uint32_t want = (uint32_t)(o->end - o->start);
  if (want < 1024) want = 1024;
  while (want < used + n) want *= 2;
  uint8_t *m = malloc(HDR_ROOM + want);
  if (!m) abort();
  memcpy(m + HDR_ROOM, o->start, used);
  if (o->b->cap == BLOCK) block_put(o->b->mem);
  else free(o->b->mem);
  o->b->mem = m;
  o->b->cap = HDR_ROOM + want;
  o->start = m + HDR_ROOM;
  o->p = o->start + used;
  o->end = m + HDR_ROOM + want;
}
void out_begin(out_t *o) {
  obuf *b = o->b = obuf_get();
  b->mem = block_get();
  b->cap = BLOCK;
  o->start = o->p = b->mem + HDR_ROOM;
  o->end = b->mem + BLOCK;
}
void out_abort(out_t *o) {
  if (o->b) obuf_release(o->b);
  o->b = 0;
}
void out_need(out_t *o, uint32_t n) {
  if (UNLIKELY((uint32_t)(o->end - o->p) < n)) out_grow(o, n);
}
void out_lit(out_t *o, const char *s, uint32_t n) {
  out_need(o, n);
  memcpy(o->p, s, n);
  o->p += n;
}
void out_i64(out_t *o, int64_t v) {
  out_need(o, 24);
  if (v < 0) { *o->p++ = '-'; v = -v; }
  o->p += fmt_u64(o->p, (uint64_t)v);
}
void out_jstr(out_t *o, const uint8_t *s, uint32_t n) {
  out_need(o, JSTR_ROOM(n));
  uint8_t *d = o->p;
  *d++ = '"';
  for (uint32_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c == '"' || c == '\\' || c < 0x20) {
      *d++ = '\\';
      switch (c) {
      case '"': *d++ = '"'; break;
      case '\\': *d++ = '\\'; break;
      case '\b': *d++ = 'b'; break;
      case '\f': *d++ = 'f'; break;
      case '\n': *d++ = 'n'; break;
      case '\r': *d++ = 'r'; break;
      case '\t': *d++ = 't'; break;
      default:
        *d++ = 'u'; *d++ = '0'; *d++ = '0';
        *d++ = (uint8_t)"0123456789abcdef"[c >> 4];
        *d++ = (uint8_t)"0123456789abcdef"[c & 15];
        break;
      }
    } else {
      *d++ = c;
    }
  }
  *d++ = '"';
  o->p = d;
}
obuf *out_finish(out_t *o, const char *status, int close_conn) {
  uint8_t hdr[HDR_ROOM];
  uint32_t n = 0;
  const char *p1 = "HTTP/1.1 ";
  while (*p1) hdr[n++] = (uint8_t)*p1++;
  while (*status) hdr[n++] = (uint8_t)*status++;
  const char *p2 = "\r\nContent-Type: application/json\r\nContent-Length: ";
  while (*p2) hdr[n++] = (uint8_t)*p2++;
  uint32_t blen = (uint32_t)(o->p - o->start);
  n += fmt_u64(hdr + n, blen);
  if (close_conn) {
    const char *p3 = "\r\nConnection: close\r\n\r\n";
    while (*p3) hdr[n++] = (uint8_t)*p3++;
  } else {
    hdr[n++] = '\r'; hdr[n++] = '\n'; hdr[n++] = '\r'; hdr[n++] = '\n';
  }
  memcpy(o->start - n, hdr, n);
  o->b->data = o->start - n;
  o->b->len = n + blen;
  return o->b;
}
void put_post(out_t *o, int64_t id, const uint8_t *body, uint32_t body_len,
              const uint8_t *created, uint32_t created_len,
              const uint8_t *author, uint32_t author_len, int64_t likes) {
  out_need(o, 96 + JSTR_ROOM(body_len) + JSTR_ROOM(created_len) + JSTR_ROOM(author_len));
  out_lit(o, "{\"id\":", 6);
  out_i64(o, id);
  out_lit(o, ",\"body\":", 8);
  out_jstr(o, body, body_len);
  out_lit(o, ",\"created_at\":", 14);
  out_jstr(o, created, created_len);
  out_lit(o, ",\"author\":", 10);
  out_jstr(o, author, author_len);
  out_lit(o, ",\"like_count\":", 14);
  out_i64(o, likes);
  out_lit(o, "}", 1);
}

void out_module_init(void) { dig2_init(); }
