/* Build-time profile training (PGO), used only by build.sh as `server --pgo-train <scratch.db>`.
 *
 * Creates a small synthetic database with the challenge schema, then pushes raw HTTP requests through
 * the same handle_request() the network path uses, in roughly the load test's mix (feed, post, like,
 * create, plus error paths). No sockets are involved; responses are discarded. */
#pragma once

static const char TRAIN_SCHEMA[] =
    "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
    "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
    "  body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
    "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
    "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
    "CREATE INDEX posts_user_id_idx ON posts (user_id);"
    "CREATE INDEX likes_post_id_idx ON likes (post_id);"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 5000)"
    "  INSERT INTO users (id, username, created_at) SELECT i, 'user_' || i, '2025-01-01T00:00:00.000Z' FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 50000)"
    "  INSERT INTO posts (id, user_id, body, created_at) SELECT i, 1 + abs(random()) % 5000,"
    "  substr('Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore "
    "et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex "
    "ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse.', 1, 20 + abs(random()) % 280),"
    "  strftime('%Y-%m-%dT%H:%M:%fZ', '2025-01-01', '+' || (i * 600) || ' seconds') FROM n;"
    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000)"
    "  INSERT OR IGNORE INTO likes (user_id, post_id, created_at)"
    "  SELECT 1 + abs(random()) % 5000, 1 + abs(random()) % 50000, '2025-06-01T00:00:00.000Z' FROM n;"
    "ANALYZE;"
    "PRAGMA journal_mode=WAL;";

static const char TRAIN_SECRET[] = "pgo-training-secret";

static void b64url_encode(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        *out++ = A[v >> 18]; *out++ = A[v >> 12 & 63]; *out++ = A[v >> 6 & 63]; *out++ = A[v & 63];
    }
    if (n - i == 1) { uint32_t v = in[i] << 16; *out++ = A[v >> 18]; *out++ = A[v >> 12 & 63]; }
    if (n - i == 2) { uint32_t v = in[i] << 16 | in[i + 1] << 8; *out++ = A[v >> 18]; *out++ = A[v >> 12 & 63]; *out++ = A[v >> 6 & 63]; }
    *out = 0;
}

static void train_token(int uid, char *out)
{
    char payload[160], p64[256];
    snprintf(payload, sizeof payload, "{\"sub\":\"%d\",\"username\":\"user_%d\",\"iat\":%ld,\"exp\":%ld}", uid, uid,
             (long)now_s, (long)now_s + 3600);
    b64url_encode((const uint8_t *)payload, strlen(payload), p64);
    int n = sprintf(out, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.%s", p64);
    uint8_t mac[32];
    hmac_sha256(&jwt_key, out, n, mac);
    out[n++] = '.';
    b64url_encode(mac, 32, out + n);
}

static void train_req(const char *req)
{
    conn_t *c = &conns[0];
    size_t n = strlen(req);
    if (handle_request(0, req, n) != n) { fprintf(stderr, "pgo-train: request not consumed: %.40s\n", req); exit(1); }
    c->out.len = 0;
    c->closing = 0;
    c->dirty = 0;
    ndirty = 0;
}

static int pgo_train(const char *path)
{
    char p2[4096];
    unlink(path);
    snprintf(p2, sizeof p2, "%s-wal", path); unlink(p2);
    snprintf(p2, sizeof p2, "%s-shm", path); unlink(p2);
    sqlite3 *d;
    if (sqlite3_open(path, &d) != SQLITE_OK) { fprintf(stderr, "pgo-train: cannot create %s\n", path); return 1; }
    exec_or_die(d, TRAIN_SCHEMA);
    sqlite3_close(d);

    update_clock();
    hmac_sha256_key(&jwt_key, (const uint8_t *)TRAIN_SECRET, strlen(TRAIN_SECRET));
    db_path = path;
    db_open();
    nslots = 16;
    conns = calloc(nslots, sizeof *conns);
    dirty = calloc(nslots, sizeof *dirty);
    conns[0].open = 1;

    char tok[512], req[2048];
    unsigned seed = 12345;
    int64_t top = 50000;
    for (int i = 0; i < 40000; i++) {
        int uid = 1 + rand_r(&seed) % 5000;
        train_req("GET /feed HTTP/1.1\r\nHost: localhost\r\nUser-Agent: k6/1.0\r\n\r\n");
        int64_t id = top - rand_r(&seed) % 20;
        snprintf(req, sizeof req, "GET /posts/%lld HTTP/1.1\r\nHost: localhost\r\nUser-Agent: k6/1.0\r\n\r\n", (long long)id);
        train_req(req);
        if (rand_r(&seed) % 100 < 15) {
            train_token(uid, tok);
            snprintf(req, sizeof req, "POST /posts/%lld/like HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\n"
                     "Content-Type: application/json\r\n\r\n", (long long)id, tok);
            train_req(req);
        }
        if (rand_r(&seed) % 100 < 2) {
            train_token(uid, tok);
            char body[200];
            int bl = snprintf(body, sizeof body, "{\"body\":\"user_%d says hi at 2026-01-01T00:00:00.000Z (VU %d, iter %d)\"}", uid, uid, i);
            snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\n"
                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", tok, bl, body);
            train_req(req);
            top++;
        }
        if (i % 500 == 0) {
            train_req("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
            train_req("GET /posts/abc HTTP/1.1\r\nHost: localhost\r\n\r\n");
            train_req("GET /posts/99999999 HTTP/1.1\r\nHost: localhost\r\n\r\n");
            train_req("POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer abc\r\nContent-Length: 2\r\n\r\n{}");
            train_req("GET /nope HTTP/1.1\r\nHost: localhost\r\n\r\n");
        }
        if (i % 4 == 3) end_batch();
    }
    end_batch();
    sqlite3_close(db);
    unlink(path);
    snprintf(p2, sizeof p2, "%s-wal", path); unlink(p2);
    return 0;
}
