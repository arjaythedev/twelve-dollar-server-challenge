# Parallel static optimisation review

Reviewed 2026-10-14. Source-only review: no builds, tests, benchmarks or production edits performed. Baseline remains the packaged one-shot/scalar/reference-query candidate with redundant batch savepoints disabled, verified locally at 275,000 users for five minutes. This review establishes opportunities, not faster scores or whole-field superiority.

## Highest-priority actionable findings

1. **Make timeout retirement incremental.** `src/server.asm:596-620` drains an entire expired timer bucket before returning to ring processing (`src/uring.inc:672-673`). Each raw-ring close queues cancellation and direct close (`uring.inc:422-451`). A synchronised cohort of idle connections can therefore issue hundreds of thousands of operations in one event-loop turn; SQ-full handling itself enters the kernel (`uring.inc:301-316`). The timer wheel avoids scanning every live socket but does not bound work per turn. Proposed change: a resumable expiration cursor with a fixed per-turn count and time budget. Do not advance `last_tick` beyond a partly drained bucket, and do not hide it behind the current `last_tick >= now_sec` exit. Give pending responses drain opportunities between slices. Test synchronised idle expiry and stalled-output expiry alongside active requests, recording worst loop gap and expiry lateness. This is a concrete missing planned safeguard, not a demonstrated corruption bug.

2. **Remove the retry queue's complete-CQ-empty prerequisite carefully.** `uring.inc:521-525` only permits ENOBUFS retry draining when the live CQ head equals the live tail. The caller processes at most 128 completions/about 1 ms (`776-804`), so a continuously replenished CQ can indefinitely postpone queued retries even while buffers return (`487-508`). The queue is generation tagged and bounded, which is good; its scheduling condition is the weakness. First record retry depth, oldest age, returned buffers per frozen batch and useful rearms. Then experiment with a progress-bounded retry allowance after each committed frozen batch, while preserving single outstanding receive and avoiding immediate depletion loops. Queued retries must count as runnable work so epoll sleep cannot strand them; current `688-692` and `815-816` already address this. Do not simply raise the fixed 128 cap (`528`).

3. **Bound send-priority scanning separately from useful completion count.** `uring.inc:610-665` stops after 64 useful resource-releasing CQEs, but can inspect all remaining frozen entries if they are predominantly receives. The 1 ms clock check runs only after each 16 useful matches (`648-660`). With an 8,192-entry CQ, repeated 128-completion rounds can rescan many entries without the intended time bound. Proposed change: independent scanned-entry budget and/or time check every fixed number inspected. Measure entries inspected versus actual releases and whole-loop CPU. Preserve FIFO progress and exactly-once publication of handled entries; a cursor must not refer to reclaimed CQ slots.

4. **Add admission hysteresis and explicit kernel-memory awareness only after transport evidence.** Admission currently closes accepted sockets at input/output thresholds (`uring.inc:889-894`), and already existing sockets can still reach input pressure (`961-997`) or output pressure (`1162-1181`). Reserving each admitted upload's full input/output capacity prevents partial-input allocation deadlock, but socket/kernel memory and queue resources are not bounded by those application counters. A low-water resumption policy should govern both accepts and selected receive/processing admission, letting pending outputs drain. Avoid pausing all partial uploads. Current passing 275k result does not prove safe behaviour near 2 GiB under different payloads or a different kernel.

## Lessons from pinned Rust #82

Compared local immutable trees #19 `bd517fa7c8a5` and #82 `63eeda6655a0`, specifically `src/uring.rs`. Relevant changes:

