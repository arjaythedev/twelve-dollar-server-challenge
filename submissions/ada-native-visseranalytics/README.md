# Ada on a $12 server

We wanted to bring a less usual language to the challenge, so we picked Ada
and started squeezing. That turned into a small HTTP server, some time spent
chasing allocations, a lot more attention to idle sockets than expected, and
finally a rewrite around io_uring.

The implementation uses **Ada 2022**, with no web framework: HTTP, JSON,
routing and the event loop are written in Ada. io_uring and sockets are called
through the Linux ABI directly (no liburing), SQLite is compiled into the
binary from the official amalgamation (the two hot read routes use its B-tree
layer directly), and OpenSSL computes the HS256 HMAC. It
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
rings of 65,535 slots each (393,210 connections) from one thread. Accepts are
multishot, and an idle socket keeps a single multishot readiness watch; when
it fires, one non-blocking receive into a provided buffer runs in the same
batch (see "Memory decides the score" below). Rings are set up with
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

**The loop does not sleep while requests are waiting.** After a heavy turn (64
completions or more) the loop goes straight back to the kernel for the next
batch without waiting: the work itself is the batching interval. After a
lighter busy turn it waits for 64 completions or 250 µs, whichever comes first,
and an idle loop sleeps until something arrives. Earlier versions waited up to
2 ms for 64 completions on every busy turn, which put the vCPU to sleep between
batches while requests queued.

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

**Memory decides the score.** On the benchmark box every entry near the top
stops between 280,000 and 300,000 users with mass "connection refused": the
box runs out of memory and the server is killed. The kernel needs about 4.4 KB
for each open connection (socket, inode, file, dentry, the io_uring request
watching it), and a request that waits unread in a socket costs about 2.9 KB
more, so the server's own memory and any stall decide where that happens. #216
spent its effort there:

- An idle socket keeps a multishot *poll* instead of a parked multishot
  receive. Both are one io_uring request, but the receive also holds an async
  poll record, so this saves 96 bytes of kernel memory per connection (26 MB at
  280,000), at the same CPU per request because the receive runs in the same
  batch as the poll that announced it.
- SQLite's cache is 32 MiB (64 in #211, 16 in #216 and #223). Most of what
  this load keeps hot is the likes primary key (29.5 MB; every like inserts
  into it at a random place). Whatever does not fit sits in the kernel's page
  cache, which can be reclaimed under pressure, while SQLite's cache is
  anonymous memory, which cannot; but a page the kernel has dropped is read
  back from disk inside the event loop. #211 (64 MiB) was the smoothest of our
  runs on the box at 270,000 users and the one killed for memory at 280,000;
  32 MiB sits between the two.
- A memory guard. While the kernel reports less than 160 MB available
  (`MemAvailable`, read once a second), the server stops accepting: new
  connections wait in the listen queue while the open ones are served, and
  accepting resumes above 256 MB. Near the edge, a server that is OOM-killed
  loses every connection at once, while one that pauses only delays the
  newest.
- Connection records are 40 bytes (flags packed into bits), rings have 1,024
  receive buffers each instead of 2,048, and the response pool keeps at most
  1,024 spare blocks per size and returns the rest to the kernel
  (`malloc_trim`).
- After a stall, every waiting socket wants a receive buffer at once. Sockets
  that find none now queue up and are served a bounded number per ring per
  batch; before, every one of them retried on every batch and failed again
  (about 3.5 failed receives per connection after a 3-second pause at 200,000
  users), which stretched the backlog that was eating the memory.

Altogether the server's own memory at 280,000 connections went from 77 MB to
40 MB, and the kernel's from 1,249 MB to 1,220 MB.

**Nothing on the request path allocates.** HTTP requests are parsed in place
from the kernel's receive buffers into index ranges; there is no
Unbounded_String or secondary-stack string left in the hot path. Responses are
built in one scratch buffer, framed, and copied once into a pooled block that
lives until its send completes. JSON escaping first counts escapable bytes in a
branch-free loop that GCC vectorizes, so ordinary text is copied with one
`memcpy`.

