# Endpoint-specific payload layouts

Status: J1/S1 retained as one representation change; no standalone CPU gain claimed. Source baseline: `100234f`.

## Work to eliminate

Every request initializes and carries fields for all endpoints in job_t. Reads drag authentication and write-body metadata through batch scans.

## Type and correctness design

Derive payload fields from endpoint demand, keeping a small common transport envelope and separate exact payloads. A typed reference must identify the payload kind and lifetime. An endpoint tag alone is not proof unless construction and consumption are both generated from the same description.

## Implementation plan

Generate payload structs and typed construction/accessors. Try a compact envelope plus cold payload arena or per-kind arrays; do not merely replace fields with an equally large union. Preserve all connection generation, close-after, response order and error handling semantics.

## Validation and decision gate

Measure sizeof/layout, bytes initialized per read, mixed decode/batch CPU and pipeline behavior. Benchmark memory at high concurrency separately from per-batch temporary memory. Reject extra indirection if it outweighs reduced cache traffic.

## Experiments

Entries below record the candidate/control commits, checks, measurements and disposition. Full commit identities and collected artifacts are linked from the index. Coherent grouped changes are identified explicitly.

### Coupled J1/S1 design before implementation

Generate `job_gen.h` from `Endpoint.payload`: post has one ID; like has post/user IDs;
create has user ID, username span and body span; feed/health have no payload. A common
transport envelope carries kind/write/close flags, connection/generation, a cold payload
pointer and response pointer. The planned x86-64 envelope is 32 bytes versus 72 currently.
Payloads live in the request/batch arena and expire only after execution. Allocation follows
authentication/id validation so error precedence is preserved.

Introduce endpoint/argument-indexed references whose membership is checked by Lean; generate
both payload declarations and accessor expressions from this description. Authentication
returns validated claims to the generated constructor, and materializes username only for
creation. Body decoding fills the creation payload. Runtime consumers use the specific
payload type inside the matching endpoint implementation. The generator emits both the C
application and the job header; build.sh must regenerate/compare both.

The envelope has four alignment bytes after its generation field. Use them for a `next`
32-bit slot index for S1, so execution lists do not increase envelope size. Keep only stream
heads, tail pointers and counts; no MAX_JOBS-sized pointer arrays. Typed slot indices lower
to u32 with UINT32_MAX as the empty-list sentinel. The parser already bounds job slots.

`app_decode` receives the destination slot index as an explicit argument. Runtime parsing
passes `njobs-1`; the benchmark driver passes its local array index. A stream append writes
the previous tail's next index, and app_batch follows indices relative to its `js` array.
The envelope/streams are one coherent representation change and will be measured together.

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

### Retained representation decision

Keep J1/S1 together. The common envelope reduction saves 40 bytes per job, or
2.5 MiB of static job-array address space at MAX_JOBS=65,536. Cold post/like payloads
use the existing batch arena; creation's 40-byte payload offsets the common-envelope
saving for that endpoint. This is not a 40-byte saving per connection or a guarantee
of the same RSS decrease. Mixed decode/batch measurements remain near parity/noisy;
the justified benefit is exact storage and removal of redundant classification work.
