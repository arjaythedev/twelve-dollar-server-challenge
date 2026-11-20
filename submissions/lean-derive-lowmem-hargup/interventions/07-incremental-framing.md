# Phase-indexed incremental HTTP framing

Status: H1 retained; fragmented parsing improves, ordinary traffic remains comparable. Source baseline: `100234f`.

## Work to eliminate

An incomplete body causes http_parse to rescan the header block on subsequent receives. Finding the header terminator also rescans prior fragments.

## Type and correctness design

A connection-local framing state distinguishes searching for headers from waiting for a known body length. Stored offsets remain valid across realloc/memmove; borrowed pointers must not survive those moves. The state resets exactly when one framed request is consumed.

## Implementation plan

Persist the search frontier and validated framing metadata only for partial requests. Keep the common complete-request path compact. Decide whether cold state belongs in the existing fragment allocation rather than enlarging every idle connection.

## Validation and decision gate

Split requests at every delimiter and body boundary, pipeline next requests, test conflicting Content-Length, close/error/reset paths, and run sanitizer checks. Benchmark fragmented large bodies and ordinary traffic; report cold-state memory and reject broad normal-path regressions.

## Experiments

Entries below record the candidate/control commits, checks, measurements and disposition. Full commit identities and collected artifacts are linked from the index. Coherent grouped changes are identified explicitly.

### H1 implementation and validation design

`Feed.Frame.Phase.fields` determines a phase-specific C union: a header search frontier,
or validated method/close/auth flags and six 32-bit header-relative spans/lengths.
Membership-indexed field references generate stores. Body restoration reconstructs
pointers only from the current receive base. The existing C header parser is the trust
boundary; its successful header validation constructs the body-wait phase.

The 32-byte state is prepended to an allocated fragment buffer, adding no per-idle-
connection fields. Search resumes with three bytes of overlap. Body wait tests only
`n >= header_length + body_length`. Consuming a request, capacity hold/replay, idle
release and close/reset preserve or reset the matching phase. Header errors retain
the original status and connection-close behavior.

`framing_cases.c` compares each incremental prefix against stateless parsing across
all first-split positions, relocates storage between calls, exercises malformed/duplicate
headers, large fragmented bodies, pipelined prefixes and size limits. ASAN/UBSAN passed
locally. `transport_cases.c` exercises the actual cold allocation/growth/release and
write/capacity hold paths on Linux without opening a ring. Network tests and ordinary
traffic comparison remain required before keeping H1.

### H1 (`9c2439f`, control `adbbbda`)

Linux transport ownership tests passed ASAN/UBSAN. Framing microbenchmark with a
16 KB body delivered in 31-byte fragments: stateless median 26,449 ns/request,
incremental 1,378 ns/request (~19x less parsing CPU). Identical completion checksums.
All API, SQL post/feed, edge and claim tests passed UID 65534. The 10,000-connection
fragment test returned to idle with 1,605,632 bytes anonymous growth (cold fragment
allocations released); raw `results/9c2439f-fragment-1793083552003916275`.

Eight-second mixed HTTP ABBA: control 175,099 / 179,648 req/s; candidate 155,177 /
185,573 req/s, zero errors in all runs. The candidate has a large outlier and this
short run does not establish normal-path parity. Raw directories timestamped
1793083564614543428, 1793083584370138143, 1793083604229395418, 1793083623998398903.
Repeat with longer windows before final keep/reject decision.

### H1 longer repeat and decision

Twenty-second ABBA with improved interference monitoring: control 181,061 / 175,973
req/s; H1 184,443 / 176,371, zero errors and no observed external loadgen/blast/cc1.
CPU µs/request: control 5.550 / 5.693, H1 5.447 / 5.653. Raw result timestamps
1793084718284603130, 1793084750015374982, 1793084781735206836,
1793084813455210900. The earlier large negative outlier does not reproduce; normal
traffic remains in the same performance range. Keep H1 for the ~19x fragmented
parse improvement with no idle-connection storage increase; no standalone mixed-
throughput speedup claim.
