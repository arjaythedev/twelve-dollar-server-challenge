# Crystal, Rayo + SQLite, no GC

A normal [Rayo](https://github.com/faustinoaq/rayo) application: five routes,
`ctx.json` with named fields, borrowed SQLite rows with custom `to_json`,
and `Rayo::JWT` for every authenticated request. Rayo owns HTTP, routing, io_uring,
connection lifetimes and one worker per available CPU. Rayo also supplies the
SQLite adapter and commit-before-response batching.

| | |
|---|---|
| Language | Crystal 1.22.1, checksum-pinned official toolchain |
| Framework | Rayo 0.1.0, commit `c9553d94583fe2a56ab299e3788fe40dcd0e72b2` |
| SQLite | 3.54.1, checksum-pinned amalgamation, static, `-O3 -march=x86-64-v3` + PGO |
| Crypto | Rayo HS256 JWT through OpenSSL 3 libcrypto |
| Nginx or direct | **Direct**, configured by `HOST` and `PORT` |
| Needs | Linux 6.1+, io_uring permitted, libcrypto and libdeflate |
| Build | `--release --mcpu x86-64-v3 -Dgc_none -Dwithout_mt` |

## Implementation

- Prepared statements cache plans; every request reads SQLite live. No response,
  query-result or JWT verification cache. Per-request feed scratch buffers are cleared
  before reuse.
- The feed uses the hand-rolled entry's three index scans when its newest 20 IDs
  span fewer than 256 IDs, preserving timestamp/ID ordering. A wider range uses
  the original correlated-count query. Both execute inside one transaction.
- `Rayo::SQLite::GroupCommit` shares a transaction across requests in one loop
  turn and defers responses until commit. Failed batches roll back and return 500.
  The application no longer owns deferred-handle queues or a transaction flusher.
- `app.on_worker_start` opens SQLite, prepares statements, attaches GroupCommit
  and starts the checkpoint helper after fork in each worker.
- WAL, `synchronous=NORMAL`, foreign keys, 1 GiB mmap, a 64 MiB SQLite page cache,
  and a five-second busy timeout. Like the reference, automatic checkpoints are off;
  a separate connection on a plain pthread runs PASSIVE checkpoints every 20 ms.
  Only C calls run on that helper; SQLite queries remain synchronous on the worker.
- Decoded strings use reusable scratch buffers. Unicode whitespace trimming and
  the 500-code-point limit work without constructing Strings. Usernames are
  decoded from JWT claims and escaped by Rayo when serialized.
- The ordinary request path has no managed allocations. Buffers grow to their
  high-water mark and are reused. `-Dwithout_mt` also avoids Crystal's background
  runtime monitor allocation when using `-Dgc_none`.

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`App#run` chooses one worker per CPU in the process affinity mask. No machine
settings or affinity changes are made by the production startup script.

## Validation

The repository's unmodified `test/run.sh` and `test/test.sh` pass **42/42** checks.
`validate.sh` copies those scripts under `build/validation` and links an existing
seed there, allowing validation without writing outside this submission:

```bash
SEED_DIR=/path/to/challenge/seed bash validate.sh
SEED_DIR=/path/to/challenge/seed taskset -c 9 bash check/run.sh
```

The additional checks cover 500/501 Unicode code points, Unicode whitespace,
escaped JWT usernames, committed rows visible to an independent SQLite connection,
192 concurrent pipelined writes with distinct, ordered IDs, and both feed query
plans compared with live SQLite, including timestamp ties and gaps.

## Measurements

Wave 3 sanity comparison on 2026-10-28: Ryzen 5 3600, Linux 6.6.87.2 WSL2,
inside `crdev`. Each server runs on CPU 9, with clients on CPUs 10–11. Every case
gets a fresh seed copy. The reference is the hand-rolled
`crystal-epoll-faustinoaq` entry from challenge commit
`bfedb84069e2a7d7175b70effd7d41e99c294bac`, copied into this submission's ignored
build directory and rebuilt with its own PGO script. Both entries use io_uring
and statically linked SQLite targeting x86-64-v3.

The shared `bench/feed.lua` and `bench/mix.lua` scripts run through wrk with two
threads, 256 connections, three seconds of warm-up and five seconds of measurement.
The mix is 46% feed, 46% random post, 7% like and 1% create. These are short sanity
measurements on a shared host. The final comparison is in the next section.

| Workload | Rayo req/s | Hand-rolled req/s | Rayo p99 | Hand-rolled p99 |
|---|---:|---:|---:|---:|
| Feed, 256 connections | 21,180.49 | 24,907.37 | 18.28 ms | 16.06 ms |
| Mix, 256 connections | 23,563.48 | 32,143.53 | 17.15 ms | 15.95 ms |

Neither workload reported HTTP or socket errors. Rayo feed RSS stayed at
**28,420 KiB** across warm-up and measurement; mixed RSS rose from **156,956 to
164,716 KiB**, including SQLite mappings and page cache. The earlier large latency
spikes did not appear in this short session. Throughput remains about 15% below
the reference for feed and 27% below for mix; the gap is not closed.

Earlier measurements remain in `measurements/`; the current sanity session is in
`measurements/wave3/`. Historical numbers use an older framework and are not the
current entry's score.

Reproduce from this folder inside `crdev`, after building the application:

```bash
flock /home/user/experiments/crystal-challenges/bench/.lock env \
  WORKLOADS="feed mix" DURATION=5s RESULTS=measurements/wave3 bash benchmark.sh
```

`SERVER_CPU`, `CLIENT_CPUS`, `PORT`, `SEED_DIR`, `REFERENCE_DIR`, `CONNECTIONS`,
`DURATION`, `WORKLOADS`, `RESULTS` and `SESSION_FILE` can be overridden. Connection counts are capped at 3,000. The
wrapper starts and stops only its own recorded server PID and removes its
scratch database. Raw results and the complete session are in `measurements/`.

## Final comparison (Rayo c9553d9)

Three alternating rounds, 2026-10-28, against the `crystal-epoll-faustinoaq` build.
Server pinned to one CPU, wrk on two others (`-t2 -c256 -d12s`, after warm-up),
fresh copy of the seed database per run. Medians:

| Workload | epoll req/s | Rayo req/s | Rayo vs epoll | epoll p99 | Rayo p99 |
|---|---:|---:|---:|---:|---:|
| Mixed | 29,755 | 27,653 | −7% | 11.0 ms | 11.8 ms |
| Feed | 25,103 | 23,281 | −7% | 15.1 ms | 16.4 ms |

Rayo c9553d9 prepares SQLite transaction control once and writes JSON strings
without re-validating UTF-8; on the previous revision the gap was −27% (mixed)
and −15% (feed). Earlier measurements below describe older revisions.

## Rayo gaps

The old SQLite FFI adapter, transaction flusher, lazy worker initialization and
JSON writer workaround have been removed. Custom `Post#to_json` now works for
single posts and array elements. Application Crystal source (`src/*.cr`, including
blank/comment lines) shrank from **417 to 362 lines**, including the new 31-line
checkpoint helper. Gzip and bcrypt are not needed by this API.

The remaining generic piece is background WAL checkpointing
(`src/checkpoint.cr`): Rayo exposes no checkpoint API or helper. It adds one FFI
symbol and follows the reference's 20 ms PASSIVE policy. This is a candidate to
upstream. Rayo's normal SQLite adapter remains in use for all queries and both
connections. The custom SQLite PGO build is retained; `build.sh` puts its archive
first on the linker's search path, verified by the absence of a dynamic SQLite
dependency in `ldd bin/server`.

No new Rayo bug was found. Remaining differences from the reference include the
framework's general JSON/HTTP processing and SQLite column validation, and
GroupCommit executing BEGIN/COMMIT through `exec` rather than cached statements.
Their individual performance costs have not been isolated. There is no Postgres
in this challenge, so the multicore Postgres query-profile issue does not apply.
