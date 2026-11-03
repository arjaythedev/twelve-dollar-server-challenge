// API-level tests: drives api_handle over a (copy of the) seed DB and checks
// exact response bytes, including golden files. CachyOS/Linux only (sqlite3).
//   cc -O1 -std=gnu11 -Wall -Wextra -Werror -Isrc tests/api_test.c src/api.c src/db.c src/jwt.c src/json.c src/http.c -lsqlite3 -o /tmp/api_test
//   cp seed/feed.db /tmp/t.db && /tmp/api_test /tmp/t.db <repo>/test/golden twelve-dollar-challenge
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include "api.h"
#include "http.h"
#include "jwt.h"

static int fails = 0;
static int total = 0;
#define CHECK(cond, ...) do { total++; \
  if (!(cond)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static api_t G;
static char Gsc[API_SCRATCH];
static const char *Gsec;
static uint64_t Gnow;

// Mint a token. variant: 0 normal, 1 expired, 2 numeric-sub, 3 no-username, 4 alg-none, 5 bad-sig.
static size_t mint(char *tok, int variant, const char *uid, const char *user) {
  char h[128], p[768];
  long long exp = (variant == 1) ? (long long)Gnow - 3600 : (long long)Gnow + 3600;
  int hl, pl;
  if (variant == 4) hl = snprintf(h, sizeof h, "{\"alg\":\"none\",\"typ\":\"JWT\"}");
  else hl = snprintf(h, sizeof h, "{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
  if (variant == 2) pl = snprintf(p, sizeof p, "{\"sub\":123,\"username\":\"%s\",\"iat\":1000,\"exp\":%lld}", user, exp);
  else if (variant == 3) pl = snprintf(p, sizeof p, "{\"sub\":\"%s\",\"iat\":1000,\"exp\":%lld}", uid, exp);
  else pl = snprintf(p, sizeof p, "{\"sub\":\"%s\",\"username\":\"%s\",\"iat\":1000,\"exp\":%lld}", uid, user, exp);
  char hb[256], pb[1024];
  size_t hbl = b64url_enc((uint8_t *)h, (size_t)hl, hb);
  size_t pbl = b64url_enc((uint8_t *)p, (size_t)pl, pb);
  char msg[1400];
  memcpy(msg, hb, hbl); msg[hbl] = '.'; memcpy(msg + hbl + 1, pb, pbl);
  size_t mlen = hbl + 1 + pbl;
  char sb[128]; size_t sbl;
  if (variant == 4) { memcpy(sb, "e30", 3); sbl = 3; }
  else {
    uint8_t mac[32];
    hmac_sha256((uint8_t *)Gsec, strlen(Gsec), (uint8_t *)msg, mlen, mac);
    sbl = b64url_enc(mac, 32, sb);
  }
  memcpy(tok, msg, mlen); tok[mlen] = '.';
  memcpy(tok + mlen + 1, sb, sbl);
  size_t tlen = mlen + 1 + sbl;
  if (variant == 5) tok[tlen - 1] = (char)(tok[tlen - 1] == 'A' ? 'B' : 'A');
  return tlen;
}

// Build raw request, parse, handle. Returns resp (body points into Gsc).
static resp_t call(const char *method, const char *path, const char *auth, const char *body) {
  static char raw[32768];
  int hl;
  if (body)
    hl = snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: x\r\n%s%s%sContent-Length: %zu\r\n\r\n%s",
                  method, path, auth ? "Authorization: " : "", auth ? auth : "",
                  auth ? "\r\n" : "", strlen(body), body);
  else if (auth)
    hl = snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: x\r\nAuthorization: %s\r\n\r\n",
                  method, path, auth);
  else
    hl = snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: x\r\n\r\n", method, path);
  req_t rq;
  int rc = http_parse(raw, (size_t)hl, &rq);
  if (rc != 1) { fprintf(stderr, "test driver http_parse failed for %s %s\n", method, path); exit(1); }
  return api_handle(&G, &rq);
}

static char *readfile(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)s + 1);
  if (fread(b, 1, (size_t)s, f) != (size_t)s) { fclose(f); free(b); return NULL; }
  fclose(f); b[s] = 0; *n = (size_t)s;
  return b;
}

