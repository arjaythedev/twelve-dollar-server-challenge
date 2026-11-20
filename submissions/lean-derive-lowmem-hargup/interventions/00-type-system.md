# Shared typed specialization system

Status: kept; shared types drive all retained lowerings. Source baseline: `100234f`.

## Work to eliminate

The current generator represents fields and C expressions as strings. It loses field membership, endpoint demand, output bounds and storage information before lowering. Establish the vocabulary once so several interventions can follow from it.

## Type and correctness design

Use schema-indexed field references with ordinal and type evidence; endpoint-indexed demand; typed wire values and serialization bounds; and explicit lifetime/ownership contracts at the C boundary. Static evidence is erased at generation time. Do not pretend that this proves arbitrary emitted C. Avoid building an unused general compiler: each abstraction must drive a concrete generated representation or operation.

## Implementation plan

Introduce checked field projections and endpoint demand first; migrate consumers instead of maintaining a second model. Make malformed field references unrepresentable. Add compile-time examples for field membership and capacity arithmetic. Preserve a byte-identical or behavior-equivalent initial lowering before enabling optimized lowerings.

## Validation and decision gate

Lean kernel checks, generator regeneration, official tests, and differential output checks. Compare generated C and assembly to verify which operations actually disappear. A structural foundation may be retained without a speed claim if it is necessary for measured specializations.

## Experiments

Entries below record the candidate/control commits, checks, measurements and disposition. Full commit identities and collected artifacts are linked from the index. Coherent grouped changes are identified explicitly.

### Typed migration (2026-10-26)

Introduced context/type-indexed wire references and table/type-indexed schema fields.
Serializer C parameter lists are now derived from the same context as the wire shape.
The oracle consumes a typed environment and rejects model integers outside the C domain.
Endpoint payload demand is declared once, with a theorem that only creation consumes the
username. `lake build` passed all 20 targets; regenerating C produced no diff whatsoever.
This is a structural foundation, with no claimed runtime gain.
