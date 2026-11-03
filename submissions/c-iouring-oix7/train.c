/* train.c — PGO trainer for bin/server.
 *
 * A second main() compiled over the same translation units as server.c minus
 * the transport (no uring.c / poll.c): it builds a synthetic database with the
 * exact challenge schema, then drives the real request pipeline
 * (http_parse -> app_decode -> app_batch) with an in-memory request mix for a
 * fixed number of seconds so a -fprofile-generate build records realistic
 * profiles for the final -fprofile-use compile of server.c (see build.sh,
 * Linux branch). On Darwin this file is compiled only as the bin/bench timing
 * harness; the PGO trainer runs on Linux.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "sqlite/sqlite3.c"
#include "runtime/rt.h"

i64 now_s;

#include "runtime/out.c"
#include "runtime/json.c"
#include "runtime/jwt.c"
#include "runtime/http.c"
#include "runtime/db.c"
#include "gen/app_gen.c"

/* ---- trainer configuration --------------------------------------------- */

#define N_USERS        5000
#define N_POSTS        40000
#define LIKES_PER_POST 4
#define BATCH_MAX      256
#define TRAIN_SECRET   "twelve-dollar-challenge"
#define TRAIN_BODY     "{\"body\":\"hello train world\"}"

static char g_tok[256];

