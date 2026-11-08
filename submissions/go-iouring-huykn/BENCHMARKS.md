# Local performance comparison

## Feed-path fixes

The earlier indexed revision was 16–17% slower than v2 in repeated long feed
trials. Reducing nesting had not removed the underlying work. This revision:

- Borrows the C-owned record metadata and text spans directly during rendering,
  removing the second 20-row Go array. An isolated conversion diagnostic measured
  about 149 ns per feed for the old materialization step.
- Decodes the like index's leading integer directly, keeps the decoded key
  across requested posts, and accumulates each count locally. Unusual record
  layouts still fall back to SQL. Integer decoding covers every SQLite integer
  width and signed limits without negative-shift undefined behavior.
- Skips task sorting when observed IDs are already descending, retaining the
  sorting path for arbitrary timestamp/ID order. Adjacent author IDs share a
  request-local lookup/span without a hash table.
- Uses explicit C-owned opaque handles at the cgo boundary and `-fno-plt`.
  These reduce bridge/compiler overhead; their individual contributions are not
  isolated in the final comparison. Go buffer pointer checks remain enabled.
- Checks JSON strings in bounded words, covering short fields with overlapping
  loads rather than scalar tails. Short-field escaping was a measurable handler
  cost after the database changes. No load extends past its source span.

The changes were evaluated together; the timing difference below must not be
attributed entirely to any one change. SQLite schema checks, cursor locks,
transaction boundaries, result ownership, and SQL fallbacks remain enabled.

Seven alternating 10-second trials per version and feed family used separate
processes, fresh seed copies, one Docker CPU, Go 1.27.3 on Linux arm64,
`GOMAXPROCS=1`, and `GODEBUG=asyncpreemptoff=1`. Both versions used the same
benchmark harness. Source and binaries stayed unchanged during the timed runs.

| Feed benchmark | v2 median (range), µs/op | Fixed median (range), µs/op | Median time change |
|---|---:|---:|---:|
| Database bridge | 2.563 (2.492–2.582) | 2.539 (2.504–2.600) | -0.9% |
| Request handler | 3.060 (3.048–3.111) | 3.031 (2.984–3.163) | -0.9% |

The 28 samples measured **119,169,689 operations**; all reported zero Go
allocations. The fixed handler was faster in six of seven paired trials, with
one trial 3.5% slower. Database pairs ranged from 2.3% faster to 0.7% slower.
The timing ranges overlap: these results support near parity on this machine,
not a reliable speedup. The previous clear 16–17% feed regression no longer
appears in this comparison. Earlier tables below describe earlier code and
must not be used as measurements of this revision.

Validation of this revision: all 42 integration checks, Go unit tests, forced-SQL
unit tests, and `go vet` passed. C contracts passed with both `SQLITE_DEBUG` and
undefined-behavior instrumentation. New indexed/SQL fixtures exercise signed
integer limits and SQLite integer widths. JSON alignment/boundary and Linux
guard-page tests passed, including a Go `checkptr=2` run.

The database benchmark includes the Go adapter and commit. v2 materializes its
original rows; this revision returns a borrowed view, whose fields are consumed
by the handler benchmark. The handler benchmark includes dispatch and JSON but
excludes networking and event-loop batching. These are warm local tests on
Linux arm64, not an organizer score or evidence of parity on the target x86_64
VM, under mixed writes, or at tail latencies.

Raw results, the run script, diagnostic sources, and validation logs are retained
in `/tmp/huykn-feed-fix-results/` in the author workspace. They are local artifacts
and are not required to build this submission.

## Earlier implementation comparisons

Baseline: v2 `b76baf7`. Intermediate: public-SQL rewrite `aabee9c`.
Candidate in the historical tables below: indexed read plan `1a42885`
(documented at `ded98c4`), before the feed-path fixes above.

Seven alternating trials on Linux arm64 in a Docker container limited to one
CPU. Each benchmark family ran in a separate process with a fresh copy of
`seed/feed.db`, `GOMAXPROCS=1`, `GODEBUG=asyncpreemptoff=1`, and 10,000 operations
per case. The three versions used identical database and endpoint benchmark
harnesses. Values are medians; a negative time change is faster.

### Database calls

These call the Go database bridge and commit after each operation. They exclude
HTTP handlers, JWT verification, and handler JSON rendering. Bridge work differs:
v2 returns its existing row representation; the candidate returns owned record
spans. This is an application database comparison, not an isolated SQLite test.

| Operation | v2 (µs/op) | SQL rewrite (µs/op) | Indexed plan (µs/op) | Change vs v2 |
|---|---:|---:|---:|---:|
| feed | 2.430 | 9.953 | 2.872 | +18.2% |
| post | 0.439 | 1.008 | 0.472 | +7.6% |
| create | 9.938 | 24.951 | 9.166 | -7.8% |
| repeat_like | 0.924 | 0.821 | 0.433 | -53.1% |

