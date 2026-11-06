#define _GNU_SOURCE
#include "sqlite3.c"
#include "sqlite_engine.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#define FEED_N 20
#define RANGE_MAX 1024
#define CKPT_WAKE_FRAMES 2048
#define CKPT_RESTART_FRAMES 8192
#define CKPT_DRAIN_FRAMES 512
#define CKPT_HARD_FRAMES 32768
#define CKPT_DRAIN_MAX_MS 250.0

static const char HEX_LUT[] = "0123456789abcdef";
static const char DEC2_LUT[200] = {
  '0','0','0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8','0','9',
  '1','0','1','1','1','2','1','3','1','4','1','5','1','6','1','7','1','8','1','9',
  '2','0','2','1','2','2','2','3','2','4','2','5','2','6','2','7','2','8','2','9',
  '3','0','3','1','3','2','3','3','3','4','3','5','3','6','3','7','3','8','3','9',
  '4','0','4','1','4','2','4','3','4','4','4','5','4','6','4','7','4','8','4','9',
  '5','0','5','1','5','2','5','3','5','4','5','5','5','6','5','7','5','8','5','9',
  '6','0','6','1','6','2','6','3','6','4','6','5','6','6','6','7','6','8','6','9',
  '7','0','7','1','7','2','7','3','7','4','7','5','7','6','7','7','7','8','7','9',
  '8','0','8','1','8','2','8','3','8','4','8','5','8','6','8','7','8','8','8','9',
  '9','0','9','1','9','2','9','3','9','4','9','5','9','6','9','7','9','8','9','9'
};

static inline char *esc_char(char *o, unsigned char c) {
  switch (c) {
    case '"':  o[0] = '\\'; o[1] = '"';  return o + 2;
    case '\\': o[0] = '\\'; o[1] = '\\'; return o + 2;
    case '\b': o[0] = '\\'; o[1] = 'b';  return o + 2;
    case '\f': o[0] = '\\'; o[1] = 'f';  return o + 2;
    case '\n': o[0] = '\\'; o[1] = 'n';  return o + 2;
    case '\r': o[0] = '\\'; o[1] = 'r';  return o + 2;
    case '\t': o[0] = '\\'; o[1] = 't';  return o + 2;
    default:
      memcpy(o, "\\u00", 4);
      o[4] = HEX_LUT[c >> 4];
      o[5] = HEX_LUT[c & 15];
      return o + 6;
  }
}

static inline int needs_esc(unsigned char c) {
  return c < 0x20 || c == '"' || c == '\\';
}

static size_t json_escape_buf(char *out, const char *s, size_t n) {
  char *o = out;
  size_t i = 0;
#if defined(__AVX2__)
  while (i + 32 <= n) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
    _mm256_storeu_si256((__m256i *)o, v);
    __m256i q = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"'));
    __m256i b = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'));
    __m256i c = _mm256_cmpeq_epi8(_mm256_min_epu8(v, _mm256_set1_epi8(0x1f)), v);
    unsigned mask = (unsigned)_mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(q, b), c));
    if (!mask) {
      i += 32;
      o += 32;
      continue;
    }
    unsigned k = (unsigned)__builtin_ctz(mask);
    o += k;
    i += k;
    o = esc_char(o, (unsigned char)s[i]);
    i++;
  }
#endif
#if defined(__AVX2__) || defined(__SSE2__)
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
    _mm_storeu_si128((__m128i *)o, v);
    __m128i q = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
    __m128i b = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
    __m128i c = _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(0x1f)), v);
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(q, b), c));
    if (!mask) {
      i += 16;
      o += 16;
      continue;
    }
    unsigned k = (unsigned)__builtin_ctz(mask);
    o += k;
    i += k;
    o = esc_char(o, (unsigned char)s[i]);
    i++;
  }
#endif
  for (; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (needs_esc(ch)) o = esc_char(o, ch);
    else *o++ = (char)ch;
  }
  return (size_t)(o - out);
}

static inline char *w_lit(char *p, const char *s, size_t n) {
  memcpy(p, s, n);
  return p + n;
}
#define WLIT(p, s) w_lit(p, s, sizeof(s) - 1)

static inline char *w_i64(char *p, i64 v) {
  u64 u;
  if (v < 0) {
    *p++ = '-';
    u = (u64)0 - (u64)v;
  } else {
    u = (u64)v;
  }
  if (u < 10) {
    *p++ = (char)('0' + u);
    return p;
  }
  if (u < 100) {
    memcpy(p, &DEC2_LUT[u * 2], 2);
    return p + 2;
  }
  char t[24];
  int k = 24;
  while (u >= 100) {
    u64 r = (u % 100) * 2;
    u /= 100;
    k -= 2;
    memcpy(&t[k], &DEC2_LUT[r], 2);
  }
  if (u < 10) {
    t[--k] = (char)('0' + u);
  } else {
    k -= 2;
    memcpy(&t[k], &DEC2_LUT[u * 2], 2);
  }
  int len = 24 - k;
  memcpy(p, &t[k], (size_t)len);
  return p + len;
}

static inline char *w_str(char *p, const void *s, size_t n) {
  *p++ = '"';
  p += json_escape_buf(p, (const char *)s, n);
  *p++ = '"';
  return p;
}

static void format_utc_ms(char o[24], int64_t ms) {
  int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
  int64_t rem = ms - days * 86400000;
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  unsigned hh = (unsigned)(rem / 3600000);
  unsigned mi = (unsigned)(rem / 60000 % 60);
  unsigned ss = (unsigned)(rem / 1000 % 60);
  unsigned mss = (unsigned)(rem % 1000);
  o[0] = (char)('0' + y / 1000 % 10);
  o[1] = (char)('0' + y / 100 % 10);
  o[2] = (char)('0' + y / 10 % 10);
  o[3] = (char)('0' + y % 10);
  o[4] = '-';
  memcpy(&o[5], &DEC2_LUT[m * 2], 2);
  o[7] = '-';
  memcpy(&o[8], &DEC2_LUT[d * 2], 2);
  o[10] = 'T';
  memcpy(&o[11], &DEC2_LUT[hh * 2], 2);
  o[13] = ':';
  memcpy(&o[14], &DEC2_LUT[mi * 2], 2);
  o[16] = ':';
  memcpy(&o[17], &DEC2_LUT[ss * 2], 2);
  o[19] = '.';
  o[20] = (char)('0' + mss / 100);
  memcpy(&o[21], &DEC2_LUT[(mss % 100) * 2], 2);
  o[23] = 'Z';
}

static int64_t realtime_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static double mono_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static inline u64 rd_varint(const u8 *p, int *n) {
  if (!(p[0] & 0x80)) { *n = 1; return p[0]; }
  if (!(p[1] & 0x80)) { *n = 2; return ((u64)(p[0] & 0x7f) << 7) | p[1]; }
  u64 v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 7) | (p[i] & 0x7f);
    if (!(p[i] & 0x80)) { *n = i + 1; return v; }
  }
  v = (v << 8) | p[8];
  *n = 9;
  return v;
}

static inline u32 st_len(u64 t) {
  static const u8 L[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? L[t] : (u32)((t - 12) / 2);
}

static inline i64 st_int(u64 t, const u8 *p) {
  switch (t) {
    case 1: return (i8)p[0];
    case 2: return (i16)((p[0] << 8) | p[1]);
    case 3: return ((int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8)) >> 8;
    case 4: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
    case 5: {
      i64 v = 0;
      for (int i = 0; i < 6; i++) v = (v << 8) | p[i];
      return (v << 16) >> 16;
    }
    case 6: {
      u64 v = 0;
      for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
      return (i64)v;
    }
    case 9: return 1;
    default: return 0;
  }
}

static inline int st_isint(u64 t) {
  return (t >= 1 && t <= 6) || t == 8 || t == 9;
}

static inline int st_istext(u64 t) {
  return t >= 13 && (t & 1);
}

static int rec_decode(const u8 *rec, u32 amt, int nf, u64 *types, const u8 **data) {
  int n;
  u64 hdr = rd_varint(rec, &n);
  const u8 *h = rec + n, *he = rec + hdr, *d = rec + hdr;
  int i = 0;
  if (hdr > amt) return -1;
  while (h < he && i < nf) {
    types[i] = rd_varint(h, &n);
    h += n;
    data[i] = d;
    d += st_len(types[i]);
    i++;
  }
  return d <= rec + amt ? i : -1;
}

#define LOCK_BYTE_OFFSET 0x40000000u

typedef struct {
  sqlite3_pcache *base;
  unsigned max_key;
  unsigned lock_page;
  unsigned n_parked;
} wcache_t;

static sqlite3_pcache_methods2 pcache_base;
static int pcache_installed = 0;

static int wc_init(void *arg) { (void)arg; return pcache_base.xInit(pcache_base.pArg); }
static void wc_shutdown(void *arg) { (void)arg; pcache_base.xShutdown(pcache_base.pArg); }
static sqlite3_pcache *wc_create(int szPage, int szExtra, int bPurgeable) {
  wcache_t *w = sqlite3_malloc(sizeof *w);
  if (!w) return NULL;
  w->base = pcache_base.xCreate(szPage, szExtra, bPurgeable);
  if (!w->base) { sqlite3_free(w); return NULL; }
  w->max_key = 0;
  w->lock_page = LOCK_BYTE_OFFSET / (unsigned)szPage + 1;
  w->n_parked = 0;
  return (sqlite3_pcache *)w;
}
static void wc_cachesize(sqlite3_pcache *p, int n) { pcache_base.xCachesize(((wcache_t *)p)->base, n); }
static int wc_pagecount(sqlite3_pcache *p) { return pcache_base.xPagecount(((wcache_t *)p)->base); }
static sqlite3_pcache_page *wc_fetch(sqlite3_pcache *p, unsigned key, int create) {
  wcache_t *w = (wcache_t *)p;
  sqlite3_pcache_page *pg = pcache_base.xFetch(w->base, key, create);
  if (pg && create) {
    if (key == w->lock_page) w->n_parked++;
    else if (key > w->max_key) w->max_key = key;
  }
  return pg;
}
static void wc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) {
  pcache_base.xUnpin(((wcache_t *)p)->base, pg, discard);
}
static void wc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned old_key, unsigned new_key) {
  wcache_t *w = (wcache_t *)p;
  pcache_base.xRekey(w->base, pg, old_key, new_key);
  if (old_key == w->lock_page && w->n_parked) w->n_parked--;
  if (new_key == w->lock_page) w->n_parked++;
  else if (new_key > w->max_key) w->max_key = new_key;
}
static void wc_truncate(sqlite3_pcache *p, unsigned limit) {
  wcache_t *w = (wcache_t *)p;
  if (w->n_parked == 0 && w->max_key < limit) return;
  pcache_base.xTruncate(w->base, limit);
  if (limit > 0 && w->max_key >= limit) w->max_key = limit - 1;
  if (w->lock_page >= limit) w->n_parked = 0;
}
static void wc_destroy(sqlite3_pcache *p) {
  wcache_t *w = (wcache_t *)p;
  pcache_base.xDestroy(w->base);
  sqlite3_free(w);
}
static void wc_shrink(sqlite3_pcache *p) { pcache_base.xShrink(((wcache_t *)p)->base); }

