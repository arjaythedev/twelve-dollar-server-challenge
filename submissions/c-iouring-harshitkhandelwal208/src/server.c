// Feed API: high-performance io_uring event loop, SQLite in-process, group commit per batch.
// Pure C11, single translation unit with SQLite amalgamation, zero-allocation request pipeline.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <immintrin.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "sqlite3.c"  // Direct inclusion: enables compiler cross-inlining into B-tree routines

#define MAXREQ (64 * 1024)       // header + body cap per request
#define BATCH_OUT (4 << 20)      // response bytes held per batch until commit
#define MAXSEG 65536             // responses per batch
#define NBUF 2048                // provided receive buffers per ring
#define BSZ 1024                 // buffer size per receive buffer
#ifndef CACHE_KB
#define CACHE_KB 32768           // 32 MB SQLite page cache (matches kensac)
#endif
#define STR2(x) #x
#define STR(x) STR2(x)
#define MAXRINGS 16              // Up to 16 rings (matches kensac)
#define NLISTEN 8                // SO_REUSEPORT listeners sharing the port
#define ACCEPTS 512              // In-flight accepts (matches kensac ACCEPT_DEPTH 512)

// ---------- PCACHE2 Wrapper (Skip commit truncate when no page past EOF is cached) ----------
#define LOCK_BYTE_OFFSET 0x40000000u

typedef struct {
  sqlite3_pcache *base;
  unsigned max_key;
  unsigned lock_page;
  unsigned n_parked;
} wcache_t;

static sqlite3_pcache_methods2 pc_base;

static int wc_init(void *a) { (void)a; return pc_base.xInit(pc_base.pArg); }
static void wc_shutdown(void *a) { (void)a; pc_base.xShutdown(pc_base.pArg); }
static sqlite3_pcache *wc_create(int sz, int extra, int purgeable) {
  wcache_t *w = sqlite3_malloc(sizeof *w);
  if (!w) return NULL;
  w->base = pc_base.xCreate(sz, extra, purgeable);
  if (!w->base) { sqlite3_free(w); return NULL; }
  w->max_key = 0;
  w->lock_page = LOCK_BYTE_OFFSET / (unsigned)sz + 1;
  w->n_parked = 0;
  return (sqlite3_pcache *)w;
}
static void wc_cachesize(sqlite3_pcache *p, int n) { pc_base.xCachesize(((wcache_t *)p)->base, n); }
static int wc_pagecount(sqlite3_pcache *p) { return pc_base.xPagecount(((wcache_t *)p)->base); }
static sqlite3_pcache_page *wc_fetch(sqlite3_pcache *p, unsigned key, int create) {
  wcache_t *w = (wcache_t *)p;
  sqlite3_pcache_page *pg = pc_base.xFetch(w->base, key, create);
  if (pg && create) {
    if (key == w->lock_page) w->n_parked++;
    else if (key > w->max_key) w->max_key = key;
  }
  return pg;
}
static void wc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) { pc_base.xUnpin(((wcache_t *)p)->base, pg, discard); }
static void wc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned from, unsigned to) {
  wcache_t *w = (wcache_t *)p;
  pc_base.xRekey(w->base, pg, from, to);
  if (from == w->lock_page && w->n_parked) w->n_parked--;
  if (to == w->lock_page) w->n_parked++;
  else if (to > w->max_key) w->max_key = to;
}
static void wc_truncate(sqlite3_pcache *p, unsigned limit) {
  wcache_t *w = (wcache_t *)p;
  if (!w->n_parked && w->max_key < limit) return; // Shortcut: skip full hash-table walk!
  pc_base.xTruncate(w->base, limit);
  if (limit > 0 && w->max_key >= limit) w->max_key = limit - 1;
  if (w->lock_page >= limit) w->n_parked = 0;
}
static void wc_destroy(sqlite3_pcache *p) {
  wcache_t *w = (wcache_t *)p;
  pc_base.xDestroy(w->base);
  sqlite3_free(w);
}
static void wc_shrink(sqlite3_pcache *p) { pc_base.xShrink(((wcache_t *)p)->base); }

static int pcache_install(void) {
  if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pc_base) != SQLITE_OK) return -1;
  static sqlite3_pcache_methods2 m;
  m = pc_base;
  m.pArg = NULL;
  m.xInit = wc_init; m.xShutdown = wc_shutdown; m.xCreate = wc_create;
  m.xCachesize = wc_cachesize; m.xPagecount = wc_pagecount; m.xFetch = wc_fetch;
  m.xUnpin = wc_unpin; m.xRekey = wc_rekey; m.xTruncate = wc_truncate;
  m.xDestroy = wc_destroy; m.xShrink = wc_shrink;
  return sqlite3_config(SQLITE_CONFIG_PCACHE2, &m) == SQLITE_OK ? 0 : -1;
}

static void pagecache_arena(int cache_kib) {
  int hdr = 0;
  if (sqlite3_config(SQLITE_CONFIG_PCACHE_HDRSZ, &hdr) != SQLITE_OK) return;
  int sz = (4096 + hdr + 63) & ~63;
  int n = cache_kib / 4 + 64;
  size_t bytes = ((size_t)sz * n + 0x1fffff) & ~0x1fffff;
  void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return;
#ifdef MADV_HUGEPAGE
  madvise(m, bytes, MADV_HUGEPAGE);
#endif
  for (size_t i = 0; i < bytes; i += 4096) ((volatile char *)m)[i] = 0;
  if (sqlite3_config(SQLITE_CONFIG_PAGECACHE, m, sz, n) != SQLITE_OK) munmap(m, bytes);
}

// ---------- Background Checkpoint Coordination ----------
#define CKPT_FRAMES 4096         // WAL threshold to trigger background bulk checkpoint
#define CKPT_FORCE 65536         // Last-resort inline checkpoint threshold
#define HOLD_MAX_MS 250          // Maximum duration to hold POST writes during tail checkpoint
#define CKPT_NICE 19

static sqlite3 *db, *ckdb;
enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };
static int ck_state;             // Guarded by ck_mu; polled atomically by event loop
static int ck_full;              // 1 if last checkpoint pass fully caught up
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static volatile int writes_held = 0;
static struct timespec hold_t0;
static int wal_frames = 0;

static void ck_set(int s) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, s, __ATOMIC_RELEASE);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

static int tail_restart(sqlite3 *c) {
  static int uv = 0;
  char sql[32];
  snprintf(sql, sizeof sql, "PRAGMA user_version=%d", uv ^= 1);
  return sqlite3_exec(c, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static void *ck_main(void *arg) {
  (void)arg;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), CKPT_NICE);
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    while (ck_state != CK_BULK && ck_state != CK_TAIL) pthread_cond_wait(&ck_cv, &ck_mu);
    int st = ck_state;
    pthread_mutex_unlock(&ck_mu);

    int nlog = -1, ndone = -2;
    int rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);

    pthread_mutex_lock(&ck_mu);
    ck_full = (rc == SQLITE_OK && nlog == ndone && nlog > 0);
    if (ck_full && st == CK_TAIL) tail_restart(ckdb);
    __atomic_store_n(&ck_state, st + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&ck_mu);
  }
  return NULL;
}

static inline long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

static void td_tick(void) {
  switch (__atomic_load_n(&ck_state, __ATOMIC_ACQUIRE)) {
    case CK_IDLE:
      if (wal_frames >= CKPT_FORCE) {
        int nlog = 0, ndone = 0;
        if (sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK && nlog == ndone)
          wal_frames = 0;
      } else if (wal_frames >= CKPT_FRAMES) {
        ck_set(CK_BULK);
      }
      break;
    case CK_BULK_DONE:
      writes_held = 1;
      clock_gettime(CLOCK_MONOTONIC, &hold_t0);
      ck_set(CK_TAIL);
      break;
    case CK_TAIL_DONE:
      if (ck_full) wal_frames = 0;
      writes_held = 0;
      ck_set(CK_IDLE);
      break;
    default:
      if (writes_held && ms_since(&hold_t0) > HOLD_MAX_MS) {
        writes_held = 0;
      }
      break;
  }
}

static int wal_hook(void *arg, sqlite3 *d, const char *name, int n) {
  (void)arg; (void)d; (void)name;
  wal_frames = n;
  return SQLITE_OK;
}

// ---------- Response in Flight ----------
typedef struct { uint64_t who; uint32_t len, off, cap; char d[]; } sop_t;

typedef struct {
  char *in; uint32_t inlen, incap;
  sop_t *q;
  sop_t *out;
  uint32_t gen;
  uint8_t open, closing, sending, eof, sent100, queued, ready, held;
} conn_t;

typedef struct {
  struct io_uring r;
  conn_t *c;
  uint32_t n, pend, reads;
  struct io_uring_buf_ring *br;
  char *bufs;
} ring_t;

static ring_t R[MAXRINGS];
static int nrings = 0, active_rings = 1, lfd[NLISTEN];
static uint32_t tsize;
static sqlite3_stmt *st_feed, *st_post, *st_ins, *st_like, *st_one, *st_begin, *st_commit, *st_rollback;
static time_t started, now_s;

enum { T_SEND, T_ACCEPT, T_RECV, T_NOP, T_READ };
#define RECV_UD(k, slot, gen) (T_RECV | (uint64_t)(k) << 3 | (uint64_t)(slot) << 8 | (uint64_t)(gen) << 32)

static inline void clamp_socket(int fd) {
  int snd = 16384;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
  int rcv = 2048;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof(one));
}

// ---------- Batch Output Arena ----------
static char *arena; static size_t alen;
typedef struct { uint64_t who; uint32_t off, len; } seg_t;
static seg_t segs[MAXSEG]; static int nsegs;
static uint64_t *deferred, *held, *eofs, *limbo;
static int ndef, nheld, neof, nlimbo, cap_def, cap_held, cap_eof, cap_limbo;
static int arena_full, in_txn, txn_writes, batch_failed;

static inline void put(const void *p, size_t n) { memcpy(arena + alen, p, n); alen += n; }
#define PUTS(s) put(s, sizeof(s) - 1)

static const char digits[200] =
  "0001020304050607080910111213141516171819"
  "2021222324252627282930313233343536373839"
  "4041424344454647484950515253545556575859"
  "6061626364656667686970717273747576777879"
  "8081828384858687888990919293949596979899";

static inline void putu(uint64_t v) {
  char b[24]; int i = 24;
  while (v >= 100) {
    uint32_t r = (uint32_t)(v % 100);
    v /= 100;
    i -= 2;
    b[i] = digits[2 * r];
    b[i + 1] = digits[2 * r + 1];
  }
  if (v >= 10) {
    i -= 2;
    b[i] = digits[2 * v];
    b[i + 1] = digits[2 * v + 1];
  } else {
    b[--i] = '0' + (char)v;
  }
  put(b + i, 24 - i);
}

