# C# + ASP.NET Core, Native AOT

| | |
|---|---|
| Language | C# 14 on .NET 10.0.12 (SDK 10.0.401, official tarball), compiled with Native AOT |
| Framework | ASP.NET Core 10 / Kestrel: `CreateEmptyBuilder` + `UseKestrelCore` and one request delegate (no configuration, routing or middleware) |
| SQLite | SQLite 3.53.4 compiled from the official source by `build.sh` and linked into the binary, called through hand-written `[LibraryImport]` bindings |
| JSON | System.Text.Json (`Utf8JsonWriter`, `Utf8JsonReader`, `JsonDocument`), part of the framework |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` (`0.0.0.0:80`) itself |

## For the benchmark run

- **Install** (`install.sh`, as root): apt `ca-certificates curl clang zlib1g-dev`, then the .NET SDK 10.0.401 tarball, checked against its SHA-512. It supports x86_64 and arm64.
- **Build** (`build.sh`, as a normal user):
  - Downloads SQLite's source from sqlite.org, checks its SHA-256, and compiles it with clang.
  - Runs `dotnet publish` (Native AOT) into `out/`. This fetches the SDK's own AOT compiler package from nuget.org, at the version the SDK pins. There are no third-party NuGet packages.
  - Takes about 7 minutes on 1 vCPU, with a peak of about 700 MB RAM.
- **Start** (`start.sh`): runs `out/Server` in the foreground, configured only by `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`. There's no JIT warm-up, so `/health` answers within a second.
- **The database is held exclusively.** The server opens `$SQLITE_PATH` with `PRAGMA locking_mode=EXCLUSIVE`, so no other process, the `sqlite3` CLI included, can open it while the server runs. Stop the server first to inspect the database. The WAL index lives in memory, so there is no `-shm` file.
- **Memory**: about 20 MB idle. Measured with 64,000 idle keep-alive connections open: 814 MiB.
- **No logs.** No logging providers are loaded. Errors return the spec's 500 body.

## How it meets the rules

| Rule | How |
|---|---|
| 2. SQLite, in-process | SQLite is linked into the binary and runs on one connection to `$SQLITE_PATH`. Nothing else runs. |
| 3. Fixed schema | No DDL. It only runs `SELECT` and `INSERT` against the schema as given. |
| 5. No caching | Every request runs its SQL while it is served. Prepared statements are reused, which caches the plan, not the data. Every JWT's signature and `exp` are verified on every request. |
| 6. Durable writes | `journal_mode=WAL`, `synchronous=NORMAL`, autocommit. A `201` is sent only after its statement has stepped to `SQLITE_DONE`, which commits it. There's no group commit. |
| 7. Nothing benchmark-specific | Responses are built from query results. The only constant bodies are the spec's error messages. |
| 10. Built on the box, pinned | SDK tarball by SHA-512, SQLite source by SHA-256, `global.json` pins the SDK exactly. |
| 11. In-process settings only | GC mode, the socket engine's inline completions, and the process's own open-files soft limit, all set by the server itself (see below). |

`test/run.sh` passes 42/42. The full install, build and test cycle also passed in clean Ubuntu 24.04 containers, on x86_64 (emulated) and on arm64 limited to 1 CPU, 2 GB and no swap.

## Optimizations, and why

- **Native AOT.** There's no JIT on the box, startup is instant, and memory stays low. Memory is the scarce resource with tens of thousands of open connections on 2 GB.
- **Direct instead of Nginx.** On one core, Nginx's CPU comes straight out of the app's budget.
  - On one pinned core this build served 71.8k req/s direct, against 43.9k behind Nginx.
- **No thread hops.** Each request runs start to finish on .NET's socket thread (one per core, so one on the droplet).
  - `UnsafePreferInlineScheduling` keeps Kestrel's own continuations inline.
  - .NET still hands every socket completion from its epoll thread to a thread-pool worker unless `DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS=1` is set. `Program.cs` sets it for its own process before the first socket exists.
  - Measured with `perf` on one core, this cut context switches per request from 1.23 to 0.007, and CPU per request from 16.8 to 12.9 µs. Throughput rose from about 53.6k to 71.8k req/s.
  - Running requests on that thread works because SQLite calls are short: every query is an index lookup that takes microseconds. The exception is SQLite's automatic WAL checkpoint, which fsyncs and briefly stalls the thread about once per thousand pages written.
- **No zero-byte reads** (`WaitForDataBeforeAllocatingBuffer = false`). Kestrel's default first waits for data with an empty read, which costs an extra syscall per request. Turning it off costs about 4 KB per idle connection: 15,000 connections went from 206 to 275 MB.
- **Open-files soft limit raised to the hard limit at startup.** The JIT runtime does this itself, but Native AOT doesn't. Under a shell's default soft limit of 1024, the server would stop accepting connections at about a thousand users. Under the spec's `LimitNOFILE=65535` it changes nothing.
- **No framework pipeline.**
  - No configuration sources, file watchers or logging providers are loaded.
  - One `RequestDelegate` routes on the path itself. There's no endpoint routing, model binding or reflection.
  - Read requests complete synchronously, so only a request whose flush or body read has to wait creates an async state machine.
- **SQLite built for this server, called directly.**
  - `build.sh` compiles SQLite with [SQLite's recommended compile-time options](https://www.sqlite.org/compile.html#recommended_compile_time_options), for example `THREADSAFE=0`, `DEFAULT_MEMSTATUS=0` and `OMIT_SHARED_CACHE`. `THREADSAFE=0` is safe because every statement runs under one lock.
  - SQLite is linked into the binary with `DirectPInvoke`, so calls into it are plain direct calls.
  - The bindings pass raw handles. A wrapper's `SafeHandle` would add reference counting to every call, and the feed makes about a hundred calls. The column getters are `[SuppressGCTransition]`, because they are tiny and never block.
  - Text columns come back as UTF-8 spans and go straight into the JSON writer, with no `string` per column.
  - Statements are prepared once (`SQLITE_PREPARE_PERSISTENT`) and reset after each use.
- **JSON** is written with `Utf8JsonWriter` into a buffer reused across requests, with pre-encoded property names. Each response goes into Kestrel's pipe in one write, with a `Content-Length` header rather than chunked encoding.
- **SQL**: the reference queries, plus:
  - A like is one statement, `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`. A post-existence check runs only when nothing was inserted, to tell "already liked" from "no such post".
  - Creating a post uses `RETURNING`.
- **Pragmas**:
  - `locking_mode=EXCLUSIVE`, set before WAL is first touched. With one process and one connection, SQLite takes the file lock once instead of around every statement. Measured with `perf`, this cut syscalls per request from 5.7–5.9 to 3.6, and CPU per request from 12.8–13.1 to 12.2–12.3 µs.
  - `journal_mode=WAL` and `synchronous=NORMAL` (rule 6).
  - `mmap_size` of 1 GiB, so pages are read from the OS page cache without copying. A 32 MiB page cache.
- **WAL checkpoints left automatic.** I tried a background thread running `PRAGMA wal_checkpoint(PASSIVE)`. With writes landing continuously, a passive checkpoint from a second connection never finishes, and the WAL grew to 2.6 GB in two minutes. The default checkpoint runs inside the committing connection, and the WAL stayed at about 4 MB through 5-minute runs.
- **JWT verified by hand on every request.** HS256 only, checked with `HMACSHA256.HashData` and a constant-time compare. `exp` is required. `[SkipLocalsInit]` stops the runtime zeroing the `stackalloc` buffers this uses.
- **GC**: workstation and non-concurrent, so no background GC thread competes for the one core. Globalization is invariant.
- **Bodies are trimmed with JavaScript's `trim()` whitespace set**, exactly like the reference implementations.

The measurements above come from one pinned Apple M4 Max core in Docker; the throughput figures use a closed-loop version of the `bench/load.js` request mix, so the comparisons between configurations are the point.

## On a $12 droplet

`bench/load.js` unchanged against an `s-1vcpu-2gb` droplet (Ubuntu 24.04, lon1), served directly on `:80` under `LimitNOFILE=65535`, with k6 on four droplets in the same region: 2-minute warm-up, then a 60 s ramp and a 5-minute hold.

| Submission | Users | Result | Server CPU | p95 / p99 |
|---|---|---|---|---|
| This one | 64,000 | pass | 78% | 271 / 427 ms |
| #27 (the C# entry in the community video), same setup | 32,000 | pass | 88% | 15–18 / 90–445 ms |
| #27 | 34,000 | pass | 91% | 16 / 30 ms |

## License

MIT, under the repo's [license](../../LICENSE).