static void install_pcache_once(void) {
  if (pcache_installed) return;
  if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pcache_base) != SQLITE_OK) return;
  static sqlite3_pcache_methods2 m;
  m = pcache_base;
  m.pArg = NULL;
  m.xInit = wc_init;
  m.xShutdown = wc_shutdown;
  m.xCreate = wc_create;
  m.xCachesize = wc_cachesize;
  m.xPagecount = wc_pagecount;
  m.xFetch = wc_fetch;
  m.xUnpin = wc_unpin;
  m.xRekey = wc_rekey;
  m.xTruncate = wc_truncate;
  m.xDestroy = wc_destroy;
  m.xShrink = wc_shrink;
  if (sqlite3_config(SQLITE_CONFIG_PCACHE2, &m) == SQLITE_OK) pcache_installed = 1;
}

typedef struct {
  sqlite3_file base;
  sqlite3_file *real;
  int wal;
  int commit_hdr;
  u8 *buf;
  size_t n, cap;
  sqlite3_int64 off;
  sqlite3_int64 next;
  int size_ok;
  sqlite3_int64 size;
} aw_file_t;

static sqlite3_vfs *aw_base = NULL;
static sqlite3_vfs aw_vfs;
#define AWR(f) (((aw_file_t *)(f))->real)

static int aw_flush(aw_file_t *f) {
  if (!f->n) return SQLITE_OK;
  int rc = SQLITE_OK;
  for (size_t o = 0; o < f->n && rc == SQLITE_OK; o += 0x1f000) {
    size_t k = f->n - o < 0x1f000 ? f->n - o : 0x1f000;
    rc = f->real->pMethods->xWrite(f->real, f->buf + o, (int)k, f->off + (sqlite3_int64)o);
  }
  f->n = 0;
  return rc;
}

#define AW_FLUSH(f) do { if (((aw_file_t *)(f))->n) { int rc_ = aw_flush((aw_file_t *)(f)); if (rc_) return rc_; } } while (0)

static int aw_close(sqlite3_file *f) {
  aw_file_t *c = (aw_file_t *)f;
  int rc = aw_flush(c);
  free(c->buf);
  c->buf = NULL;
  int rc2 = c->real->pMethods ? c->real->pMethods->xClose(c->real) : SQLITE_OK;
  return rc ? rc : rc2;
}

static int aw_read(sqlite3_file *f, void *b, int n, sqlite3_int64 o) {
  AW_FLUSH(f);
  return AWR(f)->pMethods->xRead(AWR(f), b, n, o);
}

static int aw_write(sqlite3_file *f, const void *b, int n, sqlite3_int64 o) {
  aw_file_t *c = (aw_file_t *)f;
  if (!c->wal) {
    int rc = c->real->pMethods->xWrite(c->real, b, n, o);
    if (rc) c->size_ok = 0;
    else if (c->size_ok && o + n > c->size) c->size = o + n;
    return rc;
  }
  if (n == 24 && o != c->next) {
    const u8 *h = b;
    AW_FLUSH(f);
    c->next = o + n;
    c->commit_hdr = (h[4] | h[5] | h[6] | h[7]) != 0;
    return c->real->pMethods->xWrite(c->real, b, n, o);
  }
  c->next = o + n;
  if (c->n && o != c->off + (sqlite3_int64)c->n) AW_FLUSH(f);
  if (c->n + (size_t)n > c->cap) {
    if (c->n + (size_t)n > (4u << 20)) {
      AW_FLUSH(f);
      if ((size_t)n > c->cap) return c->real->pMethods->xWrite(c->real, b, n, o);
    } else {
      size_t nc = c->cap ? c->cap : 65536;
      while (nc < c->n + (size_t)n) nc *= 2;
      u8 *q = realloc(c->buf, nc);
      if (!q) {
        AW_FLUSH(f);
        return c->real->pMethods->xWrite(c->real, b, n, o);
      }
      c->buf = q;
      c->cap = nc;
    }
  }
  if (!c->n) c->off = o;
  memcpy(c->buf + c->n, b, (size_t)n);
  c->n += (size_t)n;
  if (n == 24) {
    const u8 *h = b;
    c->commit_hdr = (h[4] | h[5] | h[6] | h[7]) != 0;
    return SQLITE_OK;
  }
  if (c->commit_hdr) {
    c->commit_hdr = 0;
    return aw_flush(c);
  }
  return SQLITE_OK;
}

static int aw_trunc(sqlite3_file *f, sqlite3_int64 s) {
  AW_FLUSH(f);
  ((aw_file_t *)f)->size_ok = 0;
  return AWR(f)->pMethods->xTruncate(AWR(f), s);
}

static int aw_sync(sqlite3_file *f, int fl) {
  AW_FLUSH(f);
  return AWR(f)->pMethods->xSync(AWR(f), fl);
}

static int aw_size(sqlite3_file *f, sqlite3_int64 *s) {
  aw_file_t *c = (aw_file_t *)f;
  AW_FLUSH(f);
  if (c->size_ok) {
    *s = c->size;
    return SQLITE_OK;
  }
  int rc = c->real->pMethods->xFileSize(c->real, s);
  if (rc == SQLITE_OK && !c->wal) {
    c->size = *s;
    c->size_ok = 1;
  }
  return rc;
}

static int aw_lock(sqlite3_file *f, int l) {
  return AWR(f)->pMethods->xLock(AWR(f), l);
}

static int aw_unlock(sqlite3_file *f, int l) {
  AW_FLUSH(f);
  return AWR(f)->pMethods->xUnlock(AWR(f), l);
}

static int aw_crl(sqlite3_file *f, int *r) { return AWR(f)->pMethods->xCheckReservedLock(AWR(f), r); }

static int aw_fc(sqlite3_file *f, int op, void *a) {
  AW_FLUSH(f);
  switch (op) {
    case SQLITE_FCNTL_LOCKSTATE: case SQLITE_FCNTL_VFSNAME: case SQLITE_FCNTL_PRAGMA:
    case SQLITE_FCNTL_BUSYHANDLER: case SQLITE_FCNTL_HAS_MOVED: case SQLITE_FCNTL_MMAP_SIZE:
    case SQLITE_FCNTL_SYNC: case SQLITE_FCNTL_COMMIT_PHASETWO: case SQLITE_FCNTL_PDB:
    case SQLITE_FCNTL_LOCK_TIMEOUT: case SQLITE_FCNTL_DATA_VERSION: case SQLITE_FCNTL_CKPT_START:
    case SQLITE_FCNTL_CKPT_DONE: case SQLITE_FCNTL_POWERSAFE_OVERWRITE: case SQLITE_FCNTL_PERSIST_WAL:
    case SQLITE_FCNTL_TEMPFILENAME: case SQLITE_FCNTL_WAL_BLOCK: case SQLITE_FCNTL_FILE_POINTER:
    case SQLITE_FCNTL_EXTERNAL_READER:
      break;
    default:
      ((aw_file_t *)f)->size_ok = 0;
  }
  return AWR(f)->pMethods->xFileControl(AWR(f), op, a);
}

static int aw_ss(sqlite3_file *f) { return AWR(f)->pMethods->xSectorSize(AWR(f)); }
static int aw_dc(sqlite3_file *f) { return AWR(f)->pMethods->xDeviceCharacteristics(AWR(f)); }
static int aw_shmmap(sqlite3_file *f, int i, int s, int e, void volatile **p) { return AWR(f)->pMethods->xShmMap(AWR(f), i, s, e, p); }
static int aw_shmlock(sqlite3_file *f, int o, int n, int fl) { return AWR(f)->pMethods->xShmLock(AWR(f), o, n, fl); }
static void aw_shmbar(sqlite3_file *f) { AWR(f)->pMethods->xShmBarrier(AWR(f)); }
static int aw_shmunmap(sqlite3_file *f, int d) { return AWR(f)->pMethods->xShmUnmap(AWR(f), d); }
static int aw_fetch(sqlite3_file *f, sqlite3_int64 o, int n, void **p) { return AWR(f)->pMethods->xFetch(AWR(f), o, n, p); }
static int aw_unfetch(sqlite3_file *f, sqlite3_int64 o, void *p) { return AWR(f)->pMethods->xUnfetch(AWR(f), o, p); }

