# C# / Native AOT / raw io_uring / SQLite

| | |
|---|---|
| Language | C# 14, .NET 10 (SDK 10.0.401, pinned and SHA-512 checked in `install.sh`), **Native AOT** |
| HTTP server | None. A hand-written HTTP/1.1 server on **io_uring**, driven through the raw syscalls from C# (no liburing, no C shim) |
| SQLite | 3.53.4 amalgamation, SHA-256 checked, built by `build.sh` with GCC `-O3 -march=native` and **profile-guided optimization**, linked statically |
| Driver | None. Direct `DllImport` calls into the static SQLite (`DirectPInvoke`), no Microsoft.Data.Sqlite |
| NuGet packages | None at runtime (only the ILCompiler that Native AOT needs, pinned in `packages.lock.json`) |
| Deployment | **Direct** on `HOST:PORT` (`0.0.0.0:80`), no Nginx |

`test/run.sh` passes 42/42.

## Design

**Connections beyond `LimitNOFILE=65535`.** Accepted sockets never enter the process's file descriptor table.
Each io_uring ring registers a sparse table of 65,535 *direct descriptors*, and a multishot accept with
`IORING_FILE_INDEX_ALLOC` places every new socket in it. The tables are per ring, so five rings hold about 327,000
connections under the spec's limit. That limit is what stopped the earlier C# (#74) and F# (#108) entries at about
65,000 users. Each ring has its own thread (`SINGLE_ISSUER` + `DEFER_TASKRUN` + `COOP_TASKRUN`), and all of them
accept on one shared listening socket. A ring stops accepting while fewer than 1,024 of its slots are free. It
resumes at 2,048.

**No memory per idle connection.** Receives are multishot into a provided-buffer ring (2,048 × 2 KiB per ring).
The kernel picks a buffer only when bytes arrive, and the buffer goes back as soon as they are parsed. An idle
keep-alive connection costs one 88-byte record. Output buffers come from a pool and are held only while a
response is in flight.

**Reads inline, writes on a writer thread, checkpoints on a third.** Three SQLite connections (built with
`SQLITE_THREADSAFE=2`, so each connection is used by one thread at a time):

- **The rings read.** Each loop waits for 128 completions or 4 ms. It then takes the application lock and runs
  every complete request of that batch in one read transaction (one snapshot) on a `query_only` connection.
  Feeds, single posts and health checks are 92 % of the load, and they never touch the disk.
- **Writes go to the writer thread** (new posts and likes, 8 %). The ring parses the request, verifies the JWT
  and validates the body, then queues the write. The writer runs everything queued in one `BEGIN IMMEDIATE`
  transaction and commits once (group commit). Only after that commit does it hand each response back to its
  ring, through an eventfd read the ring keeps armed. A 201 is therefore always sent after its row is committed
  (rule 6). A connection with a write in flight stops parsing until the write answers, so pipelined responses
  stay in order.
- **Checkpoints run on their own connection and thread.** They are `PASSIVE` and start once the WAL has 1,024
  frames, so they never block the reader or the writer.

Why: on a busy disk a single checkpoint fsync took up to 3.4 s, and a WAL write throttled by the kernel's
dirty-page limits took up to 0.9 s. With everything on the request path, those stalls became multi-second p99s at
60,000+ users even though the CPU was 80 % idle. With this split, the rings never wait on the disk. At 200,000
users locally, the slowest gap between two ring loops was under 5 ms.

