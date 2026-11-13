# Go + fasthttp

| | |
|---|---|
| Language | Go 1.27.1 (official tarball, installed by `install.sh`) |
| HTTP | [`valyala/fasthttp`](https://github.com/valyala/fasthttp) v1.75.0, no router library |
| SQLite driver | hand-written cgo bindings to `libsqlite3` (Ubuntu 24.04's `libsqlite3-dev`) — **no `database/sql`** |
| JSON | hand-written encoder for responses and an allocation-free scanner for JWT + request bodies (no `encoding/json`) |
| Auth | HMAC-SHA256 with precomputed key pads, `crypto/sha256`, constant-time compare |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All dependency versions are pinned in `go.mod` / `go.sum` (`fasthttp v1.75.0`, `klauspost/compress v1.20.1`,
`molecule-man/go-brrr v1.1.1`, `valyala/bytebufferpool v1.0.0`).

## Running it

```bash
sudo bash install.sh     # Go toolchain + build-essential + libsqlite3-dev
bash build.sh            # go build -trimpath -ldflags "-s -w" -o bin/server .
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Why direct

One vCPU means Nginx's CPU comes straight out of the server's budget. `fasthttp` handles the
~15,000 direct keep-alive connections by itself, and `Server.ReduceMemoryUsage` releases each
connection's read/write buffers and `RequestCtx` while it sits idle, so idle connections cost
almost nothing. On a local run with **15,000 open VUs**: 0 errors, p95 ≈ 0.7 ms, p99 ≈ 3.6 ms,
and a **~135 MiB** peak RSS for the whole process.

## Optimizations

### Framework / HTTP

- **fasthttp**, direct on `:80`, no Nginx. Manual routing over the raw path bytes (`bytes.Equal` /
  `HasPrefix`) — no allocs, no router trie.
- **`ReduceMemoryUsage = true`**: per-connection buffers are returned to the pool between requests,
  which is what makes ~15k mostly-idle connections affordable.
- **No per-request read deadlines** (`ReadTimeout`/`IdleTimeout` unset) plus `LazyRequestTime`: this
  removes a timer arm/cancel and a clock read from every request and every keep-alive wait. SPEC only
  requires idle connections to be held open (≥ 65 s), and never timing them out satisfies that.
- `ReadBufferSize` 4 KiB, `WriteBufferSize` 16 KiB (a whole 20-post feed fits in one write).
- Disabled the default `Server`, `Date` and `Content-Type` headers; we emit only what's needed.
- Static error bodies are pre-serialized `[]byte`; success bodies are built into a `sync.Pool`
  buffer and copied once into the response.
- **Allocation-free parsing.** JWT pieces are base64-decoded into one pooled scratch buffer reused for
  header/signature/payload; the header, payload and `{"body": …}` are scanned by a small hand-written
  JSON reader that unescapes strings in place. A like/create request now allocates a couple of bytes
  instead of ~1 KiB, and the read endpoints allocate nothing at all.
- A top-level `recover()` turns any panic into a 500 instead of killing the process.

### Database

- **Direct cgo bindings** to `libsqlite3` — no `database/sql` layer, no `driver.Value` /
  `driver.Rows` allocations. Column text is read straight from SQLite's buffer and appended into the
  response byte slice, so a feed of 20 posts costs ~one buffer.
- **Prepared statements per connection**, reused across requests (cached query plan only — allowed
  by rule 5; no data is cached).
- **WAL with a dedicated write connection** (serialized by a mutex) plus a small pool of **read
  connections**. WAL readers never block the single writer and vice-versa.
- **Checkpoints run off the request path.** `wal_autocheckpoint` is raised as a backstop and a
  background goroutine issues a `PASSIVE` checkpoint on its own connection every 5 s, so copying WAL
  pages back into the database never blocks a writer mid-request.
- Pragmas: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), `mmap_size=1 GiB` (reads come from the
  OS page cache with no copies), `cache_size=-4096`, `temp_store=MEMORY`, `busy_timeout=5000`.
- `sqlite3_open_v2` with `SQLITE_OPEN_NOMUTEX` — each connection is only ever touched by one
  goroutine at a time (pool + write mutex), so SQLite's internal mutexes are skipped.
- The like endpoint is a single `INSERT ... SELECT ?1, ?2 WHERE EXISTS(post) ON CONFLICT DO NOTHING`;
  the existence check runs (on the same write connection) only when nothing was inserted.
- Every write is stepped through to `SQLITE_DONE` before the response is written, so a 201 always
  means the row is committed.

### Process

- `debug.SetGCPercent(-1)` with a soft `GOMEMLIMIT` of 512 MiB: the request path is allocation-free,
  so the collector is effectively absent from the latency tail, while the soft limit still bounds
  memory (measured ~135 MiB at 15k connections).
- No response caches, no query-result caches, no in-memory table copies, and every JWT signature is
  verified on every request (rule 5).

## Testing

`bash test/run.sh submissions/go-fasthttp-b3nhard` passes all 42 checks.

## License

MIT, under the repo's [license](../../LICENSE).
