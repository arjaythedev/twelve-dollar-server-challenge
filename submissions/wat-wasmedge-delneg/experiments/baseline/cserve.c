// cserve.c: wserve.c (the C reference) with its poll() loop swapped for the same level-triggered epoll loop
// and the same 8 "sys" calls the WAT entry uses, so C->wasm and WAT run on one host. Native build: same calls
// as plain syscalls. Baseline only; not part of the entry.
// Builds natively and for WebAssembly. How the listening socket is obtained:
//   native / WASI p2 / WAMR socket-ext : socket+bind+listen   (default)
//   -DLISTEN_PREOPEN                   : WASI p1, runtime pre-opens it (first socket fd >= 3)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#if !defined(LISTEN_PREOPEN) && !defined(WASMEDGE_SOCKET)
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#ifdef WAMR_SOCKET
#include "wasi_socket_ext.h"
#endif
#include "sqlite3.h"

/* ---------- sha256 / hmac ---------- */
typedef struct { uint32_t h[8]; uint8_t b[64]; uint64_t n; } sha_t;
static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha_blk(sha_t *s, const uint8_t *p) {
  uint32_t w[64], a,b,c,d,e,f,g,h;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i]<<24 | p[4*i+1]<<16 | p[4*i+2]<<8 | p[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3), s1 = ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);
    w[i] = w[i-16]+s0+w[i-7]+s1;
  }
  a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h+(ROR(e,6)^ROR(e,11)^ROR(e,25))+((e&f)^(~e&g))+K[i]+w[i];
    uint32_t t2 = (ROR(a,2)^ROR(a,13)^ROR(a,22))+((a&b)^(a&c)^(b&c));
    h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
  }
  s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha_init(sha_t *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, 32); s->n = 0;
}
static void sha_upd(sha_t *s, const void *d, size_t len) {
  const uint8_t *p = d;
  while (len) { size_t o = s->n & 63, k = 64-o < len ? 64-o : len; memcpy(s->b+o, p, k); s->n += k; p += k; len -= k; if (((s->n)&63) == 0) sha_blk(s, s->b); }
}
static void sha_fin(sha_t *s, uint8_t out[32]) {
  uint64_t bits = s->n*8; uint8_t pad = 0x80, z = 0;
  sha_upd(s, &pad, 1); while ((s->n & 63) != 56) sha_upd(s, &z, 1);
  uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = bits >> (56-8*i); sha_upd(s, L, 8);
  for (int i = 0; i < 8; i++) { out[4*i]=s->h[i]>>24; out[4*i+1]=s->h[i]>>16; out[4*i+2]=s->h[i]>>8; out[4*i+3]=s->h[i]; }
}
static sha_t hm_in, hm_out;   // HMAC states after absorbing the padded key
static void hmac_init(const char *key) {
  uint8_t k[64] = {0}, ip[64], op[64]; size_t kl = strlen(key);
  if (kl > 64) { sha_t t; sha_init(&t); sha_upd(&t, key, kl); sha_fin(&t, k); } else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) { ip[i] = k[i]^0x36; op[i] = k[i]^0x5c; }
  sha_init(&hm_in); sha_upd(&hm_in, ip, 64); sha_init(&hm_out); sha_upd(&hm_out, op, 64);
}
static void hmac(const void *d, size_t n, uint8_t out[32]) {
  sha_t s = hm_in; uint8_t t[32]; sha_upd(&s, d, n); sha_fin(&s, t);
  s = hm_out; sha_upd(&s, t, 32); sha_fin(&s, out);
}

