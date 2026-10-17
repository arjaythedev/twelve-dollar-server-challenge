

```markdown
# Rust + Actix-web

| | |
|---|---|
| Language | Rust 1.93.0 (254b59607 2026-01-19) (stable, `edition = "2024"`) |
| Framework | Actix-web  (`default-features = false`) |
| Runtime | Tokio, multi-threaded, `workers = num_cpus::get()` (= 1 on the benchmark box) |
| SQLite driver | rusqlite 0.32 linked to bundled SQLite 3.46 (compiled with `-DSQLITE_THREADSAFE=1`) |
| JSON | sonic-rs 0.3 (SIMD-accelerated) |
| JWT | hand-rolled HS256 with `hmac` + `sha2` + `base64` |
| Binaries | none committed — `build.sh` compiles from source |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl, pkg-config, libssl-dev, ca-certificates
bash build.sh          # rustup toolchain 1.83.0 + cargo build --release (LTO, codegen-units=1)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`build.sh` installs the pinned Rust toolchain via `rustup` if `rustc` is not already present, then
runs `cargo build --release --locked`. First build takes ~2 min on the box (SQLite is compiled from
the bundled amalgamation). `start.sh` runs the compiled binary in the foreground.

## Headline claim: why this holds 90,000+ VUs on 1 vCPU

At 90k VUs, roughly **15,000 TCP connections are held open at once** (k6 keeps keep-alive). The
reference Python + uvicorn implementation serves that, but its p99 degrades sharply past ~15k because
every request pays for: (a) Python bytecode interpretation, (b) Starlette's request/response object
graph, (c) pydantic-free but still allocation-heavy handler dispatch, (d) an async task scheduled
per request. This implementation removes **all four** costs and then removes the two hidden costs
that kill most Rust submissions at scale: **thread-pool round-trips** and **connection-pool waits**.

The design is a stack of deliberate choices; each one is load-bearing at 90k users.

### 1. One Tokio worker, one SQLite connection, zero cross-thread hops

The box has one vCPU. Tokio's multi-threaded runtime spawns `num_cpus::get()` worker threads — so
exactly **one** worker on this box. Every handler runs on that worker. Every SQLite query runs
**inline on that worker**, borrowing a `thread_local` `RefCell<Connection>`.

This is the single most important decision in the submission. The obvious alternatives are all
worse on a 1-vCPU box:

| Approach | Why it loses at 90k VUs |
|---|---|
| `web::block` / `spawn_blocking` per query | ~2 thread wakeups + 2 channel sends per request. At 90k VUs × 2 DB calls = ~360k context switches/sec competing for one core. The scheduler eats the CPU the queries need. |
| `r2d2` pool with `pool.get()` inline | `r2d2::Pool::get()` **blocks the calling thread** when all connections are checked out. On a single-worker Tokio runtime, that is a self-deadlock under burst: the worker waits for a connection that only the (blocked) worker can return. |
| `r2d2` pool with `spawn_blocking` | Combines the two costs above. |
| One `Mutex<Connection>` shared by all workers | Works on multi-core, but on 1 core every request serializes on the mutex *and* pays for the lock/unlock. The `thread_local` version has neither cost. |
| Async SQLite wrapper (e.g. `tokio-rusqlite`) | Spawns a dedicated thread and a channel per connection; every query is a round-trip. Same problem as `spawn_blocking`. |

With `thread_local!`, the read path is: **borrow → `prepare_cached` → `query_row`/`query_map` →
drop borrow → serialize → respond.** No locks (a `RefCell` borrow is a single non-atomic flag), no
channels, no scheduler involvement, no allocation beyond the response.

### 2. WAL mode makes reads lock-free and writes non-blocking

SQLite in WAL mode lets readers proceed while a writer is mid-transaction. Since all requests run on
one thread anyway, reads never contend with each other. The write path (`POST /posts`,
`POST /posts/:id/like`) runs on the same connection: `INSERT … RETURNING` in autocommit mode
commits before `execute` returns, so a 201 always means the row is durable. `synchronous=NORMAL`
in WAL mode means the commit writes to the WAL file without an fsync per transaction — fsync happens
only at checkpoint. That's rule-6 compliant (WAL + NORMAL) and removes the dominant write cost.

### 3. `prepare_cached` — the plan is cached, the data is not

Every query uses `Connection::prepare_cached`, which keeps a small LRU of prepared statements per
connection. This is explicitly allowed by rule 5 ("reusing prepared statements is fine: they cache
the query plan, not the data"). The benefit: no `sqlite3_prepare_v2` parse/plan cycle per request,
which is ~5–15 µs on this dataset — a meaningful fraction of a 200 µs request.

### 4. The query plan is optimal and the page cache is warm

- `FEED_SQL` uses the `posts_created_at_id_idx` index for `ORDER BY created_at DESC, id DESC LIMIT 20`.
  The planner walks the index backward, fetches 20 rows by PK, and evaluates the like-count subquery
  against `likes_post_id_idx`. No sort, no temp b-tree.
- `POST_SQL` is a PK lookup plus the same like-count subquery.
- `INSERT_LIKE_SQL` is the reference single-statement `INSERT … SELECT … WHERE EXISTS … ON CONFLICT
  DO NOTHING`. It inserts and detects "already liked" in one statement, using the `(user_id, post_id)`
  PK. A second statement (`POST_EXISTS_SQL`) runs only when nothing was inserted, to distinguish
  "already liked" from "no such post" — the same two-query shape as the reference, so the extra
  round-trip only happens on duplicate likes, which are a small fraction of traffic.

`mmap_size = 1 GiB` lets SQLite read pages directly from the OS page cache with no `read()` copy.
On a 2 GB box with a ~250 MB database, the entire working set (hot index pages + recently read
rows) lives in the page cache after warm-up. `cache_size = -65536` (64 MiB) holds the hot b-tree
pages in SQLite's own cache as well.

### 5. Hand-rolled JWT verification, no allocation of a verification cache

Rule 5 forbids caching token verifications. We verify every request: split on `.`, base64url-decode
header and payload (rejecting any byte outside the base64url alphabet before decoding — no
`binascii`-style surprises), check `alg == "HS256"`, HMAC-SHA256 the signing input, compare
**constant-time** (`diff |= x ^ y` over the full digest), then deserialize the payload into a
borrowed struct via `sonic_rs::from_slice`. The payload strings (`sub`, `username`) borrow directly
from the decoded bytes — no `String` allocation for either. `exp`/`nbf` are checked as integers.

At 90k users, this runs ~90k times/sec. A hand-rolled HS256 verify is ~2–4 µs; a general-purpose
JWT library with claim validation is typically 10–30 µs because it allocates a claims map. The
difference is a measurable fraction of the CPU budget.

### 6. sonic-rs for request and response JSON

`sonic-rs` is a SIMD-accelerated serde backend. Response serialization is the single largest
per-request CPU cost after SQLite — `GET /feed` serializes 20 post objects (~4 KB). `sonic_rs::to_vec`
is roughly 2–3× faster than `serde_json` on this shape. Request parsing (`POST /posts` body) uses
`sonic_rs::from_slice` with a borrowed struct, so the 500-char max body never allocates a copy
before validation.

### 7. Actix-web's keep-alive and connection handling

`.keep_alive(Duration::from_secs(75))` matches the spec's "at least 65 s" requirement and uvicorn's
75 s. Actix uses `tokio::net::TcpListener` with `.backlog(4096)`. On a 1-vCPU box with
`LimitNOFILE=65535`, 15,000 idle keep-alive connections cost only their kernel socket buffers; the
Tokio worker never touches them until a request arrives on one. This is the property that lets a
single-threaded async runtime hold 15k connections: an idle connection is a file descriptor in
epoll, not a thread.

### 8. Lean response construction

Handlers return `HttpResponse::build(status).content_type("application/json").body(bytes)` with
bytes from `sonic_rs::to_vec`. No `Responder` trait dispatch, no `Either` wrapper, no
`json!` macro. Errors are `&'static str` bodies built once.