static const char hexd[] = "0123456789abcdef";
static void putjs(const unsigned char *s, size_t n) {
  size_t i = 0;
#if defined(__AVX2__)
  const __m256i q = _mm256_set1_epi8('"'), b = _mm256_set1_epi8('\\'), c = _mm256_set1_epi8(0x1f);
  for (; i + 32 <= n; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
    __m256i m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, b)),
                                _mm256_cmpeq_epi8(_mm256_max_epu8(v, c), c));
    if (_mm256_movemask_epi8(m)) break;
  }
#endif
  for (; i + 8 <= n; i += 8) {
    uint64_t v; memcpy(&v, s + i, 8);
    #define LO8 0x0101010101010101ULL
    #define HI8 0x8080808080808080ULL
    uint64_t qq = v ^ (LO8 * '"'), bb = v ^ (LO8 * '\\');
    if (((v - LO8 * 0x20) | (qq - LO8) | (bb - LO8)) & ~v & HI8) break;
  }
  for (; i < n; i++) {
    unsigned char ch = s[i];
    if (ch < 0x20 || ch == '"' || ch == '\\') break;
  }
  put(s, i);
  if (i == n) return;
  size_t run = i;
  for (; i < n; i++) {
    unsigned char ch = s[i];
    if (ch >= 0x20 && ch != '"' && ch != '\\') continue;
    put(s + run, i - run); run = i + 1;
    char e[6] = {'\\', 0};
    switch (ch) {
      case '"': e[1] = '"'; put(e, 2); break;
      case '\\': e[1] = '\\'; put(e, 2); break;
      case '\n': e[1] = 'n'; put(e, 2); break;
      case '\r': e[1] = 'r'; put(e, 2); break;
      case '\t': e[1] = 't'; put(e, 2); break;
      default: e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hexd[ch >> 4]; e[5] = hexd[ch & 15]; put(e, 6);
    }
  }
  put(s + run, n - run);
}

static size_t body_start, cl_pos;
static void begin_resp(int status, int close) {
  switch (status) {
    case 200: PUTS("HTTP/1.1 200 OK\r\n"); break;
    case 201: PUTS("HTTP/1.1 201 Created\r\n"); break;
    case 400: PUTS("HTTP/1.1 400 Bad Request\r\n"); break;
    case 401: PUTS("HTTP/1.1 401 Unauthorized\r\n"); break;
    case 404: PUTS("HTTP/1.1 404 Not Found\r\n"); break;
    case 503: PUTS("HTTP/1.1 503 Service Unavailable\r\n"); break;
    default: PUTS("HTTP/1.1 500 Internal Server Error\r\n"); break;
  }
  if (close) PUTS("Connection: close\r\n");
  PUTS("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
  cl_pos = alen - 10; body_start = alen;
}

static void finish(void) {
  size_t n = alen - body_start; char *p = arena + cl_pos + 6;
  do { *--p = '0' + n % 10; n /= 10; } while (n);
}

static void err(int status, const char *msg, int close) {
  begin_resp(status, close); PUTS("{\"error\":\""); put(msg, strlen(msg)); PUTS("\"}"); finish();
}

// ---------- SHA-256 / HMAC ----------
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; uint32_t bl; } sha_t;
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t *h, const unsigned char *p) {
  uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
  for (int i = 0; i < 16; i++) {
    uint32_t val; memcpy(&val, p + 4 * i, 4);
    w[i] = __builtin_bswap32(val);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
  for (int i = 0; i < 64; i++) {
    t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha_init(sha_t *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, 32); s->n = 0; s->bl = 0;
}

static void sha_upd(sha_t *s, const unsigned char *p, size_t n) {
  s->n += n;
  if (s->bl) { size_t t = 64 - s->bl; if (t > n) t = n; memcpy(s->b + s->bl, p, t); s->bl += t; p += t; n -= t; if (s->bl == 64) { sha_block(s->h, s->b); s->bl = 0; } }
  for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
  if (n) { memcpy(s->b, p, n); s->bl = n; }
}

static void sha_fin(sha_t *s, unsigned char *out) {
  uint64_t bits = s->n * 8; unsigned char pad = 0x80, z[64] = {0}, len[8];
  sha_upd(s, &pad, 1);
  sha_upd(s, z, (s->bl <= 56 ? 56 : 120) - s->bl);
  for (int i = 0; i < 8; i++) len[i] = bits >> (56 - 8 * i);
  sha_upd(s, len, 8);
  for (int i = 0; i < 8; i++) { out[4*i] = s->h[i] >> 24; out[4*i+1] = s->h[i] >> 16; out[4*i+2] = s->h[i] >> 8; out[4*i+3] = s->h[i]; }
}

static sha_t hm_in, hm_out;
static void hmac_key(const char *key) {
  unsigned char k[64] = {0}, pad[64]; size_t kl = strlen(key);
  if (kl > 64) { sha_t s; sha_init(&s); sha_upd(&s, (const unsigned char *)key, kl); sha_fin(&s, k); } else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha_init(&hm_in); sha_upd(&hm_in, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha_init(&hm_out); sha_upd(&hm_out, pad, 64);
}

static void hmac(const unsigned char *m, size_t n, unsigned char *out) {
  sha_t s = hm_in; unsigned char ih[32];
  sha_upd(&s, m, n); sha_fin(&s, ih);
  s = hm_out; sha_upd(&s, ih, 32); sha_fin(&s, out);
}

// ---------- base64url ----------
static signed char b64t[256];
static int b64dec(const char *s, size_t n, unsigned char *out) {
  if (n % 4 == 1) return -1;
  uint32_t acc = 0; int bits = 0, o = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64t[(unsigned char)s[i]]; if (v < 0) return -1;
    acc = acc << 6 | v; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = acc >> bits; }
  }
  return o;
}

static inline int b64dec43(const char *s, unsigned char *out) {
  for (int i = 0; i < 10; i++, s += 4, out += 3) {
    int v0 = b64t[(unsigned char)s[0]], v1 = b64t[(unsigned char)s[1]];
    int v2 = b64t[(unsigned char)s[2]], v3 = b64t[(unsigned char)s[3]];
    if ((v0 | v1 | v2 | v3) < 0) return -1;
    uint32_t val = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
    out[0] = val >> 16; out[1] = val >> 8; out[2] = val;
  }
  int v0 = b64t[(unsigned char)s[0]], v1 = b64t[(unsigned char)s[1]], v2 = b64t[(unsigned char)s[2]];
  if ((v0 | v1 | v2) < 0) return -1;
  uint32_t val = ((uint32_t)v0 << 12) | ((uint32_t)v1 << 6) | (uint32_t)v2;
  out[0] = val >> 10; out[1] = val >> 2;
  return 32;
}

// ---------- JSON (Strict RFC 8259) ----------
typedef struct { const char *p, *e; } js_t;
static inline void ws(js_t *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int utf8_len(const unsigned char *p, const unsigned char *e) {
  unsigned c = p[0];
  if (c < 0x80) return 1;
  if (c >= 0xc2 && c <= 0xdf) return e - p >= 2 && (p[1] & 0xc0) == 0x80 ? 2 : 0;
  if (c >= 0xe0 && c <= 0xef) {
    if (e - p < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f)) return 0;
    return 3;
  }
  if (c >= 0xf0 && c <= 0xf4) {
    if (e - p < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80) return 0;
    if ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f)) return 0;
    return 4;
  }
  return 0;
}

static long jstr(js_t *j, char *out, size_t cap) {
  const unsigned char *p = (const unsigned char *)j->p + 1, *e = (const unsigned char *)j->e; size_t o = 0; int over = 0;
  while (p < e) {
    unsigned c = *p;
    if (c == '"') { j->p = (const char *)p + 1; return over ? -2 : (long)o; }
    if (c < 0x20) return -1;
    if (c == '\\') {
      if (++p >= e) return -1;
      char ch;
      switch (*p) {
        case '"': ch = '"'; break; case '\\': ch = '\\'; break; case '/': ch = '/'; break;
        case 'b': ch = '\b'; break; case 'f': ch = '\f'; break; case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break; case 't': ch = '\t'; break;
        case 'u': {
          if (e - p < 5) return -1;
          int a = hexv(p[1]), b = hexv(p[2]), c2 = hexv(p[3]), d = hexv(p[4]);
          if ((a | b | c2 | d) < 0) return -1;
          uint32_t cp = a << 12 | b << 8 | c2 << 4 | d; p += 4;
          if (cp >= 0xd800 && cp <= 0xdbff && e - p >= 7 && p[1] == '\\' && p[2] == 'u') {
            int a2 = hexv(p[3]), b2 = hexv(p[4]), c3 = hexv(p[5]), d2 = hexv(p[6]);
            if ((a2 | b2 | c3 | d2) < 0) return -1;
            uint32_t lo = a2 << 12 | b2 << 8 | c3 << 4 | d2;
            if (lo >= 0xdc00 && lo <= 0xdfff) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
          }
          if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
          char u[4]; int ul;
          if (cp < 0x80) { u[0] = cp; ul = 1; }
          else if (cp < 0x800) { u[0] = 0xc0 | cp >> 6; u[1] = 0x80 | (cp & 63); ul = 2; }
          else if (cp < 0x10000) { u[0] = 0xe0 | cp >> 12; u[1] = 0x80 | (cp >> 6 & 63); u[2] = 0x80 | (cp & 63); ul = 3; }
          else { u[0] = 0xf0 | cp >> 18; u[1] = 0x80 | (cp >> 12 & 63); u[2] = 0x80 | (cp >> 6 & 63); u[3] = 0x80 | (cp & 63); ul = 4; }
          if (out && o + ul > cap) { out = NULL; over = 1; }
          if (out) memcpy(out + o, u, ul);
          o += ul; p++; continue;
        }
        default: return -1;
      }
      if (out && o >= cap) { out = NULL; over = 1; }
      if (out) out[o] = ch;
      o++; p++; continue;
    }
    int l = utf8_len(p, e); if (!l) return -1;
    if (out && o + l > cap) { out = NULL; over = 1; }
    if (out) memcpy(out + o, p, l);
    o += l; p += l;
  }
  return -1;
}

static int jnumber(js_t *j) {
  const char *p = j->p, *e = j->e;
  if (p < e && *p == '-') p++;
  if (p >= e) return 0;
  if (*p == '0') p++; else if (*p >= '1' && *p <= '9') while (p < e && *p >= '0' && *p <= '9') p++; else return 0;
  if (p < e && *p == '.') { p++; if (p >= e || *p < '0' || *p > '9') return 0; while (p < e && *p >= '0' && *p <= '9') p++; }
  if (p < e && (*p == 'e' || *p == 'E')) { p++; if (p < e && (*p == '+' || *p == '-')) p++; if (p >= e || *p < '0' || *p > '9') return 0; while (p < e && *p >= '0' && *p <= '9') p++; }
  j->p = p; return 1;
}

