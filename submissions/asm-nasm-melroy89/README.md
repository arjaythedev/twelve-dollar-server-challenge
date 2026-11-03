# NASM / Linux x86-64 / SQLite

|                       |                                                       |
| --------------------- | ----------------------------------------------------- |
| Language              | NASM assembly for Linux x86-64                        |
| HTTP server           | Direct HTTP; no Nginx or application framework        |
| Database              | SQLite 3.54.0, compiled from checksum-verified source |
| I/O                   | Raw Linux syscalls; io_uring with epoll fallback      |
| Application libraries | None for HTTP, JSON, crypto or io_uring               |
| C runtime             | Required by SQLite                                    |

The server is written in NASM; SQLite retains its required system C runtime.

## Build and run

On clean Ubuntu 24.04 x86-64:

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=your-secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Installation runs as root; build/run as a normal user. Port 80 requires the
organizer's CAP_NET_BIND_SERVICE. Configuration uses only those four variables.
The executable runs in the foreground. Source-only submission; generated files
are ignored. NASM 2.16.01 is package-pinned; the compiler is Ubuntu GCC, recorded
in build/identity.json.

For a non-PGO diagnostic build:

```bash
bash build.sh diagnostic
```

Release builds use synthetic file-database PGO, LTO and native CPU optimization.
The training driver is assembly and is not linked into the server. It never
reads challenge seed data.

The explicit omission options are SQLITE_OMIT_LOAD_EXTENSION,
SQLITE_OMIT_DEPRECATED, SQLITE_OMIT_SHARED_CACHE and
SQLITE_OMIT_PROGRESS_CALLBACK: the application uses none
of those facilities. SQLITE_THREADSAFE=0 is valid only for this single SQLite
owner architecture; reconsider it before adding database threads.
SQLITE_DEFAULT_MEMSTATUS=0 disables allocator accounting, SQLITE_DQS=0 rejects
legacy double-quoted SQL strings, and HAVE_FDATASYNC=1 selects the available
Linux synchronization primitive. Foreign-key enforcement and JSON support are
retained. O3/native/LTO and synthetic PGO are recorded in build/identity.json;
the non-PGO diagnostic mode remains reproducible. No build/version comparison
campaign is required or claimed.

## Design

- Raw io_uring when available, epoll fallback; direct Linux syscalls.
- One owner thread, up to eight sparse registered-file tables. Capacity rings
  activate as the current table fills. Bounded accept credits avoid dropping
  a newly accepted socket at fixed-table exhaustion.
- Shared provided receive buffers, generation-tagged completions, stable send
  storage and bounded reusable input/output slabs.
- Startup explicitly enables and verifies WAL, synchronous=NORMAL and
  foreign_keys=ON. Production SQL statements are prepared once at startup.
- Reference SQL with NASM JSON serialization is the current candidate default.
  Experimental SQL-generated/hybrid JSON and selected-ID/range queries preserve
  membership and timestamp/ID ordering.
- Every token signature is verified in assembly, accepting HS256 only and
  validating expiration/payload types. No data or JWT verification cache.
- Frozen completion batches share a transaction, collected before BEGIN. Writes
  use SQLite statement rollback for ordinary constraint failures, verifying the
  transaction remains active. Unexpected failures invalidate the whole batch;
  successful writes and dependent reads wait for COMMIT. No future requests are awaited with a transaction open.
- Automatic checkpoints are disabled; a WAL hook schedules passive checkpoints
  outside transactions after 1,000 frames. They can still stall the owner loop.
  WAL/NORMAL meets the challenge rule, not a power-loss durability guarantee.
- Idle timeout 120 seconds; stalled input/send timeout 30 seconds.
- Each admitted upload reserves its full bounded input and response capacity.
  Existing uploads therefore need no additional buffer allocation to finish
  when admission is exhausted. Pending sends continue draining.

Input buffers are 32 KiB (28 MiB admission budget, 32 MiB slab capacity);
response buffers are 64 KiB (64 MiB aggregate capacity). Bodies over 16 KiB are
rejected. Slabs are lazily touched and recycled; idle sockets own no buffers.
No affinity, kernel tuning, external service, schema modification or application
result cache is used.

## Diagnostic controls

These command-line controls are for comparative tests; start.sh uses defaults:

```bash
bin/server --epoll
bin/server --transaction=per-request
bin/server --transaction=writes
bin/server --transaction=batch
bin/server --query=reference --json=sql
bin/server --query=selected --json=nasm
bin/server --query=range --json=hybrid
bin/server --query=scan-ranges
bin/server --savepoints=on
bin/server --literal-copy=inline
bin/server --recv=multi --batch=32 --wait-us=250
bin/server --rings=1 --nodelay=on
```

Candidate defaults: shared frozen-batch transactions, reference SQL, NASM JSON,
one-shot provided receives, 32-completion/250-us bounded coalescing, eight available
rings, TCP_NODELAY off, send-first completion processing and deferred ENOBUFS
recovery, with redundant write savepoints disabled. The earlier savepoint-on
configuration passed a local 250k-user five-minute hold. The current defaults
passed a local 275k-user five-minute hold with zero HTTP/check failures and
p99 65.4 ms. See PERFORMANCE.md for exact identities and measurement limits.

The send-first path prioritises at most 64 send/close completions per frozen batch,
leaving receive work a share of its 128-completion budget. Deferred recovery uses
a bounded generation-tagged FIFO and rearms at most 128 entries after the ring
completion backlog drains, outside SQLite transactions. The eight retry queues
add four MiB of lazily touched storage beyond the connection records. Use
--completion-order=fifo and --enobuf=immediate for the original controls.
Scalar JSON quoting remains the default; --quote=vector enables the SSE2
experiment. Its guard-page/ABI/protocol checks passed, but the short throughput
gain was small and noisy. Individual contributions and the best final defaults
remain under investigation. A multishot receive screen improved saturation CPU
throughput but failed the 275k p99 threshold; one-shot remains the default.
The bounded three-query range experiment was slower and remains opt-in.
`--literal-copy=inline` selects fixed-width NASM copies for known response
fragments; scalar remains the default because unchanged-k6 screens did not show
a repeatable gain. See `research/literal-copy-results.md` for the measurements.

## Validation

From the repository root:

```bash
bash test/run.sh submissions/asm-nasm-melroy89
python3 submissions/asm-nasm-melroy89/tests/primitives.py
python3 submissions/asm-nasm-melroy89/tests/integration.py --idle
python3 submissions/asm-nasm-melroy89/tests/matrix.py
python3 submissions/asm-nasm-melroy89/tests/pressure.py
bash submissions/asm-nasm-melroy89/build.sh diagnostic
python3 submissions/asm-nasm-melroy89/tests/faults.py
python3 submissions/asm-nasm-melroy89/tests/capacity.py --connections 80000
```

Tests use disposable databases. Primitive tests include independent SHA/HMAC,
JSON/UTF-8 and base64url checks, guard pages, canaries and assembly ABI checks.
The matrix checks 27 configurations against the official suite and reference SQL.
C sanitizers do not automatically validate handwritten assembly.

See PERFORMANCE.md for evidence boundaries and remaining qualification.
MIT, under the repository license.
