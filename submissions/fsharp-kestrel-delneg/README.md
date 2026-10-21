# F# on raw Kestrel (fsharp-kestrel-delneg)

| Item | Value |
|---|---|
| Language | F# 10 on .NET 10 (SDK 10.0.401, pinned in `global.json`) |
| Server | Kestrel (ASP.NET Core 10) with its own epoll transport. The transport calls the HTTP handler directly. No web framework. |
| Database driver | SQLite 3.53.4 from source with a profile-guided gcc build, called through `DllImport` (`Sqlite.fs`) |
| JSON | `System.Text.Json` (`Utf8JsonWriter`, `Utf8JsonReader`) |
| Build | ReadyToRun with TieredPGO. Workstation GC, not concurrent. |
| **Nginx or direct** | **Direct**: the server listens on `HOST:PORT` (`0.0.0.0:80`) itself |

## How it works

1. Kestrel uses our transport (`Transport.fs`) on Linux x86-64. One thread waits on an epoll set. When a socket has data, the thread reads it with one `recv` call into the receive buffer of the thread. When one `epoll_wait` returns fewer than 32 events, the thread sleeps 2 ms before the next wait. Thus one wake-up serves more sockets. On other platforms, Kestrel uses its socket transport, and `Conn.run` reads the pipes of the connection.
2. The thread gives the bytes to the handler (`Conn.direct`). Kestrel's HTTP layer does not run. The connections have no pipes and no send task. A connection keeps a buffer only for a request that is not complete, or for bytes that the socket did not take.
3. The handler parses HTTP/1.1 itself. It finds the method, the path and three headers: `Authorization`, `Content-Length` and `Connection`. Routes are in code.
4. Each endpoint (`Api.fs`) writes the full response into one buffer of the thread: the status line, the headers and the JSON body. Then the thread sends all the responses for that read with one `send` call.
5. One SQLite connection serves all requests. With the epoll transport, one thread runs all requests, so it needs no lock. With the socket transport, a lock serializes the requests. The server prepares each statement one time at start.
6. The JWT check (`Auth.fs`) runs on each request. It decodes the header and the payload, computes HMAC-SHA256 over the token bytes and compares the signature in fixed time. Then it checks `sub`, `username` and `exp`.

## Rules

- Writes: WAL with `synchronous=NORMAL`. Each write is an autocommit statement. The statement commits before the server sends the response.
- No cache: each request runs its SQL. The server keeps no copy of rows, responses or JWT results.
- `install.sh` (root) installs gcc, libicu and the .NET SDK from the official tarball with a SHA-512 check.
- `build.sh` (normal user) gets the SQLite source with a SHA-256 check and compiles it with gcc two times. Between the two compiles, `sqlite-train.c` runs the server's statements on a synthetic database, and gcc records a profile. Then `build.sh` publishes the server.

## Optimizations

| Change | Effect on the local machine | Idea from |
|---|---|---|
| Own HTTP/1.1 parser in a Kestrel connection handler. One response write, one flush for each read. | CPU per request -9.6%. RSS 748 MB to 490 MB at 64,000 users. | #74 (own parser, one response buffer), #27 (raw Kestrel, no `HttpContext`) |
| `locking_mode=EXCLUSIVE`. SQLite takes the file lock one time, not around each statement. | CPU per request -8.4%. Hold p99 rose from 2 ms to 10 ms (checkpoints on the request thread). | #77, #27, #74 |
| Own epoll transport for Kestrel. One `recv` and one `send` for each request. Kestrel's socket transport did one more `recv` after each request. | CPU per request -4.5%. RSS 497 MB to 376 MB. | #74 (own epoll loop) |
| Profile-guided SQLite build. A perf profile showed SQLite at 36% of the server CPU. | CPU per request -4.5% | #60 |
| The epoll thread calls the HTTP handler directly on its receive buffer. No pipes and no send task for each connection. | CPU per request -7.9%. RSS 376 MB to 263 MB. | #74 (own loop calls the parser) |
| The loop finds a connection by its fd in an array, not in a dictionary. The database calls do not take a lock when the loop thread runs every request. | CPU per request -1.8%. RSS +36 MB, from the GC timing during the ramp. | perf profile |
| The loop sleeps 1 ms after an `epoll_wait` with fewer than 32 events. A perf profile showed about 7 µs of kernel time for each wake-up of the thread. | CPU per request -9.9%. Hold p95 0.3 ms to 1.8 ms. | perf profile (interrupt coalescing) |
| The sleep is 2 ms, not 1 ms | CPU per request -3.0%. Hold p95 1.9 ms to 3.6 ms. | — |
| SQLite from source with `THREADSAFE=2` and SQLite's recommended options | No mutex calls in SQLite | #77, #108 |
| `mmap_size` 1 GB, `cache_size` 32 MB | Reads use memory, not `read` calls | #108 |
| JSON from the SQLite UTF-8 bytes, no .NET strings for rows | Less allocation | #77 |
| HMAC on the token bytes with a reused `IncrementalHash` for each thread | No allocation for each request | #108 |
| Inline socket completions, inline scheduling in Kestrel | No thread switch for each request | #77, #108 |
| Workstation GC, not concurrent. ReadyToRun with TieredPGO. | Less CPU and memory on one core | #108 |

