# Repository Analysis — BinaryCore

This document is Step 1-3 of the BinaryCore engineering plan. It records the official requirements, the existing architecture, performance data, bottlenecks, opportunities, risks, and the implementation strategy. Every claim references an actual source file and line number.

---

## 1. Official Requirements

### 1.1 Challenge framing
- **README.md:3-10**: The $12 Server Challenge — how many users can one $12/month server (1 vCPU, 2 GB RAM) handle? Leaderboard at benchmarks.arjaythedev.com/12-dollar-server.
- **README.md:27-31**: Quick start — `bash seed/make-seed.sh` (node >= 18, sqlite3), `bash test/run.sh submissions/<folder>`, `k6 run -e VUS=1000 bench/load.js`.
- **README.md:33-65**: Rules. Any language/runtime. SQLite at `$SQLITE_PATH`, in-process only. No other database/cache/server. Schema fixed. Pass `test/test.sh` 100% with exact bytes. No caching across requests (README.md:44-47). Writes durable before response — WAL + `synchronous=NORMAL` or stronger, no `synchronous=OFF`/`journal_mode=OFF`/in-memory DB (README.md:49). Group commit allowed if each response waits for its commit (README.md:51). No hard-coded responses / benchmark detection (README.md:52).
- **README.md:54-64**: VM = Ubuntu 24.04 x86_64, 1 vCPU, 2 GB RAM, no swap. Nginx optional — behind Nginx on `127.0.0.1:3000` or direct on `0.0.0.0:80`. Direct saves Nginx CPU (14–20%) but the server must handle up to ~15,000 keep-alive connections itself (README.md:58-60). Built from source via `install.sh`/`build.sh`/`start.sh`, healthy within 60 seconds (README.md:62). No kernel tuning, no CPU pinning (README.md:64).
- **README.md:97-110**: Scoring. Warm-up 1,000 users for 2 min. Find limit with `bench/load.js` from 2,500 users doubling. Confirm with one 5-minute hold. Score = most users passing a 5-minute hold with p95 < 500 ms, p99 < 1 s, errors < 1%.

### 1.2 API specification (SPEC.md)
Runtime config (SPEC.md:8-16): `SQLITE_PATH`, `JWT_SECRET=twelve-dollar-challenge`, `HOST`, `PORT`. Listen on `HOST:PORT`, speak HTTP/1.1 with keep-alive. Compression optional.

Response contract (SPEC.md:23-32): Every body compact JSON, `Content-Type: application/json`. Key order exactly as shown. Post object: `{"id":<int>,"body":<string>,"created_at":<string>,"author":<string>,"like_count":<int>}`. `created_at` is UTC millisecond precision with `Z` suffix, stored in exactly this format; new rows get `strftime('%Y-%m-%dT%H:%M:%fZ','now')`. Escaping `<>&/` or non-ASCII optional; load-test payloads are plain ASCII.

Endpoints (SPEC.md:36-43):
| Request | Success | Body |
|---|---|---|
| `GET /health` | 200 | `{"status":"ok","db":"ok","uptime_s":<int>}` after `SELECT 1`; 503 `{"status":"degraded","db":"unreachable","error":<string>}` on failure |
| `GET /feed` | 200 | `{"posts":[<post> × 20]}` newest 20, `ORDER BY created_at DESC, id DESC` |
| `GET /posts/:id` | 200 | `{"post":<post>}` |
| `POST /posts` (auth) | 201 | `{"post":{"id":…,"body":<trimmed body>,"created_at":…,"author":<token username>,"like_count":0}}` |
| `POST /posts/:id/like` (auth) | 201 first / 200 repeat | `{"liked":true,"already_liked":<bool>,"post_id":<int>}` |

Reference queries (SPEC.md:46-53): feed uses `ORDER BY p.created_at DESC, p.id DESC LIMIT 20`; like_count via correlated subquery; post create `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`; like `INSERT INTO likes … ON CONFLICT DO NOTHING`.

Errors (SPEC.md:59-71): every error body `{"error":"<message>"}`. Unknown path → 404 `not found`. Invalid id → 400 `invalid post id`. Missing post → 404 `post not found`. No/malformed Bearer → 401 `missing bearer token`. Bad/expired/malformed JWT / non-HS256 → 401 `invalid or expired token`. Invalid payload → 401 `invalid token payload`. Invalid JSON body → 400 `malformed JSON body`. Missing/empty/non-string body → 400 `body is required`. Body > 500 chars trimmed → 400 `body must be at most 500 characters`. Anything else → 500 `internal server error`.
Auth checked before id on authenticated routes (SPEC.md:73): `POST /posts/abc/like` without token → 401; with valid token → 400.

