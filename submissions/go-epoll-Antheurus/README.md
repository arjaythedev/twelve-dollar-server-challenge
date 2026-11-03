# Go + raw epoll + SQLite (no framework)

| | |
|---|---|
| Language | Go 1.27.2 (official tarball, sha256-checked in `install.sh`) |
| Framework | none: an HTTP/1.1 server on raw `epoll`/`accept4`/`read`/`write` syscalls, parsing requests from bytes |
| SQLite | 3.54.0 amalgamation compiled into the binary with cgo (sha256-checked in `build.sh`), driven through a small C shim, no `database/sql` |
| JWT / JSON | hand-written: HS256 over the standard library's `crypto/hmac`, a strict RFC 8259 validator |
| Dependencies | the Go standard library and SQLite, nothing else |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

```bash
sudo bash install.sh   # apt: build-essential, curl, unzip; Go 1.27.2 into /usr/local/go
bash build.sh          # downloads + verifies SQLite 3.54.0, builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`test/run.sh` passes 42/42 on linux/amd64 and linux/arm64. `e2e_test.go` covers what curl in
`test/test.sh` cannot reach: pipelined requests behind a write, requests split across packets,
`Connection: close` on a write, HTTP/1.0, `Expect: 100-continue` and chunked bodies
(`E2E_ADDR=127.0.0.1:3000 JWT_SECRET=... go test -run E2E`).

## Design, and why

**One event loop per process, everything on it.** Each process is one OS thread running
level-triggered `epoll`: accept, read, parse, query SQLite, write. On one vCPU there is nothing to
gain from more threads, and a connection costs a small struct in a table indexed by fd, not a
goroutine and its buffers. Reads, writes, accepts and closes on the non-blocking sockets use raw
syscalls, skipping the scheduler hand-off of a blocking syscall. The steady state allocates
nothing, so the Go GC never runs during a load test (verified with `GODEBUG=gctrace=1`).

**Up to three processes on port 80, filled one at a time, for file descriptors, not CPU.** The
service gets `LimitNOFILE=65535`, which is per process, so one process tops out near 59k open
sockets; past that it would have to close idle keep-alive connections and k6 would reconnect
(more CPU per request, and on a box with connection tracking a full conntrack table drops SYNs).
More processes hold more sockets, but every process that serves traffic costs CPU of its own:
more wakeups and smaller batches per wakeup (measurements below). So the three processes all bind
the port with `SO_REUSEPORT`, and the first attaches a one-instruction classic BPF program to the
group (`SO_ATTACH_REUSEPORT_CBPF`, no privileges needed) that returns the index of the process
that is currently filling. When a process reaches ~56.5k sockets (checked on every accept) the
program is re-pointed at the next one. Below ~56k users one process serves everything; at 80k
two do; the third only takes connections past ~113k. Listeners are never closed, so nothing
waiting in an accept queue is lost. `socket(7)` defines the indices (`listen()` order) and the
fallback (an out-of-range index means plain hashing); if a process dies, the first one detaches
the program, falls back to hashing over the survivors, and restarts it. The first process
supervises the others, which die with it (`Pdeathsig`). If the fd table still runs short, a
process closes only connections idle for more than 2 s, shorter than the 3 s minimum think time
in `bench/load.js` and well past any request in flight.

**SQLite: one connection per process, statements prepared once, one cgo call per query.** The C
shim binds, steps every row, packs the columns into a buffer and resets, so `/feed` (20 rows, 100
columns) crosses into C once. WAL with `synchronous=NORMAL`, `mmap_size` 1 GB.

**Writes: queued, then group-committed per loop pass.** A like or a new post is authenticated and
validated at once, then queued. After the loop has served every ready socket, it runs all queued
writes in one `BEGIN IMMEDIATE … COMMIT` and only then sends their responses, so a 201 always
means committed (rule 6). The write lock is held for the inserts alone, never while sockets are
served, and `BEGIN` never waits: if the lock is held (by the other processes or the
checkpointer) the writes stay queued and are retried a millisecond later while reads keep being
served. A connection's later pipelined requests wait behind its write, so responses stay in order.

**Checkpoints on their own thread, in `RESTART` mode.** Auto-checkpointing is off in the event
loops, so a checkpoint's fsync never stalls one. A `PASSIVE` checkpoint was the first version and
it was wrong: under a steady write load the WAL never rewinds (frames arrive faster than a passive
checkpoint finishes), and it grew 8.6 MB/s to 800 MB in a minute, making every page lookup slower.
`RESTART` mode takes the write lock briefly once a second so the next writer starts the WAL from
the beginning; it now stays at ~10 MB.

**Queries.** `/feed` is the reference join (its plan is an index scan plus PK and covering-index
lookups). `/posts/:id` uses scalar subqueries instead of the join: for one row the join measured
7.0 µs against 1.5 µs (a fixed per-execution cost), while for twenty rows the join wins, 11 µs
against 12.6 µs. A like is one `INSERT … SELECT … WHERE EXISTS (post) ON CONFLICT DO NOTHING`; only
when it changes nothing does a second query tell "already liked" from "no such post".

**HTTP and JSON from bytes.** Request line and headers are matched in place
(`Content-Length`, `Authorization`, `Connection`, `Expect`, `Transfer-Encoding`), responses are
appended into one buffer and sent with one `write`. Keep-alive, pipelining, HTTP/1.0 and
`Connection: close` work; chunked request bodies get 501 (out of scope). JSON output escapes only
what must be escaped; the request body goes through a strict validator before the `body` field is
extracted. `body` is trimmed with JavaScript's `String.prototype.trim` whitespace set and limited
to 500 code points, the same unit as the schema's `length(body)` check.

**JWT.** Every token is verified on every request: signature (HS256 only, constant-time compare),
`exp`, `nbf` if present, then `sub` and `username`. The HMAC object is reused between requests
(its key schedule, not any verification result).

## Measured

Local, not the droplet: a Docker container capped at one CPU (`--cpu-period=10000 --cpu-quota=10000`)
and 2 GB on an Apple M2 Pro, `LimitNOFILE=65535`, a fresh seed copy every run, served directly.
k6 needs ~280 KB per user, too much to put 120k users on one machine, so `loadgen/` replays
`bench/load.js` (same loop, think times, mix, one keep-alive connection per user, latency
excluding the dial) over raw TCP from four client containers, scored by the same rule: p95 <
500 ms, p99 < 1 s, < 1% failed. Server CPU is the container's cgroup usage over the hold.

| Users | Hold | Requests | Failed | p95 | p99 | Server CPU | Result |
|---:|---:|---:|---:|---:|---:|---:|---|
| 55,000 | 90 s | — | 0 | 1.0 ms | 28 ms | 40 % | pass |
| 80,000 | 90 s | — | 0 | 1.4 ms | 229 ms | 57 % | pass |
| 120,000 | 120 s | 1,517,603 | 0 | 5.4 ms | 223 ms | 68 % | pass |
| 120,000 | 300 s | 3,805,601 | 0 | 1.0 ms | 14 ms | 64 % | pass |
| 120,000 | 300 s | 3,806,572 | 0 | 0.87 ms | 3.6 ms | 67 % | pass |
| 122,500 | 120 s | | | | | 101 % | fail |
| 125,000 | 120 s | | | | | 93 % | fail |

The same 80k load held for five minutes (two processes, plain hashing, before steering) served
2,536,475 requests with 0 failed, p95 1.08 ms, p99 21 ms, 55 % CPU. Binary search as in the scoring
(2 min holds): 80k pass, 160k fail, 120k pass, 140k fail, 130k fail, 125k fail, 122.5k fail. The
160k, 140k and 130k runs used the earlier hashed layout, which costs more CPU than this one; the
rest are this build. Memory peaks under 720 MiB for the whole container (page cache of the
database included); the Go heap is 14–44 MiB and the GC does not run under load. 120k was then confirmed
the way the scoring confirms a score, with a five-minute hold, twice in a row on this exact build;
all 120,004 connections stayed open through every hold (0 reconnects).

### On x86

The same commit on a 6-vCPU AMD EPYC VPS (Ubuntu 24.04, kernel 6.8), the server container again
held to one CPU and 2 GB, the four load clients on the same machine:

| Users | Hold | Failed | p95 | p99 | Server CPU | Result |
|---:|---:|---:|---:|---:|---:|---|
| 40,000 | 120 s | 0 | 4.3 ms | 176 ms | 66 % | pass |
| 50,000 | 120 s | 0 | 33 ms | 199 ms | 79 % | pass |
| 55,000 | 300 s | 0 of 1.74M | 22 ms | 168 ms | 78 % | pass |
| 57,500 | 300 s | 0 | 374 ms | 1.13 s | 90 % | fail |
| 60,000 | 120 s | 0 | 2.0 s | 3.6 s | 94 % | fail |

So about 55k there, roughly half of the M2 figure: each request costs about twice the CPU. That
box is a pessimistic stand-in for the droplet: it was also running other services, and with the
load clients on the same kernel part of their receive work lands on the server's CPU, which k6 on
a separate machine does not do.

A droplet vCPU is slower than an M2 core, so expect lower numbers there than the table above. At the limit about three
quarters of the server's CPU is the kernel's TCP work (in this setup it also carries part of the
clients' receive path), so the user-space cost is no longer what decides it.

What the process layout costs, same load, interleaved runs:

| Users | Layout | Server CPU |
|---:|---|---:|
| 55,000 | one process | 37–38 % |
| 55,000 | two processes, hashed | 45–47 % |
| 55,000 | steered (one process serves) | 40 % |
| 80,000 | two processes, hashed | 53–55 % |
| 80,000 | three processes, hashed | 68 % |
| 120,000 | two processes, hashed | fail: fd budget full, 19k reconnects, p95 575 ms |
| 120,000 | steered | 68 %, pass |

## Tried and rejected (measured, not guessed)

| Idea | Result |
|---|---|
| One process | Capped near 59k sockets by `LimitNOFILE`; beyond that every request reconnects |
| Two or three processes with plain `SO_REUSEPORT` hashing | Every process serves traffic: +8–14 points CPU, and two run out of fds before 120k |
| Starting the next process only when one fills | The filling process has no fds left to fork with (`pipe2: too many open files`) |
| Switching to the next process on the housekeeping tick | A burst pushed a process hundreds of sockets past its mark; now checked on every accept |
| Group commit holding the write lock from the first write to the end of the loop pass | p99 ~470 ms: the other process waited for the whole pass |
| `PASSIVE` checkpoints | WAL never rewound, 800 MB after a minute |
| `locking_mode=EXCLUSIVE` (one process, checkpoint in the loop) | -3 points CPU, but every fsync landed in the event loop and p99 rose |
| Committing at most every 10 / 25 ms (fewer page-cache resets in the other processes) | CPU within noise (54 → 50/51 %), p95 0.8 → 6/16 ms |
| SQLite `-O3`, `SQLITE_THREADSAFE=0` | `/feed` 12.4–13.5 µs vs 12.5–12.9 µs: noise |
| gzip | k6 creates its transport with `DisableCompression: true` and sends no `Accept-Encoding` |
| Handing over at ~61k instead of ~56.5k (one process up to 61k) | On x86 at 60k: 94 → 83–85 % CPU, but at 120k on the M2 p95 went 29–419 ms → 575–796 ms in two A/B runs |
| Coalescing wakeups (a 200 µs pause before `epoll_wait` when a pass found few events) | Same CPU as without it in an A/B on x86; and where the timer is coarse the pause becomes 1 ms (measured in Docker on macOS) and caps the loop: 120k collapsed |