**The hot reads skip SQL.** `GET /feed` and `GET /posts/:id` are 92% of the
load. They read their rows through SQLite's own B-tree cursors instead of
prepared statements: the same pages, page cache and WAL snapshot, inside the
same batch transaction, just without the bytecode interpreter in between.
SQLite's cursor functions are internal to the amalgamation, so
`src/sqlite_btree.c` compiles the amalgamation together with ten small
read-only wrappers (open the cursors, seek a rowid, seek an index key, step,
fetch the current record); the Ada side decodes the records (`Database`,
"Direct B-tree reads"). It only ever reads, and anything it does not expect, such
as a missing author, a value of another type, or a record that spills onto
an overflow page, sends that request through the SQL statements instead, which
remain the reference. At startup the server checks that the tables and
indexes are exactly the challenge's before it enables this. Without the
network, CPU per request in the load test's mix went from 3.8 to 2.9 µs
(`bench/request_cost.adb`). `tests/btree_reads.py` compares both routes byte
for byte with the reference SQL under a random mix of likes and new posts, and
on rows that must fall back. (Reading SQLite's B-tree directly was ruled
allowed for lean-derive-hargup, #153.)

**SQLite built for this job.** SQLite 3.54.1 is downloaded by `build.sh`
(SHA-256 pinned; it matches sqlite.org's published SHA3-256) and compiled with
`SQLITE_THREADSAFE=0` and the recommended omissions. The page cache is 16 MiB
(see above). Memory-mapped I/O is off:
with a single connection SQLite's own page cache stays valid across requests,
while mmap made every page access probe the WAL index again (11% more
instructions per request). Feeds find the newest 20 IDs, then read rows and
like counts with two range scans when the IDs are dense; sparse, reordered or
orphaned rows fall back to the reference query.

**Connection lifecycle.** Idle keep-alive connections stay open for 75 seconds;
unfinished requests and responses get 30. Pipelined requests are answered in
order, one at a time, and a socket is not read again while its response is
being sent, so a client that sends far ahead without reading is held back by
TCP itself. Closed sockets are released at a
bounded rate, and SIGTERM stops the listener immediately so a restart can bind
the port at once (a restart also waits for the port if needed).

Closing is cheap even when every client leaves at once. A socket's watch is
cancelled by its user data, which the kernel finds with a hash lookup, and a
client's end of stream is read like any request, a bounded number per ring per
batch. (Cancelling by
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
  reads SQLite while it is served, through SQL or SQLite's B-tree cursors; a
  batch's transaction starts after all of its requests have arrived and ends
  before any of its responses leave.
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
fresh copy of the seed. The CPU is faster than the benchmark's vCPU, so CPU
numbers only compare versions.

### The tail at the edge (this version)

#223 (below) cut CPU per request, and on the box it did: 26 µs per request
against #216's 30. But it scored 257,000: 270,000 passed the 2-minute probe and
then failed the 5-minute hold on p99 (1.89 s at only 80% CPU), and 280,000
failed on p99 (2.3 s at 84%). The limit had moved from CPU to the tail. This
version changes how the loop waits (see Design), raises SQLite's cache from
16 to 32 MiB, and adds the memory guard.

Locally, with 480 MB locked away and a CPU spinner sharing the vCPU equally
with the server (as below), 2-minute holds:

| Version | 280,000 users | 290,000 users |
| --- | --- | --- |
| #223 | 2 of 3 passed (p99 9–20 ms); one ran out of memory | latency spike at the end of the ramp, then killed for memory |
| this version | **4 of 4 passed, p99 4.5–7.2 ms** | **2 of 3 passed (p95 7.5–12 ms, p99 17–23 ms)**; the third was killed for memory at the end of the ramp |

What we tried and left out:

- Checkpointing every 2 s instead of every 250 ms, with the WAL restarting at
  16,000 frames instead of 4,000: 41% fewer disk writes, but 0 of 2 passes at
  280,000 (a bigger WAL in memory, bigger bursts of copying).
- Restarting the WAL from the event loop between batches (copying the last
  frames itself) instead of briefly holding writes: no held writes, but every
  request then waits for those copies and syncs (p99 34 ms instead of 7 ms).
- Going back to a parked multishot receive for idle sockets: 96 more bytes of
  kernel memory per connection, and one of two runs at 280,000 ran out of
  memory.

### Less CPU per request (#223)

On the box, #216 held 270,000 users at 86% CPU and failed 280,000 on CPU
(p99 7.7 s at 97%), so #223 went after CPU per request. Reading the two
hot routes through SQLite's B-tree cursors does most of it; building with
GNAT's default validity checks instead of `-gnatVa` (every check on) does the
rest.

The request path without the network (`bench/request_cost.adb`, the load
test's mix, alternating runs):

| Version | CPU per request |
| --- | ---: |
| #216 | 3.76–3.88 µs |
| + B-tree reads | 2.88–2.94 µs |
| + default validity checks | about 4% less again |

The whole server at 280,000 users, 2-minute holds, alternating runs:

| Version | VM CPU per request | Server process alone |
| --- | ---: | ---: |
| #216 | 16.5–17.4 µs | 10.2–11.0 µs |
| #223 | 15.2–15.9 µs | 8.9–9.6 µs |

To see what that buys on a box short of both memory and CPU, we also ran
with 480 MB locked (see below) and a CPU spinner sharing the vCPU equally with
the server:

| Version | Users, 2-minute hold | Result |
| --- | --- | --- |
| #216 | 270,000 | 0 errors, p99 10 ms |
| #216 | 280,000 | latency spike at the end of the ramp, killed for memory: 71% errors |
| #223 | 280,000 | 0 errors, p95 6.4 ms, p99 11.8 ms |
| #223 | 290,000 | latency spike at the end of the ramp, killed for memory: 71% errors |

#216 behaves here as it did on the box (270,000 passes, 280,000 fails), and
#223 moved that edge up one step here (on the box it scored 257,000; see
above). Going back to a parked multishot receive for idle sockets, as in #211,
measured the same CPU as the poll watch in six alternating runs (17.0–17.6
against 16.8–18.7 µs), so idle sockets keep the poll and its smaller memory
footprint.

### The memory the benchmark box does not have (#216)

The VM has about 450–500 MB more room than the benchmark box under the same
load: our #211 code held 320,000 users here but stopped at 270,000 there,
failing 280,000 with refused connections. So we now lock memory away for
these runs with a small helper (it allocates and `mlock`s N MB and is exempt
from the OOM killer), calibrated against that official result. With 500 MB
locked, the #211 code fails 280,000 users the way it did on the box (the
kernel kills the server about a minute in: 3.2% errors, mostly refused
connections); with 450 MB it just passes (0 errors, p99 302 ms, 50 MB free).
500 MB is therefore a slightly pessimistic stand-in for the box.

| Version | Locked | Users, hold | Result |
| --- | ---: | --- | --- |
| #211 as benchmarked | 450 MB | 280,000, 2 min | 0 errors, p99 302 ms |
| #211 as benchmarked | 500 MB | 280,000, 2 min | killed for memory, 3.2% errors |
| + 16 MiB SQLite cache | 500 MB | 280,000, 2 min | **0 errors, p99 33 ms** |
| + 40-byte records, 1,024 buffers, bounded re-arm | 500 MB | 290,000, 5 min | killed at the end of the ramp, 4.4% errors |
| + idle sockets watched by poll (#216) | 500 MB | 290,000, 5 min | clean for 4 minutes, then killed: 1.0% errors |
| #216 | 480 MB | 290,000, 5 min | killed after 2 minutes, 14.5% errors |

So #216 moved the edge from about 275,000 to about 285,000 users here;
290,000 sits right on it, and runs there end either way. What tips it is the
database growing during the hold (about 50 MB of new posts and likes in five
minutes): once the hot pages no longer fit, SQLite's reads go to disk, the
event loop slows, and the requests waiting in the kernel (2.9 KB each) take
the rest.

Without locked memory, #216 held 280,000 users for five minutes with
the server paused for 3 seconds in the middle (`SIGSTOP`/`SIGCONT`): 0 errors,
p95 5.0 ms, back to normal latency about a second after the pause, and no
receive failed for want of a buffer (20,000 sockets per ring waited their
turn instead). Watch mode measured the same CPU per request as the parked
receive it replaced (15.0–18.1 µs against 15.9–17.2 µs of VM time in
alternating runs at 200,000 users).

What did not work:

- Closing idle keep-alive connections early so clients reconnect, as entries
  limited by the descriptor table do. With an 8-second idle limit at 280,000
  users, about 10,000 reconnects a second filled the kernel with orphaned
  sockets until it ran out of TCP memory, and the VM locked up.
- An 8 MiB cache and 512 buffers per ring saved another 10 MB but did worse at
  the edge (4.4% errors at 290,000 with 500 MB locked).
- A 64 MiB cache, as before: the same CPU, 32 MB more anonymous memory.

On the box, #216 scored 270,000 again: its memory held at 280,000, but the CPU
ran out there (above).

Most of the CPU at this scale is the kernel's: in a profile at 300,000
connections, about 28% of the busy time went to finding each packet's socket. A
2 GB machine gets 16,384 buckets in the TCP established-connection table, so
every incoming packet walks a chain of about 18 sockets.

### Earlier versions

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
under Rosetta). The io_uring path (which a container cannot use) is checked in
the server VM: the official suite and the 139 focused checks pass there too.

`tests/btree_reads.py` (also run by `validate-container.sh`) compares the
B-tree read path byte for byte with the reference SQL.
`bench/request_cost.adb` measures CPU per request without the network, in the
load test's request mix. `bash local.sh up` starts a local preview at
`http://127.0.0.1:3001`; the other tools in `bench/` run the unchanged k6
workload or a Python diagnostic and keep their results in the ignored
`benchmark-results/` directory.
