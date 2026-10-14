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
- **Hand-verified JWT** (`node:crypto` HMAC + `timingSafeEqual`) on every request. HS256-only, `exp`/`nbf` checked, `sub`/`username` shape checked. No library overhead, no verification caching.
- **Single-statement like**: `INSERT ... SELECT WHERE EXISTS ... ON CONFLICT DO NOTHING`, then one existence check only when nothing was inserted. Distinguishes `already_liked` from `post not found` without an extra read on the hot path.
- **Writes use `RETURNING id, created_at`.** No JS clock, no format drift; autocommit commits before the 201 is sent.
- **Feed walks the index, then counts**: `ORDER BY created_at DESC, id DESC LIMIT 20` scans `posts_created_at_id_idx` and stops after 20 rows, with one covering-index `count(*)` per post (~25 µs). A `LEFT JOIN likes ... GROUP BY p.id` looks equivalent but aggregates all 500k posts before the `LIMIT` (~770 ms).
- **Pragmas**: `WAL`, `synchronous=NORMAL`, `busy_timeout=5000`, `mmap_size=512MB`, `cache_size=64MB`, `temp_store=MEMORY`, `journal_size_limit=64MB`. Durable per rule 6, read through OS page cache.
- **Checkpoints on a worker thread.** With SQLite's auto-checkpoint, the commit that crosses the threshold copies pages into the DB and `fsync`s it on the event-loop thread. Any event-loop stall risks overflowing Bun's fixed 512-entry accept backlog, and every dropped SYN costs a client a 1–3 s retransmit. So the main connection sets `wal_autocheckpoint=0` and a `Worker` with its own connection runs `wal_checkpoint(PASSIVE)` every second (no locks held against readers or writers); if the worker ever dies, auto-checkpoint is switched back on.
- **Direct, with a 13 s idle timeout.** The bench Nginx runs one worker with `worker_connections 16384`, so it can't hold more than ~16k users' keep-alive connections. Direct, every user holds its own socket, and with a long idle timeout tens of thousands of idle sockets would exceed `LimitNOFILE=65535` and eat the 2 GB in kernel socket memory. Users think 3–15 s between requests. Bun sweeps idle sockets every 4 s, so `idleTimeout:13` really closes after 12–16 s (measured): that keeps the connection through every gap before a like/post (≤ 8 s, so a POST never races the server's close; clients only auto-retry GETs) and closes it during the long idle, so open sockets stay under the limit; clients just reconnect. When `HOST` is loopback (behind Nginx) it uses 70 s instead so the 64 upstream sockets are never closed early.
- **Connections are retired under descriptor pressure.** Past ~66k users even the idle timeout leaves more sockets open than `LimitNOFILE=65535` allows, and new users would wait in the accept queue. Every 250 ms the server reads its open-descriptor count (`stat("/proc/self/fd").size`, O(1) on Linux ≥ 6.2, ~1 µs). Above 92% of the limit, finished responses carry `Connection: close`; below 88% it goes back to keep-alive. Bun closes the socket from the server side, so TIME_WAIT stays on the server and never eats the clients' ephemeral ports. Clients just reconnect on their next request.
- **Clean layers** (`config/db/checkpoint/connections/auth/validation/http/index`): each function <20 lines, one thing, intention-revealing names. Unit-tested pure functions (`parsePostId`, `extractTrimmedBody`, `verifyBearerToken`, `shouldRetire`, `parseOpenFilesLimit`).

## License

MIT, under the repo's [license](../../LICENSE).
