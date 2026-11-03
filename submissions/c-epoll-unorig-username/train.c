// Profile training, compiled into the instrumented build only (build.sh, -DPGO_TRAIN). It serves a
// request mix from memory against a synthetic database it creates (no challenge data), so the
// optimizer sees the real hot paths: HTTP and JSON parsing, the JWT check, SQLite reads and writes,
// response building and group commits.
static void train(void) {
  const char *path = getenv("PGO_TRAIN_DB");
  static const char *tok = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiI3IiwidXNlcm5hbWUiOiJ0cmFpbmVyIiwiZXhwIjo0MTAyNDQ0ODAwfQ.fmsWbbPWXeDx0P3cbcdmOTM6bC10yq05y3vDN36tSfU";
  sqlite3 *d; sqlite3_initialize(); unlink(path);
  if (sqlite3_open(path, &d) != SQLITE_OK || sqlite3_exec(d,
      "PRAGMA journal_mode=WAL;"
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
      "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
      "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC); CREATE INDEX posts_user_id_idx ON posts (user_id); CREATE INDEX likes_post_id_idx ON likes (post_id);"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000) INSERT INTO users (id, username) SELECT i, 'user' || i FROM n;"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000) INSERT INTO posts (user_id, body, created_at)"
      "  SELECT 1 + i % 20000, 'synthetic post ' || i || ' \"quoted\" text', strftime('%Y-%m-%dT%H:%M:%fZ', 1700000000 + i * 60, 'unixepoch') FROM n;"
      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 600000) INSERT OR IGNORE INTO likes (user_id, post_id)"
      "  SELECT 1 + abs(random()) % 20000, 1 + abs(random()) % 200000 FROM n;", NULL, NULL, NULL) != SQLITE_OK) exit(1);
  sqlite3_close(d);
  memset(b64t, -1, sizeof b64t);
  for (int i = 0; i < 64; i++) b64t[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]] = i;
  hmac_key("pgo-training-secret"); now_s = time(NULL);
  db_open(path); arena = malloc(BATCH_OUT);
  char req[1024]; int closing = 0;
  for (int i = 1; i <= 300000; i++) {
    int k = i % 217, id = 1 + (i * 7919) % 200000, n;
    if (k < 100) n = snprintf(req, sizeof req, "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n");
    else if (k < 200) n = snprintf(req, sizeof req, "GET /posts/%d HTTP/1.1\r\nHost: x\r\n\r\n", id);
    else if (k < 215) n = snprintf(req, sizeof req, "POST /posts/%d/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", id, tok);
    else { char b[64]; int bl = snprintf(b, sizeof b, "{\"body\":\"training post %d\"}", i);
      n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", tok, bl, b); }
    serve(-1, req, n, &closing);
    if (i % 64 == 0) { nsegs = 0; flush_batch(); }
  }
  exit(0);
}
