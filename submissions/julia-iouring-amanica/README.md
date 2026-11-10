# julia-iouring-amanica

Julia 1.13.3 · no framework · SQLite 3.54.1 (amalgamation compiled from source, called with `ccall`) · **direct** (no Nginx)

Work in progress.

## Current state

- Single thread, no libuv, no Tasks. Transport (`TRANSPORT` in `config.sh`, default `auto`):
  - **io_uring** through raw syscalls (no liburing). Sockets are direct descriptors in the rings' file
    tables, not process fds, so `LimitNOFILE=65535` doesn't cap connections: a new ring opens when one is
    full. Multishot receives into a provided-buffer ring (an idle connection holds no buffer), 4096
    accepts in flight, sends from refcounted arenas, close = shutdown linked to close.
  - **epoll** fallback (level-triggered, raw fds) when io_uring is unavailable (seccomp, old kernel); capped
    at ~65k connections by the fd limit.
- HTTP/1.1 parsed in place in one read buffer shared by all connections; responses written straight into
  a shared output buffer (no intermediate objects, no JSON library). An idle connection holds no buffers;
  0 bytes allocated per request, so the GC does not run under load.
- No Julia packages: the server uses only Base and `ccall`.
- JWT HS256 verified on every request with libcrypto's `HMAC`.
- SQLite through its public C API only: prepared statements for every read and write (no B-tree internals).
  WAL checkpoints run on a background thread, so their fsyncs don't block the event loop.
- Group commit (io_uring): all writes of one event-loop pass share one transaction, committed before the
  loop hands any reply to the kernel, so each 201 still waits for its commit (rule 6). Max req/s went up
  ~48% (blast 30k → 45k); at 150k users p95 fell from 4–27 ms to 1.5–3.7 ms.

## Files

| File | |
|---|---|
| `config.sh` | pinned versions (Julia, SQLite) and build switches |
| `install.sh` | root: build-essential, curl, unzip, openssl; the official Julia tarball (sha256 checked) |
| `build.sh` | user: downloads the SQLite amalgamation (SHA3-256 checked), compiles `build/libjr.so` |
| `start.sh` | runs `src/main.jl` |
| `src/` | the server |
| `csrc/shim.c` | WAL checkpoints on a background thread (public SQLite API), compiled into `build/libjr.so` |
| `dev/` | **development only**: local test and benchmark tooling (podman, k6/wrk, `tools.jl`), see below |
| `stress-results.csv` | every local stress run, one row each (written by `dev/`) |

## Running locally

Everything in `dev/` (and `stress-results.csv`) exists only to test and benchmark while developing. The
submission itself is `install.sh`, `build.sh`, `start.sh`, `config.sh` and `src/`: none of them use `dev/`,
and the benchmark box never runs it. The dev tools' Julia package (JSON.jl) lives in `dev/`'s own project
environment; the server has no package dependencies.

Needs podman (rootless is fine), Julia on the host for `dev/tools.jl` (records runs and builds the table
below; its own pinned environment in `dev/Project.toml`, installed on first use; juliaup's `1.13` channel
is used if present, or set `JULIA_DEV`), plus `seed/feed.db` (from the repo root: `bash seed/make-seed.sh`).
Everything runs from this folder, in containers that mimic the box: Ubuntu 24.04, server limited to
1 CPU / 2 GB / no swap / nofile 65535. Each run copies the submission fresh into the container, so local
build output never leaks in.

```bash
dev/ct.sh test                                    # build + start + the 42 checks, as CI does
LABEL="what changed" dev/ct.sh stress 10000 1m    # one k6 run with 10k users → a row in stress-results.csv
LABEL="what changed" dev/ct.sh ladder 2500 20000  # double users until a run fails, then bisect to within 5%
GEN=blast LABEL="what changed" dev/ct.sh stress 256 30s  # cheap max-throughput run, 256 connections
GEN=blast LABEL="what changed" dev/ct.sh ladder          # connections 16 → 16384 (mostly shows queueing, see caveats)
GEN=users LABEL="what changed" dev/ct.sh --debug stress 80000 1m # 80k users with think time; --debug checks they all connected
GEN=users LABEL="what changed" dev/ct.sh ladder         # users 10k → 320k
dev/ct.sh report                                  # rebuild the results tables below from the CSV
dev/ct.sh shell                                   # shell inside the Ubuntu 24.04 dev image
dev/ct.sh clean                                   # remove stress containers left by an interrupted run
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

`--debug` (first argument, e.g. `dev/ct.sh --debug stress 60000`) also counts the server's open connections
(`users` runs then fail if fewer than 99% of users connected) and makes the server log allocation/GC stats
every 10 s (`JR_DEBUG=1`; bytes per request should stay 0). Off by default so measuring costs nothing. CPU
and memory are always sampled from the host side (cgroup files, `/proc`), outside the server's 1-CPU budget.
The CSV's `debug` column says which mode a run used.

The server's database copy lives on a per-run podman volume on the host's disk (`DATA=volume`, the default).
`DATA=overlay` puts it on the container's own fuse-overlayfs instead, where throughput measured about twice
as high (probably cheaper checkpoint fsyncs, not verified), so numbers there may flatter the server: runs
before 2026-11-08 used it.

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
| P3: shared buffers | – | 60,000 (315 MB) | 21,288 (-11%), n=6 |
| P4: io_uring | – | 175,000 (396 MB) | 24,699 (+16%), n=4 |
| P5.2: checkpoint auto | – | – | 11,448 (-54%), n=3 |
| P5.2: checkpoint thread | – | – | 32,668 (+185%), n=3 |
| P5.3: write hold | – | 225,000 (460 MB) | 30,557 (-6%), n=1 |
| P5.3b: restart loop | – | 175,000 (426 MB) | – |
| P5.3b: restart thread | – | 175,000 (428 MB) | – |
| P5.3c: restart hold | – | 175,000 (433 MB) | – |
| P5.3c: restart catchup | – | – | – |
| P5.4: group off | – | 150,000 (418 MB) | 30,472 (-0%), n=3 |
| P5.4: group on | – | 150,000 (420 MB) | 45,018 (+48%), n=3 |
<!-- results:end -->
