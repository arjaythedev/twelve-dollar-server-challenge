# C / io_uring / SQLite â€” curvynes

A direct HTTP/1.1 implementation of the fixed feed API, by
[curvynes](https://github.com/curvynes).

## Stack and startup

- C11, built with the Ubuntu 24.04 C toolchain.
- SQLite **3.54.1**, using the unmodified upstream amalgamation. The native
  read binding is compiled in the same translation unit; writes use SQL.
- liburing **2.6** and yyjson **0.10.0**, built from pinned source.
- Ubuntu's OpenSSL **3.0.13** development package, including security updates.
- Dependency URLs and SHA-256 hashes are recorded in `deps.lock`.

**Direct mode:** use `HOST=0.0.0.0 PORT=80` with the benchmark's bind capability.
Linux 6.8 or newer with io_uring enabled is required. The application runs as
an ordinary user and changes no resource limits, CPU affinity or kernel settings.

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=127.0.0.1 PORT=3000 bash start.sh
```

Only `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT` configure the running server.
The database and secret are required; host and port default to `127.0.0.1:3000`.
The build uses one compiler/LTO worker to fit the 2-GB machine. Dependencies are
downloaded and verified during the build; application and dependency binaries
are not included in the submission. Build options `CC` and `SANITIZE=1` are for
developer validation.

## Network processing

Eight io_uring instances run on one event/API thread. Each owns a `SO_REUSEPORT`
listener and a sparse direct-descriptor table. At the existing 65,535 file limit,
each table has 65,500 slots. The application reserves ordinary descriptors for
listeners, rings, epoll and SQLite, and never raises the limit.

Multishot accept and receive reduce repeated submissions. Each ring has 4,096
shared receive buffers of 2 KiB; idle connections keep compact metadata.
Request and response buffers live only while needed. Generation tags identify
stale completions after a direct slot is reused. Partial sends, fragmented and
chunked requests, pipelining, half-close and connection-close EOF are supported.

The normal receive path uses provided-buffer rings. Some Ubuntu 6.8 updates
reject their registration; the compatibility path uses provided-buffer SQEs and
the same shared pool. Successful buffer returns skip CQEs. A bounded FIFO has
one retry node per connection, and returning a buffer grants at most one retry.
Closing a connection removes its queued retry. No kernel change is needed.

Each pass handles up to 512 CQEs per ring. Requests share bounded SQLite
transactions, and pending sends are submitted before checkpoint maintenance.
Small busy batches can coalesce for at most one millisecond after submission;
large ready batches proceed immediately. Scheduling depends on queue activity.
Shutdown quiesces outstanding I/O before releasing its memory and descriptors.

## SQLite and correctness

The supplied `SQLITE_PATH` is opened in process. The schema and indexes remain
unchanged. Each feed reads the actual top 20 ids with SQL's required timestamp/id
ordering. For a bounded id span, the C binding reads post and author rows and
counts likes through SQLite B-tree cursors over the existing tables and index.
Unsupported cases use the reference SQL. Single-post reads use the same binding.

The binding keeps root-page, collation and schema-version metadata as a query
plan. It opens/closes read cursors and frees scratch data per request. Current
ids are checked for monotonic order before using cursor steps; arbitrary order
uses point seeks. All reads use SQLite's pager, WAL snapshot and locking. The
SQLite source is unchanged, and the binding checks the pinned version.

There are no response caches, query-result caches, copied tables or remembered
JWT verification results. SQLite's own 96-MiB page cache, prepared statements
and up to 512 MiB of SQLite-managed mmap are used. mmap is a file mapping, not
a resident table copy. Every authenticated request verifies HS256 and expiry;
only the secret's token-independent HMAC key schedule is reused.

Writes use prepared SQL and WAL with `synchronous=NORMAL`. A batch holds at most
1,024 database-dependent responses. Writes and reads that observed staged writes
wait for successful COMMIT before being sent. Commit failure replaces the staged
responses with HTTP 500 and rolls back. JSON input, Unicode trimming, the body
limit, response key order and exact errors follow the API specification.

A checkpoint child is forked before SQLite/network initialization and opens its
own connection with a 2-MiB SQLite page cache. Each process uses SQLite from one
thread with independent library state (`SQLITE_THREADSAFE=0`). The helper does
PASSIVE WAL backfill, then hands final RESTART maintenance to a transaction
boundary in the parent. Both processes share the application's resource limits.
The helper exits when its parent dies; there is no restart loop or extra service.

## Validation

After building, run the official suite and the included checks:

```bash
bash test/run.sh submissions/c-uring-curvynes
python3 submissions/c-uring-curvynes/verify.py
bash submissions/c-uring-curvynes/verify-extra.sh
```

The extra checks require Python 3 and a generated seed. They operate on their
own database copies. They cover JWT failures, Unicode/escaping, committed
external updates, timestamp ties and gaps, duplicate likes, HTTP fragmentation,
pipelining, chunking, half-close, ambiguous framing and database integrity.
The receive burst queues 16,384 requests with 2,048 abandoned peers while its own
test child is paused, then checks recovery. A separate test lowers only its
child's descriptor limit to 256 and holds 1,280 connections across ring boundaries.

The C tests cover staged read visibility, commit/rollback failure and recovery.
A differential test requires native-plan success for 20,000 random post reads
and 300 descending, ascending and shuffled groups, comparing every result with
public SQL. It also checks missing records, range fallback and overflow pages.
The test files are never linked into the server and add no testing endpoints.

For sanitizer builds, use `CC=clang-18 SANITIZE=1` with the official build/test
command, then run the extra checks with matching options. GCC sanitizers can
also be used. No sanitizer suppressions or SQLite patches are required.

Local validation: **200,000 users for 300 seconds**, after a two-minute 1,000-user k6 warm-up and a 60-second ramp.

| Hold req/s | p95 | p99 | Errors | App CPU avg | Peak app cgroup RAM |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 21,138 | 75.8 ms | 197.1 ms | 0% | 60.78% | 653.48 MiB |

All 30 ten-second windows passed (worst p95 488.3 ms, worst p99 747.2 ms). There were 6,341,387 requests, exactly 200,000 established connections, zero dial/check failures and no OOM events. Monitor continuity and the measured runtime's source hashes were checked.

This is a local QEMU/KVM Ubuntu 24.04 / Linux 6.8.0-147 simulation on a Ryzen AI 9 HX PRO 370 host: one guest vCPU, 2 GiB total RAM, no swap, with the load generator outside the guest. The application cgroup was limited to one CPU and 1,750 MiB. The RAM column is the application cgroup peak; the VM's total limit also covers its OS and kernel. The client is a Go approximation of `bench/load.js`, and req/s and percentiles cover the hold. Official organizer/Xeon benchmarking remains pending; this is not a leaderboard score claim.

## Attribution and license

This entry uses the repository's MIT license. Downloaded dependency sources
retain their upstream notices.

The authorized `c-iouring-unorig-username` README informed the bounded range
query plan and commit barrier; its implementation files were not read.
Selected `lean-derive-hargup` runtime/generated C files were reviewed at
`4fa5858bd2ab93db9806441b55798b8da3c320c4` to inform the SQLite read binding.
Selected PR #210 transport/protocol files were reviewed at
`15e32715fdb99d8dc6585dcbf3cdcd8286ae89ee`. This implementation retains upstream
SQLite for its pager, WAL, transactions and data operations.
