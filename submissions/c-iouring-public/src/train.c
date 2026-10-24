// train.c — PGO training driver, used only by build.sh: `server --train <dir>`.
//
// 1. Builds a synthetic database in <dir> with the challenge schema but its own random data (generator after
//    the engine's gen_train.c: own seed, own words, nothing from seed/feed.db).
// 2. Signs its own tokens with a random secret (the scalar HMAC from jwt.c).
// 3. Forks the real server on a loopback port and drives it with batched, pipelined traffic in roughly the scored
//    mix, plus the error paths the test suite hits. The profile records decisions about code paths, never data.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "jwt.h"

struct sqlite3;
struct sqlite3_stmt;  // the SQLite API lives in the other TU (db.c); declare what we use
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open(const char *, sqlite3 **);
int sqlite3_close(sqlite3 *);
int sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int sqlite3_bind_int(sqlite3_stmt *, int, int);
int sqlite3_bind_double(sqlite3_stmt *, int, double);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
int sqlite3_step(sqlite3_stmt *);
int sqlite3_reset(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
#define TR_TRANSIENT ((void (*)(void *)) - 1)

static unsigned long long tr_s = 0xC0FFEE1234567ULL;
static unsigned long long tr_rnd(void) {
  tr_s ^= tr_s << 13;
  tr_s ^= tr_s >> 7;
  tr_s ^= tr_s << 17;
  return tr_s;
}

static const char TR_SCHEMA[] =  // challenge/schema.sql (the fixed schema)
    "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
    " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
    "CREATE INDEX posts_user_id_idx ON posts (user_id);"
    "CREATE INDEX likes_post_id_idx ON likes (post_id);";

enum { TR_USERS = 4000, TR_POSTS = 30000, TR_LIKES = 120000 };

static int tr_make_db(const char *path) {
  char p2[1100];
  remove(path);
  snprintf(p2, sizeof p2, "%s-wal", path);
  remove(p2);
  snprintf(p2, sizeof p2, "%s-shm", path);
  remove(p2);
  sqlite3 *db;
  if (sqlite3_open(path, &db)) return -1;
  if (sqlite3_exec(db, TR_SCHEMA, 0, 0, 0)) return -1;  // default journal; the bulk insert is one transaction
  static const char *w[] = {"lorem", "ipsum",  "dolor", "sit",         "amet",  "quux",         "zap",
                            "frob",  "wibble", "\"q\"", "back\\slash", "tab\t", "n\xc3\xa4ive", "\xe2\x82\xac"};
  sqlite3_exec(db, "BEGIN", 0, 0, 0);
  sqlite3_stmt *st;
  char buf[600];
  sqlite3_prepare_v2(db, "INSERT INTO users (id, username, created_at) VALUES (?, ?, '2024-01-01T00:00:00.000Z')", -1,
                     &st, 0);
  for (int i = 1; i <= TR_USERS; i++) {
    snprintf(buf, sizeof buf, "trainer_%c%c_%d", 'a' + (int)(tr_rnd() % 26), 'a' + (int)(tr_rnd() % 26), i);
    sqlite3_bind_int(st, 1, i);
    sqlite3_bind_text(st, 2, buf, -1, TR_TRANSIENT);
    sqlite3_step(st);
    sqlite3_reset(st);
  }
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db,
                     "INSERT INTO posts (id, user_id, body, created_at) VALUES (?, ?, ?, "
                     "strftime('%Y-%m-%dT%H:%M:%fZ', 1704067200 + ?1 * 37.5 + ?4, 'unixepoch'))",
                     -1, &st, 0);
  for (int i = 1; i <= TR_POSTS; i++) {
    int len = 0, words = 1 + (int)(tr_rnd() % 40);
    for (int k = 0; k < words && len < 400; k++)
      len += snprintf(buf + len, sizeof buf - (size_t)len, "%s%s", k ? " " : "", w[tr_rnd() % 14]);
    sqlite3_bind_int(st, 1, i);
    sqlite3_bind_int(st, 2, 1 + (int)(tr_rnd() % TR_USERS));
    sqlite3_bind_text(st, 3, buf, len, TR_TRANSIENT);
    sqlite3_bind_double(st, 4, (double)(tr_rnd() % 30000) / 1000.0);
    sqlite3_step(st);
    sqlite3_reset(st);
  }
  sqlite3_finalize(st);
  sqlite3_prepare_v2(
      db, "INSERT OR IGNORE INTO likes (user_id, post_id, created_at) VALUES (?, ?, '2024-06-01T00:00:00.000Z')", -1,
      &st, 0);
  for (long i = 0; i < TR_LIKES; i++) {
    sqlite3_bind_int(st, 1, 1 + (int)(tr_rnd() % TR_USERS));
    sqlite3_bind_int(st, 2, 1 + (int)(tr_rnd() % TR_POSTS));
    sqlite3_step(st);
    sqlite3_reset(st);
  }
  sqlite3_finalize(st);
  sqlite3_exec(db, "COMMIT; ANALYZE; PRAGMA journal_mode=WAL;", 0, 0, 0);
  sqlite3_close(db);
  return 0;
}

