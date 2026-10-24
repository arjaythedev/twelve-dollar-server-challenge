# F# on raw Kestrel (fsharp-kestrel-delneg)

| Item | Value |
|---|---|
| Language | F# 10 on .NET 10 (SDK 10.0.401, pinned in `global.json`) |
| Server | Kestrel (ASP.NET Core 10) with its own io_uring transport (its own epoll transport when io_uring is not allowed). The transport calls the HTTP handler directly. No web framework. |
| Database driver | SQLite 3.53.4 from source with a profile-guided gcc build, called through `DllImport` (`Sqlite.fs`) |
| JSON | Own writer (`Api.fs`); `System.Text.Json`'s `Utf8JsonReader` for request bodies |
| Build | ReadyToRun with TieredPGO. Workstation GC, not concurrent. |
| **Nginx or direct** | **Direct**: the server listens on `HOST:PORT` (`0.0.0.0:80`) itself |

## How it works

1. Kestrel uses our io_uring transport (`Uring.fs`) on Linux x86-64. One thread owns the rings and calls `io_uring_enter` through `syscall`; there is no liburing. Accept is one multishot accept that puts each new socket into the ring's table of fixed files (`IORING_FILE_INDEX_ALLOC`), not into the process fd table, so `LimitNOFILE=65535` does not cap the connections: a ring has as many slots as the fd limit (at most 65,535), and the thread opens up to 8 rings. Accept moves to the next ring when a ring has fewer than 4,096 free slots (a full table makes the kernel drop the socket it just accepted). Each connection has one multishot receive into a ring of 2,048 provided 2 KB buffers. If the kernel refuses the buffer ring (`IORING_REGISTER_PBUF_RING`, kernel 5.19+), the ring hands the same buffers to the kernel with `IORING_OP_PROVIDE_BUFFERS` instead, one request per run of freed buffers. The thread waits for 32 completions or 2 ms, handles every completion of that wait, commits, then queues one send per connection with `MSG_DONTWAIT` (it completes inside the next `io_uring_enter`). A send the socket does not take whole goes on from a buffer of the connection. A close is a `SHUTDOWN` linked to the `CLOSE` of the fixed file (on kernel 6.8 the `CLOSE` alone sends the FIN late). The rings use `SINGLE_ISSUER`, `DEFER_TASKRUN` and `COOP_TASKRUN`. If the kernel refuses any of it (old kernel, seccomp), the server uses the epoll transport below.
   The epoll transport (`Transport.fs`): one thread waits on an epoll set. When a socket has data, the thread reads it with one `recv` call into the receive buffer of the thread. When one `epoll_wait` returns fewer than 32 events, the thread sleeps 2 ms before the next wait. Thus one wake-up serves more sockets. On other platforms, Kestrel uses its socket transport, and `Conn.run` reads the pipes of the connection.
2. The thread gives the bytes to the handler (`Conn.direct`). Kestrel's HTTP layer does not run. Kestrel calls our connection handler one time for each new keep-alive connection. The handler gives the connection to the loop and returns, so Kestrel keeps no state for the connection. With epoll only: past the keep-alive budget (the fd limit minus 1,024), a new connection is shed: it gets one request, a `Connection: close` response and the close. Kestrel never sees a shed connection: the loop keeps it from accept to close. The connections have no pipes and no send task. A connection keeps a buffer only for a request that is not complete, or for bytes that the socket did not take.
3. The handler parses HTTP/1.1 itself. It reads the method, the path and these headers: `Authorization`, `Content-Length`, `Connection` and `Expect` (it answers `100 Continue`). A request with `Transfer-Encoding` gets 400. Routes are in code.
4. Each endpoint (`Api.fs`) writes the full response into one buffer of the thread: the status line, the headers and the JSON body. The JSON writer copies the keys as fixed bytes and the column values as SQLite's UTF-8 bytes. It escapes only what JSON requires (`"`, `\` and control characters, found with one vectorized search). Then the thread sends the responses of each connection with one send.
5. One SQLite transaction holds all the writes of one wake-up of the loop (group commit). With io_uring it holds the reads of the pass too, so SQLite takes its WAL locks once per pass. The first write opens it. After the last request of the wake-up, the loop commits it and then sends the responses. Thus each response goes out after the commit of its write. One SQLite connection serves all requests. With the epoll transport, one thread runs all requests, so it needs no lock. With the socket transport, a lock serializes the requests. The server prepares each statement one time at start.
6. The JWT check (`Auth.fs`) runs on each request. It decodes the header and the payload, computes HMAC-SHA256 over the token bytes and compares the signature in fixed time. Then it checks `sub`, `username` and `exp`.

