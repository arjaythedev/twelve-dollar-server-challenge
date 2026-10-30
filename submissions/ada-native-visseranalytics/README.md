# Ada on a $12 server

We wanted to bring a less usual language to the challenge, so we picked Ada
and started squeezing. That turned into a small HTTP server, some time spent
chasing allocations, a lot more attention to idle sockets than expected, and
finally a rewrite around io_uring.

The implementation uses **Ada 2022**, with no web framework: HTTP, JSON,
routing and the event loop are written in Ada. io_uring and sockets are called
through the Linux ABI directly (no liburing), SQLite is compiled into the
binary from the official amalgamation, and OpenSSL computes the HS256 HMAC. It
targets Ubuntu 24.04 x86_64 and **serves directly on `0.0.0.0:80`** (no Nginx).

## Design

**One event loop, one SQLite connection.** Our first version ran six
worker processes to get past the 65,535-descriptor limit. Profiling showed what
that cost: with six SQLite connections, every write by one worker invalidates
the page cache and memory map of the other five, so they kept rebuilding the
map, re-reading WAL frames and taking file locks. Moving to a single process
cut CPU per request by a quarter on its own. All requests are still served by
one thread on one connection; the only other process is a small checkpoint
helper (below), which never writes rows.

**io_uring with direct sockets.** A single process can still hold hundreds of
thousands of connections: accepted sockets go straight into io_uring
fixed-file tables instead of the descriptor table, and the server drives six
rings of 65,535 slots each (393,210 connections) from one thread. Multishot
accept and multishot receive with provided buffer rings keep one request in
flight per socket instead of one per read. Rings are set up with
`SINGLE_ISSUER | DEFER_TASKRUN`, so completions are processed only when the
loop asks for them; when busy, the loop waits for 64 completions or 2 ms, and
one wake-up serves a whole batch. The ABI layer (`src/uring.adb`) is about
350 lines of Ada.

If io_uring is unavailable (older kernels, Docker's default seccomp profile,
Rosetta), the same request handling runs on a portable epoll loop, limited by
the descriptor table to about 65,000 connections.

**A workaround for Ubuntu's current 6.8 kernels.** Ubuntu 24.04's kernels from
6.8.0-136 (August 2026) on reject every provided-buffer-ring registration with
`EINVAL` ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843);
we found it in 6.8.0-136 and 6.8.0-142, and it is reported up to 6.8.0-147): their
backport of "io_uring/kbuf: use mem_is_zero()" turned the check
"reserved fields must be zero" into "reserved fields must not be zero"
(`if (!memchr_inv(...))` where upstream has `if (!mem_is_zero(...))`). Any
io_uring server that uses buffer rings, through liburing or not, loses them on
such a kernel; ours would silently drop to epoll and its 65k-connection limit.
When the first registration fails with `EINVAL`, the server retries once with a
nonzero reserved field, which those kernels accept; a correct kernel accepts
the first attempt and never sees the retry (`src/uring.adb`).

**Batches share one SQLite transaction.** Requests handled in the same event
batch run inside one transaction, opened on first use. At the end of the batch
it commits, and only then are that batch's responses sent: a 201 always means
the row is committed, and no read can ever be answered with data that might
still roll back (if a commit fails, every response from it becomes a 500).
This is also group commit: one WAL append per batch instead of one per write.

