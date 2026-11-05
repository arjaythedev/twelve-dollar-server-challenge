# Go + Fiber + SQLite

| | |
|---|---|
| Language | Go 1.27.3 (official tarball) |
| HTTP framework | [Fiber v3](https://github.com/gofiber/fiber) v3.6.1, built on `valyala/fasthttp` v1.75.0 |
| SQLite driver | [`mattn/go-sqlite3`](https://github.com/mattn/go-sqlite3) v1.14.37 (cgo, bundled SQLite 3.50.6) |
| **Nginx or direct** | **Nginx**: listens on `HOST:PORT` (`127.0.0.1:3000` on the benchmark box) |

All Go dependencies are pinned in `go.mod` / `go.sum`; nothing is prebuilt in the
repository. `install.sh` installs `build-essential` plus Go 1.27.3 into
`/usr/local/go`, `build.sh` compiles `bin/server`, and `start.sh` runs it in the
foreground using only `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`.

## API

The five endpoints from `SPEC.md` are implemented directly:

- `GET /health` runs `SELECT 1` and returns uptime in whole seconds.
- `GET /feed` returns the 20 newest posts with `ORDER BY created_at DESC, id DESC`.
- `GET /posts/:id` returns one post or the spec's 404.
- `POST /posts` verifies the HS256 JWT, trims and validates the body, inserts with
  `RETURNING id, created_at`, and returns the post with the token's username.
- `POST /posts/:id/like` checks auth before the id, inserts with
  `ON CONFLICT DO NOTHING`, and returns 201 the first time / 200 on repeats.

Responses use the exact key order from the spec. Unknown paths and all other
errors are mapped to the specified compact JSON error bodies.

## Database and durability

The fixed schema and indexes are untouched. The connection string enables:

- `journal_mode=WAL` and `synchronous=NORMAL`, so a 201 is sent only after SQLite
  has committed the write (no `OFF` / `MEMORY` journal modes).
- `busy_timeout=5000` so a writer waits briefly instead of failing under load.
- a 64 MiB SQLite page cache per connection and a 256 MiB `mmap_size`; both are
  SQLite's own cache/mmap, not an application-level copy of the data.
- `foreign_keys=on`.

The server talks to `mattn/go-sqlite3` through its raw `driver.Conn` /
`driver.Stmt` API instead of `database/sql`. That removes the generic
`database/sql` row/argument machinery from the hot path. A tiny channel-based
pool hands one connection to one goroutine at a time, because SQLite statements
are not safe for concurrent use. On the 1 vCPU scoring box the pool defaults to
one connection: all queries then share a single warm page cache instead of
thrashing several caches, and worst-case RSS stays around 130–170 MiB. The size
scales with `GOMAXPROCS` on larger machines (capped at 4) and can be overridden
with `DB_POOL` for experiments.

Every query is executed against SQLite on every request: there are no response
caches, query-result caches, in-memory tables or remembered JWT verifications.
Prepared statements are reused only to cache query plans, which the rules
explicitly allow.

## Optimizations

- **JSON is built inside SQLite for the two read-heavy endpoints.** `GET /feed`
  uses `json_group_array(json_object(...))` and `GET /posts/:id` uses
  `json_object(...)`, so Go reads one text column instead of 20 rows × 5 columns
  of cgo values. SQLite's JSON escaping also matches the golden responses byte
  for byte. This was the largest win: roughly +35% feed throughput on a pinned
  single core.
- **Raw driver, no `database/sql`.** The prepared statements and rows are used
  directly, and the feed query is a single `sqlite3_step` over an
  `INDEXED BY posts_created_at_id_idx` scan.
- **Hand-rolled JSON for writes.** `POST /posts` and the like response append
  fixed key order directly into a pooled byte buffer, avoiding reflection and
  `encoding/json` on the response side. The small `POST` request body and JWT
  payload are still decoded with `encoding/json`; a third-party encoder would
  change less than 1% of total CPU here, so the dependency/JIT cost is not
  justified.
- **JWT fast path.** Tokens with the standard `{"alg":"HS256","typ":"JWT"}`
  header skip base64/JSON header decoding; the signature is still verified on
  every request with a pooled HMAC-SHA256 hasher, and `exp` is always checked.
- **One-statement like path.** `INSERT ... SELECT ... WHERE EXISTS ... ON
  CONFLICT DO NOTHING`; only the uncommon rows-affected==0 case runs the extra
  existence query to distinguish a duplicate like (200) from a missing post
  (404).
- Fiber/fasthttp with a 75 s idle timeout, so Nginx's 64 idle upstream sockets
  are never closed just before reuse.
- Prepared statements for the feed, single-post, health, create, like and
  post-existence queries.
- Built with `-trimpath -ldflags='-s -w'` and `sqlite_omit_load_extension`.

## Build and run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=127.0.0.1 PORT=3000 bash start.sh
```

Test locally from the repository root:

```bash
bash test/run.sh submissions/jww-fiber
```