static const sqlite3_io_methods aw_io = {
  3, aw_close, aw_read, aw_write, aw_trunc, aw_sync, aw_size, aw_lock, aw_unlock, aw_crl, aw_fc,
  aw_ss, aw_dc, aw_shmmap, aw_shmlock, aw_shmbar, aw_shmunmap, aw_fetch, aw_unfetch
};

static int aw_open(sqlite3_vfs *v, const char *name, sqlite3_file *f, int flags, int *out) {
  (void)v;
  aw_file_t *c = (aw_file_t *)f;
  memset(c, 0, sizeof *c);
  c->real = (sqlite3_file *)(c + 1);
  int rc = aw_base->xOpen(aw_base, name, c->real, flags, out);
  c->base.pMethods = (rc == SQLITE_OK && c->real->pMethods) ? &aw_io : NULL;
  c->wal = (flags & SQLITE_OPEN_WAL) != 0;
  return rc;
}

static const char *register_astraea_vfs(void) {
  if (!aw_base) {
    aw_base = sqlite3_vfs_find("unix-excl");
    if (!aw_base) return "unix-excl";
    aw_vfs = *aw_base;
    aw_vfs.zName = "astraea-wal-excl";
    aw_vfs.szOsFile = (int)sizeof(aw_file_t) + aw_base->szOsFile;
    aw_vfs.xOpen = aw_open;
    sqlite3_vfs_register(&aw_vfs, 0);
  }
  return "astraea-wal-excl";
}

enum { C_P = 0, C_U = 1, C_L = 2, C_C = 3, NRC = 4 };

struct astraea_eng {
  sqlite3 *db;
  Btree *bt;
  Pager *pager;
  Pgno r_posts, r_users, r_likes, r_lpid, r_pcat;
  KeyInfo *ki_lpid, *ki_pcat;
  UnpackedRecord *uk_lpid;
  BtCursor *rc[NRC];
  unsigned rc_open;
  int in_batch, txn, sql_txn, failed, wrote_sql;
  int native_ok, inv_ok;
  sqlite3_stmt *s_begin, *s_commit, *s_rollback, *s_health;
  sqlite3_stmt *s_feed, *s_post, *s_exists, *s_ins, *s_like;
  int64_t ts_ms;
  char ts[24];
  u8 *scratch[2];
  u32 scratch_n[2];
  u32 counts[RANGE_MAX];
  int wal_frames;
  char *path;
  pthread_t ck_thr;
  pthread_mutex_t ck_mu;
  pthread_cond_t ck_cv;
  int ck_on, ck_want, ck_stop;
  sqlite3 *ck_db;
  int drain;
  double drain_t0;
};

static int busy_sleep_main(void *arg, int n) {
  (void)arg;
  if (n >= 4000) return 0;
  struct timespec ts = {0, n < 10 ? 20000 : n < 100 ? 100000 : 500000};
  nanosleep(&ts, NULL);
  return 1;
}

static int busy_sleep_ck(void *arg, int n) {
  (void)arg;
  if (n >= 20000) return 0;
  struct timespec ts = {0, 200000};
  nanosleep(&ts, NULL);
  return 1;
}

static sqlite3_file *conn_wal_fd(sqlite3 *db) {
  Pager *pg = sqlite3BtreePager(db->aDb[0].pBt);
  return (pg && pg->pWal) ? pg->pWal->pWalFd : NULL;
}

static int unwrap_unix_fd(sqlite3_file *f) {
  if (f && f->pMethods == &aw_io) f = ((aw_file_t *)f)->real;
  if (f && f->pMethods && f->pMethods->xRead == unixRead) return ((unixFile *)f)->h;
  return -1;
}

static void wal_drop_pages(sqlite3_file *wfd, u32 from, u32 to) {
#if defined(__linux__) && defined(POSIX_FADV_DONTNEED)
  if (!wfd || to <= from) return;
  int fd = unwrap_unix_fd(wfd);
  if (fd < 0) return;
  i64 sz = 4096 + 24;
  i64 a = 32 + (i64)from * sz;
  i64 b = 32 + (i64)to * sz;
  a = (a + 4095) & ~(i64)4095;
  b &= ~(i64)4095;
  if (b > a) (void)posix_fadvise(fd, a, b - a, POSIX_FADV_DONTNEED);
#else
  (void)wfd; (void)from; (void)to;
#endif
}

static void wal_state(astraea_eng *e, u32 *mx, u32 *pending) {
  Wal *w = e->pager->pWal;
  *mx = *pending = 0;
  if (!w || !w->nWiData || !w->apWiData[0]) return;
  u32 bf = walCkptInfo(w)->nBackfill;
  *mx = w->hdr.mxFrame;
  *pending = *mx > bf ? *mx - bf : 0;
}

static void *ck_thread_main(void *arg) {
  astraea_eng *e = arg;
#ifdef __linux__
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
#endif
  u32 bf_prev = 0;
  pthread_mutex_lock(&e->ck_mu);
  for (;;) {
    while (!e->ck_want && !e->ck_stop) pthread_cond_wait(&e->ck_cv, &e->ck_mu);
    if (e->ck_stop) break;
    e->ck_want = 0;
    pthread_mutex_unlock(&e->ck_mu);

    for (int pass = 0; pass < 16; pass++) {
      int nlog = 0, nck = 0;
      if (sqlite3_wal_checkpoint_v2(e->ck_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &nck) != SQLITE_OK) break;
      if ((u32)nck < bf_prev) bf_prev = 0;
      wal_drop_pages(conn_wal_fd(e->ck_db), bf_prev, (u32)nck);
      bf_prev = (u32)nck;
      if (!__atomic_load_n(&e->drain, __ATOMIC_ACQUIRE) || nck >= nlog) break;
      struct timespec ts = {0, 200000};
      nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&e->ck_mu);
  }
  pthread_mutex_unlock(&e->ck_mu);
  return NULL;
}