- Provided buffers 1,024 -> 4,096; CQ 8,192 -> 16,384; SQ 1,024 -> 4,096 (`#82:22,218-222`). Our SQ is already 4,096; our CQ is the kernel default twice SQ. Moving our buffer count to 4,096 costs another 6 MiB per ring / 48 MiB across eight rings, plus descriptors, before any additional kernel accounting. Bigger queues can absorb bursts but cannot fix permanently insufficient service rate.
- A per-client REARM bit plus generation-tagged deque prevents duplicate retries (`#82:629-645`). NASM already has the equivalent bit256 and bounded queue (`uring.inc:510-604`); no need to copy this mechanism wholesale.
- Retry allowance depends on completed selected buffers: `returned.max(32).min(NBUFS / 2)` (`#82:798-819`). Unlike NASM, #82 copies/drains a complete CQ snapshot (`763-768`) before processing it. Adapting its allowance without accounting for NASM's bounded partial-CQ processing changes the safety/fairness tradeoff.
- Pending retry work selects the short wait (`737-740`); ready application work uses wait=0 (`742-748`). NASM currently applies the configured completion target/wait on every active ring (`uring.inc:715-725`). A no-wait control specifically when the current CQ already has work is worth measuring separately from a globally immediate configuration; avoid collecting future completions while a transaction is open.
- #82's ten-second full client sweep and 120-second stalled-input/output policy (`824-832`) are not substitutes for our required 120-second idle / 30-second stalled policy. Borrowing code must not silently change that contract.

## Lessons from pinned NASM #75

`results/field/75-4b8ba1cc1bcb/source/submissions/asm-epoll-FlavouredTux/` uses reference indexed per-post counts (`src/db.asm:39-49`), direct NASM serialisation and prepared statements. These overlap our proven baseline; language alone does not establish an advantage. Its HTTP response policy retires keep-alive connections above 15,000 active sockets (`src/http.asm:296`), trading retained socket memory for reconnect work. That is a transport control worth measuring only after matched results, not evidence that its user capacity is 15,000. Our multiple registered-file rings solve a different bottleneck: retained connection count beyond the ordinary descriptor limit.

## Keep the measured lessons

- Bounded range-query implementation was correct on adversarial fixtures but lost about 5.7% requests per CPU second in three paired short trials. Retain as a diagnostic option, not a default rewrite.
- Removing redundant batch savepoints improved all three paired short trials by about 2.9% aggregate. Preserve default ABORT semantics, immediate constraints, autocommit-state checks and whole-batch failure handling (`experiments.inc:221-258,289-328`). Additional write statements, deferred constraints or schema triggers would require revisiting that proof.
- Multishot improved a small saturation diagnostic but failed the 275k short p99 screen; matched one-shot passed. Retain one-shot until independently qualified. Saturation rate and user capacity answer different questions.
- SSE quote scanning's approximately 1.4% noisy diagnostic gain did not establish a capacity win. Scalar remains a justified default.
- Passive checkpoint remains synchronous event-loop work (`experiments.inc:333`, `server.asm:2112-2128`), even after send submission. Measure checkpoint duration/loop gaps before adding a thread; THREADSAFE=0 and exclusive locking require architectural changes for a separate checkpoint connection.

## Suggested next experiment order

1. Finish the already authorised matched contender qualification; preserve exact artifact identities and do not overlap large load generators.
2. Add diagnostic-only loop-gap, priority-scan and retry-age counters; reproduce a sustained CQ/retry backlog and synchronised timer cohort.
3. Apply the smallest bounded timer and priority-scan changes; validate lifecycle, pressure, fault batches and idle/stalled timeouts before population tests.
4. Compare progress-bounded retries at unchanged 1,024 buffers, then buffer-size/CQ controls individually if evidence shows depletion. Each new default needs matched short screening and a full hold.

No serious memory-ownership corruption was established by this static pass. This is not an exhaustive correctness audit. The timeout and priority loops demonstrably lack the planned per-turn bounds; retry starvation is a plausible scheduling failure to reproduce rather than a measured production outcome.


## Unapplied progress-retry experiment

`research/progress-retries.patch` is an unapplied source diff, not a validated implementation. It adds `--retry=progress` and `--retry=cq-empty`; default remains the existing CQ-empty policy. Applying even an opt-in patch changes binary identity and adds a branch to returns, so retain the measured baseline artifact for comparisons.

Ring layout audit: each context is 512 bytes; pointers/counters occupy offsets 0..135, setup parameters 256..375. Offset 136 is an unused aligned dword. It is initially BSS-zero and explicitly zeroed during ring initialisation, never reset at frozen-batch start or during SQL. Partial-initialisation failure closes rings and falls back; no later reinitialisation exists. The new field is a **saturating scheduling credit**, capped at NBUFS, not an exact kernel-free-buffer ledger. Each completed provided-buffer return publishes its descriptor/tail first and then credits once; stale-generation receive CQEs already return their selected buffer exactly once. The priority skip markers do not invoke another return.

