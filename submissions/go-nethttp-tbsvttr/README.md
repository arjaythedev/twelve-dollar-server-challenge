# Go + net/http + SQLite

A Go implementation of the five-endpoint feed API, using the standard HTTP, JSON,
base64 and HMAC/SHA-256 libraries. Go 1.27.1 and `mattn/go-sqlite3` 1.14.48 are pinned;
SQLite 3.53.4 is separately built and statically linked using the driver's `libsqlite3`
mode. Go's standard library/runtime and the platform C runtime are not counted as
external dependencies. The driver and SQLite count as **two resolved libraries**.

The standard HTTP server owns connections and upload handling. A bounded queue
feeds one database goroutine owning a single connection and its prepared statements.
It batches up to 256 ready requests, yields to ready goroutines without a timer,
and replies after commit. `GOMAXPROCS=1` matches the one-CPU deployment target.
Idle, request and response timeouts are 75 seconds. Application LOC counts all of
`main.go`, including comments and blank lines, after `gofmt`.

## Build and run

On Ubuntu 24.04:

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Direct HTTP/1.1 with keep-alive, without Nginx. Configuration uses only the four
challenge environment variables. Installation supports Linux x86_64 and ARM64;
measurements below are native ARM64, not the official x86_64 droplet.

Source accounting: **391 application LOC**, **1,181 total own source/config LOC**,
and **300,744 own plus inspected dependency-source LOC**. The latter
is a scoped source footprint, not executed code or a complete runtime footprint.
File scopes and hashes are recorded in `source-inventory.json`; documentation,
lockfiles, benchmark JSON and downloaded/build output are excluded from own LOC.

## Database and transaction behavior

The fixed schema and indexes are unchanged. All requests read SQLite directly;
there are no response/query/JWT caches. Every authenticated request checks HS256,
the signature, expiration, optional `nbf`, positive string `sub`, and string username.
Prepared statements are reused. JSON output preserves the required key order.
Body validation counts Unicode scalars and trims the same whitespace set as
JavaScript `String.trim`; embedded NULs are preserved.

All four compared servers use SQLite 3.53.4, WAL, `synchronous=NORMAL`, foreign keys,
exclusive locking, an 8 MiB page-cache target, and a 256 MiB mmap limit. Mapped pages
and other allocations are additional to the cache target. The three new builds use
the same source archive, SHA256 verification, and SQLite compiler options, including
thread support for the Go driver. C++ retains its existing single-thread SQLite build.
The application is compiled from source without CPU-specific architecture flags.

Ready requests can share a transaction without an intentional batching timer.
Success is sent only after COMMIT; reads that see an open transaction also wait.
Commit failure rolls back and changes queued responses to HTTP 500. A peer that
abandons a complete request may still have its write committed; it receives no
success response. Constraint failures do not undo unrelated successful statements.
An automatically rolled-back transaction invalidates the whole pending batch.

## Validation

After generating the repository seed, run from the submission directory:

```bash
python3 verify.py
bash tests/commit_check.sh
```

`verify.py` uses a fresh database and checks all 42 official cases, 62 additional
HTTP cases, 16 simultaneous duplicate likes, Unicode boundaries, strict JSON and
JWT validation, fragmented/chunked uploads, 100 abandoned partial uploads,
100 reset complete uploads, 1,800 pipelined responses under backpressure, and reuse
of the original connection after 66 idle seconds. It also verifies that 192
acknowledged creates and their likes survive SIGKILL/restart, interleaved with 32
failed foreign-key writes. This tests process recovery, not power-loss durability.

The separate fault-test executable installs a SQLite commit hook that can fail or
block a commit. It checks rollback, withholding success until commit, and successful
writing afterward. Fault injection is absent from the production executable.
Fresh builds from source and all 42 official checks also passed as unprivileged
user `nobody` after running the supplied installation scripts. Results are recorded
in `validation-results.json`.

## Measured four-stack comparison — 2026-10-06

All rows below were measured together under the setup described below. Mixed-load
figures are medians of five 15-second trials. All 20 mixed, 24 read-only, and four
idle-capacity measurements completed without reported HTTP or socket errors.

