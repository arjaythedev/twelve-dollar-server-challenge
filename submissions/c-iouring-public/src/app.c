// app.c — the API on top of loop.c (I/O), http_gen.h + batchparse.h (parsing), cursed-jwt (auth), body.c and engine.c (SQLite).
//
// One loop wakeup = one batch = one call of app_batch():
//   1. PARSE     first request of every connection lane-parallel (bp_parse), pipelined rest with fast_parse;
//                FP_FALLBACK -> fbparse.h (pico's API and results): HTTP/1.0, Connection headers, odd framing.
//                Both paths are selectable at build time (see "parser selection" below)
//   2. AUTH      every Bearer token of the batch in one jwt_verify_batch call
//   3. VALIDATE  in the spec's order: 404, 401 missing, 401 token, 400 id, 400 body
//   4. HOLD      while eng_writes_paused(): POSTs (and what follows them on that connection) stay unconsumed in the
//                loop and are re-presented with loop_represent() once the checkpoint thread has drained the WAL
//   5. DB        eng_begin_batch -> ops in arrival order -> eng_commit_batch; bodies go to a per-batch arena
//   6. EMIT      headers written backwards in front of each body; loop_write only after the commit returned, so no
//                byte of a batch reaches a socket before its COMMIT (rule 6); a failed commit turns DB replies into 500
#include "app.h"
#include "loop.h"
#include "engine.h"
#include "body.h"
#include "jwt.h"
#include "config.h"
#include "fbparse.h"  // fb_decode_chunked (chunked request bodies), whichever slow parser is built
#if APP_FAST == 3
static fp_learn_t app_learn;  // fast_parse_learn's learned layouts
#endif
const char *app_parsers(void) {
  return FAST_NAME "+" SLOW_NAME;
}

