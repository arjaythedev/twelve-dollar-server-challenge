# Zig + uSockets + SQLite

Direct HTTP/1.1 submission, without Nginx. The API, authentication, JSON output,
HTTP framing and response ownership are implemented in Zig 0.17.0. uSockets supplies
the event loop and TCP sockets; picohttpparser parses HTTP headers and chunked bodies;
SQLite is called through generated C bindings. JSON parsing, UTF-8, base64 and
HMAC-SHA256 use Zig's standard library.

## Build and run

On Ubuntu 24.04, from this directory:

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

The challenge service supplies permission to bind port 80. For local testing use
`HOST=127.0.0.1 PORT=3000`. `SQLITE_PATH` must name an existing seeded database.
Defaults are `seed/feed.db`, `twelve-dollar-challenge`, `127.0.0.1`, and `3000`.

`install.sh` supports Linux x86_64 and aarch64 and verifies the official compiler
archive's SHA256. `build.sh` runs as a normal user, compiles the C libraries with
the system C compiler and the application with Zig `ReleaseFast`, and links one
executable. No CPU affinity or kernel settings are changed by the submission.
`ZIG_OPTIMIZE=ReleaseSafe bash build.sh` enables Zig runtime safety checks.

All three external libraries are direct dependencies, pinned and SHA256-verified:

| Library | Version / revision |
|---|---|
| SQLite | 3.53.4 |
| uSockets | `86097c490263ab662d62e8e7b541390bdec7d149` |
| picohttpparser | `465a7ff09fbd3432fe56c673451f5460154d1f07` |

There are **3 direct / 3 resolved dependencies**, excluding Zig's standard library,
libc, libm and the compiler. TLS is disabled. Application code is **680 physical
lines** across `src/*.zig` and `src/c.h`, including blank lines and comments, after
`zig fmt`. Automatically translated C declarations and downloaded library source
are excluded from application LOC. Build, test and benchmark code is counted
separately in the recorded source inventory.

## Requests and transactions

One thread owns the sockets, prepared statements and SQLite connection. Every read
queries the database and every authenticated request verifies its signature and
expiry. There are no response, query-result or JWT caches. Retained byte buffers
reuse allocations but are cleared between responses.

The first write starts `BEGIN IMMEDIATE`. Requests handled in the same event-loop
iteration may share that transaction. `COMMIT` runs at the end of the iteration,
or after 256 queued responses, without an artificial timer delay. All responses
produced while a transaction is open own their bytes and wait for commit, including
reads that saw uncommitted writes. A failed commit rolls back and changes the queued
responses to HTTP 500. Constraint errors that leave the transaction intact do not
undo other successful requests. A disconnected client's completed write can commit
even when its response cannot be delivered.

SQLite uses WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, an 8 MiB page
cache target and a 256 MiB mmap limit. The schema, indexes and triggers are unchanged.
`INSERT ... RETURNING` is stepped through `SQLITE_DONE` before a success can be sent.
Unused SQLite threading, dynamic extension loading, memory statistics, shared cache,
progress callbacks, deprecated APIs and declared-column-type metadata are disabled.

The HTTP layer handles fragmented requests, chunked uploads, trailers, pipelining,
partial writes, half-closes and `100 Continue`. Ambiguous content lengths, conflicting
transfer encodings and duplicate authorization headers are rejected. Socket buffers
are allocated lazily. Bodies are limited to 16 KiB, headers to 32 KiB / 64 fields,
encoded chunked bodies to 128 KiB, buffered input to 512 KiB and queued output to
8 MiB per connection. The idle timeout is 75 seconds. The process raises its own
descriptor soft limit to the existing hard limit.

## Connection pressure — 2026-10-09

At ordinary connection counts, keep-alive retains the 75-second timeout. At
60,000 open connections, completed responses advertise `Connection: close`
and the transport drains the response before closing its socket. Clients reconnect
for their next request. Uploads finish and successful writes commit before this
response-based retirement takes place. The threshold is a connection/memory budget,
not a limit on the number of logical users. Counters store transport metadata.

The socket open/close callbacks count connections. The keep-alive budget leaves 128 descriptors free when the inherited file limit is smaller. An interim 100 Continue response retains the connection so its upload can finish.

The source accounting and performance tables recorded before this change describe
the pinned baseline. New diagnostic evidence is in [capacity-results.json](capacity-results.json).
The selected local ARM64 diagnostic passed at **70,000 logical users**
for a **1-minute hold**, with zero errors and a worst-shard p99 of
**18.6 ms**. Peak process RSS was **65.6 MiB**;
sampled peak container memory was **599.9 MiB**. This is
one diagnostic trial, not a maximum-capacity search or an official droplet score.

From the repository root, against a running server on a fresh database copy:

```bash
python3 submissions/zig-usockets-tbsvttr/tests/connection_check.py 3000 --users 512 --seconds 20 --ramp 2 --timeout 5
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
cc -O3 -Wall -Wextra submissions/zig-usockets-tbsvttr/tests/connection_load.c -lm -o submissions/zig-usockets-tbsvttr/bin/connection-load
submissions/zig-usockets-tbsvttr/bin/connection-load 3000 --users 70000 --seconds 300 --ramp 60 --tokens seed/tokens.json
```

