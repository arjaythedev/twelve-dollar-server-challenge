# Zig + io_uring fibers + SQLite

| | |
|---|---|
| Language | Zig 0.17.0 |
| Framework | None. HTTP/1.1, JWT and JSON are hand-written in Zig. |
| Database | SQLite 3.54.1 amalgamation, compiled in. It is the only C code. |
| Other dependencies | [zig-patch](https://github.com/marler8997/zig-patch) and [zig-staticio](https://github.com/marler8997/zig-staticio), build-time only (see below) |
| Nginx or direct | **Direct** on `HOST:PORT` (`0.0.0.0:80`) |

`install.sh` installs nothing beyond curl. `build.sh` downloads anyzig into this
folder as `./zig`, which fetches Zig 0.17.0 and the pinned packages from
`build.zig.zon`, and runs `zig build`. It builds natively on the box, optimized
for speed by default.

## Why it's interesting

The server reads like ordinary blocking code. It is written against `std.Io`,
Zig's new I/O interface: `server.accept`, then one `Io.Group` task per
connection that reads a request through a `net.Stream.Reader`, handles it, and
writes the response through a `net.Stream.Writer`. Group commit waits on an
`Io.Event`. There are no callbacks, state machines or async/await.

It is still fully asynchronous. All of it runs on our own `Io` implementation,
[`src/UringIo.zig`](src/UringIo.zig): one thread, one io_uring, and a fiber
(its own small stack) for every task, so one per connection. When a fiber makes
a call that would block, `UringIo` queues the operation on the ring and switches
to the next ready fiber. The completion makes it ready again. 64,000
connections are 64,000 fibers on one thread.

The usual price of fibers is a stack per connection. Here an idle connection's
stack is packed into about 160 bytes and its pages go back to the kernel (see
below), so the whole process uses 24 MB at 64,000 connections.

Two things are specific to `UringIo`. Request handling and commits go through
`UringIo.callOnSchedulerStack`, which runs deep code that never blocks on the
scheduler's stack so fiber stacks stay shallow. With another `Io` they would be
plain calls. SQLite checkpoints run on a separate pthread.

zig-staticio patches std so that `std_options.io_vtable = .{ .custom = UringIo }`
turns every `std.Io` call into a direct call to `UringIo`, with no vtable. Only
what the server reaches is implemented, and a missing function is a compile
error. We measured the alternative, a runtime vtable over `std.Io.failing`: the
same instruction count, but about 11 MB more RSS at 64k connections. std's own io_uring
`Io` (`std.Io.Evented`) doesn't compile in 0.17.0 and has no TCP listen or accept.

## What makes it cheap

**About 300 bytes per idle connection.** A connection waits for its next
request in a read. When a fiber blocks there (after its first read), `UringIo`
"parks" it. It packs the live stack, excluding the read's destination buffer
(the kernel is about to fill that), into a small heap block, and returns the
stack's pages with `madvise(DONTNEED)`. When the fiber is next resumed, the
bytes are unpacked to the same addresses. Fiber stacks are 64 KB slots in one
`MAP_NORESERVE` mapping, so only touched pages cost memory. The red zone is
disabled and the iovec-building functions are `noinline`, so a parked stack is
about 1.1 KB raw.

[`src/stack_pack.zig`](src/stack_pack.zig) packs it to about 160 bytes. Each
8-byte word gets a 2-bit tag. Pointers into the fiber's own slot take 2 bytes
and small integers 4. A word that equals the same word of the first stack ever
parked, its "base", takes nothing. Every connection parks in the same read with
the same frames, so about 85% of words match the base: return addresses,
constants, and pointers into the fiber's own slot, which are relocated. At
64,000 connections the process uses 24 MB (4.8 MB idle).

**One page fault per request.** A connection's live stack, including its 2 KB
read buffer, fits in its slot's top page, so waking it faults in one page.
Requests that don't fit move to a 20 KB heap buffer. `std.mem.indexOfPos` with
a string needle reserves a 2 KB Boyer-Moore-Horspool table in its stack frame,
which added a second page to every request, so the HTTP parser only searches
for `'\n'` (SIMD).

**Deep work runs on the scheduler's stack.** Routing, SQLite, JWT and JSON never
suspend, so `callOnSchedulerStack` runs them on the main thread's stack. Fiber
stacks then stay within a page.

**One io_uring, one thread.** The ring uses SINGLE_ISSUER, DEFER_TASKRUN,
COOP_TASKRUN and SUBMIT_ALL. Accepts come from one multishot accept, which
re-arms after a 100 ms pause if the kernel stops it (for example when out of
file descriptors), so it never spins. Reads are RECV; a read that waits for a
connection's next request sets POLL_FIRST, skipping a recv that would find
nothing. Responses are one SEND. The scheduler runs every ready fiber, then
makes one `io_uring_enter` that submits everything and waits. When the last
wait returned completions, it waits for 32 completions or 0.5 ms, whichever
comes first, instead of one: at 64k users that cut CPU from 23.3% to 19.0% for
about 0.4 ms of latency. Idle connections are never closed.

**Our own context switch.** `std.Io.fiber.contextSwitch` declares only zmm0–31
as clobbered, which LLVM ignores on CPUs without AVX-512, so values kept in
ymm registers across a switch were corrupted. `switchContext` declares the
vector registers the target actually has.

**Group commit.** The first write opens `BEGIN IMMEDIATE`, every write that
becomes ready in the same scheduler pass joins it, and a committer task commits
once. Each response waits for its own commit. If the commit fails, every
response in the batch becomes a 500, never a false 201.

**Checkpoints off the event loop.** A checkpoint fsyncs twice (79 ms measured
on NVMe), which would stall every connection. A second connection on a
`SCHED_IDLE` thread runs PASSIVE checkpoints every 1000 WAL frames. Under
constant writes the WAL never restarts on its own: the writer only rewinds it
if every frame is already checkpointed when a transaction begins. So once the
WAL reaches 32 MiB, the committer holds new writes (reads keep going) until the
checkpoint thread catches up, and the next write rewinds it. That pause costs
30–140 ms about every 6 s, for writes only. Without it the WAL grew by about
5 MB/s without bound.

**SQLite.** WAL, `synchronous=NORMAL`, `temp_store=MEMORY`, a 2 MiB page
cache, and no mmap. The `unix-excl` VFS keeps the WAL index on the heap and
takes one exclusive file lock for the process, instead of `fcntl` locks on
every transaction. The database stays in the OS page cache. A 64 MiB SQLite
cache measured the same CPU and p99 for 26 MB more RSS. Statements are prepared
once (`SQLITE_PREPARE_PERSISTENT`). The build uses SQLITE_THREADSAFE=2,
DEFAULT_MEMSTATUS=0, DQS=0, OMIT_{DEPRECATED, SHARED_CACHE, LOAD_EXTENSION,
PROGRESS_CALLBACK, DECLTYPE, AUTOINIT} and USE_ALLOCA.

**Queries.**
- **Feed:** the top 20 ids, then two range scans over that id span (like counts
  into an array, rows with authors) instead of 20 correlated `count(*)`
  lookups. It falls back to the reference query if the ids span 256 or more,
  or if 20 long posts don't fit the row buffer.
- **Like:** a single `INSERT … SELECT … WHERE EXISTS … ON CONFLICT DO NOTHING`.
- **Create:** `INSERT … RETURNING`.

**Requests and responses.** A small HTTP/1.1 parser looks at only the headers
it needs (`StaticStringMap`, case-insensitive), and supports `100-continue`.
Status lines are prebuilt, and the head is written in front of the body, so
each response is one send. JWT verification copies an HMAC state keyed once
with the secret, then checks every signature. JSON input is parsed straight
into a declared struct, such as `struct { body: ?[]const u8 }`: invalid JSON
fails the parse, and a wrong shape leaves the field null ("body is required").
JSON string escaping scans a SIMD vector at a time.

There are no response, query or JWT caches: every request reads SQLite.

## Measurements

These are local and unofficial: a Ryzen 9 5950X, Linux 7.0, with the server
pinned to one core (`taskset -c 0`). The load comes from our own load
generator on another core, not k6. Results on the droplet will differ.

64,000 virtual users, 60 s ramp, 120 s hold, following `bench/load.js`:

| | |
|---|---|
| Requests | 812,784 in the hold (~6,770/s), 0 failed |
| Latency | p50 0.4 ms, p95 0.7 ms, p99 1.3 ms, max 127 ms |
| Writes | p99 ~39 ms (likes and creates wait for their group commit and the occasional WAL drain) |
| Server CPU | 16.1% of one core over the hold |
| Server RSS | 24.2 MB peak, flat during the hold |

## Load generator

`zig build loadgen` builds `zig-out/bin/loadgen`, a load generator that follows
`bench/load.js`. It runs one fiber per virtual user, on the same `UringIo`.
k6 needs about 300 KB per virtual user; this needs a few KB, so one machine can
hold 64k+ keep-alive connections. It spreads connections over several
`127.0.0.x` addresses to get past the ephemeral port range, so the server must
listen on `0.0.0.0`.

```bash
SQLITE_PATH=/tmp/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3200 taskset -c 0 zig-out/bin/server &
taskset -c 4 zig-out/bin/loadgen --vus 64000 --port 3200 --ramp 60 --duration 120 --pid $!
```

It prints a table every `--interval` seconds (request rate, p50/p99/max, and
the server's CPU and RSS from `--pid`), then a summary of the hold per endpoint.

## Tests

`zig build test` runs the unit tests. `bash test/run.sh submissions/zig-iouring-marler8997`
passes all 42 checks.
