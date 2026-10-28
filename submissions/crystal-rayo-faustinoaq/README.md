# Crystal, Rayo + SQLite, no GC

A normal [Rayo](https://github.com/faustinoaq/rayo) application: five routes,
`ctx.json` with named fields, a SQLite row implementing Rayo's JSON writer protocol,
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
  query-result or JWT verification cache. Per-request feed fragments are cleared
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
and 192 concurrent pipelined writes with distinct, ordered IDs.

## Rayo gaps

SQLite is not a Rayo component yet. The reusable prepared-statement adapter and
commit-before-response batching are candidates to upstream. A worker initialization
hook would let applications open SQLite explicitly after fork; this application
initializes its connection and registers its flusher on the first database request.
No Rayo source changes are required.
