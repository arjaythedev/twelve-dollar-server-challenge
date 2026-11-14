# Typed SQE construction and batch publication

Status: Q1 retained; publication work reduced, standalone throughput gain unproven. Source baseline: `100234f`.

## Work to eliminate

sqe_get clears 64 bytes and publishes the shared SQ tail for every entry, before callers fill opcode-specific fields. Construction and publication can be separated.

## Type and correctness design

Model reserved, initialized and published entries plus queue capacity. Only fully initialized entries become visible. Preserve release/acquire ordering and partial submission accounting. Zeroing removal requires proving every kernel-consumed field initialized; batching publication is the first experiment.

## Implementation plan

Maintain a private construction tail and publish it immediately before every io_uring_enter, including capacity-triggered flushes. Retain full zeroing initially. Consider opcode-specific initialization only after inspecting the kernel contract and generated assembly.

## Validation and decision gate

Test SQ wraparound, forced small queues, saturation, short sends, cancellation, rearm and shutdown. Measure enters/job, SQ tail stores and mixed network CPU, with unprivileged high-connection tests. Do not change host tuning or use busy kernel polling.

## Experiments

Entries below record the candidate/control commits, checks, measurements and disposition. Full commit identities and collected artifacts are linked from the index. Coherent grouped changes are identified explicitly.

### Q1 design before measurement

`Feed.Sqe.ReadyPlan op` contains typed references into the exact opcode argument context
and proof that all required dynamic fields are assigned. It emits six specialized
constructors: accept, recv, send, shutdown, fixed-file close and accept cancellation.
The shared `Wire.Ref` system now includes signed fd, u32 and u64 machine domains.
Constructors reserve/zero a slot, initialize constants and typed arguments, and only
then commit to the private tail. Full 64-byte zeroing remains; removing it is not
justified without a complete per-opcode kernel field model.

`ring_enter` is the single publication boundary for both normal submission and capacity
flushes. It release-stores the ready tail immediately before entry and subtracts only
the returned submission count. Reserve uses acquire-loaded head and preserves unsigned
wrap arithmetic. Shutdown reserves two slots before constructing its linked close,
preventing an application-induced capacity flush from splitting that pair.

The ABI ordering and link boundary follow the Linux manual:
[io_uring_enter(2)](https://man7.org/linux/man-pages/man2/io_uring_enter.2.html).
This is an explicit C/kernel trust boundary, not a Lean proof of kernel behavior.
`sqe_cases.c` mocks only the syscall and tests actual production functions with tiny
queues, unsigned tail wrap, interrupt retries, partial consumption, field zeroing and
close-pair capacity. Real small-queue API/pipeline tests and mixed network measurements
are required before keeping Q1.

### Q1 (`5a536b3`, control `9c2439f`)

Mocked-boundary tests passed ASAN/UBSAN on Linux with 2/4/8 entries, tail wrap,
EINTR retries, one-at-a-time submission consumption, full zeroing and shutdown/close
capacity. Transport ownership tests also passed. All 42 API tests, SQL post/feed,
edge/pipeline and claim checks passed under UID 65534.

Twenty-second mixed HTTP ABBA: control 175,112 / 181,945 req/s; candidate
176,987 / 174,808 req/s; every run zero errors. Raw directories timestamped
1793083820821960831, 1793083852575660979, 1793083884308561263,
1793083916068052564. This does **not** establish a throughput gain (candidate mean
~1.5% lower with overlapping spread). The intended benefit is fewer shared-tail
publications and complete construction before visibility; collect STATS publication
counts plus real tiny-queue/high-connection checks before final disposition.

### Real kernel and publication diagnostics

`1ec22ff-small` (same production C as `5a536b3`, `-DSQ_ENTRIES=4`) passes all
42 API tests, 20,000 SQL-oracle post checks plus feed, fragmentation/pipelines and
claims under UID 65534. Raw: `results/1ec22ff-small-check-1793084159115816100`.
`1ec22ff-batch` passes the actual generated queue and forced-COMMIT rollback/reuse
test under ASAN/UBSAN as UID 65534.

The diagnostic STATS build during mixed traffic recorded 1,574,907 initialized SQEs
and 12,368 shared-tail publications in one reporting interval (about 127 SQEs per
publication, 99.2% fewer tail stores than per-entry publication). It recorded 12,379
entries and 1,574,776 jobs. Raw server log:
`results/1ec22ff-stats-blast-1793084190665757759/server.log`. This demonstrates the
intended work reduction, while the non-instrumented throughput comparison remains
inconclusive. Keep Q1 for the measured publication reduction and construction/order
fix; do not claim an end-to-end speedup from it.
