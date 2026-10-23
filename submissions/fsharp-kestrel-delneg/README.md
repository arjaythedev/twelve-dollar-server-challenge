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
2. The thread gives the bytes to the handler (`Conn.direct`). Kestrel's HTTP layer does not run. Kestrel calls our connection handler one time for each new keep-alive connection. The handler gives the connection to the loop and returns, so Kestrel keeps no state for the connection. Past the keep-alive budget (the fd limit minus 1,024), a new connection is shed: it gets one request, a `Connection: close` response and the close. Kestrel never sees a shed connection: the loop keeps it from accept to close. The connections have no pipes and no send task. A connection keeps a buffer only for a request that is not complete, or for bytes that the socket did not take.
3. The handler parses HTTP/1.1 itself. It reads the method, the path and these headers: `Authorization`, `Content-Length`, `Connection` and `Expect` (it answers `100 Continue`). A request with `Transfer-Encoding` gets 400. Routes are in code.
4. Each endpoint (`Api.fs`) writes the full response into one buffer of the thread: the status line, the headers and the JSON body. Then the thread sends all the responses for that read with one `send` call.
5. One SQLite transaction holds all the writes of one wake-up of the loop (group commit). The first write opens it. After the last request of the wake-up, the loop commits it and then sends the responses. Thus each response goes out after the commit of its write. One SQLite connection serves all requests. With the epoll transport, one thread runs all requests, so it needs no lock. With the socket transport, a lock serializes the requests. The server prepares each statement one time at start.
6. The JWT check (`Auth.fs`) runs on each request. It decodes the header and the payload, computes HMAC-SHA256 over the token bytes and compares the signature in fixed time. Then it checks `sub`, `username` and `exp`.

## Rules

- Writes: WAL with `synchronous=NORMAL`. The writes of one wake-up of the loop share one transaction. The server sends a response only after the commit of that transaction. With the socket transport, each write is an autocommit statement.
- No cache: each request runs its SQL. The server keeps no copy of rows, responses or JWT results.
- `install.sh` (root) installs gcc, libicu and the .NET SDK from the official tarball with a SHA-512 check.
- `build.sh` (normal user) gets the SQLite source with a SHA-256 check and compiles it with gcc two times. Between the two compiles, `sqlite-train.c` runs the server's statements on a synthetic database, and gcc records a profile. Then `build.sh` publishes the server.
- Nothing on the box is tuned. All settings are inside the process: SQLite pragmas, GC mode, the listen backlog.

## Trade-off: slow disks

The connection holds the database lock for its whole life (`locking_mode=EXCLUSIVE`), so SQLite's automatic checkpoint runs on the loop thread, inside the commit that fills the WAL to 1,000 frames. A checkpoint calls `fsync` two times, and every request waits for it. This saves about 8% CPU against a checkpoint thread on a second connection (measured on the machine below), and it is fine when `fsync` takes a few milliseconds. On a disk where `fsync` takes 80 ms, the tail latency grows a lot (see the second table below).

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
| Two small changes together. `mmap_size=0`: SQLite reads pages from its page cache. The loop sleeps with `usleep`, and `recv`, `send` and `epoll_ctl` skip the GC transition. Each change alone was less than the noise. | CPU per request -3.6%. RSS 297 MB to 268 MB. | perf profile |
| Kestrel gives each connection to the loop and then forgets it. Kestrel's timer no longer visits 64,000 connections each second, and the GC frees Kestrel's objects for each connection. | CPU per request -1.5% (three pairs: -0.79, -0.22, -0.74 µs). RSS 273 MB to 170 MB. | perf profile (Kestrel timer at 1.7% of CPU) |
| Group commit: one transaction for the writes of one wake-up of the loop. The responses wait for the commit. | CPU per request -11.3% in pair 2. Pair 1 gave -6.33 µs, but the old version had slow disk flushes in that pair. | #74 (group commit for each epoll batch), #60 |
| `temp_store=MEMORY`. In a group-commit transaction, SQLite keeps a statement journal for each write. On disk, the journal wrote one more 4 KB page for each changed page. | CPU per request -1.9%. `pwrite` calls for each request 1.17 to 0.64 at 3,000 requests per second. | the task's list of known tricks, `strace` |
| SQLite from source with `THREADSAFE=2` and SQLite's recommended options | No mutex calls in SQLite | #77, #108 |
| `cache_size` 32 MB | The SQLite page cache is 16 times the default size (2 MB) | #108 |
| JSON from the SQLite UTF-8 bytes, no .NET strings for rows | Less allocation | #77 |
| HMAC on the token bytes with a reused `IncrementalHash` for each thread | No allocation for each request | #108 |
| Inline socket completions, inline scheduling in Kestrel (only for the socket transport on other platforms) | No thread switch for each request | #77, #108 |
| Workstation GC, not concurrent. ReadyToRun with TieredPGO. | Less CPU and memory on one core | #108 |

