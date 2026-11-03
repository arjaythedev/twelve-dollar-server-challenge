# Pinned field collection and source observations

This is new implementation-era research. Earlier planning documents were removed;
the supplied conversation plan remains the handoff. No old plan was reconciled.

The inventory retrieved 2026-10-13 contains 59 pull requests through #61.
All affected submission directories are collected from their pinned Git trees,
with individual file SHA-256 hashes and original executable modes. A PR without
submission files is retained in the inventory. Collection itself is read-only. PR #9 has subsequently been built and tested
in an isolated Docker image; no contender installer runs on the host.

Reproduction from this submission directory:

```bash
python3 tools/competitors.py
python3 tools/field.py --fetch all
```

Ignored results/competitors.json records original descriptions and immutable heads.
results/field/<number>-<head-prefix>/manifest.json records complete affected
submission trees and downloaded source identities. Collection is not an eligibility
verdict, successful build or competitive ranking. Every candidate still needs
root installation, normal-user build, startup/configuration checks, semantics and
comparable unchanged-load qualification. Unavailable dependency pins are recorded
as build failures rather than silently upgraded for the comparison.

| PR | Pinned head |
| --- | --- |
| [#9](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/9) | 3d92a3b349bdf8abb0425fce990fdfb8ab7dfdc3 |
| [#19](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/19) | bd517fa7c8a57b135579ae02fdddb6196b4cd9a4 |
| [#26](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/26) | 35cc55c85fa837cd703367a740e0b8eeb6b825b4 |
| [#32](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/32) | 647f962dc2eab8fc64e2f112728ac5dfb20154c5 |
| [#42](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/42) | bb05a6f52932e955954393aba5fa681a09f9f780 |
| [#51](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/51) | 39ee48dfe83ee08e5145d7b0de1f0fe99a6e7e94 |
| [#55](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/55) | 98b16d56372f751793a0ca5d518e2940094639e4 |
| [#60](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/60) | 92226df06a5bc9f450424a43ed24693a8be86394 |

## Lessons applied or awaiting measurement

- #51 server.ts: freeze the pending write queue, open its transaction afterward,
  distinguish statement failures from transaction-wide rollback, and complete
  replies after commit. NASM implements these controls with request savepoints and
  whole-batch failure handling. Its injected commit/rollback tests cover successful
  writes and reads observing tentative writes. TS also retires completed responses
  near descriptor pressure; NASM still needs broader retirement/exhaustion proof.
- #51 uses SQL JSON for feed and typed serialization for individual posts. NASM
  compares SQL/NASM/hybrid explicitly; local saturation favours NASM overall, while
  the hybrid has not won. Framework-language assumptions do not select the result.
- #19 main.rs explicitly serializes the shared SQLite/Auth owner with Mutex<App>
  across io_uring issuer threads. Its db.rs has correlated-count and three-query
  range paths. A single issuer is a NASM baseline, not an enduring restriction.
- #55 db.c preserves selected IDs and their order separately, checks membership
  inside the ID span, and falls back when IDs are far apart. This is more specific
  than the current NASM range CTE; the CTE's poor timing does not reject that whole
  algorithm. Large like populations remain a range-scan hazard even in narrow spans.
- #32 server.cpp sets 128 completions and 6 ms, but gates its wait behaviour using
  activity and population. Copying these numbers into an unconditional wait is not
  equivalent. Our 64-active-client screen demonstrated that mismatch sharply.
- #60 Db.cs uses a separate SQLite connection for passive checkpoints. Investigate
  this only with locking/thread-safety changes and profiles; NASM's exclusive single
  owner cannot acquire a second checkpoint connection unchanged.
- #9's current folder name contains inmem, but source opens the supplied database
  and describes mmap/OS page cache. A name is not evidence of forbidden caching.
  Conversely its startup source does not explicitly enable foreign_keys; this needs
  semantic/eligibility review rather than relying on the official happy-path suite.

SQLite remains pinned at 3.54.0, with no version or broad build comparison campaign.
Latest does not establish fastest; optimisation claims here require measurements.

## First packaging/correctness qualification

PR #9 at the pinned head passed its unchanged root installer and normal-user
build on Ubuntu 24.04, became healthy in 0.314 seconds as UID 1001, and passed
42/42 official checks under one CPU, 2 GiB, no swap and nofile=65,535. Image:
sha256:29a57ab7e502997d035e035651c673f3ff61c4510b71b2cf2591be49e26f66f6.
Raw build/test reports and qualification.json are stored beside its manifest.
Broader semantics, including foreign-key enforcement, and comparable capacity
remain unverified; this is not an eligibility clearance or ranking.

```bash
python3 tools/qualify-field.py --pr 9 --direct
```

The direct-mode harness requires prior source/mode review and never silently
converts an Nginx submission to direct mode. Source hashes are checked before
build; dependency versions are preserved. Its installer/build logs are retained.

## Additional packaging gates

Pinned #19 and #42 also passed unchanged root installers, normal-user builds,
foreground startup as UID 1001 and official 42/42 checks under the same Docker
runtime limits. Health checks completed in 0.113 seconds for each. Their exact
image identities, logs and source pins are in their per-PR qualification.json.
Broader semantics and capacity are not established by this gate. Matched 250k short screens subsequently both passed. Rust #19 had lower p99
and peak memory in that single pair; maximum capacity and five-minute matched
comparison remain unverified. See PERFORMANCE.md for exact conditions and identities.

## Live revision refresh

Fetched 2026-10-14T15:16:15.357986+00:00: 81 PRs. The previous source collection contains 59 PRs.
The following are gaps in collection, not eligibility or performance verdicts.

| PR | Earlier captured head | Current fetched head |
| --- | --- | --- |
| #33 | 2df5a26534e8ba22af37142f477239f953b57b2f | 845accd87117b4353dc706231d165a3fb9522830 |
| #44 | 576c9b79d685c2c704ed426fefe64254b1d34e75 | c5eba7b5c9fff27b5e428958bc3e45ebd1dc0375 |
| #48 | 84c404036bdcc13de73f3e24831f1796bfa666bf | 143a1abd4e783c2d1030d7746aa8e7fb04e3b032 |
| #51 | 39ee48dfe83ee08e5145d7b0de1f0fe99a6e7e94 | 6ba12bd883c0198e48c252ded0f72f72ded99c1a |
| #58 | a2169471dc8c96f0f8307f077210a08bc79c4be2 | 0600708b01e9c550143da51583d240e83efa4614 |
| #59 | f0f53e79af18dfa790c52d9d02ce33fcfb9b068f | 23310e30a3ae67223a3f06f787dd20ed1c9b80be |

Additional uncaptured PRs: #63, #64, #65, #66, #67, #68, #69, #70, #71, #72, #73, #74, #75, #76, #77, #78, #79, #80, #81, #82, #83, #84.

The primary captured heads #9, #19, #26, #32, #42, #55 and #60 were unchanged
in this refresh. The author's TS #51 has changed and needs a fresh source review.
No new code was executed. Raw: results/field-revision-refresh.json.

## Refreshed source collection completed

All 81 PRs in results/competitors-refreshed.json now have captured pinned
submission trees. Current inventory source hashes verified with zero drift and
zero missing manifests. Historical revisions remain preserved separately;
qualification-status.py selects exact inventory heads rather than counting
multiple revisions of one PR. Three current heads (#9, #19, #42) retain recorded
build/official-test passes. Collection is not eligibility or ranking evidence.
No contender code or performance test ran during this refresh.

Reproduce the refreshed report with:

```bash
python3 tools/qualification-status.py --inventory results/competitors-refreshed.json
```

## Newly reviewed #75 and #82

#75 at 4b8ba1cc1bcb412273a0f577a7578da0ec3d3791 is another NASM server.
Its common.inc sets MAX_FDS=65536 and REQ_BUF_SIZE=8192; main.asm accepts
ordinary descriptors into epoll. http.asm selects Connection: close when
active_conns reaches 15000. SQLite and OpenSSL crypto are linked libraries.
The inspected README/PR body report 42 correctness tests, without a comparable
capacity result. Descriptor limits constrain simultaneous sockets, not total
virtual users: connection retirement can serve users through churn. Consequently
our 275k retained-population confirmation does not itself prove a win over #75.
The PR head was rechecked live and matched; no server was run.

#82 at 63eeda6655a0 is a follow-up by Rust #19's author. Source comparison shows
db.rs, main.rs and net.rs unchanged from the captured #19 head; uring.rs changed.
Provided buffers increase from 1024 to 4096 per issuer, CQ entries from 8192 to
16384 and SQ entries from 1024 to 4096. It adds generation-tagged ENOBUFS retry
queues, publishes returned buffers before intermediate SQ submission, and
budgets retries from receive-buffer completions (minimum 32, maximum NBUFS/2).
Pending retries also prevent the otherwise two-second idle wait. Its README
explicitly attributes older performance results to the previous revision;
those results must not be assigned to the new transport code automatically.

This increases the priority of qualifying #82 separately from #19. Our own
measured candidate already defers ENOBUFS retries but uses 1024 buffers and an
8192-entry CQ per ring, fixed 128-entry retry budgets, and one-shot receive.
A future diagnostic experiment can compare progress-based retries and larger
buffer/CQ pools against the current control before reconsidering multishot.
Fourfold provided buffers across eight NASM rings add about 48 MiB of payload
storage, plus descriptor/CQ costs; that memory must be included. No proposal
has been applied to the measured NASM source, and no new tests ran today.
