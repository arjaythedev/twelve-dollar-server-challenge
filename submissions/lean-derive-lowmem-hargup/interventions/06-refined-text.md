# Refined strings through parsing and serialization

Status: T1 retained; global timestamp refinement not adopted. Source baseline: `100234f`.

## Work to eliminate

All output text uses the general escaping serializer. Input validation already discovers escaping and UTF-8 facts, but later stages forget them. Timestamp output is repeatedly scanned as arbitrary text.

## Type and correctness design

Distinguish arbitrary text, validated unescaped JSON content, bounded decoded text and fixed-format timestamps. Establish facts at a checked boundary and preserve them. Stored text refinements require actual validation and invalidation on foreign writes, not assumptions about the seed.

## Implementation plan

First preserve request-local no-escape information through trimming and create-response serialization without another counting pass. Consider a validated global timestamp-format invariant only if its preservation and fallback are complete. Generate safe-copy and general-escape serializers from the same wire description.

## Validation and decision gate

Test ASCII, escapes, control characters, BMP/astral UTF-8, trim behavior, timestamps outside the refinement and foreign writes. Use long-body and normal fixtures. Include initialization/validation overhead in the report. Exact-sizing double scans were already rejected; do not silently repeat them.

## Experiments

Entries below record the candidate/control commits, checks, measurements and disposition. Full commit identities and collected artifacts are linked from the index. Coherent grouped changes are identified explicitly.

### T1 design and implementation

`Wire.PlainText` carries a per-character no-escape predicate. `Shape.plainStr` requires
a reference of that type; the common post-shape constructor instantiates either generic
text or plain text. The emitter selects exact `n+2` capacity and a quoted memcpy for
the latter. The C trust boundary is the existing JSON scanner: only its validated
`escaped == false` branch constructs plain text. Trimming takes a substring, preserving
the property; no extra scan or sizing pass is added. A flag occupies existing padding
in the creation payload. General stored body/author/timestamp fields retain escaping.

Do not add a global timestamp invariant: its setup/invalidation complexity adds an extra
stored-data property for a small field; T1 directly tests the request-local opportunity.
ASAN/UBSAN compares the copy and general serializer for every substring of representative
plain UTF-8 bytes. Existing edge/claim cases exercise escaped and unescaped creation.

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