Auth (SPEC.md:76-79): `Authorization: Bearer <jwt>`, HS256 with `JWT_SECRET`, payload `{"sub":"<user id>","username":"<name>","iat":…,"exp":…}`. Verify signature + `exp`, HS256 only. `sub` = integer user id; `username` used as-is, not looked up. No login endpoint. Pre-signed tokens for users 1–20,000 in `seed/tokens.json`.

### 1.3 Schema (schema.sql)
Fixed schema (schema.sql:2-2). Tables: `users(id, username, created_at)`, `posts(id, user_id, body, created_at)`, `likes(user_id, post_id, created_at)` with PK `(user_id, post_id)`. Indexes: `posts_created_at_id_idx (created_at DESC, id DESC)`, `posts_user_id_idx (user_id)`, `likes_post_id_idx (post_id)`. Timestamps TEXT in `YYYY-MM-DDTHH:MM:SS.mmmZ` UTC, which also sorts correctly (schema.sql:3).

### 1.4 Test suite (test/test.sh)
42 checks. Compares exact response bytes against `test/golden/*.json` on a fresh database, then writes a post and likes, so a second run against the same DB fails the golden checks. Needs curl, jq, openssl. JWT_SECRET defaults to the challenge secret.
- Golden reads (test.sh:52-57): `GET /feed` == `feed.json`; `GET /posts/1`, `/posts/250000`, `/posts/500000` == their golden files.
- Health/404/id validation (test.sh:60-66): `/health` → 200 with `{"status":"ok","db":"ok","uptime_s":<int>}`; `/nope` → 404 `not found`; ids `0`, `-1`, `abc`, `1.5` → 400 `invalid post id`; `/posts/999999999` → 404 `post not found`.
- Auth (test.sh:69-83): expired token → 401; bad sub → 401 `invalid token payload`; missing username → 401; alg=none → 401; no token → 401 `missing bearer token`; Basic auth → 401; garbage token → 401; bad signature → 401; `/posts/abc/like` no token → 401 (auth before id).
- Create validation (test.sh:87-93): empty/whitespace/numeric/missing body → 400 `body is required`; malformed JSON → 400 `malformed JSON body`; 501-char body → 400 `at most 500 characters`.
- Create/like/read-back (test.sh:96-123): special characters round-trip (trimmed); 500-char post → 201 with exact body; `created_at` format `YYYY-MM-DDTHH:MM:SS.mmmZ` and within 60s of now; like first → 201 `already_liked:false`, repeat → 200 `already_liked:true`; like missing post → 404; like bad id with token → 400 `invalid post id`; read back like_count=1; like on seeded post increments like_count; new post at top of feed; feed has 20 posts.

### 1.5 Test runner (test/run.sh)
Builds the submission (`build.sh`), starts it on a fresh copy of `seed/feed.db`, waits up to 60s for `/health`, runs `test/test.sh`, cleans up. Runs `build.sh` as normal user, `install.sh` as root (CI).

### 1.6 Load test (bench/load.js)
One VU = one person: GET /feed → think 3-7s → GET /posts/:id → think 3-8s → 15% like → 2% post → idle 5-15s → repeat. Tokens from `seed/tokens.json`. Thresholds: p(95)<500, p(99)<1000, failed rate<0.01. Default VUS=100, DURATION=5m, RAMP_UP=60s.

### 1.7 Nginx config (bench/nginx.conf)
Upstream `app { server 127.0.0.1:3000; keepalive 64; }`. Nginx listens on 80, gzip on for `application/json`, `proxy_http_version 1.1`, `proxy_set_header Connection ""`, `proxy_connect_timeout 5s`, `proxy_read_timeout 30s`. Nginx keeps up to 64 idle connections to the app; idle connections must stay open at least 65s (SPEC.md:19).

### 1.8 Seed script (seed/make-seed.sh)
Builds `seed/feed.db` (~250 MB, WAL mode) + `seed/tokens.json`. Needs node >= 18 and sqlite3 CLI. Runs `generate.mjs`, imports TSVs with `PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;`, then `ANALYZE; VACUUM; PRAGMA journal_mode=WAL; PRAGMA wal_checkpoint(TRUNCATE);`. Content sha256 verified against `EXPECTED=738d11ab...` (seed/make-seed.sh:12). 50k users, 500k posts, 2M likes (README.md:18).