Changes that did not help (discarded):

| Change | Result | Idea from |
|---|---|---|
| `WaitForDataBeforeAllocatingBuffer=false` | Same CPU, RSS +237 MB | #77 |
| Own JSON writer, tried two times | Same CPU at first. Later, CPU -0.9% (less than the noise). | #74 |
| Native AOT | CPU +2%, RSS -93 MB | #77, #74 |
| Byte strings made one time, not in each call. An F# `"..."B` literal in a function makes a new array each time. Allocation for each feed request fell from 221 bytes to 4 bytes. | CPU -1.0% (less than the noise), RSS -15 MB | allocation counters |
| Own JSON writer and byte strings made one time, together | CPU -1.1% (less than the noise) | #74, allocation counters |
| `DOTNET_ReadyToRun=0` (the JIT compiles all code with dynamic PGO) | Same CPU | TechEmpower ASP.NET settings |
| `DOTNET_EnableWriteXorExecute=0` | Same CPU | TechEmpower ASP.NET settings |
| `DOTNET_GCgen0size` 64 MB (fewer GCs) | Same CPU, RSS +36 MB | perf profile |
| `usleep` in place of `Thread.Sleep`, no GC transition for `recv`, `send` and `epoll_ctl`, alone | CPU -0.9% (less than the noise). Kept later with `mmap_size=0`. | perf profile |
| SQLite with `-O3 -march=native` | Same CPU | #60 |
| `mmap_size=0` alone | CPU -0.7% (less than the noise), RSS -24 MB. Kept later with the change above. | — |
| `wal_autocheckpoint=200` | Same CPU, hold p99 6 ms | — |
| Checkpoints on a second connection in a background thread, normal locking | CPU +8%, hold p99 0.9 ms | #108 |
| `wal_autocheckpoint=8000` | CPU +4%, hold p99 72 ms | — |
| `wal_autocheckpoint=4000`, with group commit and no `mmap` | Same CPU | — |

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
| No `mmap`, `usleep`, no GC transition | 39.02 µs (see note) | 26.8% | 12.1 to 22.7 ms | 0 | 269 MB |
| Kestrel forgets the connection | 38.44 µs (see note) | 26.5% | 11.5 to 21.9 ms | 0 | 171 MB |
| Group commit for each wake-up | 33.05 µs (see note) | 23.6% | 12.5 ms | 0 | 170 MB |
| Statement journal in memory | 32.38 µs (see note) | 23.5% | 12.1 to 12.4 ms | 0 | 170 MB |

Note: the machine became slower after a restart. From the epoll transport on, each new version and the version before it ran in turns, two times each. The table shows the earlier value minus the mean difference of the two pairs. The optimizations table gives each difference as a percentage.

Final check of this version (commit `aba8cf9`) on another machine: a VM with 4 vCPUs of an Intel Core i5-11400F and 3.9 GB of RAM, server with `--cpus=1 --memory=2g --memory-swap=2g` and `nofile` 65535, direct, kload on the other 3 vCPUs. 15,000 users, 60 s ramp. The disk of this VM is slow: a 4 KB write with `O_DSYNC` takes about 80 ms.

| Database on | Hold | Requests in the hold | Failed | Hold p95 / p99 | Max (whole run) | CPU in the hold | Peak anonymous RSS |
|---|---|---|---|---|---|---|---|
| VM disk (slow `fsync`) | 5 min | 473,086 | 0 | 440 / 578 ms | 1,278 ms | 9.2% | 52 MB |
| tmpfs | 1 min | 95,264 | 0 | 2.8 / 3.3 ms | 18 ms | 8.8% | 50 MB |

## Checks

- `test/test.sh`: 42/42, also in a clean `ubuntu:24.04` container with 1 CPU, 2 GB and no swap, after `install.sh` (61 s) and `test/run.sh` (77 s).
- HTTP edge cases: 30 pipelined requests in one write, a request sent 7 bytes at a time, `Connection: close`, HTTP/1.0, a bad request line, a 20 KB header, `Transfer-Encoding: chunked`, a 501-character body, 2,000 open connections (no file descriptor leak), a client that closes in the middle of a body.
- Writes: 100 clients sent likes and new posts, and the server was killed with `SIGKILL` during the load. After the restart, all 1,379 rows that got a 201 were in the database. With the database on a full disk, the server sent no 201 for a write that did not commit: it closed those connections, and `/health` still answered 200.
