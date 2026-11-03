/* out.c — pooled response buffers.
 *
 * Two kinds of obuf flow through the transport:
 *   - static: the header points straight at read-only bytes (mem = 0,
 *     cap = 0), no copy, no allocation beyond the recycled obuf itself;
 *   - dynamic: the body is appended into a recycled 8 KiB block starting
 *     after a HDR_ROOM reserve, so out_finish can write the status line,
 *     headers and body length directly in front of the body without
 *     moving a single body byte.
 *
 * Both the 8 KiB blocks and the obuf headers are recycled through free
 * lists; grown responses use plain heap blocks that are freed, not cached.
 *
 * Shared responses: one obuf can be handed to several jobs of the same batch.
 * refs counts the owners; a transport that queues a shared obuf on a connection
 * first splits it (obuf_clone) so every output queue owns a private header and
 * the single ->next linkage stays safe.
 */

#include "rt.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#define BLOCK 8192
#define BLOCK_CACHE_MAX 4096u

static obuf *obuf_free_list;
static u8 *block_free_list;
static u32 block_free_count;

static inline void out_oom(void) {
  fputs("fatal: out of memory\n", stderr);
  exit(1);
}

static inline obuf *obuf_get(void) {
  obuf *b = obuf_free_list;
  if (LIKELY(b)) {
    obuf_free_list = b->next;
    b->next = 0;
    b->refs = 0;
    return b;
  }
  b = (obuf *)malloc(sizeof(obuf));
  if (UNLIKELY(!b)) out_oom();
  b->next = 0;
  b->refs = 0;
  return b;
}

/* Idle blocks are linked through their first pointer-sized bytes; those
 * bytes live inside the HDR_ROOM reserve and are always overwritten
 * before the block is handed out again. */
static inline u8 *block_get(void) {
  u8 *m = block_free_list;
  if (LIKELY(m)) {
    block_free_list = *(u8 **)m;
    block_free_count--;
    return m;
  }
  m = (u8 *)malloc(BLOCK);
  if (UNLIKELY(!m)) out_oom();
  return m;
}

static inline void block_put(u8 *m) {
  if (block_free_count < BLOCK_CACHE_MAX) {
    *(u8 **)m = block_free_list;
    block_free_list = m;
    block_free_count++;
  } else {
    free(m);
  }
}

/* Wrap read-only bytes without copying them. */
static inline obuf *obuf_static(const u8 *data, u32 len) {
  obuf *b = obuf_get();
  b->data = data;
  b->len = len;
  b->cap = 0;
  b->mem = 0;
  b->refs = 0;
  return b;
}

/* Mark a response as shared: refs becomes the owner count. */
static inline void obuf_share_init(obuf *b) { b->refs = 1; }

/* One more owner of an already-shared response. */
static inline obuf *obuf_retain(obuf *b) { if (b->refs) b->refs++; return b; }

/* A private copy of a shared response for one output queue: the source keeps
 * its remaining owners, the clone owns its own storage. */
static inline obuf *obuf_clone(obuf *b) {
  obuf *c = obuf_get();
  if (b->cap == BLOCK) {
    u8 *m = block_get();
    memcpy(m, b->mem, BLOCK);
    c->mem = m;
    c->cap = BLOCK;
    c->data = m + (b->data - b->mem);
  } else if (b->cap) {
    u8 *m = (u8 *)malloc(b->cap);
    if (UNLIKELY(!m)) out_oom();
    memcpy(m, b->mem, b->cap);
    c->mem = m;
    c->cap = b->cap;
    c->data = m + (b->data - b->mem);
  } else {
    c->mem = 0;
    c->cap = 0;
    c->data = b->data;            /* static: the bytes are read-only and immortal */
  }
  c->len = b->len;
  return c;
}

static inline void obuf_release(obuf *b) {
  if (b->refs && --b->refs) return;
  if (b->cap == BLOCK) block_put(b->mem);
  else if (b->cap) free(b->mem);
  b->next = obuf_free_list;
  obuf_free_list = b;
}
/* Fresh dynamic buffer: 8 KiB block with HDR_ROOM kept free in front of
 * the body so out_finish can prepend the header in place. */
static inline void out_begin(out_t *o) {
  u8 *m = block_get();
  obuf *b = obuf_get();
  b->data = 0;
  b->len = 0;
  b->cap = BLOCK;
  b->mem = m;
  b->next = 0;
  b->refs = 0;
  o->b = b;
  o->start = m + HDR_ROOM;
  o->p = o->start;
  o->end = m + BLOCK;
}

/* Relocate the body into a larger buffer, keeping the HDR_ROOM front
 * reserve so out_finish semantics stay identical. */
static void out_grow(out_t *o, u32 n) {
  u32 used = (u32)(o->p - o->start);
  u32 old_cap = o->b->cap;
  u8 *old = o->b->mem;
  u32 new_cap = old_cap ? old_cap : (u32)BLOCK;
  while (new_cap - HDR_ROOM < used + n) new_cap <<= 1;
  u8 *m = (u8 *)malloc(new_cap);
  if (UNLIKELY(!m)) out_oom();
  memcpy(m + HDR_ROOM, o->start, used);
  o->b->mem = m;
  o->b->cap = new_cap;
  o->start = m + HDR_ROOM;
  o->p = o->start + used;
  o->end = m + new_cap;
  if (old_cap == BLOCK) block_put(old);
  else if (old) free(old);
}