static int ck_start(astraea_eng *e) {
  if (sqlite3_open_v2(e->path, &e->ck_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) return -1;
  sqlite3_busy_handler(e->ck_db, busy_sleep_ck, NULL);
  if (sqlite3_exec(e->ck_db,
                   "PRAGMA locking_mode=NORMAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=16;"
                   "PRAGMA wal_autocheckpoint=0; PRAGMA journal_size_limit=0; SELECT count(*) FROM sqlite_schema;",
                   NULL, NULL, NULL) != SQLITE_OK) return -1;
  pthread_mutex_init(&e->ck_mu, NULL);
  pthread_cond_init(&e->ck_cv, NULL);
  if (pthread_create(&e->ck_thr, NULL, ck_thread_main, e) != 0) return -1;
  e->ck_on = 1;
  return 0;
}

static void ck_signal(astraea_eng *e) {
  pthread_mutex_lock(&e->ck_mu);
  e->ck_want = 1;
  pthread_cond_signal(&e->ck_cv);
  pthread_mutex_unlock(&e->ck_mu);
}

static void ck_stop(astraea_eng *e) {
  if (e->ck_on) {
    pthread_mutex_lock(&e->ck_mu);
    e->ck_stop = 1;
    pthread_cond_signal(&e->ck_cv);
    pthread_mutex_unlock(&e->ck_mu);
    pthread_join(e->ck_thr, NULL);
    e->ck_on = 0;
  }
  if (e->ck_db) {
    sqlite3_close(e->ck_db);
    e->ck_db = NULL;
  }
}

static int wal_hook_cb(void *arg, sqlite3 *db, const char *name, int frames) {
  (void)db; (void)name;
  ((astraea_eng *)arg)->wal_frames = frames;
  return SQLITE_OK;
}

static BtCursor *alloc_cursor(void) {
  BtCursor *c = sqlite3MallocZero(sqlite3BtreeCursorSize());
  if (c) sqlite3BtreeCursorZero(c);
  return c;
}

static int bt_txn(astraea_eng *e, int wr) {
  if (e->txn > wr) return 0;
  if (e->txn == 1 && wr) {
    if (sqlite3BtreeBeginTrans(e->bt, 1, 0) != SQLITE_OK) return -1;
    e->txn = 2;
    return 0;
  }
  int cookie = 0;
  if (sqlite3BtreeBeginTrans(e->bt, wr, &cookie) != SQLITE_OK) return -1;
  if (cookie != e->db->aDb[0].pSchema->schema_cookie) {
    sqlite3BtreeCommit(e->bt);
    return -1;
  }
  e->txn = wr ? 2 : 1;
  return 0;
}

static void close_cursors(astraea_eng *e) {
  for (int i = 0; i < NRC; i++) {
    if (e->rc_open & (1u << i)) sqlite3BtreeCloseCursor(e->rc[i]);
  }
  e->rc_open = 0;
}

static int writer_upgrade(astraea_eng *e) {
  if (e->txn == 2) return 0;
  if (e->txn == 1) {
    int rc = sqlite3BtreeBeginTrans(e->bt, 1, 0);
    if (rc == SQLITE_OK) {
      e->txn = 2;
      return 0;
    }
    if ((rc & 0xff) != SQLITE_BUSY) return -1;
    close_cursors(e);
    if (e->sql_txn) {
      int r2 = sqlite3_step(e->s_commit);
      sqlite3_reset(e->s_commit);
      e->sql_txn = 0;
      if (r2 != SQLITE_DONE) return -1;
    } else {
      sqlite3BtreeCommit(e->bt);
    }
    e->txn = 0;
  }
  return bt_txn(e, 1);
}

static int sql_txn(astraea_eng *e) {
  if (e->sql_txn) return 0;
  int rc = sqlite3_step(e->s_begin);
  sqlite3_reset(e->s_begin);
  if (rc != SQLITE_DONE) return -1;
  e->sql_txn = 1;
  return 0;
}

static BtCursor *rcur(astraea_eng *e, int k) {
  if (!(e->rc_open & (1u << k))) {
    if (bt_txn(e, 0)) return NULL;
    Pgno root = k == C_P ? e->r_posts : k == C_U ? e->r_users : k == C_L ? e->r_lpid : e->r_pcat;
    KeyInfo *ki = k == C_L ? e->ki_lpid : k == C_C ? e->ki_pcat : NULL;
    sqlite3BtreeCursorZero(e->rc[k]);
    if (sqlite3BtreeCursor(e->bt, root, 0, ki, e->rc[k]) != SQLITE_OK) return NULL;
    e->rc_open |= 1u << k;
  }
  return e->rc[k];
}

static inline int text_le(const u8 *a, u32 an, const u8 *b, u32 bn) {
  u32 m = an < bn ? an : bn;
  int c = memcmp(a, b, m);
  return c < 0 || (c == 0 && an <= bn);
}

static void check_startup_invariant(astraea_eng *e) {
  if (!e->native_ok) {
    e->inv_ok = 0;
    return;
  }
  BtCursor *cp = rcur(e, C_P), *cu = rcur(e, C_U);
  if (!cp || !cu) {
    e->inv_ok = 0;
    close_cursors(e);
    if (e->txn) { sqlite3BtreeCommit(e->bt); e->txn = 0; }
    return;
  }
  u64 *ubits = calloc(1024, sizeof(u64));
  int res = 0;
  if (ubits && sqlite3BtreeFirst(cu, &res) == SQLITE_OK && !res) {
    do {
      i64 uid = sqlite3BtreeIntegerKey(cu);
      if (uid > 0 && uid < 65536) ubits[uid >> 6] |= (1ULL << (uid & 63));
    } while (sqlite3BtreeNext(cu, 0) == SQLITE_OK);
  }
  int ok = 1;
  i64 expect = 1;
  u8 prev_ts[64];
  u32 prev_n = 0;
  int have_prev = 0;
  if (sqlite3BtreeFirst(cp, &res) != SQLITE_OK) {
    ok = 0;
  } else if (!res) {
    do {
      i64 id = sqlite3BtreeIntegerKey(cp);
      if (id != expect) { ok = 0; break; }
      u32 sz = sqlite3BtreePayloadSize(cp), amt = 0;
      const u8 *rec = sqlite3BtreePayloadFetch(cp, &amt);
      u64 t[4];
      const u8 *d[4];
      if (amt < sz || rec_decode(rec, sz, 4, t, d) != 4 || !st_isint(t[1]) || !st_istext(t[3])) {
        ok = 0;
        break;
      }
      u32 cl = st_len(t[3]);
      if (cl > sizeof prev_ts) { ok = 0; break; }
      if (have_prev && !text_le(prev_ts, prev_n, d[3], cl)) { ok = 0; break; }
      memcpy(prev_ts, d[3], cl);
      prev_n = cl;
      have_prev = 1;
      i64 uid = st_int(t[1], d[1]);
      if (ubits && uid > 0 && uid < 65536) {
        if (!(ubits[uid >> 6] & (1ULL << (uid & 63)))) { ok = 0; break; }
      } else {
        int ures = 0;
        if (sqlite3BtreeTableMoveto(cu, uid, 0, &ures) != SQLITE_OK || ures != 0) { ok = 0; break; }
      }
      expect++;
    } while (sqlite3BtreeNext(cp, 0) == SQLITE_OK);
  }
  free(ubits);
  close_cursors(e);
  if (e->txn) {
    sqlite3BtreeCommit(e->bt);
    e->txn = 0;
  }
  sqlite3_db_release_memory(e->db);
#if defined(__linux__) && defined(POSIX_FADV_DONTNEED)
  int dbfd = unwrap_unix_fd(e->pager->fd);
  if (dbfd >= 0) (void)posix_fadvise(dbfd, 0, 0, POSIX_FADV_DONTNEED);
#endif
  e->inv_ok = ok;
}

static const u8 *fetch_payload(astraea_eng *e, BtCursor *c, u32 *amt, int slot) {
  u32 sz = sqlite3BtreePayloadSize(c);
  const u8 *p = sqlite3BtreePayloadFetch(c, amt);
  if (*amt >= sz) {
    *amt = sz;
    return p;
  }
  if (e->scratch_n[slot] < sz) {
    u8 *q = realloc(e->scratch[slot], sz + 16);
    if (!q) return NULL;
    e->scratch[slot] = q;
    e->scratch_n[slot] = sz;
  }
  if (sqlite3BtreePayload(c, 0, sz, e->scratch[slot]) != SQLITE_OK) return NULL;
  *amt = sz;
  return e->scratch[slot];
}

static inline const u8 *idx_fetch(BtCursor *c, u32 *amt) {
  u32 sz = sqlite3BtreePayloadSize(c);
  const u8 *p = sqlite3BtreePayloadFetch(c, amt);
  return *amt >= sz ? (*amt = sz, p) : NULL;
}

static inline i64 lpid_cell(const MemPage *pg, int ix) {
  const u8 *c = pg->aData + (pg->maskPage & get2byteAligned(&pg->aCellIdx[2 * ix]));
  if (!pg->leaf) c += 4;
  int n;
  u64 sz = rd_varint(c, &n);
  c += n;
  if (sz > pg->maxLocal) return INT64_MIN;
  u64 hdr = rd_varint(c, &n);
  const u8 *h = c + n;
  u64 t = rd_varint(h, &n);
  if (!st_isint(t) || hdr > sz) return INT64_MIN;
  return st_int(t, c + hdr);
}

static i64 count_likes(astraea_eng *e, i64 lo, i64 hi, u32 *counts) {
  BtCursor *c = rcur(e, C_L);
  if (!c) return -1;
  UnpackedRecord *r = e->uk_lpid;
  int res;
  r->nField = 1;
  r->default_rc = +1;
  r->eqSeen = 0;
  r->errCode = 0;
  r->aMem[0].flags = MEM_Int;
  r->aMem[0].u.i = lo;
  if (sqlite3BtreeIndexMoveto(c, r, &res) != SQLITE_OK) return -1;
  if (c->eState != CURSOR_VALID) return 0;
  int rc = SQLITE_OK;
  if (res < 0) {
    rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc) return -1;
  }
  i64 total = 0;
  for (;;) {
    MemPage *pg = c->pPage;
    int ix = c->ix;
    if (pg->leaf) {
      for (; ix < pg->nCell; ix++) {
        i64 pid = lpid_cell(pg, ix);
        if (pid == INT64_MIN) return -1;
        if (pid > hi) return total;
        total++;
        if (counts) counts[pid - lo]++;
      }
      c->ix = (u16)(pg->nCell - 1);
    } else {
      i64 pid = lpid_cell(pg, ix);
      if (pid == INT64_MIN) return -1;
      if (pid > hi) return total;
      total++;
      if (counts) counts[pid - lo]++;
    }
    rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return total;
    if (rc) return -1;
  }
}

typedef struct {
  const u8 *body, *at, *user;
  u32 bn, an, un;
} prow_t;

static int load_row(astraea_eng *e, BtCursor *cp, prow_t *o) {
  u32 amt;
  const u8 *rec = fetch_payload(e, cp, &amt, 0);
  u64 t[4];
  const u8 *d[4];
  if (!rec || rec_decode(rec, amt, 4, t, d) != 4 || !st_isint(t[1])) return -1;
  o->body = d[2];
  o->bn = st_len(t[2]);
  o->at = d[3];
  o->an = st_len(t[3]);
  BtCursor *cu = rcur(e, C_U);
  if (!cu) return -1;
  int res;
  if (sqlite3BtreeTableMoveto(cu, st_int(t[1], d[1]), 0, &res) != SQLITE_OK) return -1;
  if (res != 0 || cu->eState != CURSOR_VALID) return 0;
  rec = fetch_payload(e, cu, &amt, 1);
  u64 tu[2];
  const u8 *du[2];
  if (!rec || rec_decode(rec, amt, 2, tu, du) != 2) return -1;
  o->user = du[1];
  o->un = st_len(tu[1]);
  return 1;
}

static size_t esc_len(const u8 *s, u32 n) {
  size_t l = n;
  for (u32 i = 0; i < n; i++) {
    unsigned c = s[i];
    if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') l += 1;
    else if (c < 0x20) l += 5;
  }
  return l;
}

static inline int post_fits(const prow_t *r, size_t room) {
  if (room >= 120 + 6 * ((size_t)r->bn + r->an + r->un)) return 1;
  return room >= 120 + esc_len(r->body, r->bn) + esc_len(r->at, r->an) + esc_len(r->user, r->un);
}

static inline char *w_post(char *p, i64 id, const prow_t *r, i64 likes) {
  p = WLIT(p, "{\"id\":");
  p = w_i64(p, id);
  p = WLIT(p, ",\"body\":");
  p = w_str(p, r->body, r->bn);
  p = WLIT(p, ",\"created_at\":");
  p = w_str(p, r->at, r->an);
  p = WLIT(p, ",\"author\":");
  p = w_str(p, r->user, r->un);
  p = WLIT(p, ",\"like_count\":");
  p = w_i64(p, likes);
  *p++ = '}';
  return p;
}

static int err500(char *out, size_t *n) {
  static const char m[] = "{\"error\":\"internal server error\"}";
  memcpy(out, m, sizeof m - 1);
  *n = sizeof m - 1;
  return 500;
}