**The feed in three range scans.** `GET /feed` is 46 % of the load and the most expensive request. The 20 newest
ids come from the `(created_at, id)` index. When they lie within 256 ids of each other, as new posts always do, one
range scan of `likes_post_id_idx` counts the likes of all 20 posts. One rowid range scan of `posts` joined to
`users` then fetches their rows, and they are written straight to the response. That replaces 20 separate count
subqueries. Any other shape falls back to the reference query. Everything is read live in the request's
transaction; nothing is cached. The idea comes from the Rust (#82), Go (#72) and C (#112) entries.

**Batching.** Each wake-up has a fixed cost: the syscall, the lock, the read transaction, and submitting the
sends. The loop therefore waits for 128 completions or 4 ms, whichever comes first. Measured at 100,000 users on an
i7-3770 pinned to one core:

| Wait | CPU per request | p99 |
|---|---:|---:|
| 250 µs / 32 completions | 55.5 µs | 1.0 ms |
| 1 ms / 64 | 41.8 µs | 3.0 ms |
| **4 ms / 128** | **35.1 µs** | **10 ms** |
| 8 ms / 256 | 34.7 µs | 20 ms |

At high load the 128 completions arrive before the 4 ms, so the wait only lengthens latency when the server has
time to spare.

**SQLite settings.**
- Pragmas: WAL with `synchronous=NORMAL`, `foreign_keys=ON`, a 1 GiB mmap.
- Statements are prepared once with `SQLITE_PREPARE_PERSISTENT`.
- Automatic checkpoints are off; the checkpoint thread runs them.
- New posts are read back by rowid instead of `RETURNING`, which builds a temporary table.
- Compile flags: `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`, and the usual `OMIT_*` options.
- PGO: trained on a synthetic database built by `pgo/train.c`, never on the seed.

**C#-specific choices.**
- Native AOT, with `IlcInstructionSet=x86-64-v3`. `native` breaks ILC on AVX-512 machines.
- Workstation non-concurrent GC. The hot path allocates nothing: connection records, buffers and SQEs are all
  native memory.
- `[SuppressGCTransition]` on the SQLite calls that take nanoseconds (bind, column, reset, step on cached pages).
  `COMMIT` keeps the GC transition.

**Auth.** The HS256 signature is verified on every request with a reused `HMACSHA256` (no verification cache).
The verifier also requires `alg` to be `HS256`, rejects expired tokens and **rejects tokens without `exp`**.

**Overload behaviour instead of a crash.** Worker 0 reads `/proc/meminfo` four times a second. Below 192 MB usable
it stops accepting, and below 96 MB it also answers with `Connection: close`. Running out of slots or memory never
aborts the process. Connections whose partial input or blocked output is older than 120 s are closed. Idle
keep-alives are left open.

## Measurements

These are local runs, not official scores. Each ran the unchanged load model with
[kload](https://github.com/3x3xX3N0N/kload), a replay of `bench/load.js`, on loopback. The server ran in a systemd
scope pinned to one core, with `MemoryMax=2G`, no swap and `nofile=65535`. kload ran on the other cores.
µs/request is the server cgroup's CPU time divided by requests.

**i7-3770 (2012, 3.4 GHz).** #74 costs 91 µs/request on this core against 46 µs on the batch-2 Synteq VPS, so this
core is roughly half as fast as the VPS.

| Users | Hold | Errors | p95 / p99 | Max | App CPU | µs/request | Peak memory |
|---:|---|---:|---|---:|---:|---:|---:|
| 50,000: #74, for comparison | 2 min | 0 | 8.6 / 44 ms | 306 ms | 38 % | 91.3 | 227 MB |
| 200,000 | 2 min | 0 | 8.5 / 10 ms | 75 ms | 53 % | 32.1 | 877 MB |
| 250,000 | **5 min** | 0 | 8.4 / 10 ms | 128 ms | 73 % | 31.3 | 1,439 MB |
| 300,000 | **5 min** | 0 | 9.0 / 12 ms | 157 ms | 83 % | 29.8 | 1,341 MB |

At 300,000 users that is 31,700 requests/s. The memory is mostly the kernel's per-socket structures, about 3.8 KB per
connection. The process itself used 80 MB at 300,000 connections. The rings stop accepting at about 322,000
connections. The memory guard stops them earlier if the box runs short of memory.

On the VPS, a core about twice as fast should carry the same load at roughly 40 % CPU. At that point the limit is
connections and memory, not CPU. This is an estimate, not a score.

## Credits

The HTTP request parsing, the API validation and error precedence, the JSON writer and the JWT verifier are adapted
from gichamba's C# entry (#74), which built on maks-march's (#27). The direct-descriptor ring layout, the range-scan
feed and the checkpoint policy follow the Rust (#82), C (#112) and Go (#72) entries. The memory guard follows the
C entry (#112).