## Rules

- Writes: WAL with `synchronous=NORMAL` on both connections. The writes of one wake-up of the loop share one transaction. The server sends a response only after the commit of that transaction. With the socket transport, each write is an autocommit statement.
- No cache: each request runs its SQL. The server keeps no copy of rows, responses or JWT results.
- `install.sh` (root) installs gcc, libicu and the .NET SDK from the official tarball with a SHA-512 check.
- `build.sh` (normal user) gets the SQLite source with a SHA-256 check and compiles it with gcc two times. Between the two compiles, `sqlite-train.c` runs the server's statements on a synthetic database, and gcc records a profile. Then `build.sh` publishes the server.
- Nothing on the box is tuned. All settings are inside the process: SQLite pragmas, GC mode, the listen backlog.

## Checkpoints and slow disks

With io_uring, a second SQLite connection on its own thread runs the checkpoints (`PASSIVE`, every 50 ms when there were commits), so their `fsync` calls never stop the loop thread. The request connection has `wal_autocheckpoint=0` and normal locking. A `PASSIVE` checkpoint never blocks writes, so under steady writes it never finds the whole WAL copied, and SQLite would never start the WAL again from the beginning. So at 4,096 frames (16 MB) the loop holds writes: a `POST` stays in the connection's input while reads go on, until the checkpoint thread has copied every frame (about 80 to 170 ms on the box below). Then the held requests run, and the first write starts the WAL again. If that takes over 500 ms, writes go on and the next hold comes 4,096 frames later.

On the Synteq box below, SQLite's automatic checkpoint inside the commit (the old design) kept the loop thread waiting for the disk 22% of the time at 320,000 users (`fsync`, then the block layer's writeback throttling): the server sat idle while requests queued. With the checkpoint thread, the hold p95 at 160,000 users fell from 79 ms to 6 ms.