| Implementation | Mixed req/s | p99 ms | RSS MiB | App LOC | Resolved deps | Weighted score |
|---|---:|---:|---:|---:|---:|---:|
| C++ / uWebSockets group commit | 55,771.64 | 15.71 | 41.56 | 424 | 4 | **86.4** |
| Fortran / libmicrohttpd | 24,130.08 | 15.17 | 39.98 | 731 | 3 | **62.1** |
| Rust / Axum | 45,628.64 | 14.45 | 41.75 | 393 | 62 | **67.5** |
| Go / net/http | 25,928.66 | 14.73 | 52.03 | 391 | 2 | **67.7** |

Read-only medians use three eight-second trials. The capacity checks use one
eight-second mixed run with 15,000 additional idle connections; all 15,000 original
sockets successfully served another health request afterward.

| Implementation | Feed req/s | Single-post req/s | Mixed req/s with 15k idle | RSS with 15k idle, MiB |
|---|---:|---:|---:|---:|
| C++ / uWebSockets group commit | 38,868.12 | 177,185.81 | 57,719.91 | 43.28 |
| Fortran / libmicrohttpd | 14,694.68 | 100,742.30 | 23,882.57 | 517.40 |
| Rust / Axum | 32,621.42 | 141,368.93 | 43,844.58 | 282.99 |
| Go / net/http | 16,993.29 | 52,590.20 | 24,790.54 | 536.20 |

Memory at 15,000 idle connections is substantially different from the 64-connection
score input. The score retains the existing agreed post-load RSS definition; it
does not substitute the exploratory capacity measurement.

Scores retain **40% throughput, 20% application LOC, 20% resolved dependencies,
20% RAM** and recompute observed min/max bounds across 17 solutions: the preceding
14-row snapshot, replacing C++ #15's throughput/RSS with this fresh measurement,
plus these three new submissions. Other historical measurements are unchanged.
This is an illustrative preference index across batches. C++'s previous **94.0**
used another batch and bounds and is not directly comparable with its current
score. The small overall difference between Go and Rust is not evidence of a
statistically significant ranking. Rust has substantially higher measured
throughput, while Go gains points from fewer externally packaged dependencies.

[`benchmark-results.json`](benchmark-results.json) retains every raw output, CPU/RSS
sample and measured binary hash. [`scoring.json`](scoring.json) records all inputs,
bounds and component scores; [`source-inventory.json`](source-inventory.json)
records file-level LOC and dependency-source hashes. See `validation-results.json`
for correctness results. None of these numbers is the official five-minute k6 score.

## Reproduce the comparison

Install the three new submissions and C++ `cpp-uwebsockets-v2-tbsvttr` in a checkout.
Two Ubuntu 24.04 containers mount that checkout at `/work`: `twelve-language-server`
with the build/test tools, one CPU, 2 GiB RAM, 2 GiB memory+swap (no container swap),
and `nofile=65535:65535`; and `twelve-language-loadgen` with wrk/Python, other CPUs,
and `--network container:twelve-language-server`. CPU assignments are benchmark
controls only and are not part of any submission's startup script.

From this directory on the host:

```bash
python3 compare.py run --trials 5 --seconds 15 --output mixed-results.json
python3 compare.py run --workloads feed,post --trials 3 --seconds 8 --output read-results.json
python3 idle_compare.py --output idle-results.json
```

The first two commands retain raw output and refuse to overwrite existing evidence.
Every run starts on a fresh seed, warms for two seconds, and uses two wrk threads
and 64 connections. The mixed script averages 100 feeds, 100 post reads, 15 likes,
and two creates per 217 requests, with live IDs and real writes. It has no think
time or independent virtual-user state. Orders rotate/reverse between trials.
RAM is post-load process RSS, including resident mappings; peak RSS and CPU time
are also retained. Kernel socket memory and the load generator are excluded.
The idle test adds 15,000 connections, then verifies every original socket again.

This is a comparison of these implementations and library choices on this host.
It is not a universal language ranking or the challenge's five-minute k6 user score.
Host activity and the shared Docker VM can affect measurements.

## License

Application code is MIT under the repository license. Dependency sources retain
their upstream licenses; no downloaded code, toolchains, or binaries are committed.
