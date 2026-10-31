# Python + Granian

A direct HTTP implementation using Python handlers, Granian's native RSGI interface, and SQLite-generated JSON. Application, server and pressure helper total 183 physical lines.

| Component | Version |
|---|---|
| Python | 3.12.3, Ubuntu 24.04's package |
| SQLite | 3.45.1, through standard-library `sqlite3` |
| HTTP server | Granian 2.8.4, native RSGI, HTTP/1 |
| JSON validation and small responses | orjson 3.12.0 |
| CLI dependency | click 8.5.0 |
| Deployment | Direct, `0.0.0.0:80`; no Nginx |

All three installed runtime packages are pinned in `requirements.txt`.

## Connection pressure — 2026-10-09

The Linux process descriptor count is sampled on the response path at most once
per 100 ms. At 40,000 open descriptors, responses advertise `Connection: close`;
the transport drains their headers and body before retiring the socket. Smaller
inherited limits reduce the budget with a reserve of 128. An `EMFILE`/`ENFILE`
sampling failure also triggers retirement. The sample includes non-socket
descriptors and incomplete uploads; it stores transport metadata, not API data.
On platforms without `/proc`, normal keep-alive remains available without this
Linux pressure guard. Successful writes commit before their replies.

Ordinary connection counts retain the 75-second keep-alive timeout. Clients
reconnect after a retired response. The resource budget is not a logical-user cap.

The source and throughput comparisons recorded before this update describe their
pinned baseline revisions. New evidence and exact source hashes are recorded in
[capacity-results.json](capacity-results.json).

The selected diagnostic served **70,000 logical users** for a **5-minute hold**, with **0 errors**, worst-shard p95/p99 **33.8/42.8 ms**, and sampled peak container memory **1289.1 MiB**. The sum of process RSS high-water marks peaked at **872.7 MiB**; shared pages may be counted more than once. This is one local trial, not a maximum-capacity search.

Run correctness and recovery checks from the submission directory on a fresh seed:

```bash
python3 verify.py
```

With server hard/soft `nofile=256` and a higher client limit:

```bash
python3 tests/pressure_regression.py 3000
python3 tests/fd_exhaustion.py 3000
```

The first regression finishes an interrupted Unicode upload during retirement,
checks committed writes/readbacks, and confirms keep-alive returns when pressure
subsides. The second exhausts descriptors with incomplete HTTP headers before
completing one valid request and checking that its full response drains and closes.

For a sustained Linux diagnostic, against a running server and fresh seed copy:

```bash
mkdir -p bin
cc -O3 -Wall -Wextra tests/connection_load.c -lm -o bin/connection-load
bin/connection-load 3000 --users 70000 --seconds 300 --ramp 60 --tokens ../../seed/tokens.json
```

The C generator follows the feed/post/like/create loop, request probabilities and
think-time ranges in `bench/load.js`. It uses a different PRNG and post text,
several loopback source addresses, and no separate warm-up or ramp-down. It reports
hold-only request latency, latency including reconnects, and generator resource
usage. These are local diagnostics, not official k6/droplet scores or a search for
maximum capacity. The diagnostic writes posts and likes.

## Run