With the epoll transport, the connection holds the database lock for its whole life (`locking_mode=EXCLUSIVE`), so SQLite's automatic checkpoint runs on the loop thread, inside the commit that fills the WAL to 1,000 frames. A checkpoint calls `fsync` two times, and every request waits for it. That saves about 8% CPU against a checkpoint thread on a second connection with a transaction per statement (measured on the machine below), and it is fine when `fsync` takes a few milliseconds. On a disk where `fsync` takes 80 ms, the tail latency grows a lot (see the second table below).

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
| Accept stops while the fd limit minus 512 client sockets are open. With the fd table full, the .NET runtime's own `pipe2` calls fail, and the process aborts with `PAL_SEHException` (the crash of #108 at the judge's fd limit). | 80,000 users: the old version aborted (11.1% failed), this one passes with 0 failed (wsl26 x86) | `gdb` on the crash, #117, #83 |
| Keep-alive budget. A connection accepted while the fd limit minus 1,024 sockets are open is shed: one request, `Connection: close`, then the server closes it. Kept connections stay kept. | 130,000 and 160,000 users pass with 0 failed (wsl26 x86, and agent-box arm64) | #117, #72 |
| The last send before a close that the loop owns has `MSG_MORE`, so the FIN goes on the segment with the response | 160,000 users: CPU per request -4.6% (38.35 to 36.58 µs, two runs each), hold p99 24.6 to 18.7 ms (wsl26 x86) | #117 |
| A shed connection never goes to Kestrel: the loop keeps it from accept to close. Kestrel's dispatch of a connection costs a thread-pool wake-up. | 160,000 users: CPU per request -12% (36.58 to 32.2 µs). Shed connections only: 35.1 to 20.5 µs per request (wsl26 x86) | consult (DeepSeek), perf |
| Own io_uring transport (above): fixed files past the fd limit, multishot accept and receive, provided buffers, sends that finish inside `io_uring_enter`. No keep-alive budget: every connection stays open. | 160,000 users: CPU per request -18% (36.85 to 30.15 and 30.08 µs), 727 new connections per second (was 7,372: the shed users). 240,000 users pass at 72% CPU (Synteq box, below). `GET /posts/:id` at saturation: 3.0 to 1.0 syscalls per request. | #152, #82, #148, #154 |
| io_uring: checkpoints on a second connection in a background thread, one transaction per loop pass (reads too), writes held at 4,096 WAL frames until the checkpoint catches up | 160,000 users: hold p95 79 to 6 ms, p99 110-198 to 33-35 ms; CPU per request +2.5% (30.1 to 30.72 and 30.93 µs). 320,000 users: 25,568 to 31,967 requests per second (Synteq box) | #141, #82 (one read transaction per batch), perf (`fsync` on the loop thread) |
| `/feed` by id range: when the 20 newest posts are one run of consecutive ids (the usual case, since posts are created in id order), one rowid range scan reads the posts and one range scan of `likes_post_id_idx` counts all 20 posts' likes, in the transaction of the id query. Else the plain query. | 160,000 users: CPU per request -14% (30.72 and 30.93 to 26.34 and 26.39 µs). `/feed` at saturation: instructions -11% (Synteq box) | #154, #152, #82 (tried in round 1 on wsl26: CPU the same) |
| The PGO training run (`sqlite-train.c`) does what the io_uring mode does: normal locking, one transaction for each 16 statements, `PASSIVE` checkpoints on a second connection, the feed by id range | 160,000 users: CPU per request -2.1% (four pairs: -1.37, -0.01, -0.76, +0.04 µs). `/feed` at saturation: cycles -4.5% in 6 of 6 rounds, same instructions | #60 (PGO build) |
| Own JSON writer instead of `Utf8JsonWriter` (third try; the first two ran on WSL2, where syscalls hid it): keys as fixed bytes, values as SQLite's UTF-8 bytes, one vectorized search for the bytes to escape | 160,000 users: CPU per request -3.3% (two pairs: -0.44, -1.20 µs). `/feed` at saturation: instructions -5.6% | #74 |
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
| Own JSON writer, tried two times on WSL2 (kept on the third try, above) | Same CPU at first. Later, CPU -0.9% (less than the noise). | #74 |
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
| Checkpoints on a second connection in a background thread, normal locking, a transaction per statement (epoll transport) | CPU +8%, hold p99 0.9 ms. Kept later for io_uring, with a transaction per loop pass. | #108 |
| `wal_autocheckpoint=8000` | CPU +4%, hold p99 72 ms | — |
| `wal_autocheckpoint=4000`, with group commit and no `mmap` | Same CPU | — |
| Feed in three statements: the 20 ids, then the like counts and the posts by id range | `/feed` CPU the same (instructions -9%, cycles +2%) (wsl26 x86). Kept later with io_uring: -14% on the Synteq box. | #72, #82 |
| `TCP_NODELAY` on the listening socket (accepted sockets inherit it), not on each socket | Less than the noise (wsl26 x86) | #112 |
| No `epoll_ctl` before `close` of a connection the loop owns | Less than the noise (wsl26 x86) | consult (DeepSeek) |
| The first `recv` right after `accept` | Slower (wsl26 x86) | consult (DeepSeek), #117 |
| The loop sleeps out the rest of a 2 ms cycle when one wake-up has 32 or more events | 160,000 users -0.6 µs, but 64,000 users +0.3 µs and hold p99 worse (wsl26 x86) | — |

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