/* ---------- small helpers ---------- */
static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c-'A'; if (c >= 'a' && c <= 'z') return c-'a'+26;
  if (c >= '0' && c <= '9') return c-'0'+52; if (c == '-') return 62; if (c == '_') return 63; return -1;
}
static int b64dec(const char *s, size_t n, char *out, size_t cap) {  // base64url, no padding
  size_t o = 0; uint32_t acc = 0; int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64v((unsigned char)s[i]); if (v < 0) return -1;
    acc = acc<<6 | v; bits += 6;
    if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (char)(acc >> bits); }
  }
  return (int)o;
}
static void b64enc32(const uint8_t *in, char *out) {  // 32 bytes -> 43 chars base64url
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  int o = 0; uint32_t acc = 0; int bits = 0;
  for (int i = 0; i < 32; i++) { acc = acc<<8 | in[i]; bits += 8; while (bits >= 6) { bits -= 6; out[o++] = A[(acc>>bits)&63]; } }
  if (bits) out[o++] = A[(acc<<(6-bits))&63];
  out[o] = 0;
}

typedef struct { char *p; size_t n, cap; } buf_t;
static void bput(buf_t *b, const void *d, size_t n) {
  if (b->n + n > b->cap) { size_t c = b->cap ? b->cap : 4096; while (c < b->n+n) c *= 2; b->p = realloc(b->p, c); b->cap = c; }
  memcpy(b->p+b->n, d, n); b->n += n;
}
static void bstr(buf_t *b, const char *s) { bput(b, s, strlen(s)); }
static void bint(buf_t *b, long long v) { char t[24]; int n = snprintf(t, sizeof t, "%lld", v); bput(b, t, n); }
static void bjson(buf_t *b, const unsigned char *s, int n) {  // JSON string with quotes
  bput(b, "\"", 1);
  int st = 0;
  for (int i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c == '"' || c == '\\' || c < 0x20) {
      bput(b, s+st, i-st); st = i+1;
      if (c == '"') bput(b, "\\\"", 2); else if (c == '\\') bput(b, "\\\\", 2);
      else if (c == '\n') bput(b, "\\n", 2); else if (c == '\r') bput(b, "\\r", 2); else if (c == '\t') bput(b, "\\t", 2);
      else { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); bput(b, t, 6); }
    }
  }
  bput(b, s+st, n-st); bput(b, "\"", 1);
}

