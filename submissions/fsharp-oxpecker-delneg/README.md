# F# + Oxpecker

| | |
|---|---|
| Language | F# 10 (SDK 10.0.401, official tarball), FSharp.Core 10.0.100, on .NET 10.0.12 |
| Framework | [Oxpecker](https://github.com/Lanayx/Oxpecker) 3.0.0-beta-3 on ASP.NET Core 10.0.12 / Kestrel. Source from the fork [delneg/Oxpecker](https://github.com/delneg/Oxpecker) (see below) |
| SQLite | SQLite 3.53.4, compiled from the official source by `build.sh`, called through F# `DllImport` bindings with raw handles |
| JSON | System.Text.Json (`Utf8JsonWriter`, `Utf8JsonReader`), part of the framework |
| **Nginx or direct** | **Direct**: the server listens on `HOST:PORT` (`0.0.0.0:80`) itself |

## Oxpecker version

`build.sh` fetches Oxpecker at a pinned commit, `ef46b35db7b00a02075435861bd31494265b0437` of [delneg/Oxpecker](https://github.com/delneg/Oxpecker), branch `autoresearch/perf-2026-10-14`. That branch is upstream `develop` at `369cda2` plus allocation fixes in routing, response writing and model binding. `build.sh` removes one commit of that branch, `c911ea1` (a fast path in `>=>`), because it was slower on x86-64. The server builds Oxpecker from this source as a project reference.

Routing goes through Oxpecker: `GET [ route ... ]` and `POST [ route ... ]` on ASP.NET Core endpoint routing. The handlers are Oxpecker `EndpointHandler`s (`HttpContext -> Task`).

## For the benchmark run

- **Install** (`install.sh`, as root): apt `ca-certificates curl gcc libc6-dev git libicu74`, then the .NET SDK 10.0.401 tarball, checked against its SHA-512. It supports x86-64 and arm64.
- **Build** (`build.sh`, as a normal user):
  - Downloads the SQLite source from sqlite.org, checks its SHA-256, and compiles `libsqlite3.so` with gcc.
  - Fetches Oxpecker at the pinned commit with git and removes commit `c911ea1`.
  - Runs `dotnet publish` (ReadyToRun) into `out/`. The project pins its NuGet packages: FSharp.Core 10.0.100, plus TypeShape 10.0.0 and Microsoft.IO.RecyclableMemoryStream 3.0.1 from Oxpecker.
- **Start** (`start.sh`): runs `out/Server` in the foreground. It reads only `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`. `/health` answers in less than a second.
- **No logs.** The server loads no logging providers. Errors return the spec's 500 body.

## How it works

- **One request thread.** Kestrel runs each request start to finish on .NET's socket thread. All SQL of a request runs on one SQLite connection under one lock.
- **Writes are autocommit.** A like or a new post is one statement. It commits before the response goes out. `synchronous=NORMAL` in WAL mode makes the commit durable without an fsync per commit.
- **A checkpoint thread.** A second connection on a background thread copies the WAL into the database file (`PASSIVE` checkpoints, every 100 ms after writes). The fsyncs that a checkpoint needs happen on this thread, not on the request thread. Auto-checkpoints are off on the request connection.
- **The WAL stays bounded.** SQLite restarts the WAL at a write only when every frame is already copied. Under steady writes that moment may not come. At 65,536 frames (256 MB), the checkpoint thread copies the last frames while it holds a write semaphore. Reads go on during that time. A write that comes then waits without blocking the thread.

## How it meets the rules

| Rule | How |
|---|---|
| 2. SQLite, in-process | SQLite is a shared library in `out/`, loaded into the server process. Two connections open `$SQLITE_PATH`. Nothing else runs. |
| 3. Fixed schema | No DDL. The server runs only `SELECT` and `INSERT` on the schema as given. |
| 5. No caching | Every request runs its SQL while the server serves it. The server reuses prepared statements. They keep the query plan, not the data. The server checks the signature and `exp` of every JWT on every request. |
| 6. Durable writes | `journal_mode=WAL`, `synchronous=NORMAL`, autocommit. A `201` goes out only after its statement steps to `SQLITE_DONE`, which commits it. There is no group commit. |
| 7. Nothing benchmark-specific | Responses come from query results. The only constant bodies are the spec's error messages. |
| 10. Built on the box, pinned | SDK tarball by SHA-512, SQLite source by SHA-256, Oxpecker by commit hash, NuGet packages by version, `global.json` pins the SDK. |
| 11. In-process settings only | GC mode, inline socket completions and Kestrel options, all set by the server itself. |

## Optimizations, and why

The numbers come from 26 measured experiments in two rounds, with one change in each. A change stayed only if it was better than the noise. A percentage compares two builds on the same machine at a fixed request rate.

- **Direct instead of Nginx.** On one core, Nginx's CPU comes out of the server's budget.
- **Inline socket completions** (`DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS=1`, set by the server at startup) and Kestrel's `UnsafePreferInlineScheduling`. A request runs on the socket thread with no hand-off to a thread-pool worker.
- **Kestrel's zero-byte read stays on** (the default). An idle connection then keeps no buffer. On arm64, removing the zero-byte read cut CPU by about 5%. On x86-64 it saved no CPU, and at 64,000 users it used 250 MB more memory. A later commit put the default back.
- **`route "/posts/{id}"` with the route value, not `routef`.** Oxpecker's `routef` calls the handler through reflection and makes closures for each request. This change cut CPU for each request by another 5%.
- **SQLite built for this server.** SQLite's recommended compile-time options, such as `DEFAULT_MEMSTATUS=0` and `OMIT_SHARED_CACHE`. `THREADSAFE=2`: one thread at a time uses each connection. `mmap_size` 1 GB, 32 MB page cache.
- **Raw bindings.** The bindings pass raw handles. The column getters skip the GC transition. Text columns go from SQLite's UTF-8 bytes into the JSON writer with no `string` for each column.
- **One JSON buffer.** `Utf8JsonWriter` writes into one buffer under the lock, with pre-encoded property names. Each response goes into Kestrel's pipe in one write, with a `Content-Length` header.
- **Checkpoints off the request thread.** On a disk with slow fsync (about 67 ms), checkpoints on the request thread stopped all requests. With the checkpoint thread, p95 went from 500 ms to 0.21 ms at the same load.
- **JWT by hand.** HS256 only, HMAC-SHA256 and a constant-time compare, on every request. A token without `exp` fails. Each thread makes one keyed HMAC context and uses it again for each token. Each token is still hashed in full. This cut CPU for each like by about 5%.
- **GC**: workstation, not concurrent, so no background GC thread uses the one core. Globalization is invariant.
- **Trim with JavaScript's whitespace set**, as the reference implementations do.

These did not help, and are not in the code:

| Change | Result |
|---|---|
| Native AOT | arm64: same CPU as JIT with ReadyToRun and tiered PGO. x86-64: 4% more CPU. It saves only 53-55 MB. It links with gcc, so it does not need clang. |
| A writer thread with group commit | Reads stopped waiting for the slow disk, but each write cost twice the CPU (thread hand-offs on one core). A like costs about 45 us more than a read, and writes are about 8% of the requests. So group commit can save 2% of the CPU at most. |
| `/feed` JSON built by SQLite (`json_group_array`) | First round: 31% more CPU for each `/feed` request. Second round: 10% more CPU on the mixed load. |
| SQLite at `-O3` | No change, in both rounds. |
| GC gen0 budget of 8, 32 or 64 MB | No CPU change. At 64,000 users, a smaller budget gave more GCs with the same pause each, so p99 got worse. |
| No ReadyToRun (JIT and tiered PGO only) | No change after warm-up. |
| `EventSourceSupport` and `MetricsSupport` off | No change. |
| An inlined `Monitor` lock instead of F#'s `lock` | No change. F#'s `lock` is already inline. |

## Local measurements

These numbers are local diagnostics, not an official score.

### Second round: WSL2 VM on a Windows desktop

- **Machine**: a WSL2 VM on a Windows desktop with an AMD Ryzen 9 7900X3D (12 cores, 24 threads) and 30 GB of RAM. Ubuntu 26.04, kernel 6.18 (WSL2). The disk does an fsync in about 2 ms. Another job used CPUs 0-11 at the same time.
- **Server**: a Docker container with `ubuntu:24.04` and this `install.sh`. It has 1 CPU (cpuset on one core, with its second thread left idle), 2 GB of memory, no swap, and `nofile` 65535. It serves direct on port 8080.
- **Load**: kload on CPUs 14-23 over loopback. 1,000 users for 60 s of warm-up, then a 60 s ramp and a 5-minute hold.
- **CPU note**: system calls cost more in WSL2. Each request used about twice the CPU of the first-round VM.

| Users | Hold | Result | p95 / p99 (whole run) | p95 / p99 (hold) | Errors | Server CPU | Requests/s | Peak RSS |
|---|---|---|---|---|---|---|---|---|
| **64,000** | **5 min** | **pass** | **3.5 / 68 ms** | **3.3 / 23 ms** | **0 of 2.03M** | **60%** | **6,771** | **785 MB** |

Before the second round, the same machine gave peak RSS 986 MB at 64,000 users (2-minute hold, p99 74-106 ms). The cgroup memory peak in the final hold was 1,312 MB, which includes the page cache. The WAL file grew to 271 MB.

The p99 at 64,000 users comes from GC pauses. With 64,000 open connections, the heap is about 630 MB, and each gen0 GC stops the server for 100-190 ms. The GC runs about once every 15 seconds.

`test/run.sh` passes 42/42 in a clean `ubuntu:24.04` container with 1 CPU, 2 GB and no swap, after `install.sh`, on this machine. There, `install.sh` took 22 s and `test/run.sh` took 45 s.

### First round: x86-64 VM with a slow disk

These numbers are for commit `ce63f0e`, before the second round.

- **Machine**: a VM with 4 vCPUs of an Intel Core i5-11400F and 3.9 GB of RAM, Ubuntu 26.04. Its disk has slow fsync (about 67 ms).
- **Server**: a Docker container with `ubuntu:24.04` and this `install.sh`. It has 1 CPU (cpuset), 2 GB of memory, no swap, and `nofile` 65535. It serves direct on port 8080.
- **Load**: [kload](https://github.com/3x3xX3N0N/kload) (the low-memory k6-compatible generator from PR #87), which replays `bench/load.js` with the same schedule and thresholds. It runs on the other 3 CPUs over loopback. k6 at these user counts needs more memory than the VM has.
- **Schedule**: 1,000 users for 60 s of warm-up, then a 60 s ramp and a hold.

| Users | Hold | Result | p95 / p99 | Errors | Server CPU | Requests/s | Peak RSS |
|---|---|---|---|---|---|---|---|
| 10,000 | 2 min | pass | 0.7 / 1.1 ms | 0 | 12% | 1,058 | 180 MB |
| 30,000 | 2 min | pass | 0.3 / 12 ms | 0 | 16% | 3,175 | 490 MB |
| 50,000 | 2 min | pass | 0.3 / 49 ms | 0 | 23% | 5,292 | 823 MB |
| 62,000 | 2 min | pass | 0.3 / 55 ms | 0 | 27% | 6,560 | 979 MB |
| **64,000** | **5 min** | **pass** | **0.3 / 56 ms** | **0 of 2.03M** | **28%** | **6,774** | **1,170 MB** |

`test/run.sh` passes 42/42 in a clean `ubuntu:24.04` container with 1 CPU, 2 GB and no swap, after `install.sh`. There, `install.sh` took 87 s and `test/run.sh` (build and tests) took 50 s.

Writes are slower than reads on this disk. In the 64,000-user hold, likes had p95 2.0 ms and p99 733 ms. The cgroup memory peak was 1,686 MB, which includes the page cache of the 273 MB WAL file.

## License

MIT, under the repo's [license](../../LICENSE).
