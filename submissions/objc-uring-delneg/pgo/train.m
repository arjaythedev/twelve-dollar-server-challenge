// PGO trainer (build time only, not linked into the server). Builds a synthetic
// database with the challenge schema (no seed data) and drives the server's
// own store, JSON and JWT code with a request mix like the load test.
#include "../src/store.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng % n); }

static void must(sqlite3 *db, const char *sql) {
    char *e = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &e) != SQLITE_OK) { fprintf(stderr, "%s: %s\n", sql, e); exit(1); }
}

static const char *SCHEMA =
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

static void build_db(const char *path, int users, int posts, int likes) {
    remove(path);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s-wal", path); remove(tmp);
    snprintf(tmp, sizeof tmp, "%s-shm", path); remove(tmp);
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) exit(1);
    must(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;");
    must(db, SCHEMA);
    must(db, "BEGIN");
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "INSERT INTO users (id, username, created_at) VALUES (?1, ?2, '2025-01-01T00:00:00.000Z')", -1, &s, NULL);
    for (int i = 1; i <= users; i++) {
        char name[64];
        int l = snprintf(name, sizeof name, "trainer_user_%d", i);
        sqlite3_bind_int(s, 1, i); sqlite3_bind_text(s, 2, name, l, SQLITE_STATIC);
        sqlite3_step(s); sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    sqlite3_prepare_v2(db, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)", -1, &s, NULL);
    static const char *words[] = {"tiny", "server", "fast", "cache", "queue", "ring", "page", "query", "token", "feed", "like", "post"};
    for (int i = 1; i <= posts; i++) {
        char body[256], ts[32];
        int l = 0, w = 4 + (int)rnd(14);
        for (int k = 0; k < w; k++) l += snprintf(body + l, sizeof body - (size_t)l, "%s%s", k ? " " : "", words[rnd(12)]);
        long t = 1735689600L + i * 60L;
        time_t tt = t;
        struct tm tm; gmtime_r(&tt, &tm);
        int tl = (int)strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
        tl += snprintf(ts + tl, sizeof ts - (size_t)tl, ".%03dZ", (int)rnd(1000));
        sqlite3_bind_int(s, 1, i); sqlite3_bind_int(s, 2, 1 + (int)rnd((uint32_t)users));
        sqlite3_bind_text(s, 3, body, l, SQLITE_STATIC); sqlite3_bind_text(s, 4, ts, tl, SQLITE_STATIC);
        sqlite3_step(s); sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, '2025-06-01T00:00:00.000Z')", -1, &s, NULL);
    for (int i = 0; i < likes; i++) {
        sqlite3_bind_int(s, 1, 1 + (int)rnd((uint32_t)users)); sqlite3_bind_int(s, 2, 1 + (int)rnd((uint32_t)posts));
        sqlite3_step(s); sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    must(db, "COMMIT; ANALYZE; PRAGMA journal_mode=WAL;");
    sqlite3_close(db);
}

// HS256 token for the trainer's own secret, made with the server's SHA-256.
static void b64url(const uint8_t *in, size_t n, char *out) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i+1] << 8 | in[i+2];
        out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63]; out[o++] = a[v & 63];
    }
    if (n - i == 1) { uint32_t v = (uint32_t)in[i] << 16; out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; }
    else if (n - i == 2) { uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i+1] << 8; out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63]; }
    out[o] = 0;
}
static void hmac(const char *key, const char *msg, uint8_t out[32]) {
    uint8_t k[64] = {0}, ip[64], op[64];
    memcpy(k, key, strlen(key));
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    TDSha256 c; td_sha256_init(&c); td_sha256_update(&c, ip, 64); td_sha256_update(&c, msg, strlen(msg)); td_sha256_final(&c, out);
    td_sha256_init(&c); td_sha256_update(&c, op, 64); td_sha256_update(&c, out, 32); td_sha256_final(&c, out);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "train.db";
    int ops = argc > 2 ? atoi(argv[2]) : 300000;
    build_db(path, 20000, 200000, 800000);
    TDStore *st = [[TDStore make] initWithPath:path];
    TDAuth *auth = [[TDAuth make] initWithSecret:"pgo-trainer-secret"];
    char tok[1024], hdr[1100], sig[64], pl[256];
    const char *h = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    int64_t now = time(NULL);
    char hb[128], pb[512];
    TDBuf b = {0}, req = {0};
    int64_t newest = 200000;
    for (int i = 0; i < ops;) {
        [st begin];
        for (int k = 0; k < 32 && i < ops; k++, i++) {
            b.len = 0;
            uint32_t r = rnd(217);
            if (r < 100) [st feed:&b];
            else if (r < 200) [st post:newest - rnd(20) into:&b];
            else {
                int uid = 1 + (int)rnd(20000);
                int pn = snprintf(pl, sizeof pl, "{\"sub\":\"%d\",\"username\":\"trainer_user_%d\",\"iat\":%lld,\"exp\":%lld}",
                                  uid, uid, (long long)now, (long long)now + 3600);
                (void)pn;
                b64url((const uint8_t *)h, strlen(h), hb);
                b64url((const uint8_t *)pl, strlen(pl), pb);
                snprintf(tok, sizeof tok, "%s.%s", hb, pb);
                uint8_t mac[32]; hmac("pgo-trainer-secret", tok, mac); b64url(mac, 32, sig);
                int hl = snprintf(hdr, sizeof hdr, "Bearer %s.%s", tok, sig);
                int64_t vid; char name[TD_UNAME_MAX]; uint32_t nl;
                if ([auth verify:hdr length:(uint32_t)hl now:now uid:&vid name:name nameLen:&nl] != TD_AUTH_OK) { fprintf(stderr, "jwt\n"); return 1; }
                if (r < 215) [st like:vid post:newest - rnd(20) into:&b];
                else {
                    req.len = 0;
                    td_buf_lit(&req, "{\"body\":\"  trainer says hi \\\"quoted\\\" caf\\u00e9  \"}");
                    TDJField f[1] = {{ .key = "body", .keylen = 4 }};
                    char out[600];
                    if (td_json_object(req.p, req.len, f, 1) == 1) {
                        int l = td_json_unescape(f[0].v, f[0].vlen, out, sizeof out);
                        if ([st create:vid name:name length:nl body:out length:(uint32_t)l into:&b] == 201) newest++;
                    }
                }
            }
        }
        [st commit];
    }
    return 0;
}