/* ---------- minimal JSON reader (enough for JWT payloads and {"body":...}) ---------- */
typedef struct { const char *p, *e; } js_t;
static void jws(js_t *j) { while (j->p < j->e && (*j->p==' '||*j->p=='\t'||*j->p=='\n'||*j->p=='\r')) j->p++; }
// parse a string at j->p (which must be '"'); unescaped UTF-8 into out (may be NULL). returns length or -1
static int jstr(js_t *j, char *out, int cap) {
  if (j->p >= j->e || *j->p != '"') return -1; j->p++;
  int o = 0;
  while (j->p < j->e) {
    unsigned char c = *j->p++;
    if (c == '"') return o;
    if (c < 0x20) return -1;
    uint32_t cp = c;
    if (c == '\\') {
      if (j->p >= j->e) return -1; c = *j->p++;
      switch (c) {
        case '"': cp='"'; break; case '\\': cp='\\'; break; case '/': cp='/'; break; case 'b': cp=8; break;
        case 'f': cp=12; break; case 'n': cp=10; break; case 'r': cp=13; break; case 't': cp=9; break;
        case 'u': {
          if (j->e - j->p < 4) return -1; cp = 0;
          for (int i = 0; i < 4; i++) { char h = *j->p++; cp <<= 4;
            if (h>='0'&&h<='9') cp |= h-'0'; else if (h>='a'&&h<='f') cp |= h-'a'+10; else if (h>='A'&&h<='F') cp |= h-'A'+10; else return -1; }
          if (cp >= 0xD800 && cp < 0xDC00 && j->e - j->p >= 6 && j->p[0]=='\\' && j->p[1]=='u') {
            uint32_t lo = 0; const char *q = j->p+2; int ok = 1;
            for (int i = 0; i < 4; i++) { char h = q[i]; lo <<= 4;
              if (h>='0'&&h<='9') lo |= h-'0'; else if (h>='a'&&h<='f') lo |= h-'a'+10; else if (h>='A'&&h<='F') lo |= h-'A'+10; else ok = 0; }
            if (ok && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp-0xD800)<<10) + (lo-0xDC00); j->p += 6; }
          }
          break; }
        default: return -1;
      }
      char t[4]; int k;
      if (cp < 0x80) { t[0]=cp; k=1; } else if (cp < 0x800) { t[0]=0xC0|cp>>6; t[1]=0x80|(cp&63); k=2; }
      else if (cp < 0x10000) { t[0]=0xE0|cp>>12; t[1]=0x80|((cp>>6)&63); t[2]=0x80|(cp&63); k=3; }
      else { t[0]=0xF0|cp>>18; t[1]=0x80|((cp>>12)&63); t[2]=0x80|((cp>>6)&63); t[3]=0x80|(cp&63); k=4; }
      for (int i = 0; i < k; i++) { if (out) { if (o >= cap) return -1; out[o] = t[i]; } o++; }
    } else { if (out) { if (o >= cap) return -1; out[o] = c; } o++; }
  }
  return -1;
}
static int jskip(js_t *j, int depth) {  // skip any value; 0 ok, -1 bad
  jws(j); if (j->p >= j->e || depth > 32) return -1;
  char c = *j->p;
  if (c == '"') return jstr(j, NULL, 0) < 0 ? -1 : 0;
  if (c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']'; j->p++; jws(j);
    if (j->p < j->e && *j->p == close) { j->p++; return 0; }
    for (;;) {
      if (c == '{') { jws(j); if (jstr(j, NULL, 0) < 0) return -1; jws(j); if (j->p >= j->e || *j->p != ':') return -1; j->p++; }
      if (jskip(j, depth+1)) return -1; jws(j);
      if (j->p >= j->e) return -1;
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == close) { j->p++; return 0; }
      return -1;
    }
  }
  const char *s = j->p;
  if (!strncmp(s, "true", 4) && j->e-s >= 4) { j->p += 4; return 0; }
  if (!strncmp(s, "false", 5) && j->e-s >= 5) { j->p += 5; return 0; }
  if (!strncmp(s, "null", 4) && j->e-s >= 4) { j->p += 4; return 0; }
  if (c == '-' || (c >= '0' && c <= '9')) { j->p++; while (j->p < j->e && strchr("0123456789.eE+-", *j->p)) j->p++; return 0; }
  return -1;
}
// walk a top-level object; for key `want`, leave j->p at its value and return 1. 0 = not found, -1 = malformed.
// Validates the whole object either way (so malformed JSON is always caught).
static int jfind(const char *s, size_t n, const char *want, js_t *val) {
  js_t j = { s, s+n }; int found = 0; jws(&j);
  if (j.p >= j.e || *j.p != '{') return -1; j.p++; jws(&j);
  if (j.p < j.e && *j.p == '}') { j.p++; goto end; }
  for (;;) {
    char key[64]; jws(&j); int kl = jstr(&j, key, sizeof key - 1);
    if (kl < 0) { js_t t = j; if (jstr(&t, NULL, 0) < 0) return -1; j = t; kl = -2; }  // long key
    jws(&j); if (j.p >= j.e || *j.p != ':') return -1; j.p++; jws(&j);
    if (kl >= 0) { key[kl] = 0; if (!found && !strcmp(key, want)) { *val = j; found = 1; } }
    if (jskip(&j, 0)) return -1; jws(&j);
    if (j.p >= j.e) return -1;
    if (*j.p == ',') { j.p++; continue; }
    if (*j.p == '}') { j.p++; break; }
    return -1;
  }
end:
  jws(&j); if (j.p != j.e) return -1;
  return found;
}

/* ---------- database ---------- */
static sqlite3 *db;
static sqlite3_stmt *st_one, *st_feed, *st_ins, *st_like, *st_exists, *st_ping;
static time_t t0;
#define POST_SQL "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id "
static void prep(const char *sql, sqlite3_stmt **s) {
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, s, NULL) != SQLITE_OK) { fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db)); exit(1); }
}
static void db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { fprintf(stderr, "open %s: %s\n", path, sqlite3_errmsg(db)); exit(1); }
#ifdef __wasi__
  // WASI has no shared memory: WAL needs exclusive locking so the wal-index lives on the heap.
  sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE", 0, 0, 0);
