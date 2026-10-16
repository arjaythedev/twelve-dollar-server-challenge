# Rust / Axum submission

The five challenge endpoints use a native Hyper service with Axum request
extraction, a single-threaded Tokio event loop, and one dedicated SQLite worker.
All listening ports share the same database and application state. The design
keeps idle connections small and avoids allocating temporary objects for each
request.

## Keeping open connections small

Most users spend time thinking between requests. Retaining a complete HTTP
parser, buffers and task for each of those sockets exhausted the memory budget.
On Linux, idle connections instead keep a standard TCP socket and service handle
in a shared map. One epoll instance reports activity through a single Tokio
registration; the server does not scan all idle sockets.

The [Hyper patches](https://github.com/hyperium/hyper/compare/master...mjc:hyper:benchmark-patches)
can finish a response and return the still-open socket without retaining the
HTTP session. Partial requests, pipelined bytes, pending handlers and unfinished
writes stay active. Once that work has finished, the server drops the HTTP state
and returns the socket to idle storage. Keep-alive connections remain open.

The server permits at most 8,192 active HTTP sessions. Each has its own Tokio
task, giving it an independent scheduling budget. Readiness batches contain at
most 1,024 events, and capacity is checked before consuming another batch.
Connections start idle and acquire HTTP state only when data arrives.

Each socket's edge-triggered epoll watch stays installed between requests,
avoiding a rearm syscall on every return to idle. Tokens are never reused, so a
stale event cannot select a replacement connection that reused a file descriptor.
Notifications received during active work are remembered. Before parking again,
a nonblocking peek distinguishes unread data, a closed peer and an empty socket.
This prevents a stale notification from retaining an idle HTTP session or losing
an arriving request.

A socket is registered with Tokio only when a read or write actually has to wait.
Waiting for a database reply alone does not require that registration. After
registration, HTTP is polled again immediately to avoid missing a notification.
The adapter adds no second transport buffer. Idle and active maps release their
storage after all connections drain.

## Avoiding temporary objects and copies

Here, borrowing means using existing text while its owner still holds it,
instead of allocating and copying another string.

| Step | What changes, and why |
| --- | --- |
| Routing | The five routes are matched directly, avoiding the general Axum router and the adapter between Tower services and Hyper. Paths are borrowed; percent-encoded post IDs are decoded and checked in one pass without creating a decoded string. |
| Handler state | Dispatch returns its existing asynchronous result directly, avoiding another wrapper that must retain the handler's state. Hyper allocates space for the handler only when a request arrives and releases it on completion. |
| HTTP bodies | Requests retain Hyper's concrete body type. Only post creation uses Axum's body extractor and size limit. Responses use a single byte buffer, avoiding the allocation needed for a generic response-body wrapper. |
| Headers | After authentication, request headers are cleared and their allocated table is reused for response headers. The empty table returns to Hyper's pool after encoding. |
| Post input | Unescaped JSON text borrows the received bytes; trimming returns a slice. Escaped text reuses the decoder's allocation. Accepted requests are parsed once, without first constructing a general JSON object. Rejected type errors get the additional validation needed to preserve API error ordering. |
| Authentication | The decoder reads the required claims without first constructing a general JSON object. Likes validate the username without keeping a copied string; creation keeps it because it appears in the response. Signatures are checked on every request. |
| Post and feed JSON | Text fields borrow SQLite row values. Fixed keys and punctuation are written directly; serde_json encodes the values and handles escaping. Each row is serialized before advancing the query, avoiding a list of post objects and separate copies of their text fields. |
| Output sizing | Feed, post and create responses remember the previous buffer capacity as a size hint. Each response owns new bytes; the hint avoids repeated growth without caching response data. |
| HTTP output | The finished vector transfers ownership of its allocation to the response without copying. Headers and body remain separate buffers for vectored writes. Hyper's body queue initially reserves just one entry for these single-buffer responses. |
| Feed assembly | Fixed-size arrays track selected IDs, like counts and serialized row offsets, avoiding a heap-allocated lookup map and list of post objects. Serialized segments are copied into their final order. |
| Batch replies | Completion arrays are reused, and processing visits only the entries filled by the current batch. |
| Queue admission | The worker queue reserves a slot before creating a reply channel. Canceled requests are skipped before SQL starts; a fully canceled write batch does not begin a transaction. |

The Hyper patches also reduce allocations inside HTTP itself:

- Adaptive reads start at 1 KiB rather than 8 KiB and can grow for larger input.
  An empty small read first uses a 1 KiB stack buffer; heap storage is acquired
  only when bytes arrive. The configured minimum and default maximum remain
  8 KiB and 417,792 bytes; request-size limits are unchanged.
- Response-header byte storage is allocated on first use. Read buffers,
  write-header buffers and header tables return to bounded per-thread pools.
  Each pool retains at most 128 items; buffers above 8 KiB and header tables
  with capacity above 32 are discarded.
- Read-buffer growth is tracked before splitting the buffer. A small slice at
  the end of a large allocation cannot keep that whole allocation in the pool.
- Server-only builds remove Hyper's per-connection header-cache field. The
  shared pools last for the runtime's lifetime, including periods when every
  HTTP session is parked.
- Linux vectored writes send existing header and body buffers together without
  copying them into a combined buffer. Partial and pending writes retain the
  original bytes. Other platforms use scalar writes; pipeline flushing is
  disabled. The separate performance effect of vectored writes is unmeasured.

Rust allocations use jemalloc with background decay after heap inspection found
freed allocations still occupying resident memory. SQLite's C allocations keep
the system allocator. The
[Tokio patch](https://github.com/tokio-rs/tokio/compare/master...mjc:tokio:benchmark-patches)
reserves 64 KiB for the current-thread runtime's ready queue at startup, before
connection allocation churn. That queue can still grow.

## Sharing database setup and commit costs

A bounded queue keeps SQLite work off the HTTP event loop. It holds up to 4,096
jobs. The worker collects up to 1,024 requests, stopping when full or 1 ms after
the first request. The deadline does not restart for each arrival. Waiting uses
park/unpark on the existing SQL thread, without another asynchronous runtime.

Consecutive reads share one read transaction, ending at the next write. Each
request still executes its own queries and gets its own response. A lone read
avoids the batch transaction setup where possible. Feed queries retain a
transaction because their multiple queries need a consistent snapshot.

A write begins a mixed batch that preserves queue order. Reads in that batch see
preceding writes. Per-write savepoints isolate failed writes, and all replies
wait for the group commit. A failed commit rolls back before replies; if cleanup
leaves a transaction open, the worker closes the connection. Canceling a reply
after SQL executes does not undo the write. Read transactions finish before
responses are sent, so a slow client does not keep a database snapshot open.

Requests wait up to one second for a queue slot before receiving HTTP 503
"database busy". This is a queue-admission timeout, not SQLite's lock error.
Execution adds to that wait, so it is an overload cutoff rather than a latency
guarantee. It does not cancel work already queued.

Queries and transaction-control statements are prepared once, including BEGIN,
COMMIT and savepoint operations. `SQLITE_PREPARE_PERSISTENT` keeps these long-lived
statements out of SQLite's small-allocation lookaside reserve. Each connection
has one owning thread and is opened without a per-connection mutex. SQLite
retains thread support; the build disables optional global page-cache memory
management to avoid its shared cache-group mutex. Private cache groups may use
more memory.

Post creation uses INSERT RETURNING for the ID and timestamp and reuses the
validated request body and authenticated username in the response, avoiding a
second read of the new row. A successful like relies on SQLite's foreign-key
check rather than first querying whether the post exists. Duplicates and
foreign-key failures take the extra lookup needed to preserve the API's errors.

Each feed request finds the latest 20 post IDs in timestamp and ID order. If
their checked ID span is below 256, prepared range queries count likes and read
joined posts through the existing indexes. Fixed arrays restore the selected
order. Sparse spans, arithmetic overflow and missing authors fall back to the
original query, whose limit follows the join. The range path serializes into
scratch storage, then copies row segments into the response; it is not an
end-to-end zero-copy path. Scratch contents are cleared after each request,
retaining at most 64 KiB of capacity. No post, token or query-result cache is used.
The supplied schema and indexes are unchanged. The default SQLite page-cache
target is 32 MiB, leaving more of the budget for kernel sockets and request bursts;
mmap is disabled.

## Avoiding storage stalls during traffic

SQLite's automatic WAL checkpoint threshold is disabled. A separate connection
checks once per second and starts a PASSIVE checkpoint only after the SQL worker
has done no batch work for a full interval. When it has copied all committed WAL
frames, it skips further checkpoints until new work arrives. New requests may
still arrive during a checkpoint, and the WAL can grow during continuous load.

With `journal_size_limit=0`, SQLite discards the unused tail on the first commit
after a checkpointed WAL resets. Later writes extend the file, avoiding reads of
cold old blocks needed for partial-page overwrites. Physical file size alone no
longer triggers repeated checkpoints after the log has logically reset.

On Unix, both connections use SQLite's
[unix-excl filesystem backend](https://www.sqlite.org/vfs.html). The temporary
index used to find pages in the WAL lives in process memory, avoiding filesystem
write faults in the `-shm` file. The server needs exclusive access to the database.
The database and WAL remain on disk with NORMAL synchronization, and successful
write responses still wait for commit.

Dependencies: Rust stable, Axum 0.8.9, Mio 1.2.4, rusqlite 0.40.2 with bundled SQLite,
jsonwebtoken 11.1.0, and Mold 3.0.0.

## Run

On the Ubuntu challenge image, run `sudo bash install.sh` once, then:

```sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

`SQLITE_PATH` must name an existing database. `JWT_SECRET` is required. The
local defaults are `HOST=127.0.0.1` and `PORT=3000`. The foreground server handles
SIGINT and SIGTERM. Accepted sockets enable TCP_NODELAY through Tokio's safe API.
Set `HOST=0.0.0.0 PORT=3100-3105` to listen on all six ports in one process.
`HOST` also accepts comma-separated IPv4/IPv6 addresses and inclusive IPv4
ranges, such as `127.0.0.[1-5]` or `127.0.0.1-127.0.0.5`. `PORT` accepts
comma-separated ports and inclusive ranges. Every requested IP/port combination
is bound, with duplicates removed in input order. IPv6 addresses can be listed
individually. Expansion is limited to 65,536 socket addresses.
All listeners must bind successfully before serving requests. They share one
database worker, application state, and graceful shutdown. HTTP/1.1 connection
reuse follows the request's normal `Connection` semantics; the server does not
retire keep-alive connections based on connection counts or descriptor limits.

The profiling tasks can request a range with `BENCH_PORT=3100` and
`BENCH_PORT_END=3105` in the private benchmark config. The load generator and
health probe use the first port. A single `BENCH_PORT` keeps one listener.

`READ_BATCHING` accepts `true` or `false` and defaults to `true`.
`POST /posts` accepts at most 64 KiB of request-body bytes, including JSON
whitespace and extra fields, and returns HTTP 413 above that limit. The decoded,
trimmed post text still has its separate 500-character limit.
`SQLITE_MMAP_SIZE` sets the request connection's mmap size in bytes and defaults
to `0`; startup reports both the requested and effective sizes.
`SQLITE_PAGE_CACHE_KIB` sets the request connection's page-cache target in KiB
and defaults to `32768` (32 MiB). Set it to `65536` for a 64 MiB target.
It must be positive, fit a signed 32-bit KiB value, and cover between one and
one billion database pages. Startup reports the selected target.
`SQLITE_TIMINGS=true` records queue waits and timeouts, batch sizes and durations,
slow commits, and checkpoint timing. It is disabled by default.

## Development

Use an up-to-date Rust stable with rustfmt and Clippy. Local shell setup and
profiling orchestration are maintained outside this submission in the shared
development configuration.

```sh
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo nextest run --locked
mkdir -p target/test-runs
TMPDIR="$PWD/target/test-runs" bash ../../test/run.sh .
```

## Benchmarking and profiling

For load at these connection counts, use
[mjc/twelve-dollar-benchmark](https://github.com/mjc/twelve-dollar-benchmark)
or distribute the workload across many parallel k6 instances. The generator
needs enough CPU and memory to keep up and runs separately from the constrained
server; otherwise its own resource limit can end the run first.

With the standalone benchmark repository checked out at
`../twelve-dollar-benchmark`, run its existing launcher from the challenge
repository root. Set the token file and both result destinations explicitly:

```sh
TOKENS="$PWD/seed/tokens.json" \
SUMMARY="$PWD/results/benchmark-summary.json" \
  ../twelve-dollar-benchmark/run.sh 127.0.0.1 3000 --users 1000 \
  --output-dir "$PWD/results/run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
```

Run the load generator on a separate machine for capacity measurements.
For CPU or heap profiling, `cargo build --locked --profile profiling --jobs 1`
retains full DWARF symbols with the release optimization settings. Rust and
bundled SQLite preserve frame pointers. Run perf, heaptrack, or Pyroclast against
that executable; deployment and recording scripts live in the shared devshell.

### Ubuntu VM result

The highest-throughput passing run used Ubuntu 24.04 in a VM with one vCPU,
2 GiB RAM, no swap, ten listeners, a fresh supplied seed, a 32 MiB SQLite cache,
and mmap disabled.
The generator ran separately. The run used a 60-second ramp, 60-second nominal
user hold, 30-second drain, and 10-second grace period, with CPU and whole-VM
resource recording. Initial think time delays reaching the full TCP count.

| Users | Hold requests/s | Requests | Errors | p95 / p99 | Hold app CPU | Hold whole-VM CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 240,000 | 25,333 | 2,849,999 | 0 | 478.15 / 618.25 ms | 78.38% | 85.54% |

It completed ramp, hold, and drain and passed the latency and error thresholds.
At 240,000 users, hold throughput was 25,333 requests/s, sampled peak app RSS was
129.68 MiB, and peak whole-VM used memory was 1,274.10 MiB, measured as
`MemTotal - MemAvailable`. Whole-VM CPU and memory include the OS and observer.
Most of this memory is in the kernel, which tracks the open sockets and
connections. At the peak whole-VM used-memory sample, unreclaimable kernel
slab (kernel object storage that cannot be reclaimed while in use) alone
occupied 770.06 MiB; app RSS at that same sample was 128.55 MiB.
This kernel counter covers the whole guest; individual kernel object types
were not attributed separately.

The 240,000-user capture added request-phase counters to the generator and
enabled server timing logs on tmpfs. Normal runs with the same server had failed
latency thresholds. This passing capture does not establish a repeatable
240,000-user limit or show that intermittent queue stalls are fixed. Its slow
responses clustered near the ramp-to-hold transition; identifying the blocked
SQL-worker operation remains unfinished.

This measurement precedes the switch to pinned dependency forks and the
Tokio 1.53.3 / serde_json 1.0.154 update; the updated build has not been
capacity-benchmarked. This one-minute hold does not establish the five-minute
contest result.

### Host systemd results

The host systemd service was set up to explore the difference between service
resource limits and running the server inside a VM. The service had a configured
one-core CPU quota and a 2,147,483,648-byte memory limit. Its memory budget
included the server, kernel socket memory and file cache; the host OS remained
outside it. The load generator ran on a separate machine.

The highest user count that passed the latency and error thresholds in this
setup was 500,000, with two completed runs of the same frozen executable:

| Run | Requests | Errors | Hold requests/s | p95 / p99 | Peak app RSS | Hold service CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| First 500k pass | 5,966,831 | 0 | 52,670 | 48.82 / 63.83 ms | 99.76 MiB | 120.66% |
| Profiled 500k repeat | 5,964,750 | 0 | 52,678 | 51.42 / 462.23 ms | 108.67 MiB | 119.97% |

Both used ten listeners, a 32 MiB SQLite cache, mmap disabled, the 8,192-active
connection limit, and CPU/memory/I/O recording. The executable SHA-256 was
`9f573cd70ee792cddc9dad80b0d51db4d4ada302022cc1977a93d75f114759bb`.
The preserved Rust generator and observer hashes started with `5adb671f` and
`27bfb0bc`. Workload starts were 2026-10-15 16:40:02.584778443 UTC and
16:57:45.221453242 UTC, respectively.

Each run completed a 60-second ramp, 60-second nominal user hold, 30-second
drain and 10-second grace period. The generator sampled all 500,000 TCP
connections live over spans of 53.466 and 51.560 seconds during those holds.
Initial user think time delays the full connection count; these are not
60-second measurements with every socket live throughout. The profiled repeat's
sampled cgroup memory peaked at 2,147,479,552 bytes during hold, including kernel
memory and file cache. App RSS is only the process portion of that budget.

CPU percentages are relative to one core. Despite `cpu.max=100000 100000`, the
service used about 1.2 cores during hold. Quota enforcement was not established
under the host scheduler. A later 510,000-user attempt reached that count but
was OOM-killed during drain and failed the error threshold.

The systemd and VM runs show the two setups' observed results, but do not isolate
VM overhead: server revisions, recording and the scope of the resource limits
also differed. They precede the dependency refresh and do not establish a
five-minute contest result inside a one-vCPU, whole-system 2 GB VM.
