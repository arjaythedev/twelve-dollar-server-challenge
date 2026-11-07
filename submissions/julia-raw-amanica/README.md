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
| `dev/` | local testing in podman (not used on the box): `dev/ct.sh test`, `dev/ct.sh stress 5000 2m` |

## Local stress results

<!-- results:begin -->
Local runs, `dev/ct.sh` (podman: server 1 CPU / 2 GB; k6 on the same host, so numbers are only
comparable with each other, not with the official leaderboard). Source: [stress-results.csv](stress-results.csv).

**Best passing run per label**

| label | commit | VUS | req/s | p95 ms | p99 ms | err % | server CPU avg | RSS MB | boot ms |
|---|---|--:|--:|--:|--:|--:|--:|--:|--:|
| epoll baseline | 22d7c53+dirty | 19350 | 1542.5 | 1.958 | 24.505 | 0.000 | 19% | 540 | – |
| epoll + heap-hint 512M | 22d7c53+dirty | 10000 | 798.5 | 1.602 | 5.452 | 0.000 | 15% | 421 | 2278 |

**Last 20 runs**

| when (UTC) | label | VUS | dur | req/s | p95 ms | p99 ms | err % | CPU avg/max | result |
|---|---|--:|---|--:|--:|--:|--:|--:|---|
| 2026-11-07 01:13 | epoll + heap-hint 512M | 10000 | 1m | 798.5 | 1.602 | 5.452 | 0.000 | 15/29% | pass |
| 2026-11-07 01:04 | epoll baseline | 19350 | 1m | 1542.5 | 1.958 | 24.505 | 0.000 | 19/34% | pass |
| 2026-11-07 01:01 | epoll baseline | 18750 | 1m | 1494.8 | 1.621 | 10.433 | 0.000 | 21/43% | pass |
| 2026-11-07 00:58 | epoll baseline | 17500 | 1m | 1395.3 | 1.517 | 12.315 | 0.000 | 19/38% | pass |
| 2026-11-07 00:55 | epoll baseline | 15000 | 1m | 1195.2 | 1.626 | 13.448 | 0.000 | 20/32% | pass |
| 2026-11-07 00:52 | epoll baseline | 20000 | 1m | 1404.6 | 421.929 | 8052.534 | 0.000 | 28/48% | FAIL p(99)<1000 |
| 2026-11-07 00:49 | epoll baseline | 10000 | 1m | 796.6 | 1.352 | 3.296 | 0.000 | 12/24% | pass |
| 2026-11-07 00:46 | epoll baseline | 5000 | 1m | 399.3 | 0.789 | 1.807 | 0.000 | 6/11% | pass |
| 2026-11-07 00:43 | epoll baseline | 2500 | 1m | 199.2 | 0.884 | 1.842 | 0.000 | 4/7% | pass |
<!-- results:end -->