Changes that did not help (discarded):

| Change | Result | Idea from |
|---|---|---|
| `WaitForDataBeforeAllocatingBuffer=false` | Same CPU, RSS +237 MB | #77 |
| Own JSON writer | Same CPU | #74 |
| Native AOT | CPU +2%, RSS -93 MB | #77, #74 |
| `DOTNET_ReadyToRun=0` (the JIT compiles all code with dynamic PGO) | Same CPU | TechEmpower ASP.NET settings |
| `DOTNET_EnableWriteXorExecute=0` | Same CPU | TechEmpower ASP.NET settings |
| `DOTNET_GCgen0size` 64 MB (fewer GCs) | Same CPU, RSS +36 MB | perf profile |
| SQLite with `-O3 -march=native` | Same CPU | #60 |
| `mmap_size=0`: pages from the SQLite page cache, not from `mmap` | CPU -0.7% (less than the noise), RSS -24 MB | — |
| `wal_autocheckpoint=200` | Same CPU, hold p99 6 ms | — |
| Checkpoints on a second connection in a background thread, normal locking | CPU +8%, hold p99 0.9 ms | #108 |
| `wal_autocheckpoint=8000` | CPU +4%, hold p99 72 ms | — |

## Local results (diagnostics, not an official score)

These numbers come from one local machine, not from the official box. Use them to compare versions on that machine only.

- Machine: WSL2 on a Ryzen 9 7900X3D.
- Server: Docker `ubuntu:24.04` with `--cpus=1 --memory=2g --memory-swap=2g`, one core, `nofile` 65535. It serves direct.
- Load: kload (#87) on other cores, with the request mix of `bench/load.js`.
- Run: 64,000 users, 60 s ramp, 120 s hold.

| Version | CPU per request | CPU in the hold | Hold p99 | Failed | Peak RSS |
|---|---|---|---|---|---|
| First version (Kestrel HTTP layer) | 68.09 µs | 46.1% | 3.7 ms | 0 | 748 MB |
| Own parser | 61.56 µs | 41.7% | 1.9 ms | 0 | 490 MB |
| `locking_mode=EXCLUSIVE` | 56.37 µs | 38.2% | 9.7 ms | 0 | 496 MB |
| Own epoll transport | 53.55 µs (see note) | 37.3% | 10.3 ms | 0 | 376 MB |
| Profile-guided SQLite | 51.14 µs (see note) | 34.5% | 9.2 ms | 0 | 376 MB |
| Handler called from the epoll thread | 47.18 µs (see note) | 31.4% | 8.4 ms | 0 | 263 MB |
| Connection array, no database lock | 46.32 µs (see note) | 31.2% | 7.8 to 9.0 ms | 0 | 301 MB |
| 1 ms wake-up coalescing | 41.73 µs (see note) | 28.6% | 10.0 to 10.8 ms | 0 | 297 MB |
| 2 ms wake-up coalescing | 40.50 µs (see note) | 28.0% | 12.4 to 23.2 ms | 0 | 297 MB |

Note: the machine became slower after a restart. From the epoll transport on, each new version and the version before it ran in turns, two times each. The table shows the earlier value minus the mean difference of the two pairs. The optimizations table gives each difference as a percentage.

`test/test.sh`: 42/42.