#endif
  char *err = 0;
  if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-65536;", 0, 0, &err) != SQLITE_OK) { fprintf(stderr, "pragma: %s\n", err); exit(1); }
  prep(POST_SQL "WHERE p.id = ?", &st_one);
  prep(POST_SQL "ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &st_feed);
  prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at", &st_ins);
  prep("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING", &st_like);
  prep("SELECT 1 FROM posts WHERE id = ?", &st_exists);
  prep("SELECT 1", &st_ping);
}
static void post_json(buf_t *b, sqlite3_stmt *s) {
  bstr(b, "{\"id\":"); bint(b, sqlite3_column_int64(s, 0));
  bstr(b, ",\"body\":"); bjson(b, sqlite3_column_text(s, 1), sqlite3_column_bytes(s, 1));
  bstr(b, ",\"created_at\":\""); bput(b, sqlite3_column_text(s, 2), sqlite3_column_bytes(s, 2));
  bstr(b, "\",\"author\":"); bjson(b, sqlite3_column_text(s, 3), sqlite3_column_bytes(s, 3));
  bstr(b, ",\"like_count\":"); bint(b, sqlite3_column_int64(s, 4)); bstr(b, "}");
}

/* ---------- request handling ---------- */
static void respond(buf_t *out, int code, const char *body, size_t n) {
  const char *msg = code==200?"OK":code==201?"Created":code==400?"Bad Request":code==401?"Unauthorized":code==404?"Not Found":code==503?"Service Unavailable":"Internal Server Error";
  char h[160]; int hn = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", code, msg, n);
  bput(out, h, hn); bput(out, body, n);
}
static void err(buf_t *out, int code, const char *m) { char t[128]; int n = snprintf(t, sizeof t, "{\"error\":\"%s\"}", m); respond(out, code, t, n); }

static long long parse_id(const char *s, size_t n) {  // positive integer or -1
  if (n == 0 || n > 18) return -1; long long v = 0;
  for (size_t i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; v = v*10 + (s[i]-'0'); }
  return v > 0 ? v : -1;
}

// 0 ok (uid, uname/ulen = raw JSON-escaped username bytes); else writes the error
static int auth(buf_t *out, const char *h, size_t hn, long long *uid, char *uname, int *ulen) {
  if (!h || hn < 7 || memcmp(h, "Bearer ", 7)) { err(out, 401, "missing bearer token"); return -1; }
  const char *t = h+7; size_t tn = hn-7;
  const char *d1 = memchr(t, '.', tn); const char *d2 = d1 ? memchr(d1+1, '.', t+tn-d1-1) : NULL;
  if (!d1 || !d2 || memchr(d2+1, '.', t+tn-d2-1)) goto bad;
  char hdr[256], pl[1024]; int hl = b64dec(t, d1-t, hdr, sizeof hdr), pln = b64dec(d1+1, d2-d1-1, pl, sizeof pl);
  if (hl < 0 || pln < 0) goto bad;
  js_t v; char alg[16]; int al;
  if (jfind(hdr, hl, "alg", &v) != 1 || (al = jstr(&v, alg, sizeof alg)) != 5 || memcmp(alg, "HS256", 5)) goto bad;
  uint8_t mac[32]; char sig[48]; hmac(t, d2-t, mac); b64enc32(mac, sig);
  if ((size_t)(t+tn-d2-1) != 43 || memcmp(sig, d2+1, 43)) goto bad;
  if (jfind(pl, pln, "exp", &v) != 1) goto bad;
  double exp = strtod(v.p, NULL); if (exp <= (double)time(NULL)) goto bad;
  char sub[32]; int sl;
  if (jfind(pl, pln, "sub", &v) != 1 || (sl = jstr(&v, sub, sizeof sub)) < 0 || (*uid = parse_id(sub, sl)) < 0) { err(out, 401, "invalid token payload"); return -1; }
  if (jfind(pl, pln, "username", &v) != 1 || *v.p != '"') { err(out, 401, "invalid token payload"); return -1; }
  js_t u = v; if ((*ulen = jstr(&u, uname, 255)) < 0) { err(out, 401, "invalid token payload"); return -1; }
  return 0;
bad:
  err(out, 401, "invalid or expired token"); return -1;
}