static int jlit(js_t *j, const char *s, size_t n) { if ((size_t)(j->e - j->p) < n || memcmp(j->p, s, n)) return 0; j->p += n; return 1; }

static int jvalue(js_t *j, int depth) {
  if (depth > 64) return 0;
  ws(j); if (j->p >= j->e) return 0;
  switch (*j->p) {
    case '"': return jstr(j, NULL, 0) >= 0;
    case '{':
      j->p++; ws(j);
      if (j->p < j->e && *j->p == '}') { j->p++; return 1; }
      for (;;) {
        ws(j); if (j->p >= j->e || *j->p != '"' || jstr(j, NULL, 0) < 0) return 0;
        ws(j); if (j->p >= j->e || *j->p++ != ':') return 0;
        if (!jvalue(j, depth + 1)) return 0;
        ws(j); if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return 1; }
        return 0;
      }
    case '[':
      j->p++; ws(j);
      if (j->p < j->e && *j->p == ']') { j->p++; return 1; }
      for (;;) {
        if (!jvalue(j, depth + 1)) return 0;
        ws(j); if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return 1; }
        return 0;
      }
    case 't': return jlit(j, "true", 4);
    case 'f': return jlit(j, "false", 5);
    case 'n': return jlit(j, "null", 4);
    default: return jnumber(j);
  }
}

typedef struct { const char *key; const char *v, *ve; } want_t;
static int jdoc(const char *s, size_t n, want_t *w, int nw) {
  js_t j = {s, s + n};
  for (int i = 0; i < nw; i++) w[i].v = NULL;
  ws(&j);
  if (j.p < j.e && *j.p == '{') {
    j.p++; ws(&j);
    if (j.p < j.e && *j.p == '}') j.p++;
    else for (;;) {
      char kb[32]; ws(&j);
      if (j.p >= j.e || *j.p != '"') return 0;
      long kl = jstr(&j, kb, sizeof kb);
      if (kl == -1) return 0;
      ws(&j); if (j.p >= j.e || *j.p++ != ':') return 0;
      ws(&j); const char *vs = j.p;
      if (!jvalue(&j, 1)) return 0;
      if (kl >= 0) for (int i = 0; i < nw; i++) if ((size_t)kl == strlen(w[i].key) && !memcmp(kb, w[i].key, kl)) { w[i].v = vs; w[i].ve = j.p; }
      ws(&j); if (j.p >= j.e) return 0;
      if (*j.p == ',') { j.p++; continue; }
      if (*j.p == '}') { j.p++; break; }
      return 0;
    }
  } else if (!jvalue(&j, 0)) return 0;
  ws(&j);
  return j.p == j.e;
}

static inline int parse_jwt_num(const char *p, const char *e, double *out) {
  if (p >= e) return 0;
  int neg = 0;
  if (*p == '-') { neg = 1; p++; }
  if (p >= e || *p < '0' || *p > '9') return 0;
  int64_t v = 0;
  while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  double d = (double)v;
  if (p < e && *p == '.') {
    p++;
    double frac = 0.1;
    while (p < e && *p >= '0' && *p <= '9') {
      d += (*p++ - '0') * frac;
      frac *= 0.1;
    }
  }
  *out = neg ? -d : d;
  return p == e;
}

