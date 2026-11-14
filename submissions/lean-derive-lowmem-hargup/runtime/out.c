#include "rt.h"
#include <stdlib.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

/* Send buffers. A response body is written after HDR_ROOM reserved bytes; out_finish() writes
 * the status line and headers right in front of it. Bodies start in a block of one of two
 * sizes (the generated code picks the class per endpoint); a block that runs out moves to its
 * own allocation. Freed blocks are kept on capped free lists, so a burst of responses does not
 * stay resident. Static (generated) responses are sent from read-only memory without copying. */

#define OUT_INLINE 192
#define OUT_SMALL 1024
#define OUT_BIG 8192

static obuf *obuf_free_list;
typedef struct { obuf b; u8 bytes[OUT_INLINE]; } inline_obuf;
static obuf *inline_free_list;
static u32 inline_free_count;
typedef struct { u8 *head; u32 count, cap; } block_pool;
static block_pool pool_small = {0, 0, 2048}, pool_big = {0, 0, 256};
static inline block_pool *pool_of(u32 size) { return size == OUT_SMALL ? &pool_small : size == OUT_BIG ? &pool_big : 0; }

static inline obuf *obuf_get(void) {
  obuf *b = obuf_free_list;
  if (b) obuf_free_list = b->next;
  else if (!(b = malloc(sizeof *b))) abort();
  b->next = 0;
  return b;
}
static inline u8 *block_get(u32 size) {
  block_pool *p = pool_of(size);
  u8 *m = p->head;
  if (m) { p->head = *(u8 **)m; p->count--; return m; }
  if (!(m = malloc(size))) abort();
  return m;
}
/* Return a body allocation of `size` bytes. */
static inline void block_put(u8 *m, u32 size) {
  block_pool *p = pool_of(size);
  if (!p || p->count >= p->cap) { free(m); return; }
  *(u8 **)m = p->head;
  p->head = m;
  p->count++;
}
static obuf *obuf_static(const u8 *data, u32 len) {
  obuf *b = obuf_get();
  b->data = data; b->len = len; b->cap = 0; b->mem = 0;
  return b;
}
static void obuf_release(obuf *b) {
  if (b->cap == OUT_INLINE) {
    if (inline_free_count == 2048) { free(b); return; }
    b->next = inline_free_list; inline_free_list = b; inline_free_count++;
    return;
  }
  if (b->cap) block_put(b->mem, b->cap);
  b->next = obuf_free_list;
  obuf_free_list = b;
}

/* Bounds-derived small responses own their bytes in the send-queue node. */
static inline void out_begin_inline(out_t *o) {
  obuf *b = inline_free_list;
  if (b) { inline_free_list = b->next; inline_free_count--; }
  else if (!(b = malloc(sizeof(inline_obuf)))) abort();
  b->next = 0; b->mem = ((inline_obuf *)b)->bytes; b->cap = OUT_INLINE;
  o->b = b; o->start = o->p = b->mem + HDR_ROOM; o->end = b->mem + OUT_INLINE;
}
static inline void out_begin(out_t *o, u32 size) {
  obuf *b = o->b = obuf_get();
  b->mem = block_get(size);
  b->cap = size;
  o->start = o->p = b->mem + HDR_ROOM;
  o->end = b->mem + size;
}
/* Make room for n more body bytes. */
static void out_grow(out_t *o, u32 n) {
  u32 used = (u32)(o->p - o->start);
  u32 want = (u32)(o->end - o->start) * 2;
  if (want < used + n) want = used + n;
  u8 *m = malloc(HDR_ROOM + want);
  if (!m) abort();
  memcpy(m + HDR_ROOM, o->start, used);
  if (o->b->cap == OUT_INLINE) {
    /* No queue can reference a response before out_finish; replace the whole node. */
    obuf_release(o->b);
    o->b = obuf_get();
  } else block_put(o->b->mem, o->b->cap);
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
  u64 magnitude = (u64)v;
  if (v < 0) { *o->p++ = '-'; magnitude = 0u - magnitude; }
  o->p += fmt_u64(o->p, magnitude);
}
/* JSON string: escapes '"', '\\' and control characters; UTF-8 passes through unchanged.
 * Needs room for 6*n + 2 bytes (+16 for the vector stores). */
#define JSTR_ROOM(n) (6u * (n) + 18u)
/* Requires validated unescaped JSON contents (or a substring of them). */
static inline void out_plain_jstr(out_t *o, const u8 *s, u32 n) {
  *o->p++ = '"'; memcpy(o->p, s, n); o->p += n; *o->p++ = '"';
}

static inline void out_jstr(out_t *o, const u8 *s, u32 n) {
  u8 *d = o->p;
  *d++ = '"';
  u32 i = 0;
  for (;;) {
#if defined(__AVX2__) && !defined(OUT_NO_AVX2)
    const __m256i q2 = _mm256_set1_epi8('"'), bs2 = _mm256_set1_epi8('\\'), lim2 = _mm256_set1_epi8(0x1f);
    while (i + 32 <= n) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
      __m256i bad = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, q2), _mm256_cmpeq_epi8(v, bs2)),
                                    _mm256_cmpeq_epi8(_mm256_min_epu8(v, lim2), v));
      _mm256_storeu_si256((__m256i *)d, v);
      u32 m = (u32)_mm256_movemask_epi8(bad);
      if (m) { u32 k = (u32)__builtin_ctz(m); d += k; i += k; goto escape; }
      d += 32; i += 32;
    }
#endif
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