After `flush_batch` commits/releases all tentative replies, progress mode can drain queued retries even with a replenished live CQ. Queue visits are capped at min(credits, 128), independent of useful submissions; each popped stale entry also consumes credit and visit budget. Excess credits carry to another pass, bounded by NBUFS. With zero credits only an empty CQ permits a 32-entry restart allowance. When no retry entries exist, old credits are cleared so a later depletion is not funded by idle history. Existing generation checks, the single queued bit, receive-in-flight bit and closing checks remain in place. Drained visits remain counted as runnable work, so even stale cleanup cannot trigger the one-second sleep prematurely. `--enobuf=immediate` bypasses this experiment.

The patch corrects the existing source comment saying retries occur only after the frozen CQ is consumed: the new policy requires the **bounded processed batch to have committed**, not complete draining of the entire original frozen suffix. No waiting for future I/O occurs inside SQL transactions. Buffer credits can be consumed by ordinary rearmed receives before retries execute; this policy bounds retry churn but does not guarantee each retry finds a free buffer.

Required before adoption:

- Compile both controls; differential integration, failed batch/COMMIT/automatic rollback, one-shot and multishot lifecycle, stale generations, direct-slot reuse and bounded queue exhaustion.
- Force ENOBUFS with a continuously replenished CQ; show oldest paused request makes progress without double receives or buffer re-publication. Exercise stale-entry runs longer than 128 and credit saturation/carry/empty reset.
- Force aggregate input/output pressure with partial uploads and slow outputs; confirm selected uploads finish, pending outputs drain and admission recovers.
- Add diagnostic-only per-ring retry depth/high-water, oldest queued age, actual useful submissions versus popped stale entries, returns/credits, ENOBUFS frequency, CQ high-water and maximum event-loop gap. Existing counters alone cannot show retry fairness.
- Alternate unchanged baseline and progress mode in matched 275k short screens, identical memory/generator/ramp settings, fresh seed, no simultaneous large generators. Only select a default after consistent CPU/tail/pressure improvement and a five-minute confirmation.

No compiler, assembler, executable test or patch application was run for this proposal.


## Latest matched 275k repeat: neither passed

After this review, root measured the pinned #75 and unchanged NASM candidate sequentially with identical unchanged-script 60-second ramp / 30-second hold / no warmup, one CPU / 2 GiB / no swap / nofile 65,535 and private generator allowance 92 GiB:

| Server | Requests | HTTP/check failures | p95 | p99 | Peak server memory |
| --- | ---: | ---: | ---: | ---: | ---: |
| #75 | 2,213,345 | 0 | 882.828 ms | 3,583.575 ms | 625.83 MiB |
| NASM candidate | 2,343,445 | 647 (fraction ~0.000276) | 184.757 ms | 4,562.447 ms | 1,667,018,752 bytes (~1,589.8 MiB) |

Both failed the latency gate and survived without OOM. NASM's maximum was 7,857.773 ms. Its lower p95 does not compensate for worse p99 and nonzero errors. The historical five-minute pass and this failing repeat must both be retained; no matched win or stable maximum can be claimed. Root is preparing a diagnostic repeat to distinguish application budget exhaustion, buffer depletion and scheduling pressure.

## Static patch re-review and diagnostics

`git apply --check --directory=submissions/asm-nasm-melroy89 submissions/asm-nasm-melroy89/research/progress-retries.patch` passed against the original checkout on this review. That checks applicability only, not assembly syntax or runtime correctness.

Static state audit: offset136 has no existing references and is below setup parameters at256; initial BSS and explicit startup reset are safe. No reset during batch preparation discards returns from priority handling. Credits are debited for every popped queue entry, including stale generations; excess survives for later passes, capped at NBUFS. The static re-review caught a credit-lifecycle issue: waiting until the next drain to clear an emptied queue could let intervening new depletion inherit historical credits. The proposal now clears credits immediately on observing empty head/tail at loop completion as well as at entry. Retry queue head/tail wrap is unchanged. The `[rsp]` dword budget fits ENTER's existing eight-byte local slot and is preserved over nested calls. Buffer publication precedes credit; receive in-flight and queued flags are checked by unchanged uring_recv, closing state blocks rearming. A progress pass with credits can run while the CQ has entries, but the caller has already committed its bounded processed batch. No transaction-open collection was introduced.