// ---------- Auth ----------
static const char *auth(const char *h, size_t hl, int64_t *uid, char *uname, size_t *unl) {
  static const char *bad = "invalid or expired token", *payload = "invalid token payload";
  if (!h || hl < 7 || memcmp(h, "Bearer ", 7)) return "missing bearer token";
  const char *t = h + 7, *te = h + hl, *d1 = memchr(t, '.', te - t), *d2;
  if (!d1) return bad;
  d2 = memchr(d1 + 1, '.', te - d1 - 1);
  if (!d2 || memchr(d2 + 1, '.', te - d2 - 1)) return bad;
  unsigned char sig[48], mac[32];
  if (te - d2 - 1 != 43 || b64dec43(d2 + 1, sig) != 32) return bad;
  hmac((const unsigned char *)t, d2 - t, mac);
  unsigned diff = 0; for (int i = 0; i < 32; i++) diff |= sig[i] ^ mac[i];
  if (diff) return bad;

  // Header fast path: standard HS256 base64 header
  static const char std_hdr[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
  if (d1 - t != 36 || memcmp(t, std_hdr, 36) != 0) {
    if (d1 - t > 680) return bad;
    unsigned char hdr[512];
    int hn = b64dec(t, d1 - t, hdr);
    if (hn < 0) return bad;
    want_t hw[1] = {{"alg"}};
    if (!jdoc((char *)hdr, hn, hw, 1) || !hw[0].v || *hw[0].v != '"') return bad;
    char alg[8]; js_t aj = {hw[0].v, hw[0].ve}; long al = jstr(&aj, alg, sizeof alg);
    if (al != 5 || memcmp(alg, "HS256", 5)) return bad;
  }

  if (d2 - d1 - 1 > 2700) return bad;
  unsigned char pl[2048];
  int pn = b64dec(d1 + 1, d2 - d1 - 1, pl);
  if (pn < 0) return bad;
  want_t pw[4] = {{"exp"}, {"nbf"}, {"sub"}, {"username"}};
  if (!jdoc((char *)pl, pn, pw, 4) || !pw[0].v) return bad;
  for (int i = 0; i < 2; i++) if (pw[i].v) {
    double v;
    if (!parse_jwt_num(pw[i].v, pw[i].ve, &v)) return bad;
    if (i == 0 ? (double)now_s >= v : (double)now_s < v) return bad;
  }
  if (!pw[2].v || *pw[2].v != '"' || !pw[3].v || *pw[3].v != '"') return payload;
  char sb[24]; js_t sj = {pw[2].v, pw[2].ve}; long sl = jstr(&sj, sb, sizeof sb);
  if (sl <= 0 || sl > 15) return payload;
  int64_t v = 0; for (long i = 0; i < sl; i++) { if (sb[i] < '0' || sb[i] > '9') return payload; v = v * 10 + sb[i] - '0'; }
  if (v <= 0) return payload;
  js_t uj = {pw[3].v, pw[3].ve}; long ul = jstr(&uj, uname, 1024);
  if (ul < 0) return payload;
  *uid = v; *unl = ul; return NULL;
}

// ---------- Database Direct B-tree Access ----------
static Btree *bt;
static BtCursor *cur_posts, *cur_users, *cur_feed, *cur_likes;
static Pgno root_posts, root_users, root_feed, root_likes;
static KeyInfo *ki_feed, *ki_likes;
static int bt_ok, cur_open;

static void close_cursors(void) {
  if (!cur_open) return;
  sqlite3BtreeCloseCursor(cur_posts); sqlite3BtreeCloseCursor(cur_users);
  sqlite3BtreeCloseCursor(cur_feed); sqlite3BtreeCloseCursor(cur_likes);
  cur_open = 0;
}

static int open_cursors(void) {
  BtCursor *c[4] = {cur_posts, cur_users, cur_feed, cur_likes};
  Pgno roots[4] = {root_posts, root_users, root_feed, root_likes};
  KeyInfo *ki[4] = {NULL, NULL, ki_feed, ki_likes};
  for (int i = 0; i < 4; i++) {
    sqlite3BtreeCursorZero(c[i]);
    if (sqlite3BtreeCursor(bt, roots[i], 0, ki[i], c[i]) != SQLITE_OK) {
      while (i--) sqlite3BtreeCloseCursor(c[i]);
      return 0;
    }
  }
  return cur_open = 1;
}

static int txn(void) {
  if (in_txn) return 1;
  int rc = sqlite3_step(st_begin); sqlite3_reset(st_begin);
  if (rc != SQLITE_DONE) return 0;
  if (bt_ok && (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK || !open_cursors())) {
    sqlite3_step(st_rollback); sqlite3_reset(st_rollback);
    return 0;
  }
  return in_txn = 1;
}

static int wstep(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  if (rc == SQLITE_ROW || rc == SQLITE_DONE) txn_writes = 1;
  return rc;
}

static void db_fail(void) {
  if (in_txn && sqlite3_get_autocommit(db)) { close_cursors(); in_txn = 0; if (txn_writes) batch_failed = 1; txn_writes = 0; }
}

static void put_post_row(sqlite3_stmt *s) {
  PUTS("{\"id\":"); putu(sqlite3_column_int64(s, 0));
  PUTS(",\"body\":\""); putjs(sqlite3_column_text(s, 1), sqlite3_column_bytes(s, 1));
  PUTS("\",\"created_at\":\""); put(sqlite3_column_text(s, 2), sqlite3_column_bytes(s, 2));
  PUTS("\",\"author\":\""); putjs(sqlite3_column_text(s, 3), sqlite3_column_bytes(s, 3));
  PUTS("\",\"like_count\":"); putu(sqlite3_column_int64(s, 4)); PUTS("}");
}

typedef struct { uint32_t type, len; const unsigned char *d; } col_t;
static inline int rec_parse(const unsigned char *p, const unsigned char *e, col_t *out, int nc) {
  uint32_t hs, t, i = getVarint32(p, hs);
  const unsigned char *d = p + hs;
  if (d > e) return 0;
  for (int k = 0; k < nc; k++) {
    if (i >= hs) return 0;
    i += getVarint32(p + i, t);
    if (t == 10 || t == 11) return 0;
    uint32_t l = t >= 12 ? (t - 12) / 2 : sqlite3SmallTypeSizes[t];
    if (d + l > e) return 0;
    out[k] = (col_t){t, l, d}; d += l;
  }
  return 1;
}

static inline int rec_cols(BtCursor *c, col_t *out, int nc) {
  getCellInfo(c);
  if (c->info.nLocal != c->info.nPayload) return 0;
  return rec_parse(c->info.pPayload, c->info.pPayload + c->info.nPayload, out, nc);
}

static inline int col_text(const col_t *c) { return c->type >= 13 && (c->type & 1); }
static inline int col_int(const col_t *c, int64_t *v) {
  if (c->type == 8 || c->type == 9) { *v = c->type - 8; return 1; }
  if (c->type < 1 || c->type > 6) return 0;
  int64_t x = (signed char)c->d[0];
  for (uint32_t i = 1; i < c->len; i++) x = x << 8 | c->d[i];
  *v = x; return 1;
}

static int tally_cells(MemPage *pg, int first, int last, int64_t lo, int64_t hi, uint32_t *n) {
  for (int i = first; i <= last; i++) {
    const unsigned char *cell = findCell(pg, i) + pg->childPtrSize;
    if (__builtin_expect(cell[0] == 0x09 && cell[1] == 0x03 && cell[2] == 0x03, 1)) {
      int64_t post = ((int32_t)cell[4] << 16) | ((int32_t)cell[5] << 8) | cell[6];
      if (post > hi) return 0;
      if (post >= lo) n[post - lo]++;
      continue;
    } else if (cell[0] == 0x0a && cell[1] == 0x03 && cell[2] == 0x04) {
      int64_t post = (int32_t)__builtin_bswap32(*(const uint32_t *)(cell + 4));
      if (post > hi) return 0;
      if (post >= lo) n[post - lo]++;
      continue;
    }
    uint32_t size, skip = getVarint32(cell, size);
    col_t key; int64_t post;
    if (size > pg->maxLocal || cell + skip + size > pg->aDataEnd) return -1;
    if (!rec_parse(cell + skip, cell + skip + size, &key, 1) || !col_int(&key, &post)) return -1;
    if (post > hi) return 0;
    if (post >= lo) n[post - lo]++;
  }
  return 1;
}

static Mem lk_key;
static UnpackedRecord lk_want;

static int likes_from(int64_t lo) {
  int cmp = 0;
  lk_key.u.i = lo;
  if (sqlite3BtreeIndexMoveto(cur_likes, &lk_want, &cmp) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(cur_likes)) return 0;
  if (cmp >= 0) return 1;
  int rc = sqlite3BtreeNext(cur_likes, 0);
  return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

static int count_likes(int64_t lo, int64_t hi, uint32_t *n) {
  int on = likes_from(lo);
  while (on == 1) {
    MemPage *pg = cur_likes->pPage;
    int last = pg->leaf ? pg->nCell - 1 : cur_likes->ix;
    int more = tally_cells(pg, cur_likes->ix, last, lo, hi, n);
    if (more <= 0) return more < 0 ? 0 : 1;
    cur_likes->ix = (u16)last;
    on = sqlite3BtreeNext(cur_likes, 0);
    on = on == SQLITE_OK ? 1 : on == SQLITE_DONE ? 0 : -1;
  }
  return on == 0 ? 1 : -1;
}

static inline int seek_row(BtCursor *c, int64_t id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID;
}

static inline void put_row(int64_t id, const col_t *body, const unsigned char *ca, uint32_t cal, const col_t *name, uint32_t likes) {
  PUTS("{\"id\":"); putu(id);
  PUTS(",\"body\":\""); putjs(body->d, body->len);
  PUTS("\",\"created_at\":\""); put(ca, cal);
  PUTS("\",\"author\":\""); putjs(name->d, name->len);
  PUTS("\",\"like_count\":"); putu(likes); PUTS("}");
}

static inline int row_author(int64_t uid, col_t *name) {
  int rc = seek_row(cur_users, uid); if (rc <= 0) return rc;
  col_t c[2];
  if (!rec_cols(cur_users, c, 2) || !col_text(&c[1])) return 0;
  *name = c[1]; return 1;
}

// ---------- Batched Author Descent with Interpolated Search ----------
#define FEED_N 20
#define FEED_SPAN 512

struct bt_hit {
  MemPage *pg;
  int ix;
};

static inline int64_t bt_cell_key(MemPage *pg, int i) {
  u8 *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf)
    while (0x80 <= *(c++))
      if (c >= pg->aDataEnd) return INT64_MAX;
  return sqlite3VarintValue(c);
}

static int bt_lower_bound(MemPage *pg, int64_t k) {
  int n = pg->nCell;
  int64_t k0 = bt_cell_key(pg, 0), kn = bt_cell_key(pg, n - 1);
  if (k <= k0) return 0;
  if (k > kn) return n;
  int lo = 1, hi = n - 1;
  int g = lo + (int)((double)(k - k0) / (double)(kn - k0) * (double)(n - 1) - 0.5);
  if (g < lo) g = lo;
  if (g > hi) g = hi;
  for (int probe = 0; lo < hi; probe++) {
    if (bt_cell_key(pg, g) >= k) hi = g;
    else lo = g + 1;
    if (probe < 2 && lo < hi) g = (hi == g) ? g - 1 : g + 1;
    else g = lo + ((hi - lo) >> 1);
    if (g < lo) g = lo;
    if (g > hi) g = hi;
  }
  return lo;
}

static void bt_release_hits(struct bt_hit *h, int n) {
  for (int i = 0; i < n; i++)
    if (h[i].pg) releasePageNotNull(h[i].pg);
}

static int bt_seek_many(Pgno root, const int64_t *key, int n, struct bt_hit *hit) {
  BtShared *bs = bt->pBt;
  MemPage *pg[FEED_N];
  Pgno next[FEED_N];
  if (n < 1 || n > FEED_N) return -1;
  for (int i = 0; i < n; i++) next[i] = root;
  for (int depth = 0;; depth++) {
    for (int i = 0; i < n; i++) {
      int j = i - 1;
      while (j >= 0 && next[j] != next[i]) j--;
      if (j >= 0) {
        pg[i] = pg[j];
        sqlite3PagerRef(pg[i]->pDbPage);
      } else if (getAndInitPage(bs, next[i], &pg[i], PAGER_GET_READONLY) != SQLITE_OK) {
        pg[i] = NULL;
      } else {
        __builtin_prefetch(pg[i]->aCellIdx);
        __builtin_prefetch(pg[i]->aCellIdx + 2 * (pg[i]->nCell - 1));
      }
      if (!pg[i] || !pg[i]->intKey || pg[i]->nCell == 0 || pg[i]->nOverflow || depth >= BTCURSOR_MAX_DEPTH ||
          (i > 0 && pg[i]->leaf != pg[0]->leaf)) {
        for (int k = 0; k <= i; k++)
          if (pg[k]) releasePageNotNull(pg[k]);
        return -1;
      }
    }
    if (pg[0]->leaf) break;
    for (int i = 0; i < n; i++) {
      int lw = bt_lower_bound(pg[i], key[i]);
      next[i] = lw >= pg[i]->nCell ? get4byte(&pg[i]->aData[pg[i]->hdrOffset + 8]) : get4byte(findCell(pg[i], lw));
    }
    for (int i = 0; i < n; i++) releasePageNotNull(pg[i]);
  }
  for (int i = 0; i < n; i++) {
    int ix = bt_lower_bound(pg[i], key[i]);
    if (ix < pg[i]->nCell && bt_cell_key(pg[i], ix) == key[i]) {
      hit[i].pg = pg[i];
      hit[i].ix = ix;
    } else {
      releasePageNotNull(pg[i]);
      hit[i].pg = NULL;
    }
  }
  return 0;
}

static inline int hit_username(struct bt_hit *h, col_t *out) {
  if (!h->pg) return 0;
  CellInfo info;
  h->pg->xParseCell(h->pg, findCell(h->pg, h->ix), &info);
  if (info.nLocal != info.nPayload) return 0;
  col_t cols[2];
  if (!rec_parse(info.pPayload, info.pPayload + info.nPayload, cols, 2)) return 0;
  if (!col_text(&cols[1])) return 0;
  *out = cols[1];
  return 1;
}

static inline int feed_leaf_fast(MemPage *pg, int64_t *ids, unsigned char ca[FEED_N][32], uint32_t *cal) {
  if (!pg->leaf || pg->nCell < FEED_N) return 0;
  for (int i = 0; i < FEED_N; i++) {
    const unsigned char *cell = findCell(pg, i) + pg->childPtrSize;
    if (__builtin_expect(cell[0] == 0x22 && cell[1] == 0x04 && cell[2] == 0x3d && cell[3] == 0x03, 1)) {
      memcpy(ca[i], cell + 5, 24);
      cal[i] = 24;
      ids[i] = ((int32_t)cell[29] << 16) | ((int32_t)cell[30] << 8) | cell[31];
    } else if (cell[0] == 0x24 && cell[1] == 0x04 && cell[2] == 0x3d && cell[3] == 0x04) {
      memcpy(ca[i], cell + 5, 24);
      cal[i] = 24;
      ids[i] = (int32_t)__builtin_bswap32(*(const uint32_t *)(cell + 29));
    } else {
      return 0;
    }
  }
  return 1;
}

static int feed_btree(int close) {
  if (!bt_ok) return 0;
  int64_t ids[FEED_N]; unsigned char ca[FEED_N][32]; uint32_t cal[FEED_N], likes[FEED_N]; int n = 0, empty = 1;
  int64_t min_id = 0, max_id = 0;
  if (sqlite3BtreeFirst(cur_feed, &empty) != SQLITE_OK) return -1;
  if (!empty) {
    if (feed_leaf_fast(cur_feed->pPage, ids, ca, cal)) {
      n = FEED_N;
      min_id = ids[0]; max_id = ids[0];
      for (int i = 0; i < FEED_N; i++) {
        if (ids[i] < min_id) min_id = ids[i];
        if (ids[i] > max_id) max_id = ids[i];
      }
    } else for (;;) {
      col_t c[3];
      if (!rec_cols(cur_feed, c, 3) || !col_text(&c[0]) || c[0].len > sizeof ca[0] || !col_int(&c[1], &ids[n])) return 0;
      memcpy(ca[n], c[0].d, c[0].len); cal[n] = c[0].len;
      if (n == 0 || ids[n] < min_id) min_id = ids[n];
      if (n == 0 || ids[n] > max_id) max_id = ids[n];
      if (++n == FEED_N) break;
      int rc = sqlite3BtreeNext(cur_feed, 0);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_OK) return -1;
    }
  }
  if (n == 0) {
    begin_resp(200, close); PUTS("{\"posts\":[]}"); finish(); return 1;
  }
  memset(likes, 0, sizeof likes);
  if (max_id - min_id < FEED_SPAN) {
    uint32_t span[FEED_SPAN]; memset(span, 0, (max_id - min_id + 1) * sizeof span[0]);
    int rc = count_likes(min_id, max_id, span); if (rc <= 0) return rc;
    for (int i = 0; i < n; i++) likes[i] = span[ids[i] - min_id];
  } else {
    for (int i = 0; i < n; i++) { int rc = count_likes(ids[i], ids[i], &likes[i]); if (rc <= 0) return rc; }
  }

  // 1. Post lookups in ascending id order
  int order[FEED_N];
  for (int i = 0; i < n; i++) order[i] = i;
  for (int i = 1; i < n; i++) {
    int k = order[i]; int j = i - 1;
    while (j >= 0 && ids[order[j]] > ids[k]) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }

  col_t bodies[FEED_N];
  int64_t uids[FEED_N];
  MemPage *pinned[FEED_N]; int npinned = 0;
  for (int k = 0; k < n; k++) {
    int idx = order[k];
    int rc = seek_row(cur_posts, ids[idx]);
    if (rc <= 0) {
      for (int p = 0; p < npinned; p++) releasePageNotNull(pinned[p]);
      return rc;
    }
    col_t c[3];
    if (!rec_cols(cur_posts, c, 3) || !col_text(&c[2]) || !col_int(&c[1], &uids[idx])) {
      for (int p = 0; p < npinned; p++) releasePageNotNull(pinned[p]);
      return 0;
    }
    bodies[idx] = c[2];
    if (!npinned || pinned[npinned - 1] != cur_posts->pPage) {
      sqlite3PagerRef(cur_posts->pPage->pDbPage);
      pinned[npinned++] = cur_posts->pPage;
    }
  }

  // 2. Author lookups: batched simultaneous descent
  struct bt_hit hits[FEED_N];
  col_t names[FEED_N];
  int rc_many = bt_seek_many(root_users, uids, n, hits);
  for (int i = 0; i < n; i++) {
    if (rc_many != 0 || !hit_username(&hits[i], &names[i])) {
      int rc = row_author(uids[i], &names[i]);
      if (rc <= 0) {
        if (rc_many == 0) bt_release_hits(hits, n);
        for (int p = 0; p < npinned; p++) releasePageNotNull(pinned[p]);
        return rc;
      }
    }
  }

  // 3. Emit JSON directly in original feed order
  begin_resp(200, close); PUTS("{\"posts\":[");
  for (int i = 0; i < n; i++) {
    if (i) PUTS(",");
    put_row(ids[i], &bodies[i], ca[i], cal[i], &names[i], likes[i]);
  }
  PUTS("]}"); finish();
  if (rc_many == 0) bt_release_hits(hits, n);
  for (int p = 0; p < npinned; p++) releasePageNotNull(pinned[p]);
  return 1;
}