static int err404(char *out, size_t *n) {
  static const char m[] = "{\"error\":\"post not found\"}";
  memcpy(out, m, sizeof m - 1);
  *n = sizeof m - 1;
  return 404;
}

#define MS_MAX 32
typedef struct {
  MemPage *leaf;
  int ix;
  int found;
} ms_res_t;
#define MS_PF(p) __builtin_prefetch((const void *)(p), 0, 3)

static inline i64 ms_cell_key(MemPage *pg, int i) {
  u8 *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf) while (0x80 <= *(c++)) {}
  return (i64)sqlite3VarintValue(c);
}

static inline int ms_lower_bound(MemPage *pg, i64 k) {
  int n = pg->nCell, lo = 0, len = n;
  if (n >= 8) {
    i64 k0 = ms_cell_key(pg, 0), kn = ms_cell_key(pg, n - 1);
    if (k <= k0) return 0;
    if (k > kn) return n;
    int g = (int)((double)(k - k0) * (n - 1) / (double)(kn - k0));
    if (g < 1) g = 1;
    if (g > n - 1) g = n - 1;
    if (ms_cell_key(pg, g) >= k) {
      for (int s = 0; s < 4; s++) {
        if (ms_cell_key(pg, g - 1) < k) return g;
        g--;
      }
      lo = 1;
      len = g;
    } else {
      for (int s = 0; s < 4; s++) {
        g++;
        if (ms_cell_key(pg, g) >= k) return g;
      }
      lo = g + 1;
      len = n - 1 - g;
    }
  }
  while (len > 0) {
    int h = len >> 1;
    if (ms_cell_key(pg, lo + h) < k) {
      lo += h + 1;
      len -= h + 1;
    } else {
      len = h;
    }
  }
  return lo;
}

static inline PgHdr1 **ms_bucket(BtShared *bt, Pgno pg) {
  sqlite3_pcache *raw = bt->pPager->pPCache->pCache;
  PCache1 *pc = (PCache1 *)(pcache_installed ? ((wcache_t *)raw)->base : raw);
  return &pc->apHash[pg % pc->nHash];
}

static int ms_table_seek(Btree *p, Pgno root, const i64 *keys_in, int n, ms_res_t *r) {
  BtShared *bt = p->pBt;
  MemPage *cur[MS_MAX];
  Pgno ch[MS_MAX];
  int rc, ord[MS_MAX];
  i64 keys[MS_MAX];
  if (n > MS_MAX || n < 1) return SQLITE_MISUSE;
  for (int i = 0; i < n; i++) {
    int j = i;
    while (j > 0 && keys_in[ord[j - 1]] > keys_in[i]) {
      ord[j] = ord[j - 1];
      j--;
    }
    ord[j] = i;
  }
  for (int i = 0; i < n; i++) keys[i] = keys_in[ord[i]];
  if ((rc = getAndInitPage(bt, root, &cur[0], PAGER_GET_READONLY))) return rc;
  if (cur[0]->nCell == 0) {
    releasePage(cur[0]);
    for (int i = 0; i < n; i++) { r[i].leaf = NULL; r[i].found = 0; r[i].ix = 0; }
    return SQLITE_OK;
  }
  for (int i = 1; i < n; i++) {
    sqlite3PagerRef(cur[0]->pDbPage);
    cur[i] = cur[0];
  }
  for (int depth = 0; !cur[0]->leaf; depth++) {
    if (depth > 20) { rc = SQLITE_CORRUPT_BKPT; goto fail; }
    for (int i = 0; i < n; i++) {
      MemPage *pg = cur[i];
      int lb = ms_lower_bound(pg, keys[i]);
      ch[i] = lb >= pg->nCell ? get4byte(&pg->aData[pg->hdrOffset + 8]) : get4byte(findCell(pg, lb));
      if (!i || ch[i] != ch[i - 1]) MS_PF(ms_bucket(bt, ch[i]));
    }
    for (int i = 0; i < n; i++) {
      if (i && ch[i] == ch[i - 1]) continue;
      PgHdr1 *h = *ms_bucket(bt, ch[i]);
      while (h && h->iKey != ch[i]) h = h->pNext;
      if (h) {
        u8 *d = h->page.pBuf;
        MS_PF(d);
        MS_PF(d + 64);
        MS_PF(h->page.pExtra);
        MS_PF((u8 *)h->page.pExtra + 64);
      }
    }
    for (int i = 0; i < n; i++) releasePage(cur[i]);
    for (int i = 0; i < n; i++) cur[i] = NULL;
    for (int i = 0; i < n; i++) {
      if (i && ch[i] == ch[i - 1]) {
        sqlite3PagerRef(cur[i - 1]->pDbPage);
        cur[i] = cur[i - 1];
        continue;
      }
      if ((rc = getAndInitPage(bt, ch[i], &cur[i], PAGER_GET_READONLY))) goto fail;
      if (cur[i]->nCell < 1 || cur[i]->intKey != 1 || cur[i]->leaf != cur[0]->leaf) {
        rc = SQLITE_CORRUPT_PGNO(ch[i]);
        goto fail;
      }
      MS_PF(findCellPastPtr(cur[i], cur[i]->nCell >> 1));
    }
  }
  for (int i = 0; i < n; i++) {
    MemPage *pg = cur[i];
    int lb = ms_lower_bound(pg, keys[i]), found = 0;
    if (lb < pg->nCell) {
      u8 *c = findCellPastPtr(pg, lb);
      while (0x80 <= *(c++)) {}
      found = (i64)sqlite3VarintValue(c) == keys[i];
    }
    ms_res_t *o = &r[ord[i]];
    o->leaf = pg;
    o->ix = lb;
    o->found = found;
  }
  return SQLITE_OK;
fail:
  for (int i = 0; i < n; i++) if (cur[i]) releasePage(cur[i]);
  return rc;
}

static void ms_release(ms_res_t *r, int n) {
  for (int i = 0; i < n; i++) if (r[i].leaf) releasePage(r[i].leaf);
}

static int feed_tail_batch(astraea_eng *e, char *out, size_t cap, size_t *n) {
  BtCursor *cp = rcur(e, C_P);
  if (!cp) return -1;
  int res = 0;
  if (sqlite3BtreeLast(cp, &res) != SQLITE_OK || res) return -1;
  i64 hi = sqlite3BtreeIntegerKey(cp);
  if (hi < FEED_N) return -1;
  i64 lo = hi - (FEED_N - 1);

  i64 ids[FEED_N], uid[FEED_N];
  prow_t r[FEED_N];
  DbPage *pins[FEED_N];
  ms_res_t ur[FEED_N];
  int npin = 0, st = -1, have_ur = 0;

  for (int i = 0; i < FEED_N; i++) {
    if (i > 0 && sqlite3BtreePrevious(cp, 0) != SQLITE_OK) goto done;
    i64 id = sqlite3BtreeIntegerKey(cp);
    if (id != hi - i) {
      e->inv_ok = 0;
      goto done;
    }
    u32 sz = sqlite3BtreePayloadSize(cp), amt = 0;
    const u8 *rec = sqlite3BtreePayloadFetch(cp, &amt);
    u64 t[4];
    const u8 *d[4];
    if (amt < sz || rec_decode(rec, sz, 4, t, d) != 4 || !st_isint(t[1])) goto done;
    ids[i] = id;
    uid[i] = st_int(t[1], d[1]);
    r[i].body = d[2];
    r[i].bn = st_len(t[2]);
    r[i].at = d[3];
    r[i].an = st_len(t[3]);
    DbPage *pg = cp->pPage->pDbPage;
    if (!npin || pins[npin - 1] != pg) {
      sqlite3PagerRef(pg);
      pins[npin++] = pg;
    }
  }

  memset(e->counts, 0, FEED_N * sizeof(u32));
  if (count_likes(e, lo, hi, e->counts) < 0) goto done;
  if (ms_table_seek(e->bt, e->r_users, uid, FEED_N, ur) != SQLITE_OK) goto done;
  have_ur = 1;
  for (int i = 0; i < FEED_N; i++) {
    if (!ur[i].found) goto done;
    MemPage *pg = ur[i].leaf;
    CellInfo info;
    pg->xParseCell(pg, findCell(pg, ur[i].ix), &info);
    u64 t[2];
    const u8 *d[2];
    if (info.nLocal != info.nPayload || rec_decode(info.pPayload, info.nPayload, 2, t, d) != 2) goto done;
    r[i].user = d[1];
    r[i].un = st_len(t[1]);
  }
  {
    char *p = WLIT(out, "{\"posts\":["), *end = out + cap;
    for (int i = 0; i < FEED_N; i++) {
      i64 likes = (i64)e->counts[ids[i] - lo];
      if (!post_fits(&r[i], (size_t)(end - p) - 8)) goto done;
      if (i) *p++ = ',';
      p = w_post(p, ids[i], &r[i], likes);
    }
    *p++ = ']';
    *p++ = '}';
    *n = (size_t)(p - out);
    st = 200;
  }
done:
  if (have_ur) ms_release(ur, FEED_N);
  for (int i = 0; i < npin; i++) sqlite3PagerUnref(pins[i]);
  return st;
}

