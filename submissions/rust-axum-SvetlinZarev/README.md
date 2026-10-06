# rust-axum-SvetlinZarev

The $12 server challenge API in Rust.

|                |                                                                                       |
|----------------|---------------------------------------------------------------------------------------|
| Language       | Rust 1.99 (edition 2024)                                                              |
| HTTP           | axum 0.8.9 (tokio + hyper, HTTP/1.1 keep-alive)                                       |
| SQLite         | rusqlite 0.40.2 (`bundled`, compiled into the binary)                                 |
| JWT            | jsonwebtoken 11.1.0 (`rust_crypto`, pure Rust)                                        |
| JSON           | serde_json, response structs field-ordered to match the spec                          |
| Allocator      | tikv-jemallocator                                                                     |
| **Deployment** | **Behind Nginx** — serves `127.0.0.1:3000`, Nginx (`bench/nginx.conf`) fronts port 80 |

Versions are pinned in `Cargo.lock`; `build.sh` uses `--locked`.

## Running it

```bash
sudo bash install.sh   # rustup toolchain 1.99.0 + build deps
bash build.sh          # cargo build --release --locked
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 bash start.sh
```

Config is the four env vars above, nothing else.

## Validation

```bash
bash test/run.sh submissions/rust-axum-SvetlinZarev   # 42/42
```

## Design

One principle: on a single vCPU, do the least work per request and keep the
write fsync off the latency path.

- **Single writer thread** owns the one read-write connection; handlers send
  writes over an mpsc channel and await a oneshot reply. SQLite allows one
  writer, so this serialises writes without locks or `SQLITE_BUSY`. The reply is
  sent only after `COMMIT`, so a 201 is durable (rule 6: WAL +
  `synchronous=NORMAL`).
- **Opportunistic group commit** — the writer drains whatever is already queued
  (≤32) into one transaction: N writes, one fsync. No timer, so latency is
  unchanged when idle. On error the batch rolls back and each write retries
  alone, so one failure never sinks its batch-mates.
- **Lock-free reads** via a per-worker-thread read-only connection. WAL lets
  connections read in parallel with no shared lock; on 1 vCPU that's one
  connection, scaling to one per core on bigger boxes.
- **Checkpoints off the commit path** — `wal_autocheckpoint=0`; the writer runs
  a `PASSIVE` checkpoint between batches when idle (or every 250 ms under load),
  with `TRUNCATE` past ~1000 frames to cap WAL size. It only relocates
  already-committed data; durability is at commit, not checkpoint.
- **Pragmas:** WAL, `synchronous=NORMAL`, `busy_timeout=5000`, `mmap_size=1 GiB`
  (reads via the OS page cache), `cache_size=64 MiB`, `temp_store=MEMORY`.
- **No caching** (rule 5): prepared statements cache the query *plan* only;
  every request reads live from SQLite; every JWT is verified every time (HS256,
  signature + `exp`).

## Benchmarks

`bench/load.js`, VUs stepped by 1000 (30s ramp + 45s hold each) until a run
breaks a threshold (p95≥500ms, p99≥1s, or ≥1% errors). Server pinned to one
worker (`TOKIO_WORKER_THREADS=1`).

**Direct** (k6 → server):

| VUs   | req/s | p50   | p95   | p99    | err% | result |
|-------|-------|-------|-------|--------|------|--------|
| 1000  | 76    | 0.9ms | 2.6ms | 4.5ms  | 0.00 | pass   |
| 2000  | 153   | 0.9ms | 2.6ms | 4.1ms  | 0.00 | pass   |
| 3000  | 229   | 0.7ms | 2.5ms | 4.5ms  | 0.00 | pass   |
| 4000  | 306   | 0.7ms | 2.6ms | 5.0ms  | 0.00 | pass   |
| 5000  | 382   | 0.6ms | 2.5ms | 5.9ms  | 0.00 | pass   |
| 6000  | 458   | 0.6ms | 3.0ms | 7.0ms  | 0.00 | pass   |
| 7000  | 535   | 0.6ms | 3.1ms | 8.0ms  | 0.00 | pass   |
| 8000  | 611   | 0.5ms | 3.3ms | 8.8ms  | 0.00 | pass   |
| 9000  | 688   | 0.5ms | 4.6ms | 10.8ms | 0.00 | pass   |
| 10000 | 773   | 0.5ms | 4.4ms | 11.0ms | 2.01 | fail   |

**Behind Nginx** (k6 → nginx → server, `bench/nginx.conf`):

| VUs   | req/s | p50   | p95   | p99     | err% | result |
|-------|-------|-------|-------|---------|------|--------|
| 1000  | 76    | 1.3ms | 3.7ms | 6.4ms   | 0.00 | pass   |
| 2000  | 153   | 1.0ms | 3.2ms | 5.9ms   | 0.00 | pass   |
| 3000  | 229   | 1.0ms | 3.3ms | 5.8ms   | 0.00 | pass   |
| 4000  | 306   | 1.0ms | 3.3ms | 6.2ms   | 0.00 | pass   |
| 5000  | 382   | 0.9ms | 2.9ms | 6.1ms   | 0.00 | pass   |
| 6000  | 458   | 0.9ms | 3.2ms | 7.4ms   | 0.00 | pass   |
| 7000  | 536   | 0.9ms | 3.8ms | 9.4ms   | 0.00 | pass   |
| 8000  | 610   | 0.8ms | 3.8ms | 9.3ms   | 0.00 | pass   |
| 9000  | 686   | 0.8ms | 3.5ms | 8.2ms   | 0.13 | pass   |
| 10000 | 776   | 0.7ms | 5.2ms | 873.7ms | 3.61 | fail   |

