# Schema-projected SQLite record kernels

Status: P3 retained; P1/P2 superseded; cursor lifetime fix retained. Source baseline: `100234f`.

## Work to eliminate

user_name decodes three columns and materializes col_t[3], although only username is consumed; the loop remains in the compiled binary. Feed requests repeat this for up to 20 authors.

## Type and correctness design

Represent a projection by fields that belong to a particular schema, with an exact requested storage type. Lower to bounded prefix decoding and named outputs. Retain checks on untrusted serial types and lengths: SQLite declarations are not runtime typing guarantees.

## Implementation plan

Generate a username projection first, then a post projection and integer index prefix where justified. Skip unused trailing columns and avoid intermediate column arrays. Keep overflow-page handling and ensure a borrowed result cannot be overwritten by the shared spill buffer before use.

## Validation and decision gate

Compare native reads against SQL for 20,000 seeded posts and feeds, plus long/overflow rows and unexpected storage types. Measure feed/post/mixed application CPU in interleaved pairs, then network traffic. Keep only reproducible improvements without regressions.

## Experiments

Entries below record the candidate/control commits, checks, measurements and disposition. Full commit identities and collected artifacts are linked from the index. Coherent grouped changes are identified explicitly.

### Experiment P1: username projection

The typed `usersName` reference supplies the ordinal. The lowering returns one checked
column, skips trailing serial types, and avoids the three-element intermediate array.
The generated ordinal remains static; untrusted lengths and serial types remain checked.
Control executable: `93ab6eb` (same C as typed foundation `1bf9f9b`). Candidate SHA and
measurements will be added after the build. Lean clean build passed all 20 targets.

P1 source `c420aa1`, control `93ab6eb`: official 42/42, edge cases, and SQL comparison
for 20,000 posts passed. Interleaved thread-CPU medians (ns/request): feed 8056.01 →
7888.08 (-2.1%); mixed batch 32: 3121.21 → 3198.46 (+2.5%); post 3366.97 →
3386.66 (+0.6%). No competing load generator was detected; substantial isolated control
outliers remain. Raw data: `metal/results/pair-93ab6eb-c420aa1-1793081628975389705`.
Decision: do not keep this generic lowering as the final optimization; it does not establish
a broad win.

### Experiment P2: schema-selected physical encoding

The schema identifies a text field immediately after a rowid alias. Emit a guarded one-byte
header/type path that returns a text span directly; retain the general checked projection
for longer encodings. This removes the intermediate column and varint loops on the common
encoding. The pure record codec was separated for direct sanitizer testing: 20,000 generated
valid/random records and truncated prefixes passed ASAN/UBSAN. This experiment shares a
commit with endpoint claim specialization; read-only microbenchmarks isolate the projection,
authentication microbenchmarks isolate claims, and mixed HTTP measures the combination.

P2/C1 source `e735916`, control `93ab6eb`: all correctness checks passed, including new
claim cases. Medians (ns/request): feed 8028.47 → 8083.09; mixed batch 32: 3155.58 →
3148.98; post 3309.80 → 3258.39. These do not establish an application-CPU gain. Mixed
HTTP ABBA runs were 174619, 182628, 179514, 176689 requests/s (about +3.1% for the
combination), all zero errors. Raw micro data: `pair-93ab6eb-e735916-1793082008107683415`;
HTTP summaries have timestamp suffixes 1793082188847693270 through 1793082223373738941.
Keep this as a promising control executable, not proof of an isolated projection speedup.

### Experiment P3: simpler prefix derived from membership

Replace the custom physical-encoding helper with the existing decoder instantiated at
`usersName.prefix = 2`, with a two-element array. `Field.prefix_le` proves that this is a
valid prefix of the schema. This directly removes the unused trailing field, adds no new
runtime parser and reduces code complexity. Shares a commit with C2 and R1; compare the
combined lowering with P2/C1. Sanitizer checks cover 20,000 valid records and truncations.

### P3/C2/R1 measured together (`99730e2`, control `e735916`)

All 42 official cases, 20,000 SQL-oracle post lookups, fragmentation/pipeline edges
and claim validation passed as UID 65534. Paired thread-CPU medians (ns/request):
health batch 1 2779.09 → 2409.05, health batch 32 127.97 → 121.76; mixed batch 32
4370.35 → 3060.16, with large control variability; decode-like 1239.40 → 1270.50.
Raw: `results/pair-e735916-99730e2-1793082571431849072`. No attribution of the noisy
mixed delta to an individual intervention. R1 reduces each retained inline allocation
from a 32-byte descriptor plus 1024-byte block to a single 224-byte allocation.

Long escaped claims against foundation `93ab6eb`: like decode 4012.44 → 3877.22;
create decode 4013.53 → 4156.19. Raw:
`results/pair-93ab6eb-99730e2-longclaims-1793082657017962536`. The forced-inline
claim specialization does not show a general decode speedup. Keep provisional pending
end-to-end comparisons; final decisions must not cite these samples as a clear win.

### Overflow lifetime regression and T1 results

The first overflow fixture exceeded the schema's 500-character body constraint and
failed during setup; it was invalid evidence. Harness correction `dd12d01` keeps 30
dense seed posts, uses 512-byte SQLite pages, a valid 500-emoji body and a long username.
Startup confirms the native feed invariant holds. Old `ce57153` then produces invalid
UTF-8 on a post read; fixed `adbbbda` passes 1,000 random/deterministic post checks and
the feed SQL oracle, then passes again after a foreign write changes a timestamp.
Raw fixed result: `results/adbbbda-overflow-1793083331022446433`. Cursor fix `7de6f60`
is mandatory correctness work independent of performance decisions.

T1 `adbbbda` passes 42 official cases, 20,000 SQL post checks plus feed, pipelines,
escaping and claims under UID 65534. Generated serializer A/B/B/A/A/B medians
(ns/object): short 34.91 → 28.74; 500 ASCII chars 98.91 → 36.69; 500 emoji
(2,000 UTF-8 bytes) 208.73 → 79.09. No observed loadgen/compiler overlap.
Raw: `results/text-adbbbda-1793083350660246083`. Every run first compares complete
serialized objects for exact equality. This is a serializer gain, not a claim that
1%-creation mixed HTTP throughput improves by the same percentage. Keep T1; output
remains owned and no request-local refinement is assumed for stored text.

### P3 isolated prefix measurement and decision

`record_micro.c` uses the actual `rec_cols` implementation with constant prefixes
3 and 2, identical username consumers, realistic short/long record headers and
matching checksums. Built from `8f7dabd` with `gcc -O2 -march=native`, run on CPU0
after a quiet window, 30 million records per sample, A/B/B/A/A/B. Medians:
short usernames 8.565 → 6.266 ns/record; long usernames 9.329 → 6.695.
Raw: `results/record-prefix-8f7dabd.txt`. Keep P3: 27–28% less decoder CPU in this
isolated kernel, only 2–3 ns saved per username. This is not a comparable percentage
gain for feed/HTTP requests. P1/P2 helpers remain rejected/superseded.