static int post_btree(int64_t id, int close) {
  if (!bt_ok) return 0;
  int rc = seek_row(cur_posts, id); col_t c[4], name; int64_t uid; uint32_t likes = 0;
  if (rc <= 0) return rc;
  if (!rec_cols(cur_posts, c, 4) || !col_text(&c[2]) || !col_text(&c[3]) || !col_int(&c[1], &uid)) return 0;
  if ((rc = row_author(uid, &name)) <= 0) return rc;
  if ((rc = count_likes(id, id, &likes)) <= 0) return rc;
  begin_resp(200, close); PUTS("{\"post\":"); put_row(id, &c[2], c[3].d, c[3].len, &name, likes); PUTS("}"); finish();
  return 1;
}

static int64_t parse_id(const char *s, size_t n) {
  if (!n) return -1;
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; if (v < 100000000000000000LL) v = v * 10 + s[i] - '0'; else v = 0x7fffffffffffffffLL; }
  return v > 0 ? v : -1;
}

static int jsws_at(const unsigned char *p, const unsigned char *e) {
  if (*p == ' ' || (*p >= 9 && *p <= 13)) return 1;
  if (e - p >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
  if (e - p >= 3) {
    uint32_t c = p[0] << 16 | p[1] << 8 | p[2];
    if (c == 0xe19a80 || (c >= 0xe28080 && c <= 0xe2808a) || c == 0xe280a8 || c == 0xe280a9 || c == 0xe280af || c == 0xe2819f || c == 0xe38080 || c == 0xefbbbf) return 3;
  }
  return 0;
}

static int jsws_before(const unsigned char *s, const unsigned char *e) {
  if (e[-1] == ' ' || (e[-1] >= 9 && e[-1] <= 13)) return 1;
  if (e - s >= 2 && e[-2] == 0xc2 && e[-1] == 0xa0) return 2;
  if (e - s >= 3 && jsws_at(e - 3, e) == 3) return 3;
  return 0;
}

static void handle(const char *m, size_t ml, const char *path, size_t pl, const char *authh, size_t al,
                   const char *body, size_t bl, int close) {
  const char *q = memchr(path, '?', pl); if (q) pl = q - path;
  int get = ml == 3 && !memcmp(m, "GET", 3), post = ml == 4 && !memcmp(m, "POST", 4);
  int rc;
  if (get && pl == 5 && !memcmp(path, "/feed", 5)) {
    if (!txn()) { err(500, "internal server error", close); return; }
    if ((rc = feed_btree(close))) { if (rc < 0) { db_fail(); err(500, "internal server error", close); } return; }
    size_t mark = alen;
    begin_resp(200, close); PUTS("{\"posts\":[");
    int i = 0;
    while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) { if (i++) PUTS(","); put_post_row(st_feed); }
    sqlite3_reset(st_feed);
    if (rc != SQLITE_DONE) { db_fail(); alen = mark; err(500, "internal server error", close); return; }
    PUTS("]}"); finish(); return;
  }
  if (pl > 7 && !memcmp(path, "/posts/", 7)) {
    const char *seg = path + 7, *sl = memchr(seg, '/', pl - 7);
    if (get && !sl) {
      int64_t id = parse_id(seg, pl - 7);
      if (id < 0) { err(400, "invalid post id", close); return; }
      if (!txn()) { err(500, "internal server error", close); return; }
      if ((rc = post_btree(id, close))) { if (rc < 0) { db_fail(); err(500, "internal server error", close); } return; }
      sqlite3_bind_int64(st_post, 1, id);
      size_t mark = alen;
      rc = sqlite3_step(st_post);
      if (rc == SQLITE_ROW) { begin_resp(200, close); PUTS("{\"post\":"); put_post_row(st_post); PUTS("}"); finish(); }
      else if (rc == SQLITE_DONE) err(404, "post not found", close);
      else { db_fail(); alen = mark; err(500, "internal server error", close); }
      sqlite3_reset(st_post); return;
    }
    if (post && sl && (size_t)(path + pl - sl) == 5 && !memcmp(sl, "/like", 5)) {
      int64_t uid; char un[1024]; size_t unl;
      const char *e = auth(authh, al, &uid, un, &unl);
      if (e) { err(401, e, close); return; }
      int64_t id = parse_id(seg, sl - seg);
      if (id < 0) { err(400, "invalid post id", close); return; }
      if (!txn()) { err(500, "internal server error", close); return; }
      rc = bt_ok ? seek_row(cur_posts, id) : 1;
      if (rc < 0) { db_fail(); err(500, "internal server error", close); return; }
      if (!rc) { err(404, "post not found", close); return; }
      sqlite3_bind_int64(st_like, 1, uid); sqlite3_bind_int64(st_like, 2, id);
      rc = wstep(st_like); sqlite3_reset(st_like);
      if (rc != SQLITE_DONE) { db_fail(); err(500, "internal server error", close); return; }
      int created = sqlite3_changes(db) > 0;
      if (!created && !bt_ok) {
        sqlite3_bind_int64(st_post, 1, id);
        rc = sqlite3_step(st_post); sqlite3_reset(st_post);
        if (rc == SQLITE_DONE) { err(404, "post not found", close); return; }
        if (rc != SQLITE_ROW) { db_fail(); err(500, "internal server error", close); return; }
      }
      begin_resp(created ? 201 : 200, close);
      PUTS("{\"liked\":true,\"already_liked\":"); if (created) PUTS("false"); else PUTS("true");
      PUTS(",\"post_id\":"); putu(id); PUTS("}"); finish(); return;
    }
  }
  if (post && pl == 6 && !memcmp(path, "/posts", 6)) {
    int64_t uid; char un[1024]; size_t unl;
    const char *e = auth(authh, al, &uid, un, &unl);
    if (e) { err(401, e, close); return; }
    want_t w[1] = {{"body"}};
    if (!jdoc(body, bl, w, 1)) { err(400, "malformed JSON body", close); return; }
    if (!w[0].v || *w[0].v != '"') { err(400, "body is required", close); return; }
    static char txt[MAXREQ]; js_t bj = {w[0].v, w[0].ve};
    long n = jstr(&bj, txt, sizeof txt);
    if (n < 0) { err(400, "malformed JSON body", close); return; }
    const unsigned char *s = (unsigned char *)txt, *se = s + n; int l;
    while (s < se && (l = jsws_at(s, se))) s += l;
    while (se > s && (l = jsws_before(s, se))) se -= l;
    if (s == se || !*s) { err(400, "body is required", close); return; }
    size_t chars = 0; for (const unsigned char *c = s; c < se; c++) chars += (*c & 0xc0) != 0x80;
    if (chars > 500) { err(400, "body must be at most 500 characters", close); return; }
    if (!txn()) { err(500, "internal server error", close); return; }
    sqlite3_bind_int64(st_ins, 1, uid); sqlite3_bind_text(st_ins, 2, (const char *)s, se - s, SQLITE_STATIC);
    rc = wstep(st_ins);
    if (rc != SQLITE_ROW) { sqlite3_reset(st_ins); db_fail(); err(500, "internal server error", close); return; }
    size_t mark = alen;
    begin_resp(201, close);
    PUTS("{\"post\":{\"id\":"); putu(sqlite3_column_int64(st_ins, 0));
    PUTS(",\"body\":\""); putjs(s, se - s);
    PUTS("\",\"created_at\":\""); put(sqlite3_column_text(st_ins, 1), sqlite3_column_bytes(st_ins, 1));
    PUTS("\",\"author\":\""); putjs((unsigned char *)un, unl); PUTS("\",\"like_count\":0}}"); finish();
    rc = sqlite3_step(st_ins); sqlite3_reset(st_ins);
    if (rc != SQLITE_DONE) { db_fail(); alen = mark; err(500, "internal server error", close); }
    return;
  }
  if (get && pl == 7 && !memcmp(path, "/health", 7)) {
    rc = sqlite3_step(st_one); sqlite3_reset(st_one);
    if (rc == SQLITE_ROW) {
      begin_resp(200, close); PUTS("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); putu(now_s - started); PUTS("}"); finish();
    } else {
      begin_resp(503, close); PUTS("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
      const char *m2 = sqlite3_errmsg(db); putjs((const unsigned char *)m2, strlen(m2)); PUTS("\"}"); finish();
    }
    return;
  }
  err(404, "not found", close);
}

static uint8_t no_conn, *sent100 = &no_conn;
static int hdr_is(const char *p, size_t n, const char *name, size_t nl) { return n == nl && !strncasecmp(p, name, nl); }

static inline char *find_crlfcrlf(const char *p, size_t n) {
  if (n < 4) return NULL;
  const char *e = p + n - 3;
#if defined(__AVX2__)
  const __m256i cr = _mm256_set1_epi8('\r');
  while (p + 32 <= e) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, cr));
    while (mask) {
      int idx = __builtin_ctz(mask);
      if (*(const uint32_t *)(p + idx) == 0x0a0d0a0du) return (char *)(p + idx);
      mask &= mask - 1;
    }
    p += 32;
  }
#endif
  while (p < e) {
    if (*(const uint32_t *)p == 0x0a0d0a0du) return (char *)p;
    p++;
  }
  return NULL;
}