const char *app_db_mode(void) {
  static char m[256];
  const char *ckpt = APP_DB_CKPT == 4   ? "bounded inline"
                     : APP_DB_CKPT == 3 ? "io_uring"
                     : APP_DB_CKPT == 2 ? "thread"
                     : APP_DB_CKPT == 1 ? "inline"
                                        : "sqlite-auto";
  const char *locking = APP_DB_CKPT == 2 ? "unix-excl" : APP_DB_EXCL ? "exclusive" : "normal";
  snprintf(m, sizeof m, "%s reads, %s VFS, %s checkpoints%s, %s locking, cache %d KiB, %s JWT, stock SQLite (%s)",
           APP_DB_DIRECT ? "direct" : "SQL", APP_DB_VFS ? "cursed-wal" : "stock", ckpt,
           APP_DB_WAL_DROP ? " + WAL page drop" : "", locking, (int)APP_DB_CACHE_KIB,
           APP_JWT_SCALAR ? "scalar" : jwt_engine(),
           APP_SQLITE_LINK ? "separate unit, public API only" : "one unit with the engine");
  return m;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HDR_ROOM 128  // header bytes reserved in front of every body
#define MAX_PIPE 64   // requests parsed per connection per batch (the rest is re-presented)

enum { K_DB = 1, K_FIXED = 2 };  // reply comes from the engine / is a constant error
enum { OP_NONE, OP_FEED, OP_GET, OP_LIKE, OP_CREATE, OP_HEALTH };

typedef struct {
  loop_conn conn;
  uint8_t op, kind, close, last;  // close: Connection: close (client asked, HTTP/1.0, error); last: no bytes after it
  int16_t status;
  int64_t id;
  const char *tok;
  int tok_len, auth, jwt;  // jwt: index into the batch's jwt results, -1 = none
  const char *body;
  int body_len;
  uint32_t off, len;  // body in the arena at [off, off+len); header goes right before off
} slot_t;

static struct {
  eng *e;
  jwt_key_t key;
  int64_t t0_s;
  char *arena;
  size_t acap, aused;
  slot_t *s;
  int scap, n;
  const char **tok;
  int *toklen;
  jwt_result_t *jres;
  int jcap;
  loop_conn *held;
  int nheld, hcap;  // connections with held POSTs
  int had_batch;
  int test_hold_ms;      // test only (CURSED_TEST_HOLD_MS): also hold writes N ms of every 200 ms
  uint32_t deadline_us;  // deadline shedding threshold (0: off)
  int press;             // socket-memory pressure flagged by the loop for this batch
  // 503 error budget (CURSED_SOCK503_BUDGET, 0.6% by default): a token bucket that earns `b503_rate` tokens per
  // request answered, starts full and holds at most `b503_burst`; each 503 costs one. When it's empty the valve
  // stops answering 503 and requests are served normally (the drain-first backlog absorbs them). Bounds the 503s
  // of a run to burst + rate x requests, i.e. error % <= rate + burst / requests.
  double b503_rate, b503_tok, b503_burst;
  app_stats st;
  char **chunked;  // decoded chunked request bodies of this batch (freed when the next batch starts)
  int nchunked, chunkedcap;
} A;

// one 503 from the error budget: always yes when no budget is set
static int b503_take(void) {
  if (A.b503_rate <= 0) return 1;
  if (A.b503_tok >= 1) { A.b503_tok -= 1; return 1; }
  A.st.b503_skipped++;
  return 0;
}

static void *xgrow(void *p, int *cap, int need, size_t elt) {
  if (need <= *cap) return p;
  int c = *cap ? *cap : 256;
  while (c < need)
    c *= 2;
  void *q = realloc(p, (size_t)c * elt);
  if (!q) {
    perror("realloc");
    abort();
  }
  *cap = c;
  return q;
}

static char *arena_reserve(size_t need) {  // room for HDR_ROOM + need bytes at A.aused; returns the body pointer
  size_t want = A.aused + HDR_ROOM + need + 64;
  if (want > A.acap) {
    size_t c = A.acap ? A.acap : (1u << 20);
    while (c < want)
      c *= 2;
    char *q = realloc(A.arena, c);
    if (!q) {
      perror("realloc");
      abort();
    }
    A.arena = q;
    A.acap = c;
  }
  return A.arena + A.aused + HDR_ROOM;
}

static void slot_body(slot_t *s, size_t n) {  // body of n bytes was written at arena_reserve()'s pointer
  s->off = (uint32_t)(A.aused + HDR_ROOM);
  s->len = (uint32_t)n;
  A.aused += HDR_ROOM + n;
}

static void slot_const(slot_t *s, int status, const char *body) {
  size_t n = strlen(body);
  memcpy(arena_reserve(n), body, n);
  slot_body(s, n);
  s->status = (int16_t)status;
  s->kind = K_FIXED;
}

#define E_NOTFOUND   "{\"error\":\"not found\"}"
#define E_BADID      "{\"error\":\"invalid post id\"}"
#define E_NOBEARER   "{\"error\":\"missing bearer token\"}"
#define E_BADTOKEN   "{\"error\":\"invalid or expired token\"}"
#define E_BADPAY     "{\"error\":\"invalid token payload\"}"
#define E_MALFORMED  "{\"error\":\"malformed JSON body\"}"
#define E_REQUIRED   "{\"error\":\"body is required\"}"
#define E_TOOLONG    "{\"error\":\"body must be at most 500 characters\"}"
#define E_INTERNAL   "{\"error\":\"internal server error\"}"
#define E_BADREQ     "{\"error\":\"bad request\"}"
#define E_NOTIMPL    "{\"error\":\"not implemented\"}"
#define E_TOOLARGE   "{\"error\":\"request body too large\"}"
#define E_OVERLOADED "{\"error\":\"overloaded\"}"

int app_init(const char *db_path, const char *secret, char *err, size_t errn) {
  eng_opts o;
  eng_default_opts(&o);
  // build-time cursedness (cursed.sh): what the engine does to SQLite
  o.direct_reads = APP_DB_DIRECT;               // 0: the spec's prepared SELECTs
  o.wal_coalesce = APP_DB_VFS;                  // 0: SQLite's own unix VFS
  o.exclusive = APP_DB_EXCL;                    // 0: locking_mode=NORMAL
  o.ckpt_thread = APP_DB_CKPT == 2;             // 2: checkpoint thread + write gate: a pass every 1,024 frames, the
  if (o.ckpt_thread) {                          //    WAL restarted at 8,192 (32 MB; was 4,096 / 32,768 = 128 MB). Every
    o.ckpt_frames = 1024;                       //    frame past the restart is a fresh page-cache page, and at the box's
    o.ckpt_restart = 8192;                      //    memory wall the OOM killer fired on such a WAL write. Xeon at ~24k
    // nice 19: on 1 vCPU a nice-0 thread takes half the CPU from the loop for each 300-430 ms pass; at the memory
    // wall that stall left every connection's next request unread in the kernel and the OOM killer took the server
    // (2 of 5 and 2 of 3 five-minute edge holds, 8 rings, ~29k req/s; nice 19: 3 of 3 held, TCP memory <= 2 MB,
    // p99 60-177 ms; wall-280k). Costs held writes instead (drains up to ~1.1 s), which stay inside p99 < 1 s
    o.ckpt_nice = 19;
  }                                             //    req/s: 4,096 held writes 8x as often, p99 131 ms; 8,192: 77 (exp1132/1140)
  if (APP_DB_CKPT == 1) o.ckpt_frames = 16384;  // 1: inline PASSIVE after the replies
  if (APP_DB_CKPT == 0) o.ckpt_frames = 0;      // 0: SQLite's own wal_autocheckpoint (1000 frames)
  o.ckpt_async = APP_DB_CKPT == 3;              // 3: the engine's checkpoint on the loop's io_uring (app_tick),
  if (APP_DB_CKPT == 3) o.ckpt_frames = 4096;   //    in the thread's pass size (as in the engine's Xeon A/B)
  if (APP_DB_CKPT == 4) {                       // 4: Rust #82's checkpoint on our one thread: RESTART every 16,384 frames,
    o.ckpt_frames = 16384;                      //    after the batch's replies are submitted (app_tick), WAL reused in
    o.ckpt_mode = 2;                            //    place (2 = SQLITE_CHECKPOINT_RESTART); no backlog, so no write gate.
                                                //    16,384 not #82's 4,096: each pass stalls the loop about as long at
                                                //    any mark, so 4x fewer passes held 17.5k vs 15.5k req/s (Xeon, p99 < 1 s)
  }
  o.wal_fadvise =
      APP_DB_WAL_DROP;  // drop checkpointed WAL pages from the page cache (~256 -> <=20 MiB measured on a 1-vCPU Xeon)
  // page cache (default 4 MiB, as Lean #153; was 8, the engine says 16: memory is the wall; 16 -> 8 measured 0.056 vs 0.022
  // misses/request, ~+0.1 us/request, ~8 MB less RSS)
  o.cache_kib = APP_DB_CACHE_KIB;
  const char *v;
  if ((v = getenv("CURSED_CACHE_KIB"))) o.cache_kib = atoi(v);  // experiment knobs; start.sh sets none
  if ((v = getenv("CURSED_CKPT_FRAMES"))) o.ckpt_frames = atoi(v);
  if ((v = getenv("CURSED_CKPT_THREAD"))) o.ckpt_thread = atoi(v);
  if ((v = getenv("CURSED_CKPT_RESTART"))) o.ckpt_restart = atoi(v);
  if ((v = getenv("CURSED_CKPT_NICE")))
    o.ckpt_nice = atoi(
        v);  // checkpoint thread priority (engine default 0, from Xeon exp47)  // the write gate's drain mark (frames)
  A.e = eng_open(db_path, &o, err, errn);
  if (!A.e) return -1;
  if ((v = getenv("CURSED_COALESCE"))) eng_set_coalesce(A.e, atoi(v));
  if ((v = getenv("CURSED_TEST_HOLD_MS"))) A.test_hold_ms = atoi(v);
  if ((v = getenv("CURSED_DEADLINE_MS"))) A.deadline_us = (uint32_t)atoi(v) * 1000u;  // 0: off (default)
  // the burst covers draining the largest in-hold backlog seen with the valve on (sock <= 76 MB ~ 19k queued
  // requests at ~4 KB each, arena hb12b 450k/475k); 20k at rate 0.006 keeps a 60 s-hold arena run (~6M requests)
  // under 0.6% + 0.33%
  A.b503_rate = (v = getenv("CURSED_SOCK503_BUDGET")) ? atof(v) : 0.006;  // 0: no cap
  A.b503_burst = (v = getenv("CURSED_SOCK503_BURST")) ? atof(v) : 20000;
  A.b503_tok = A.b503_burst;
  jwt_key_init(&A.key, secret, strlen(secret));
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  A.t0_s = t.tv_sec;
  return 0;
}

static int writes_paused(void) {
  if (eng_writes_paused(A.e)) return 1;
  if (!A.test_hold_ms) return 0;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int)((t.tv_sec * 1000 + t.tv_nsec / 1000000) % 200) < A.test_hold_ms;
}

