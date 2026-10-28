# Crystal, Rayo + SQLite, no GC

A normal [Rayo](https://github.com/faustinoaq/rayo) application: five routes,
`ctx.json` with named fields, borrowed SQLite rows exposed as NamedTuples,
and `Rayo::JWT` for every authenticated request. Rayo owns HTTP, routing, io_uring,
connection lifetimes and one worker per available CPU. The application owns SQLite.

| | |
|---|---|
| Language | Crystal 1.22.1, checksum-pinned official toolchain |
| Framework | Rayo 0.1.0, commit `e0006c8b773dae7b75d78eedab314419fb08c0e9` |
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
- Requests in a Rayo completion turn share a SQLite transaction. `ctx.defer` keeps
  each response pending; a `Rayo::Uring::Flusher` commits before finishing them.
  Commit failure replaces the entire batch's responses with 500 errors. Separate
  handle queues protect pipelined requests dispatched while finishing responses.
- WAL, `synchronous=NORMAL`, foreign keys, 1 GiB mmap, a 64 MiB SQLite page cache,
  and automatic checkpoints every 1,000 WAL pages. SQLite calls run synchronously
  on the worker; the intended box has one CPU. Multiple workers open independent
  connections after fork with a five-second busy timeout.
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

Local comparison on 2026-10-28: Ryzen 5 3600, Linux 6.6.87.2 WSL2, inside
`crdev`. Each server runs on CPU 9, with clients on CPUs 10–11. Every case gets
a fresh seed copy. The reference is the hand-rolled `crystal-epoll-faustinoaq`
entry from challenge commit `bfedb84069e2a7d7175b70effd7d41e99c294bac`, copied
into this submission's ignored build directory and rebuilt with its own PGO script.
Both entries use io_uring and SQLite targeting x86-64-v3.

`bench/feed.lua` and `bench/mix.lua` from the shared challenge bench tooling run
through wrk with two threads, 256 connections, three seconds of warm-up and a
15-second measurement. The mix is 46% feed, 46% random post, 7% like and 1% create.
The tooling's `ul12` driver also runs its `bench/load.js` user model with 1,000
users, think times scaled by 0.01, a five-second ramp and a 20-second measured window (the driver samples on
ten-second ticks).
These short, local measurements are not the challenge's five-minute user score.

| Workload | Rayo req/s | Hand-rolled req/s | Rayo p99 | Hand-rolled p99 |
|---|---:|---:|---:|---:|
| Feed, 256 connections | 19,587.54 | 23,023.53 | 646.22 ms | 16.23 ms |
| Mix, 256 connections | 28,138.19 | 34,325.12 | 654.34 ms | 10.84 ms |
| Scaled user model, 1,000 users | 10,427 | 9,679 | 16.9 ms | 245.8 ms |

Both user-model runs passed the latency thresholds with **0.000% errors**.
Neither wrk workload reported HTTP or socket errors. The Rayo runs had occasional
latency spikes up to about 0.83 seconds; the table includes them. Precise Rayo RSS
for the read-only feed stayed at **28,352 KiB** after warm-up. Mixed-load RSS went
from **163,460 to 176,540 KiB**, including SQLite's mapped pages and page cache.

Reproduce from this folder inside `crdev`, after building the application:

```bash
flock /home/user/experiments/crystal-challenges/bench/.lock bash benchmark.sh
```

`SERVER_CPU`, `CLIENT_CPUS`, `PORT`, `SEED_DIR`, `REFERENCE_DIR`, `CONNECTIONS`
`DURATION`, `WORKLOADS` and `SESSION_FILE` can be overridden. Connection counts are capped at 3,000. The
wrapper starts and stops only its own recorded server PID and removes its
scratch database. Raw results and the complete session are in `measurements/`.

## Rayo gaps

SQLite is not a Rayo component yet. The reusable prepared-statement adapter and
commit-before-response batching are candidates to upstream. A worker initialization
hook would let applications open SQLite explicitly after fork; this application
initializes its connection and registers its flusher on the first database request.
No Rayo source changes are required.

At the pinned commit, `src/rayo/json.cr:603` calls `v.to_json(self)` with a copy
of the Writer struct. Custom serializers used as array elements lose their
changes to the outer writer's separator state: two items that each serialize
`{id: 1}` produce `[{"id":1}{"id":1}]`. The minimal application workaround is to
return row fields as NamedTuples and give those to the writer directly. Both
feed paths use this form; the wider-range integration check covers the case.
