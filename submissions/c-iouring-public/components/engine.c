/* db/engine.c (sha256 8bf59ee9eea9), developed separately for this entry. */
// engine.c — see engine.h. One translation unit with the SQLite amalgamation.
#include "sqlite3.c"
#if ENG_TURBO
#include "qc.c"   // cursed-sqlite-turbo (DB_READS=turbo): qc/qcg/qvm, over the same unmodified amalgamation (QC_STOCK)
#endif
#include "engine.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#if SQLITE_THREADSAFE
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define ENG_SIMD "neon"
#elif defined(__AVX2__)
#include <immintrin.h>
#define ENG_SIMD "avx2"
#elif defined(__SSE2__)
#include <emmintrin.h>
#define ENG_SIMD "sse2"
#else
#define ENG_SIMD "scalar"
#endif
#ifdef ENG_FORCE_SCALAR
#undef ENG_SIMD
#define ENG_SIMD "scalar"
#endif

#define FEED_N 20
#define RANGE_MAX 1024        // feed_range: widest id window counted with one likes range scan

// ---------------------------------------------------------------- JSON string escaping
static const char HEX[] = "0123456789abcdef";
static inline char *esc1(char *o, unsigned char c) {   // JSON.stringify's escapes
  switch (c) {
    case '"': o[0] = '\\'; o[1] = '"'; return o + 2;
    case '\\': o[0] = '\\'; o[1] = '\\'; return o + 2;
    case '\b': o[0] = '\\'; o[1] = 'b'; return o + 2;
    case '\f': o[0] = '\\'; o[1] = 'f'; return o + 2;
    case '\n': o[0] = '\\'; o[1] = 'n'; return o + 2;
    case '\r': o[0] = '\\'; o[1] = 'r'; return o + 2;
    case '\t': o[0] = '\\'; o[1] = 't'; return o + 2;
    default: memcpy(o, "\\u00", 4); o[4] = HEX[c >> 4]; o[5] = HEX[c & 15]; return o + 6;
  }
}
static inline int needs_esc(unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; }

size_t eng_json_escape_scalar(char *out, const char *s, size_t n) {
  char *o = out;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (needs_esc(c)) o = esc1(o, c); else *o++ = (char)c;
  }
  return (size_t)(o - out);
}

// SIMD: store 16 (32) input bytes unconditionally, then advance only up to the first byte that needs
// escaping. Never reads past s+n: the tail (< one vector) is copied into a padded stack block first.
#if !defined(ENG_FORCE_SCALAR) && defined(__aarch64__) && defined(__ARM_NEON)
static inline uint64_t esc_mask16(uint8x16_t v) {      // 4 bits per byte (shrn trick)
  uint8x16_t m = vorrq_u8(vorrq_u8(vceqq_u8(v, vdupq_n_u8('"')), vceqq_u8(v, vdupq_n_u8('\\'))), vcltq_u8(v, vdupq_n_u8(0x20)));
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
}
size_t eng_json_escape(char *out, const char *s, size_t n) {
  char *o = out; size_t i = 0;
  while (i + 16 <= n) {
    uint8x16_t v = vld1q_u8((const uint8_t *)s + i);
    vst1q_u8((uint8_t *)o, v);
    uint64_t b = esc_mask16(v);
    if (!b) { i += 16; o += 16; continue; }
    unsigned k = (unsigned)__builtin_ctzll(b) >> 2;
    o += k; i += k; o = esc1(o, (unsigned char)s[i]); i++;
  }
  while (i < n) {                                      // tail block padded with 'a'
    uint8_t blk[16]; size_t r = n - i; memset(blk, 'a', 16); memcpy(blk, s + i, r);
    uint8x16_t v = vld1q_u8(blk);
    vst1q_u8((uint8_t *)o, v);
    uint64_t b = esc_mask16(v);
    if (!b) { o += r; break; }
    unsigned k = (unsigned)__builtin_ctzll(b) >> 2;
    o += k; i += k; o = esc1(o, (unsigned char)s[i]); i++;
  }
  return (size_t)(o - out);
}
#elif !defined(ENG_FORCE_SCALAR) && (defined(__AVX2__) || defined(__SSE2__))
static inline unsigned esc_mask_128(__m128i v) {
  __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'))),
                           _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(0x1f)), v));   // v <= 0x1f
  return (unsigned)_mm_movemask_epi8(m);
}
#ifdef __AVX2__
static inline unsigned esc_mask_256(__m256i v) {
  __m256i m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('"')), _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'))),
                              _mm256_cmpeq_epi8(_mm256_min_epu8(v, _mm256_set1_epi8(0x1f)), v));
  return (unsigned)_mm256_movemask_epi8(m);
}
#endif
size_t eng_json_escape(char *out, const char *s, size_t n) {
  char *o = out; size_t i = 0;
#ifdef __AVX2__
  while (i + 32 <= n) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
    _mm256_storeu_si256((__m256i *)o, v);
    unsigned b = esc_mask_256(v);
    if (!b) { i += 32; o += 32; continue; }
    unsigned k = (unsigned)__builtin_ctz(b);
    o += k; i += k; o = esc1(o, (unsigned char)s[i]); i++;
  }
#endif
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
    _mm_storeu_si128((__m128i *)o, v);
    unsigned b = esc_mask_128(v);
    if (!b) { i += 16; o += 16; continue; }
    unsigned k = (unsigned)__builtin_ctz(b);
    o += k; i += k; o = esc1(o, (unsigned char)s[i]); i++;
  }
  while (i < n) {
    uint8_t blk[16]; size_t r = n - i; memset(blk, 'a', 16); memcpy(blk, s + i, r);
    __m128i v = _mm_loadu_si128((const __m128i *)blk);
    _mm_storeu_si128((__m128i *)o, v);
    unsigned b = esc_mask_128(v);
    if (!b) { o += r; break; }
    unsigned k = (unsigned)__builtin_ctz(b);
    o += k; i += k; o = esc1(o, (unsigned char)s[i]); i++;
  }
  return (size_t)(o - out);
}
#else
size_t eng_json_escape(char *out, const char *s, size_t n) { return eng_json_escape_scalar(out, s, n); }
#endif
const char *eng_simd_name(void) { return ENG_SIMD; }

// ---------------------------------------------------------------- small writers
static inline char *w_lit(char *p, const char *s, size_t n) { memcpy(p, s, n); return p + n; }
#define W(p, s) w_lit(p, s, sizeof(s) - 1)
static inline char *w_i64(char *p, i64 v) {
  char t[24]; int k = 0; u64 u;
  if (v < 0) { *p++ = '-'; u = (u64)0 - (u64)v; } else u = (u64)v;
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
  while (k) *p++ = t[--k];
  return p;
}
static inline char *w_str(char *p, const void *s, size_t n) { *p++ = '"'; p += eng_json_escape(p, (const char *)s, n); *p++ = '"'; return p; }

