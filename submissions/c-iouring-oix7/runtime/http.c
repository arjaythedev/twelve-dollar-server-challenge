/* HTTP/1.1 request framing and path routing (http.c) */

#if defined(__x86_64__)
#include <immintrin.h>
#endif

/* Find the offset just past the first "\r\n\r\n" in p[0..n), or 0 if absent. */
static u32 find_head_end(const u8 *p, u32 n) {
  u32 i = 0;
#if defined(__x86_64__)
  /* SSE2 scan: process 16-byte blocks with a 3-byte overlap so a
     terminator straddling a block boundary is still observed.
     A hit at block-relative index k means the terminator starts at i+k:
       p[i+k]   == '\r'   (mr bit k)
       p[i+k+1] == '\n'   (mn bit k+1, i.e. mn>>1 bit k)
       p[i+k+2] == '\r'   (mr bit k+2, i.e. mr>>2 bit k)
       p[i+k+3] == '\n'   (mn bit k+3, i.e. mn>>3 bit k)
     Shift the byte-equality masks so they all line up on bit k, then AND. */
  while (i + 16 <= n) {
    __m128i blk = _mm_loadu_si128((const __m128i *)(p + i));
    u32 mr = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(blk, _mm_set1_epi8('\r')));
    if (UNLIKELY(mr == 0)) { i += 16; continue; }
    u32 mn = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(blk, _mm_set1_epi8('\n')));
    u32 m  = mr & (mn >> 1) & (mr >> 2) & (mn >> 3);
    if (UNLIKELY(m != 0)) return i + (u32)__builtin_ctz(m) + 4;
    i += 13;
  }
#elif defined(__aarch64__) || defined(__LP64__)
  while (i + 11 <= n) {
    u64 w;
    memcpy(&w, p + i, 8);
    u64 xr = w ^ 0x0d0d0d0d0d0d0d0dULL;
    u64 mr = (xr - 0x0101010101010101ULL) & ~xr & 0x8080808080808080ULL;
    if (LIKELY(mr == 0)) { i += 8; continue; }
    for (u32 k = 0; k < 8; k++) {
      if (p[i + k] == '\r' && p[i + k + 1] == '\n' && p[i + k + 2] == '\r' && p[i + k + 3] == '\n')
        return i + k + 4;
    }
    i += 8;
  }
#endif
  while (i + 3 < n) {
    if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n')
      return i + 4;
    i++;
  }
  return 0;
}

/* Case-insensitive equality of s[0..n) against a lowercase ASCII literal. */
static inline int hdr_eq(const u8 *s, const char *lit, u32 n) {
  for (u32 i = 0; i < n; i++) {
    u8 a = s[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (a != (u8)lit[i]) return 0;
  }
  return 1;
}

/* Parse one complete HTTP/1.x request framing from p[0..n).
   Returns bytes consumed (>0), 0 if more bytes are needed, or a negative
   HTTP status (-400, -413, -431) on a framing error. */
static int http_parse(const u8 *p, u32 n, req_t *r) {
  u32 he = find_head_end(p, n < MAX_HEAD ? n : MAX_HEAD);
  if (!he) return n >= MAX_HEAD ? -431 : 0;
  memset(r, 0, sizeof *r);
  const u8 *e = p + he - 2, *q = p;
  if (LIKELY(he >= 5 && !memcmp(p, "GET ", 4))) { r->method = M_GET; q = p + 4; }
  else if (he >= 6 && !memcmp(p, "POST ", 5)) { r->method = M_POST; q = p + 5; }
  else {
    while (q < e && *q != ' ') q++;
    if (q == p || q >= e) return -400;
    q++;
    r->method = M_OTHER;
  }
  const u8 *path = q, *qm = 0;
  while (q < e && *q != ' ' && *q != '\r') {
    if (UNLIKELY(*q == '?' && !qm)) qm = q;
    q++;
  }
  if (UNLIKELY(q == path || q >= e || *q != ' ')) return -400;
  r->path = path;
  r->path_len = (u32)((qm ? qm : q) - path);
  q++;
  if (UNLIKELY(e - q < 10 || memcmp(q, "HTTP/1.", 7) != 0)) return -400;
  int http10 = (q[7] == '0');
  if (UNLIKELY(!http10 && q[7] != '1')) return -400;
  q += 8;
  if (UNLIKELY(q[0] != '\r' || q[1] != '\n')) return -400;
  q += 2;
  int keepalive10 = 0;
  i64 clen = -1;
  int chunked = 0;
  while (q < e) {
    const u8 *ls = q;
    const u8 *le = (const u8 *)memchr(q, '\r', (size_t)(e - q + 2));
    if (UNLIKELY(!le || le[1] != '\n')) return -400;
    const u8 *colon = (const u8 *)memchr(ls, ':', (size_t)(le - ls));
    if (UNLIKELY(!colon || colon == ls)) return -400;
    u32 nl = (u32)(colon - ls);
    switch (nl) {
    case 13:
      if (hdr_eq(ls, "authorization", 13)) {
        const u8 *v = colon + 1, *ve = le;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        r->auth = v; r->auth_len = (u32)(ve - v); r->has_auth = 1;
      }
      break;
    case 14:
      if (hdr_eq(ls, "content-length", 14)) {
        const u8 *v = colon + 1, *ve = le;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        u32 vl = (u32)(ve - v);
        if (!vl || vl > 9) return -400;
        i64 x = 0;
        for (u32 i = 0; i < vl; i++) { if (v[i] < '0' || v[i] > '9') return -400; x = x * 10 + (v[i] - '0'); }
        if (clen >= 0 && clen != x) return -400;
        clen = x;
      }
      break;
    case 10:
      if (hdr_eq(ls, "connection", 10)) {
        const u8 *v = colon + 1, *ve = le;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        u32 vl = (u32)(ve - v);
        if (vl == 5 && hdr_eq(v, "close", 5)) r->close = 1;
        else if (vl == 10 && hdr_eq(v, "keep-alive", 10)) keepalive10 = 1;
      }
      break;
    case 17:
      if (hdr_eq(ls, "transfer-encoding", 17)) chunked = 1;
      break;
    }
    q = le + 2;
  }
  if (http10 && !keepalive10) r->close = 1;
  if (chunked) return -400;
  if (clen < 0) clen = 0;
  if (clen > MAX_BODY) return -413;
  if ((u64)he + (u64)clen > n) return 0;
  r->body = p + he;
  r->body_len = (u32)clen;
  return (int)(he + (u32)clen);
}

/* Route helpers: match a literal path prefix, then capture a segment. */

static inline int path_lit(const u8 **s, const u8 *e, const char *lit, u32 n) {
  if ((u32)(e - *s) < n || memcmp(*s, lit, n) != 0) return 0;
  if (*s + n < e && (*s)[n] != '/') return 0;
  *s += n;
  return 1;
}

static inline int path_param(const u8 **s, const u8 *e, const u8 **v, u32 *vn) {
  if (*s >= e || **s != '/') return 0;
  const u8 *a = *s + 1, *b = a;
  while (b < e && *b != '/') b++;
  if (b == a) return 0;
  *v = a;
  *vn = (u32)(b - a);
  *s = b;
  return 1;
}
