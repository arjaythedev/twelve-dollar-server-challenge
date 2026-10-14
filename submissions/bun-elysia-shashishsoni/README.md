# Bun + Elysia

| | |
|---|---|
| Language | TypeScript on Bun 1.3.14 |
| Framework | Elysia 1.4.30 (`aot:true`, `normalize:false`, no schemas/plugins) |
| Server | Elysia over `Bun.serve` |
| SQLite driver | built-in `bun:sqlite` |
| JSON | native `JSON.stringify` with SPEC-ordered literals |
| **Nginx or direct** | **Direct**: `HOST=0.0.0.0 PORT=80`, `idleTimeout:13s`. (Behind Nginx on `127.0.0.1:3000` it switches to `idleTimeout:70s`.) |

## Running it

```bash
sudo bash install.sh   # installs pinned Bun to /usr/local/bin/bun
bash build.sh          # bun install --frozen-lockfile --production
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh   # direct (benchmark)
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh  # behind Nginx / CI
bun test               # unit tests for auth + validation
```

## Optimizations, and why

- **One process, one `bun:sqlite` connection, prepared statements reused.** 1 vCPU means a pool only adds context switches and `SQLITE_BUSY` retries. Prepared statements cache the plan (allowed), data is still read fresh every request.
- **Lean Elysia.** No `t` schemas, no plugins, no OpenAPI. Handlers validate manually and return raw `Response` with compact JSON, skipping validation + `jsonable_encoder`-style overhead and preserving exact key order.
- **Hot-path parsing without `BigInt`.** `parsePostId`/`sub` validation used `BigInt`, which is slow per request. `parsePostId` now strips leading zeros and compares against `"9007199254740991"` as a string (length > 16 can only be a 404 sentinel); `sub` just checks digits + a nonzero digit. Same 400/404/401 behavior, no bigint allocation.
- **JWT header fast path.** Seed and test tokens all use the fixed header `{"alg":"HS256","typ":"JWT"}` (`eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9`), so matching headers skip a base64 decode + `JSON.parse`; anything else falls back to full parse + HS256 check. Signature (`node:crypto` HMAC + `timingSafeEqual`), `exp`/`nbf`, `sub`/`username` shape still verified on every request — no verification caching.
- **Single-statement like**: `INSERT ... SELECT WHERE EXISTS ... ON CONFLICT DO NOTHING`, then one existence check only when nothing was inserted. Distinguishes `already_liked` from `post not found` without an extra read on the hot path.
- **Writes use `RETURNING id, created_at`.** No JS clock, no format drift; autocommit commits before the 201 is sent.
- **Feed walks the index, then counts**: `ORDER BY created_at DESC, id DESC LIMIT 20` scans `posts_created_at_id_idx` and stops after 20 rows, with one covering-index `count(*)` per post (~25 µs). A `LEFT JOIN likes ... GROUP BY p.id` looks equivalent but aggregates all 500k posts before the `LIMIT` (~770 ms).
- **Pragmas**: `WAL`, `synchronous=NORMAL`, `busy_timeout=5000`, `mmap_size=512MB`, `cache_size=64MB`, `temp_store=MEMORY`, `journal_size_limit=64MB`. Durable per rule 6, read through OS page cache.
- **Checkpoints on a worker thread.** With SQLite's auto-checkpoint, the commit that crosses the threshold copies pages into the DB and `fsync`s it on the event-loop thread. Any event-loop stall risks overflowing Bun's fixed 512-entry accept backlog, and every dropped SYN costs a client a 1–3 s retransmit. So the main connection sets `wal_autocheckpoint=0` and a `Worker` with its own connection runs `wal_checkpoint(PASSIVE)` every second (no locks held against readers or writers); if the worker ever dies, auto-checkpoint is switched back on.
- **Direct, with a 30 s idle timeout.** The bench Nginx runs one worker with `worker_connections 16384`, so it can't hold more than ~16k users' keep-alive connections. Direct, every user holds its own socket. Bun sweeps idle sockets every 4 s, so `idleTimeout:30` really closes after 28–32 s (measured): far above every think gap (3–15 s, and the ≤ 8 s gaps before a like/post, so a POST never races the server's close; clients only auto-retry GETs). Eviction itself is left to the proportional shedder below, which closes finished responses smoothly; the sweep then only reaps strays instead of firing synchronized 4 s-quantum herds. (An earlier 13 s timeout let the sweep own eviction: at 80K users that gave p95 2.1 s with ~30K accept-queue overflows per run; 30 s gave p95 0.23 s with ~2K, then zero.) When `HOST` is loopback (behind Nginx) it uses 70 s instead so the 64 upstream sockets are never closed early.
- **Proportional shedding under descriptor pressure.** Past ~56k users the server holds more sockets than `LimitNOFILE=65535` allows, so finished responses must sometimes close (server side, so TIME_WAIT stays here) instead of idling in keep-alive; clients just reconnect. Every 50 ms the server reads its open-descriptor count (`stat("/proc/self/fd").size`, O(1) on Linux ≥ 6.2, ~1 µs) and sheds a fraction of responses that ramps 0 → 100% between 85% and 95% of the limit. The previous all-or-nothing flag evicted in synchronized herds whose reconnect bursts overflowed Bun's fixed 512-entry accept backlog (every dropped SYN costs 1–3 s): at 80K that was 432 failed requests and p95 2.1 s; proportional shedding cut it to 58 fails and p95 1.3 s before the idle-timeout change above.
- **Clean layers** (`config/db/checkpoint/connections/auth/validation/http/index`): each function <20 lines, one thing, intention-revealing names. Unit-tested pure functions (`parsePostId`, `extractTrimmedBody`, `verifyBearerToken`, `shedForLoad`, `parseOpenFilesLimit`).

## Local stress (not the droplet score)

Same-box runs: server pinned to 1 CPU, `LimitNOFILE=65535`, direct `:8080`; load from a raw-socket generator (1 TCP connection per user, same request mix / probabilities / think times as `bench/load.js`, calibrated against k6 within ~2x at 2K users). 60 s ramp + 90 s hold. Absolute numbers will differ on the droplet; the shape is what matters.

| Users | Reqs | p50 | p95 | p99 | Errors | Server core | Server FDs |
|---|---|---|---|---|---|---|---|
| 80,000 | 1.08M | 0.25 ms | 3.8 ms | 45 ms | 0 | ~45% | ~57.5K |
| 100,000 | 1.36M | 0.25 ms | 0.82 ms | 3.8 ms | 0 | ~55% | ~58.4K |
| 120,000 | 1.63M | 0.28 ms | 1.5 ms | 10 ms | 0 | ~62% | ~59K |
| 150,000 | 2.03M | 0.48 ms | 18 ms | 40 ms | 0 | ~73% | ~59.7K |
| 180,000 | 2.38M | 45 ms | 304 ms | 570 ms | 0 | ~81% | ~60K |
| 200,000 | 2.51M | 188 ms | 513 ms | 734 ms | 2 | ~84% | ~60K |

Read: clean passes through 180K (gates: p95 < 500 ms, p99 < 1 s, < 1% errors); 200K fails p95 by 13 ms as the core saturates and the accept queue starts dropping SYNs again. The ceiling on one core is connection churn, not request CPU (p50 stays sub-ms until ~150K) and not SQLite (writes hold steady at ~800/s, WAL residue stays under its 64 MB cap).

## License

MIT, under the repo's [license](../../LICENSE).