// ---------------------------------------------------------------- tokens
static int tr_b64url(char *o, const unsigned char *s, int n) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  int k = 0, i = 0;
  for (; i + 2 < n; i += 3) {
    unsigned v = s[i] << 16 | s[i + 1] << 8 | s[i + 2];
    o[k++] = T[v >> 18];
    o[k++] = T[v >> 12 & 63];
    o[k++] = T[v >> 6 & 63];
    o[k++] = T[v & 63];
  }
  if (n - i == 1) {
    unsigned v = s[i] << 16;
    o[k++] = T[v >> 18];
    o[k++] = T[v >> 12 & 63];
  } else if (n - i == 2) {
    unsigned v = s[i] << 16 | s[i + 1] << 8;
    o[k++] = T[v >> 18];
    o[k++] = T[v >> 12 & 63];
    o[k++] = T[v >> 6 & 63];
  }
  return k;
}

static int tr_sign(char *out, const jwt_key_t *k, const char *payload) {
  int n = tr_b64url(out, (const unsigned char *)"{\"alg\":\"HS256\",\"typ\":\"JWT\"}", 27);
  out[n++] = '.';
  n += tr_b64url(out + n, (const unsigned char *)payload, (int)strlen(payload));
  const uint8_t *msg = (const uint8_t *)out;
  int len = n;
  uint32_t mac[1][8];
  jwt_hmac_scalar(k, 1, &msg, &len, mac);
  unsigned char sig[32];
  for (int j = 0; j < 8; j++) {
    sig[4 * j] = (unsigned char)(mac[0][j] >> 24);
    sig[4 * j + 1] = (unsigned char)(mac[0][j] >> 16);
    sig[4 * j + 2] = (unsigned char)(mac[0][j] >> 8);
    sig[4 * j + 3] = (unsigned char)mac[0][j];
  }
  out[n++] = '.';
  n += tr_b64url(out + n, sig, 32);
  out[n] = 0;
  return n;
}

// ---------------------------------------------------------------- client
enum { TR_CONNS = 96, TR_ROUNDS = 400 };
static char tr_tok[64][512];

