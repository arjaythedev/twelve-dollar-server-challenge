# julia-raw-amanica

Julia 1.13.3 · no framework · SQLite 3.54.1 (amalgamation compiled from source, called with `ccall`) · **direct** (no Nginx)

Work in progress. See [PLAN.md](PLAN.md) for the roadmap and the decisions log.

## Current state

- Single-threaded, level-triggered epoll loop on raw file descriptors (no libuv, no Tasks).
- HTTP/1.1 parsed in place in the connection's read buffer; responses written straight into reusable
  byte buffers (no intermediate objects, no JSON library).
- JWT HS256 verified on every request with libcrypto's `HMAC`.
- Prepared statements via the SQLite C API. Reads go through a read-backend interface (`READ_BACKEND`
  in `config.sh`; only `sql`, the public-API backend, exists so far).

## Files

| File | |
|---|---|
| `config.sh` | pinned versions (Julia, SQLite) and build switches |
| `install.sh` | root: build-essential, curl, unzip, openssl; the official Julia tarball (sha256 checked) |
| `build.sh` | user: downloads the SQLite amalgamation (SHA3-256 checked), compiles `build/libjr.so` |
| `start.sh` | runs `src/main.jl` |
| `src/` | the server |
| `dev/` | local testing in podman (not used on the box), see below |
| `stress-results.csv` | every local stress run, one row each |

## Running locally

Needs podman (rootless is fine), plus `seed/feed.db` (from the repo root: `bash seed/make-seed.sh`).
Everything runs from this folder, in containers that mimic the box: Ubuntu 24.04, server limited to
1 CPU / 2 GB / no swap / nofile 65535. Each run copies the submission fresh into the container, so local
build output never leaks in.

```bash
dev/ct.sh test                                    # build + start + the 42 checks, as CI does
LABEL="what changed" dev/ct.sh stress 10000 1m    # one k6 run with 10k users → a row in stress-results.csv
LABEL="what changed" dev/ct.sh ladder 2500 20000  # double users until a run fails, then bisect to within 5%
GEN=blast LABEL="what changed" dev/ct.sh stress 256 30s  # cheap max-throughput run, 256 connections
GEN=blast LABEL="what changed" dev/ct.sh ladder          # connections 16 → 16384 (mostly shows queueing, see caveats)
GEN=users LABEL="what changed" dev/ct.sh stress 80000 1m # 80k users with think time: tests connection limits
GEN=users LABEL="what changed" dev/ct.sh ladder         # users 10k → 320k
dev/ct.sh report                                  # rebuild the results tables below from the CSV
dev/ct.sh shell                                   # shell inside the Ubuntu 24.04 dev image
```

Three load generators (`GEN`):

| `GEN` | What it runs | N means | Cost | Use it for |
|---|---|---|---|---|
| `k6` (default) | `bench/load.js`, the scoring's user model, think time included | virtual users | ~0.3 MB per user: this 32 GB host tops out near 20k | latency under the real traffic shape |
| `blast` | wrk + `dev/blast.lua`: the same request mix (feed 1 : post 1 : like 0.15 : create 0.02), no think time | open connections | a few MB | max req/s at 1 CPU, comparing optimizations |
| `users` | wrk + `dev/users.lua`: the same mix, each connection pausing 3–16 s (mean 9.5 s, like load.js) | users (one connection each) | ~25 KB per user | many open connections: fd limit, per-connection memory, accept rate |

Every run starts a fresh server on a fresh copy of the seed database (like the scoring), checks the same
thresholds (p95 < 500 ms, p99 < 1 s, errors < 1%), samples server CPU / RSS every 2 s, and records the boot
time (`start.sh` → first healthy `/health`). Raw output goes to `results/<time>-<gen><N>/` (gitignored).

wrk runs as several parallel processes (shards) in one generator container: `CONNS_PER_SHARD` (5000) or
`SHARDS` sets how many, `WRK_THREADS` (4) the threads in each. Each shard connects to its own `127.0.0.x`
(the server binds `0.0.0.0`), so one client address never runs out of its ~64k ephemeral ports. For `users`,
shard start times are spread over `RAMP_UP`, because wrk opens all its connections at once.

Other settings: `NOTES` (free text in the row), `RAMP_UP` (30s, k6 and users), `WARMUP` (5s, blast),
`CPUS` / `MEM` (1 / 2g), `SECCOMP=default` (podman's default profile, which blocks io_uring).

Caveats:
- Don't run two stress runs at once, and close heavy apps first: the generator shares the host. If the
  host swaps, k6 stalls and the run fails for reasons unrelated to the server. A note is printed when a
  run fails while server CPU stayed below 85%.
- Keep k6 users below about (free MB ÷ 0.3) (`free -m`).
- Blast's "est. users" (req/s × 10.6, the leaderboard ratio) is inflated: this desktop core is faster than
  the benchmark's shared Xeon vCPU, and blast does not include the cost of holding hundreds of thousands of
  idle connections. Compare blast numbers with each other only.
- Blast is closed-loop, so once the server is saturated, more connections only add queueing:
  latency ≈ connections ÷ req/s. The useful blast number is the saturated req/s (a single run at
  256 connections is enough). The blast ladder's "highest passing" just marks where that queue passes 500 ms.

## Progress

<!-- results:begin -->
Best local result per change. Raw runs: [stress-results.csv](stress-results.csv); how to run: above.
Blast req/s (max throughput at 1 CPU): median of the passing runs (they vary ~10%), with % change against the
previous change and the number of runs. Changes under ~10% are noise unless they repeat.
k6 / wrk users = highest passing run (not always a full ladder). k6 is capped near 20k users on this host;
wrk users (`GEN=users`, think time per connection) are cheap, so they reach the connection limits.

| change | k6 users | wrk users (server RSS) | blast req/s |
|---|--:|--:|--:|
| epoll baseline | 19,350 | – | – |
| epoll + heap-hint 512M | 10,000 | 60,000 (990 MB) | 23,832, n=12 |
| P3: shared buffers | – | 60,000 (315 MB) | 22,857 (-4%), n=3 |
<!-- results:end -->