static void handle(buf_t *out, const char *m, size_t mn, const char *path, size_t pn, const char *ah, size_t an, const char *body, size_t bn) {
  buf_t b = {0};
  int get = mn == 3 && !memcmp(m, "GET", 3), post = mn == 4 && !memcmp(m, "POST", 4);
  const char *q = memchr(path, '?', pn); if (q) pn = q - path;
  if (get && pn == 5 && !memcmp(path, "/feed", 5)) {
    bstr(&b, "{\"posts\":["); int first = 1;
    while (sqlite3_step(st_feed) == SQLITE_ROW) { if (!first) bput(&b, ",", 1); first = 0; post_json(&b, st_feed); }
    sqlite3_reset(st_feed); bstr(&b, "]}"); respond(out, 200, b.p, b.n);
  } else if (get && pn == 7 && !memcmp(path, "/health", 7)) {
    if (sqlite3_step(st_ping) == SQLITE_ROW) { bstr(&b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); bint(&b, time(NULL)-t0); bstr(&b, "}"); respond(out, 200, b.p, b.n); }
    else { bstr(&b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"); const char *e = sqlite3_errmsg(db); bjson(&b, (const unsigned char*)e, strlen(e)); bstr(&b, "}"); respond(out, 503, b.p, b.n); }
    sqlite3_reset(st_ping);
  } else if (pn > 7 && !memcmp(path, "/posts/", 7) && (get || post)) {
    const char *rest = path+7; size_t rn = pn-7; const char *sl = memchr(rest, '/', rn);
    if (get && !sl) {
      long long id = parse_id(rest, rn);
      if (id < 0) err(out, 400, "invalid post id");
      else {
        sqlite3_bind_int64(st_one, 1, id);
        if (sqlite3_step(st_one) == SQLITE_ROW) { bstr(&b, "{\"post\":"); post_json(&b, st_one); bstr(&b, "}"); respond(out, 200, b.p, b.n); }
        else err(out, 404, "post not found");
        sqlite3_reset(st_one);
      }
    } else if (post && sl && rest+rn-sl == 5 && !memcmp(sl, "/like", 5)) {
      long long uid; char un[256]; int ul;
      if (auth(out, ah, an, &uid, un, &ul) == 0) {
        long long id = parse_id(rest, sl-rest);
        if (id < 0) err(out, 400, "invalid post id");
        else {
          sqlite3_bind_int64(st_exists, 1, id); int ex = sqlite3_step(st_exists) == SQLITE_ROW; sqlite3_reset(st_exists);
          if (!ex) err(out, 404, "post not found");
          else {
            sqlite3_bind_int64(st_like, 1, uid); sqlite3_bind_int64(st_like, 2, id);
            int rc = sqlite3_step(st_like); sqlite3_reset(st_like);
            if (rc != SQLITE_DONE) err(out, 500, "internal server error");
            else { int fresh = sqlite3_changes(db) > 0;
              bstr(&b, "{\"liked\":true,\"already_liked\":"); bstr(&b, fresh ? "false" : "true"); bstr(&b, ",\"post_id\":"); bint(&b, id); bstr(&b, "}");
              respond(out, fresh ? 201 : 200, b.p, b.n); }
          }
        }
      }
    } else err(out, 404, "not found");
  } else if (post && pn == 6 && !memcmp(path, "/posts", 6)) {
    long long uid; char un[256]; int ul;
    if (auth(out, ah, an, &uid, un, &ul) == 0) {
      js_t v; int f = jfind(body, bn, "body", &v);
      char *txt = NULL; int tl = -1;
      if (f < 0) err(out, 400, "malformed JSON body");
      else if (f == 0 || *v.p != '"') err(out, 400, "body is required");
      else {
        txt = malloc(bn+1); js_t vv = v; tl = jstr(&vv, txt, (int)bn);
        int s = 0, e = tl;
        while (s < e && strchr(" \t\n\r\v\f", txt[s])) s++;
        while (e > s && strchr(" \t\n\r\v\f", txt[e-1])) e--;
        int chars = 0; for (int i = s; i < e; i++) if (((unsigned char)txt[i] & 0xC0) != 0x80) chars++;
        if (e == s) err(out, 400, "body is required");
        else if (chars > 500) err(out, 400, "body must be at most 500 characters");
        else {
          sqlite3_bind_int64(st_ins, 1, uid); sqlite3_bind_text(st_ins, 2, txt+s, e-s, SQLITE_STATIC);
          if (sqlite3_step(st_ins) == SQLITE_ROW) {
            bstr(&b, "{\"post\":{\"id\":"); bint(&b, sqlite3_column_int64(st_ins, 0));
            bstr(&b, ",\"body\":"); bjson(&b, (const unsigned char*)txt+s, e-s);
            bstr(&b, ",\"created_at\":\""); bput(&b, sqlite3_column_text(st_ins, 1), sqlite3_column_bytes(st_ins, 1));
            bstr(&b, "\",\"author\":\""); bput(&b, un, ul); bstr(&b, "\",\"like_count\":0}}");
            sqlite3_step(st_ins); sqlite3_reset(st_ins);
            respond(out, 201, b.p, b.n);
          } else { sqlite3_reset(st_ins); err(out, 500, "internal server error"); }
        }
      }
      free(txt);
    }
  } else err(out, 404, "not found");
  free(b.p);
}

/* ---------- connections ---------- */
typedef struct { int fd; buf_t in, out; size_t off; } conn_t;
static int ieq(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) { char x = a[i]; if (x >= 'A' && x <= 'Z') x += 32; if (x != b[i]) return 0; } return 1; }