int main(int argc, char **argv) {
  if (argc != 4) { fprintf(stderr, "usage: api_test <db> <golden-dir> <jwt-secret>\n"); return 2; }
  Gsec = argv[3];
  Gnow = (uint64_t)time(NULL);
  static db_t D;
  if (db_open(&D, argv[1]) != 0) return 2;
  G.db = &D; G.jwt_secret = (uint8_t *)Gsec; G.secret_len = strlen(Gsec);
  G.now_s = Gnow; G.now_mono = Gnow; G.boot_mono = Gnow - 42; G.scratch = Gsc;

  char tok[2048], auth[2200];
  size_t tl = mint(tok, 0, "1", "golden_ember_1");
  tok[tl] = 0;
  snprintf(auth, sizeof auth, "Bearer %s", tok);

  // --- golden reads (must run before any writes) ---
  resp_t r = call("GET", "/feed", NULL, NULL);
  size_t gn = 0; char *gold = readfile(argv[2], &gn); // placeholder
  (void)gold;
  {
    char fp[1024]; snprintf(fp, sizeof fp, "%s/feed.json", argv[2]);
    size_t fn = 0; char *fb = readfile(fp, &fn);
    CHECK(fb && r.status == 200 && r.body_len == fn && !memcmp(r.body, fb, fn), "feed == golden (%d %zu vs %zu)", r.status, r.body_len, fn);
    free(fb);
  }
  DIR *gd = opendir(argv[2]);
  struct dirent *de;
  int gold_n = 0;
  while (gd && (de = readdir(gd))) {
    if (strncmp(de->d_name, "post-", 5) != 0) continue;
    char *dot = strrchr(de->d_name, '.');
    if (!dot || strcmp(dot, ".json") != 0) continue;
    char id[32]; size_t il = (size_t)(dot - (de->d_name + 5));
    if (il >= sizeof id) continue;
    memcpy(id, de->d_name + 5, il); id[il] = 0;
    char path[64]; snprintf(path, sizeof path, "/posts/%s", id);
    r = call("GET", path, NULL, NULL);
    char fp[1024]; snprintf(fp, sizeof fp, "%s/%s", argv[2], de->d_name);
    size_t fn = 0; char *fb = readfile(fp, &fn);
    CHECK(fb && r.status == 200 && r.body_len == fn && !memcmp(r.body, fb, fn), "%s == golden", path);
    free(fb); gold_n++;
  }
  if (gd) closedir(gd);
  CHECK(gold_n > 0, "found golden post files");

  // --- health / errors ---
  r = call("GET", "/health", NULL, NULL);
  CHECK(r.status == 200 && r.body_len > 30 && !memcmp(r.body, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":42}", 38), "health %d %.*s", r.status, (int)r.body_len, r.body);
  r = call("GET", "/nope", NULL, NULL);
  CHECK(r.status == 404 && r.body_len == 21 && !memcmp(r.body, "{\"error\":\"not found\"}", 21), "404");
  const char *badids[] = { "/posts/0", "/posts/-1", "/posts/abc", "/posts/1.5" };
  for (int i = 0; i < 4; i++) {
    r = call("GET", badids[i], NULL, NULL);
    CHECK(r.status == 400 && !memcmp(r.body, "{\"error\":\"invalid post id\"}", 26), "bad id %s", badids[i]);
  }
  r = call("GET", "/posts/999999999", NULL, NULL);
  CHECK(r.status == 404 && !memcmp(r.body, "{\"error\":\"post not found\"}", 26), "missing post");

  // --- auth matrix ---
  char bad[2200];
  r = call("POST", "/posts", NULL, "{\"body\":\"x\"}");
  CHECK(r.status == 401 && !memcmp(r.body, "{\"error\":\"missing bearer token\"}", 32), "no token");
  r = call("POST", "/posts", "Basic abc", "{\"body\":\"x\"}");
  CHECK(r.status == 401, "basic auth");
  r = call("POST", "/posts", "Bearer abc", "{\"body\":\"x\"}");
  CHECK(r.status == 401 && !memcmp(r.body, "{\"error\":\"invalid or expired token\"}", 36), "garbage token");
  tl = mint(tok, 5, "1", "golden_ember_1"); tok[tl] = 0;
  snprintf(bad, sizeof bad, "Bearer %s", tok);
  r = call("POST", "/posts", bad, "{\"body\":\"x\"}");
  CHECK(r.status == 401, "bad sig");
  tl = mint(tok, 4, "1", "golden_ember_1"); tok[tl] = 0;
  snprintf(bad, sizeof bad, "Bearer %s", tok);
  r = call("POST", "/posts", bad, "{\"body\":\"x\"}");
  CHECK(r.status == 401, "alg none");
  tl = mint(tok, 1, "1", "golden_ember_1"); tok[tl] = 0;
  snprintf(bad, sizeof bad, "Bearer %s", tok);
  r = call("POST", "/posts", bad, "{\"body\":\"x\"}");
  CHECK(r.status == 401, "expired");
  tl = mint(tok, 2, "1", "golden_ember_1"); tok[tl] = 0;
  snprintf(bad, sizeof bad, "Bearer %s", tok);
  r = call("POST", "/posts", bad, "{\"body\":\"x\"}");
  CHECK(r.status == 401 && !memcmp(r.body, "{\"error\":\"invalid token payload\"}", 33), "numeric sub");
  tl = mint(tok, 3, "1", "golden_ember_1"); tok[tl] = 0;
  snprintf(bad, sizeof bad, "Bearer %s", tok);
  r = call("POST", "/posts", bad, "{\"body\":\"x\"}");
  CHECK(r.status == 401, "no username");
  r = call("POST", "/posts/abc/like", NULL, NULL);
  CHECK(r.status == 401, "auth before id");

  // --- create validation ---
  tl = mint(tok, 0, "1", "golden_ember_1"); tok[tl] = 0;
  snprintf(auth, sizeof auth, "Bearer %s", tok);
  r = call("POST", "/posts", auth, "{\"body\":\"\"}");
  CHECK(r.status == 400 && !memcmp(r.body, "{\"error\":\"body is required\"}", 28), "empty body");
  r = call("POST", "/posts", auth, "{\"body\":\"   \"}");
  CHECK(r.status == 400, "ws body");
  r = call("POST", "/posts", auth, "{\"body\":123}");
  CHECK(r.status == 400, "numeric body");
  r = call("POST", "/posts", auth, "{}");
  CHECK(r.status == 400, "missing body");
  r = call("POST", "/posts", auth, "{bad json");
  CHECK(r.status == 400 && !memcmp(r.body, "{\"error\":\"malformed JSON body\"}", 31), "malformed");
  static char b501[600];
  memset(b501, 'a', 501); b501[501] = 0;
  static char j501[640];
  snprintf(j501, sizeof j501, "{\"body\":\"%s\"}", b501);
  r = call("POST", "/posts", auth, j501);
  CHECK(r.status == 400 && !memcmp(r.body, "{\"error\":\"body must be at most 500 characters\"}", 46), "501 chars");

  // --- create special chars + 500-char post ---
  r = call("POST", "/posts", auth, "{\"body\":\"  test: <b>&amp;</b> \\\"quoted\\\" / caf\xc3\xa9 \xe2\x9c\x93  \"}");
  CHECK(r.status == 201 && strstr(r.body, "\"body\":\"test: <b>&amp;</b> \\\"quoted\\\" / caf\xc3\xa9 \xe2\x9c\x93\"") != NULL,
        "special chars trimmed: %.*s", (int)r.body_len, r.body);
  if (db_commit(&D) != 0) { printf("commit failed\n"); return 1; }
  static char b500[520];
  memset(b500, 'b', 500); b500[500] = 0;
  static char j500[640];
  snprintf(j500, sizeof j500, "{\"body\":\"  %s \"}", b500);
  r = call("POST", "/posts", auth, j500);
  CHECK(r.status == 201, "create 500 %d", r.status);
  // extract id + created_at, verify exact shape + format
  long long nid = 0;
  char created[32] = "";
  if (r.status == 201) {
    const char *ip = strstr(r.body, "\"id\":");
    if (ip) nid = atoll(ip + 5);
    const char *cp = strstr(r.body, "\"created_at\":\"");
    if (cp) { memcpy(created, cp + 14, 24); created[24] = 0; }
  }
  int fmt = strlen(created) == 24 && created[4] == '-' && created[7] == '-' && created[10] == 'T'
            && created[13] == ':' && created[16] == ':' && created[19] == '.' && created[23] == 'Z';
  CHECK(nid > 500000 && fmt, "id/ts %lld %s", nid, created);
  char expect[1024];
  snprintf(expect, sizeof expect,
           "{\"post\":{\"id\":%lld,\"body\":\"%s\",\"created_at\":\"%s\",\"author\":\"golden_ember_1\",\"like_count\":0}}",
           nid, b500, created);
  CHECK(r.body_len == strlen(expect) && !memcmp(r.body, expect, r.body_len), "create exact bytes");
  if (db_commit(&D) != 0) { printf("commit failed\n"); return 1; }

  // --- like flow ---
  char lpath[64];
  snprintf(lpath, sizeof lpath, "/posts/%lld/like", nid);
  r = call("POST", lpath, auth, NULL);
  char lexp[128];
  snprintf(lexp, sizeof lexp, "{\"liked\":true,\"already_liked\":false,\"post_id\":%lld}", nid);
  CHECK(r.status == 201 && r.body_len == strlen(lexp) && !memcmp(r.body, lexp, r.body_len), "like first %d %.*s", r.status, (int)r.body_len, r.body);
  if (db_commit(&D) != 0) { printf("commit failed\n"); return 1; }
  r = call("POST", lpath, auth, NULL);
  snprintf(lexp, sizeof lexp, "{\"liked\":true,\"already_liked\":true,\"post_id\":%lld}", nid);
  CHECK(r.status == 200 && r.body_len == strlen(lexp) && !memcmp(r.body, lexp, r.body_len), "like repeat");
  if (db_commit(&D) != 0) { printf("commit failed\n"); return 1; }
  r = call("POST", "/posts/999999999/like", auth, NULL);
  CHECK(r.status == 404, "like missing");
  r = call("POST", "/posts/abc/like", auth, NULL);
  CHECK(r.status == 400, "like bad id");
  // read back with like_count 1
  char gpath[64];
  snprintf(gpath, sizeof gpath, "/posts/%lld", nid);
  r = call("GET", gpath, NULL, NULL);
  snprintf(expect, sizeof expect,
           "{\"post\":{\"id\":%lld,\"body\":\"%s\",\"created_at\":\"%s\",\"author\":\"golden_ember_1\",\"like_count\":1}}",
           nid, b500, created);
  CHECK(r.status == 200 && r.body_len == strlen(expect) && !memcmp(r.body, expect, r.body_len), "readback like_count 1");
  // feed top
  r = call("GET", "/feed", NULL, NULL);
  { size_t el = strlen(expect);
    size_t inner_len = el - 9; // strip {"post": prefix and final }
    CHECK(r.status == 200 && r.body_len > 10 + inner_len
          && !memcmp(r.body, "{\"posts\":[", 10)
          && !memcmp(r.body + 10, expect + 8, inner_len), "feed top matches"); }
  // like golden post 1 -> count+1
  r = call("GET", "/posts/1", NULL, NULL);
  const char *lp = strstr(r.body, "\"like_count\":");
  long long before = lp ? atoll(lp + 13) : -1;
  r = call("POST", "/posts/1/like", auth, NULL);
  CHECK((r.status == 201 || r.status == 200), "like post 1 %d", r.status);
  if (db_commit(&D) != 0) { printf("commit failed\n"); return 1; }
  r = call("GET", "/posts/1", NULL, NULL);
  lp = strstr(r.body, "\"like_count\":");
  CHECK(lp && atoll(lp + 13) == before + 1, "seeded like counted %lld -> %s", before, lp ? lp : "?");

  db_close(&D);
  if (fails == 0) printf("ALL API TESTS PASSED (%d checks)\n", total);
  else printf("%d/%d API FAILURES\n", fails, total);
  return fails != 0;
}