static inline void out_need(out_t *o, u32 n) {
  if (UNLIKELY((u32)(o->end - o->p) < n)) out_grow(o, n);
}

static inline void out_lit(out_t *o, const char *s, u32 n) {
  memcpy(o->p, s, n);
  o->p += n;
}

static const char DIG2[200] =
  "0001020304050607080910111213141516171819"
  "2021222324252627282930313233343536373839"
  "4041424344454647484950515253545556575859"
  "6061626364656667686970717273747576777879"
  "8081828384858687888990919293949596979899";

static inline u32 fmt_u64(u8 *d, u64 v) {
  u8 tmp[20];
  u32 n = 0;
  while (v >= 100u) {
    u32 r = (u32)(v % 100u) * 2u;
    v /= 100u;
    tmp[n++] = (u8)DIG2[r + 1];
    tmp[n++] = (u8)DIG2[r];
  }
  if (v >= 10u) {
    u32 r = (u32)v * 2u;
    tmp[n++] = (u8)DIG2[r + 1];
    tmp[n++] = (u8)DIG2[r];
  } else {
    tmp[n++] = (u8)('0' + (u32)v);
  }
  for (u32 i = 0; i < n; i++) d[i] = tmp[n - 1u - i];
  return n;
}

static inline void out_i64(out_t *o, i64 v) {
  /* (u64)v is i64 mod 2^64; negate in u64 so INT64_MIN stays defined. */
  u64 m = (u64)v;
  if (v < 0) { *o->p++ = '-'; m = 0ULL - m; }
  o->p += fmt_u64(o->p, m);
}

/* Worst case per byte is a 6-byte \u00XX escape; the constant covers the
 * quotes plus slack so one out_need up front suffices for the whole call. */
#define JSTR_ROOM(n) (6u * (n) + 18u)

/* Escape one byte at p, return the advanced write cursor. */
static inline u8 *out_esc_byte(u8 *p, u8 c) {
  switch (c) {
    case '"':  *p++ = '\\'; *p++ = '"';  break;
    case '\\': *p++ = '\\'; *p++ = '\\'; break;
    case '\b': *p++ = '\\'; *p++ = 'b';  break;
    case '\f': *p++ = '\\'; *p++ = 'f';  break;
    case '\n': *p++ = '\\'; *p++ = 'n';  break;
    case '\r': *p++ = '\\'; *p++ = 'r';  break;
    case '\t': *p++ = '\\'; *p++ = 't';  break;
    default:
      if (c < 0x20) {
        p[0] = '\\';
        p[1] = 'u';
        p[2] = '0';
        p[3] = '0';
        p[4] = (u8)("0123456789abcdef"[c >> 4]);
        p[5] = (u8)("0123456789abcdef"[c & 15u]);
        p += 6;
      } else {
        *p++ = c; /* includes all UTF-8 bytes >= 0x20, passed through */
      }
  }
  return p;
}

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
      if (UNLIKELY(m != 0)) { u32 k = (u32)__builtin_ctz((u32)m); d += k; i += k; goto escape; }
      d += 16; i += 16;
    }
#elif defined(__aarch64__) || defined(__LP64__)
    while (i + 8 <= n) {
      u64 w;
      memcpy(&w, s + i, 8);
      u64 lo  = (w - 0x2020202020202020ULL) & ~w & 0x8080808080808080ULL;
      u64 xq  = w ^ 0x2222222222222222ULL;
      u64 eqq = (xq - 0x0101010101010101ULL) & ~xq & 0x8080808080808080ULL;
      u64 xb  = w ^ 0x5c5c5c5c5c5c5c5cULL;
      u64 eqb = (xb - 0x0101010101010101ULL) & ~xb & 0x8080808080808080ULL;
      if (UNLIKELY((lo | eqq | eqb) != 0)) break;
      memcpy(d, &w, 8);
      d += 8; i += 8;
    }
#endif
    for (; i < n; i++) {
      u8 c = s[i];
      if (UNLIKELY(c == '"' || c == '\\' || c < 0x20)) goto escape;
      *d++ = c;
    }
    break;
  escape:;
    d = out_esc_byte(d, s[i++]);
  }
  *d++ = '"';
  o->p = d;
}

/* Write [hdr][decimal body length][\r\n\r\n] ending exactly at o->start,
 * then expose hdr+body as one contiguous obuf. Requires the prepended
 * bytes to fit the reserve in front of o->start (HDR_ROOM). */
static inline obuf *out_finish(out_t *o, const u8 *hdr, u32 hdr_len) {
  u8 nb[20];
  u32 blen = (u32)(o->p - o->start);
  u32 nl = fmt_u64(nb, (u64)blen);
  u32 pre = hdr_len + nl + 4;
  u8 *h = o->start - pre;
  memcpy(h, hdr, hdr_len);
  memcpy(h + hdr_len, nb, nl);
  h[hdr_len + nl] = '\r';
  h[hdr_len + nl + 1] = '\n';
  h[hdr_len + nl + 2] = '\r';
  h[hdr_len + nl + 3] = '\n';
  o->b->data = h;
  o->b->len = pre + blen;
  return o->b;
}

static inline void out_abort(out_t *o) {
  if (o->b) {
    obuf_release(o->b);
    o->b = 0;
  }
}