### 1.8 Leaderboard (LEADERBOARD.md)
Machine: Synteq 1 vCPU / 2 GB VPS, KVM, Xeon Platinum 8260 @ 2.4 GHz AVX-512, 1,967 MB RAM, no swap, Ubuntu 24.04, kernel 6.8, stock (no UFW, no kernel tuning). Scored as `deploy` with LimitNOFILE=65535, CAP_NET_BIND_SERVICE, fresh DB copy, 42-check suite first. Load from separate 16-core generators, 0.25 ms RTT. Score = most users passing 5-minute hold with p95<500ms, p99<1s, errors<1%.

Top entries (LEADERBOARD.md:59-79):
- #1 `rust-iouring-kensac` #264: **310,000** users, 29,284 req/s, p95 62ms, p99 190ms, 0 err, 82% CPU, 25 µs/req, 1,114 MB RAM, 2026-11-02. Rust io_uring, SQLite B-tree reads.
- #2 `mojo-iouring-oh-meh` #257: **304,000** users, Mojo io_uring, SQLite B-tree reads.
- #3 `c-iouring-pavlyhalim` #245: **295,000** users, 28,013 req/s, p95 28ms, p99 117ms, 0 err, 81% CPU, 26 µs/req, 1,536 MB RAM. C io_uring, SQLite B-tree reads.
- #4 `rust-native-scooter1337` #267: **290,000** users, Rust io_uring, SQLite B-tree reads.
- #5 (tied) `c-iouring-unorig-username` #290: **280,000** users, C io_uring, SQLite B-tree reads, 78% CPU, 26 µs/req, 1,357 MB.
- #9 `c-iouring-shivanshthapliyal` #250: **276,000** users, C io_uring.
- #10 `c-iouring-public` #158: **275,000** users, C io_uring.
- #24 `c-iouring-meros` #199: **238,000** users, C io_uring (no B-tree reads — plain SQL).
- #27 `c-iouring-moneebullah25` #244: **230,000** users, C io_uring, SQLite B-tree reads, 67% CPU, 28 µs/req, 828 MB.

Current C state-of-the-art: **295,000 users** (c-iouring-pavlyhalim). Target to beat: **310,000 users** (rust-iouring-kensac).

---

## 2. Existing Architecture Comparison

### 2.1 Reference: c-iouring-pavlyhalim (top C, 295,000 users)

File layout (`submissions/c-iouring-pavlyhalim/README.md:248-255`):
- `src/sys.h` — raw io_uring ABI (rings, direct descriptors, buffer rings, SQE builders), no liburing.
- `src/server.c` — event loop, batches, arenas, connection table; `#include "sqlite3.c"` (one translation unit).
- `src/app.h` — five endpoints.
- `src/db.h` — SQLite transactions, statements, checkpoint thread, write gate, custom VFS + page cache.
- `src/btree.h` — read plans on SQLite's B-tree cursors.
- `src/http.h`, `json.h`, `jwt.h`, `sha256.h`.
- `src/train.c`, `train.sh` — PGO training (build time only).