// ---------------------------------------------------------------- timestamps
// civil_from_days (H. Hinnant), exact for the whole proleptic Gregorian range we care about.
void eng_format_ts(char o[24], int64_t ms) {
  int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
  int64_t rem = ms - days * 86400000;
  int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400; unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9; y += (m <= 2);
  unsigned hh = (unsigned)(rem / 3600000), mi = (unsigned)(rem / 60000 % 60), ss = (unsigned)(rem / 1000 % 60), mss = (unsigned)(rem % 1000);
  o[0] = (char)('0' + y / 1000 % 10); o[1] = (char)('0' + y / 100 % 10); o[2] = (char)('0' + y / 10 % 10); o[3] = (char)('0' + y % 10);
  o[4] = '-'; o[5] = (char)('0' + m / 10); o[6] = (char)('0' + m % 10); o[7] = '-'; o[8] = (char)('0' + d / 10); o[9] = (char)('0' + d % 10);
  o[10] = 'T'; o[11] = (char)('0' + hh / 10); o[12] = (char)('0' + hh % 10); o[13] = ':'; o[14] = (char)('0' + mi / 10); o[15] = (char)('0' + mi % 10);
  o[16] = ':'; o[17] = (char)('0' + ss / 10); o[18] = (char)('0' + ss % 10); o[19] = '.';
  o[20] = (char)('0' + mss / 100); o[21] = (char)('0' + mss / 10 % 10); o[22] = (char)('0' + mss % 10); o[23] = 'Z';
}
static int64_t realtime_ms(void) {   // same clock as SQLite's unixCurrentTimeInt64 (gettimeofday), truncated to ms
  struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// ---------------------------------------------------------------- record decoding
static inline u64 rd_varint(const u8 *p, int *n) {
  if (!(p[0] & 0x80)) { *n = 1; return p[0]; }
  if (!(p[1] & 0x80)) { *n = 2; return ((u64)(p[0] & 0x7f) << 7) | p[1]; }
  u64 v = 0; int i;
  for (i = 0; i < 8; i++) { v = (v << 7) | (p[i] & 0x7f); if (!(p[i] & 0x80)) { *n = i + 1; return v; } }
  v = (v << 8) | p[8]; *n = 9; return v;
}
static inline u32 st_len(u64 t) { static const u8 L[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0}; return t < 12 ? L[t] : (u32)((t - 12) / 2); }
static inline i64 st_int(u64 t, const u8 *p) {
  switch (t) {
    case 1: return (i8)p[0];
    case 2: return (i16)((p[0] << 8) | p[1]);
    case 3: return ((int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8)) >> 8;
    case 4: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
    case 5: { i64 v = 0; for (int i = 0; i < 6; i++) v = (v << 8) | p[i]; return (v << 16) >> 16; }
    case 6: { u64 v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return (i64)v; }
    case 9: return 1;
    default: return 0;
  }
}
static inline int st_isint(u64 t) { return (t >= 1 && t <= 6) || t == 8 || t == 9; }
// decode up to nf fields; returns fields decoded, -1 if the record is malformed / runs past amt
static int rec_decode(const u8 *rec, u32 amt, int nf, u64 *types, const u8 **data) {
  int n; u64 hdr = rd_varint(rec, &n); const u8 *h = rec + n, *he = rec + hdr, *d = rec + hdr; int i = 0;
  if (hdr > amt) return -1;
  while (h < he && i < nf) { types[i] = rd_varint(h, &n); h += n; data[i] = d; d += st_len(types[i]); i++; }
  return d <= rec + amt ? i : -1;
}

// ---------------------------------------------------------------- record encoding (== OP_MakeRecord)
static inline u32 int_type(i64 i, int fmt) {
  u64 u = i < 0 ? ~(u64)i : (u64)i;
  if (u <= 127) return ((i & 1) == i && fmt >= 4) ? 8 + (u32)i : 1;
  if (u <= 32767) return 2;
  if (u <= 8388607) return 3;
  if (u <= 2147483647) return 4;
  if (u <= (((u64)0x7fff) << 32 | 0xffffffffu)) return 5;
  return 6;
}
static inline u8 *put_int(u8 *p, i64 v, u32 t) {
  u32 n = st_len(t); u64 u = (u64)v;
  for (u32 k = n; k > 0; k--) { p[k - 1] = (u8)u; u >>= 8; }
  return p + n;
}
static inline u8 *put_varint(u8 *p, u64 v) { if (v < 0x80) { *p = (u8)v; return p + 1; } return p + sqlite3PutVarint(p, v); }
static inline int varint_len(u64 v) { return v < 0x80 ? 1 : sqlite3VarintLen(v); }
// Encodes ints/texts in column order: kinds[i] = 'i' (ints[i]) | 't' (txt[i], tn[i]) | 'n' (NULL)
static u32 make_record(u8 *out, int nf, const char *kinds, const i64 *ints, const void *const *txt, const u32 *tn, int fmt) {
  u32 types[8]; u32 hdr = 0;
  for (int i = 0; i < nf; i++) {
    types[i] = kinds[i] == 'i' ? int_type(ints[i], fmt) : kinds[i] == 't' ? 13 + 2 * tn[i] : 0;
    hdr += (u32)varint_len(types[i]);
  }
  hdr += hdr + 1 < 0x80 ? 1 : 2;                    // the header size counts itself (ours are < 16K)
  u8 *p = put_varint(out, hdr);
  for (int i = 0; i < nf; i++) p = put_varint(p, types[i]);
  for (int i = 0; i < nf; i++) {
    if (kinds[i] == 'i') p = put_int(p, ints[i], types[i]);
    else if (kinds[i] == 't') { memcpy(p, txt[i], tn[i]); p += tn[i]; }
  }
  return (u32)(p - out);
}


// ---------------------------------------------------------------- WAL write coalescing VFS ("cursed-wal")
// SQLite writes each WAL frame with two pwrite()s (24-byte header, then the page). This shim appends
// sequential WAL writes to a buffer and issues one pwrite per commit. The flush happens inside the
// xWrite of the commit frame's page (the frame header's "db size after commit" field is non-zero only
// for the commit frame), so a failed flush fails that xWrite and SQLite rolls the transaction back exactly
// as if its own write had failed. Any other access to the WAL (read, sync, truncate, size, close, a
// non-sequential write) flushes first. Durability is unchanged: with synchronous=NORMAL SQLite issues no
// fsync at commit either, and every byte of a commit has been written when the commit returns.
// The shim also answers xFileSize for the database file from a cached value while it holds an EXCLUSIVE lock
// (locking_mode=EXCLUSIVE holds it for the life of the connection): nobody else can resize the file then, and
// every read transaction otherwise makes one fstat (explore/sqlite-patches vfs.h: read txn 0.40 -> 0.10 µs on
// the Xeon). Kept current by our own writes; dropped on truncate, on any unlock below EXCLUSIVE, and on file
// controls that may resize.
typedef struct { sqlite3_file base; sqlite3_file *real; int wal, commit_hdr; u8 *buf; size_t n, cap; sqlite3_int64 off;
                 int lock, size_ok; sqlite3_int64 size; sqlite3_int64 next; } cwfile;
static sqlite3_vfs *cw_base, cw_vfs;
static uint64_t cw_flushes, cw_writes;
#define CWR(f) (((cwfile *)(f))->real)
static int cw_flush(cwfile *f) {
  if (!f->n) return SQLITE_OK;
  // unix seekAndWriteFd() masks each write with nBuf &= 0x1ffff: one write per < 128 KiB chunk
  int rc = SQLITE_OK;
  for (size_t o = 0; o < f->n && rc == SQLITE_OK; o += 0x1f000) {
    size_t k = f->n - o < 0x1f000 ? f->n - o : 0x1f000;
    rc = f->real->pMethods->xWrite(f->real, f->buf + o, (int)k, f->off + (sqlite3_int64)o);
    cw_flushes++;
  }
  f->n = 0;
  return rc;
}
#define CW_FLUSH(f) do { if (((cwfile *)(f))->n) { int rc_ = cw_flush((cwfile *)(f)); if (rc_) return rc_; } } while (0)
static int cw_close(sqlite3_file *f) { cwfile *c = (cwfile *)f; int rc = cw_flush(c); free(c->buf); c->buf = 0; int rc2 = c->real->pMethods->xClose(c->real); return rc ? rc : rc2; }
static int cw_read(sqlite3_file *f, void *b, int n, sqlite3_int64 o) { CW_FLUSH(f); return CWR(f)->pMethods->xRead(CWR(f), b, n, o); }
static int cw_write(sqlite3_file *f, const void *b, int n, sqlite3_int64 o) {
  cwfile *c = (cwfile *)f;
  if (!c->wal) {
    int rc = c->real->pMethods->xWrite(c->real, b, n, o);
    if (rc) c->size_ok = 0; else if (c->size_ok && o + n > c->size) c->size = o + n;
    return rc;
  }
  cw_writes++;
  // A lone frame-header write that doesn't continue the append run is walRewriteChecksums() rewriting headers in
  // place at commit, after a transaction overwrote frames it had already written (a cache spill, then the page
  // modified again). Write it through: buffered, the commit frame's final checksum stayed in memory after COMMIT
  // returned, and a crash then lost the commit (found by explore/sqlite-conformance with SQLite's walcksum-2.1 /
  // waloverwrite-1.x.9; test_crash.c reproduces it: 39 of 40 spilling batches lost).
  if (n == 24 && o != c->next) {
    const u8 *h = b; CW_FLUSH(f); c->next = o + n; c->commit_hdr = (h[4] | h[5] | h[6] | h[7]) != 0;
    return c->real->pMethods->xWrite(c->real, b, n, o);
  }
  c->next = o + n;
  if (c->n && o != c->off + (sqlite3_int64)c->n) CW_FLUSH(f);
  if (c->n + (size_t)n > c->cap) {
    if (c->n + (size_t)n > (8u << 20)) { CW_FLUSH(f); if ((size_t)n > c->cap) return c->real->pMethods->xWrite(c->real, b, n, o); }
    else { size_t nc = c->cap ? c->cap : 65536; while (nc < c->n + (size_t)n) nc *= 2; u8 *q = realloc(c->buf, nc); if (!q) { CW_FLUSH(f); return c->real->pMethods->xWrite(c->real, b, n, o); } c->buf = q; c->cap = nc; }
  }
  if (!c->n) c->off = o;
  memcpy(c->buf + c->n, b, (size_t)n); c->n += (size_t)n;
  if (n == 24) { const u8 *h = b; c->commit_hdr = (h[4] | h[5] | h[6] | h[7]) != 0; return SQLITE_OK; }
  if (c->commit_hdr) { c->commit_hdr = 0; return cw_flush(c); }   // the commit frame is complete
  return SQLITE_OK;
}
static int cw_trunc(sqlite3_file *f, sqlite3_int64 s) { CW_FLUSH(f); ((cwfile *)f)->size_ok = 0; return CWR(f)->pMethods->xTruncate(CWR(f), s); }
static int cw_sync(sqlite3_file *f, int fl) { CW_FLUSH(f); return CWR(f)->pMethods->xSync(CWR(f), fl); }
static int cw_size(sqlite3_file *f, sqlite3_int64 *s) {
  cwfile *c = (cwfile *)f; CW_FLUSH(f);
  if (c->size_ok) { *s = c->size; return SQLITE_OK; }
  int rc = c->real->pMethods->xFileSize(c->real, s);
  if (rc == SQLITE_OK && !c->wal && c->lock == SQLITE_LOCK_EXCLUSIVE) { c->size = *s; c->size_ok = 1; }
  return rc;
}
static int cw_lock(sqlite3_file *f, int l) {
  cwfile *c = (cwfile *)f; int rc = c->real->pMethods->xLock(c->real, l);
  if (rc == SQLITE_OK) { if (l == SQLITE_LOCK_EXCLUSIVE && c->lock != l) c->size_ok = 0; c->lock = l; }
  return rc;
}
static int cw_unlock(sqlite3_file *f, int l) {
  cwfile *c = (cwfile *)f; CW_FLUSH(f);
  int rc = c->real->pMethods->xUnlock(c->real, l);
  if (l < SQLITE_LOCK_EXCLUSIVE) c->size_ok = 0;
  if (rc == SQLITE_OK) c->lock = l;
  return rc;
}
static int cw_crl(sqlite3_file *f, int *r) { return CWR(f)->pMethods->xCheckReservedLock(CWR(f), r); }
static int cw_fc(sqlite3_file *f, int op, void *a) {
  CW_FLUSH(f);
  switch (op) {   // controls that cannot change the file's size keep the cached size (list from sqlite-patches vfs.h)
    case SQLITE_FCNTL_LOCKSTATE: case SQLITE_FCNTL_VFSNAME: case SQLITE_FCNTL_PRAGMA: case SQLITE_FCNTL_BUSYHANDLER:
    case SQLITE_FCNTL_HAS_MOVED: case SQLITE_FCNTL_MMAP_SIZE: case SQLITE_FCNTL_SYNC: case SQLITE_FCNTL_COMMIT_PHASETWO:
    case SQLITE_FCNTL_PDB: case SQLITE_FCNTL_LOCK_TIMEOUT: case SQLITE_FCNTL_DATA_VERSION: case SQLITE_FCNTL_CKPT_START:
    case SQLITE_FCNTL_CKPT_DONE: case SQLITE_FCNTL_POWERSAFE_OVERWRITE: case SQLITE_FCNTL_PERSIST_WAL:
    case SQLITE_FCNTL_TEMPFILENAME: case SQLITE_FCNTL_WAL_BLOCK: case SQLITE_FCNTL_FILE_POINTER: case SQLITE_FCNTL_EXTERNAL_READER:
      break;
    default: ((cwfile *)f)->size_ok = 0;
  }
  return CWR(f)->pMethods->xFileControl(CWR(f), op, a);
}
static int cw_ss(sqlite3_file *f) { return CWR(f)->pMethods->xSectorSize(CWR(f)); }
static int cw_dc(sqlite3_file *f) { return CWR(f)->pMethods->xDeviceCharacteristics(CWR(f)); }
static int cw_shmmap(sqlite3_file *f, int i, int s, int e, void volatile **p) { return CWR(f)->pMethods->xShmMap(CWR(f), i, s, e, p); }
static int cw_shmlock(sqlite3_file *f, int o, int n, int fl) { return CWR(f)->pMethods->xShmLock(CWR(f), o, n, fl); }
static void cw_shmbar(sqlite3_file *f) { CWR(f)->pMethods->xShmBarrier(CWR(f)); }
static int cw_shmunmap(sqlite3_file *f, int d) { return CWR(f)->pMethods->xShmUnmap(CWR(f), d); }
static int cw_fetch(sqlite3_file *f, sqlite3_int64 o, int n, void **p) { return CWR(f)->pMethods->xFetch(CWR(f), o, n, p); }
static int cw_unfetch(sqlite3_file *f, sqlite3_int64 o, void *p) { return CWR(f)->pMethods->xUnfetch(CWR(f), o, p); }
static const sqlite3_io_methods cw_io = {3, cw_close, cw_read, cw_write, cw_trunc, cw_sync, cw_size, cw_lock, cw_unlock, cw_crl, cw_fc,
                                         cw_ss, cw_dc, cw_shmmap, cw_shmlock, cw_shmbar, cw_shmunmap, cw_fetch, cw_unfetch};
// Two registrations: "cursed-wal" over the default unix VFS, "cursed-wal-excl" over "unix-excl" (ckpt_thread)
static sqlite3_vfs *cwx_base, cwx_vfs;
static int cw_open_on(sqlite3_vfs *base, const char *name, sqlite3_file *f, int flags, int *out) {
  cwfile *c = (cwfile *)f; memset(c, 0, sizeof *c); c->real = (sqlite3_file *)(c + 1);
  int rc = base->xOpen(base, name, c->real, flags, out);
  c->base.pMethods = rc == SQLITE_OK && c->real->pMethods ? &cw_io : 0;
  c->wal = (flags & SQLITE_OPEN_WAL) != 0;
  return rc;
}
static int cw_open(sqlite3_vfs *v, const char *name, sqlite3_file *f, int flags, int *out) { (void)v; return cw_open_on(cw_base, name, f, flags, out); }
static int cwx_open(sqlite3_vfs *v, const char *name, sqlite3_file *f, int flags, int *out) { (void)v; return cw_open_on(cwx_base, name, f, flags, out); }
static const char *cw_register(int excl) {
  if (excl) {
    if (!cwx_base) {
      if (!(cwx_base = sqlite3_vfs_find("unix-excl"))) return 0;
      cwx_vfs = *cwx_base; cwx_vfs.zName = "cursed-wal-excl"; cwx_vfs.szOsFile = (int)sizeof(cwfile) + cwx_base->szOsFile; cwx_vfs.xOpen = cwx_open;
      sqlite3_vfs_register(&cwx_vfs, 0);
    }
    return "cursed-wal-excl";
  }
  if (!cw_base) {
    cw_base = sqlite3_vfs_find(0); cw_vfs = *cw_base;
    cw_vfs.zName = "cursed-wal"; cw_vfs.szOsFile = (int)sizeof(cwfile) + cw_base->szOsFile; cw_vfs.xOpen = cw_open;
    sqlite3_vfs_register(&cw_vfs, 0);
  }
  return "cursed-wal";
}

// ---------------------------------------------------------------- engine state
enum { C_P, C_U, C_L, C_C, C_A, NRC };                          // read cursors: posts, users, likes_post_id, posts_cat, likes autoindex
enum { W_P, W_PU, W_PC, W_L, W_LA, W_LP, NWC };                 // write cursors
struct eng {
  sqlite3 *db; Btree *bt; Pager *pager; eng_opts o; int fmt;
  Pgno r_posts, r_users, r_likes, r_lpid, r_lauto, r_pcat, r_puid;
  KeyInfo *ki_lpid, *ki_lauto, *ki_pcat, *ki_puid;
  UnpackedRecord *uk_lpid, *uk_lauto;
  BtCursor *rc[NRC], *wc[NWC]; unsigned rc_open, wc_open;
  int in_batch, txn, sql_txn, failed;        // txn: 0 none, 1 read, 2 write (direct B-tree level)
  sqlite3_stmt *s_begin, *s_commit, *s_rollback, *s_health, *s_feed, *s_post, *s_ids, *s_rlikes, *s_rposts,
               *s_exists, *s_ins, *s_like;
  int64_t (*clock)(void); int64_t ts_ms; char ts[24];
  u8 *scratch[2]; u32 scratch_n[2]; u8 rec[16384 + 128];
  u32 counts[RANGE_MAX];
  int wal_frames, wrote_sql; eng_stats st;
  int coalesce; eng_body *fshared; char *fbuf;   // feed coalescing: the batch's shared feed body
  const char *path;
#if SQLITE_THREADSAFE
  pthread_t ck; pthread_mutex_t ck_mu; pthread_cond_t ck_cv; int ck_on, ck_want, ck_stop; sqlite3 *ck_db;
#endif
  int drain; double drain_t0;   // ckpt_thread: writes are being held so the checkpoint can catch up
  struct ack *ack;              // ckpt_async: the event-loop checkpoint's state (see "asynchronous checkpoint")
  eng_fop_submit fop_submit; void *fop_ctx;
};

void eng_default_opts(eng_opts *o) {
  memset(o, 0, sizeof *o);
  // measured choices, see RESULTS.md: direct writes pass every check but are not faster than prepared INSERTs
  o->direct_reads = 1; o->direct_writes = 0; o->feed_range = 1; o->walk_cells = 1; o->exclusive = 1; o->feed_batch = 1;   // feed_batch: Xeon exp16 -8% feed (7.11 vs 7.69-7.77 µs)
  o->wal_coalesce = 1; o->wal_fadvise = 1; o->mmap_size = 0; o->cache_kib = 16384; o->ckpt_mode = SQLITE_CHECKPOINT_PASSIVE;
  // On the Xeon's disk an inline checkpoint is a 130-270 ms stall (p99 129-216 ms at 340k-user pacing); the thread
  // with the write gate gave p99 5 ms (exp15). A THREADSAFE build therefore checkpoints from the thread by default.
  // ckpt_nice 0: Xeon exp47 at ~92% busy, p99 15.1 ms at nice 0, 16.4 at 5, 44.3 at 19. A starved thread only moves its
  // work into held writes and queueing (pegged-CPU test_wal at nice 19: 33 of 50 drains timed out).
  o->ckpt_thread = SQLITE_THREADSAFE != 0; o->ckpt_nice = 0;
  // Thread: passes of 4096 frames, drain mark 65536 (ckpt_restart). Xeon exp35 with wal_fadvise: p99 2.7 ms at ~55% busy
  // and 10.2 ms at ~90%, WAL page cache <= 20 MiB. Passes of 16384 frames: 2.5 ms at 55% but 344 ms at ~90% (a 1.2 s
  // pass), 60-78 MiB. (exp17: a drain mark of 16384 instead of 65536 means many more drains: writes p99 50 ms.)
  o->ckpt_frames = o->ckpt_thread ? 4096 : 16384;
}

// Sleep, never spin, on SQLITE_BUSY: the holder may be the low-priority checkpoint thread on the same CPU.
static int busy_sleep(void *arg, int n) {
  eng *e = arg;
  if (n >= 4000) return 0;                                  // ~2 s in total, then the batch fails (500s)
  struct timespec ts = {0, n < 10 ? 20000 : n < 100 ? 100000 : 500000};
  nanosleep(&ts, 0); __atomic_add_fetch(&e->st.busy_sleeps, 1, __ATOMIC_RELAXED);
  return 1;
}
static int busy_sleep_ck(void *arg, int n) { (void)arg; if (n >= 20000) return 0; struct timespec ts = {0, 200000}; nanosleep(&ts, 0); return 1; }
static double mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
// frames in the WAL and frames not yet copied into the database (as of this connection's last transaction)
static void wal_state(eng *e, u32 *mx, u32 *pending) {
  Wal *w = e->pager->pWal; *mx = *pending = 0;
  if (!w || !w->nWiData || !w->apWiData[0]) return;
  u32 bf = walCkptInfo(w)->nBackfill; *mx = w->hdr.mxFrame; *pending = *mx > bf ? *mx - bf : 0;
}
static void note_ckpt(eng *e, double ms, int nck) {
  __atomic_add_fetch(&e->st.checkpoints, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&e->st.ckpt_frames, (u64)nck, __ATOMIC_RELAXED);
  e->st.ckpt_total_ms += ms; if (ms > e->st.ckpt_max_ms) e->st.ckpt_max_ms = ms;
}


// Drop WAL frames (from, to] from the OS page cache once a checkpoint has copied them into the database (and
// fsynced the WAL first). Readers never read backfilled frames again: a read transaction starts at
// minFrame = nBackfill + 1 and takes those pages from the database file. Without this the WAL's page cache grows
// to the high mark (65536 frames = ~256 MB, charged to the server's cgroup) before the WAL restarts. Only whole
// 4 KiB pages strictly inside the range are dropped: frames are 24 + 4096 bytes, so the boundary pages hold
// neighbouring frames. Linux only; elsewhere a no-op.
static void wal_drop(eng *e, sqlite3_file *wfd, u32 from, u32 to) {
#if defined(__linux__) && defined(POSIX_FADV_DONTNEED)
  if (!e->o.wal_fadvise || !wfd || to <= from) return;
  if (wfd->pMethods == &cw_io) wfd = ((cwfile *)wfd)->real;   // the shim wraps the unix file
  if (wfd->pMethods->xRead != unixRead) { e->st.wal_fadvise_skipped++; return; }   // a unix-VFS file; not a test shim
  i64 sz = 4096 + 24, a = 32 + (i64)from * sz, b = 32 + (i64)to * sz;   // byte range of frames from+1 .. to
  a = (a + 4095) & ~(i64)4095; b &= ~(i64)4095;
  if (b > a && posix_fadvise(((unixFile *)wfd)->h, a, b - a, POSIX_FADV_DONTNEED) == 0)
    __atomic_add_fetch(&e->st.wal_dropped_bytes, (u64)(b - a), __ATOMIC_RELAXED);
#else
  (void)e; (void)wfd; (void)from; (void)to;
#endif
}
static sqlite3_file *conn_wal_fd(sqlite3 *db) {
  Pager *pg = sqlite3BtreePager(db->aDb[0].pBt);
  return pg && pg->pWal ? pg->pWal->pWalFd : 0;
}

// ---------------------------------------------------------------- asynchronous checkpoint (ckpt_async, "DB_CKPT=uring")
// SQLite's walCheckpoint() as a state machine advanced by the event loop, with the file I/O submitted as batches of
// file ops (io_uring through the server's loop, or the synchronous fallback below). Same steps and the same order:
//   1. snapshot mxSafeFrame = mxFrame, mxPage = nPage, claim the read marks (as walCheckpoint does; locks are no-ops
//      in EXCLUSIVE mode, and we run between batches, so no read transaction is open);
//   2. the pages to copy: SQLite's own walIteratorInit/Next (latest frame of each page in (nBackfill, mxSafeFrame]);
//   3. fsync the WAL, linked before the first database write (a database page must never be durable before the WAL
//      frames it came from);
//   4. copy the frames into the database file, in chunks of linked writes (frame data read from the WAL with pread:
//      recently written, so a page-cache copy);
//   5. if no frame was added meanwhile, truncate the database to nPage pages (as walCheckpoint does);
//   6. fdatasync the database, and only when that completes store nBackfill = mxSafeFrame.
// Readers are unaffected while this runs: a page whose frame is being copied is still read from the WAL
// (minFrame <= that frame) until nBackfill moves. The WAL restarts at the next write after a full backfill, done by
// SQLite itself (walRestartLog), with the same write gate as ckpt_thread around that moment. No SQLite checkpoint may
// run while a cycle has writes in flight: a newer version of a page could be overwritten by an older in-flight write.
// This re-implements SQLite's checkpoint step (same file format, same ordering): see RESULTS.md, "grey".
enum { ACK_IDLE, ACK_COPY, ACK_SYNC };
#define ACK_CHUNK 256
typedef struct { u32 pg, frame; } ack_pf;
struct ack {
  int state, inflight, err, dbfd, walfd, szpage, nchunks;
  u32 nb0, mxsafe, mxpage, npf, next, salt[2];
  ack_pf *pf; u8 *buf; eng_fop ops[2 * ACK_CHUNK + 2]; double t0;
  void (*trace)(void *, int); void *trace_ctx;
  int quiesce;   // eng_ckpt_quiesce(): finish the cycle in flight, start no new one
};
// The file descriptor under a database / WAL handle: through our shim to the unix file, or asked from another
// wrapping VFS with the ENG_FCNTL_GETFD file control (a test VFS answers it).
static int unix_fd(sqlite3_file *f) {
  if (f && f->pMethods == &cw_io) f = ((cwfile *)f)->real;
  if (f && f->pMethods && f->pMethods->xRead == unixRead) return ((unixFile *)f)->h;
  int fd = -1;
  if (f && f->pMethods && f->pMethods->xFileControl(f, ENG_FCNTL_GETFD, &fd) == SQLITE_OK) return fd;
  return -1;
}
static void ack_cb(void *loop, void *ud, int res) {
  (void)loop;
  eng *e = ud; struct ack *a = e->ack;
  if (res < 0) a->err = res;   // short reads/writes arrive as errors (the backends check len)
  a->inflight--;
}
// the synchronous fallback (also used by tests): runs the ops in order now, honouring the link semantics
static int fop_sync(void *ctx, const eng_fop *ops, int n) {
  int cancel = 0;
  for (int i = 0; i < n; i++) {
    const eng_fop *op = &ops[i]; int res;
    if (cancel) res = -ECANCELED;
    else if (op->op == ENG_FOP_WRITE) { ssize_t w = pwrite(op->fd, op->buf, op->len, (off_t)op->off); res = w < 0 ? -errno : (int)w; if (w >= 0 && (uint32_t)w != op->len) res = -EIO; }
    else if (op->op == ENG_FOP_READ) { ssize_t r = pread(op->fd, op->buf, op->len, (off_t)op->off); res = r < 0 ? -errno : (int)r; if (r >= 0 && (uint32_t)r != op->len) res = -EIO; }
    else if (op->op == ENG_FOP_FDATASYNC) res = fdatasync(op->fd) ? -errno : 0;
    else res = fsync(op->fd) ? -errno : 0;
    if (res < 0 && op->link) cancel = 1;
    else if (!op->link) cancel = 0;
    op->cb(ctx, op->ud, res);
  }
  return 0;
}
static void ack_trace(eng *e, int step) { if (e->ack->trace) e->ack->trace(e->ack->trace_ctx, step); }
static void ack_end(eng *e, int ok) {
  struct ack *a = e->ack;
  free(a->pf); a->pf = 0;
  a->state = ACK_IDLE;
  if (!ok) e->st.ack_errors++;
}
static int ack_submit(eng *e, eng_fop *ops, int n) {
  struct ack *a = e->ack; a->inflight += n;
  int rc = (e->fop_submit ? e->fop_submit : fop_sync)(e->fop_ctx, ops, n);
  if (rc) { a->inflight -= n; a->err = rc; }
  return rc;
}
// submit the next chunk: [fsync(WAL)] -> read frame 1 -> write page 1 -> read frame 2 -> ... all linked, so the loop
// itself copies nothing (the reads run with the writes, off the loop thread on io_uring)
static int ack_copy_chunk(eng *e) {
  struct ack *a = e->ack; int k = 0;
#ifndef ENG_TEST_BUG_NO_WAL_SYNC
  if (a->next == 0) a->ops[k++] = (eng_fop){a->walfd, ENG_FOP_FSYNC, 1, 0, 0, 0, ack_cb, e};
#endif
  u32 end = a->next + ACK_CHUNK < a->npf ? a->next + ACK_CHUNK : a->npf;
  for (u32 i = a->next; i < end; i++) {
    u8 *b = a->buf + (size_t)(i - a->next) * (size_t)a->szpage;
    i64 woff = walFrameOffset(a->pf[i].frame, a->szpage) + WAL_FRAME_HDRSIZE;
    a->ops[k++] = (eng_fop){a->walfd, ENG_FOP_READ, 1, b, (uint32_t)a->szpage, (uint64_t)woff, ack_cb, e};
    a->ops[k++] = (eng_fop){a->dbfd, ENG_FOP_WRITE, (u8)(i + 1 < end), b, (uint32_t)a->szpage, (uint64_t)(a->pf[i].pg - 1) * (uint64_t)a->szpage, ack_cb, e};
  }
  a->next = end; a->nchunks++;
  return ack_submit(e, a->ops, k);
}
// start a cycle (between batches only)
static void ack_start(eng *e) {
  struct ack *a = e->ack; Wal *w = e->pager->pWal;
  if (a->state != ACK_IDLE || a->quiesce || e->in_batch || !w || !w->nWiData || !w->apWiData[0]) return;
  volatile WalCkptInfo *ci = walCkptInfo(w);
  // the main connection's view of the WAL index is refreshed at each read transaction; between batches it is current
  u32 mxsafe = w->hdr.mxFrame, nb = ci->nBackfill;
  if (nb >= mxsafe) return;
  a->dbfd = unix_fd(e->pager->fd); a->walfd = unix_fd(w->pWalFd); a->szpage = walPagesize(w);
  if (a->dbfd < 0 || a->walfd < 0) { e->st.ack_errors++; return; }
  for (int i = 1; i < WAL_NREADER; i++) {            // walCheckpoint's read-mark update (no reader can hold one now)
    u32 y = ci->aReadMark[i];
    if (mxsafe > y) ci->aReadMark[i] = i == 1 ? mxsafe : READMARK_NOT_USED;
  }
  WalIterator *it = 0;
  if (walIteratorInit(w, nb, &it) != SQLITE_OK || !it) { e->st.ack_errors++; return; }
  u32 cap = mxsafe - nb + 16, n = 0, pg, fr; ack_pf *pf = malloc(sizeof *pf * cap);
  if (!pf) { walIteratorFree(it); e->st.ack_errors++; return; }
  while (walIteratorNext(it, &pg, &fr) == 0) {   // in page order: the database writes go out sorted
    if (fr <= nb || fr > mxsafe || pg > w->hdr.nPage) continue;
    if (n == cap) { cap *= 2; ack_pf *q = realloc(pf, sizeof *pf * cap); if (!q) { free(pf); walIteratorFree(it); e->st.ack_errors++; return; } pf = q; }
    pf[n].pg = pg; pf[n].frame = fr; n++;
  }
  walIteratorFree(it);
  if (!a->buf && !(a->buf = malloc((size_t)ACK_CHUNK * (size_t)a->szpage))) { free(pf); e->st.ack_errors++; return; }
  a->pf = pf; a->npf = n; a->next = 0; a->nb0 = nb; a->mxsafe = mxsafe; a->mxpage = w->hdr.nPage; a->err = 0; a->nchunks = 0;
  a->salt[0] = w->hdr.aSalt[0]; a->salt[1] = w->hdr.aSalt[1];
  ci->nBackfillAttempted = mxsafe;
#ifdef ENG_TEST_BUG_EARLY_BACKFILL
  ci->nBackfill = mxsafe;   // test-only bug: record the backfill before the copy is durable
#endif
  a->state = ACK_COPY; a->t0 = mono_ms(); e->st.ack_cycles++;
  ack_trace(e, 1);
  if (n == 0) {   // nothing to copy (every page beyond nPage): still sync the WAL, as SQLite does
    eng_fop op = {a->walfd, ENG_FOP_FSYNC, 0, 0, 0, 0, ack_cb, e};
    a->next = 0; ack_submit(e, &op, 1);
  } else ack_copy_chunk(e);
  ack_trace(e, 2);
}
// advance the state machine: call between batches (eng_tick, eng_after_batch)
static void ack_step(eng *e) {
  struct ack *a = e->ack;
  if (!a || a->state == ACK_IDLE || a->inflight > 0 || e->in_batch) return;
  sqlite3_file *dbf = e->pager->fd;
  if (dbf->pMethods == &cw_io) ((cwfile *)dbf)->size_ok = 0;   // our writes bypass the shim: its cached size is stale
  if (a->err) { ack_end(e, 0); ack_trace(e, 9); return; }
  Wal *w = e->pager->pWal;
  if (a->state == ACK_COPY) {
    if (a->next < a->npf) { ack_trace(e, 3); if (ack_copy_chunk(e)) { ack_end(e, 0); ack_trace(e, 9); } return; }
    // all pages written: truncate if nothing was added meanwhile (walCheckpoint does this), then fdatasync the db
    if (a->mxsafe == walIndexHdr(w)->mxFrame) {
      struct stat st; i64 sz = (i64)a->mxpage * a->szpage;
      if (fstat(a->dbfd, &st) == 0 && st.st_size > sz && ftruncate(a->dbfd, (off_t)sz)) { a->err = -errno; ack_end(e, 0); ack_trace(e, 9); return; }
    }
    a->state = ACK_SYNC; ack_trace(e, 4);
    eng_fop op = {a->dbfd, ENG_FOP_FDATASYNC, 0, 0, 0, 0, ack_cb, e};
    if (ack_submit(e, &op, 1)) { ack_end(e, 0); ack_trace(e, 9); }
    return;
  }
  // ACK_SYNC done: the copied pages are durable. Record the backfill, unless the WAL was restarted meanwhile (cannot
  // happen while nBackfill < mxFrame, but checked like walCheckpoint's salt test)
  if (w->hdr.aSalt[0] == a->salt[0] && w->hdr.aSalt[1] == a->salt[1]) {
    volatile WalCkptInfo *ci = walCkptInfo(w);
#ifdef ENG_TEST_BUG_EARLY_BACKFILL
    ci->nBackfill = a->nb0;
#endif
    if (ci->nBackfill == a->nb0) {
      ci->nBackfill = a->mxsafe;
      note_ckpt(e, mono_ms() - a->t0, (int)(a->mxsafe - a->nb0));
      wal_drop(e, w->pWalFd, a->nb0, a->mxsafe);
    }
  }
  ack_end(e, 1);
  ack_trace(e, 5);
}
void eng_set_fileops(eng *e, eng_fop_submit submit, void *ctx) { e->fop_submit = submit; e->fop_ctx = ctx; }
void eng_set_ckpt_trace(eng *e, void (*fn)(void *, int), void *ctx) { if (e->ack) { e->ack->trace = fn; e->ack->trace_ctx = ctx; } }
int eng_ckpt_inflight(eng *e) { return e->ack ? e->ack->inflight : 0; }
void eng_ckpt_quiesce(eng *e) { if (e->ack) e->ack->quiesce = 1; }
void eng_tick(eng *e) {
  if (!e->ack || e->in_batch) return;
  ack_step(e);
  if (e->ack->state == ACK_IDLE) {
    u32 mx, pend; wal_state(e, &mx, &pend);
    if (pend >= (u32)e->o.ckpt_frames || (e->drain && pend > 0)) ack_start(e);
  }
}

#if SQLITE_THREADSAFE
static void *ck_main(void *arg) {
  eng *e = arg;
#ifdef __linux__
  // Not SCHED_IDLE, and by default not even a raised nice value: a low-priority thread is starved under full load
  // while it holds the checkpoint lock, so drains time out, writes stay held and the WAL grows (SCHED_IDLE: 895k
  // frames in a flat-out hold; nice 19: Xeon exp47 p99 44 ms vs 15 ms at nice 0, ~92% busy).
  if (e->o.ckpt_nice > 0) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), e->o.ckpt_nice);