// parse as many complete requests as are buffered; returns -1 to close
static int process(conn_t *c) {
  size_t pos = 0;
  for (;;) {
    char *s = c->in.p + pos; size_t n = c->in.n - pos;
    char *end = n >= 4 ? memmem(s, n, "\r\n\r\n", 4) : NULL;
    if (!end) break;
    char *sp1 = memchr(s, ' ', end-s); if (!sp1) return -1;
    char *sp2 = memchr(sp1+1, ' ', end-sp1-1); if (!sp2) return -1;
    char *line = memchr(s, '\n', end+2-s);
    const char *ah = NULL; size_t an = 0, clen = 0; int close_after = 0;
    for (char *h = line+1; h < end+2; ) {
      char *nl = memchr(h, '\n', end+4-h); if (!nl) break;
      size_t hl = nl - h; if (hl && h[hl-1] == '\r') hl--;
      char *colon = memchr(h, ':', hl);
      if (colon) {
        size_t kl = colon-h; char *v = colon+1; while (v < h+hl && *v == ' ') v++; size_t vl = h+hl-v;
        if (kl == 14 && ieq(h, "content-length", 14)) clen = strtoul(v, NULL, 10);
        else if (kl == 13 && ieq(h, "authorization", 13)) { ah = v; an = vl; }
        else if (kl == 10 && ieq(h, "connection", 10) && vl == 5 && ieq(v, "close", 5)) close_after = 1;
      }
      h = nl+1;
    }
    size_t hdrlen = end+4-s;
    if (n < hdrlen + clen) break;
    handle(&c->out, s, sp1-s, sp1+1, sp2-sp1-1, ah, an, s+hdrlen, clen);
    pos += hdrlen + clen;
    if (close_after) return -1;
  }
  if (pos) { memmove(c->in.p, c->in.p+pos, c->in.n-pos); c->in.n -= pos; }
  return 0;
}


