# Rust + Axum + SQLite

A Rust implementation of the five-endpoint feed API. Rust 1.93.0, Axum 0.8.4,
Tokio 1.47.1, rusqlite 0.37.0, serde 1.0.228, serde_json 1.0.145, base64 0.22.1,
hmac 0.12.1, and sha2 0.10.9 are pinned. `Cargo.lock` pins the entire resolution.
SQLite 3.53.4 is separately compiled and statically linked, not rusqlite's bundled
version. Default Axum features are disabled; only HTTP/1 and Tokio support are used.

Axum/Hyper owns HTTP connections; a single-thread Tokio runtime runs a database
actor with a bounded queue and batches of up to 256 ready requests. The actor owns
its SQLite connection, statement cache and transaction state. Pending replies use
oneshot channels, so dropped clients cannot invalidate response pointers. Ready
HTTP tasks are yielded to without a batching timer. Release builds use LTO and one
codegen unit. Application LOC counts `src/main.rs`, including comments and blanks,
after `rustfmt`.

The dependency count is **61 Linux production-graph crates plus SQLite = 62**,
including macro dependencies reached through normal dependency edges. Three
build-only crates (`pkg-config`, `vcpkg`, `version_check`) are disclosed separately;
including them gives 65 libraries. The cross-platform lockfile has 85 external
crates, or 86 including SQLite. Platform-specific unused crates are not charged to
the Linux score. There are eight direct Rust crates plus directly linked SQLite.
Rust/C runtimes and toolchains are excluded, consistently with the other stacks.
This package count reflects ecosystem boundaries, not dependency size or quality.

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

Baseline source accounting: **393 application LOC**, **1,197 total own source/config LOC**,
and **1,031,431 own plus inspected dependency-source LOC**. The latter
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

## Connection pressure — 2026-10-09

At ordinary connection counts, keep-alive retains the 75-second timeout. At
40,000 open connections, completed responses advertise `Connection: close`
and the transport drains the response before closing its socket. Clients reconnect
for their next request. Uploads finish and successful writes commit before this
response-based retirement takes place. The threshold is a connection/memory budget,
not a limit on the number of logical users. Counters store transport metadata.

Axum connection metadata owns an Arc lease whose final drop decrements the open-connection count. The keep-alive budget leaves 128 descriptors free when the inherited file limit is smaller. This uses the existing dependencies.

The source accounting and performance tables recorded before this change describe
the pinned baseline. New diagnostic evidence is in [capacity-results.json](capacity-results.json).
The selected local ARM64 diagnostic passed at **70,000 logical users**
for a **1-minute hold**, with zero errors and a worst-shard p99 of
**23.7 ms**. Peak process RSS was **978.9 MiB**;
sampled peak container memory was **1418.0 MiB**. This is
one diagnostic trial, not a maximum-capacity search or an official droplet score.

From the repository root, against a running server on a fresh database copy:

```bash
python3 submissions/rust-axum-tbsvttr/tests/connection_check.py 3000 --users 512 --seconds 20 --ramp 2 --timeout 5
```

For descriptor pressure, run the server with hard/soft `nofile=256` and give the
client a higher limit. The diagnostic uses the feed/post/like/create loop and think
times from `bench/load.js`, measures the hold, and reports request latency plus
latency including reconnects. It uses Python, separate loopback source addresses,
and no warm-up or ramp-down; it is not an official k6 score. It writes posts and likes.

For sustained runs on Linux, compile the socket generator. It retains only each
user's selected post ID, uses deferred ephemeral-port allocation on several
loopback addresses, and lets the server's FIN retire completed connections. The
body text and random-number generator differ from k6; the request mix, user loop
and think-time ranges match. It reports generator CPU, memory and loop delay:

```bash
cc -O3 -Wall -Wextra submissions/rust-axum-tbsvttr/tests/connection_load.c -lm -o submissions/rust-axum-tbsvttr/bin/connection-load
submissions/rust-axum-tbsvttr/bin/connection-load 3000 --users 70000 --seconds 300 --ramp 60 --tokens seed/tokens.json
```

With the same low server limit, this targeted regression keeps an unfinished upload
open during connection retirement, verifies committed writes, and checks that
keep-alive resumes after the other connections close:

```bash
python3 submissions/rust-axum-tbsvttr/tests/pressure_regression.py 3000
```

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