#endif
  u32 bf_prev = 0;   // frames already dropped from the page cache
  pthread_mutex_lock(&e->ck_mu);
  for (;;) {
    while (!e->ck_want && !e->ck_stop) pthread_cond_wait(&e->ck_cv, &e->ck_mu);
    if (e->ck_stop) break;
    e->ck_want = 0;
    pthread_mutex_unlock(&e->ck_mu);
    // PASSIVE only: it never takes the writer lock, so the loop never waits on the checkpoint's fsyncs. While the
    // main connection holds writes back (drain), repeat until every frame is in the database; the next write then
    // starts the WAL over. No RESTART: on the real disk its fsync under the writer lock costs 30-100 ms of p99.
    for (int pass = 0; pass < 16; pass++) {
      int nlog = 0, nck = 0; double t0 = mono_ms();
      if (sqlite3_wal_checkpoint_v2(e->ck_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &nck) != SQLITE_OK) break;
      if ((u32)nck < bf_prev) { bf_prev = 0; e->st.wal_resets++; }   // the WAL restarted since the last pass
      note_ckpt(e, mono_ms() - t0, (int)((u32)nck - bf_prev));      // nck is cumulative: count only this pass's frames
      wal_drop(e, conn_wal_fd(e->ck_db), bf_prev, (u32)nck); bf_prev = (u32)nck;
      if (!__atomic_load_n(&e->drain, __ATOMIC_ACQUIRE) || nck >= nlog) break;
      struct timespec ts = {0, 200000}; nanosleep(&ts, 0);   // a write batch may still be finishing
    }
    pthread_mutex_lock(&e->ck_mu);
  }
  pthread_mutex_unlock(&e->ck_mu);
  return 0;
}
static int ck_start(eng *e) {
  if (sqlite3_open_v2(e->path, &e->ck_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) return -1;
  sqlite3_busy_handler(e->ck_db, busy_sleep_ck, 0);
  if (sqlite3_exec(e->ck_db, "PRAGMA locking_mode=NORMAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=16; PRAGMA wal_autocheckpoint=0;", 0, 0, 0)) return -1;
  pthread_mutex_init(&e->ck_mu, 0); pthread_cond_init(&e->ck_cv, 0);
  if (pthread_create(&e->ck, 0, ck_main, e)) return -1;
  e->ck_on = 1; return 0;
}
static void ck_signal(eng *e) { pthread_mutex_lock(&e->ck_mu); e->ck_want = 1; pthread_cond_signal(&e->ck_cv); pthread_mutex_unlock(&e->ck_mu); }
static void ck_stop(eng *e) {
  if (e->ck_on) {
    pthread_mutex_lock(&e->ck_mu); e->ck_stop = 1; pthread_cond_signal(&e->ck_cv); pthread_mutex_unlock(&e->ck_mu);
    pthread_join(e->ck, 0); e->ck_on = 0;
  }
  if (e->ck_db) { sqlite3_close(e->ck_db); e->ck_db = 0; }
}
#else
static int ck_start(eng *e) { (void)e; return -1; }
static void ck_signal(eng *e) { (void)e; }
static void ck_stop(eng *e) { (void)e; }
#endif

static int wal_hook(void *arg, sqlite3 *db, const char *name, int frames) { (void)db; (void)name; ((eng *)arg)->wal_frames = frames; return SQLITE_OK; }

static BtCursor *mkcur(void) { BtCursor *c = sqlite3MallocZero(sqlite3BtreeCursorSize()); if (c) sqlite3BtreeCursorZero(c); return c; }

static int prep(eng *e, const char *sql, sqlite3_stmt **s) { return sqlite3_prepare_v3(e->db, sql, -1, SQLITE_PREPARE_PERSISTENT, s, 0); }
#if ENG_TURBO
// the bodies w_post writes, as qc templates: i = integer, s = JSON string (JSON.stringify's escapes, as eng_json_escape)
#define TJ_POST { "{\"id\":", ",\"body\":", ",\"created_at\":", ",\"author\":", ",\"like_count\":", "}" }
static const QcJson tj_feed = { "{\"posts\":[", "]}", ",", 5, "isssi", TJ_POST };
static const QcJson tj_post = { "{\"post\":", "}", ",", 5, "isssi", TJ_POST };
// 1: qc wrote the body (*rows rows); 0: QC_FALLBACK, step the statement as usual
static int turbo_body(eng_stats *st, sqlite3_stmt *s, const i64 *param, char *out, size_t cap, size_t *n, int *rows) {
  QcOut o = { out, cap, 0, 0, param, param ? 1 : 0 };
  if (sqlite3_qc_exec(s, 0, &o) != SQLITE_OK) { st->turbo_fallbacks++; return 0; }
  st->turbo_served++; *n = o.n; *rows = o.rows; return 1;
}
#endif

#define POSTQ "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count FROM posts p JOIN users u ON u.id = p.user_id "

eng *eng_open(const char *path, const eng_opts *opts, char *err, size_t errn) {
  eng *e = calloc(1, sizeof *e); char sql[512]; const char *why = "out of memory";
  if (!e) goto fail0;
  if (opts) e->o = *opts; else eng_default_opts(&e->o);
  e->clock = realtime_ms; e->ts_ms = INT64_MIN; e->coalesce = ENG_COALESCE_DEFAULT;
  e->path = path;
  if (e->o.ckpt_hard <= 0) e->o.ckpt_hard = 4 * (e->o.ckpt_frames > 0 ? e->o.ckpt_frames : 1000);
  // drain mark (independent of the pass size): Xeon exp39/40 sweep 8k/16k/32k/64k with wal_fadvise. 32768 keeps
  // 64k's tail (p99 3.8 / 15.8 ms at 56 / 92% busy) with half the WAL file; 8k-16k save < 10 MiB but writes p99 100-320 ms
  if (e->o.ckpt_restart <= 0) e->o.ckpt_restart = 32768;
  if (e->o.ckpt_thread && !(opts && opts->ckpt_hard > 0)) e->o.ckpt_hard = 4 * e->o.ckpt_restart;
  if (e->o.ckpt_drain_ms <= 0) e->o.ckpt_drain_ms = 1000;
  if (e->o.ckpt_drain_frames <= 0) e->o.ckpt_drain_frames = 256;
  if (e->o.ckpt_nice < 0 || e->o.ckpt_nice > 19) e->o.ckpt_nice = 0;
  if (e->o.ckpt_thread) {
    why = "ckpt_thread needs a SQLITE_THREADSAFE=2 build";
    if (!sqlite3_threadsafe() || e->o.ckpt_frames <= 0) goto fail;
    e->o.exclusive = 0;   // two connections; "unix-excl" keeps other processes out and the WAL index in heap memory
  }
  const char *vfs = e->o.wal_coalesce ? cw_register(e->o.ckpt_thread) : e->o.ckpt_thread ? "unix-excl" : 0;
  if (sqlite3_open_v2(path, &e->db, SQLITE_OPEN_READWRITE, vfs) != SQLITE_OK) { why = e->db ? sqlite3_errmsg(e->db) : "open failed"; goto fail; }
  if (e->o.ckpt_thread) sqlite3_busy_handler(e->db, busy_sleep, e);
  snprintf(sql, sizeof sql,
           "PRAGMA locking_mode=%s; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-%d;"
           "PRAGMA mmap_size=%lld; PRAGMA temp_store=MEMORY; PRAGMA wal_autocheckpoint=%d; PRAGMA journal_size_limit=%d;"
           "SELECT count(*) FROM sqlite_schema;",
           e->o.exclusive ? "EXCLUSIVE" : "NORMAL", e->o.cache_kib, (long long)e->o.mmap_size, e->o.ckpt_frames > 0 ? 0 : 1000,
           // with wal_fadvise the old WAL pages are gone from the page cache: truncate the WAL at the first commit after a
           // restart, so later writes append past EOF instead of partly overwriting uncached pages (a read each)
           e->o.wal_fadvise ? 0 : -1);
  if (sqlite3_exec(e->db, sql, 0, 0, 0) != SQLITE_OK) { why = sqlite3_errmsg(e->db); goto fail; }
  if (e->o.ckpt_frames > 0) sqlite3_wal_hook(e->db, wal_hook, e);
  sqlite3 *db = e->db;
  // 3.54.0 marks "WHERE p.id = ?" as reprepare-on-bind (explore/sqlite-versions): without QPSG the SQL path
  // re-plans the post query on every bind
  sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_QPSG, 1, 0);
  e->bt = db->aDb[0].pBt; e->pager = sqlite3BtreePager(e->bt); e->fmt = db->aDb[0].pSchema->file_format;
  Table *tp = sqlite3FindTable(db, "posts", "main"), *tu = sqlite3FindTable(db, "users", "main"), *tl = sqlite3FindTable(db, "likes", "main");
  Index *ilp = sqlite3FindIndex(db, "likes_post_id_idx", "main"), *ila = sqlite3FindIndex(db, "sqlite_autoindex_likes_1", "main"),
        *ipc = sqlite3FindIndex(db, "posts_created_at_id_idx", "main"), *ipu = sqlite3FindIndex(db, "posts_user_id_idx", "main");
  why = "schema mismatch";
  if (!tp || !tu || !tl || !ilp || !ila || !ipc || !ipu) goto fail;
  // the B-tree code below hard-codes column positions and the index order of OP_IdxInsert: verify them
  if (tp->nCol != 4 || tp->iPKey != 0 || tu->nCol != 3 || tu->iPKey != 0 || tl->nCol != 3 || HasRowid(tl) == 0 ||
      ila->nKeyCol != 2 || ila->aiColumn[0] != 0 || ila->aiColumn[1] != 1 || ilp->nKeyCol != 1 || ilp->aiColumn[0] != 1 ||
      ipc->nKeyCol != 2 || ipc->aiColumn[0] != 3 || ipc->aiColumn[1] != 0 || ipu->nKeyCol != 1 || ipu->aiColumn[0] != 1 ||
      tp->pIndex == 0 || tl->pIndex == 0)
    goto fail;
  e->r_posts = tp->tnum; e->r_users = tu->tnum; e->r_likes = tl->tnum;
  e->r_lpid = ilp->tnum; e->r_lauto = ila->tnum; e->r_pcat = ipc->tnum; e->r_puid = ipu->tnum;
  Parse ps; memset(&ps, 0, sizeof ps); ps.db = db;
  e->ki_lpid = sqlite3KeyInfoOfIndex(&ps, ilp); e->ki_lauto = sqlite3KeyInfoOfIndex(&ps, ila);
  e->ki_pcat = sqlite3KeyInfoOfIndex(&ps, ipc); e->ki_puid = sqlite3KeyInfoOfIndex(&ps, ipu);
  why = "keyinfo";
  if (!e->ki_lpid || !e->ki_lauto || !e->ki_pcat || !e->ki_puid) goto fail;
  e->uk_lpid = sqlite3VdbeAllocUnpackedRecord(e->ki_lpid); e->uk_lauto = sqlite3VdbeAllocUnpackedRecord(e->ki_lauto);
  for (int i = 0; i < NRC; i++) if (!(e->rc[i] = mkcur())) goto fail;
  for (int i = 0; i < NWC; i++) if (!(e->wc[i] = mkcur())) goto fail;
  why = "prepare";
  if (e->o.ckpt_thread && ck_start(e)) { why = "checkpoint thread"; goto fail; }
  if (e->o.ckpt_async) {
    why = "ckpt_async needs EXCLUSIVE locking, no checkpoint thread and no mmap";
    if (e->o.ckpt_thread || !e->o.exclusive || e->o.mmap_size > 0 || e->o.ckpt_frames <= 0) goto fail;
    if (!(e->ack = calloc(1, sizeof *e->ack))) goto fail;
  }
  if (prep(e, "BEGIN", &e->s_begin) || prep(e, "COMMIT", &e->s_commit) || prep(e, "ROLLBACK", &e->s_rollback) ||
      prep(e, "SELECT 1", &e->s_health) ||
      prep(e, POSTQ "ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &e->s_feed) || prep(e, POSTQ "WHERE p.id = ?1", &e->s_post) ||
      prep(e, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20", &e->s_ids) ||
      prep(e, "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2", &e->s_rlikes) ||
      prep(e, "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2", &e->s_rposts) ||
      prep(e, "SELECT 1 FROM posts WHERE id = ?1", &e->s_exists) ||
      prep(e, "INSERT INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)", &e->s_ins) ||
      prep(e, "INSERT INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, ?3) ON CONFLICT (user_id, post_id) DO NOTHING", &e->s_like))
    { why = sqlite3_errmsg(db); goto fail; }
#if ENG_TURBO
  if (e->o.turbo_reads && !e->o.direct_reads) {
    e->o.feed_range = 0;   // qc compiles the spec's one-statement feed (its count subquery becomes one range walk)
    // a statement qc doesn't take stays on SQLite's interpreter; log it, the server still works
    if (sqlite3_qc(e->s_feed) != 1 || !sqlite3_qc_json(e->s_feed, &tj_feed)) fprintf(stderr, "turbo: feed not compiled (%s)\n", qc_why);
    if (sqlite3_qc(e->s_post) != 1 || !sqlite3_qc_json(e->s_post, &tj_post)) fprintf(stderr, "turbo: post not compiled (%s)\n", qc_why);
  }
#else
  e->o.turbo_reads = 0;
#endif
  return e;
fail:
  if (err) snprintf(err, errn, "%s", why);
  eng_close(e);
  return 0;
fail0:
  if (err) snprintf(err, errn, "%s", why);
  return 0;
}

void eng_close(eng *e) {
  if (!e) return;
  ck_stop(e);
  if (e->ack) {
    if (e->ack->inflight > 0) {
      // the server must drain (eng_ckpt_inflight() == 0) before closing; if it didn't, the kernel may still write from
      // our buffers: leak them, and don't let SQLite checkpoint on close underneath in-flight writes
      fprintf(stderr, "eng_close: %d checkpoint ops still in flight\n", e->ack->inflight);
      sqlite3_db_config(e->db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, 0);
    } else { free(e->ack->buf); free(e->ack->pf); free(e->ack); }
    e->ack = 0;
  }
  sqlite3_stmt **ss[] = {&e->s_begin, &e->s_commit, &e->s_rollback, &e->s_health, &e->s_feed, &e->s_post, &e->s_ids, &e->s_rlikes,
                         &e->s_rposts, &e->s_exists, &e->s_ins, &e->s_like};
#if ENG_TURBO && QC_STOCK
  for (size_t i = 0; i < sizeof ss / sizeof *ss; i++) if (*ss[i]) sqlite3_qc_forget(*ss[i]);
#endif
  for (size_t i = 0; i < sizeof ss / sizeof *ss; i++) sqlite3_finalize(*ss[i]);
  for (int i = 0; i < NRC; i++) sqlite3_free(e->rc[i]);
  for (int i = 0; i < NWC; i++) sqlite3_free(e->wc[i]);
  if (e->uk_lpid) sqlite3DbFree(e->db, e->uk_lpid);
  if (e->uk_lauto) sqlite3DbFree(e->db, e->uk_lauto);
  KeyInfo *ks[] = {e->ki_lpid, e->ki_lauto, e->ki_pcat, e->ki_puid};
  for (int i = 0; i < 4; i++) if (ks[i]) sqlite3KeyInfoUnref(ks[i]);
  free(e->scratch[0]); free(e->scratch[1]);
  if (e->fshared) eng_body_unref(e->fshared);
  free(e->fbuf);
  if (e->db) sqlite3_close(e->db);
  free(e);
}

void eng_get_stats(eng *e, eng_stats *s) { *s = e->st; s->wal_flushes = cw_flushes; s->wal_writes = cw_writes; }
void eng_set_clock(eng *e, int64_t (*f)(void)) { e->clock = f ? f : realtime_ms; e->ts_ms = INT64_MIN; }
sqlite3 *eng_db(eng *e) { return e->db; }

static const char *now_ts(eng *e) {   // formatted once per millisecond
  int64_t ms = e->clock();
  if (ms != e->ts_ms) { e->ts_ms = ms; eng_format_ts(e->ts, ms); }
  return e->ts;
}

// ---------------------------------------------------------------- transactions
// Direct B-tree transaction: read (wr=0) or write (wr=1, upgrades an open read transaction).
static int bt_txn(eng *e, int wr) {
  if (e->txn > wr) return 0;
  if (e->txn == 1 && wr) {   // read -> write upgrade; open read cursors stay valid
    if (sqlite3BtreeBeginTrans(e->bt, 1, 0) != SQLITE_OK) return -1;
    e->txn = 2; return 0;
  }
  int cookie;
  if (sqlite3BtreeBeginTrans(e->bt, wr, &cookie) != SQLITE_OK) return -1;
  if (cookie != e->db->aDb[0].pSchema->schema_cookie) { sqlite3BtreeCommit(e->bt); return -1; }   // schema changed under us
  e->txn = wr ? 2 : 1;
  return 0;
}
// Take the write lock at the start of a like / create. With a second connection (ckpt_thread) the upgrade of a
// read transaction can hit SQLITE_BUSY (a RESTART checkpoint holds the writer), and an upgrade never waits:
// then drop the read snapshot (no op is in flight between ops) and wait for the lock from no transaction,
// where the busy handler applies. Earlier reads of the batch were consistent at their time.
static void close_cursors(eng *e);
static int writer_upgrade(eng *e) {
  if (e->txn == 2) return 0;
  if (e->txn == 1) {
    int rc = sqlite3BtreeBeginTrans(e->bt, 1, 0);
    if (rc == SQLITE_OK) { e->txn = 2; return 0; }
    if ((rc & 0xff) != SQLITE_BUSY) return -1;
    close_cursors(e);
    if (e->sql_txn) {
      int r2 = sqlite3_step(e->s_commit); sqlite3_reset(e->s_commit); e->sql_txn = 0;
      if (r2 != SQLITE_DONE) return -1;
    } else sqlite3BtreeCommit(e->bt);
    e->txn = 0;
  }
  return bt_txn(e, 1);
}

// SQL statements may only run inside BEGIN..COMMIT during a batch: in autocommit mode their Halt would
// commit the B-tree transaction that the direct cursors live in.
static int sql_txn(eng *e) {
  if (e->sql_txn) return 0;
  int rc = sqlite3_step(e->s_begin); sqlite3_reset(e->s_begin);
  if (rc != SQLITE_DONE) return -1;
  e->sql_txn = 1; return 0;
}
static BtCursor *rcur(eng *e, int k) {
  if (!(e->rc_open & (1u << k))) {
    if (bt_txn(e, 0)) return 0;
    Pgno root = k == C_P ? e->r_posts : k == C_U ? e->r_users : k == C_L ? e->r_lpid : k == C_C ? e->r_pcat : e->r_lauto;
    KeyInfo *ki = k == C_L ? e->ki_lpid : k == C_C ? e->ki_pcat : k == C_A ? e->ki_lauto : 0;
    sqlite3BtreeCursorZero(e->rc[k]);   // CloseCursor frees pKey/aOverflow without clearing them
    if (sqlite3BtreeCursor(e->bt, root, 0, ki, e->rc[k]) != SQLITE_OK) return 0;
    e->rc_open |= 1u << k;
  }
  return e->rc[k];
}
static BtCursor *wcur(eng *e, int k) {
  if (!(e->wc_open & (1u << k))) {
    if (bt_txn(e, 1)) return 0;
    Pgno root; KeyInfo *ki = 0;
    switch (k) {
      case W_P: root = e->r_posts; break;
      case W_PU: root = e->r_puid; ki = e->ki_puid; break;
      case W_PC: root = e->r_pcat; ki = e->ki_pcat; break;
      case W_L: root = e->r_likes; break;
      case W_LA: root = e->r_lauto; ki = e->ki_lauto; break;
      default: root = e->r_lpid; ki = e->ki_lpid; break;
    }
    sqlite3BtreeCursorZero(e->wc[k]);
    if (sqlite3BtreeCursor(e->bt, root, BTREE_WRCSR, ki, e->wc[k]) != SQLITE_OK) return 0;
    e->wc_open |= 1u << k;
  }
  return e->wc[k];
}
static void close_cursors(eng *e) {
  for (int i = 0; i < NRC; i++) if (e->rc_open & (1u << i)) sqlite3BtreeCloseCursor(e->rc[i]);
  for (int i = 0; i < NWC; i++) if (e->wc_open & (1u << i)) sqlite3BtreeCloseCursor(e->wc[i]);
  e->rc_open = e->wc_open = 0;
}

void eng_body_unref(eng_body *b) { if (b && --b->refs == 0) free(b); }
void eng_set_coalesce(eng *e, int on) { e->coalesce = on != 0; }
static void feed_drop(eng *e) { if (e->fshared) { eng_body_unref(e->fshared); e->fshared = 0; } }

int eng_begin_batch(eng *e) {
  if (e->in_batch) return -1;
  feed_drop(e);
  e->in_batch = 1; e->failed = 0; e->st.batches++;
  return 0;
}

int eng_commit_batch(eng *e) {
  if (!e->in_batch) return -1;
  int wrote = e->txn == 2, ok = !e->failed;
  feed_drop(e);
  close_cursors(e);
  if (e->sql_txn) {
    int rc = SQLITE_ERROR;
    if (ok) { rc = sqlite3_step(e->s_commit); sqlite3_reset(e->s_commit); }
    if (rc != SQLITE_DONE) { ok = 0; sqlite3_step(e->s_rollback); sqlite3_reset(e->s_rollback); }
    if (e->db->autoCommit == 0) { ok = 0; sqlite3_exec(e->db, "ROLLBACK", 0, 0, 0); }
  } else if (e->txn) {
    int rc = ok ? sqlite3BtreeCommit(e->bt) : SQLITE_ABORT;
    if (rc != SQLITE_OK) { ok = 0; sqlite3BtreeRollback(e->bt, SQLITE_OK, 0); }
    if (ok && wrote) { int f = sqlite3PagerWalCallback(e->pager); if (f > 0) e->wal_frames = f; }
  }
  // Belt and braces for rule 6: nothing of a committed transaction may still sit in the shim's buffer when we return.
  // A late flush means the shim's commit detection missed a write pattern; if that flush fails, the commit is not on
  // disk, so the batch must not report success.
  if (ok && (wrote || e->wrote_sql) && e->o.wal_coalesce) {
    sqlite3_file *wf = conn_wal_fd(e->db);
    if (wf && wf->pMethods == &cw_io && ((cwfile *)wf)->n) {
      e->st.cw_late_flushes++;
      if (cw_flush((cwfile *)wf) != SQLITE_OK) ok = 0;
    }
  }
  if (wrote || e->wrote_sql) e->st.write_batches++;
  e->wrote_sql = 0;
  if (!ok) e->st.commits_failed++;
  e->txn = 0; e->sql_txn = 0; e->in_batch = 0; e->failed = 0;
  return ok ? 0 : -1;
}

static void ckpt_inline(eng *e) {
  int nlog = 0, nck = 0; double t0 = mono_ms();
  if (sqlite3_wal_checkpoint_v2(e->db, "main", e->o.ckpt_mode, &nlog, &nck) == SQLITE_OK) {
    note_ckpt(e, mono_ms() - t0, nck); e->wal_frames = 0;   // inline: each checkpoint covers the whole WAL (it restarts after)
    if (nck == nlog) wal_drop(e, conn_wal_fd(e->db), 0, (u32)nck);   // all copied: the WAL restarts at the next write
  }
}
void eng_after_batch(eng *e) {
  if (e->in_batch) return;
  if (e->o.ckpt_frames <= 0 && !e->o.direct_writes) return;   // SQLite's own autocheckpoint handles the SQL path
  int lim = e->o.ckpt_frames > 0 ? e->o.ckpt_frames : 1000;
  if (e->o.ckpt_thread || e->ack) {
    u32 mx, pend; wal_state(e, &mx, &pend);
    if (mx > e->st.wal_max) e->st.wal_max = mx;
    // the thread / the event-loop cycle is starved: checkpoint here (never while async writes are in flight)
    if (pend >= (u32)e->o.ckpt_hard && (!e->ack || e->ack->state == ACK_IDLE)) { ckpt_inline(e); return; }
    // WAL at its high mark: let the thread catch up with writes still flowing, and hold writes (drain) only once
    // it is nearly there. The held writes then wait for one short final pass (few frames, small fsyncs) instead of a
    // full checkpoint: on the Xeon a drain that started with ~4k frames pending held writes for 160-200 ms.
    // A writer faster than the thread's passes never lets it come within ckpt_drain_frames, and without a drain the
    // WAL never restarts (test_wal: 407k frames). So at 1.25x the mark, hold writes anyway (a longer hold, once).
    u32 mark = (u32)e->o.ckpt_restart;
    if (!e->drain && mx >= mark && pend > 0 && (pend <= (u32)e->o.ckpt_drain_frames || mx >= mark + mark / 4)) {
      __atomic_store_n(&e->drain, 1, __ATOMIC_RELEASE); e->drain_t0 = mono_ms(); e->st.drains++;
    }
    if (e->drain || pend >= (u32)lim || (mx >= (u32)e->o.ckpt_restart && pend > 0)) {
      if (e->ack) { ack_step(e); if (e->ack->state == ACK_IDLE) ack_start(e); } else ck_signal(e);
    }
    return;
  }
  if ((u64)e->wal_frames > e->st.wal_max) e->st.wal_max = (u64)e->wal_frames;
  if (e->wal_frames >= (e->o.ckpt_defer ? e->o.ckpt_hard : lim)) ckpt_inline(e);
}
int eng_ckpt_due(eng *e) {
  if (e->in_batch || e->o.ckpt_thread || e->ack || (e->o.ckpt_frames <= 0 && !e->o.direct_writes)) return 0;
  int lim = e->o.ckpt_frames > 0 ? e->o.ckpt_frames : 1000;
  return e->wal_frames >= (e->o.ckpt_defer ? e->o.ckpt_hard : lim);
}
int eng_writes_paused(eng *e) {
  if (!e->drain) return 0;
  u32 mx, pend; wal_state(e, &mx, &pend);
  double held = mono_ms() - e->drain_t0;
  if (pend == 0 || held > e->o.ckpt_drain_ms) {          // caught up (the next write restarts the WAL), or give up
    __atomic_store_n(&e->drain, 0, __ATOMIC_RELEASE);
    e->st.drain_ms_total += held; if (held > e->st.drain_ms_max) e->st.drain_ms_max = held;
    if (pend) e->st.drains_timed_out++;
    return 0;
  }
  return 1;
}
void eng_idle(eng *e) {
  if (e->in_batch || e->o.ckpt_thread || !e->o.ckpt_defer) return;
  if (e->wal_frames >= (e->o.ckpt_frames > 0 ? e->o.ckpt_frames : 1000)) ckpt_inline(e);
}

// ---------------------------------------------------------------- error bodies
static int err500(char *out, size_t *n) { static const char m[] = "{\"error\":\"internal server error\"}"; memcpy(out, m, sizeof m - 1); *n = sizeof m - 1; return 500; }
static int err404(char *out, size_t *n) { static const char m[] = "{\"error\":\"post not found\"}"; memcpy(out, m, sizeof m - 1); *n = sizeof m - 1; return 404; }

// ---------------------------------------------------------------- direct reads
// Pointer to a cursor's record; rows that spill to overflow pages are copied into scratch[slot].
static const u8 *fetch(eng *e, BtCursor *c, u32 *amt, int slot) {
  u32 sz = sqlite3BtreePayloadSize(c); const u8 *p = sqlite3BtreePayloadFetch(c, amt);
  if (*amt >= sz) { *amt = sz; return p; }
  if (e->scratch_n[slot] < sz) {
    u8 *q = realloc(e->scratch[slot], sz + 16); if (!q) return 0;
    e->scratch[slot] = q; e->scratch_n[slot] = sz;
  }
  if (sqlite3BtreePayload(c, 0, sz, e->scratch[slot]) != SQLITE_OK) return 0;
  *amt = sz; return e->scratch[slot];
}

// Small index records: never spill. PayloadSize parses the cell, which PayloadFetch needs.
static inline const u8 *idx_fetch(BtCursor *c, u32 *amt) {
  u32 sz = sqlite3BtreePayloadSize(c); const u8 *p = sqlite3BtreePayloadFetch(c, amt);
  return *amt >= sz ? (*amt = sz, p) : 0;
}

// Parses an index cell of likes_post_id_idx (post_id, rowid) in place: returns post_id, or INT64_MIN if odd
static inline i64 lpid_cell(const MemPage *pg, int ix) {
  const u8 *c = pg->aData + (pg->maskPage & get2byteAligned(&pg->aCellIdx[2 * ix]));
  if (!pg->leaf) c += 4;
  int n; u64 sz = rd_varint(c, &n); c += n;
  if (sz > pg->maxLocal) return INT64_MIN;
  u64 hdr = rd_varint(c, &n); const u8 *h = c + n; u64 t = rd_varint(h, &n);
  if (!st_isint(t) || hdr > sz) return INT64_MIN;
  return st_int(t, c + hdr);
}

// Counts likes for post ids in [lo, hi] with one likes_post_id_idx range scan.
// counts != NULL: per-id counts (counts[id-lo]); returns the total, -1 on error.
static i64 count_likes(eng *e, i64 lo, i64 hi, u32 *counts) {
  BtCursor *c = rcur(e, C_L); if (!c) return -1;
  UnpackedRecord *r = e->uk_lpid; int res;
  r->nField = 1; r->default_rc = +1; r->eqSeen = 0; r->errCode = 0;
  r->aMem[0].flags = MEM_Int; r->aMem[0].u.i = lo;
  if (sqlite3BtreeIndexMoveto(c, r, &res) != SQLITE_OK) return -1;
  if (c->eState != CURSOR_VALID) return 0;   // empty index
  int rc = SQLITE_OK;
  if (res < 0) { rc = sqlite3BtreeNext(c, 0); if (rc == SQLITE_DONE) return 0; if (rc) return -1; }
  i64 total = 0;
  if (e->o.walk_cells) {
    for (;;) {
      MemPage *pg = c->pPage; int ix = c->ix;
      if (pg->leaf) {
        for (; ix < pg->nCell; ix++) {
          i64 pid = lpid_cell(pg, ix);
          if (pid == INT64_MIN) return -1;
          if (pid > hi) return total;
          total++; if (counts) counts[pid - lo]++;
        }
        c->ix = (u16)(pg->nCell - 1);        // BtreeNext continues from the last cell of this leaf
      } else {
        i64 pid = lpid_cell(pg, ix);
        if (pid == INT64_MIN) return -1;
        if (pid > hi) return total;
        total++; if (counts) counts[pid - lo]++;
      }
      rc = sqlite3BtreeNext(c, 0);
      if (rc == SQLITE_DONE) return total;
      if (rc) return -1;
    }
  }
  do {
    u32 amt; const u8 *rec = idx_fetch(c, &amt); u64 t[1]; const u8 *d[1];
    if (!rec || rec_decode(rec, amt, 1, t, d) != 1 || !st_isint(t[0])) return -1;
    i64 pid = st_int(t[0], d[0]);
    if (pid > hi) break;
    total++; if (counts) counts[pid - lo]++;
  } while ((rc = sqlite3BtreeNext(c, 0)) == SQLITE_OK);
  return rc == SQLITE_OK || rc == SQLITE_DONE ? total : -1;
}

typedef struct { const u8 *body, *at, *user; u32 bn, an, un; } prow;

// posts cursor positioned on row id: decode body / created_at, look up the author. 1 ok, 0 no such user (JOIN drops it), -1 error
static int load_row(eng *e, BtCursor *cp, prow *o) {
  u32 amt; const u8 *rec = fetch(e, cp, &amt, 0); u64 t[4]; const u8 *d[4];
  if (!rec || rec_decode(rec, amt, 4, t, d) != 4 || !st_isint(t[1])) return -1;
  o->body = d[2]; o->bn = st_len(t[2]); o->at = d[3]; o->an = st_len(t[3]);
  BtCursor *cu = rcur(e, C_U); if (!cu) return -1;
  int res; if (sqlite3BtreeTableMoveto(cu, st_int(t[1], d[1]), 0, &res) != SQLITE_OK) return -1;
  if (res != 0 || cu->eState != CURSOR_VALID) return 0;
  rec = fetch(e, cu, &amt, 1); u64 tu[2]; const u8 *du[2];
  if (!rec || rec_decode(rec, amt, 2, tu, du) != 2) return -1;
  o->user = du[1]; o->un = st_len(tu[1]);
  return 1;
}
static size_t esc_len(const u8 *s, u32 n) {
  size_t l = n;
  for (u32 i = 0; i < n; i++) {
    unsigned c = s[i];
    if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') l += 1; else if (c < 0x20) l += 5;
  }
  return l;
}
// room check for one post object: the cheap worst case (6 bytes per input byte) almost always passes;
// otherwise count the exact escaped size. +32: the SIMD escaper stores whole vectors past the end.
static inline int post_fits(const prow *r, size_t room) {
  if (room >= 120 + 6 * ((size_t)r->bn + r->an + r->un)) return 1;
  return room >= 120 + esc_len(r->body, r->bn) + esc_len(r->at, r->an) + esc_len(r->user, r->un);
}
static inline char *w_post(char *p, i64 id, const prow *r, i64 likes) {
  p = W(p, "{\"id\":"); p = w_i64(p, id);
  p = W(p, ",\"body\":"); p = w_str(p, r->body, r->bn);
  p = W(p, ",\"created_at\":"); p = w_str(p, r->at, r->an);
  p = W(p, ",\"author\":"); p = w_str(p, r->user, r->un);
  p = W(p, ",\"like_count\":"); p = w_i64(p, likes);
  *p++ = '}'; return p;
}

static int direct_get(eng *e, i64 id, char *out, size_t cap, size_t *n) {
  BtCursor *cp = rcur(e, C_P); if (!cp) return err500(out, n);
  int res; if (sqlite3BtreeTableMoveto(cp, id, 0, &res) != SQLITE_OK) return err500(out, n);
  if (res != 0 || cp->eState != CURSOR_VALID) return err404(out, n);
  prow r; int k = load_row(e, cp, &r);
  if (k < 0) return err500(out, n);
  if (k == 0) return err404(out, n);
  if (!post_fits(&r, cap - 16)) return err500(out, n);
  // the like count walk moves only the likes cursor: r's pointers into the posts/users pages stay valid
  i64 likes = count_likes(e, id, id, 0); if (likes < 0) return err500(out, n);
  char *p = W(out, "{\"post\":"); p = w_post(p, id, &r, likes); *p++ = '}';
  *n = (size_t)(p - out); return 200;
}

// ---------------------------------------------------------------- batched author seeks (explore/sqlite-simd msearch.h)
// ms_table_seek: up to MS_MAX rowids descend one table B-tree level by level; at each level every key's child page is
// found first, then the pcache1 hash buckets and page headers of all children are prefetched, then all are fetched.
// The cache misses of different keys overlap instead of queueing. Same leaf and cell as sqlite3BtreeTableMoveto.
// On the Xeon (exp13) the feed's 20 author lookups this way cut the feed by 10% warm and 15-20% with a polluted cache.
#define MS_MAX 64
typedef struct { MemPage *leaf; int ix; int found; } ms_res;
#define MS_PF(p) __builtin_prefetch((const void *)(p), 0, 3)
static inline i64 ms_cell_key(MemPage *pg, int i) {
  u8 *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf) while (0x80 <= *(c++)) {}   // leaf cells start with the payload size
  return (i64)sqlite3VarintValue(c);
}
// first cell with key >= k (nCell if none); same child choice as the stock search. Interpolation first: table pages of
// dense rowids (users 1..50,000, posts) put k almost exactly where a linear guess says, so the usual 6-10 binary-search
// probes become 1-3. A short walk from the guess, then a binary search over what is left if the keys are not even.
static inline int ms_lower_bound(MemPage *pg, i64 k) {
  int n = pg->nCell, lo = 0, len = n;
  if (n >= 8) {
    i64 k0 = ms_cell_key(pg, 0), kn = ms_cell_key(pg, n - 1);
    if (k <= k0) return 0;
    if (k > kn) return n;
    int g = (int)((double)(k - k0) * (n - 1) / (double)(kn - k0));
    if (g < 1) g = 1; if (g > n - 1) g = n - 1;
    // invariant from here: key(0) < k <= key(n-1); find the first index with key >= k
    if (ms_cell_key(pg, g) >= k) {
      for (int s = 0; s < 4; s++) { if (ms_cell_key(pg, g - 1) < k) return g; g--; }
      lo = 1; len = g - 1 + 1;                     // answer in [1, g]
    } else {
      for (int s = 0; s < 4; s++) { g++; if (ms_cell_key(pg, g) >= k) return g; }
      lo = g + 1; len = n - 1 - g;                 // answer in [g+1, n-1]
    }
  }
  while (len > 0) {
    int h = len >> 1;
    if (ms_cell_key(pg, lo + h) < k) { lo += h + 1; len -= h + 1; } else len = h;
  }
  return lo;
}
static inline PgHdr1 **ms_bucket(BtShared *bt, Pgno pg) {
  PCache1 *pc = (PCache1 *)bt->pPager->pPCache->pCache;
  return &pc->apHash[pg % pc->nHash];
}
// Keys are processed in sorted order, so keys under the same child page are adjacent: such a page is fetched once
// and shared with an extra pager reference (the root always, interior pages mostly, leaves sometimes). For the feed's
// 20 authors that is ~25 page fetches instead of ~60.
static int ms_table_seek(Btree *p, Pgno root, const i64 *keys_in, int n, ms_res *r) {
  BtShared *bt = p->pBt; MemPage *cur[MS_MAX]; Pgno ch[MS_MAX]; int rc, ord[MS_MAX]; i64 keys[MS_MAX];
  if (n > MS_MAX || n < 1) return SQLITE_MISUSE;
  for (int i = 0; i < n; i++) {                       // insertion sort of the key order (n <= 64)
    int j = i; while (j > 0 && keys_in[ord[j - 1]] > keys_in[i]) { ord[j] = ord[j - 1]; j--; }
    ord[j] = i;
  }
  for (int i = 0; i < n; i++) keys[i] = keys_in[ord[i]];
  if ((rc = getAndInitPage(bt, root, &cur[0], PAGER_GET_READONLY))) return rc;
  if (cur[0]->nCell == 0) { releasePage(cur[0]); for (int i = 0; i < n; i++) r[i].leaf = 0, r[i].found = 0, r[i].ix = 0; return SQLITE_OK; }
  for (int i = 1; i < n; i++) { sqlite3PagerRef(cur[0]->pDbPage); cur[i] = cur[0]; }
  for (int depth = 0; !cur[0]->leaf; depth++) {          // every key sits at the same depth (B-trees are balanced)
    if (depth > 20) { rc = SQLITE_CORRUPT_BKPT; goto fail; }
    for (int i = 0; i < n; i++) {
      MemPage *pg = cur[i]; int lb = ms_lower_bound(pg, keys[i]);
      ch[i] = lb >= pg->nCell ? get4byte(&pg->aData[pg->hdrOffset + 8]) : get4byte(findCell(pg, lb));
      if (!i || ch[i] != ch[i - 1]) MS_PF(ms_bucket(bt, ch[i]));
    }
    for (int i = 0; i < n; i++) {
      if (i && ch[i] == ch[i - 1]) continue;
      PgHdr1 *h = *ms_bucket(bt, ch[i]);
      while (h && h->iKey != ch[i]) h = h->pNext;
      if (h) { u8 *d = h->page.pBuf; MS_PF(d); MS_PF(d + 64); MS_PF(h->page.pExtra); MS_PF((u8 *)h->page.pExtra + 64); }
    }
    for (int i = 0; i < n; i++) releasePage(cur[i]);
    for (int i = 0; i < n; i++) cur[i] = 0;
    for (int i = 0; i < n; i++) {
      if (i && ch[i] == ch[i - 1]) { sqlite3PagerRef(cur[i - 1]->pDbPage); cur[i] = cur[i - 1]; continue; }
      if ((rc = getAndInitPage(bt, ch[i], &cur[i], PAGER_GET_READONLY))) goto fail;
      if (cur[i]->nCell < 1 || cur[i]->intKey != 1 || cur[i]->leaf != cur[0]->leaf) { rc = SQLITE_CORRUPT_PGNO(ch[i]); goto fail; }
      MS_PF(findCellPastPtr(cur[i], cur[i]->nCell >> 1));
    }
  }
  for (int i = 0; i < n; i++) {
    MemPage *pg = cur[i]; int lb = ms_lower_bound(pg, keys[i]), found = 0;
    if (lb < pg->nCell) { u8 *c = findCellPastPtr(pg, lb); while (0x80 <= *(c++)) {} found = (i64)sqlite3VarintValue(c) == keys[i]; }
    ms_res *o = &r[ord[i]]; o->leaf = pg; o->ix = lb; o->found = found;
  }
  return SQLITE_OK;
fail:
  for (int i = 0; i < n; i++) if (cur[i]) releasePage(cur[i]);
  return rc;
}
static void ms_release(ms_res *r, int n) { for (int i = 0; i < n; i++) if (r[i].leaf) releasePage(r[i].leaf); }

// The feed with the 20 author lookups as one ms_table_seek (explore/sqlite-simd feedms.h). Posts leaves get an extra
// pager ref so the record pointers stay valid while other pages are fetched. Anything unusual (fewer than 20 rows, a
// post or author missing, a record on overflow pages) returns -1 and the caller runs the one-by-one path, so the
// output is always exactly direct_feed's.
static int direct_feed_one(eng *e, char *out, size_t cap, size_t *n);
static int feed_batch(eng *e, char *out, size_t cap, size_t *n) {
  BtCursor *cc = rcur(e, C_C), *cp = rcur(e, C_P); if (!cc || !cp) return -1;
  i64 ids[FEED_N], uid[FEED_N]; prow r[FEED_N]; int nid = 0, res, rc = SQLITE_OK, npin = 0, st = -1, have_ur = 0;
  DbPage *pins[FEED_N]; ms_res ur[FEED_N];
  if (sqlite3BtreeFirst(cc, &res) != SQLITE_OK || res) return -1;
  do {
    u32 amt; const u8 *rec = idx_fetch(cc, &amt); u64 t[3]; const u8 *d[3];
    int nf = rec ? rec_decode(rec, amt, 3, t, d) : -1;
    if (nf < 1 || !st_isint(t[nf - 1])) return -1;
    ids[nid++] = st_int(t[nf - 1], d[nf - 1]);
  } while (nid < FEED_N && (rc = sqlite3BtreeNext(cc, 0)) == SQLITE_OK);
  if (nid < FEED_N) return -1;
  i64 lo = INT64_MAX, hi = INT64_MIN;
  for (int i = 0; i < nid; i++) { if (ids[i] < lo) lo = ids[i]; if (ids[i] > hi) hi = ids[i]; }
  int ranged = e->o.feed_range && (u64)(hi - lo) < RANGE_MAX;
  if (ranged) { memset(e->counts, 0, sizeof(u32) * (size_t)(hi - lo + 1)); if (count_likes(e, lo, hi, e->counts) < 0) return -1; }
  i64 prev = INT64_MIN;
  for (int i = 0; i < nid; i++) {
    i64 id = ids[i]; int found = 0;
    if (prev == id + 1 && cp->eState == CURSOR_VALID) {
      rc = sqlite3BtreePrevious(cp, 0);
      if (rc == SQLITE_OK && sqlite3BtreeIntegerKey(cp) == id) found = 1;
      else if (rc != SQLITE_OK && rc != SQLITE_DONE) goto done;
    }
    if (!found) {
      if (sqlite3BtreeTableMoveto(cp, id, 0, &res) != SQLITE_OK) goto done;
      found = res == 0 && cp->eState == CURSOR_VALID;
    }
    prev = id;
    if (!found) goto done;
    u32 sz = sqlite3BtreePayloadSize(cp), amt; const u8 *rec = sqlite3BtreePayloadFetch(cp, &amt); u64 t[4]; const u8 *d[4];
    if (amt < sz || rec_decode(rec, sz, 4, t, d) != 4 || !st_isint(t[1])) goto done;
    r[i].body = d[2]; r[i].bn = st_len(t[2]); r[i].at = d[3]; r[i].an = st_len(t[3]); uid[i] = st_int(t[1], d[1]);
    DbPage *pg = cp->pPage->pDbPage;
    if (!npin || pins[npin - 1] != pg) { sqlite3PagerRef(pg); pins[npin++] = pg; }
  }
  if (ms_table_seek(e->bt, e->r_users, uid, nid, ur) != SQLITE_OK) goto done;
  have_ur = 1;
  for (int i = 0; i < nid; i++) {
    if (!ur[i].found) goto done;
    MemPage *pg = ur[i].leaf; CellInfo info; pg->xParseCell(pg, findCell(pg, ur[i].ix), &info);
    u64 t[2]; const u8 *d[2];
    if (info.nLocal != info.nPayload || rec_decode(info.pPayload, info.nPayload, 2, t, d) != 2) goto done;
    r[i].user = d[1]; r[i].un = st_len(t[1]);
  }
  {
    char *p = W(out, "{\"posts\":["), *end = out + cap;
    for (int i = 0; i < nid; i++) {
      i64 likes = ranged ? (i64)e->counts[ids[i] - lo] : count_likes(e, ids[i], ids[i], 0);
      if (likes < 0 || !post_fits(&r[i], (size_t)(end - p) - 8)) goto done;
      if (i) *p++ = ',';
      p = w_post(p, ids[i], &r[i], likes);
    }
    *p++ = ']'; *p++ = '}';
    *n = (size_t)(p - out); st = 200;
  }
done:
  if (have_ur) ms_release(ur, nid);
  for (int i = 0; i < npin; i++) sqlite3PagerUnref(pins[i]);
  return st;
}
static int direct_feed(eng *e, char *out, size_t cap, size_t *n) {
  // feed_batch pins pages with sqlite3PagerRef and peeks at pcache1's hash; mmap'd pages have neither (sqlite3PagerRef
  // on one dereferences a NULL pCache), so with mmap the feed takes the one-by-one path, which is mmap-safe.
  if (e->o.feed_batch && e->o.mmap_size <= 0) { int st = feed_batch(e, out, cap, n); if (st >= 0) return st; e->st.feed_batch_fallbacks++; }
  return direct_feed_one(e, out, cap, n);
}

static int direct_feed_one(eng *e, char *out, size_t cap, size_t *n) {
  BtCursor *cc = rcur(e, C_C), *cp = rcur(e, C_P); if (!cc || !cp) return err500(out, n);
  i64 ids[FEED_N]; int nid = 0, res, rc;
  if (sqlite3BtreeFirst(cc, &res) != SQLITE_OK) return err500(out, n);
  if (!res) {
    do {
      u32 amt; const u8 *rec = idx_fetch(cc, &amt); u64 t[3]; const u8 *d[3];
      int nf = rec ? rec_decode(rec, amt, 3, t, d) : -1;
      if (nf < 1 || !st_isint(t[nf - 1])) return err500(out, n);
      ids[nid++] = st_int(t[nf - 1], d[nf - 1]);
    } while (nid < FEED_N && (rc = sqlite3BtreeNext(cc, 0)) == SQLITE_OK);
  }
  i64 lo = INT64_MAX, hi = INT64_MIN;
  for (int i = 0; i < nid; i++) { if (ids[i] < lo) lo = ids[i]; if (ids[i] > hi) hi = ids[i]; }
  int ranged = e->o.feed_range && nid > 0 && (u64)(hi - lo) < RANGE_MAX;
  if (ranged) {
    memset(e->counts, 0, sizeof(u32) * (size_t)(hi - lo + 1));
    if (count_likes(e, lo, hi, e->counts) < 0) return err500(out, n);
  }
  char *p = W(out, "{\"posts\":["), *end = out + cap; int emitted = 0, i = 0, more = nid == FEED_N; i64 prev = INT64_MIN;
  while (emitted < FEED_N) {
    i64 id;
    if (i < nid) id = ids[i++];
    else {   // a post was dropped by the JOIN (author missing): keep walking the index
      if (!more) break;
      rc = sqlite3BtreeNext(cc, 0);
      if (rc == SQLITE_DONE) break;
      if (rc) return err500(out, n);
      u32 amt; const u8 *rec = idx_fetch(cc, &amt); u64 t[3]; const u8 *d[3];
      int nf = rec ? rec_decode(rec, amt, 3, t, d) : -1;
      if (nf < 1 || !st_isint(t[nf - 1])) return err500(out, n);
      id = st_int(t[nf - 1], d[nf - 1]);
    }
    // ids usually descend by one: step the posts cursor back instead of seeking from the root
    int found = 0;
    if (prev == id + 1 && cp->eState == CURSOR_VALID) {
      rc = sqlite3BtreePrevious(cp, 0);
      if (rc == SQLITE_OK && sqlite3BtreeIntegerKey(cp) == id) found = 1;
      else if (rc != SQLITE_OK && rc != SQLITE_DONE) return err500(out, n);
    }
    if (!found) {
      if (sqlite3BtreeTableMoveto(cp, id, 0, &res) != SQLITE_OK) return err500(out, n);
      found = res == 0 && cp->eState == CURSOR_VALID;
    }
    prev = id;
    if (!found) continue;                         // index entry without a row: SQL would not return it either
    prow r; int k = load_row(e, cp, &r);
    if (k < 0) return err500(out, n);
    if (k == 0) continue;
    i64 likes = ranged && id >= lo && id <= hi ? (i64)e->counts[id - lo] : count_likes(e, id, id, 0);
    if (likes < 0) return err500(out, n);
    if (!post_fits(&r, (size_t)(end - p) - 8)) return err500(out, n);
    if (emitted) *p++ = ',';
    p = w_post(p, id, &r, likes); emitted++;
  }
  *p++ = ']'; *p++ = '}';
  *n = (size_t)(p - out); return 200;
}

// ---------------------------------------------------------------- SQL reads (baseline)
static inline char *w_post_stmt(char *p, sqlite3_stmt *s, int likes_col, i64 likes) {
  prow r;
  r.body = sqlite3_column_text(s, 1); r.bn = (u32)sqlite3_column_bytes(s, 1);
  r.at = sqlite3_column_text(s, 2); r.an = (u32)sqlite3_column_bytes(s, 2);
  r.user = sqlite3_column_text(s, 3); r.un = (u32)sqlite3_column_bytes(s, 3);
  return w_post(p, sqlite3_column_int64(s, 0), &r, likes_col >= 0 ? sqlite3_column_int64(s, likes_col) : likes);
}
static inline int stmt_fits(sqlite3_stmt *s, size_t room) {
  prow r;
  r.body = sqlite3_column_text(s, 1); r.bn = (u32)sqlite3_column_bytes(s, 1);
  r.at = sqlite3_column_text(s, 2); r.an = (u32)sqlite3_column_bytes(s, 2);
  r.user = sqlite3_column_text(s, 3); r.un = (u32)sqlite3_column_bytes(s, 3);
  return post_fits(&r, room);
}
static int sql_get(eng *e, i64 id, char *out, size_t cap, size_t *n) {
  if (sql_txn(e)) return err500(out, n);
#if ENG_TURBO
  int rows;
  if (e->o.turbo_reads && turbo_body(&e->st, e->s_post, &id, out, cap, n, &rows)) return rows ? 200 : err404(out, n);
#endif
  sqlite3_stmt *s = e->s_post; sqlite3_bind_int64(s, 1, id);
  int rc = sqlite3_step(s), st;
  if (rc == SQLITE_ROW) {
    if (!stmt_fits(s, cap - 16)) st = err500(out, n);
    else { char *p = W(out, "{\"post\":"); p = w_post_stmt(p, s, 4, 0); *p++ = '}'; *n = (size_t)(p - out); st = 200; }
  } else st = rc == SQLITE_DONE ? err404(out, n) : err500(out, n);
  sqlite3_reset(s);
  return st;
}
static int sql_feed_ref(eng *e, char *out, size_t cap, size_t *n) {
#if ENG_TURBO
  int rows;
  if (e->o.turbo_reads && turbo_body(&e->st, e->s_feed, 0, out, cap, n, &rows)) return 200;
#endif
  sqlite3_stmt *s = e->s_feed; char *p = W(out, "{\"posts\":["), *end = out + cap; int k = 0, rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    if (!stmt_fits(s, (size_t)(end - p) - 8)) { rc = SQLITE_NOMEM; break; }
    if (k++) *p++ = ',';
    p = w_post_stmt(p, s, 4, 0);
  }
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) return err500(out, n);
  *p++ = ']'; *p++ = '}'; *n = (size_t)(p - out); return 200;
}
// 3-statement range feed (rust-epoll / go-epoll style); falls back to the reference query
static int sql_feed(eng *e, char *out, size_t cap, size_t *n) {
  if (sql_txn(e)) return err500(out, n);
  if (!e->o.feed_range) return sql_feed_ref(e, out, cap, n);
  i64 ids[FEED_N]; int nid = 0, rc;
  while (nid < FEED_N && (rc = sqlite3_step(e->s_ids)) == SQLITE_ROW) ids[nid++] = sqlite3_column_int64(e->s_ids, 0);
  sqlite3_reset(e->s_ids);
  i64 lo = INT64_MAX, hi = INT64_MIN;
  for (int i = 0; i < nid; i++) { if (ids[i] < lo) lo = ids[i]; if (ids[i] > hi) hi = ids[i]; }
  if (nid < FEED_N || (u64)(hi - lo) >= 64) return sql_feed_ref(e, out, cap, n);
  memset(e->counts, 0, sizeof(u32) * (size_t)(hi - lo + 1));
  sqlite3_bind_int64(e->s_rlikes, 1, lo); sqlite3_bind_int64(e->s_rlikes, 2, hi);
  while ((rc = sqlite3_step(e->s_rlikes)) == SQLITE_ROW) e->counts[sqlite3_column_int64(e->s_rlikes, 0) - lo]++;
  sqlite3_reset(e->s_rlikes);
  if (rc != SQLITE_DONE) return err500(out, n);
  // rows come out in id order; format each into its slot of a scratch area, then concatenate in feed order
  static __thread char slot[64][3200]; size_t sl[64] = {0};
  sqlite3_bind_int64(e->s_rposts, 1, lo); sqlite3_bind_int64(e->s_rposts, 2, hi);
  while ((rc = sqlite3_step(e->s_rposts)) == SQLITE_ROW) {
    i64 id = sqlite3_column_int64(e->s_rposts, 0);
    if (!stmt_fits(e->s_rposts, sizeof slot[0])) { rc = SQLITE_NOMEM; break; }
    sl[id - lo] = (size_t)(w_post_stmt(slot[id - lo], e->s_rposts, -1, e->counts[id - lo]) - slot[id - lo]);
  }
  sqlite3_reset(e->s_rposts);
  if (rc != SQLITE_DONE) return err500(out, n);
  for (int i = 0; i < nid; i++) if (!sl[ids[i] - lo]) return sql_feed_ref(e, out, cap, n);   // author missing
  char *p = W(out, "{\"posts\":["), *end = out + cap;
  for (int i = 0; i < nid; i++) {
    size_t l = sl[ids[i] - lo];
    if ((size_t)(end - p) < l + 8) return err500(out, n);
    if (i) *p++ = ',';
    memcpy(p, slot[ids[i] - lo], l); p += l;
  }
  *p++ = ']'; *p++ = '}'; *n = (size_t)(p - out); return 200;
}