static long serve(char *buf, size_t n, int *closing, conn_t *c, uint64_t who) {
  size_t used = 0;
  while (used < n && !*closing) {
    char *p = buf + used, *e = buf + n;
    if (alen + 2 * MAXREQ > BATCH_OUT || nsegs == MAXSEG) { arena_full = 1; break; }
    if (writes_held && *p == 'P') {
      if (c && !c->held) {
        c->held = 1;
        if (nheld >= cap_held) { cap_held = cap_held ? cap_held * 2 : 1024; held = realloc(held, cap_held * sizeof *held); }
        held[nheld++] = who;
      }
      break;
    }
    char *he = find_crlfcrlf(p, e - p);
    if (!he) { if ((size_t)(e - p) > MAXREQ) return -1; break; }

    if (__builtin_expect((size_t)(e - p) >= 19 &&
        *(const uint64_t *)p == 0x6565662f20544547ULL &&
        *(const uint64_t *)(p + 8) == 0x312f505454482064ULL &&
        (p[16] == '1' || p[16] == '0') && p[17] == '\r' && p[18] == '\n', 1)) {
      int close = (p[16] == '0');
      for (char *h = p + 19; h < he + 2;) {
        char *hl = memchr(h, '\r', he + 2 - h);
        if (!hl) return -1;
        if ((h[0] | 0x20) == 'c') {
          char *c2 = memchr(h, ':', hl - h);
          if (c2) {
            size_t kn = c2 - h;
            char *v = c2 + 1; while (v < hl && (*v == ' ' || *v == '\t')) v++;
            char *ve = hl; while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            if (hdr_is(h, kn, "connection", 10)) {
              if (ve - v == 5 && !strncasecmp(v, "close", 5)) close = 1;
              else if (ve - v == 10 && !strncasecmp(v, "keep-alive", 10)) close = 0;
            }
          }
        }
        h = hl + 2;
      }
      if (!txn()) { err(500, "internal server error", close); if (close) *closing = 1; used = (he + 4) - buf; continue; }
      int rc = feed_btree(close);
      if (rc <= 0) {
        if (rc < 0) { db_fail(); err(500, "internal server error", close); }
        else {
          size_t mark = alen;
          begin_resp(200, close); PUTS("{\"posts\":[");
          int i = 0;
          while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) { if (i++) PUTS(","); put_post_row(st_feed); }
          sqlite3_reset(st_feed);
          if (rc != SQLITE_DONE) { db_fail(); alen = mark; err(500, "internal server error", close); if (close) *closing = 1; used = (he + 4) - buf; continue; }
          PUTS("]}"); finish();
        }
      }
      if (close) *closing = 1;
      used = (he + 4) - buf;
      continue;
    }

    char *le = memchr(p, '\r', he + 2 - p);
    char *m = p, *sp1 = memchr(m, ' ', le - m); if (!sp1) return -1;
    char *path = sp1 + 1, *sp2 = memchr(path, ' ', le - path); if (!sp2) return -1;
    int http10 = le - sp2 - 1 == 8 && !memcmp(sp2 + 1, "HTTP/1.0", 8);
    const char *authh = NULL; size_t al = 0, clen = 0; int close = http10, expect = 0;
    for (char *h = le + 2; h < he + 2;) {
      char *hl = memchr(h, '\r', he + 2 - h), *c2;
      if (!hl) return -1;
      c2 = memchr(h, ':', hl - h);
      if (c2) {
        char *v = c2 + 1; while (v < hl && (*v == ' ' || *v == '\t')) v++;
        char *ve = hl; while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t kn = c2 - h;
        char c0 = h[0] | 0x20;
        if (c0 == 'c') {
          if (hdr_is(h, kn, "content-length", 14)) { clen = 0; for (char *d = v; d < ve; d++) { if (*d < '0' || *d > '9') return -1; clen = clen * 10 + *d - '0'; if (clen > MAXREQ) return -1; } }
          else if (hdr_is(h, kn, "connection", 10)) { if (ve - v == 5 && !strncasecmp(v, "close", 5)) close = 1; else if (ve - v == 10 && !strncasecmp(v, "keep-alive", 10)) close = 0; }
        } else if (c0 == 'a') {
          if (hdr_is(h, kn, "authorization", 13)) { authh = v; al = ve - v; }
        } else if (c0 == 'e') {
          if (hdr_is(h, kn, "expect", 6)) expect = ve - v == 12 && !strncasecmp(v, "100-continue", 12);
        } else if (c0 == 't') {
          if (hdr_is(h, kn, "transfer-encoding", 17)) return -1;
        }
      }
      h = hl + 2;
    }
    char *body = he + 4;
    if ((size_t)(e - body) < clen) {
      if (expect && body == e && !*sent100) { PUTS("HTTP/1.1 100 Continue\r\n\r\n"); *sent100 = 1; }
      break;
    }
    *sent100 = 0;
    handle(m, sp1 - m, path, sp2 - path, authh, al, body, clen, close);
    if (close) *closing = 1;
    used = body + clen - buf;
  }
  return used;
}

// ---------- Connections ----------
#define K(who) ((int)((who) >> 3 & 31))
#define SLOT(who) ((int)((who) >> 8 & 0xffffff))

static conn_t *conn_of(uint64_t who) {
  int k = K(who); uint32_t s = SLOT(who), gen = (uint32_t)(who >> 32);
  if (k >= nrings || s >= tsize) return NULL;
  conn_t *c = &R[k].c[s];
  return (c->open && c->gen == gen) ? c : NULL;
}

static inline struct io_uring_sqe *sqe(int k) {
  struct io_uring_sqe *s = io_uring_get_sqe(&R[k].r);
  if (!s) { io_uring_submit(&R[k].r); s = io_uring_get_sqe(&R[k].r); }
  return s;
}

#define POOL_CAP 4096
#define POOL_N 2048
static sop_t *pool[POOL_N]; static int npool;
static inline sop_t *sop_new(uint32_t n) {
  if (n <= POOL_CAP && npool) return pool[--npool];
  uint32_t cap = n <= POOL_CAP ? POOL_CAP : n;
  sop_t *o = malloc(sizeof *o + cap); o->cap = cap; return o;
}
static inline void sop_free(sop_t *o) {
  if (o->cap == POOL_CAP && npool < POOL_N) pool[npool++] = o; else free(o);
}

static void send_op(sop_t *o) {
  int k = K(o->who); struct io_uring_sqe *s = sqe(k);
  io_uring_prep_send(s, SLOT(o->who), o->d + o->off, o->len - o->off, MSG_NOSIGNAL);
  s->flags |= IOSQE_FIXED_FILE; s->user_data = (uintptr_t)o;
}

static void arm_recv(uint64_t who) {
  int k = K(who); struct io_uring_sqe *s = sqe(k);
  io_uring_prep_recv_multishot(s, SLOT(who), NULL, 0, 0);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT; s->buf_group = 0; s->user_data = who;
}

typedef struct { uint64_t *a; int n, cap, head; } fifo_t;
static fifo_t readq, readyq;
static void queue_read(uint64_t who, conn_t *c) {
  if (c->queued) return;
  c->queued = 1;
  if (readq.n >= readq.cap) { readq.cap = readq.cap ? readq.cap * 2 : 1024; readq.a = realloc(readq.a, readq.cap * sizeof *readq.a); }
  readq.a[readq.n++] = who;
}

static void conn_close(uint64_t who) {
  int k = K(who); uint32_t s = SLOT(who); conn_t *c = &R[k].c[s];
  if (!c->open || c->gen != (uint32_t)(who >> 32)) return;
  if (c->in) { free(c->in); c->in = NULL; }
  c->inlen = c->incap = 0;
  for (sop_t *o = c->q, *n; o; o = n) { n = (sop_t *)(uintptr_t)o->who; sop_free(o); }
  c->q = NULL;
  struct io_uring_sqe *sq = sqe(k);
  io_uring_prep_shutdown(sq, s, SHUT_RDWR); sq->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK; sq->user_data = T_NOP;
  sq = sqe(k);
  io_uring_prep_close_direct(sq, s); sq->user_data = T_NOP;
  c->open = c->closing = c->sending = c->eof = c->sent100 = c->queued = c->ready = c->held = 0;
  c->gen++; R[k].n--;
}

static void send_bytes(uint64_t who, conn_t *c, const char *p, uint32_t n) {
  if (c->closing) return;
  if (!c->sending) {
    sop_t *o = sop_new(n); *o = (sop_t){who, n, 0, n <= POOL_CAP ? (uint32_t)POOL_CAP : n}; memcpy(o->d, p, n);
    c->sending = 1; c->out = o; send_op(o); return;
  }
  sop_t *q = c->q;
  if (!q || q->cap - q->len < n) {
    uint32_t len = q ? q->len : 0, cap = (len + n) * 2;
    q = realloc(q, sizeof *q + cap); *q = (sop_t){who, len, 0, cap}; c->q = q;
  }
  memcpy(q->d + q->len, p, n); q->len += n;
}

static void process(uint64_t who, conn_t *c, char *buf, size_t n) {
  uint32_t start = alen; int closing = 0; arena_full = 0;
  sent100 = &c->sent100;
  long used = serve(buf, n, &closing, c, who);
  if (used < 0) { alen = start; conn_close(who); return; }
  if (alen > start) segs[nsegs++] = (seg_t){who, start, alen - start};
  c->closing = closing;
  size_t left = closing ? 0 : n - used;
  if (left) {
    if (buf != c->in) { c->incap = left + BSZ; c->in = malloc(c->incap); memcpy(c->in, buf + used, left); }
    else if (used) memmove(c->in, c->in + used, left);
    c->inlen = left;
    if (arena_full) {
      if (ndef >= cap_def) { cap_def = cap_def ? cap_def * 2 : 1024; deferred = realloc(deferred, cap_def * sizeof *deferred); }
      deferred[ndef++] = who;
    }
  } else if (c->in) { free(c->in); c->in = NULL; c->inlen = c->incap = 0; }
  if (alen == start && (closing || (c->eof && !c->sending && !arena_full && !c->held))) conn_close(who);
}

#define ANSWERS 2048
#define READS 4096
static int answers_left = ANSWERS;

static inline uint64_t fifo_pop(fifo_t *f) {
  uint64_t v = f->a[f->head++];
  if (f->head == f->n) f->head = f->n = 0;
  else if (f->head >= 65536 && f->head * 2 >= f->n) { f->n -= f->head; memmove(f->a, f->a + f->head, f->n * sizeof *f->a); f->head = 0; }
  return v;
}

static void on_data(uint64_t who, conn_t *c, const char *p, uint32_t n) {
  if (c->closing) return;
  int later = c->ready || !answers_left;
  if (!later) answers_left--;
  if (!later && !c->inlen && !c->sending) { process(who, c, (char *)p, n); return; }
  if (c->incap - c->inlen < n) { c->incap = (c->inlen + n) * 2; c->in = realloc(c->in, c->incap); }
  memcpy(c->in + c->inlen, p, n); c->inlen += n;
  if (c->inlen > 4 * MAXREQ) { conn_close(who); return; }
  if (later) {
    if (!c->ready) {
      c->ready = 1;
      if (readyq.n >= readyq.cap) { readyq.cap = readyq.cap ? readyq.cap * 2 : 1024; readyq.a = realloc(readyq.a, readyq.cap * sizeof *readyq.a); }
      readyq.a[readyq.n++] = who;
    }
  } else if (!c->sending) process(who, c, c->in, c->inlen);
}