### Request handlers

These include handler dispatch, applicable authentication, JSON output, and a
commit after each request. They exclude networking, HTTP framing, and event-loop
batching.

| Operation | v2 (µs/op) | SQL rewrite (µs/op) | Indexed plan (µs/op) | Change vs v2 |
|---|---:|---:|---:|---:|
| health | 0.275 | 0.108 | 0.111 | -59.6% |
| feed | 2.966 | 10.464 | 3.536 | +19.2% |
| post | 0.490 | 1.058 | 0.537 | +9.7% |
| create | 11.034 | 22.838 | 10.184 | -7.7% |
| like | 1.480 | 1.368 | 0.999 | -32.5% |

All cases report zero Go allocations per operation. Go's counters do not include
C allocations. The like cases repeat an existing like; they do not measure new
like inserts. These are local warm microbenchmarks, not an organizer score,
capacity estimate, tail-latency study, or proof of parity on the target x86_64 VM.

### Longer feed stability check

Follow-up on the indexed implementation at `1a42885` and v2 `b76baf7`: seven
10-second trials per version and per feed benchmark family, with separate
processes, fresh seed copies, and alternating version/family order. The container
was limited to one CPU with the same Go runtime settings as above. No production
code changed between the short comparison and this follow-up. Go calibration
ran before each timed sample; each sample measured millions of operations.

| Feed benchmark | v2 median (range), µs/op | Indexed median (range), µs/op | Median time change |
|---|---:|---:|---:|
| Database bridge | 2.508 (2.485–2.589) | 2.926 (2.894–3.080) | +16.7% |
| Request handler | 3.034 (2.977–3.081) | 3.513 (3.469–3.596) | +15.8% |

The 28 samples measured 112,754,110 feed operations in total and all
reported zero Go allocations. The indexed implementation was slower in every
paired comparison; the observed timing ranges do not overlap. Longer trials
change the estimated gap somewhat, but the feed regression persists on this
machine. That revision did not achieve feed performance parity with v2.

These runs repeatedly read a warm feed and commit after each request. They do
not exercise network traffic, mixed write load, or event-loop group commits.
Raw results, harness checksums, commit IDs, and the run script are retained in
`/tmp/huykn-feed-long-results/` in the author workspace.

### Why the SQL rewrite was slower

Lower nesting does not reduce the work performed by SQLite. The public-SQL
rewrite introduced SQL VM execution for indexed reads, Unix lock system calls,
and inline WAL checkpoint work. The new database implementation addresses that
work directly:

- An independent cursor plan replaces SQL VM reads on the supported schema.
  It orders post lookups, joins authors within each request, and merges sparse
  requested IDs with the like index. Unsupported layouts still use SQL.
- SQLite's built-in `unix-excl` VFS removes repeated process-lock system calls.
  This assumes a single server process owns the database. It is not a custom VFS.
- A new per-handle worker performs passive checkpoints. SQLite still commits
  every batch before the server sends responses. A restart barrier handles WAL
  growth, and worker startup failure preserves automatic checkpoints.
- Create uses an INSERT plus a same-transaction lookup of the stored timestamp,
  avoiding RETURNING's temporary result table.

Diagnostic write traces found about six `fcntl` calls per operation with the
standard Unix VFS, versus nine total across the 10,000-operation `unix-excl` run.
An isolated C probe also separated the checkpoint effect: committing with inline
checkpoints took roughly 21–27 µs versus 8–9 µs with automatic checkpoints
switched off for that diagnostic. The shipped implementation runs a worker; it
does not leave checkpointing disabled. These diagnostic timings are separate
from the controlled tables above and are not additional benchmark scores.

That candidate substantially improved on the SQL rewrite but had an 18%
database feed gap and an 8% single-post gap versus v2 in the short trials. Its bounded record decoding,
result copying, schema checks, and sparse join plan differ from v2's read path;
these measurements do not isolate each contributor to the remaining gap. The
handler comparison also includes different rendering and authentication code.

### Earlier validation

- All 42 checks in `test/run.sh` passed.
- Go unit tests passed with indexed reads and forced SQL reads. Disposable seed
  tests include 300 randomized SQL/JSON round trips; `go vet` passed.
- Indexed/SQL fixtures cover empty feeds, sparse IDs, mixed timestamps/authors,
  missing authors, schema/encoding/type fallbacks, and SQLite overflow pages.
- C contracts passed with `SQLITE_DEBUG`, exercising both feed and single-post
  fast plans, cursor lock requirements, commit visibility, denied commits,
  implicit rollback and recovery, fresh author reads, checkpoint snapshot
  isolation, the WAL growth barrier, and schema-cookie fallback.

Raw local timing files and medians: `/tmp/huykn-db-plan-final/` in the author
workspace. They are local artifacts and are not needed to build this submission.