#ifdef __wasm__
#define IMP(n) __attribute__((import_module("sys"), import_name(#n)))
IMP(listen) int sys_listen(uint32_t ip, int port, int backlog);
IMP(epoll_create) int sys_epoll_create(void);
IMP(epoll_ctl) int sys_epoll_ctl(int ep, int op, int fd, int ev);
IMP(epoll_wait) int sys_epoll_wait(int ep, int32_t *out, int max, int to);
IMP(accept) int sys_accept(int fd);
IMP(recv) int sys_recv(int fd, void *p, int n);
IMP(send) int sys_send(int fd, const void *p, int n);
IMP(close) int sys_close(int fd);
#else
#include <sys/epoll.h>
static int R(long r) { return r < 0 ? -errno : (int)r; }
static int sys_listen(uint32_t ip, int port, int bl) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = ip;
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, bl) < 0) return -errno; return fd; }
static int sys_epoll_create(void) { return R(epoll_create1(0)); }
static int sys_epoll_ctl(int ep, int op, int fd, int ev) { struct epoll_event e = {.events = ev, .data.u64 = fd}; return R(epoll_ctl(ep, op, fd, &e)); }
static int sys_epoll_wait(int ep, int32_t *o, int max, int to) { struct epoll_event e[1024]; int n = epoll_wait(ep, e, max, to);
  for (int i = 0; i < n; i++) { o[2*i] = (int32_t)e[i].data.u64; o[2*i+1] = e[i].events; } return n < 0 ? -errno : n; }
static int sys_accept(int fd) { return R(accept4(fd, 0, 0, SOCK_NONBLOCK)); }
static int sys_recv(int fd, void *p, int n) { return R(recv(fd, p, n, 0)); }
static int sys_send(int fd, const void *p, int n) { return R(send(fd, p, n, MSG_NOSIGNAL)); }
static int sys_close(int fd) { return R(close(fd)); }
#endif

int main(void) {
  const char *path = getenv("SQLITE_PATH"), *key = getenv("JWT_SECRET"), *host = getenv("HOST"), *port = getenv("PORT");
  t0 = time(NULL); hmac_init(key); db_open(path);
  unsigned a, b, c4, d; sscanf(host, "%u.%u.%u.%u", &a, &b, &c4, &d);
  int lfd = sys_listen(a | b << 8 | c4 << 16 | d << 24, atoi(port), 65535); if (lfd < 0) { fprintf(stderr, "listen %d\n", lfd); return 1; }
  int ep = sys_epoll_create(); sys_epoll_ctl(ep, 1, lfd, 1);
  static conn_t *cs[65536]; static char wm[65536]; static int32_t ev[1024]; static char rb[65536];
  fprintf(stderr, "listening on %s:%s\n", host, port);
  for (;;) {
    int n = sys_epoll_wait(ep, ev, 512, -1);
    for (int i = 0; i < n; i++) {
      int fd = ev[2*i], e = ev[2*i+1];
      if (fd == lfd) { for (;;) { int f = sys_accept(lfd); if (f < 0) break; if (f >= 65536) { sys_close(f); continue; }
          cs[f] = calloc(1, sizeof(conn_t)); cs[f]->fd = f; sys_epoll_ctl(ep, 1, f, 0x2001); } continue; }
      conn_t *c = cs[fd]; if (!c) continue; int dead = 0;
      if (c->off < c->out.n) { if (e & 24) dead = 1; }
      else {
        for (;;) { int r = sys_recv(fd, rb, sizeof rb);
          if (r > 0) { bput(&c->in, rb, r); if (r < (int)sizeof rb) break; } else { if (r != -11) dead = 1; break; } }
        if (!dead && process(c) < 0) dead = 2;
      }
      while (!(dead & 1) && c->off < c->out.n) { int w = sys_send(fd, c->out.p + c->off, c->out.n - c->off);
        if (w > 0) c->off += w; else { if (w != -11) dead = 1; break; } }
      if (c->off == c->out.n) { c->out.n = c->off = 0; if (dead == 2) dead = 1; if (wm[fd]) { wm[fd] = 0; sys_epoll_ctl(ep, 3, fd, 0x2001); } }
      else if (!wm[fd]) { wm[fd] = 1; sys_epoll_ctl(ep, 3, fd, 4); }
      if (dead == 1) { sys_close(fd); free(c->in.p); free(c->out.p); free(c); cs[fd] = 0; wm[fd] = 0; }
    }
  }
}
