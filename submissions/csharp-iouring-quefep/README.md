# C# / Native AOT / raw io_uring / SQLite

A direct HTTP/1.1 server written in C# and compiled with Native AOT. It uses raw
io_uring syscalls, has no HTTP framework, no ORM and no reverse proxy in front of
it, and talks to SQLite in the same process.

This is a credited derivative of
[oswaldosierra's open PR #157](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/157).
The io_uring transport, the split reader/writer/checkpointer design, the feed
range path and the build scripts started there. I changed the ring count, buffer
sizes, wait policy, WAL limit, writer wake-up behaviour and response copying, and
ran the local experiments in this folder. It is not a from-scratch
implementation and should be read as a tuned derivative, not an original one.

`test/run.sh` passes 42/42.

| | |
|---|---|
| Language | C# 14, .NET SDK 10.0.401, SHA-512 checked in `install.sh` |
| Runtime | Native AOT, one Linux x64 binary |
| HTTP | Hand-written HTTP/1.1 on raw io_uring syscalls from C#; no liburing, no C shim |
| SQLite | 3.53.4 amalgamation, SHA-256 checked, built with GCC and PGO |
| Driver | Direct `DllImport` into static SQLite; no Microsoft.Data.Sqlite |
| NuGet | None at runtime |
| Deployment | Direct on `HOST:PORT`; no Nginx |

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## What I changed

- Eight io_uring rings instead of five, giving 524,280 direct-descriptor slots
  instead of 327,675. The five-ring 300k attempt had a long p95/p99 tail; eight
  rings fixed that in the local runs.
- A 32,768-entry CQ, a 512-buffer provided receive ring, a 128 KiB response arena
  and 64 pooled 16 KiB blocks per worker.
- A 16 MiB `journal_size_limit` on the reader, writer and checkpointer. The
  earlier setting was 32 MiB; the smaller cap helped the 320k tail locally.
- Response bytes are copied into per-connection output buffers after the shared
  `App.Gate` is released. The SQLite batch still commits before anything is sent.
- A deferred write switches its ring to `min_complete=1` only while at most
  300k connections are open. Above that, the ring keeps the 128-completion / 4 ms
  wait. The writer also waits 1 ms to group commits.
- Listener `SO_RCVBUF=1 KiB` and `SO_SNDBUF=4 KiB`, inherited by accepted sockets.
- Native AOT `StackTraceSupport=false`, `IlcMaxVectorTBitWidth=256`, and
  `x86-64-v3` so the build does not use AVX-512-specific code generation.

These are heuristics from local testing, not universal wins. Several of them were
kept because they helped the best local run; the run-to-run variance is large
enough that the exact p99 numbers below should not be read as guarantees.

## How it works

Accepted sockets never go into the process file-descriptor table. Each ring
registers a sparse table of 65,535 direct descriptors and uses a multishot accept
with `IORING_FILE_INDEX_ALLOC`. Eight tables give the server roughly 524,000
keep-alive slots without raising `LimitNOFILE`. Every ring has its own issuer
thread; all rings accept on the same listening socket.

Receives are multishot into a provided-buffer ring. The kernel picks a buffer
when data arrives and the buffer is returned after parsing. An idle connection
keeps one small connection record and no receive buffer. Output buffers are
pooled and only held while a response is in flight.

SQLite is built with `SQLITE_THREADSAFE=2`. The ring threads share a
`query_only` connection and take a read transaction for each completion batch
under `App.Gate`. Writes are checked on the ring thread, then queued to one
writer thread. The writer runs a `BEGIN IMMEDIATE` for the queued jobs and
commits once; a 201 is sent only after that commit. A third connection and thread
runs passive checkpoints so checkpoint I/O does not block the rings.

The feed query first reads the 20 newest ids from the `(created_at, id)` index.
When their ids fit in a small window, one range scan over `likes_post_id_idx`
counts the likes and one rowid range scan fetches the posts and authors.
Otherwise it falls back to the reference query. Every value is read inside the
request transaction; nothing is cached across requests.

The HTTP parser handles keep-alive, pipelining, partial requests,
`Connection: close` and `Expect: 100-continue`. JWT is HS256 and is verified on
every authenticated request; tokens without `exp` are rejected. JSON is written
into reused buffers in the required order. Chunked request bodies are refused;
the 42-check suite does not exercise chunked request bodies.

## Local runs

The server ran in a systemd scope with `MemoryMax=1967M`,
`MemorySwapMax=0` and `CPUQuota=100%`, pinned to one core. Four C epoll
generators ran outside the scope on four physical cores, with a 60 s ramp and a
300 s hold. The memory figure is cgroup `memory.peak`; CPU is cgroup
`usage_usec` over the 360 s run. This is a local screen, not k6 and not the
official Synteq run.

| Entry | Users | req/s | p95 | p99 | failed | app CPU | cgroup peak |
|---|---:|---:|---:|---:|---:|---:|---:|
| C# 310k, best run | 310,000 | 32,799 | 13.5 ms | 25.3 ms | 0 | 89.9 % | 1,896.0 MB |
| C# 310k, repeat | 310,000 | 32,783 | 24.5 ms | 86.5 ms | 0 | 90.0 % | 1,883.2 MB |
| C# 320k, best run | 320,000 | 33,850 | 20.0 ms | 40.8 ms | 0 | 90.4 % | 1,956.7 MB |
| C# 320k, repeat | 320,000 | 33,822 | 36.0 ms | 103.1 ms | 0 | 90.0 % | 1,953.9 MB |
| Rust #82 310k | 310,000 | ~32,800 | 9.8-11.3 ms | 16.7-24.4 ms | 0 | 82.4-84.9 % | 1,898-1,900 MB |

`p99` here is the worst of the four generators' p99 values, not a global p99
over all requests. The official k6 run reports a global number.

Two honest observations:

- Rust #82 is faster per request in these runs. It uses less CPU and has a
  noticeably better p99 at the same 310k user count.
- The C# entry can hold 320,000 users cleanly in this local setup, while the
  Rust run at 320,000 was OOM-killed. That is a capacity result for this box, not
  a claim that C# is faster per request.

The 320k C# runs peak at 1,954-1,957 MB, roughly 10-13 MB below the 1,967 MB
limit. The 310k runs peak at 1,883-1,896 MB. I would treat 310k as the safer
target if the official Xeon is slower than the local Ryzen; 320k is the local
maximum and sits close to the memory edge.

The best local C# runs exceed the published batch-2 Rust score of 264,000
users, but no official k6/Synteq run has been done for this folder. The official
result is the only score that matters.

## Rules and caveats

- `test/run.sh`: 42/42.
- SQLite stays in-process; the schema and seed are untouched; WAL and
  `synchronous=NORMAL` are used; every 201 waits for its commit.
- No response cache, query-result cache, in-memory table copy or remembered JWT
  verification.
- The SDK and SQLite source are pinned and checksummed; the binary is built on
  the box. `start.sh` reads only `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`.
- The server asks for no kernel tuning, CPU pinning, or external service.
- Chunked request bodies are refused.
- `sqlite3_step` and `sqlite3_reset` use `SuppressGCTransition`. This is a
  documented .NET risk; it has worked in testing but should be reviewed by
  anyone reusing the code.
- `MemGuard` reads `/proc/meminfo`. The local cgroup reports the host's memory,
  which is not the same as the official box's memory view.

## Credit

The io_uring transport, writer/checkpointer split, feed range path and build are
from [oswaldosierra's PR #157](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/157).
The code also carries ideas and comments from gichamba's C# entry (#74) and
maks-march's C# entry (#27). This folder is a credited derivative; it is not a
100% copy and it is not an original implementation.

## License

MIT, under the repository's [license](../../LICENSE).
