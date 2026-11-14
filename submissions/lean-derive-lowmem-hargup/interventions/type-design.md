# Type-system design before lowering changes

## Decision

Use small dependent descriptions that the existing emitter consumes directly. Do not add a
parallel compiler or a catalogue of phantom types disconnected from generated C. Separate
semantic facts from the cost model: facts survive a rejected lowering; expensive representations
can be replaced without weakening the contract.

1. **Typed references and contexts.** A wire reference names a member of a finite context
   and has its declared type. A schema field belongs to one table and has its checked schema
   type. No string lookup with `getD 0`, and no oracle returning `?` on a type mismatch.
   Contexts also derive C argument lists, preventing serializer/caller drift.
2. **Demand.** Endpoint descriptions determine which verified claims and payload fields are
   materialized. Validation demand and result demand are different: a like validates username
   but does not retain its bytes. Record projections determine the last header field required.
3. **Encoding/storage.** Wire shapes and C scalar domains determine maximum body sizes.
   Unbounded text remains unbounded. Bounded small output can select inline queue storage;
   general output remains growable. Include SIMD overstore padding separately from wire length.
4. **Refinements.** `unescaped`, valid UTF-8, length bounds and timestamp format are established
   by parsers/checks or generation, never by an unchecked cast. Preserve them through operations
   whose semantics maintain them. Database-wide facts require checked initialization and the
   existing foreign-write invalidation. Do not cache per-row values between requests.
5. **Phases and ownership.** Request parsing phases and SQE construction phases describe legal
   transitions. Output bytes survive until send completion; receive-buffer leases do not.
   Lean is not a linear language by default: a state-indexed API is not by itself a proof of
   unique ownership of the emitted C allocation. State transitions must generate the actual
   C lifecycle and be exercised with resets, partial sends and sanitizers.

## Lowering and trust boundary

The source of truth remains `Contract`, `Schema`, `Plan` and their typed descriptions. The
emitter specializes them into plain C structs, functions and loops; the submission has no Lean
runtime. Proof terms and static descriptors disappear during generation. Runtime boundary
checks remain where bytes, SQLite storage or the kernel can supply variable data.

Model-level correctness proofs do not verify arbitrary C text. The initial typed migration must
preserve generated behavior, then native/SQL differential tests, assembly inspection and timed
runs verify each lowering. Keep an honest distinction between Lean-checked index/type facts,
tested C refinement boundaries and measured performance.

## Order and grouping

Migrate wire references and schema references first while preserving C output. Then use demand
to drive projected records and claim materialization, followed by bounds-driven response storage.
Job layouts and execution streams share the endpoint description and may be one coupled change
if keeping an intermediate generic layout would obscure the design. Refinements and transport
phases follow after their lifetime obligations are explicit. This permits improvements to fall
out of the shared design without attributing a grouped result to one isolated optimization.

## Rejected shortcuts

- A giant untyped union relabeled as a dependent type still pays the maximum storage cost.
- A runtime `needs_name` branch is not sufficient unless compilation specializes it away.
- Removing SQLite type/length checks based on declared affinity is unsound.
- A borrow into a receive buffer cannot survive buffer recycling; a borrow into the shared
  overflow scratch cannot survive another overflowing record read.
- Precomputing rows/responses or remembering verified JWTs violates the challenge contract.
- Smaller source or fewer C branches is not proof of fewer executed instructions or lower CPU.
