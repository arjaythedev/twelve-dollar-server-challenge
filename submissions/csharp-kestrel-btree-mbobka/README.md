# C# on Kestrel, io_uring, SQLite B-tree reads (csharp-kestrel-btree-mbobka)

| Item | Value |
|---|---|
| Language | C# 14 on .NET 10 (SDK 10.0.403, pinned in `global.json`), ReadyToRun with tiered PGO |
| Server | Kestrel (ASP.NET Core 10) as the host, with our io_uring transport in its transport slot |
| Database driver | Unmodified SQLite 3.53.4 amalgamation from source (SHA-256 checked), compiled with `shim.c` into `libsqlite3.so` next to the binary, profile-guided gcc build |
| JSON | Own writer (`Out.cs`); `System.Text.Json`'s `Utf8JsonReader` for request bodies and JWT claims |
| **Nginx or direct** | **Direct**: the server listens on `HOST:PORT` (`0.0.0.0:80`) itself |

Kestrel starts the server and binds the endpoint. It never sees a connection: its accept stays pending, and `Loop.cs` owns every
socket from accept to close, because handing a connection to Kestrel wakes a thread-pool thread that spins against the loop on
the one core (measured by fsharp-kestrel-delneg).

This is the sibling of `csharp-kestrel-mbobka`, with two differences: `GET /feed` and `GET /posts/:id` read SQLite's B-tree cursors
instead of running SQL, and a VFS shim writes each commit to the WAL with one `pwrite`. Writes are still SQL.

## Where this comes from

This isn't written from scratch. It started as a merge of two entries in this repo, then took the B-tree reads and the VFS shim
from the top of the board. Parts of the code are adapted from these MIT-licensed submissions:

| From | What |
|---|---|
| `fsharp-kestrel-delneg` (#201) | The architecture: Kestrel as a host, waiting in ring 0, sends with `MSG_DONTWAIT` from the arena, one transaction per pass, the checkpoint thread, write holds, the thread starting the WAL itself, the PGO training run (`sqlite-train.c`), ReadyToRun with tiered PGO, workstation non-concurrent GC, the `resv[0]=1` retry for Ubuntu's inverted buffer ring check |
| `csharp-epoll-gichamba` (#174) | The C# side: typed structs for the io_uring ABI, API validation (`Api.cs`, which it took from `csharp-aspnetcore-maks-march`, #27), JWT (`Jwt.cs`), the JSON writer |
| `rust-iouring-kensac` (#264), `c-iouring-pavlyhalim` (#245) | The B-tree layer in `shim.c`, eight `SO_REUSEPORT` listeners, checkpoint thread at nice 19, `unix-excl` VFS, `-mprefer-vector-width=256`, answering less than is read in a pass |
| `c-iouring-pavlyhalim` (#245) | The one-write-per-commit VFS shim, with the change described below |
| Not taken from those entries | The hard connection limit with refusal at SYN, connection state in native memory, heads written in front of the body, walking the posts backwards so nothing is copied (kensac walks forward and copies the text to put it in order), the extra `io_uring_enter` only on kernels that need it, `journal_size_limit = 0` for the reason below (`c-iouring-public` and `rust-axum-mjc` also set this pragma) |

## Reads go through SQLite's B-tree, not its SQL engine

The organiser allowed this on 2026-10-31 (see the rulings in `LEADERBOARD.md`). `shim.c` `#include`s the unmodified amalgamation and
exposes a few cursor calls. Every page still comes through SQLite's pager, WAL and page cache, inside the pass's SQLite transaction.
What's skipped is the bytecode interpreter and the `sqlite3_column_*` layer. `Db.cs` decodes the record bytes SQLite returns straight
into the response arena.

- **Feed.** Walk `posts_created_at_id_idx` from its first entry: the index is `(created_at DESC, id DESC)`, which is exactly the
  response order. When the 20 ids are one run, count all their likes with one range walk of `likes_post_id_idx`, seek the
  newest post, and walk `posts` *backwards*. The rows then arrive in response order, so nothing is copied or reordered: the
  body goes from the page into the arena while the cursor sits on it. Each author is a seek on `users`.
- **No assumptions about the data.** Ids with gaps, a post whose user row is missing (where the `JOIN` would drop it) or a
  record in an unexpected form all fall back to the reference SQL query, in the same transaction.
- **Live reads (rule 5).** Every request reads its rows while it is served. Only prepared statements, cursors, KeyInfo objects
  and four root page numbers are reused. `unix-excl` keeps other processes off the file and the second connection only
  checkpoints, so nothing can change the schema while the server runs. At startup the server checks the column order of every
  table and index it decodes, and refuses to start on a different schema.

Read and write interleave inside one pass, and SQLite handles that: a write saves the open cursors, and the next seek restores
them. If SQLite rolls a transaction back by itself, the cursors fault, every later read fails, and the pass's COMMIT reports the
failure, so no response claims a row that didn't land.

## One write per commit

A small VFS shim over `unix-excl` (SQLite's public plugin API, allowed by the same ruling), adapted from c-iouring-pavlyhalim, writes
each commit to the WAL with one `pwrite` instead of two per frame. It keeps writes that continue exactly where the previous one ended
in a buffer, and writes it when the commit frame's page arrives, inside SQLite's own call, so the commit is in the kernel before
`COMMIT` returns (rule 6). Any other write or call on the file writes the buffer first, so the bytes and their order are SQLite's.
`MB_VFS=0` leaves it out.

**One change from the original, because of a bug we hit.** Here a second connection restarts the WAL, so the next commit on the
request connection can begin with a frame header that doesn't continue the previous write. If that frame is itself a commit frame,
the shim has to remember it: dropping the flag left the commit's page in the buffer. Under load that corrupted a checkpoint and
crashed the server. The header is now checked on that path too, and `MB_HOLD_FRAMES=64` makes the tests hit it constantly.

**We can't show this shim gains anything on our rig.** At 280,000 users the same build measured 18.49 µs per request with it and 18.71
without, one run each, within the noise between runs. We didn't count `pwrite`s ourselves; c-iouring-pavlyhalim reports 0.24 to 0.095
per request. It stays because, with that fix, it passes every test here and costs nothing we could measure.

## Past 320,000 connections, clients are refused

**The server holds at most 320,000 connections and the score can't go above that.** On 2 GB, memory decides the score. What the
kernel keeps per open socket (about 4.4 KB, most of it unreclaimable) can't be changed from inside a process, so the budget starts
there and everything else is cut to fit:

| Memory at the end of a 320,000 hold (measured) | MiB |
|---|---|
| Kernel slab (sockets, files, io_uring requests) | 1,392 |
| This process, anonymous | 122 |
| Page cache (evictable) | 264 |
| Free | 101 |

At the limit the listeners stop listening (`shutdown` on the listening sockets), so the kernel answers a new SYN with a reset and
allocates nothing. Sockets the kernel had already accepted are closed at once: in the run below, open connections peaked at 320,154
for a moment and then stayed at 320,000. Listening resumes once the count falls below 318,976. Five rings of 64,511 slots hold the
connections. The limit looks only at how many connections are open, nothing else.

Each refused connection is a failed request for that user, so a run above the limit fails on `http_req_failed`. That is intended:
the alternative was running out of memory and failing everyone. The limit is a measured choice, not a proof that 320,000 is the
right number on your box.

## What keeps it alive at the wall

A stall of a second or two is where servers die here: every request waiting in its socket holds a 3-4 KB network buffer, and
hundreds of thousands of them run the box out of memory. Each of these came from following one such failure back to its cause:

1. **Hot pages live in SQLite's 64 MB cache,** in anonymous memory, which can't be taken back with no swap. In the kernel's page
   cache they look cold, since SQLite's cache keeps them from being touched. With a 16 MB cache, reclaim evicted them and the
   request thread blocked on a disk read in `pread`.
2. **The WAL is cut to nothing each time it starts again** (`journal_size_limit = 0`). Frames aren't page-aligned, so overwriting
   an evicted WAL page first reads it from disk, inside the COMMIT, on the request thread. Past the end of the file there's
   nothing to read. The kernel also gets the old WAL's page cache back at once.
3. **Reading comes before answering.** A pass answers at most 2,048 requests. Whatever else has arrived is copied straight out of
   the kernel into the connection's own few hundred bytes and answered in a later pass. Reading is far cheaper than answering,
   so the backlog leaves the kernel's buffers at once. The idea is from c-iouring-pavlyhalim.

## How it works

1. **One thread, many rings.** Accepted sockets are direct descriptors in a ring's fixed-file table, so `LimitNOFILE=65535` doesn't
   cap them. Eight `SO_REUSEPORT` listeners give eight accept queues of `somaxconn`.
2. **A pass.** The loop waits in ring 0 for 32 completions or 2 ms, then collects the other rings. Everything answered runs in one
   SQLite transaction. After the COMMIT, each connection with output gets one send, which finishes inside `io_uring_enter`. A
   response always follows the commit of its write (rule 6).
3. **No copy of a response.** The arena is a pinned array. The body is written straight into it after room for the head, and the
   head goes just before the body, so `Content-Length` needs no back-fill and no bytes shift. Sends point into the arena.
4. **Nothing for the GC to do.** Connection state is a 64-byte struct in `mmap`ed memory, not objects and not managed arrays, and
   the request path doesn't allocate.
5. **Checkpoints don't stop requests.** A second connection on a nice-19 thread runs `PASSIVE` checkpoints. At 15,000 WAL frames
   writes are held (reads go on) for up to 500 ms, while the thread copies the rest and starts the WAL itself by writing
   `PRAGMA user_version` with its current value, so the WAL header's `fsync` happens there too. bpftrace over a 5-minute hold shows
   no `fsync` on the request thread.

## ReadyToRun or Native AOT: it hardly matters here

ReadyToRun with tiered PGO is the setting of fsharp-kestrel-delneg; the choice isn't new here. At this load the kernel takes nearly
all the CPU: one `perf` snapshot at 320,000 users (this entry, AOT build) listed 63.5 points in the kernel, 7.7 in user code, about 5
of those in C#. So the compile mode can only move a small share. We tried both and kept ReadyToRun because it came out a little ahead,
on weak evidence:

| 280,000 users, CPU µs per request | ReadyToRun | Native AOT |
|---|---|---|
| This entry, two runs each | 16.82, 16.22 | 18.56, 19.64 |

Peak memory of the app's cgroup was the same within noise (650-719 MB against 670-683 MB). In one overload run each at 340,000, the
AOT build ran out of memory at about 110 s and ReadyToRun didn't. The host wasn't quiet during that run and we didn't establish that
the compile mode caused it.

## Local measurements (not a score)

Two VMs under QEMU/KVM on an i9-13900K, nested in WSL2: the server on Ubuntu 24.04 with kernel 6.8.0-147, 1 vCPU, 1,967 MiB, no swap,
under systemd with `LimitNOFILE=65535`, disk limited to 3,000 IOPS and 200 MB/s. kload (#87) on 14 vCPUs of a second VM, direct on
port 80, 60 s ramp, 5-minute hold, 30 s ramp-down, the host otherwise idle. This is not your droplet, so compare these numbers
with each other. As a rough control, on this rig `csharp-epoll-gichamba` passed 210,000 (hold 222/350 ms) and `fsharp-kestrel-delneg`
just missed at 247,000 (whole-run p99 1,004 ms), one run each.

One run per row, on the submitted code. Latency in ms, p95/p99. "Whole run" includes the ramps and is what k6's thresholds see.

| Users | Whole run | Hold | Req/s in hold | CPU µs/req | VM busy | Errors | Thresholds |
|---|---|---|---|---|---|---|---|
| 300,000 | 120/971 | 123/**1,204** | 31,600 | 18.00 | 71% | 0 | pass, though the hold alone is over 1 s on p99 |
| 314,000 | 113/309 | 115/326 | 33,143 | 18.50 | 76% | 0 | pass |
| 320,000 | 114/350 | 108/264 | 33,782 | 18.48 | 78% | 0 | pass |
| 340,000 (2-minute hold) | 420/1,139 | 375/594 | 37,599 | 18.32 | 88% | 7.67% whole run, 10.65% in hold | fail, as intended |

- **320,000 is at the edge, not comfortable.** An earlier run of this configuration at 320,000 on the quiet host failed (whole run
  684/2,212 ms, hold 528/1,204 ms), and an earlier 314,000 passed at 202/506 ms. So at 320,000 it's one pass in two. The 300,000 row
  shows that a bad interval can hit a lower load too.
- **The ramp-down is a known weak spot.** At 320,000, a 10-second window during the ramp-down reaches p95 815 ms, against 42 to 276 ms
  over the last three minutes of the hold: closing about 10,000 connections per second competes with requests on the one thread.
- **The 340,000 run shows the limit working.** About 490,000 connection attempts were refused, which is what the failed requests
  are. Open connections stayed at 320,000, there was no OOM, and the connected users kept being served at the latencies above.

## Checks

- `test/test.sh`: 42/42, also on a fresh Ubuntu 24.04 container from the tracked files only: `install.sh` as root, `test/run.sh` as
  a normal user.
- HTTP edge cases: 30 pipelined requests, a request sent 7 bytes at a time, `Connection: close`, HTTP/1.0, a bad request line, a 20 KB
  header, chunked bodies, `Expect: 100-continue`, 3,000 feeds pipelined to a slow reader, a half-closed client, a client vanishing
  mid-body, JSON escaping round trips.
- Limit: with `MB_MAX_CONN=2000`, of 2,100 connections 2,000 are answered and the rest refused, and listening comes back once enough
  have closed.
- Durability: 32 clients posting and liking with `MB_HOLD_FRAMES=64`, `kill -9` during the load, 5 rounds on the same files: none of
  61,732 acknowledged posts and 61,672 acknowledged likes lost, `integrity_check` ok. `integrity_check` was also ok after each load run
  from 300,000 to 320,000.

## Rules

- SQLite itself isn't patched: the amalgamation is `#include`d as downloaded, and no file format code of our own reads the database.
- Writes: WAL with `synchronous=NORMAL` on both connections. A response is sent only after the commit of its pass; if a commit
  fails, the connections of that pass are closed without a response.
- No cache: see "Live reads" above.
- Nothing detects the load test. The connection limit looks only at how many connections are open.
- `install.sh` (root) installs gcc, libicu, libssl, curl and the .NET SDK from the official tarball with a SHA-512 check.
- `build.sh` (normal user) gets the SQLite source with a SHA-256 check and builds it twice around `sqlite-train.c`. The profile comes
  from that run on a synthetic database during the build; no profile data is committed. NuGet packages come at the versions SDK
  10.0.403 pins.
- `start.sh` sets nothing but the binary. The `MB_*` variables in the source are overrides for the tests above, unset in normal
  running; their defaults are the submitted configuration.
- Nothing on the box is tuned. The process raises its own soft `RLIMIT_NOFILE` to the hard limit.