From this submission directory:

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` installs Python and venv support. `build.sh` creates `.venv` and installs pinned binary packages as a normal user. The existing seeded database is required; startup never creates or changes its schema.

## Implementation

Granian runs one worker, one Rust runtime thread, and one Python asyncio event loop. The native [RSGI interface](https://github.com/emmett-framework/granian/blob/v2.8.4/docs/spec/RSGI.md) delivers a request scope and protocol object; a response is one `protocol.response_bytes(...)` call. The application awaits the request body only for post creation, after authentication and before accessing SQLite.

A single connection and cursor execute SQLite queries inline. There are no suspension points between a like insertion and its existence check. Every query calls `fetchall()`, which exhausts `RETURNING` and finishes each autocommit write before its response is sent.

SQLite uses WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, a 500-page cache, and a 512 MiB mmap limit. Prepared statements and SQLite/OS page caches are used; responses, query results, table contents, and token-verification results are not cached. The fixed schema and indexes are unchanged.

SQLite constructs complete post/feed/create JSON in the specified key order. Feed aggregation explicitly orders by `created_at DESC, id DESC`. Casting the complete document to `BLOB` returns Python `bytes`, avoiding a text decode followed by JSON serialization. Empty feeds remain `{"posts":[]}`.

Every authenticated request verifies its HS256 signature and expiration. Optional `nbf` is checked, numeric booleans are rejected, and JSON decoding validates UTF-8. Bodies use JavaScript's exact whitespace set and a 500-code-point limit. Authentication precedes body and post-ID validation.

`server.py` sets `HTTP1Settings(header_read_timeout=75_000)` because Granian's CLI caps this setting at 60 seconds. The 75-second timeout permits the required idle keepalive interval. `backpressure=65536` accommodates 15,000 idle connections: Granian's backpressure limit counts connections, including keepalive sockets.

## Size

Physical lines, including blanks and comments, counted identically for all submissions:

| Measure | Original Python, PR #1 | FastAPI optimization, PR #10 | This submission |
|---|---:|---:|---:|
| Application + server | 235 (215 + 20) | 312 (292 + 20) | 156 (144 + 12) |
| Above + install/build/start scripts + requirements | 272 | 349 | 176 |
| Installed runtime packages | 17 | 17 | 3 |

Tests, documentation, lock files, and virtual environments are excluded. Physical lines are counted by LF; the baselines contain literal Unicode line-separator characters inside a whitespace string.

## Local comparison

The following are medians of three runs per workload and implementation, with order rotated between trials. Each run used a fresh seed copy, a two-second warmup, and 15 seconds of measurement. `wrk` used two threads and 64 connections. The mixed workload used relative weights of 100 feeds, 100 single-post reads, 15 likes, and two creates, with live feed IDs and seed JWTs.

All servers ran directly on Ubuntu 24.04 ARM64 under Docker Desktop on an Apple M2 Pro, limited to one CPU and 2 GiB RAM without swap. The load generator ran separately on other CPU cores. These are local throughput measurements, not the official x86_64 five-minute k6 user-capacity score.

| Workload, requests/second | PR #1 | PR #10 | This submission |
|---|---:|---:|---:|
| Feed | 11,409 | 14,291 | 18,146 |
| Single post | 17,318 | 37,385 | 45,135 |
| Mixed reads/writes | 12,213 | 19,534 | 25,328 |

| Workload, p99 milliseconds | PR #1 | PR #10 | This submission |
|---|---:|---:|---:|
| Feed | 11.03 | 11.46 | 5.81 |
| Single post | 7.14 | 3.27 | 2.74 |
| Mixed reads/writes | 16.83 | 14.95 | 12.32 |

All 27 measured trials reported zero HTTP/socket errors. Compared with PR #10, median throughput
increased by 27% for feeds, 21% for single posts and 30% for mixed traffic, with lower p99 in each
workload. Application/server source is 50% smaller and installed runtime packages fall from 17 to 3.
Mixed summed process RSS was 92.97 MiB for PR #1, 92.88 MiB for PR #10 and 95.48 MiB here. RSS sums
include the Granian supervisor and worker and can count shared pages more than once.

The unchanged comparison heads were PR #1 at `71d67de0e49b2152a558b41b997e9bdd918493a2` and PR #10 at `606970b8c9c85bcebf299f7208a99525d4d6aa23`.

## Validation

- All 42 official API checks passed; both unchanged baselines also passed all 42.
- A fresh dependency build as an unprivileged user succeeded.
- `check.py` passed 79 additional checks plus 16 concurrent duplicate-like requests, covering JWT claims and expiry, malformed JSON/UTF-8, Unicode, auth ordering, concurrent creates, and live counts.
- A process-group `SIGKILL` followed by restart preserved committed data.
- A separate connection test opened and validated 15,000 sockets in 1.74 seconds. It reused 200 original sockets after 66.08–67.79 seconds of idle time; summed server-process RSS was 349.86 MiB (shared pages can be counted more than once).

Run the API checks against a disposable seed copy, because they create posts and likes:

```bash
# From the repository root:
bash test/run.sh submissions/python-granian-tbsvttr
# Against a separately started server on another disposable database:
JWT_SECRET=twelve-dollar-challenge python3 submissions/python-granian-tbsvttr/check.py http://127.0.0.1:3000
```

## Credits

The starting point is [cknutson12's Python submission, PR #1](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/1), including its SQL, direct deployment, inline SQLite, and JWT approach ([compared source](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/71d67de0e49b2152a558b41b997e9bdd918493a2/submissions/python-fastapi-cknutson12)). [tejasnafde's PR #10](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/10) demonstrated the cost of framework dispatch and used raw ASGI endpoints ([compared source and analysis](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/606970b8c9c85bcebf299f7208a99525d4d6aa23/submissions/python-fastapi-tejasnafde)). This implementation builds on that work using Granian's native RSGI protocol and SQLite JSON bytes.

MIT, under the repository's [license](../../LICENSE).