The proposal now corrects the old comment: the processed batch must commit, while only the default control requires the live CQ to be empty. The credit cap can deliberately discard excess historical scheduling credit; that is not a loss of buffer ownership. Credits are not available-buffer accounting, so ordinary receives can consume returned buffers before retries execute. There is no age-based fairness guarantee across a long queue; FIFO plus bounded passes only improves the scheduling opportunity. None of those points justifies choosing it as default before execution evidence.

Existing IO_DIAG counters distinguish coarse mechanisms: 2 ENOBUFS, 3 input budget failures, 4 output budget failures, 6 send failures, 7 orphan accepts, 9 close requests, 10 peak CQ depth, 11/12 sampled input/output peaks, 13 requests and 14 live sockets. They do **not** distinguish retry starvation from no useful retry demand, nor timer stalls from per-ring waits. Add these diagnostic-only metrics before attributing the latest failure:

| Mechanism | Minimal measurement | Interpretation |
| --- | --- | --- |
| Input/output rejection | Existing counters 3/4, current bytes and close reason, sampled vs instantaneous high-water | Failures coinciding with caps imply application admission pressure; sampled peaks alone can miss short spikes. |
| Depletion churn | ENOBUFS delta, actual provided-buffer returns and retry submissions per ring per interval | Many failures per successful rearm/return indicate churn rather than useful progress. |
| Retry starvation | Per-ring queue current/high-water, oldest entry enqueue time/age, drain skips because live CQ nonempty, popped stale/useful entries | Queue age rising while buffers return and CQ-empty skips rise supports the scheduling hypothesis. Queue depth alone is insufficient. |
| CQ processing | Per-ring CQ depth, oldest observed completion age if timestamp available, inspected vs processed entries, priority scan length/time | Distinguishes available work not serviced from sparse work; priority-match count alone misses receive-only scans. |
| Ring scheduling | Per-ring last service time, maximum service gap, io_uring_enter duration/result, CQ depth before/after enter | Finds cumulative wait or unfair service across active rings; do not infer from total enter count. |
| Timer/SQLite stalls | Maximum expire_timers duration and retirees per invocation; SQL/batch/COMMIT/checkpoint durations | Shows whether long loop gaps came from unbounded retirement, SQL or checkpoint rather than depleted buffers. |
| Host/generator | Server CPU/throttling, generator CPU/memory/failed requests and sample times | Separates server exhaustion from same-host load-generator contention without assuming either explains the failure. |

Bounded next matrix: first reproduce the unchanged baseline with diagnostic-only counters; then compare **only** --retry=progress at unchanged 1,024 provided buffers; then test **only** NBUFS=4,096 (2,048-byte buffers, not 4-KiB buffer size) under the CQ-empty baseline; finally a separately labelled CQ16,384 control if needed. Do not combine retry, buffer count, CQ size and timer changes into a purported isolated result. Alternate order, fresh seed, identical generator/ramp/hold and no concurrent high-load runs. Keep short screens diagnostic; select defaults only after semantics/lifecycle/pressure checks and a successful full hold.


## Isolated extracted-assembly validation

Prepared `build/experiments/progress-retries/src/` from the current nine source files and applied the proposal only to that isolated copy. The root production source and measured binary remain unchanged. An extracted ASM harness executes the proposed provided-buffer return, retry enqueue and retry drain functions with mocked receive/close submission, real context/connection/generation/descriptor memory and no SQLite or sockets. Tiny NASM assembly and shared-library linking completed in a 0.2-second wall-clock tool call; no full-server build ran during the diagnostic population load.

The harness passed duplicate enqueue suppression, historical CQ-empty control, progress rearming with live CQ backlog, 32-entry empty-CQ restart allowance, 128-entry visit cap and credit carry, exact-budget queue-empty credit reset, stale generation skipping, closing/in-flight suppression, saturating credits, clearing idle historical credits and 32-bit FIFO index wrap. Initial harness helper incorrectly reset the queued flag during a duplicate-enqueue test; corrected the helper before the passing run. Harness source/log location: `build/experiments/progress-retries/retry-harness.asm`, `.py`, `.so`; rerun with `python3 build/experiments/progress-retries/retry-harness.py`.