static int tr_req(char *o, int conn, int i) {  // one request of the scored mix (46/46/7/1) plus some error paths
  int r = (int)(tr_rnd() % 1000), u = (int)(tr_rnd() % 64), id = 1 + (int)(tr_rnd() % TR_POSTS);
  (void)conn;
  (void)i;
  if (r < 450)
    return sprintf(o, "GET /feed HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: trainer\r\nAccept-Encoding: gzip\r\n\r\n");
  if (r < 910)
    return sprintf(
        o, "GET /posts/%d HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: trainer\r\nAccept-Encoding: gzip\r\n\r\n", id);
  if (r < 975)
    return sprintf(o,
                   "POST /posts/%d/like HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: trainer\r\nAuthorization: Bearer "
                   "%s\r\nContent-Length: 0\r\n\r\n",
                   id, tr_tok[u]);
  if (r < 988) {
    char b[300];
    int bl =
        sprintf(b, "{\"body\":\"training post %llu with some words \\\"quoted\\\" caf\\u00e9\"}", tr_rnd() % 100000);
    return sprintf(o,
                   "POST /posts HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer %s\r\nContent-Type: "
                   "application/json\r\nContent-Length: %d\r\n\r\n%s",
                   tr_tok[u], bl, b);
  }
  switch (r % 8) {  // error paths and the slow parser
    case 0: return sprintf(o, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
    case 1: return sprintf(o, "GET /posts/abc HTTP/1.1\r\nHost: x\r\n\r\n");
    case 2: return sprintf(o, "POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}");
    case 3:
      return sprintf(o, "POST /posts HTTP/1.1\r\nAuthorization: Bearer abc.def.ghi\r\nContent-Length: 2\r\n\r\n{}");
    case 4:
      return sprintf(o, "POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: 9\r\n\r\n{bad json",
                     tr_tok[u]);
    case 5: return sprintf(o, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    case 6: return sprintf(o, "GET  /feed HTTP/1.1\r\nHost: x\r\n\r\n");  // non-canonical: general path
    default: return sprintf(o, "GET /posts/%d HTTP/1.1\r\nhost: x\r\nconnection: keep-alive\r\n\r\n", id);
  }
}

// Read until `want` complete responses have arrived (Content-Length framing). 0 or -1.
static long tr_2xx, tr_201;
static int tr_read(int fd, int want, char *buf, size_t cap) {
  size_t have = 0;
  while (want > 0) {
    char *h;
    size_t used = 0;
    while (want > 0 && (h = memmem(buf + used, have - used, "\r\n\r\n", 4))) {
      char *cl = memmem(buf + used, (size_t)(h - buf - used), "Content-Length: ", 16);
      size_t len = cl ? strtoul(cl + 16, 0, 10) : 0, end = (size_t)(h + 4 - buf) + len;
      if (end > have) break;
      if (!memcmp(buf + used, "HTTP/1.1 2", 10)) {
        tr_2xx++;
        if (buf[used + 11] == '1') tr_201++;
      }
      used = end;
      want--;
    }
    memmove(buf, buf + used, have - used);
    have -= used;
    if (!want) break;
    ssize_t r = read(fd, buf + have, cap - have);
    if (r <= 0) return -1;
    have += (size_t)r;
  }
  return 0;
}

static int tr_connect(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  for (int t = 0; t < 200; t++) {
    if (!connect(fd, (struct sockaddr *)&a, sizeof a)) return fd;
    usleep(20000);
    close(fd);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  }
  return -1;
}

int train_main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: server --train <dir> [rounds]\n");
    return 2;
  }
  int rounds = argc > 2 ? atoi(argv[2]) : TR_ROUNDS;
  char db[1024];
  snprintf(db, sizeof db, "%s/train.db", argv[1]);
  if (tr_make_db(db)) {
    fprintf(stderr, "train: cannot build %s\n", db);
    return 1;
  }
  char secret[33];
  tr_s ^= (unsigned long long)time(NULL) * 2654435761u ^ (unsigned long long)getpid();
  for (int i = 0; i < 32; i++)
    secret[i] = (char)('a' + tr_rnd() % 26);
  secret[32] = 0;
  jwt_key_t k;
  jwt_key_init(&k, secret, 32);
  long now = (long)time(NULL);
  for (int u = 0; u < 64; u++) {
    char pl[200];
    snprintf(pl, sizeof pl, "{\"sub\":\"%d\",\"username\":\"trainer_%d\",\"iat\":%ld,\"exp\":%ld}", 1 + u * 7, u, now,
             now + 3600);
    tr_sign(tr_tok[u], &k, pl);
  }
  int port = 20000 + (int)(tr_rnd() % 20000);
  pid_t pid = fork();
  if (pid == 0) {
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    setenv("SQLITE_PATH", db, 1);
    setenv("JWT_SECRET", secret, 1);
    setenv("HOST", "127.0.0.1", 1);
    setenv("PORT", ps, 1);
    execl(argv[-1], argv[-1], (char *)0);  // argv[-1] = the server binary (main passed argv + 1)
    _exit(127);
  }
  int fds[TR_CONNS];
  static char out[TR_CONNS][8192], in[1 << 20];
  for (int c = 0; c < TR_CONNS; c++)
    if ((fds[c] = tr_connect(port)) < 0) {
      fprintf(stderr, "train: connect failed\n");
      kill(pid, SIGKILL);
      return 1;
    }
  long total = 0;
  int bad = 0;
  for (int r = 0; r < rounds && !bad; r++) {
    int want[TR_CONNS];
    for (int c = 0; c < TR_CONNS; c++) {  // 1-3 pipelined requests per connection, written before any read
      int n = 0, k2 = 1 + (int)(tr_rnd() % 4 == 0) + (int)(tr_rnd() % 8 == 0);
      for (int j = 0; j < k2; j++)
        n += tr_req(out[c] + n, c, j);
      want[c] = k2;
      if (write(fds[c], out[c], (size_t)n) != n) bad = 1;
    }
    for (int c = 0; c < TR_CONNS && !bad; c++) {
      if (tr_read(fds[c], want[c], in, sizeof in)) bad = 1;
      total += want[c];
    }
  }
  // HTTP/1.0 and Connection: close (each closes its connection)
  for (int c = 0; c < 4; c++) {
    int fd = tr_connect(port);
    const char *q = c & 1 ? "GET /feed HTTP/1.0\r\n\r\n" : "GET /posts/7 HTTP/1.1\r\nConnection: close\r\n\r\n";
    if (write(fd, q, strlen(q)) > 0) tr_read(fd, 1, in, sizeof in);
    close(fd);
  }
  for (int c = 0; c < TR_CONNS; c++)
    close(fds[c]);
  kill(pid, SIGTERM);
  int st = 0;
  waitpid(pid, &st, 0);
  fprintf(stderr, "train: %ld requests (%ld 2xx, %ld 201), %s, server exit %d\n", total, tr_2xx, tr_201,
          bad ? "client error" : "ok", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return bad || !WIFEXITED(st) || WEXITSTATUS(st) ? 1 : 0;
}