// ---------------------------------------------------------------- public read ops
static int feed_compute(eng *e, char *out, size_t cap, size_t *n) {
  e->st.feeds_computed++;
  return e->o.direct_reads ? direct_feed(e, out, cap, n) : sql_feed(e, out, cap, n);
}
static eng_body *body_new(const char *p, size_t n) {
  eng_body *b = malloc(sizeof *b + n); if (!b) return 0;
  b->refs = 1; b->len = (uint32_t)n; memcpy(b->data, p, n); return b;
}
int eng_feed(eng *e, char *out, size_t cap, size_t *n) {
  if (!e->in_batch || cap < 64) return err500(out, n);
  if (e->coalesce && e->fshared) {
    if (e->fshared->len > cap) return err500(out, n);
    memcpy(out, e->fshared->data, e->fshared->len); *n = e->fshared->len; e->st.feeds_shared++; return 200;
  }
  int st = feed_compute(e, out, cap, n);
  if (e->coalesce && st == 200) e->fshared = body_new(out, *n);   // errors are never shared
  return st;
}
int eng_feed_body(eng *e, eng_body **body) {
  if (e->coalesce && e->fshared && e->in_batch) { e->fshared->refs++; *body = e->fshared; e->st.feeds_shared++; return 200; }
  if (!e->fbuf && !(e->fbuf = malloc(ENG_FEED_CAP))) { *body = 0; return 500; }
  size_t n; int st = e->in_batch ? feed_compute(e, e->fbuf, ENG_FEED_CAP, &n) : err500(e->fbuf, &n);
  if (!(*body = body_new(e->fbuf, n))) return 500;
  if (e->coalesce && st == 200) { (*body)->refs++; e->fshared = *body; }
  return st;
}
int eng_get_post(eng *e, int64_t id, char *out, size_t cap, size_t *n) {
  if (!e->in_batch || cap < 64) return err500(out, n);
  return e->o.direct_reads ? direct_get(e, id, out, cap, n) : sql_get(e, id, out, cap, n);
}
int eng_health(eng *e, int64_t up, char *out, size_t cap, size_t *n) {
  if (e->in_batch && e->txn && sql_txn(e)) goto bad;   // see sql_txn: never step a statement in autocommit mid-batch
  int rc = sqlite3_step(e->s_health), v = rc == SQLITE_ROW ? sqlite3_column_int(e->s_health, 0) : 0;
  sqlite3_reset(e->s_health);
  if (rc == SQLITE_ROW && v == 1 && cap >= 64) {
    char *p = W(out, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); p = w_i64(p, up); *p++ = '}';
    *n = (size_t)(p - out); return 200;
  }
bad:;
  const char *m = sqlite3_errmsg(e->db); size_t ml = strlen(m); if (ml > 200) ml = 200;
  if (cap < 80 + 6 * ml) return err500(out, n);
  char *p = W(out, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"); p = w_str(p, m, ml); *p++ = '}';
  *n = (size_t)(p - out); return 503;
}

// ---------------------------------------------------------------- writes
// SQLite's length() of a TEXT value: characters before the first NUL (lengthFunc, func.c)
static int sql_length(const u8 *z, size_t n) {
  const u8 *ze = z + n, *z0 = z; int c;
  while (z < ze && (c = *z) != 0) {
    z++;
    if (c >= 0xc0) while (z < ze && (*z & 0xc0) == 0x80) { z++; z0++; }
  }
  return (int)(z - z0);
}

static int like_reply(char *out, size_t *n, int already, i64 pid) {
  char *p = W(out, "{\"liked\":true,\"already_liked\":");
  p = already ? W(p, "true") : W(p, "false");
  p = W(p, ",\"post_id\":"); p = w_i64(p, pid); *p++ = '}';
  *n = (size_t)(p - out); return already ? 200 : 201;
}
static int create_reply(char *out, size_t cap, size_t *n, i64 id, const char *body, size_t bn, const char *ts, const char *un, size_t unl) {
  if (cap < 160 + 6 * (bn + unl)) return err500(out, n);
  char *p = W(out, "{\"post\":{\"id\":"); p = w_i64(p, id);
  p = W(p, ",\"body\":"); p = w_str(p, body, bn);
  p = W(p, ",\"created_at\":\""); p = w_lit(p, ts, 24); *p++ = '"';
  p = W(p, ",\"author\":"); p = w_str(p, un, unl);
  p = W(p, ",\"like_count\":0}}");
  *n = (size_t)(p - out); return 201;
}

static int insert_index(eng *e, int k, u32 len) {   // record in e->rec
  BtreePayload x; memset(&x, 0, sizeof x); x.pKey = e->rec; x.nKey = len;
  BtCursor *c = wcur(e, k); if (!c) return SQLITE_ERROR;
  return sqlite3BtreeInsert(c, &x, 0, 0);
}
static int new_rowid(BtCursor *c, i64 *id) {        // == OP_NewRowid (non-random branch)
  int res, rc = sqlite3BtreeLast(c, &res); if (rc) return rc;
  if (res) { *id = 1; return SQLITE_OK; }
  i64 v = sqlite3BtreeIntegerKey(c);
  if (v >= LARGEST_INT64) return SQLITE_FULL;       // SQLite would pick a random rowid: not worth supporting
  *id = v + 1; return SQLITE_OK;
}

// Writes fail in two ways: a constraint (only this op gets a 500, the batch goes on) or an I/O / corrupt
// error inside the B-tree (the transaction is unusable: the whole batch is rolled back).
static int direct_create(eng *e, i64 uid, const char *body, size_t bn, const char *ts, i64 *idp) {
  if (bn > 16384) return SQLITE_TOOBIG;            // bodies over 16 KB are out of scope (SPEC.md)
  int len = sql_length((const u8 *)body, bn);
  if (len < 1 || len > 500) return SQLITE_CONSTRAINT;
  BtCursor *cp = wcur(e, W_P); if (!cp) return SQLITE_ERROR;
  i64 id; int rc = new_rowid(cp, &id); if (rc) return rc;
  // same order as the VDBE program: posts_user_id_idx, posts_created_at_id_idx, then the table row
  i64 iv[4] = {0}; const void *tv[4] = {0}; u32 tn[4] = {0};
  iv[0] = uid; iv[1] = id;
  rc = insert_index(e, W_PU, make_record(e->rec, 2, "ii", iv, tv, tn, e->fmt)); if (rc) return rc;
  tv[0] = ts; tn[0] = 24; iv[1] = id; iv[2] = id;
  rc = insert_index(e, W_PC, make_record(e->rec, 3, "tii", iv, tv, tn, e->fmt)); if (rc) return rc;
  iv[1] = uid; tv[2] = body; tn[2] = (u32)bn; tv[3] = ts; tn[3] = 24;
  u32 l = make_record(e->rec, 4, "nitt", iv, tv, tn, e->fmt);
  BtreePayload x; memset(&x, 0, sizeof x); x.nKey = id; x.pData = e->rec; x.nData = (int)l;
  rc = sqlite3BtreeInsert(cp, &x, BTREE_APPEND, 0); if (rc) return rc;
  *idp = id; return SQLITE_OK;
}
// 0 inserted, 1 already liked, 2 post missing; <0: -rc
static int direct_like(eng *e, i64 uid, i64 pid, const char *ts) {
  BtCursor *cp = rcur(e, C_P); if (!cp) return -SQLITE_ERROR;
  int res, rc;
  if ((rc = sqlite3BtreeTableMoveto(cp, pid, 0, &res))) return -rc;
  if (res != 0 || cp->eState != CURSOR_VALID) return 2;
  // ON CONFLICT (user_id, post_id) DO NOTHING: probe the autoindex like OP_NoConflict does
  UnpackedRecord *r = e->uk_lauto;
  r->nField = 2; r->default_rc = 0; r->eqSeen = 0; r->errCode = 0;
  r->aMem[0].flags = MEM_Int; r->aMem[0].u.i = uid; r->aMem[1].flags = MEM_Int; r->aMem[1].u.i = pid;
  int writing = e->txn == 2;
  BtCursor *ca = writing ? wcur(e, W_LA) : rcur(e, C_A); if (!ca) return -SQLITE_ERROR;
  if ((rc = sqlite3BtreeIndexMoveto(ca, r, &res))) return -rc;
  if (res == 0 && ca->eState == CURSOR_VALID) return 1;
  int seek = writing && ca->eState == CURSOR_VALID ? res : 0;   // reuse the probe position for the insert
  BtCursor *cl = wcur(e, W_L); if (!cl) return -SQLITE_ERROR;
  i64 rowid; if ((rc = new_rowid(cl, &rowid))) return -rc;
  i64 iv[3] = {0}; const void *tv[3] = {0}; u32 tn[3] = {0};
  // VDBE order: likes_post_id_idx, sqlite_autoindex_likes_1, then the table row
  iv[0] = pid; iv[1] = rowid;
  if ((rc = insert_index(e, W_LP, make_record(e->rec, 2, "ii", iv, tv, tn, e->fmt)))) return -rc;
  iv[0] = uid; iv[1] = pid; iv[2] = rowid;
  u32 l = make_record(e->rec, 3, "iii", iv, tv, tn, e->fmt);
  { BtreePayload x; memset(&x, 0, sizeof x); x.pKey = e->rec; x.nKey = l;
    BtCursor *c = wcur(e, W_LA); if (!c) return -SQLITE_ERROR;
    if ((rc = sqlite3BtreeInsert(c, &x, 0, seek))) return -rc; }
  iv[0] = uid; iv[1] = pid; tv[2] = ts; tn[2] = 24;
  l = make_record(e->rec, 3, "iit", iv, tv, tn, e->fmt);
  BtreePayload x; memset(&x, 0, sizeof x); x.nKey = rowid; x.pData = e->rec; x.nData = (int)l;
  if ((rc = sqlite3BtreeInsert(cl, &x, BTREE_APPEND, 0))) return -rc;
  return 0;
}

static int sql_create(eng *e, i64 uid, const char *body, size_t bn, const char *ts, i64 *idp) {
  if (sql_txn(e)) return SQLITE_ERROR;
  sqlite3_stmt *s = e->s_ins;
  sqlite3_bind_int64(s, 1, uid); sqlite3_bind_text(s, 2, body, (int)bn, SQLITE_STATIC); sqlite3_bind_text(s, 3, ts, 24, SQLITE_STATIC);
  int rc = sqlite3_step(s); sqlite3_reset(s); sqlite3_clear_bindings(s); e->wrote_sql = 1;
  if (rc != SQLITE_DONE) return rc;
  *idp = sqlite3_last_insert_rowid(e->db); return SQLITE_OK;
}
static int sql_like(eng *e, i64 uid, i64 pid, const char *ts) {
  if (sql_txn(e)) return -SQLITE_ERROR;
  sqlite3_stmt *s = e->s_exists; sqlite3_bind_int64(s, 1, pid);
  int rc = sqlite3_step(s); sqlite3_reset(s);
  if (rc == SQLITE_DONE) return 2;
  if (rc != SQLITE_ROW) return -SQLITE_ERROR;
  s = e->s_like;
  sqlite3_bind_int64(s, 1, uid); sqlite3_bind_int64(s, 2, pid); sqlite3_bind_text(s, 3, ts, 24, SQLITE_STATIC);
  rc = sqlite3_step(s); sqlite3_reset(s); e->wrote_sql = 1;
  if (rc != SQLITE_DONE) return -rc;
  return sqlite3_changes(e->db) ? 0 : 1;
}
// a non-constraint error leaves the write transaction in an unknown state: fail the batch
static void write_failed(eng *e, int rc) {
  if ((rc & 0xff) != SQLITE_CONSTRAINT && rc != SQLITE_TOOBIG) e->failed = 1; }

int eng_like(eng *e, int64_t uid, int64_t pid, char *out, size_t cap, size_t *n) {
  feed_drop(e);
  if (!e->in_batch || cap < 96 || e->failed) return err500(out, n);
  if (writer_upgrade(e)) { e->failed = 1; return err500(out, n); }
  const char *ts = now_ts(e);
  int r = e->o.direct_writes && !e->sql_txn ? direct_like(e, uid, pid, ts) : sql_like(e, uid, pid, ts);
  if (r < 0) { write_failed(e, -r); return err500(out, n); }
  if (r == 2) return err404(out, n);
  return like_reply(out, n, r == 1, pid);
}
int eng_create(eng *e, int64_t uid, const char *un, size_t unl, const char *body, size_t bn, char *out, size_t cap, size_t *n,
               int64_t *idp, char *created_at) {
  feed_drop(e);
  if (!e->in_batch || e->failed) return err500(out, n);
  if (writer_upgrade(e)) { e->failed = 1; return err500(out, n); }
  const char *ts = now_ts(e); i64 id = 0;
  int rc = e->o.direct_writes && !e->sql_txn ? direct_create(e, uid, body, bn, ts, &id) : sql_create(e, uid, body, bn, ts, &id);
  if (rc) { write_failed(e, rc); return err500(out, n); }
  if (idp) *idp = id;
  if (created_at) memcpy(created_at, ts, 24);
  return create_reply(out, cap, n, id, body, bn, ts, un, unl);
}