**Checkpoints happen off the event loop.** A WAL checkpoint copies committed
pages into the database file and syncs both files; done inside a commit (as
SQLite's automatic checkpoint does), every request in flight waits for it. A
helper process, forked before the server opens SQLite, runs SQLite's own
`sqlite3_wal_checkpoint_v2` in PASSIVE mode on its own connection every 250 ms
and whenever the server asks: PASSIVE never takes the write lock and never
waits, so the event loop is never blocked by it.

Only a writer can restart the WAL, and only when every frame has already been
copied as its read transaction begins; under a steady stream of writes that
moment never comes, and the WAL would grow without end. So once the WAL passes
4,000 frames, the server briefly holds write requests (reads go on), asks the
helper for a pass, and releases the held writes as soon as the helper reports
the WAL fully copied: the first of them then restarts the WAL. At 285,000
users this happened about once a second and held writes for typically up to
40 ms (91 ms at most in five minutes; most of it is the helper's two syncs),
while every other request went on. The event loop is left with one small
WAL-header sync instead of a whole checkpoint. Held writes wait a second at
most. If the helper is gone, or the WAL still reaches 16,000 frames, the server
checkpoints inline (PASSIVE), as SQLite would.

**Nothing on the request path allocates.** HTTP requests are parsed in place
from the kernel's receive buffers into index ranges; there is no
Unbounded_String or secondary-stack string left in the hot path. Responses are
built in one scratch buffer, framed, and copied once into a pooled block that
lives until its send completes. JSON escaping first counts escapable bytes in a
branch-free loop that GCC vectorizes, so ordinary text is copied with one
`memcpy`.

**SQLite built for this job.** SQLite 3.54.1 is downloaded by `build.sh`
(SHA-256 pinned; it matches sqlite.org's published SHA3-256) and compiled with
`SQLITE_THREADSAFE=0` and the recommended omissions. Memory-mapped I/O is off:
with a single connection SQLite's own page cache stays valid across requests,
while mmap made every page access probe the WAL index again (11% more
instructions per request). Feeds find the newest 20 IDs, then read rows and
like counts with two range scans when the IDs are dense; sparse, reordered or
orphaned rows fall back to the reference query.

**Connection lifecycle.** Idle keep-alive connections stay open for 75 seconds;
unfinished requests and responses get 30. Pipelined requests are answered in
order, one at a time, and a client that sends far ahead without reading has its
receive paused until its responses drain. Closed sockets are released at a
bounded rate, and SIGTERM stops the listener immediately so a restart can bind
the port at once (a restart also waits for the port if needed).

Closing is cheap even when every client leaves at once. A socket whose client
closed first has no receive left to cancel; otherwise the receive is cancelled
by its user data, which the kernel finds with a hash lookup. (Cancelling by
socket slot, as we first did, makes the kernel scan every pending request on
the ring for each close: with 250,000 clients leaving together, closing took
tens of seconds while their unread FIN packets piled up in kernel memory,
about 2.4 KB each, enough to run a 2 GB box out of memory. Now they are all
closed within a second or two and memory goes straight down.)

## Rules

- SQLite uses WAL with `synchronous=NORMAL` and foreign keys on. Checkpoints
  are SQLite's own (PASSIVE), run by the helper process; the WAL restarts
  about every 4,000 pages. Schema and indexes are unchanged.
- Held writes are answered only after they have run and committed, like any
  other write.
- No response, query result or token verification is cached. Every request
  reads SQLite while it is served; a batch's transaction starts after all of
  its requests have arrived and ends before any of its responses leave.
- Writes commit before their success responses are sent.
- Every JWT is fully verified on every authenticated request: HS256 signature
  (constant-time compare), `alg`, `exp` and `nbf`.

## How far did we get locally?

These are workstation measurements, **not official scores**. Since the first
official run (226,000 users), we test on two Linux VMs on Apple Silicon
(Apple's Virtualization framework through Lima): the server VM has 1 vCPU and
1,958 MB of RAM (the benchmark box has 1,967 MB), Ubuntu 24.04's 6.8 kernel,
and runs the server under systemd as in the benchmark (LimitNOFILE=65535);
a second VM runs a C load generator that follows the `bench/load.js` user loop
from 64 source addresses over a virtual network card. Every run starts from a
fresh copy of the seed. The CPU is faster than the benchmark's vCPU, so the
memory limit is what these runs share with the real box; CPU numbers only
compare versions.

300,000 users, 60 s ramp, 5-minute hold, about 31,700 requests per second:

| Version | Errors | p95 | p99 | Slowest | VM CPU per request |
| --- | ---: | ---: | ---: | ---: | ---: |
| As first benchmarked (inline checkpoints) | 0 | 31 ms | 58 ms | 376 ms | 21.4 µs |
| + checkpoint helper | 0 | 19 ms | 39 ms | 302 ms | 21.0 µs |
| + write gate for WAL restarts | 0 | **5.6–6.2 ms** | **12–17 ms** | 274–344 ms | **18.9–19.5 µs** |

The finished version also held 285,000 users for five minutes (0 errors, p95
5.7 ms, p99 15 ms, 1,233 MB of the VM's memory in use) and 320,000 users (0
errors, p95 8.8 ms, p99 37 ms, 1,363 MB in use), and closed all connections
cleanly when the load generator exited.

The slowest requests in the last row are mostly TCP retransmission timeouts
after packets dropped by the VM's virtual network card (about 200 ms each),
not server stalls: the longest event-loop turns in those runs were 58–100 ms,
a handful per run. At 300,000
connections the kernel holds about 1.3 GB (about 4.3 KB per connection: socket,
inode, file, and the armed io_uring receive), which is what puts 400,000 users
out of reach of a 2 GB box for any server that keeps one connection per user.

Earlier measurements, in a native arm64 Docker Desktop VM with the server
pinned to one CPU with 2 GiB and the 65,535 descriptor limit, and the load
generator in the same network namespace over loopback:

| Version | Server CPU per request (50k connections, 21k req/s) | 300k connections, 2-minute hold |
| --- | ---: | --- |
| Six worker processes, epoll | 32 µs (15 µs user) | — |
| Same code, one worker | 22 µs (12 µs user) | (capped at 65k connections) |
| Single process, io_uring | 14 µs (5 µs user) | 0 errors, p95 17 ms, p99 36 ms, 54% CPU |
| + SQLite from source, no mmap, faster framing | **12 µs (2.7 µs user)** | **0 errors, p95 12 ms, p99 31 ms, 42% CPU** |

The 300k runs served 3.8 million requests in the hold at 31,700 requests per
second, with every user active and connected. At 300k connections the server
cgroup held 1.24 GB, mostly kernel socket memory.

Loopback makes the server's CPU also pay for the clients' TCP receive work
(about a fifth of the totals above), and Apple Silicon cores are faster than the
benchmark's vCPU, so only the comparison between rows is meaningful.

The previous README's numbers (332,800 users in a Python diagnostic, 72,000
with k6) came from the six-worker version under Rosetta and are superseded.

## Build and run

Ubuntu package versions are pinned in [versions.env](versions.env) (install.sh
falls back to the current build if a pinned version has left the archive):

| Dependency | Version |
| --- | --- |
| GNAT/GCC and libgnat | 13.3.0-6ubuntu2~24.04.1 |
| OpenSSL | 3.0.13-0ubuntu3.16 |
| SQLite (source, compiled in) | 3.54.1 |

From this directory, install once as root, then build as a normal user:

```sh
sudo bash install.sh
bash build.sh        # downloads and verifies SQLite, then builds bin/server
SQLITE_PATH=/absolute/path/to/private-feed-copy.db \
JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` runs in the foreground and prints which backend it uses
(`io_uring: 6 rings of 65535 direct sockets, deferred task work`, or `epoll`).
Every 10 seconds it logs open connections, its slowest batch and commit, WAL
restarts (`gates`), and any event-loop turn of 50 ms or more with where it spent
its time. Use a writable
copy of the seed database. The challenge service provides
`CAP_NET_BIND_SERVICE` for port 80; for an unprivileged local run, use
`HOST=127.0.0.1 PORT=3000` (the defaults).

## Local verification

`bash validate-container.sh` rebuilds and runs the official 42-check suite,
139 focused HTTP/JSON/auth/keep-alive checks, 17,578 JSON oracle cases,
adversarial feed comparisons against the reference SQL with external writers,
process lifecycle checks (fresh rollback-mode databases, SIGTERM and SIGKILL
restarts, durability) and Nginx integration. All of them pass on x86_64 (epoll,
under Rosetta) and on native arm64 Linux with io_uring and with epoll.

`bench/request_cost.adb` measures CPU per request without the network, in the
load test's request mix. `bash local.sh up` starts a local preview at
`http://127.0.0.1:3001`; the other tools in `bench/` run the unchanged k6
workload or a Python diagnostic and keep their results in the ignored
`benchmark-results/` directory.
