# Specialized batch execution streams

Status: J1/S1 retained; snapshot/order/rollback semantics tested. Source baseline: `100234f`.

## Work to eliminate

app_batch scans the same generic array to count requests and dispatch reads and writes; deliver scans it again. Runtime switches recover endpoint facts already known while decoding.

## Type and correctness design

Endpoint effects determine scheduling, but write order and each connection's response order remain explicit invariants. Separate dispatch structure from delivery order. No response or database value may be shared across requests as a result cache.

## Implementation plan

Construct compact execution streams while decoding. Generate endpoint-specific read loops where independent; preserve a single ordered write stream and original delivery slots. Derive counts from construction. Do not reintroduce the previously rejected branch-heavy selective-cursor optimization.

## Validation and decision gate

Compare interleaved creates/likes/reads and failure responses with the existing ordering semantics. Measure one-request and large mixed batches, then HTTP pipelines. Keep only when both dispatch cost and full workload support the change.

## Experiments

Entries below record the candidate/control commits, checks, measurements and disposition. Full commit identities and collected artifacts are linked from the index. Coherent grouped changes are identified explicitly.

### Coupled J1/S1 design before implementation

Use three read streams (feed/post/health) and one ordered write stream. Their intrusive `next`
indices fit unused padding in the new 32-byte job envelope; heads/tails/counts are the only
additional state. Generated successful route constructors append to the appropriate stream.
Error-only jobs remain in delivery order but need no database execution entry.

Split exec_read into endpoint-specific functions so each read stream calls its own kernel
without a switch. Reads share the existing batch snapshot and may execute grouped by kind;
responses still occupy their original job slots and deliver in original order. Keep one
ordered write stream and its like/create selection; reordering writes is not justified.
Transaction begin, foreign-write invalidation, cursor lifetime, commit-before-response and
rollback response replacement remain unchanged. On commit failure only the write stream's
responses are replaced. Reset all stream state and the arena at batch completion.

A decode-only benchmark must reset the same construction state between rounds; otherwise it
would accumulate links to reused stack slots. Its driver must measure the actual generated
entry points with correct slot indices, preserving comparability with the archived executable.
Test mixed pipelines, creates followed by reads/likes, disconnects, errors and zero-read/write
batches before accepting any timings. Compare small and large batches, then mixed HTTP.

### J1/S1 (`ce57153` versus `99730e2`)

All 42 official API cases, 20,000 random SQL post checks, pipeline/fragment edges and
claim checks passed under UID 65534. `sizeof(job_t)` is 32 (was 72); post payload 8,
like payload 16, creation payload 40 bytes; health/feed allocate no payload.
Interleaved thread-CPU medians ns/request: health batch32 123.46 → 119.60; mixed32
3038.10 → 3091.58; post32 740.02 → 633.85 (noisy); like decode 1208.34 → 1227.10;
create decode 1329.79 → 1341.13. Raw:
`results/pair-99730e2-ce57153-1793082861234507110`.

The representation saves 40 bytes per hot job and removes classification passes;
CPU results are mixed, with small decode overhead and no established mixed win.
Keep provisional until paired network comparison; do not report post improvement
as reliable given its large per-sample spread.

### Transaction semantics regression

`1ec22ff-batch` runs the actual generated decoders/executors under ASAN/UBSAN as
UID 65534. A create followed by a like of the new ID executes in order, while a read
of that ID in the same batch sees the prior snapshot. An error hole does not enter
any stream. A rejecting SQLite commit hook forces rollback: only write responses
become 500, successful read responses survive, database state is unchanged, and a
subsequent creation succeeds. Queue heads/tails and the payload arena reset correctly.

### Final disposition

Retain S1 with J1. Exact payloads and intrusive streams are one representation change;
the queue link occupies existing envelope padding and adds no per-job storage.
Keep the measured space/work reduction and tested ordering semantics, without an
isolated S1 throughput claim. The complete retained candidate's paired HTTP result
is reported in the index and is not attributed to this intervention alone.