With the same low server limit, this targeted regression keeps an unfinished upload
open during connection retirement, verifies committed writes, and checks that
keep-alive resumes after the other connections close:

```bash
python3 submissions/zig-usockets-tbsvttr/tests/pressure_regression.py 3000
```

## Validation

After building, with the repository seed available:

```bash
python3 submissions/zig-usockets-tbsvttr/verify.py
bash submissions/zig-usockets-tbsvttr/tests/commit_check.sh
```

`verify.py` creates a fresh database and runs:

- All 42 official byte-exact API checks.
- 62 additional JSON, Unicode, trimming, JWT and ordering checks, plus 16 concurrent
  duplicate likes.
- HTTP framing rejection, `100 Continue`, chunk extensions/trailers, half-close,
  and 600 ordered pipelined write/read responses across commit batches.
- Fragmented/chunked uploads, 100 aborted uploads, 100 resets after complete writes,
  1,800 pipelined responses under backpressure, and same-socket reuse after 66 seconds.
- Recovery of 192 acknowledged posts and their likes after `SIGKILL`, interleaved
  with 32 rejected foreign-key writes.

Both `ReleaseSafe` and the measured `ReleaseFast` build passed these checks on Linux
ARM64. The separate commit-test executable injects failed and blocked commits. It
checks rollback, recovery, and that neither write success nor dependent read data
escapes before commit. The injection wrapper is absent from the production binary.
Crash recovery here covers a process crash; `synchronous=NORMAL` is the challenge's
accepted durability setting and does not promise power-loss durability for every
acknowledgement.

## Local comparison

The accompanying measurements use Ubuntu 24.04 ARM64 in Docker Desktop on an Apple
M2 Pro. Every candidate is rerun in this batch: one server CPU, 2 GiB RAM, no swap,
and a separate two-CPU load container on different cores. CPU placement belongs to
the local measurement harness, not to the submitted server. All six executables
pass the official 42 checks in the measurement environment.

Each trial starts a fresh process and seed copy, warms up for two seconds, then runs
`wrk` for 15 seconds with two threads and 64 connections. Request proportions are
100 feed reads, 100 single-post reads, 15 likes and 2 creates; post IDs follow the
live feed. Results are medians of three trials in seeded shuffled order. RAM is
post-load process RSS, not just managed heap or binary size. Raw outputs, source
and executable hashes, verification results, process metrics, and the scoring
formula are retained with the comparison data.

These measurements are local throughput experiments. The official Ubuntu x86_64
droplet and its five-minute k6 user-capacity score have not been measured.

| Implementation | Requests/s | RAM MiB | App LOC | Resolved deps | U5 reference score |
|---|---:|---:|---:|---:|---:|
| [C++ / uWebSockets #15](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/15) | 73,603.22 | 43.23 | 424 | 4 | **88.7** |
| **Zig / uSockets** | 72,191.36 | 42.12 | 673 | 3 | **84.3** |
| [Rust / raw epoll #19](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/19) | 83,958.01 | 41.14 | 1,357 | 25 | **75.4** |
| [Rust / Axum #17](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/17) | 58,345.52 | 42.80 | 393 | 62 | **71.2** |
| [Go / net/http #18](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/18) | 32,883.93 | 53.24 | 391 | 2 | **68.3** |
| [Fortran / libmicrohttpd #16](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/16) | 30,568.17 | 40.62 | 731 | 3 | **62.8** |

Zig is 1.9% lower in median throughput and uses 2.6% less post-load RSS than C++ #15 in this batch. It has one fewer library and 249 more application lines. C++ retains the higher weighted score. These small speed/RAM differences are observations from three trials, not a claim of statistical significance.

All **18 trials** completed with zero reported HTTP or socket errors, and every resulting database passed `PRAGMA quick_check`. Zig ranged from 71,417.62 to 72,275.42 requests/s.

The reference score uses the agreed **40% throughput, 20% application LOC, 20% resolved dependencies, 20% RAM**. To keep the original scale, the published [U5 comparison](https://github.com/arjaythedev/twelve-dollar-server-challenge/issues/7#issuecomment-5995788219) min/max bounds are held fixed. Every row above contains fresh Z1 measurements; historical U5 throughput/RAM values are not mixed into this table. Each normalized component is clipped to 0–100 when a new measurement falls outside the reference range. This is a local weighted index, separate from the official k6 score.

For throughput, `component = 100 × (value − min) / (max − min)`; for the other metrics, `component = 100 × (max − value) / (max − min)`. Bounds are 3,470.09–83,948.09 requests/s, 105–1,357 app LOC, 0–111 libraries, and 39.84375–237.30859375 MiB RSS. See [`scoring.json`](scoring.json) and [`benchmark-results.json`](benchmark-results.json) for full-precision values, trials and evidence. The request mix is [`bench.lua`](bench.lua).

A separate connection-capacity check held **15,000 idle sockets** open while 64 active
connections ran the same mixed workload for eight seconds after a two-second warmup.
All 15,000 original sockets were reused successfully after 66 seconds. This single
exploratory run reported 58480.74 requests/s, 44.20 MiB RSS and no
HTTP/socket errors. It is recorded separately and does not affect the comparison score.
