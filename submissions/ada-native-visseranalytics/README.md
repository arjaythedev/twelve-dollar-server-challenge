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

**One process, one thread, one SQLite connection.** Our first version ran six
worker processes to get past the 65,535-descriptor limit. Profiling showed what
that cost: with six SQLite connections, every write by one worker invalidates
the page cache and memory map of the other five, so they kept rebuilding the
map, re-reading WAL frames and taking file locks. Moving to a single process
cut CPU per request by a quarter on its own.

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

**Batches share one SQLite transaction.** Requests handled in the same event
batch run inside one transaction, opened on first use. At the end of the batch
it commits, and only then are that batch's responses sent: a 201 always means
the row is committed, and no read can ever be answered with data that might
still roll back (if a commit fails, every response from it becomes a 500).
This is also group commit: one WAL append per batch instead of one per write.

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

## Rules

- SQLite uses WAL with `synchronous=NORMAL`, foreign keys on, and checkpoints
  after 4,000 WAL pages. Schema and indexes are unchanged.
- No response, query result or token verification is cached. Every request
  reads SQLite while it is served; a batch's transaction starts after all of
  its requests have arrived and ends before any of its responses leave.
- Writes commit before their success responses are sent.
- Every JWT is fully verified on every authenticated request: HS256 signature
  (constant-time compare), `alg`, `exp` and `nbf`.

## How far did we get locally?

These are workstation measurements, **not official scores**: a native arm64
Linux VM on Apple Silicon (Docker Desktop), the server pinned to one CPU with
2 GiB and the 65,535 descriptor limit, and a C load generator that follows the
`bench/load.js` user loop in the same network namespace over loopback.

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
Every 10 seconds it logs open connections and its slowest batch. Use a writable
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
