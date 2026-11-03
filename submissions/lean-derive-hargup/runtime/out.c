#include "rt.h"
#include <stdlib.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

/* Send buffers. A response body is written after HDR_ROOM reserved bytes; out_finish() writes
 * the status line and headers right in front of it. obuf headers and 8 KiB blocks are kept on
 * free lists; larger bodies get their own allocation. Static (generated) responses are sent
 * from read-only memory without copying. */

#define BLOCK 8192
enum { OB_STATIC = 0, OB_BLOCK = 1, OB_HEAP = 2 };

static obuf *obuf_free_list;
static u8 *block_free_list;
static u32 block_free_count;

static inline obuf *obuf_get(void) {
  obuf *b = obuf_free_list;
  if (b) obuf_free_list = b->next;
  else if (!(b = malloc(sizeof *b))) abort();
  b->next = 0;
  return b;
}
static inline u8 *block_get(void) {
  u8 *m = block_free_list;
  if (m) { block_free_list = *(u8 **)m; block_free_count--; return m; }
  if (!(m = malloc(BLOCK))) abort();
  return m;
}
static inline void block_put(u8 *m) {
  if (block_free_count >= 16384) { free(m); return; }
  *(u8 **)m = block_free_list;
  block_free_list = m;
  block_free_count++;
}
static obuf *obuf_static(const u8 *data, u32 len) {
  obuf *b = obuf_get();
  b->data = data; b->len = len; b->cap = 0; b->mem = 0;
  return b;
}
static void obuf_release(obuf *b) {
  if (b->cap == BLOCK) block_put(b->mem);
  else if (b->cap) free(b->mem);
  b->next = obuf_free_list;
  obuf_free_list = b;
}

static inline void out_begin(out_t *o) {
  obuf *b = o->b = obuf_get();
  b->mem = block_get();
  b->cap = BLOCK;
  o->start = o->p = b->mem + HDR_ROOM;
  o->end = b->mem + BLOCK;
}
/* Make room for n more body bytes. */
static void out_grow(out_t *o, u32 n) {
  u32 used = (u32)(o->p - o->start);
  u32 want = (u32)(o->end - o->start) * 2;
  if (want < used + n) want = used + n;
  u8 *m = malloc(HDR_ROOM + want);
  if (!m) abort();
  memcpy(m + HDR_ROOM, o->start, used);
  if (o->b->cap == BLOCK) block_put(o->b->mem); else free(o->b->mem);
  o->b->mem = m;
  o->b->cap = HDR_ROOM + want;
  o->start = m + HDR_ROOM;
  o->p = o->start + used;
  o->end = m + HDR_ROOM + want;
}
static inline void out_need(out_t *o, u32 n) {
  if (UNLIKELY((u32)(o->end - o->p) < n)) out_grow(o, n);
}
static inline void out_lit(out_t *o, const char *s, u32 n) { memcpy(o->p, s, n); o->p += n; }

static inline u32 fmt_u64(u8 *d, u64 v) {
  u8 b[20]; u32 n = 0;
  do b[n++] = (u8)('0' + v % 10); while (v /= 10);
  for (u32 i = 0; i < n; i++) d[i] = b[n - 1 - i];
  return n;
}
static inline void out_i64(out_t *o, i64 v) {
  if (v < 0) { *o->p++ = '-'; v = -v; }
  o->p += fmt_u64(o->p, (u64)v);
}
/* JSON string: escapes '"', '\\' and control characters; UTF-8 passes through unchanged.
 * Needs room for 6*n + 2 bytes (+16 for the vector stores). */
#define JSTR_ROOM(n) (6u * (n) + 18u)
static inline void out_jstr(out_t *o, const u8 *s, u32 n) {
  u8 *d = o->p;
  *d++ = '"';
  u32 i = 0;
  for (;;) {
#if defined(__x86_64__)
    const __m128i q = _mm_set1_epi8('"'), bs = _mm_set1_epi8('\\'), lim = _mm_set1_epi8(0x1f);
    while (i + 16 <= n) {
      __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
      __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)),
                                 _mm_cmpeq_epi8(_mm_min_epu8(v, lim), v));
      _mm_storeu_si128((__m128i *)d, v);
      int m = _mm_movemask_epi8(bad);
      if (m) { u32 k = (u32)__builtin_ctz((u32)m); d += k; i += k; goto escape; }
      d += 16; i += 16;
    }
#endif
    for (; i < n; i++) {
      u8 c = s[i];
      if (c == '"' || c == '\\' || c < 0x20) goto escape;
      *d++ = c;
    }
    break;
  escape:;
    u8 c = s[i++];
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
      *d++ = (u8)"0123456789abcdef"[c >> 4]; *d++ = (u8)"0123456789abcdef"[c & 15];
    }
  }
  *d++ = '"';
  o->p = d;
}
/* Finish a response. hdr is the generated status line and headers up to "Content-Length: ". */
static inline obuf *out_finish(out_t *o, const u8 *hdr, u32 hdr_len) {
  u32 body = (u32)(o->p - o->start);
  u8 num[12];
  u32 nl = fmt_u64(num, body);
  u32 total = hdr_len + nl + 4;
  u8 *h = o->start - total;
  memcpy(h, hdr, hdr_len);
  memcpy(h + hdr_len, num, nl);
  memcpy(h + hdr_len + nl, "\r\n\r\n", 4);
  o->b->data = h;
  o->b->len = total + body;
  return o->b;
}
/* Abandon a partly written response (e.g. a plan failed and an error is sent instead). */
static inline void out_abort(out_t *o) { obuf_release(o->b); o->b = 0; }