Key architectural decisions:
1. **One event-loop thread** serving all connections (`server.c:1` comment).
2. **Multiple io_uring rings** (`server.c:13` `MAX_RINGS 16`): ring 0 waits (`SINGLE_ISSUED | DEFER_TASKRUN`), rings 1..N use ordinary task work + eventfd signalled into ring 0's wait (`server.c:330`).
3. **Direct descriptors** (`sys.h:298` `IORING_FILE_INDEX_ALLOC`): accepted sockets held in fixed-file tables, bypassing the 65,535 fd limit. Up to 8 rings × 65,471 slots = 500k+ connections.
4. **SO_REUSEPORT with 8 listeners** (`server.c:288` `NLISTEN 8`): kernel spreads connections, each listener has its own `somaxconn` (4,096) accept queue → ~32,000 connections held through a stall.
5. **Multishot poll, not multishot recv** (`sys.h:307` `IORING_POLL_ADD_MULTI`): idle connections hold no buffer; a burst can never exhaust the buffer ring with `ENOBUFS`. Reads issued via `MSG_DONTWAIT` into provided-buffer rings.
6. **Provided buffer rings** (`sys.h:210-234`): 4,096 × 1 KB buffers per ring (`server.c:922`).
7. **Group commit** (`server.c:997` `db_commit`): one SQLite transaction per batch; all reads+writes in the batch run in one transaction; `COMMIT` then sends.
8. **SQLite B-tree cursor reads** (`db.h:1`, `btree.h`): feed and post reads walk SQLite's own B-tree cursors (`sqlite3BtreeFirst/Next`, `sqlite3BtreeTableMoveto`, `sqlite3BtreeIndexMoveto`) inside the batch's transaction, bypassing the bytecode interpreter.
9. **Custom VFS `tdsc-wal`** (`db.h:12`): batches WAL frame writes into a single `pwrite` per commit (header + page combined when contiguous).
10. **Custom page-cache wrapper** (`db.h:169-243`): `SQLITE_CONFIG_PCACHE2` shim that skips the full-cache scan on truncate when no pages past the file end exist.
11. **Background checkpoint thread at nice 19** (`db.h:82-116`): PASSIVE checkpoints on a second connection; writes held past 4,000 WAL frames until the log is copied.
12. **WAL, `synchronous=NORMAL`, `wal_autocheckpoint=0`** (`db.h:145-147`).
13. **No mmap** (`mmap_size=0`), 8 MB page cache (`db.h:141`, `db.h:126` default `CACHE_KIB=8192`).
14. **Custom SHA-256 + HMAC** (`sha256.h`): key's inner/outer states precomputed once (`hmac_key_init`).
15. **Constant-time JWT verification** (`jwt.h:53-57` `ct_equal`), signature checked before payload parsing (`jwt.h:69-72`).
16. **Custom JSON** (`json.h`): validating reader + SIMD writer (16-byte vectorized escape detection).
17. **Static binary, `-O3`, PGO, `march=x86-64-v3`, `mlock` of code** (`build.sh:21-38`, `server.c:868-891`).
18. **Memory valve** (`server.c:445-469`): off by default (`VALVE=1`), closes a share of connections when headroom < 400 MB.

### 2.2 c-iouring-meros (238,000 users, simpler C)
- `submissions/c-iouring-meros/` — plain C io_uring without B-tree reads. Uses standard SQL. Simpler and easier to verify, but lower score. Good as a baseline reference.

### 2.3 Current leader: rust-iouring-kensac (310,000 users)
- Rust io_uring, SQLite B-tree reads. Uses `rusqlite` or direct SQLite FFI with B-tree cursor access. 25 µs/req, 1,114 MB RAM.

### 2.4 Key differentiator: B-tree reads
All top C/Rust entries (275k+) use SQLite B-tree cursor reads to skip the bytecode interpreter. This is the single largest CPU saving. c-iouring-meros (238k) does NOT use B-tree reads — it uses standard prepared SQL — confirming B-tree reads are worth ~50k+ users.

---

## 3. Performance Comparison