/* Unpadded base64url encoder; jwt.c only ships the decoder side. */
static const char B64URL[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void b64url_enc(const u8 *in, size_t n, char *out) {
  size_t i = 0, o = 0;
  while (i + 3 <= n) {
    u32 v = (u32)in[i] << 16 | (u32)in[i + 1] << 8 | in[i + 2];
    out[o++] = B64URL[(v >> 18) & 63];
    out[o++] = B64URL[(v >> 12) & 63];
    out[o++] = B64URL[(v >> 6) & 63];
    out[o++] = B64URL[v & 63];
    i += 3;
  }
  if (n - i == 1) {
    u32 v = (u32)in[i] << 16;
    out[o++] = B64URL[(v >> 18) & 63];
    out[o++] = B64URL[(v >> 12) & 63];
  } else if (n - i == 2) {
    u32 v = (u32)in[i] << 16 | (u32)in[i + 1] << 8;
    out[o++] = B64URL[(v >> 18) & 63];
    out[o++] = B64URL[(v >> 12) & 63];
    out[o++] = B64URL[(v >> 6) & 63];
  }
  out[o] = 0;
}

/* One valid HS256 token for uid 1 / username u1, signed with the server's
 * secret through the HMAC primitives of jwt.c. */
static void make_token(char *tok, size_t cap) {
  const char *hdr = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  char payload[160], h[128], p[160];
  long long now = (long long)time(0);
  snprintf(payload, sizeof payload,
           "{\"sub\":\"1\",\"username\":\"u1\",\"iat\":%lld,\"exp\":%lld}",
           now, now + 3600);
  b64url_enc((const u8 *)hdr, strlen(hdr), h);
  b64url_enc((const u8 *)payload, strlen(payload), p);
  size_t hl = strlen(h), pl = strlen(p);
  if (hl + pl + 46 > cap) {
    fprintf(stderr, "train: token buffer too small\n");
    exit(1);
  }
  memcpy(tok, h, hl);
  tok[hl] = '.';
  memcpy(tok + hl + 1, p, pl);
  tok[hl + 1 + pl] = '.';
  u8 mac[32];
  hmac_sha256((const u8 *)tok, hl + 1 + pl, mac);   /* signs "header.payload" */
  b64url_enc(mac, 32, tok + hl + 2 + pl);
}

/* ---- synthetic database ------------------------------------------------- */

static sqlite3 *g_bdb;

static void bexec(const char *sql) {
  char *err = 0;
  if (sqlite3_exec(g_bdb, sql, 0, 0, &err) != SQLITE_OK) {
    fprintf(stderr, "train: %s: %s\n", sql, err ? err : sqlite3_errmsg(g_bdb));
    exit(1);
  }
}

static void bstep(sqlite3_stmt *st) {
  if (sqlite3_step(st) != SQLITE_DONE) {
    fprintf(stderr, "train: insert failed: %s\n", sqlite3_errmsg(g_bdb));
    exit(1);
  }
  sqlite3_reset(st);
}

/* 'YYYY-MM-DDTHH:MM:SS.mmmZ' from an ms offset — the wire format the schema
 * defaults produce. */
static void ts_ms(char *out, size_t cap, long long ms) {
  long long sec = ms / 1000, msec = ms % 1000;
  snprintf(out, cap, "2026-01-01T%02lld:%02lld:%02lld.%03lldZ",
           (sec / 3600) % 24, (sec / 60) % 60, sec % 60, msec);
}

/* Tables + indexes exactly as docs/challenge/schema.sql. */
static const char *SCHEMA_SQL =
  "CREATE TABLE users ("
  "  id          INTEGER PRIMARY KEY,"
  "  username    TEXT NOT NULL UNIQUE,"
  "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))"
  ");"
  "CREATE TABLE posts ("
  "  id          INTEGER PRIMARY KEY,"
  "  user_id     INTEGER NOT NULL REFERENCES users(id),"
  "  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
  "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))"
  ");"
  "CREATE TABLE likes ("
  "  user_id     INTEGER NOT NULL REFERENCES users(id),"
  "  post_id     INTEGER NOT NULL REFERENCES posts(id),"
  "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
  "  PRIMARY KEY (user_id, post_id)"
  ");"
  "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
  "CREATE INDEX posts_user_id_idx       ON posts (user_id);"
  "CREATE INDEX likes_post_id_idx       ON likes (post_id);";

/* mkdir -p for the directory portion of path (db may live under build/). */
static void mkdirs_for(const char *path) {
  char tmp[1024];
  size_t n = strlen(path);
  if (n == 0 || n >= sizeof tmp) return;
  memcpy(tmp, path, n + 1);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
}

static void build_db(const char *path) {
  mkdirs_for(path);
  char side[1024];
  snprintf(side, sizeof side, "%s-wal", path); remove(side);
  snprintf(side, sizeof side, "%s-shm", path); remove(side);
  remove(path);

  if (sqlite3_open(path, &g_bdb) != SQLITE_OK) {
    fprintf(stderr, "train: cannot create %s\n", path);
    exit(1);
  }
  bexec("PRAGMA journal_mode=WAL");
  bexec("PRAGMA synchronous=NORMAL");
  bexec(SCHEMA_SQL);
  bexec("BEGIN");

  srand(42);                       /* deterministic contents across runs */

  sqlite3_stmt *iu, *ip, *il;
  if (sqlite3_prepare_v2(g_bdb,
        "INSERT INTO users (id, username, created_at) VALUES (?1, ?2, ?3)",
        -1, &iu, 0) != SQLITE_OK ||
      sqlite3_prepare_v2(g_bdb,
        "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)",
        -1, &ip, 0) != SQLITE_OK ||
      sqlite3_prepare_v2(g_bdb,
        "INSERT INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, ?3)",
        -1, &il, 0) != SQLITE_OK) {
    fprintf(stderr, "train: prepare insert: %s\n", sqlite3_errmsg(g_bdb));
    exit(1);
  }

  static const char FIXED_TS[] = "2026-01-01T00:00:00.000Z";
  char ts[32], body[41];

  for (long long i = 1; i <= N_USERS; i++) {
    char un[24];
    snprintf(un, sizeof un, "u%lld", i);
    sqlite3_bind_int64(iu, 1, i);
    sqlite3_bind_text(iu, 2, un, -1, SQLITE_STATIC);
    sqlite3_bind_text(iu, 3, FIXED_TS, -1, SQLITE_STATIC);
    bstep(iu);
  }

  /* created_at strictly nondecreasing AND distinct in rowid order: a
   * monotonically increasing millisecond counter is baked into the string. */
  for (long long p = 1; p <= N_POSTS; p++) {
    sqlite3_bind_int64(ip, 1, p);
    sqlite3_bind_int64(ip, 2, 1 + rand() % N_USERS);
    for (int k = 0; k < 40; k++) body[k] = (char)(33 + rand() % 94);
    body[40] = 0;
    sqlite3_bind_text(ip, 3, body, 40, SQLITE_STATIC);
    ts_ms(ts, sizeof ts, p - 1);
    sqlite3_bind_text(ip, 4, ts, -1, SQLITE_STATIC);
    bstep(ip);
  }

  static u8 seen[N_USERS + 1];
  for (long long p = 1; p <= N_POSTS; p++) {
    memset(seen, 0, sizeof seen);
    for (int k = 0; k < LIKES_PER_POST; k++) {
      long long u;
      do { u = 1 + rand() % N_USERS; } while (seen[u]);   /* 4 distinct users */
      seen[u] = 1;
      sqlite3_bind_int64(il, 1, u);
      sqlite3_bind_int64(il, 2, p);
      sqlite3_bind_text(il, 3, FIXED_TS, -1, SQLITE_STATIC);
      bstep(il);
    }
  }

  sqlite3_finalize(iu); sqlite3_finalize(ip); sqlite3_finalize(il);
  bexec("COMMIT");
  sqlite3_close(g_bdb);
}

/* ---- training loop ------------------------------------------------------ */

int main(int argc, char **argv) {
  const char *db_path = (argc > 1 && argv[1][0]) ? argv[1] : "build/train.db";
  long long target_iters = 0;
  long long secs = (argc > 2) ? atoll(argv[2]) : 8;
  int n_trials = 1;
  if (argc > 2 && strcmp(argv[2], "--bench") == 0) {
    target_iters = (argc > 3) ? atoll(argv[3]) : 1000000;
    if (target_iters < 1000) target_iters = 1000;
    n_trials = (argc > 4) ? atoi(argv[4]) : 5;
    if (n_trials < 1) n_trials = 1;
    if (n_trials > 15) n_trials = 15;
  } else {
    if (secs < 1) secs = 1;
    if (secs > 60) secs = 60;
  }

  build_db(db_path);

  now_s = (i64)time(0);
  hmac_init((const u8 *)TRAIN_SECRET, sizeof(TRAIN_SECRET) - 1);
  make_token(g_tok, sizeof g_tok);
  {
    claims_t self;
    if (jwt_verify((const u8 *)g_tok, (u32)strlen(g_tok), now_s, &self) != JWT_OK ||
        self.uid != 1) {
      fprintf(stderr, "train: generated token rejected\n");
      exit(1);
    }
  }

  db_open(db_path);
  app_init();
  app_check_inv();
  if (!inv_ok) {
    fprintf(stderr, "train: invariant failed\n");
    exit(1);
  }

  /* Pre-build a deterministic 4096-request corpus outside the timed loop so
   * the benchmark measures the actual server pipeline (http_parse, app_decode,
   * jwt_verify, B-tree plans, SQLite writes, JSON emission) rather than
   * libc snprintf() formatting overhead. */
  #define CORPUS_N 4096
  static char corpus[CORPUS_N][512];
  static u32 corpus_len[CORPUS_N];
  srand(1337);
  for (int ci = 0; ci < CORPUS_N; ci++) {
    int r = rand() % 100, len;
    char *buf = corpus[ci];
    if (r < 45) {                                   /* GET /feed */
      len = snprintf(buf, 512, "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
    } else if (r < 90) {                            /* GET /posts/<id> */
      long long id = (rand() % 100 < 30)
        ? (long long)(N_POSTS - 200) + 1 + rand() % 200   /* newest 200 ids */
        : 1 + rand() % N_POSTS;
      len = snprintf(buf, 512, "GET /posts/%lld HTTP/1.1\r\nHost: localhost\r\n\r\n", id);
    } else if (r < 91) {                            /* GET /health */
      len = snprintf(buf, 512, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    } else if (r < 92) {                            /* error paths: 404/400/401 */
      int e = rand() % 4;
      if (e == 0)                                   /* unknown post id -> 404 */
        len = snprintf(buf, 512, "GET /posts/999999999 HTTP/1.1\r\nHost: localhost\r\n\r\n");
      else if (e == 1)                              /* non-numeric id -> 400 */
        len = snprintf(buf, 512, "GET /posts/abc HTTP/1.1\r\nHost: localhost\r\n\r\n");
      else if (e == 2)                              /* missing auth -> 401 */
        len = snprintf(buf, 512,
          "POST /posts/1/like HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
      else                                          /* unknown route -> 404 */
        len = snprintf(buf, 512, "GET /nope HTTP/1.1\r\nHost: localhost\r\n\r\n");
    } else if (r < 99) {                            /* POST /posts/<id>/like */
      len = snprintf(buf, 512,
        "POST /posts/%lld/like HTTP/1.1\r\nHost: localhost\r\n"
        "Authorization: Bearer %s\r\nContent-Length: 0\r\n\r\n",
        (long long)(1 + rand() % N_POSTS), g_tok);
    } else {                                        /* POST /posts */
      len = snprintf(buf, 512,
        "POST /posts HTTP/1.1\r\nHost: localhost\r\n"
        "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\n\r\n" TRAIN_BODY,
        g_tok, sizeof(TRAIN_BODY) - 1);
    }
    if (len <= 0 || len >= 512) {
      fprintf(stderr, "train: request build failed\n");
      exit(1);
    }
    corpus_len[ci] = (u32)len;
  }

  double trial_us[16];
  double trial_rps[16];
  long long total_iters = 0, total_batches = 0;

  for (int tr = 0; tr < n_trials; tr++) {
    job_t jobs[BATCH_MAX];
    int n = 0;
    long long iters = 0, batches = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
      if (target_iters > 0) {
        if (iters >= target_iters) break;
      } else {
        if ((iters & 255) == 0) {
          struct timespec t1;
          clock_gettime(CLOCK_MONOTONIC, &t1);
          double el = (double)(t1.tv_sec - t0.tv_sec) +
                      1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
          if (el >= (double)secs) break;
        }
      }

      char dyn_buf[512];
      const u8 *buf;
      u32 len;
      if (target_iters > 0) {
        int ci = (int)(iters & (CORPUS_N - 1));
        buf = (const u8 *)corpus[ci];
        len = corpus_len[ci];
      } else {
        int r = rand() % 100, l;
        if (r < 45) {
          l = snprintf(dyn_buf, sizeof dyn_buf, "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
        } else if (r < 90) {
          long long id = (rand() % 100 < 30)
            ? (long long)(N_POSTS - 200) + 1 + rand() % 200
            : 1 + rand() % N_POSTS;
          l = snprintf(dyn_buf, sizeof dyn_buf, "GET /posts/%lld HTTP/1.1\r\nHost: localhost\r\n\r\n", id);
        } else if (r < 91) {
          l = snprintf(dyn_buf, sizeof dyn_buf, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        } else if (r < 92) {
          int e = rand() % 4;
          if (e == 0)
            l = snprintf(dyn_buf, sizeof dyn_buf, "GET /posts/999999999 HTTP/1.1\r\nHost: localhost\r\n\r\n");
          else if (e == 1)
            l = snprintf(dyn_buf, sizeof dyn_buf, "GET /posts/abc HTTP/1.1\r\nHost: localhost\r\n\r\n");
          else if (e == 2)
            l = snprintf(dyn_buf, sizeof dyn_buf,
              "POST /posts/1/like HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
          else
            l = snprintf(dyn_buf, sizeof dyn_buf, "GET /nope HTTP/1.1\r\nHost: localhost\r\n\r\n");
        } else if (r < 99) {
          l = snprintf(dyn_buf, sizeof dyn_buf,
            "POST /posts/%lld/like HTTP/1.1\r\nHost: localhost\r\n"
            "Authorization: Bearer %s\r\nContent-Length: 0\r\n\r\n",
            (long long)(1 + rand() % N_POSTS), g_tok);
        } else {
          l = snprintf(dyn_buf, sizeof dyn_buf,
            "POST /posts HTTP/1.1\r\nHost: localhost\r\n"
            "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\n\r\n" TRAIN_BODY,
            g_tok, sizeof(TRAIN_BODY) - 1);
        }
        buf = (const u8 *)dyn_buf;
        len = (u32)l;
      }

      req_t rq;
      int used = http_parse(buf, len, &rq);
      if (used <= 0) {
        fprintf(stderr, "train: http_parse rejected request (%d)\n", used);
        exit(1);
      }
      job_t j;
      memset(&j, 0, sizeof j);
      app_decode(&rq, &j);
      jobs[n++] = j;
      iters++;

      if (n == BATCH_MAX) {
        app_batch(jobs, n);
        batches++;
        for (int i = 0; i < n; i++)
          if (jobs[i].resp) obuf_release(jobs[i].resp);
        n = 0;
      }
    }

    if (n) {
      app_batch(jobs, n);
      batches++;
      for (int i = 0; i < n; i++)
        if (jobs[i].resp) obuf_release(jobs[i].resp);
    }

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double el = (double)(t_end.tv_sec - t0.tv_sec) +
                1e-9 * (double)(t_end.tv_nsec - t0.tv_nsec);
    trial_us[tr] = (iters > 0) ? (el * 1e6 / (double)iters) : 0.0;
    trial_rps[tr] = (el > 0.0) ? ((double)iters / el) : 0.0;
    total_iters = iters;
    total_batches = batches;
    if (n_trials > 1) {
      fprintf(stderr, "trial %d: %lld iterations, %lld batches, %.4f s (%.4f us/req, %.1f req/s)\n",
              tr + 1, iters, batches, el, trial_us[tr], trial_rps[tr]);
    }
  }

  /* Sort trials by us/req and report median. */
  for (int i = 0; i < n_trials; i++) {
    for (int j = i + 1; j < n_trials; j++) {
      if (trial_us[j] < trial_us[i]) {
        double tu = trial_us[i]; trial_us[i] = trial_us[j]; trial_us[j] = tu;
        double trp = trial_rps[i]; trial_rps[i] = trial_rps[j]; trial_rps[j] = trp;
      }
    }
  }
  int mid = n_trials / 2;
  fprintf(stderr, "train: %lld iterations, %lld batches (%.4f us/req, %.1f req/s)\n",
          total_iters, total_batches, trial_us[mid], trial_rps[mid]);
  return 0;
}