static void run_ready(void) {
  while (answers_left && readyq.n) {
    uint64_t who = fifo_pop(&readyq); conn_t *c = conn_of(who);
    if (!c) continue;
    c->ready = 0;
    if (c->inlen && !c->sending) { answers_left--; process(who, c, c->in, c->inlen); }
  }
}

static void send_done(sop_t *o, int res) {
  uint64_t who = o->who; conn_t *c = conn_of(who);
  if (c && res > 0 && (o->off += res) < o->len) { send_op(o); return; }
  if (!c) {
    if (nlimbo >= cap_limbo) { cap_limbo = cap_limbo ? cap_limbo * 2 : 1024; limbo = realloc(limbo, cap_limbo * sizeof *limbo); }
    limbo[nlimbo++] = (uintptr_t)o; return;
  }
  sop_free(o);
  if (res <= 0) { c->sending = 0; conn_close(who); return; }
  if (c->q) { o = c->q; c->q = NULL; c->out = o; send_op(o); return; }
  c->sending = 0;
  if (c->closing) conn_close(who);
  else if (c->inlen) process(who, c, c->in, c->inlen);
  else if (c->eof) conn_close(who);
}

static void flush_batch(void) {
  if (in_txn) {
    close_cursors();
    int rc = sqlite3_step(st_commit); sqlite3_reset(st_commit);
    if (rc != SQLITE_DONE) { sqlite3_step(st_rollback); sqlite3_reset(st_rollback); if (txn_writes) batch_failed = 1; }
    in_txn = 0; txn_writes = 0;
  }
  for (int i = 0; i < nsegs; i++) {
    conn_t *c = conn_of(segs[i].who);
    if (!c) continue;
    if (batch_failed) { conn_close(segs[i].who); continue; }
    send_bytes(segs[i].who, c, arena + segs[i].off, segs[i].len);
  }
  nsegs = 0; alen = 0; batch_failed = 0;
}

// ---------- Ring Management ----------
static struct io_uring_buf_ring *buf_ring(struct io_uring *u) {
  void *p = mmap(NULL, NBUF * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) return NULL;
  struct io_uring_buf_reg reg = {.ring_addr = (uintptr_t)p, .ring_entries = NBUF};
  int rc = io_uring_register_buf_ring(u, &reg, 0);
  if (rc == -EINVAL) { reg.resv[0] = 1; rc = io_uring_register_buf_ring(u, &reg, 0); }
  if (rc < 0) { munmap(p, NBUF * sizeof(struct io_uring_buf)); return NULL; }
  io_uring_buf_ring_init(p);
  return p;
}

static int ring_init(int k) {
  ring_t *r = &R[k];
  struct io_uring_params p = {.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE |
                              IORING_SETUP_DEFER_TASKRUN, .cq_entries = 16384};
  if (io_uring_queue_init_params(4096, &r->r, &p) < 0) return -1;
  if (io_uring_register_files_sparse(&r->r, tsize) < 0 || !(r->br = buf_ring(&r->r))) {
    io_uring_queue_exit(&r->r); return -1;
  }
  io_uring_register_ring_fd(&r->r);
  // Zero-demand anonymous memory (faulted in page-by-page as connections arrive: 0 RSS initially)
  r->c = mmap(NULL, tsize * sizeof *r->c, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (r->c == MAP_FAILED) { io_uring_queue_exit(&r->r); return -1; }
  r->bufs = mmap(NULL, (size_t)NBUF * BSZ, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (r->bufs == MAP_FAILED) { munmap(r->c, tsize * sizeof *r->c); io_uring_queue_exit(&r->r); return -1; }
  for (int i = 0; i < NBUF; i++) io_uring_buf_ring_add(r->br, r->bufs + (size_t)i * BSZ, BSZ, i, NBUF - 1, i);
  io_uring_buf_ring_advance(r->br, NBUF);
  if (k) fprintf(stderr, "%u connections: added ring %d\n", k * (tsize - 1024), k);
  nrings = k + 1;
  return 0;
}

static int armed[NLISTEN];
static void arm_accepts(void) {
  for (int l = 0, k = 0; l < NLISTEN; l++) {
    while (armed[l] < ACCEPTS && k < MAXRINGS) {
      if (k == nrings && ring_init(k) < 0) return;
      if (R[k].n + R[k].pend + 1024 >= tsize) { k++; continue; }
      if (k >= active_rings) active_rings = k + 1;
      struct io_uring_sqe *s = sqe(k);
      io_uring_prep_accept_direct(s, lfd[l], NULL, NULL, 0, IORING_FILE_INDEX_ALLOC);
      s->user_data = T_ACCEPT | (uint64_t)l << 3;
      R[k].pend++; armed[l]++;
    }
  }
}

static void on_cqe(int k, uint64_t ud, int res, unsigned f) {
  ring_t *r = &R[k]; conn_t *c;
  switch (ud & 7) {
    case T_SEND: send_done((sop_t *)(uintptr_t)ud, res); break;
    case T_ACCEPT:
      r->pend--; armed[ud >> 3]--;
      if (res >= 0) { c = &r->c[res]; c->open = 1; r->n++; arm_recv(RECV_UD(k, res, c->gen)); }
      break;
    case T_RECV:
      c = conn_of(ud);
      if (f & IORING_CQE_F_BUFFER) {
        int bid = f >> IORING_CQE_BUFFER_SHIFT; char *p = r->bufs + (size_t)bid * BSZ;
        if (c && res > 0) on_data(ud, c, p, res);
        io_uring_buf_ring_add(r->br, p, BSZ, bid, NBUF - 1, 0); io_uring_buf_ring_advance(r->br, 1);
      }
      if (!(f & IORING_CQE_F_MORE) && (c = conn_of(ud))) {
        if (res > 0 || res == -ENOBUFS) queue_read(ud, c);
        else {
          c->eof = 1;
          if (neof >= cap_eof) { cap_eof = cap_eof ? cap_eof * 2 : 1024; eofs = realloc(eofs, cap_eof * sizeof *eofs); }
          eofs[neof++] = ud;
        }
      }
      break;
    case T_READ: {
      uint64_t who = (ud & ~7ULL) | T_RECV;
      r->reads--; c = conn_of(who);
      if (f & IORING_CQE_F_BUFFER) {
        int bid = f >> IORING_CQE_BUFFER_SHIFT; char *p = r->bufs + (size_t)bid * BSZ;
        if (c && res > 0) on_data(who, c, p, res);
        io_uring_buf_ring_add(r->br, p, BSZ, bid, NBUF - 1, 0); io_uring_buf_ring_advance(r->br, 1);
      }
      if (!(c = conn_of(who))) break;
      if (res == BSZ || res == -ENOBUFS) queue_read(who, c);
      else if (res > 0 || res == -EAGAIN) arm_recv(who);
      else {
        c->eof = 1;
        if (neof >= cap_eof) { cap_eof = cap_eof ? cap_eof * 2 : 1024; eofs = realloc(eofs, cap_eof * sizeof *eofs); }
        eofs[neof++] = who;
      }
      break;
    }
  }
}

static unsigned drain(int k) {
  struct io_uring_cqe *cqe; unsigned head, n = 0;
  io_uring_for_each_cqe(&R[k].r, head, cqe) { on_cqe(k, cqe->user_data, cqe->res, cqe->flags); n++; }
  io_uring_cq_advance(&R[k].r, n);
  return n;
}

static unsigned read_rounds(void) {
  unsigned seen = 0;
  for (int budget = READS; budget > 0 && readq.n;) {
    int issued = 0, stop = readq.n - readq.head;
    for (int i = 0; i < stop && issued < budget; i++) {
      uint64_t who = fifo_pop(&readq); conn_t *c = conn_of(who); int k = K(who);
      if (!c) continue;
      if (R[k].reads >= NBUF) {
        if (readq.n >= readq.cap) { readq.cap = readq.cap ? readq.cap * 2 : 1024; readq.a = realloc(readq.a, readq.cap * sizeof *readq.a); }
        readq.a[readq.n++] = who; continue;
      }
      c->queued = 0; R[k].reads++; issued++;
      struct io_uring_sqe *s = sqe(k);
      io_uring_prep_recv(s, SLOT(who), NULL, 0, MSG_DONTWAIT);
      s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT; s->buf_group = 0; s->user_data = (who & ~7ULL) | T_READ;
    }
    if (!issued) break;
    budget -= issued;
    for (int k = 0; k < active_rings; k++) { io_uring_submit_and_get_events(&R[k].r); seen += drain(k); }
  }
  return seen;
}

static void release_held(void) {
  if (!nheld) return;
  uint64_t *todo = held; int n = nheld; held = NULL; nheld = cap_held = 0;
  for (int i = 0; i < n; i++) {
    conn_t *c = conn_of(todo[i]);
    if (c && c->open && !c->closing) {
      c->held = 0;
      if (c->inlen && !c->sending) {
        answers_left--;
        process(todo[i], c, c->in, c->inlen);
      }
    }
  }
  free(todo);
}

static void run(void) {
  uint32_t loop_tick = 0;
  for (;;) {
    arm_accepts();
    int busy = (readq.n > 0 || readyq.n > 0);
    for (int k = active_rings - 1; k >= 0; k--) {
      if (k == 0 && !busy) {
        struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; // 1 ms wait
        struct io_uring_cqe *cq = NULL;
        io_uring_submit_and_wait_timeout(&R[0].r, &cq, 1, &ts, NULL);
      } else {
        io_uring_submit_and_get_events(&R[k].r);
      }
    }
    for (int i = 0; i < nlimbo; i++) sop_free((sop_t *)(uintptr_t)limbo[i]);
    nlimbo = 0;
    if ((loop_tick++ & 63) == 0) now_s = time(NULL);
    answers_left = ANSWERS;
    for (int k = 0; k < active_rings; k++) drain(k);
    run_ready();
    read_rounds();
    flush_batch();
    if (neof) {
      for (int i = 0; i < neof; i++) {
        conn_t *c = conn_of(eofs[i]);
        if (!c || c->sending) continue;
        if (c->inlen) process(eofs[i], c, c->in, c->inlen); else conn_close(eofs[i]);
      }
      neof = 0;
    }
    if (ndef && !arena_full) {
      uint64_t *todo = deferred; int n = ndef; deferred = NULL; ndef = cap_def = 0;
      for (int i = 0; i < n; i++) { conn_t *c = conn_of(todo[i]); if (c && c->inlen && !c->sending) process(todo[i], c, c->in, c->inlen); }
      free(todo); flush_batch();
    }
    td_tick();
    if (!writes_held && nheld) {
      release_held();
      flush_batch();
    }
  }
}

// ---------- Database Setup ----------
static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s = NULL;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
    fprintf(stderr, "prep: %s\n", sqlite3_errmsg(db)); exit(1);
  }
  return s;
}

