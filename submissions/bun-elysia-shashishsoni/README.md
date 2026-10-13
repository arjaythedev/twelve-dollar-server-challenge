# Bun + Elysia

| | |
|---|---|
| Language | TypeScript on Bun 1.3.14 |
| Framework | Elysia 1.4.30 (`aot:true`, `normalize:false`, no schemas/plugins) |
| Server | Elysia over `Bun.serve` |
| SQLite driver | built-in `bun:sqlite` |
| JSON | native `JSON.stringify` with SPEC-ordered literals |
| **Nginx or direct** | **Nginx**: listens on `127.0.0.1:3000` with `idleTimeout:70s` |

## Running it

```bash
sudo bash install.sh   # installs pinned Bun to /usr/local/bin/bun
bash build.sh          # bun install --frozen-lockfile --production
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
bun test               # unit tests for auth + validation
```

## Optimizations, and why

- **One process, one `bun:sqlite` connection, prepared statements reused.** 1 vCPU means a pool only adds context switches and `SQLITE_BUSY` retries. Prepared statements cache the plan (allowed), data is still read fresh every request.
- **Lean Elysia.** No `t` schemas, no plugins, no OpenAPI. Handlers validate manually and return raw `Response` with compact JSON, skipping validation + `jsonable_encoder`-style overhead and preserving exact key order.
- **Hand-verified JWT** (`node:crypto` HMAC + `timingSafeEqual`) on every request. HS256-only, `exp`/`nbf` checked, `sub`/`username` shape checked. No library overhead, no verification caching.
- **Single-statement like**: `INSERT ... SELECT WHERE EXISTS ... ON CONFLICT DO NOTHING`, then one existence check only when nothing was inserted. Distinguishes `already_liked` from `post not found` without an extra read on the hot path.
- **Writes use `RETURNING id, created_at`.** No JS clock, no format drift; autocommit commits before the 201 is sent.
- **Pragmas**: `WAL`, `synchronous=NORMAL`, `busy_timeout=5000`, `mmap_size=512MB`, `cache_size=64MB`, `temp_store=MEMORY`. Durable per rule 6, read through OS page cache.
- **Nginx mode + 70s idle timeout** so the 64 upstream keepalive sockets are never closed early. Compression left to Nginx.
- **Clean layers** (`config/db/auth/validation/http/index`): each function <20 lines, one thing, intention-revealing names. Unit-tested pure functions (`parsePostId`, `extractTrimmedBody`, `verifyBearerToken`).

## License

MIT, under the repo's [license](../../LICENSE).
