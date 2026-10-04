# Minimal Bun

105 lines of application TypeScript, Bun's built-in HTTP router, SQLite, and HMAC. No npm packages.
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