This is real execution of extracted handwritten assembly, but mocks do not prove kernel buffer ownership, syscall validity, HTTP semantics, multi-ring fairness or capacity. Full-server linking and focused native integration remain deferred until root confirms the large diagnostic benchmark is finished. No performance benefit is established.


## Isolated full-server correctness follow-up

Once root confirmed the population benchmark had finished, assembled all three application ASM objects and linked only the existing measured SQLite object (SHA-256 `44e0a3b1ba9200da9a3f7c7dffe7f40f90600287cb52bf2cc7ff9694dcc35d17`) into scratch `build/experiments/progress-retries/bin/server`. Link/assembly took 16.54 seconds; no dependency compilation, PGO training or root production-output changes. Experiment binary SHA-256: `fa405971da1f1181b98bded9d1072cd37484448652f00ff3ecb31cff7bc18239`; per-source identities saved in scratch `identity.json`.

All four focused native integration combinations passed: retry CQ-empty/progress crossed with receive one-shot/multishot, unique port 3198. Used a disposable tiny fixture from the unchanged supplied schema with user 1; each run about 0.2 seconds. Tests cover authentication precedence and payload types, Unicode limits/escaping, FK failure followed by valid write, 32 duplicate likes (one 201 / 31 x 200), fragmented multibyte/chunked/100-continue, framing ambiguity and acknowledged-write SIGKILL persistence/integrity. This is not the full canonical-seed official 42 suite or capacity qualification. Logs and `integration-results.json` are under scratch.

Root's subsequent diagnostic unchanged-baseline 275k replay passed (p99 36.260 ms, zero errors), with zero ENOBUFS/input-budget/output-budget failures throughout. Therefore the earlier failure did not reproduce, and retry starvation cannot currently explain it. CQ peak 8,192 occurred around closing/ramp-down; that alone does not establish request-time depletion. The progress mode remains an experiment, not an adopted default or measured improvement. Next diagnostic focus is same-host generator contention/parallelism and repeatability, with all nonstandard generator controls labelled separately.


## Isolated known-literal copy experiment

Prepared fresh baseline-derived `build/experiments/literal-copy/src/` (no timer/retry patches), runtime `--literal-copy=scalar|inline`, default scalar. Unapplied proposal: `research/literal-copy.patch`; `git apply --check` passed against the original checkout. Production source and baseline binary remain unchanged.

New preprocessing-only `literals.inc` defines each of 11 APPEND literals once, derives its length with NASM `%strlen`, emits the same bytes via LITERAL, and unrolls exact 8/4/2/1-byte loads/stores. Includes the 47-byte Content-Type/Content-Length prefix via an escaped CRLF string. No NUL is copied. Feed/id prefixes originally declared later in experiments.inc have definitions available before the APPEND macro expands. Dynamic status/error/connection strings retain append_z. No extra environment variable, dependency or SQL/build change.

Clobber audit: scalar append_z advances RSI to the terminating NUL, advances RDI by payload bytes, changes AL to zero and leaves flags from TEST AL,AL. Inline advances RSI/RDI by exactly the same known length, changes RAX and ends with XOR EAX,EAX (same relevant zero/test flags). All current APPEND sites treat RAX as dead: next use assigns it, calls a helper that returns it or jumps to response finalisation that computes it. No other caller-saved or callee-saved GP register is touched by the inline macro. The runtime flag comparison adds one branch per APPEND; speedup remains to be measured.

Assembled/linked scratch application using unchanged measured SQLite object only; link took 16.97 seconds. Scratch binary SHA-256 `613a242da2ee42fe98f262ac434a3dbd0c26790b2666232c16fed7c53c3a830d`; exact source/object identities in scratch identity.json.

Validation:

- Extracted exact macro instruction sequence with harness-supplied RSI: 11 independently authored expected literals x scalar/inline x 16 destination alignments. Source ends at an unreadable guard page (inline has no source NUL; scalar has exactly one); output ends at a guard or canary tail. Passed exact bytes, no NUL write, prefix/suffix canaries and destination-pointer advancement. This also verifies the CRLF header length rather than assuming NASM escaped-string counting.
- Both scalar and inline passed the tiny-fixture native integration suite on port3198: auth/Unicode/FKrollback/duplicate likes/fragmentation/chunking/continue/framing/SIGKILL persistence.
- Both modes and reference/scan-ranges queries passed feed/post differential fixtures containing ties, reversed ID/timestamp ordering, quotes, Unicode, newline, backslash and embedded NUL. Compared parsed JSON against independent SQL rows, key order explicitly, and byte-for-byte scalar versus inline wire output. A first Python JSON byte oracle used short newline escape while the server validly emits `\u000a`; corrected the oracle to semantic SQL comparison plus exact mode-to-mode bytes. One first-pass inline-range startup failed generically; complete sequential rerun passed all four cases. No performance conclusion follows from these correctness passes.

Harnesses/logs: scratch literal-harness.asm/.py/.so, integration-{scalar,inline}.log, feed-differential.py and feed-differential-results.json. No saturation or population tests were launched by this agent. Before adoption, alternate scalar/inline controls within this identical binary/SQLite build, then qualify package/full workload for any selected default.


## PR #82 capacity-target review and collection-policy proposal

Root identified a published unchanged-k6 five-minute **365,500-user** pass for pinned #82. That author-documented result expands the competitive target; it is not automatically comparable to our host. Root is running a matched local qualification with separately labelled generator GC/memory controls. Our historical 275k result cannot establish superiority over this target.

Pinned #82 source (`63eeda6655a0ba12f763b337545ae36041ffdbf3`) differs from NASM in two important ways beyond language:

1. Multiple issuers can wait for their rings concurrently while SQLite remains serialised. NASM's single owner applies configured 32-completion / 250-us collection to each active ring (`uring.inc:715-725`); six active rings at a 365k retained population can incur up to six such waits in one sweep. Existing CQ work can also be delayed waiting for more completions. This is an explicit architectural hypothesis, not measured attribution of a failure.
2. Rust's `uring.rs:479-500` commonly parses directly from provided receive bytes when no partial input exists; it allocates saved input only for a retained remainder. Its `Buffer::reserve` (`75-99`) starts at 4 KiB and grows to actual response length. NASM unconditionally copies into a 32-KiB input slab and reserves a 64-KiB output slab before parsing. The 64-MiB output budget therefore permits only 1,024 reserved outputs, regardless of actual wire length. Reducing allocation/copy size is a larger but more invasive opportunity: transient provided-buffer lifetime must survive parsing/SQL until all references are consumed, and partial uploads still need guaranteed completion reserve. Do not optimise by weakening that guarantee.

The optional range-feed algorithm is materially similar in #82 (`db.rs:319-394`) and our bounded `ranges.inc`: selected membership/order, three scans, stable scratch. Our implementation additionally bounds skew at 4,096 scanned likes. Earlier local paired measurements rejected its default adoption. No justification to repeat an already-losing SQL rewrite without new evidence.

Prepared an isolated **collection-policy** proposal, fresh baseline-derived nine-source copy in `build/experiments/collect-policy/src`, unapplied `research/collect-policy.patch`. `git apply --check` passed. Controls:

- `--collect=baseline`: unchanged default policy.
- `--collect=ready`: when live CQ already contains work, enter with min_complete=0/no timeout instead of waiting for additional completions; still submits pending SQEs and services cooperative task work.
- `--collect=round`: ready policy plus at most one configured timed wait during each complete owner sweep across rings. Later rings use immediate task service in that sweep.

All collection happens at the existing pre-transaction location. Frozen CQ tail, bounded processing, batch COMMIT and reply release remain unchanged. No descriptor, buffer ownership or generation lifecycle is modified. Runtime CLI controls add no environment variables. Source prepared only; no assembly/link or performance run launched while root's population generator is active.

Risks: round mode may reduce transaction-batch density, increase enter/commit calls or idle-loop CPU. The first eligible ring receives the wait rather than round-robin ownership; this can change fairness and must be measured per ring. Cooperative task work can produce new completions during immediate enter, which is intentional; the frozen tail is captured afterward. A nonempty CQ indicates work but not necessarily useful live requests (stale/close entries still consume bounded processing). No-wait does not mean busy-spin is automatically acceptable: retain epoll sleeping when the sweep performs no work. Before any adoption, compare per-ring enter duration, CQ depth/service gap, requests/transaction, CPU/request, idle CPU, p99 and close/retry behaviour, then matched official-workload qualification.