static int feed_idx_batch(astraea_eng *e, char *out, size_t cap, size_t *n) {
  BtCursor *cc = rcur(e, C_C), *cp = rcur(e, C_P);
  if (!cc || !cp) return -1;
  i64 ids[FEED_N], uid[FEED_N];
  prow_t r[FEED_N];
  int nid = 0, res, rc = SQLITE_OK, npin = 0, st = -1, have_ur = 0;
  DbPage *pins[FEED_N];
  ms_res_t ur[FEED_N];
  if (sqlite3BtreeFirst(cc, &res) != SQLITE_OK || res) return -1;
  do {
    u32 amt;
    const u8 *rec = idx_fetch(cc, &amt);
    u64 t[3];
    const u8 *d[3];
    int nf = rec ? rec_decode(rec, amt, 3, t, d) : -1;
    if (nf < 1 || !st_isint(t[nf - 1])) return -1;
    ids[nid++] = st_int(t[nf - 1], d[nf - 1]);
  } while (nid < FEED_N && (rc = sqlite3BtreeNext(cc, 0)) == SQLITE_OK);
  if (nid < FEED_N) return -1;
  i64 lo = INT64_MAX, hi = INT64_MIN;
  for (int i = 0; i < nid; i++) {
    if (ids[i] < lo) lo = ids[i];
    if (ids[i] > hi) hi = ids[i];
  }
  int ranged = (u64)(hi - lo) < RANGE_MAX;
  if (ranged) {
    memset(e->counts, 0, sizeof(u32) * (size_t)(hi - lo + 1));
    if (count_likes(e, lo, hi, e->counts) < 0) return -1;
  }
  i64 prev = INT64_MIN;
  for (int i = 0; i < nid; i++) {
    i64 id = ids[i];
    int found = 0;
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
    u32 sz = sqlite3BtreePayloadSize(cp), amt;
    const u8 *rec = sqlite3BtreePayloadFetch(cp, &amt);
    u64 t[4];
    const u8 *d[4];
    if (amt < sz || rec_decode(rec, sz, 4, t, d) != 4 || !st_isint(t[1])) goto done;
    r[i].body = d[2];
    r[i].bn = st_len(t[2]);
    r[i].at = d[3];
    r[i].an = st_len(t[3]);
    uid[i] = st_int(t[1], d[1]);
    DbPage *pg = cp->pPage->pDbPage;
    if (!npin || pins[npin - 1] != pg) {
      sqlite3PagerRef(pg);
      pins[npin++] = pg;
    }
  }
  if (ms_table_seek(e->bt, e->r_users, uid, nid, ur) != SQLITE_OK) goto done;
  have_ur = 1;
  for (int i = 0; i < nid; i++) {
    if (!ur[i].found) goto done;
    MemPage *pg = ur[i].leaf;
    CellInfo info;
    pg->xParseCell(pg, findCell(pg, ur[i].ix), &info);
    u64 t[2];
    const u8 *d[2];
    if (info.nLocal != info.nPayload || rec_decode(info.pPayload, info.nPayload, 2, t, d) != 2) goto done;
    r[i].user = d[1];
    r[i].un = st_len(t[1]);
  }
  {
    char *p = WLIT(out, "{\"posts\":["), *end = out + cap;
    for (int i = 0; i < nid; i++) {
      i64 likes = ranged ? (i64)e->counts[ids[i] - lo] : count_likes(e, ids[i], ids[i], NULL);
      if (likes < 0 || !post_fits(&r[i], (size_t)(end - p) - 8)) goto done;
      if (i) *p++ = ',';
      p = w_post(p, ids[i], &r[i], likes);
    }
    *p++ = ']';
    *p++ = '}';
    *n = (size_t)(p - out);
    st = 200;
  }
done:
  if (have_ur) ms_release(ur, nid);
  for (int i = 0; i < npin; i++) sqlite3PagerUnref(pins[i]);
  return st;
}

static int direct_feed_one(astraea_eng *e, char *out, size_t cap, size_t *n) {
  BtCursor *cc = rcur(e, C_C), *cp = rcur(e, C_P);
  if (!cc || !cp) return err500(out, n);
  i64 ids[FEED_N];
  int nid = 0, res, rc;
  if (sqlite3BtreeFirst(cc, &res) != SQLITE_OK) return err500(out, n);
  if (!res) {
    do {
      u32 amt;
      const u8 *rec = idx_fetch(cc, &amt);
      u64 t[3];
      const u8 *d[3];
      int nf = rec ? rec_decode(rec, amt, 3, t, d) : -1;
      if (nf < 1 || !st_isint(t[nf - 1])) return err500(out, n);
      ids[nid++] = st_int(t[nf - 1], d[nf - 1]);
    } while (nid < FEED_N && (rc = sqlite3BtreeNext(cc, 0)) == SQLITE_OK);
  }
  i64 lo = INT64_MAX, hi = INT64_MIN;
  for (int i = 0; i < nid; i++) {
    if (ids[i] < lo) lo = ids[i];
    if (ids[i] > hi) hi = ids[i];
  }
  int ranged = nid > 0 && (u64)(hi - lo) < RANGE_MAX;
  if (ranged) {
    memset(e->counts, 0, sizeof(u32) * (size_t)(hi - lo + 1));
    if (count_likes(e, lo, hi, e->counts) < 0) return err500(out, n);
  }
  char *p = WLIT(out, "{\"posts\":["), *end = out + cap;
  int emitted = 0, i = 0, more = nid == FEED_N;
  i64 prev = INT64_MIN;
  while (emitted < FEED_N) {
    i64 id;
    if (i < nid) {
      id = ids[i++];
    } else {
      if (!more) break;
      rc = sqlite3BtreeNext(cc, 0);
      if (rc == SQLITE_DONE) break;
      if (rc) return err500(out, n);
      u32 amt;
      const u8 *rec = idx_fetch(cc, &amt);
      u64 t[3];
      const u8 *d[3];
      int nf = rec ? rec_decode(rec, amt, 3, t, d) : -1;
      if (nf < 1 || !st_isint(t[nf - 1])) return err500(out, n);
      id = st_int(t[nf - 1], d[nf - 1]);
    }
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
    if (!found) continue;
    prow_t r;
    int k = load_row(e, cp, &r);
    if (k < 0) return err500(out, n);
    if (k == 0) continue;
    i64 likes = ranged && id >= lo && id <= hi ? (i64)e->counts[id - lo] : count_likes(e, id, id, NULL);
    if (likes < 0) return err500(out, n);
    if (!post_fits(&r, (size_t)(end - p) - 8)) return err500(out, n);
    if (emitted) *p++ = ',';
    p = w_post(p, id, &r, likes);
    emitted++;
  }
  *p++ = ']';
  *p++ = '}';
  *n = (size_t)(p - out);
  return 200;
}

static int sql_feed_fallback(astraea_eng *e, char *out, size_t cap, size_t *n) {
  if (sql_txn(e)) return err500(out, n);
  sqlite3_stmt *s = e->s_feed;
  char *p = WLIT(out, "{\"posts\":["), *end = out + cap;
  int k = 0, rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    prow_t r;
    r.body = sqlite3_column_text(s, 1);
    r.bn = (u32)sqlite3_column_bytes(s, 1);
    r.at = sqlite3_column_text(s, 2);
    r.an = (u32)sqlite3_column_bytes(s, 2);
    r.user = sqlite3_column_text(s, 3);
    r.un = (u32)sqlite3_column_bytes(s, 3);
    if (!post_fits(&r, (size_t)(end - p) - 8)) { rc = SQLITE_NOMEM; break; }
    if (k++) *p++ = ',';
    p = w_post(p, sqlite3_column_int64(s, 0), &r, sqlite3_column_int64(s, 4));
  }
  sqlite3_reset(s);
  if (rc != SQLITE_DONE) return err500(out, n);
  *p++ = ']';
  *p++ = '}';
  *n = (size_t)(p - out);
  return 200;
}

static int direct_get_post(astraea_eng *e, i64 id, char *out, size_t cap, size_t *n) {
  BtCursor *cp = rcur(e, C_P);
  if (!cp) return -1;
  int res;
  if (sqlite3BtreeTableMoveto(cp, id, 0, &res) != SQLITE_OK) return -1;
  if (res != 0 || cp->eState != CURSOR_VALID) return err404(out, n);
  prow_t r;
  int k = load_row(e, cp, &r);
  if (k < 0) return -1;
  if (k == 0) return err404(out, n);
  if (!post_fits(&r, cap - 16)) return err500(out, n);
  i64 likes = count_likes(e, id, id, NULL);
  if (likes < 0) return -1;
  char *p = WLIT(out, "{\"post\":");
  p = w_post(p, id, &r, likes);
  *p++ = '}';
  *n = (size_t)(p - out);
  return 200;
}

static int sql_get_fallback(astraea_eng *e, i64 id, char *out, size_t cap, size_t *n) {
  if (sql_txn(e)) return err500(out, n);
  sqlite3_stmt *s = e->s_post;
  sqlite3_bind_int64(s, 1, id);
  int rc = sqlite3_step(s), st;
  if (rc == SQLITE_ROW) {
    prow_t r;
    r.body = sqlite3_column_text(s, 1);
    r.bn = (u32)sqlite3_column_bytes(s, 1);
    r.at = sqlite3_column_text(s, 2);
    r.an = (u32)sqlite3_column_bytes(s, 2);
    r.user = sqlite3_column_text(s, 3);
    r.un = (u32)sqlite3_column_bytes(s, 3);
    if (!post_fits(&r, cap - 16)) {
      st = err500(out, n);
    } else {
      char *p = WLIT(out, "{\"post\":");
      p = w_post(p, sqlite3_column_int64(s, 0), &r, sqlite3_column_int64(s, 4));
      *p++ = '}';
      *n = (size_t)(p - out);
      st = 200;
    }
  } else {
    st = rc == SQLITE_DONE ? err404(out, n) : err500(out, n);
  }
  sqlite3_reset(s);
  return st;
}

static const char *now_ts(astraea_eng *e) {
  int64_t ms = realtime_ms();
  if (ms != e->ts_ms) {
    e->ts_ms = ms;
    format_utc_ms(e->ts, ms);
  }
  return e->ts;
}