Both top out at **9000 VUs**, failing at 10000 on error rate (not latency — p50
stays sub-millisecond).

**On a dedicated Linux box** (same `bench/load.js` stepping) the server scales
far further, holding sub-2.2ms p99 all the way to 28000 VUs:

| VUs   | req/s | p50   | p95   | p99   | err% | result |
|-------|-------|-------|-------|-------|------|--------|
| 4000  | 307   | 0.3ms | 1.4ms | 2.2ms | 0.00 | pass   |
| 5000  | 383   | 0.3ms | 1.3ms | 1.9ms | 0.00 | pass   |
| 6000  | 460   | 0.2ms | 1.2ms | 1.8ms | 0.00 | pass   |
| 7000  | 538   | 0.2ms | 1.2ms | 1.7ms | 0.00 | pass   |
| 8000  | 611   | 0.2ms | 1.2ms | 1.7ms | 0.00 | pass   |
| 9000  | 691   | 0.2ms | 1.2ms | 1.7ms | 0.00 | pass   |
| 10000 | 769   | 0.2ms | 1.2ms | 1.7ms | 0.00 | pass   |
| 11000 | 841   | 0.2ms | 1.2ms | 1.7ms | 0.00 | pass   |
| 12000 | 917   | 0.2ms | 1.2ms | 1.7ms | 0.00 | pass   |
| 13000 | 994   | 0.2ms | 1.1ms | 1.7ms | 0.00 | pass   |
| 14000 | 1072  | 0.2ms | 1.1ms | 1.8ms | 0.00 | pass   |
| 15000 | 1147  | 0.2ms | 1.1ms | 1.8ms | 0.00 | pass   |
| 16000 | 1221  | 0.2ms | 1.1ms | 1.9ms | 0.00 | pass   |
| 17000 | 1306  | 0.2ms | 1.1ms | 2.0ms | 0.00 | pass   |
| 18000 | 1379  | 0.2ms | 1.1ms | 2.1ms | 0.00 | pass   |
| 19000 | 1455  | 0.2ms | 1.1ms | 2.1ms | 0.00 | pass   |
| 20000 | 1532  | 0.2ms | 1.1ms | 2.1ms | 0.00 | pass   |
| 21000 | 1608  | 0.2ms | 1.1ms | 2.1ms | 0.00 | pass   |
| 22000 | 1685  | 0.2ms | 1.1ms | 2.1ms | 0.00 | pass   |
| 23000 | 1760  | 0.2ms | 1.1ms | 2.1ms | 0.00 | pass   |
| 24000 | 1836  | 0.1ms | 1.1ms | 2.2ms | 0.00 | pass   |
| 25000 | 1914  | 0.1ms | 1.1ms | 2.1ms | 0.00 | pass   |
| 26000 | 1991  | 0.1ms | 1.1ms | 2.2ms | 0.00 | pass   |
| 27000 | 2067  | 0.1ms | 1.1ms | 2.2ms | 0.00 | pass   |
| 28000 | 2141  | 0.1ms | 1.1ms | 2.2ms | 0.00 | pass   |
| 29000 | 2238  | 0.1ms | 1.1ms | 2.2ms | 2.59 | fail   |

Tops out at **28000 VUs**, again failing on error rate rather than latency — p99
never crosses 2.2ms, so the ceiling is connection/accept capacity, not compute.

### Resource usage (direct, 8000 VUs)

| State      | CPU            | RSS     |
|------------|----------------|---------|
| Idle       | ~0%            | 5 MB    |
| Under load | ~11–13% / core | ~267 MB |

CPU is reported per core (100% = one core); the single pinned worker barely
ticks because each request is a sub-millisecond index lookup. Most of the ~267
MB RSS is the memory-mapped database (`mmap_size=1 GiB`) — OS page cache that
the kernel can reclaim, not anonymous heap. Well inside the 2 GB budget.

### Max throughput per endpoint

Open-loop, each endpoint hammered flat-out with no think time (`oha`, 100
connections, 15s):

| Endpoint               | req/s  | p50    | p95    | p99    | CPU   | RSS   |
|------------------------|--------|--------|--------|--------|-------|-------|
| `GET /health`          | 34,852 | 2.85ms | 3.05ms | 3.25ms | ~79%  | 10 MB |
| `GET /posts/:id`       | 32,994 | 3.04ms | 3.29ms | 3.95ms | ~96%  | 10 MB |
| `POST /posts/:id/like` | 32,388 | 3.09ms | 3.48ms | 4.03ms | ~108% | 11 MB |
| `POST /posts`          | 30,812 | 3.20ms | 3.72ms | 4.41ms | ~164% | 68 MB |
| `GET /feed`            | 20,186 | 5.03ms | 5.23ms | 5.53ms | ~99%  | 10 MB |

All 100% success. `/feed` is the floor — it returns 20 rows each with a
`count(*)` like-count subquery, ~20× the row work of a point lookup, and pegs
one core (compute-bound). Writes keep pace with reads because group commit
amortises the fsync across the deep batches a flat-out client produces;
`POST /posts` uses the most CPU (writer + workers) and the only notable memory
(inserting rows + WAL churn).

Measured on an Apple-silicon Mac with k6, nginx and the server all co-located;
the ceiling reflects the Mac's total connection capacity, not the server's
compute or the real 1-vCPU benchmark box.

