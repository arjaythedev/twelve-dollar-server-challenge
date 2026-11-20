# Ideas from PR #158 (`c-iouring-public`)

Status: W1 (WAL write buffer) and A1 (AVX2 escaping) retained; P1, K1 rejected; four ideas
not adopted after profiling; four excluded as benchmark-specific. Source baseline: `b789071`
(production C identical to `2eae17a`).

## Scope

PR #158 lists SIMD HTTP parsing, batched multi-buffer JWT verification, a WAL
write-coalescing VFS, a pcache patch, page-cache reads with mmap off, large bounded
checkpoints, drain-first receive, and memory-pressure shedding. Only general mechanisms were
considered. Anything trained on, ordered by, or tuned to the benchmark's traffic, VM size or
pass/fail thresholds was excluded: PGO trained on `load.js`'s mix, parser templates ordered
by a load-generator profile, 503s with an error budget sized to the 1 % threshold, and
memory floors tuned to the 2 GB box.

## Where the time goes

`perf record` on the server CPU, 300,000 users (60 s ramp, 60 s hold), production build:

| Share of CPU 0 | What |
|---:|---|
| 58 % | kernel, mostly TCP; over loopback part of it is the client's receive work |
| 31 % | the server process: B-tree seeks and page acquisition ~13 %, WAL `pwrite` 5.4 %, JSON output 3.7 %, HTTP parsing 0.5 %, SHA-256 (JWT) 0.5 % |
| 8 % | the checkpoint thread (included in the server share above), mostly block-layer writeback |
| 10 % | idle |

Only `POST /posts` and `POST /posts/:id/like` carry a token, so batched JWT verification can
save at most ~0.5 %, and a faster HTTP parser about as much. Neither was implemented.
`pcache1TruncateUnsafe`, which PR #158's SQLite patch targets, was 0.04 % with this server's
1 MiB cache. Receive buffers are already parsed straight out of provided buffers, so
drain-first has no backlog to move here.

## Experiments

Closed-loop numbers: 4 threads × 32 connections, the mixed workload, 30 s, fresh seed, one
server CPU, 2 GiB, no swap. 300k numbers: the native `load.js` replay, hold-window CPU per
request. Both run on bigmachine, interleaved.

### P1 page-cache reads (mmap off, 8 or 16 MiB cache): rejected

PR #158 reads through SQLite's page cache. Here, at 300k users, hold CPU per request:
baseline 14.52 / 15.66 µs, 8 MiB 14.75 / 14.36 µs, 16 MiB 15.19 / 16.16 µs. No gain beyond
the ±4 % run-to-run noise, and 16 MiB is worse. mmap reads stay.

### K1 larger checkpoint interval: rejected

PR #158 found fewer, larger checkpoints cheaper. Closed loop, µs/request: baseline 5.672,
`CKPT_FRAMES=4000` 6.039 / 6.018, `8000` 6.358 / 6.517. With mmap reads every page fetch
looks the page up in the WAL index (`walFindFrame`), whose cost grows with the WAL. A short
WAL that restarts often is cheaper here. (PR #158 reads through its page cache, which skips
that lookup on a hit.)

### W1 WAL write buffer: retained

SQLite writes each WAL frame with two `pwrite`s. The request connection's WAL is opened
through a small VFS (`runtime/db.c`) that buffers contiguous writes and flushes them when the
commit frame's page arrives, before SQLite publishes the frames in the WAL index. Every other
WAL operation flushes first, and any non-contiguous write is written through, so the file
receives SQLite's writes in SQLite's order. Database files keep the unix VFS's own methods;
the checkpoint thread's connection uses the default VFS.

A kill -9 durability test (`tests/crash_durability.py`) found a real bug in the first
version. The unix VFS reports a write of 128 KiB or more as `SQLITE_FULL` (it masks the length
with `0x1ffff`). The buffer dropped its contents on that error. When the failing flush came
from an `xRead` inside a spilling transaction, SQLite failed one statement, committed the
rest, and the WAL was left with a hole. Recovery then discarded later, acknowledged commits
(6 of 60 rounds: lost writes or a malformed image). The fix flushes in pieces below 128 KiB
and keeps the buffer on any error, and a write only advances the append position once it
succeeds. After the fix: 60 of 60 rounds with a 64 KiB cache (forcing spills, in-place
rewrites and `walRewriteChecksums`), and 20 of 20 with the production cache. The control
build passed 30 of 30 before and after.

Closed loop, µs/request: baseline 5.672 (a second baseline run, 5.975, overlapped a
compile on the sibling hyperthread), W1 5.637 / 5.609.

### A1 AVX2 JSON escaping: retained

`out_jstr` scans 32 bytes per step when the compiler targets AVX2 (`-march=native` on both
bigmachine and the scoring Xeon), then falls back to the existing 16-byte and byte loops.
A differential test in `tests/metal/output_cases.c` compares both widths with a byte-wise
reference over 200,000 strings, with escapes at every block offset, under ASan/UBSan.
Application CPU for feed requests (`tests/metal/micro.c feed 32`), interleaved: SSE2
5,454 / 5,364 / 5,406 ns, AVX2 5,248 / 5,266 / 5,300 ns (−2.5 %). End to end it is within
noise (5.635 / 5.740 µs closed loop).

## Final candidate

W1 + A1 at the source defaults (`CKPT_FRAMES` 1000, mmap reads). It passes all 42 official
checks, the edge cases and `integrity_check` on a fresh seed copy.

Work removed, closed loop with `perf stat` on the server CPU for 15 s, two runs each:

| Syscalls per request | Baseline | Final |
|---|---:|---:|
| `pwrite64` | 0.0495, 0.0492 | 0.0066, 0.0065 (−87 %) |
| `io_uring_enter` | 0.0078, 0.0078 | 0.0078, 0.0078 |
| `fsync` | 0.0001, 0.0001 | 0.0001, 0.0001 |

In the 300k profile, kernel time under `pwrite64` falls from 3.6 % to 2.7 % of the server
CPU. Most of what remains is the checkpoint thread writing database pages.

300,000 users, 60 s ramp, 60 s hold, interleaved B/F/F/B:

| | Baseline | Final |
|---|---:|---:|
| Hold CPU per request | 15.44, 14.48 µs | 14.83, 14.98 µs |
| Hold p95 / p99 | 2.2 / 8.9, 2.0 / 12.9 ms | 2.1 / 13.0, 4.7 / 12.7 ms |
| Memory mid-hold | 510, 511 MB | 519, 507 MB |
| Errors | 0, 0 | 0, 0 |

End to end the difference is inside the run-to-run noise. These are small, verified
reductions of specific work, not a demonstrated capacity gain. The largest remaining
user-space cost is the B-tree seek binary search (`sqlite3BtreeTableMoveto`, ~6 % of CPU,
mostly cache misses in the 20 author lookups per feed). Batching or prefetching those
lookups would mean re-implementing SQLite's page search, so it was left for a separate
experiment.

`-DWALBUF_TRACE` (with `WALBUF_TRACE=<file>` at run time) logs every buffered, written-through
and flushed WAL write with unbuffered writes that survive kill -9. That trace, together with
a snapshot of the WAL taken right after the kill, located the W1 bug.