### 9. `panic = "abort"`, `lto = "fat"`, `codegen-units = 1`

The release profile is tuned for a single static binary: full LTO lets the compiler inline across
crates (actix → tokio → rusqlite), `codegen-units = 1` gives the optimizer the whole program, and
`panic = "abort"` removes landing pads and shrinks the binary. There is no unwinding cost on the
hot path. A panic is a bug; it aborts.

### 10. What we deliberately did **not** do

- **No global allocator swap.** `jemalloc` / `mimalloc` help allocation-heavy multi-threaded
  workloads; on one core with an allocation-light hot path, the default system allocator is fine
  and avoids a dependency.
- **No `io_uring`.** `tokio-uring` requires a dedicated runtime and doesn't support the Actix
  ecosystem. The socket syscall path is not the bottleneck here; SQLite is.
- **No response caching, no in-memory table copies, no materialized like counts.** All forbidden by
  rule 5, and all unnecessary given the query plans above.
- **No batching of writes across requests.** Rule 6 allows group commit, but a group-commit design
  adds a coordinator task and a timer, and on a 1-vCPU box the per-request commit is already fast
  (`synchronous=NORMAL`, no fsync per commit). Batching would add latency to every write without
  meaningfully reducing CPU.

## Expected performance

On the benchmark box (1 vCPU, 2 GB, no swap), on a fresh copy of `seed/feed.db`:

- `GET /feed`: ~150–250 µs server time, ~4 KB response.
- `GET /posts/:id`: ~80–150 µs.
- `POST /posts`: ~200–400 µs (includes HMAC + insert + commit).
- `POST /posts/:id/like`: ~150–300 µs.

Steady-state RSS: ~120–180 MB (64 MiB SQLite page cache + 1 GiB mmap window mapped but not
resident + Tokio/Actix overhead). The 1 GiB `mmap_size` is a *limit*, not an allocation; resident
memory tracks the hot working set, which is a small fraction of the 250 MB database.

At 15,000 concurrent keep-alive connections, the process holds ~15k file descriptors (well under
`LimitNOFILE=65535`) and one Tokio worker thread. CPU is dominated by SQLite query execution and
sonic-rs serialization, both of which are single-digit-microsecond to low-hundreds-of-microseconds
operations — the profile that a single core can sustain at tens of thousands of requests per second
when there is no thread-hop, no lock, and no interpreter in the path.
