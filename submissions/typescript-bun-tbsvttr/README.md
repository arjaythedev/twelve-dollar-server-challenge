# Minimal Bun

130 lines of application TypeScript including the pressure helper, Bun's built-in HTTP router, SQLite, and HMAC. No npm packages.
Serves **directly**, with `HOST=0.0.0.0 PORT=80` on the benchmark machine.

| Component | Version |
|---|---|
| Runtime, HTTP server, SQLite driver, crypto | Bun 1.4.2 |
| Application dependencies | None beyond Bun |

`install.sh` installs the official, checksum-pinned Linux x86_64 Bun runtime. `build.sh` compiles
the source into a standalone executable; `start.sh` runs it in the foreground. The executable
includes Bun, so the small application source does not imply a tiny executable.

```bash
sudo bash install.sh                 # Ubuntu 24.04, once
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Use a fresh copy of the seed database, never the seed itself. From the repository root:

```bash
bash seed/make-seed.sh
bash test/run.sh submissions/typescript-bun-tbsvttr
```

For extra checks against an already-running test server:

```bash
JWT_SECRET=secret bun submissions/typescript-bun-tbsvttr/check.ts http://127.0.0.1:3000
```

## Connection pressure — 2026-10-09

The Linux process descriptor count is sampled on the response path at most once
per 100 ms. At 60,000 open descriptors, responses advertise `Connection: close`;
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

The selected diagnostic served **70,000 logical users** for a **5-minute hold**, with **0 errors**, worst-shard p95/p99 **42.3/60.1 ms**, and sampled peak container memory **690.6 MiB**. The sum of process RSS high-water marks peaked at **156.8 MiB**; shared pages may be counted more than once. This is one local trial, not a maximum-capacity search.

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

## Design

- A single process owns one SQLite connection. `locking_mode=EXCLUSIVE` avoids repeatedly releasing
  database locks. No other connection or checkpoint worker opens this database while the app runs.
- WAL with `synchronous=NORMAL` and SQLite's automatic checkpoints. Writes use autocommit;
  `INSERT ... RETURNING` is consumed completely before constructing a 201 response.
- Prepared statements use the existing indexes. Every request reads current SQLite data;
  `Response.json()` serializes rows with the required key order. No result or token-verification cache.
- A 64 MiB SQLite page cache and a 256 MiB mmap limit. Bun's development mode is explicitly disabled.
- HMAC key initialization is reused, but each JWT's signature, algorithm, expiration and claims are
  verified separately. The signature comparison uses the built-in constant-time function.
- Native HTTP parsing, routing and backpressure; 75-second keep-alive. No application thread pool,
  write queue or connection-reset strategy. The process remains subject to the service's 65,535
  descriptor limit, including database and internal descriptors.
- Foreign keys are enabled. Unicode body limits count code points rather than UTF-16 units.

The reference queries in `SPEC.md` and the single-connection design in
[PR #3](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/3) informed this implementation.

## Validation

The official suite passed all 42 checks locally on macOS ARM64 with Bun 1.4.2.
Additional checks cover Unicode boundaries, simultaneous duplicate likes, malformed signed JWTs,
token expiry, fresh reads and ordering of concurrently created posts. A separate process test
confirmed that an acknowledged post and like survived `SIGKILL` and restart. That tests process-crash
recovery, not power-loss durability.

The official Ubuntu/DigitalOcean five-minute k6 score has not been measured.

## Local comparison, 2026-10-04

Compared with PR #3 at `7dbe6be6d4e1097b856951f2b3bc71b9e48bd387`, using Bun 1.4.2 on macOS
ARM64. Both servers were compiled, tested sequentially, and given a fresh copy of the same seed
for every run. The only platform adaptation to PR #3 was guarding its Linux-only `libc.so.6`
backlog call. Its HTTP and database code were unchanged.

Median requests/second across three trials per case, with alternating server order, 64 keep-alive
connections, one wrk thread, a two-second warm-up and an eight-second measurement:

| Workload | PR #3 | Minimal Bun | Difference |
|---|---:|---:|---:|
| Feed reads | 27,249 | 29,082 | +6.7% |
| Single-post reads | 96,678 | 83,682 | -13.4% |
| Mixed reads and writes | 30,338 | 33,404 | +10.1% |

No HTTP or socket errors were reported. Mixed-load p99 was approximately 10.3 ms for Minimal Bun
versus 9.9 ms for PR #3; the throughput improvement is not a win on every metric.

The mixed test uses the challenge's expected endpoint proportions: 100 feed reads, 100 post reads,
15 likes and two creates per 217 requests on average. It selects from all seed tokens and updates
its post IDs from live feeds. It has no think time or independent state per virtual user.
Server and load generator share the local machine; the droplet's CPU/RAM limits and large idle
connection counts are not reproduced. These are throughput comparisons, not challenge scores.

To repeat the mixed test against an already-running server, from the repository root:

```bash
wrk -t1 -c64 -d2s -s submissions/typescript-bun-tbsvttr/bench.lua http://127.0.0.1:3000
wrk -t1 -c64 -d8s --latency -s submissions/typescript-bun-tbsvttr/bench.lua http://127.0.0.1:3000
```

The original runs loaded the same ordered tokens from a newline-separated temporary file;
`bench.lua` reads `seed/tokens.json` directly for easier reproduction. For individual endpoint
tests, omit `-s` and use `/feed` or `/posts/500000`. The optional benchmark tool `wrk` is not an
application dependency.

## License

MIT, under the repository's [license](../../LICENSE).