void app_close(void) {
  if (A.e) eng_close(A.e);
  A.e = 0;
}

const app_stats *app_get_stats(void) {
  return &A.st;
}

struct eng *app_engine(void) {
  return A.e;
}

// ---------------------------------------------------------------- slow path (SLOW_PARSE)
// Transfer-Encoding (RFC 9112 6.1, 6.3, 7.1): the value is a list of codings, over one or more header lines.
// Returns 1: exactly "chunked"; -1: chunked isn't the final coding (or an empty list): 400 and close, the body length
// can't be known; -3: other codings before the final chunked: 501 and close.
static int te_codings(const char *v, size_t vl, int *n, int *last_chunked, int *unknown) {
  size_t i = 0;
  while (i < vl) {
    while (i < vl && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
    size_t s = i;
    while (i < vl && v[i] != ',' && v[i] != ';') i++;
    size_t e = i;
    while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
    if (i < vl && v[i] == ';') return -1;  // transfer parameters: none are defined for chunked
    if (e == s) continue;
    (*n)++;
    *last_chunked = e - s == 7 && !strncasecmp(v + s, "chunked", 7);
    if (!*last_chunked) *unknown = 1;
  }
  return 0;
}

// Connection (RFC 9112 9.6): 1 if the comma-separated list holds the token tok (case-insensitive, OWS trimmed)
static int conn_has(const char *v, size_t vl, const char *tok, size_t tl) {
  size_t i = 0;
  while (i < vl) {
    while (i < vl && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
    size_t s = i;
    while (i < vl && v[i] != ',') i++;
    size_t e = i;
    while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
    if (e - s == tl && !strncasecmp(v + s, tok, tl)) return 1;
  }
  return 0;
}

// Returns bytes consumed (>0), 0 = need more, -1 = unparseable (400 and close), -3 = unsupported transfer coding
// (501 and close), -4 = chunked body over APP_MAX_BODY (413 and close). Fills r like fast_parse and sets *close; a
// chunked body is decoded into a buffer of its own (*body, freed when the next batch starts), r->body_len is its
// decoded length.
static int slow_parse(const char *b, int len, req_t *r, int *close, const char **body) {
  const char *m, *p;
  size_t ml, pl, nh = 32;
  int minor;
  slow_hdr h[32];
  int hl = SLOW_PARSE(b, (size_t)len, &m, &ml, &p, &pl, &minor, h, &nh, 0);
#if defined(APP_CHECK_SLOW) && APP_SLOW == 0
  {  // the slow path must equal pico byte for byte (method, path, version, every header)
    const char *m2, *p2;
    size_t ml2, pl2, nh2 = 32;
    int minor2;
    struct phr_header h2[32];
    int hl2 = phr_parse_request(b, (size_t)len, &m2, &ml2, &p2, &pl2, &minor2, h2, &nh2, 0);
    int same = hl == hl2;
    if (same && hl > 0) {
      same = m == m2 && ml == ml2 && p == p2 && pl == pl2 && minor == minor2 && nh == nh2;
      for (size_t i = 0; same && i < nh; i++)
        same = h[i].name == h2[i].name && h[i].name_len == h2[i].name_len && h[i].value == h2[i].value &&
               h[i].value_len == h2[i].value_len;
    }
    if (!same) {
      fprintf(stderr, "SLOW MISMATCH fb %d pico %d: %.*s\n", hl, hl2, len < 300 ? len : 300, b);
      abort();
    }
    A.st.slow_checked++;
  }
#endif
  if (hl == -2) return len >= FP_MAXHDR ? -1 : 0;
  if (hl < 0) return -1;
  memset(r, 0, sizeof *r);
  fp_route(m, (int)ml, p, (int)pl, r);
  int64_t cl = 0;
  int have_cl = 0, keep = minor >= 1, want_auth = fp_route_auth(r->route), have_auth = 0;
  int have_te = 0, te_n = 0, te_last_chunked = 0, te_unknown = 0, close_seen = 0;
  *body = 0;
  for (size_t i = 0; i < nh; i++) {
    const char *n = h[i].name, *v = h[i].value;
    size_t nl = h[i].name_len, vl = h[i].value_len;
    if (!n) return -1;  // obs-fold continuation lines: refuse
    if (nl == 14 && !strncasecmp(n, "content-length", 14)) {
      if (have_cl || !vl || vl > 9) return -1;
      cl = 0;
      for (size_t k = 0; k < vl; k++) {
        if (v[k] < '0' || v[k] > '9') return -1;
        cl = cl * 10 + (v[k] - '0');
      }
      have_cl = 1;
    } else if (nl == 17 && !strncasecmp(n, "transfer-encoding", 17)) {
      have_te = 1;
      if (te_codings(v, vl, &te_n, &te_last_chunked, &te_unknown)) return -1;
    } else if (nl == 10 && !strncasecmp(n, "connection", 10)) {
      if (conn_has(v, vl, "close", 5))
        keep = 0, close_seen = 1;  // close wins, over any other line too; "keep-alive" upgrades HTTP/1.0
      else if (!close_seen && conn_has(v, vl, "keep-alive", 10))
        keep = 1;
    } else if (want_auth && !have_auth && nl == 13 && !strncasecmp(n, "authorization", 13)) {
      have_auth = 1;
      if (vl >= 7 && !memcmp(v, "Bearer ", 7)) {
        r->auth = 1;
        r->tok = v + 7;
        r->tok_len = (int)vl - 7;
      }
    }
  }
  if (have_te) {
    // both framings at once is request smuggling's signature; HTTP/1.0 has no transfer codings (RFC 9112 6.1)
    if (have_cl || minor < 1 || !te_n || !te_last_chunked) return -1;
    if (te_unknown) return -3;
    // the chunked body, decoded by cursed-http-parser's strict fb_decode_chunked (stateless: it reads the whole body
    // so far on each call) into a buffer of its own, so the loop's input buffer stays as received and a pipelined
    // request after it is parsed in place as usual. Need more bytes until the zero chunk has arrived; the loop's
    // per-connection input limit bounds the raw body as it does a Content-Length one
    char *d = malloc(APP_MAX_BODY);
    if (!d) return -1;
    int dl = 0, used = fb_decode_chunked(b + hl, len - hl, d, APP_MAX_BODY, &dl);
    if (used <= 0) {
      free(d);
      return used == FB_CHUNK_NEED_MORE ? 0 : used == FB_CHUNK_TOO_LARGE ? -4 : -1;  // else invalid framing: 400
    }
    A.chunked = xgrow(A.chunked, &A.chunkedcap, A.nchunked + 1, sizeof *A.chunked);
    A.chunked[A.nchunked++] = d;
    *body = d;
    r->body_off = hl;
    r->body_len = dl;
    *close = !keep;
    return hl + used;
  }
  if (hl + cl > len) return 0;
  r->body_off = hl;
  r->body_len = (int)cl;
  *close = !keep;
  return hl + (int)cl;
}

// ---------------------------------------------------------------- the batch
static void add_req(loop_conn c, const char *base, const req_t *r, int close, const char *body) {
  A.s = xgrow(A.s, &A.scap, A.n + 1, sizeof *A.s);
  slot_t *s = &A.s[A.n++];
  memset(s, 0, sizeof *s);
  s->conn = c;
  s->close = (uint8_t)close;
  s->jwt = -1;
  s->id = r->id;
  s->auth = r->auth;
  s->tok = r->tok;
  s->tok_len = r->tok_len;
  s->body = body ? body : base + r->body_off;
  s->body_len = r->body_len;
  switch (r->route) {
    case R_FEED: s->op = OP_FEED; break;
    case R_HEALTH: s->op = OP_HEALTH; break;
    case R_GETPOST:
      if (r->id_ok)
        s->op = OP_GET;
      else
        slot_const(s, 400, E_BADID);
      break;
    case R_CREATE: s->op = OP_CREATE; break;
    case R_LIKE:
      s->op = OP_LIKE;
      if (!r->id_ok) s->id = 0;
      break;  // auth first, then the id (spec)
    default: slot_const(s, 404, E_NOTFOUND); break;
  }
}

static int is_write(const req_t *r) {
  return r->route == R_CREATE || r->route == R_LIKE;
}

// Parse one connection's bytes. first/fret: bp_parse's result for the first request (fret = FP_* or bytes).
static uint32_t parse_conn(loop_in *in, const req_t *first, int fret, int paused) {
  const char *d = in->data;
  int len = (int)in->len, pos = 0;
  for (int k = 0; pos < len && k < MAX_PIPE; k++) {
    req_t r;
    int ret, close = 0;
    const char *cbody = 0;  // a decoded chunked body (slow path only)
    if (k == 0 && first) {  // lane-parsed already
      r = *first;
      ret = fret;
    } else {
      ret = FAST_PARSE(d + pos, len - pos, &r);
    }
    if (ret == FP_FALLBACK) {
      A.st.slow++;
      ret = slow_parse(d + pos, len - pos, &r, &close, &cbody);
      if (ret < 0) {  // unparseable: 400 and close; an unsupported transfer coding: 501 and close
        A.s = xgrow(A.s, &A.scap, A.n + 1, sizeof *A.s);
        slot_t *s = &A.s[A.n++];
        memset(s, 0, sizeof *s);
        s->conn = in->conn;
        s->close = 1;
        s->last = 1;
        s->jwt = -1;
        if (ret == -3)
          slot_const(s, 501, E_NOTIMPL);
        else if (ret == -4)
          slot_const(s, 413, E_TOOLARGE);
        else
          slot_const(s, 400, E_BADREQ);
        return (uint32_t)len;
      }
    }
    if (ret <= 0) break;  // need more bytes
    // deadline shedding (CURSED_DEADLINE_MS, off by default): bytes that waited longer than the deadline (kernel
    // receive timestamp to now) get an immediate 503 instead of a slow answer. Keyed only on queue delay.
    // also while the loop flags socket-memory pressure: queued requests (oldest first, in arrival order) get the
    // same fast 503, which drains the kernel backlog without reconnects (conn-policy, sock503)
    if (((A.deadline_us && in->qdelay_us > A.deadline_us) || A.press) && b503_take()) {
      A.s = xgrow(A.s, &A.scap, A.n + 1, sizeof *A.s);
      slot_t *s = &A.s[A.n++];
      memset(s, 0, sizeof *s);
      s->conn = in->conn;
      s->close = (uint8_t)close;
      s->jwt = -1;
      slot_const(s, 503, E_OVERLOADED);
      // each 503 is counted under the reason that fired it: the deadline (queue delay) wins if both did
      if (A.deadline_us && in->qdelay_us > A.deadline_us) A.st.deadline_503++;
      else A.st.press_503++;
      pos += ret;
      if (close) {
        s->last = 1;
        return (uint32_t)len;
      }
      continue;
    }
    if (paused && is_write(&r)) {  // write-hold: leave it (and the rest) in the loop
      A.held = xgrow(A.held, &A.hcap, A.nheld + 1, sizeof *A.held);
      A.held[A.nheld++] = in->conn;
      A.st.held++;
      break;
    }
    add_req(in->conn, d + pos, &r, close, cbody);
    pos += ret;
    if (close) {  // nothing after a close is answered
      A.s[A.n - 1].last = 1;
      return (uint32_t)len;
    }
  }
  if (A.n && A.s[A.n - 1].conn == in->conn && pos == len) A.s[A.n - 1].last = 1;
  return (uint32_t)pos;
}

static const char *reason(int st) {
  switch (st) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 413: return "413 Content Too Large";
    case 501: return "501 Not Implemented";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

static char *put_u(char *p, uint32_t v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n)
    *p++ = t[--n];
  return p;
}

int app_batch(loop_t *L, loop_in *in, int n) {
  for (int i = 0; i < A.nchunked; i++)  // the last batch's replies are written (loop_write copies): drop its bodies
    free(A.chunked[i]);
  A.nchunked = 0;
  A.n = 0;
  A.aused = 0;
  A.had_batch = 1;
  A.press = loop_sock_pressure(L);
  int paused = A.nheld ? 1 : writes_paused();  // with POSTs already held, keep holding until on_tick re-presents
  A.st.batches++;

  // 1. PARSE: first request of every connection as lanes, the rest per connection
  enum { BPN = 256 };
  const char *bb[BPN];
  int bl[BPN], br[BPN];
  req_t bo[BPN];
  for (int base = 0; base < n; base += BPN) {
    int m = n - base < BPN ? n - base : BPN;
    int lanes = APP_LANE_PARSE && m >= APP_BP_MIN;
#if APP_LANE_PARSE
    if (lanes) {
      for (int i = 0; i < m; i++) {
        bb[i] = in[base + i].data;
        bl[i] = (int)in[base + i].len;
      }
      bp_parse(m, bb, bl, bo, br);
      A.st.lanes += (uint64_t)m;
#ifdef APP_CHECK_LANES  // test builds: every lane must equal fast_parse on the same bytes
      for (int i = 0; i < m; i++) {
        req_t r;
        int ret = fast_parse(bb[i], bl[i], &r);
        int same =
            ret == br[i] && (ret <= 0 || (r.route == bo[i].route && r.id_ok == bo[i].id_ok && r.id == bo[i].id &&
                                          r.auth == bo[i].auth && r.tok == bo[i].tok && r.tok_len == bo[i].tok_len &&
                                          r.body_off == bo[i].body_off && r.body_len == bo[i].body_len));
        if (!same) {
          fprintf(stderr, "LANE MISMATCH ret %d vs %d: %.*s\n", br[i], ret, bl[i] < 200 ? bl[i] : 200, bb[i]);
          abort();
        }
      }
#endif
    }
#else
    (void)bb;
    (void)bl;
#endif
    for (int i = 0; i < m; i++)
      in[base + i].consumed = parse_conn(&in[base + i], lanes ? &bo[i] : NULL, lanes ? br[i] : 0, paused);
  }
  A.st.requests += (uint64_t)A.n;
  if (A.b503_rate > 0) {
    A.b503_tok += A.b503_rate * A.n;
    if (A.b503_tok > A.b503_burst) A.b503_tok = A.b503_burst;
    A.st.b503_tokens = (uint64_t)A.b503_tok;
  }

  // 2. AUTH: every token of the batch in one call (every signature recomputed: rule 5)
  int nt = 0;
  for (int i = 0; i < A.n; i++) {
    slot_t *s = &A.s[i];
    if ((s->op != OP_CREATE && s->op != OP_LIKE) || s->kind) continue;
    if (!s->auth) {
      slot_const(s, 401, E_NOBEARER);
      s->op = OP_NONE;
      continue;
    }
    if (nt + 1 > A.jcap) {
      int c = A.jcap ? A.jcap * 2 : 64;
      A.tok = realloc(A.tok, (size_t)c * sizeof *A.tok);
      A.toklen = realloc(A.toklen, (size_t)c * sizeof *A.toklen);
      A.jres = realloc(A.jres, (size_t)c * sizeof *A.jres);
      if (!A.tok || !A.toklen || !A.jres) {
        perror("realloc");
        abort();
      }
      A.jcap = c;
    }
    A.tok[nt] = s->tok;
    A.toklen[nt] = s->tok_len;
    s->jwt = nt++;
  }
  if (nt) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    JWT_VERIFY(&A.key, nt, A.tok, A.toklen, (int64_t)ts.tv_sec, A.jres);
    A.st.jwts += (uint64_t)nt;
  }

  // 3. VALIDATE + 5. DB: ops in arrival order inside one transaction
  int need_db = 0;
  for (int i = 0; i < A.n && !need_db; i++)
    if (A.s[i].op && !A.s[i].kind) need_db = 1;
  int begun = need_db && eng_begin_batch(A.e) == 0;
  for (int i = 0; i < A.n; i++) {
    slot_t *s = &A.s[i];
    if (!s->op || s->kind) continue;
    if (s->jwt >= 0) {
      const jwt_result_t *j = &A.jres[s->jwt];
      if (j->status == JWT_INVALID) {
        slot_const(s, 401, E_BADTOKEN);
        continue;
      }
      if (j->status != JWT_OK) {
        slot_const(s, 401, E_BADPAY);
        continue;
      }
      if (s->op == OP_LIKE && s->id <= 0) {
        slot_const(s, 400, E_BADID);
        continue;
      }
    }
    size_t bodyn = 0, boff = 0;
    if (s->op == OP_CREATE) {  // decoded body goes to the arena, before its reply
      char *b = arena_reserve((size_t)s->body_len);
      int br2 = eng_parse_post_body(s->body, (size_t)s->body_len, b, (size_t)s->body_len + 1, &bodyn, 0);
      if (br2 != ENG_BODY_OK) {
        slot_const(s, 400, br2 == ENG_BODY_MALFORMED ? E_MALFORMED : br2 == ENG_BODY_REQUIRED ? E_REQUIRED : E_TOOLONG);
        continue;
      }
      boff = (size_t)(b - A.arena);
      A.aused = boff + bodyn;
    }
    if (!begun) {
      slot_const(s, 500, E_INTERNAL);
      continue;
    }
    size_t cap = s->op == OP_FEED ? ENG_FEED_CAP : ENG_POST_CAP + 6 * bodyn, bn = 0;
    char *out = arena_reserve(cap);
    int st;
    switch (s->op) {
      case OP_FEED: st = eng_feed(A.e, out, cap, &bn); break;
      case OP_GET: st = eng_get_post(A.e, s->id, out, cap, &bn); break;
      case OP_LIKE: st = eng_like(A.e, A.jres[s->jwt].uid, s->id, out, cap, &bn); break;
      case OP_CREATE: {
        const jwt_result_t *j = &A.jres[s->jwt];
        st = eng_create(A.e, j->uid, j->username, j->username_len, A.arena + boff, bodyn, out, cap, &bn, 0, 0);
        break;
      }
      default: {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        st = eng_health(A.e, t.tv_sec - A.t0_s, out, cap, &bn);
      }
    }
    slot_body(s, bn);
    s->status = (int16_t)st;
    s->kind = K_DB;
  }
  if (begun && eng_commit_batch(A.e) != 0) {  // rolled back: nothing of it may be claimed
    A.st.commit_failures++;
    for (int i = 0; i < A.n; i++)
      if (A.s[i].kind == K_DB) slot_const(&A.s[i], 500, E_INTERNAL);
  }

  // 6. EMIT: headers in front of each body, then hand the bytes to the loop (already committed)
  for (int i = 0; i < A.n; i++) {
    slot_t *s = &A.s[i];
    if (s->status == 503) A.st.all_503++;  // every 503 we send, whatever its source
    if (s->last && !s->close && loop_close_hint(L, s->conn)) s->close = 1;
    char h[HDR_ROOM], *p = h;
#define PUT(lit) (memcpy(p, lit, sizeof lit - 1), p += sizeof lit - 1)
    PUT("HTTP/1.1 ");
    {
      const char *r = reason(s->status);
      size_t rl = strlen(r);
      memcpy(p, r, rl);
      p += rl;
    }
    PUT("\r\nContent-Type: application/json\r\nContent-Length: ");
    p = put_u(p, s->len);
    if (s->close) PUT("\r\nConnection: close");
    PUT("\r\n\r\n");
#undef PUT
    size_t hl = (size_t)(p - h);
    char *dst = A.arena + s->off - hl;
    memcpy(dst, h, hl);
    loop_write(L, s->conn, dst, hl + s->len);
    if (s->close) loop_close(L, s->conn);
  }
  return LOOP_COMMITTED;
}

#if APP_DB_CKPT == 3
static int file_submit(void *L, const eng_fop *ops, int n) {  // eng_fop is layout-identical to loop_fop
  return loop_file_submit(L, (const loop_fop *)ops, n);
}
#endif

void app_quiesce(void) {
#if APP_DB_CKPT == 3
  if (A.e) eng_ckpt_quiesce(A.e);  // finish the checkpoint cycle in flight, start no new one
#endif
}

int app_inflight(void) {
#if APP_DB_CKPT == 3
  return A.e ? eng_ckpt_inflight(A.e) : 0;
#else
  return 0;
#endif
}

void app_tick(loop_t *L) {
#if APP_DB_CKPT == 3
  static int fileops_set;
  if (!fileops_set) eng_set_fileops(A.e, file_submit, L), fileops_set = 1;  // until now the ops ran synchronously
  eng_tick(A.e);
#endif
  if (A.nheld && !writes_paused()) {  // the WAL drained: bring the held POSTs back
    for (int i = 0; i < A.nheld; i++)
      loop_represent(L, A.held[i]);
    A.nheld = 0;
  }
  if (A.had_batch) {
#if APP_DB_CKPT == 4
    if (eng_ckpt_due(A.e)) loop_flush(L);  // the replies leave before the checkpoint, not after it
#endif
    eng_after_batch(A.e);
  }
  else
    eng_idle(A.e);
  A.had_batch = 0;
}