static Pgno rootpage(const char *name) {
  sqlite3_stmt *s; Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return r;
}

static void btree_setup(void) {
  bt = db->aDb[0].pBt;
  root_posts = rootpage("posts"); root_users = rootpage("users");
  root_feed = rootpage("posts_created_at_id_idx"); root_likes = rootpage("likes_post_id_idx");
  if (!root_posts || !root_users || !root_feed || !root_likes) { bt_ok = 0; return; }

  cur_posts = malloc(sqlite3BtreeCursorSize()); cur_users = malloc(sqlite3BtreeCursorSize());
  cur_feed = malloc(sqlite3BtreeCursorSize()); cur_likes = malloc(sqlite3BtreeCursorSize());

  ki_feed = sqlite3KeyInfoAlloc(db, 2, 1);
  ki_feed->aColl[0] = db->pDfltColl; ki_feed->aSortFlags[0] = KEYINFO_ORDER_DESC;
  ki_feed->aColl[1] = db->pDfltColl; ki_feed->aSortFlags[1] = KEYINFO_ORDER_DESC;
  ki_feed->aColl[2] = db->pDfltColl;

  ki_likes = sqlite3KeyInfoAlloc(db, 1, 1);
  ki_likes->aColl[0] = db->pDfltColl; ki_likes->aSortFlags[0] = 0;
  ki_likes->aColl[1] = db->pDfltColl;

  memset(&lk_key, 0, sizeof lk_key);
  lk_key.flags = MEM_Int;
  memset(&lk_want, 0, sizeof lk_want);
  lk_want.pKeyInfo = ki_likes;
  lk_want.aMem = &lk_key;
  lk_want.nField = 1;
  lk_want.default_rc = 1;
  bt_ok = 1;
}

// ---------- WAL Write Coalescing VFS Shim ----------
#define WAL_BUF_CAP 0x1ffff
struct wfile {
  sqlite3_file base;
  sqlite3_file *real;
  int is_wal, commit, len;
  sqlite3_int64 off, last_end;
  unsigned char *buf;
};

static sqlite3_vfs *vfs_real;

static int wf_close(sqlite3_file *p) {
  struct wfile *f = (struct wfile *)p;
  if (f->buf) sqlite3_free(f->buf);
  return f->real->pMethods->xClose(f->real);
}

static int wf_read(sqlite3_file *p, void *b, int n, sqlite3_int64 off) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xRead(f->real, b, n, off);
}

static int wf_flush(struct wfile *f) {
  if (!f->len) return SQLITE_OK;
  int n = f->len; f->len = 0;
  return f->real->pMethods->xWrite(f->real, f->buf, n, f->off);
}

static int wf_write(sqlite3_file *p, const void *b, int n, sqlite3_int64 off) {
  struct wfile *f = (struct wfile *)p;
  if (!f->is_wal) return f->real->pMethods->xWrite(f->real, b, n, off);
  if (off != f->last_end || f->len + n > WAL_BUF_CAP) {
    int rc = wf_flush(f); if (rc != SQLITE_OK) return rc;
    f->commit = 0;
  }
  f->last_end = off + n;
  if (!f->buf && !(f->buf = sqlite3_malloc(WAL_BUF_CAP)))
    return f->real->pMethods->xWrite(f->real, b, n, off);
  if (!f->len) f->off = off;
  memcpy(f->buf + f->len, b, n);
  f->len += n;
  if (n == 24) {
    const unsigned char *h = b;
    f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
    return SQLITE_OK;
  }
  if (f->commit) {
    f->commit = 0;
    return wf_flush(f);
  }
  return SQLITE_OK;
}

static int wf_sync(sqlite3_file *p, int flags) {
  struct wfile *f = (struct wfile *)p;
  wf_flush(f);
  return f->real->pMethods->xSync(f->real, flags);
}

static int wf_truncate(sqlite3_file *p, sqlite3_int64 size) {
  struct wfile *f = (struct wfile *)p;
  wf_flush(f);
  return f->real->pMethods->xTruncate(f->real, size);
}

static int wf_file_control(sqlite3_file *p, int op, void *pArg) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xFileControl(f->real, op, pArg);
}

static int wf_file_size(sqlite3_file *p, sqlite3_int64 *pSize) {
  struct wfile *f = (struct wfile *)p;
  wf_flush(f);
  return f->real->pMethods->xFileSize(f->real, pSize);
}

static int wf_lock(sqlite3_file *p, int lock) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xLock(f->real, lock);
}

static int wf_unlock(sqlite3_file *p, int lock) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xUnlock(f->real, lock);
}

static int wf_check_reserved_lock(sqlite3_file *p, int *pResOut) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xCheckReservedLock(f->real, pResOut);
}

static int wf_sector_size(sqlite3_file *p) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xSectorSize(f->real);
}

static int wf_device_characteristics(sqlite3_file *p) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xDeviceCharacteristics(f->real);
}

static int wf_shm_map(sqlite3_file *p, int iPg, int pgsz, int isWrite, void volatile **pp) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xShmMap(f->real, iPg, pgsz, isWrite, pp);
}

static int wf_shm_lock(sqlite3_file *p, int offset, int n, int flags) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xShmLock(f->real, offset, n, flags);
}

static void wf_shm_barrier(sqlite3_file *p) {
  struct wfile *f = (struct wfile *)p;
  f->real->pMethods->xShmBarrier(f->real);
}

static int wf_shm_unmap(sqlite3_file *p, int deleteFlag) {
  struct wfile *f = (struct wfile *)p;
  return f->real->pMethods->xShmUnmap(f->real, deleteFlag);
}

static const sqlite3_io_methods wfile_methods = {
  2,
  wf_close, wf_read, wf_write, wf_truncate, wf_sync, wf_file_size,
  wf_lock, wf_unlock, wf_check_reserved_lock, wf_file_control,
  wf_sector_size, wf_device_characteristics,
  wf_shm_map, wf_shm_lock, wf_shm_barrier, wf_shm_unmap
};

static int wvfs_open(sqlite3_vfs *v, const char *zName, sqlite3_file *p, int flags, int *pOutFlags) {
  (void)v;
  struct wfile *f = (struct wfile *)p;
  f->real = (sqlite3_file *)((char *)p + sizeof(struct wfile));
  int rc = vfs_real->xOpen(vfs_real, zName, f->real, flags, pOutFlags);
  if (rc != SQLITE_OK) return rc;
  p->pMethods = &wfile_methods;
  f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
  f->len = 0; f->buf = NULL; f->commit = 0; f->last_end = -1;
  return SQLITE_OK;
}

static int vfs_shim_install(void) {
  vfs_real = sqlite3_vfs_find("unix-excl");
  if (!vfs_real) return -1;
  static sqlite3_vfs wvfs;
  memcpy(&wvfs, vfs_real, sizeof wvfs);
  wvfs.zName = "unix-wal-shim";
  wvfs.szOsFile = sizeof(struct wfile) + vfs_real->szOsFile;
  wvfs.xOpen = wvfs_open;
  return sqlite3_vfs_register(&wvfs, 0);
}

static void db_open(const char *path) {
  int fl = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path, &db, fl, "unix-wal-shim") != SQLITE_OK) {
    fprintf(stderr, "open %s: %s\n", path, sqlite3_errmsg(db)); exit(1);
  }
  sqlite3_busy_timeout(db, 5000);
  char *e = NULL;
  if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=0; PRAGMA cache_size=-" STR(CACHE_KB) "; PRAGMA wal_autocheckpoint=0; SELECT count(*) FROM users WHERE id = 1;", NULL, NULL, &e) != SQLITE_OK) {
    fprintf(stderr, "pragma: %s\n", e); exit(1);
  }
  sqlite3_wal_hook(db, wal_hook, NULL);

  if (sqlite3_open_v2(path, &ckdb, fl, "unix-wal-shim") != SQLITE_OK ||
      sqlite3_exec(ckdb, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=16;", NULL, NULL, NULL) != SQLITE_OK) {
    fprintf(stderr, "checkpoint connection failed: %s\n", ckdb ? sqlite3_errmsg(ckdb) : "cannot open"); exit(1);
  }
  pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 << 10);
  if (pthread_create(&t, &a, ck_main, NULL)) { fprintf(stderr, "checkpoint thread failed\n"); exit(1); }

  btree_setup();
  const char *cols = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ";
  char sql[512];
  snprintf(sql, sizeof sql, "%sORDER BY p.created_at DESC, p.id DESC LIMIT 20", cols); st_feed = prep(sql);
  snprintf(sql, sizeof sql, "%sWHERE p.id = ?", cols); st_post = prep(sql);
  st_ins = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
  st_like = prep(bt_ok ? "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING"
                       : "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_one = prep("SELECT 1");
  st_begin = prep("BEGIN"); st_commit = prep("COMMIT"); st_rollback = prep("ROLLBACK");
}

#ifdef PGO_TRAIN
#include "train.c"
#endif

int main(int argc, char **argv) {
  (void)argc; (void)argv;
#ifdef PGO_TRAIN
  train();
#endif
  const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET"), *host = getenv("HOST"), *ports = getenv("PORT");
  if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }

  pcache_install();
  pagecache_arena(CACHE_KB);
  sqlite3_initialize();
  vfs_shim_install();

  struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
  if (rl.rlim_cur < rl.rlim_max) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
  tsize = (rl.rlim_cur > 63) ? rl.rlim_cur - 63 : 1024;
  if (tsize > 65472) tsize = 65472;

  memset(b64t, -1, sizeof b64t);
  for (int i = 0; i < 64; i++) b64t[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]] = i;
  hmac_key(secret);
  started = now_s = time(NULL);
  db_open(path);

  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(ports ? atoi(ports) : 3000)};
  if (inet_pton(AF_INET, host ? host : "0.0.0.0", &a.sin_addr) != 1) { fprintf(stderr, "bad HOST\n"); return 1; }
  for (int i = 0; i < NLISTEN; i++) {
    int one = 1; lfd[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    setsockopt(lfd[i], SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd[i], SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(lfd[i], IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(lfd[i], IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof one);
    clamp_socket(lfd[i]);
    if (bind(lfd[i], (struct sockaddr *)&a, sizeof a) || listen(lfd[i], 65535)) { perror("bind/listen"); return 1; }
  }
  arena = mmap(NULL, BATCH_OUT, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (arena == MAP_FAILED) { perror("mmap arena"); return 1; }

  struct rlimit ml = {.rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY};
  setrlimit(RLIMIT_MEMLOCK, &ml);
  mlockall(MCL_CURRENT);

#define TARGET_RINGS 8
  for (int k = 0; k < TARGET_RINGS; k++) {
    if (ring_init(k) < 0) {
      if (k == 0) { fprintf(stderr, "io_uring setup failed (needs Linux 6.1+ with io_uring enabled)\n"); return 1; }
      break;
    }
  }
  run();
  return 0;
}