### Past the fd limit (users held)

Each user keeps one connection, so past about 65,000 users the server runs out of fds (`LimitNOFILE=65535`). The numbers below come from the same WSL2 machine (Ryzen 9 7900X3D, x86-64), with the server on one core and kload on other cores, 60 s ramp and 120 s hold. They show CPU per request, CPU in the hold and hold p95 / p99. All runs had 0 failed requests unless the table says otherwise.

| Users | `4e7e309` (before) | `3b5782d` (fd guard, keep-alive budget) | `5141d7f` (+ `MSG_MORE`) | `c8ef1f2` (+ shed connections skip Kestrel) |
|---|---|---|---|---|
| 64,000 | — | 34.69 µs, 23%, 3.2 to 3.4 / 13.9 to 33.0 ms (4 runs) | 33.55 µs, 23%, 3.1 / 12.1 ms | 33.32 µs, 22%, 3.1 / 11.2 to 12.2 ms (2 runs) |
| 80,000 | **aborted** (`PAL_SEHException`), 11.1% failed | 41.78 µs, 35%, 6.1 to 6.4 ms p95 (2 runs) | — | — |
| 160,000 | — | 38.29 µs, 64%, 13.9 to 16.3 / 24.4 to 35.1 ms (3 runs) | 36.58 µs, 62%, 11.1 / 18.7 ms (2 runs) | 32.2 µs, 55%, 10.3 to 13.6 / 18.0 to 28.8 ms (4 runs). A fifth run failed: the server stalled for about 5 s (p99 4.2 s, 3 failed). Not seen again in the 4 runs. |
| 200,000 | — | 36.29 µs, 77%, 15.4 / 21.1 ms | 34.94 µs, 74%, 14.4 / 20.4 ms | 30.95 µs, 65%, 13.5 / 19.6 ms |
| 250,000 | — | 34.01 µs, 90%, 116 / 144 ms | 32.31 µs, 85%, 17.5 / 22.4 ms | 29.35 µs, 78%, 16.2 / 20.9 ms |
| 300,000 | — | — | — | 28.21 µs, 88%, 204 / 586 ms (one 107 ms stall of the load generator) |
| 350,000 | — | — | — | **fails**: p95 1.0 s, 0.7% failed |

The 64,000-user numbers here are higher than in the table above (32.38 µs) because they ran on another day: compare numbers inside one table only.

On this machine, Docker's connection tracking (`nf_conntrack`, 262,144 entries) filled up at 130,000 users and dropped new connections. For the runs above, connection tracking was off for loopback traffic on the bench machine (`iptables -t raw ... -j NOTRACK`). That is a setting of the bench machine, not of the submission.

The first tests of the fd guard and the keep-alive budget ran on agent-box, an OrbStack arm64 VM on an Apple CPU (a draft of the keep-alive budget passed 130,000 and 160,000 users there, 0 failed). That VM was noisy, so the table uses only the x86 numbers. All numbers in the optimizations table marked "wsl26 x86" come from the machine above.

Final check of this version (commit `aba8cf9`) on another machine: a VM with 4 vCPUs of an Intel Core i5-11400F and 3.9 GB of RAM, server with `--cpus=1 --memory=2g --memory-swap=2g` and `nofile` 65535, direct, kload on the other 3 vCPUs. 15,000 users, 60 s ramp. The disk of this VM is slow: a 4 KB write with `O_DSYNC` takes about 80 ms.

| Database on | Hold | Requests in the hold | Failed | Hold p95 / p99 | Max (whole run) | CPU in the hold | Peak anonymous RSS |
|---|---|---|---|---|---|---|---|
| VM disk (slow `fsync`) | 5 min | 473,086 | 0 | 440 / 578 ms | 1,278 ms | 9.2% | 52 MB |
| tmpfs | 1 min | 95,264 | 0 | 2.8 / 3.3 ms | 18 ms | 8.8% | 50 MB |