| Metric | c-iouring-pavlyhalim (#3, 295k) | c-iouring-unorig-username (#5, 280k) | rust-iouring-kensac (#1, 310k) | c-iouring-meros (#24, 238k) |
|---|---|---|---|---|
| Users held 5 min | 295,000 | 280,000 | 310,000 | 238,000 |
| req/s | 28,013 | 26,688 | 29,284 | 22,796 |
| p95 ms | 28 | 56.4 | 62 | 417 |
| p99 ms | 117 | 713 | 190 | 750 |
| CPU % | 81 | 78 | 82 | 78 |
| µs/req | 26 | 26 | 25 | 31 |
| RAM MB | 1,536 | 1,357 | 1,114 | 1,567 |
| Approach | C io_uring + B-tree | C io_uring + B-tree | Rust io_uring + B-tree | C io_uring + SQL |

Source: LEADERBOARD.md lines 59-83.

Per-request CPU breakdown at 160,000 users (c-iouring-pavlyhalim README.md:276-278):
- `__inet_lookup_established` (kernel socket lookup): 14%
- Checkpoint thread: 5.6%
- SQLite page-cache lookup: 2.8%
- Author lookups: 2.2%
- Feed: 1.8%
- JSON escaping: 1.7%

System calls per request at 120,000 users (README.md:178-180): 0.23 total (`io_uring_enter` 0.05, `pwrite` 0.095 including checkpoint thread, everything else ≈ 0.08).

Memory per connection (README.md:308-309): cgroup grows ~2.1 KB per connection, nearly all kernel socket objects; server heap stays near 60 MB.

---

## 4. Identified Bottlenecks

1. **SQLite bytecode interpreter** — every `sqlite3_step` runs the full VDBE bytecode engine for every query. B-tree reads bypass this entirely (btree.h:1 comment). Dominates feed/post reads.
2. **WAL write syscalls** — SQLite writes each WAL frame as two calls (24-byte header + page). Custom VFS combines them into one `pwrite` per commit (db.h:258-270). At 120k users this cut `pwrite`s/request from 0.24 to 0.095 (README.md:176-177).
3. **Page-cache truncate scan** — pcache1 never lowers its max-key high-water mark, so every commit scans the whole cache hash table. Custom wrapper skips the scan (db.h:211-218). Was 1.8% of CPU (README.md:200-201).
4. **Checkpoint thread contention** — at default priority, a checkpoint pass holds the loop off-CPU for 300–430 ms (README.md:100-101). Nice 19 fixes this.
5. **Kernel socket memory** — ~4.3 KB per socket on kernel 6.8; this is the real 2 GB ceiling (README.md:57). Server adds only 40 bytes/conn (README.md:34).
6. **Accept-queue overflow** — one listener overflows 1.8M times under stall; 8 listeners with SO_REUSEPORT hold ~32k through a stall (README.md:26-27).
7. **Buffer-ring exhaustion (ENOBUFS)** — multishot recv on every connection drains the buffer ring under burst; each ENOBUFS needs re-arm (README.md:42-44). Solved by multishot poll + read queue.
8. **JSON escaping** — 1.7% of CPU; SIMD escape detection (json.h:295-307).
9. **Memory page faults on code** — without `mlock`, the loop stalls reading its own code from disk under memory pressure (README.md:85-90).
10. **fd table limit (65,535)** — entries below ~71k users are capped by this, not speed (LEADERBOARD.md:297-317). Direct descriptors bypass it.

---

## 5. Opportunities for Improvement

1. **Match pavlyhalim's full stack** — B-tree reads, custom VFS, custom page cache, group commit, direct descriptors, SO_REUSEPORT ×8, multishot poll + provided buffers, PGO, mlock. This gets us to ~295k users.
2. **Reduce RAM below 1,114 MB** — pavlyhalim uses 1,536 MB; the leader uses 1,114 MB. Lower page cache (4–8 MB instead of 8–32 MB) frees sockets (~240 sockets/MB at the wall, README.md:192).
3. **Reduce µs/req below 25** — pavlyhalim is at 26 µs/req; the leader is at 25. Small gains in JSON escaping, HTTP parsing, and io_uring submission batching.
4. **Better tail latency** — pavlyhalim p99 is 117 ms; the leader is 190 ms (worse) but unorig-username is 713 ms. Need p99 < 1 s reliably; target p99 < 200 ms.
5. **Single-ring simplicity** — pavlyhalim uses up to 16 rings with complex eventfd signaling. A single well-tuned ring may be simpler and equally fast at < 100k connections; scale to multi-ring only if needed.
6. **PGO training** — pavlyhalim trains on a synthetic DB matching the load mix (train.sh:12-18). Reproducing this gives measurable IPC gains.
7. **Connection-close under memory pressure** — pavlyhalim's valve (off by default) is a safety valve; implementing it correctly prevents OOM kills at the extreme edge.

---

## 6. Risks

1. **B-tree cursor correctness** — reading SQLite's internal B-tree structures (btree.h:127-516) depends on undocumented on-disk format and internal APIs (`sqlite3BtreeCursor`, `sqlite3BtreeTableMoveto`, `sqlite3BtreePayload`, `getAndInitPage`, `findCell`, `releasePageNotNull`, `bt_cell_key` using `findCellPastPtr`, `bt_lower_bound`). A SQLite version change could break it. Mitigation: schema layout is verified at startup (btree.h:34-62); any surprise falls back to reference SQL (db.h:589-595, 663-666).
2. **Custom VFS correctness** — `tdsc-wal` VFS (db.h:375-436) buffers WAL writes and flushes on commit frame. A bug could lose acknowledged writes. Mitigation: crash-tested 40 rounds with SIGKILL (README.md:173-175).
3. **io_uring kernel compatibility** — Ubuntu 6.8.0-136+ rejects provided-buffer-ring registration with EINVAL (Launchpad #2162843); pavlyhalim retries with `resv[0]=1` (sys.h:221-226). Kernels charge ring memory to `RLIMIT_MEMLOCK` (8 MB under systemd); 8 rings didn't fit CI's runner (README.md:53-56).
4. **Memory OOM at the wall** — the 2 GB box has no swap; kernel socket memory (~4.3 KB/conn) is the ceiling. pavlyhalim reached ~144k connections with valve off, OOM-killed at ~59k with valve on (README.md:112-114).
5. **PGO training requires running the server** — `train.sh` starts the instrumented binary and drives 150k requests (train.sh:18). If the build environment can't run the binary, PGO falls back to plain `-O3` (build.sh:52-53).
6. **Cannot test locally** — this development machine is Windows; no Linux/io_uring/SQLite toolchain available. All code must be written to compile cleanly on Ubuntu 24.04 gcc without local verification.
7. **Direct descriptors + close race** — a read on a direct descriptor keeps the socket alive after the slot is closed; polls must be cancelled by user_data (hash lookup), not by descriptor (README.md:74-77).

---

## 7. Implementation Strategy

### Phase 1: Baseline (correct, standard SQL)
- Single io_uring ring, multishot accept + multishot poll + provided buffer ring.
- Direct descriptors in a fixed file table.
- Standard SQLite prepared statements (no B-tree reads initially).
- Group commit (one transaction per batch).
- WAL + `synchronous=NORMAL` + `wal_autocheckpoint=0`.
- Custom JSON parser/serializer, custom SHA-256/HMAC, JWT verification.
- Arena allocation, fixed connection pool.
- Pass all 42 official tests.

### Phase 2: io_uring optimization
- SO_REUSEPORT ×8 listeners.
- Multi-ring scaling (ring 0 waits, others signal via eventfd).
- Read queue with bounded reads per batch (drain-first).
- Batch WAL writes via custom VFS (one `pwrite` per commit).
- Custom page-cache wrapper.
- Background checkpoint thread at nice 19.

### Phase 3: B-tree reads (highest reward, highest risk)
- B-tree cursor plans for feed and post (btree.h pattern).
- Schema verification at startup with SQL fallback.
- Batched author lookups (one level at a time, prefetch child pages).

### Phase 4: Memory & tail latency
- `mlock` of code + data.
- Memory pre-warming (stack, arenas, ring tables).
- Memory valve (off by default) as safety valve.
- PGO training build.

### Phase 5: Benchmark & tune
- Reproduce official load test locally on Ubuntu 24.04.
- Profile with `perf`, iterate one variable at a time.
- Target: >295k users, p99 < 200 ms, RAM < 1,114 MB.

---

## 8. Rules That Must Never Be Violated

1. **Only modify files inside** `submissions/c-iouring-binarycore-zekridev1/`. Never touch official challenge files, other submissions, schema, tests, bench config, or leaderboard.
2. **Language: C.** GCC on Ubuntu 24.04 x86-64.
3. **SQLite only** — no Postgres, Redis, RocksDB, LMDB, custom DB files, or in-memory databases (README.md:37-38).
4. **Schema fixed** — no added/changed tables, columns, indexes, triggers (README.md:39, schema.sql:2).
5. **Pass test/test.sh 100%** with exact bytes (README.md:43).
6. **No caching across requests** — no response cache, query-result cache, in-memory table copies, or remembered JWT verifications. SQLite page cache and mmap are fine. Prepared statements are fine (README.md:44-47).
7. **Writes durable before response** — WAL + `synchronous=NORMAL` or stronger. No `synchronous=OFF`, no `journal_mode=OFF/MEMORY`, no in-memory DB. Group commit OK only if each response waits for its commit (README.md:49-51).
8. **No hard-coded responses, no benchmark detection** (README.md:52).
9. **Direct on `0.0.0.0:80`** or behind Nginx on `127.0.0.1:3000`. BinaryCore chooses direct (saves Nginx CPU, per README.md:58-60).
10. **Build from source** via `install.sh` (root) / `build.sh` (user) / `start.sh` (foreground). Healthy within 60s (README.md:62).
11. **Read `SQLITE_PATH` and `JWT_SECRET`** from env; bind to `HOST:PORT` (SPEC.md:8-16).
12. **Correct exit codes, useful stderr diagnostics** (README.md:9, 74).
13. **HTTP/1.1 JSON API exactly** — never replace with a custom binary protocol (master prompt §4.2, §4.3).
14. **No undefined behavior, no unsafe aliasing, no architecture-specific instructions** that break x86-64 Ubuntu 24.04 (master prompt §6).
15. **Constant-time JWT signature comparison, HS256 only, verify every token** (master prompt §4.4, SPEC.md:76-79).
16. **Every request reads from SQLite** while being served (master prompt §4.5).
17. **Do not claim first place unless the official leaderboard confirms it** (master prompt §12, §14).