static int prep_stmt(astraea_eng *e, const char *sql, sqlite3_stmt **s) {
  return sqlite3_prepare_v3(e->db, sql, -1, SQLITE_PREPARE_PERSISTENT, s, NULL);
}

#define POSTQ_BASE "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count FROM posts p JOIN users u ON u.id = p.user_id "

astraea_eng *astraea_eng_open(const char *path, int cache_kib) {
  install_pcache_once();
  sqlite3_initialize();
  astraea_eng *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  e->ts_ms = INT64_MIN;
  e->path = strdup(path);
  if (!e->path) { free(e); return NULL; }

  const char *vfs = register_astraea_vfs();
  if (sqlite3_open_v2(path, &e->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs) != SQLITE_OK) {
    astraea_eng_close(e);
    return NULL;
  }
  sqlite3_busy_handler(e->db, busy_sleep_main, e);

  char sql[512];
  snprintf(sql, sizeof sql,
           "PRAGMA locking_mode=NORMAL; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
           "PRAGMA foreign_keys=ON; PRAGMA cache_size=-%d; PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY;"
           "PRAGMA wal_autocheckpoint=0; PRAGMA journal_size_limit=0;"
           "SELECT count(*) FROM sqlite_schema;",
           cache_kib > 0 ? cache_kib : 8192);
  if (sqlite3_exec(e->db, sql, NULL, NULL, NULL) != SQLITE_OK) {
    astraea_eng_close(e);
    return NULL;
  }
  sqlite3_wal_hook(e->db, wal_hook_cb, e);
  sqlite3_db_config(e->db, SQLITE_DBCONFIG_ENABLE_QPSG, 1, 0);

  sqlite3 *db = e->db;
  e->bt = db->aDb[0].pBt;
  e->pager = sqlite3BtreePager(e->bt);

  Table *tp = sqlite3FindTable(db, "posts", "main");
  Table *tu = sqlite3FindTable(db, "users", "main");
  Table *tl = sqlite3FindTable(db, "likes", "main");
  Index *ilp = sqlite3FindIndex(db, "likes_post_id_idx", "main");
  Index *ipc = sqlite3FindIndex(db, "posts_created_at_id_idx", "main");

  if (tp && tu && tl && ilp && ipc &&
      tp->nCol == 4 && tp->iPKey == 0 && tu->nCol == 3 && tu->iPKey == 0 && tl->nCol == 3 &&
      ilp->nKeyCol == 1 && ilp->aiColumn[0] == 1 &&
      ipc->nKeyCol == 2 && ipc->aiColumn[0] == 3 && ipc->aiColumn[1] == 0) {
    e->r_posts = tp->tnum;
    e->r_users = tu->tnum;
    e->r_likes = tl->tnum;
    e->r_lpid = ilp->tnum;
    e->r_pcat = ipc->tnum;
    Parse ps;
    memset(&ps, 0, sizeof ps);
    ps.db = db;
    e->ki_lpid = sqlite3KeyInfoOfIndex(&ps, ilp);
    e->ki_pcat = sqlite3KeyInfoOfIndex(&ps, ipc);
    if (e->ki_lpid && e->ki_pcat) {
      e->uk_lpid = sqlite3VdbeAllocUnpackedRecord(e->ki_lpid);
      int cursors_ok = e->uk_lpid != NULL;
      for (int i = 0; i < NRC; i++) {
        if (!(e->rc[i] = alloc_cursor())) cursors_ok = 0;
      }
      e->native_ok = cursors_ok;
    }
  }

  check_startup_invariant(e);

  if (ck_start(e) != 0) {
    astraea_eng_close(e);
    return NULL;
  }

  if (prep_stmt(e, "BEGIN", &e->s_begin) ||
      prep_stmt(e, "COMMIT", &e->s_commit) ||
      prep_stmt(e, "ROLLBACK", &e->s_rollback) ||
      prep_stmt(e, "SELECT 1", &e->s_health) ||
      prep_stmt(e, POSTQ_BASE "ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &e->s_feed) ||
      prep_stmt(e, POSTQ_BASE "WHERE p.id = ?1", &e->s_post) ||
      prep_stmt(e, "SELECT 1 FROM posts WHERE id = ?1", &e->s_exists) ||
      prep_stmt(e, "INSERT INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)", &e->s_ins) ||
      prep_stmt(e, "INSERT INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, ?3) ON CONFLICT (user_id, post_id) DO NOTHING", &e->s_like)) {
    astraea_eng_close(e);
    return NULL;
  }
  return e;
}

void astraea_eng_close(astraea_eng *e) {
  if (!e) return;
  ck_stop(e);
  sqlite3_stmt **ss[] = {
    &e->s_begin, &e->s_commit, &e->s_rollback, &e->s_health,
    &e->s_feed, &e->s_post, &e->s_exists, &e->s_ins, &e->s_like
  };
  for (size_t i = 0; i < sizeof ss / sizeof *ss; i++) {
    if (*ss[i]) sqlite3_finalize(*ss[i]);
  }
  for (int i = 0; i < NRC; i++) {
    if (e->rc[i]) sqlite3_free(e->rc[i]);
  }
  if (e->uk_lpid) sqlite3DbFree(e->db, e->uk_lpid);
  if (e->ki_lpid) sqlite3KeyInfoUnref(e->ki_lpid);
  if (e->ki_pcat) sqlite3KeyInfoUnref(e->ki_pcat);
  free(e->scratch[0]);
  free(e->scratch[1]);
  if (e->db) sqlite3_close(e->db);
  free(e->path);
  free(e);
}

int astraea_eng_writes_paused(astraea_eng *e) {
  if (!__atomic_load_n(&e->drain, __ATOMIC_ACQUIRE)) return 0;
  u32 mx, pend;
  wal_state(e, &mx, &pend);
  double held = mono_ms() - e->drain_t0;
  if (pend == 0 || held > CKPT_DRAIN_MAX_MS) {
    __atomic_store_n(&e->drain, 0, __ATOMIC_RELEASE);
    return 0;
  }
  return 1;
}

int astraea_eng_begin_batch(astraea_eng *e) {
  if (e->in_batch) return -1;
  e->in_batch = 1;
  e->failed = 0;
  return 0;
}

int astraea_eng_commit_batch(astraea_eng *e) {
  if (!e->in_batch) return -1;
  int wrote = e->txn == 2 || e->wrote_sql;
  int ok = !e->failed;
  close_cursors(e);
  if (e->sql_txn) {
    int rc = SQLITE_ERROR;
    if (ok) {
      rc = sqlite3_step(e->s_commit);
      sqlite3_reset(e->s_commit);
    }
    if (rc != SQLITE_DONE) {
      ok = 0;
      e->inv_ok = 0;
      sqlite3_step(e->s_rollback);
      sqlite3_reset(e->s_rollback);
    }
    if (e->db->autoCommit == 0) {
      ok = 0;
      e->inv_ok = 0;
      sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    }
  } else if (e->txn) {
    int rc = ok ? sqlite3BtreeCommit(e->bt) : SQLITE_ABORT;
    if (rc != SQLITE_OK) {
      ok = 0;
      sqlite3BtreeRollback(e->bt, SQLITE_OK, 0);
    }
  }
  if (ok && wrote) {
    sqlite3_file *wf = conn_wal_fd(e->db);
    if (wf && wf->pMethods == &aw_io && ((aw_file_t *)wf)->n) {
      if (aw_flush((aw_file_t *)wf) != SQLITE_OK) ok = 0;
    }
  }
  e->wrote_sql = 0;
  e->txn = 0;
  e->sql_txn = 0;
  e->in_batch = 0;
  e->failed = 0;
  return ok ? 0 : -1;
}

void astraea_eng_after_batch(astraea_eng *e) {
  if (e->in_batch) return;
  u32 mx, pend;
  wal_state(e, &mx, &pend);
  if (pend >= CKPT_HARD_FRAMES) {
    int nlog = 0, nck = 0;
    if (sqlite3_wal_checkpoint_v2(e->db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &nck) == SQLITE_OK) {
      if (nck == nlog) wal_drop_pages(conn_wal_fd(e->db), 0, (u32)nck);
    }
    return;
  }
  if (!__atomic_load_n(&e->drain, __ATOMIC_ACQUIRE) &&
      mx >= CKPT_RESTART_FRAMES && pend > 0 &&
      (pend <= CKPT_DRAIN_FRAMES || mx >= CKPT_RESTART_FRAMES + CKPT_RESTART_FRAMES / 4)) {
    e->drain_t0 = mono_ms();
    __atomic_store_n(&e->drain, 1, __ATOMIC_RELEASE);
  }
  if (__atomic_load_n(&e->drain, __ATOMIC_ACQUIRE) || pend >= CKPT_WAKE_FRAMES || (mx >= CKPT_RESTART_FRAMES && pend > 0)) {
    ck_signal(e);
  }
}

int astraea_eng_feed(astraea_eng *e, char *out, size_t cap, size_t *out_len) {
  if (!e->in_batch || cap < 64) return err500(out, out_len);
  if (e->native_ok) {
    if (e->inv_ok) {
      int st = feed_tail_batch(e, out, cap, out_len);
      if (st >= 0) return st;
    }
    int st = feed_idx_batch(e, out, cap, out_len);
    if (st >= 0) return st;
    return direct_feed_one(e, out, cap, out_len);
  }
  return sql_feed_fallback(e, out, cap, out_len);
}

int astraea_eng_get_post(astraea_eng *e, int64_t id, char *out, size_t cap, size_t *out_len) {
  if (!e->in_batch || cap < 64) return err500(out, out_len);
  if (e->native_ok) {
    int st = direct_get_post(e, id, out, cap, out_len);
    if (st >= 0) return st;
  }
  return sql_get_fallback(e, id, out, cap, out_len);
}

int astraea_eng_health(astraea_eng *e, int64_t uptime_s, char *out, size_t cap, size_t *out_len) {
  if (e->in_batch && e->txn && sql_txn(e)) goto bad;
  int rc = sqlite3_step(e->s_health);
  int v = rc == SQLITE_ROW ? sqlite3_column_int(e->s_health, 0) : 0;
  sqlite3_reset(e->s_health);
  if (rc == SQLITE_ROW && v == 1 && cap >= 64) {
    char *p = WLIT(out, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    p = w_i64(p, uptime_s);
    *p++ = '}';
    *out_len = (size_t)(p - out);
    return 200;
  }
bad:;
  const char *m = sqlite3_errmsg(e->db);
  size_t ml = strlen(m);
  if (ml > 200) ml = 200;
  if (cap < 80 + 6 * ml) return err500(out, out_len);
  char *p = WLIT(out, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
  p = w_str(p, m, ml);
  *p++ = '}';
  *out_len = (size_t)(p - out);
  return 503;
}

int astraea_eng_like(astraea_eng *e, int64_t uid, int64_t pid, char *out, size_t cap, size_t *out_len) {
  if (!e->in_batch || cap < 96 || e->failed) return err500(out, out_len);
  if (writer_upgrade(e) || sql_txn(e)) {
    e->failed = 1;
    return err500(out, out_len);
  }
  int exists = 0;
  if (e->native_ok) {
    BtCursor *cp = rcur(e, C_P);
    int res = 0;
    if (cp && sqlite3BtreeTableMoveto(cp, pid, 0, &res) == SQLITE_OK) {
      exists = (res == 0 && cp->eState == CURSOR_VALID) ? 1 : 0;
    } else {
      exists = -1;
    }
  } else {
    sqlite3_bind_int64(e->s_exists, 1, pid);
    int rc = sqlite3_step(e->s_exists);
    sqlite3_reset(e->s_exists);
    exists = rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
  }
  if (exists < 0) {
    e->failed = 1;
    return err500(out, out_len);
  }
  if (exists == 0) return err404(out, out_len);

  const char *ts = now_ts(e);
  sqlite3_stmt *s = e->s_like;
  sqlite3_bind_int64(s, 1, uid);
  sqlite3_bind_int64(s, 2, pid);
  sqlite3_bind_text(s, 3, ts, 24, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  e->wrote_sql = 1;
  if (rc != SQLITE_DONE) {
    if ((rc & 0xff) != SQLITE_CONSTRAINT && rc != SQLITE_TOOBIG) e->failed = 1;
    return err500(out, out_len);
  }
  int already = sqlite3_changes(e->db) == 0;
  char *p = WLIT(out, "{\"liked\":true,\"already_liked\":");
  p = already ? WLIT(p, "true") : WLIT(p, "false");
  p = WLIT(p, ",\"post_id\":");
  p = w_i64(p, pid);
  *p++ = '}';
  *out_len = (size_t)(p - out);
  return already ? 200 : 201;
}

int astraea_eng_create(astraea_eng *e, int64_t uid, const char *user, size_t user_len,
                       const char *body, size_t body_len, char *out, size_t cap, size_t *out_len) {
  if (!e->in_batch || e->failed || cap < 160 + 6 * (body_len + user_len)) return err500(out, out_len);
  if (writer_upgrade(e) || sql_txn(e)) {
    e->failed = 1;
    return err500(out, out_len);
  }
  const char *ts = now_ts(e);
  i64 prev_id = 0;
  u8 prev_ts[64];
  u32 prev_tl = 0;
  int have_prev = 0, user_ok = 0;

  if (e->inv_ok && e->native_ok) {
    BtCursor *cp = rcur(e, C_P);
    int res = 0;
    if (cp && sqlite3BtreeLast(cp, &res) == SQLITE_OK) {
      if (!res) {
        prev_id = sqlite3BtreeIntegerKey(cp);
        u32 sz = sqlite3BtreePayloadSize(cp), amt = 0;
        const u8 *rec = sqlite3BtreePayloadFetch(cp, &amt);
        u64 t[4];
        const u8 *d[4];
        if (amt >= sz && rec_decode(rec, sz, 4, t, d) == 4 && st_istext(t[3]) && st_len(t[3]) <= sizeof prev_ts) {
          prev_tl = st_len(t[3]);
          memcpy(prev_ts, d[3], prev_tl);
          have_prev = 1;
        } else {
          e->inv_ok = 0;
        }
      }
      sqlite3BtreeCloseCursor(cp);
      e->rc_open &= ~(1u << C_P);
    } else {
      e->inv_ok = 0;
    }
    BtCursor *cu = rcur(e, C_U);
    int ures = 0;
    if (cu && sqlite3BtreeTableMoveto(cu, uid, 0, &ures) == SQLITE_OK && ures == 0 && cu->eState == CURSOR_VALID) {
      user_ok = 1;
    }
  }

  sqlite3_stmt *s = e->s_ins;
  sqlite3_bind_int64(s, 1, uid);
  sqlite3_bind_text(s, 2, body, (int)body_len, SQLITE_STATIC);
  sqlite3_bind_text(s, 3, ts, 24, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  sqlite3_clear_bindings(s);
  e->wrote_sql = 1;
  if (rc != SQLITE_DONE) {
    if ((rc & 0xff) != SQLITE_CONSTRAINT && rc != SQLITE_TOOBIG) e->failed = 1;
    return err500(out, out_len);
  }
  i64 id = sqlite3_last_insert_rowid(e->db);
  if (e->inv_ok) {
    if (!(id == prev_id + 1 && (!have_prev || text_le(prev_ts, prev_tl, (const u8 *)ts, 24)) && user_ok)) {
      e->inv_ok = 0;
    }
  }

  char *p = WLIT(out, "{\"post\":{\"id\":");
  p = w_i64(p, id);
  p = WLIT(p, ",\"body\":");
  p = w_str(p, body, body_len);
  p = WLIT(p, ",\"created_at\":\"");
  p = w_lit(p, ts, 24);
  *p++ = '"';
  p = WLIT(p, ",\"author\":");
  p = w_str(p, user, user_len);
  p = WLIT(p, ",\"like_count\":0}}");
  *out_len = (size_t)(p - out);
  return 201;
}

int astraea_make_train_db(const char *path, int users, int posts, int likes) {
  install_pcache_once();
  sqlite3_initialize();
  unlink(path);
  sqlite3 *c = NULL;
  if (sqlite3_open_v2(path, &c, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) return -1;
  const char *schema =
      "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;"
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
      "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
      "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), PRIMARY KEY (user_id, post_id));"
      "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
      "CREATE INDEX posts_user_id_idx ON posts (user_id);"
      "CREATE INDEX likes_post_id_idx ON likes (post_id);"
      "BEGIN;";
  if (sqlite3_exec(c, schema, NULL, NULL, NULL) != SQLITE_OK) {
    fprintf(stderr, "train schema: %s\n", sqlite3_errmsg(c));
    sqlite3_close(c);
    return -1;
  }
  sqlite3_stmt *su = NULL, *sp = NULL, *sl = NULL;
  sqlite3_prepare_v2(c, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1, &su, NULL);
  sqlite3_prepare_v2(c, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)", -1, &sp, NULL);
  sqlite3_prepare_v2(c, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)", -1, &sl, NULL);
  unsigned rng = 0x13579bdfu;
  char buf[256], ts[24];
  for (int i = 1; i <= users; i++) {
    int n = snprintf(buf, sizeof buf, "golden_ember_%d", i);
    sqlite3_bind_int64(su, 1, i);
    sqlite3_bind_text(su, 2, buf, n, SQLITE_TRANSIENT);
    sqlite3_step(su);
    sqlite3_reset(su);
  }
  int64_t base_ms = 1735689600000LL;
  for (int i = 1; i <= posts; i++) {
    rng = rng * 1664525u + 1013904223u;
    int uid = 1 + (int)(rng % (unsigned)users);
    int n = snprintf(buf, sizeof buf, "Training post %d by user %d testing B-tree tail and range scan.", i, uid);
    format_utc_ms(ts, base_ms + (int64_t)i * 1000LL);
    sqlite3_bind_int64(sp, 1, i);
    sqlite3_bind_int64(sp, 2, uid);
    sqlite3_bind_text(sp, 3, buf, n, SQLITE_TRANSIENT);
    sqlite3_bind_text(sp, 4, ts, 24, SQLITE_TRANSIENT);
    sqlite3_step(sp);
    sqlite3_reset(sp);
  }
  for (int i = 0; i < likes; i++) {
    rng = rng * 1664525u + 1013904223u;
    int uid = 1 + (int)(rng % (unsigned)users);
    rng = rng * 1664525u + 1013904223u;
    int pid = 1 + (int)(rng % (unsigned)posts);
    sqlite3_bind_int64(sl, 1, uid);
    sqlite3_bind_int64(sl, 2, pid);
    sqlite3_step(sl);
    sqlite3_reset(sl);
  }
  sqlite3_finalize(su);
  sqlite3_finalize(sp);
  sqlite3_finalize(sl);
  if (sqlite3_exec(c, "COMMIT; ANALYZE; PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_close(c);
    return -1;
  }
  sqlite3_close(c);
  return 0;
}

#if defined(__GCOV__) || defined(__GNUC__)
extern void __gcov_dump(void) __attribute__((weak));
#endif
void astraea_gcov_flush(void) {
#if defined(__GCOV__) || defined(__GNUC__)
  if (__gcov_dump) __gcov_dump();
#endif
}