### On a box like the grader's (Synteq)

A Synteq VPS with 8 shared vCPUs of the grader's CPU (Intel Xeon Platinum 8260), Ubuntu 24.04, kernel 6.8.0-117. Server in Docker with `--cpus=1 --memory=2g --memory-swap=2g`, `nofile` 65535, pinned to one vCPU, seccomp off (Docker's default profile blocks io_uring; the grader runs the server under systemd without one), direct. kload (#87) on 4 other vCPUs over loopback, 60 s ramp, 120 s hold. Connection tracking off for loopback (Docker's `nf_conntrack` table filled at 160,000 users and dropped connections). Loopback is not the grader's network: the server's CPU also pays for some of the client side of each packet, so compare these numbers with each other only.

| Version | Users | Result | CPU per request | CPU in the hold | Hold p95 / p99 | Peak cgroup memory |
|---|---|---|---|---|---|---|
| `80f7274` (epoll) | 160,000 | pass, 0 failed | 36.85 µs | 62% | 77 / 220 ms | 236 MB |
| io_uring | 160,000 | pass, 0 failed | 30.15, 30.08 µs | 51% | 77 to 79 / 110 to 198 ms | 380 MB |
| io_uring | 240,000 | pass, 0 failed | 28.69 µs | 72% | 127 / 161 ms | 510 MB |
| io_uring | 320,000 | **fails**: p95 14.7 s, 0 failed | 30.05 µs | 77% | 14.7 / 15.4 s | 968 MB |
| + checkpoint thread | 160,000 | pass, 0 failed | 30.72, 30.93 µs | 52% | 5.8 to 5.9 / 33 to 35 ms | 385 MB |
| + checkpoint thread | 320,000 | **fails**: p95 1.8 s, 0 failed (CPU-bound) | 31.04 µs | 99% | 1.8 / 4.4 s | 760 MB |
| + `/feed` by id range | 160,000 | pass, 0 failed | 26.34, 26.39 µs | 45% | 5.1 to 5.2 / 24 to 29 ms | 379 MB |

With io_uring the cgroup holds the kernel's memory for every open socket (about 2.4 KB each), so it grows with the users: the server process itself stays near 150 MB.

## Checks

- `test/test.sh`: 42/42, also in a clean `ubuntu:24.04` container with 1 CPU, 2 GB and no swap, after `install.sh` (61 s) and `test/run.sh` (77 s).
- io_uring transport: 5,000 keep-alive connections over rings of 1,100 slots (`nofile` 1100, so 5 rings): two rounds of requests on each, a pipelined pair, a request split in two writes, `Connection: close` with the FIN, half the clients closing first: 0 failed.
- io_uring with the older buffer API (`FS3_LEGACY_BUFS=1` forces it): `test/test.sh` 42/42, the 5,000-connection test and the io_uring write test below: 0 failed.
- JSON escaping: posts with quotes, backslashes, control characters, emoji, CJK, `</script>`, DEL, a no-break space and 500 characters, read back by id and in the feed: the parsed responses are the same as from the old `Utf8JsonWriter` version.
- HTTP edge cases: 30 pipelined requests in one write, a request sent 7 bytes at a time, `Connection: close`, HTTP/1.0, a bad request line, a 20 KB header, `Transfer-Encoding: chunked`, a 501-character body, 2,000 open connections (no file descriptor leak), a client that closes in the middle of a body.
- Writes with io_uring and frequent holds (`FS3_HOLD_FRAMES=16`): 64 clients each created 40 posts, liked each one twice (201, then 200 `already_liked`), read it back, and sent a pipelined create and read. 0 failed, 5,120 posts. After `kill -9` and a restart on the same files, all 5,120 posts read back.
- Writes: 100 clients sent likes and new posts, and the server was killed with `SIGKILL` during the load. After the restart, all 1,379 rows that got a 201 were in the database. With the database on a full disk, the server sent no 201 for a write that did not commit: it closed those connections, and `/health` still answered 200.
