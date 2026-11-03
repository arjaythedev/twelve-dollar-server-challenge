# Bounded optimization session, 2026-10-14

Scope: additional 30-minute optimization window; no commits, pushes, paid
provisioning or production changes. All artifacts stay in this submission.

## Bounded range-query experiment

--query=scan-ranges prepares three statements at startup: selected IDs in
timestamp/ID order, an indexed range of like post IDs, and post/author rows.
A request-local 256-entry membership map excludes nonselected IDs. Serialized
rows are copied into stable scratch before the next SQLite step/reset and
reassembled in selected order. An explicit transaction covers all three reads
unless the frozen batch already owns one. Spans of 256 or more fall back;
like skew is bounded by falling back after 4,096 scanned likes.

Passed: 18 differential fixture/mode cases (empty, dense/interleaved, sparse,
skewed and signed IDs), the 27 official-test/query/serialization/transaction
combinations with scan-ranges overriding their query control, and integration.
These 27 cases are not 27 distinct range algorithms. Signed IDs exposed and fixed
an existing unsigned NASM ID serialization bug; ordinary challenge IDs remain
positive.

Three paired 15-second saturation trials per control:
reference 50,778 versus ranges 47,883 requests per server CPU second, about 5.7%
worse. No default change. Raw: results/paired-ranges-screen.json.

## Statement rollback instead of explicit per-request savepoints

Each production write is a single INSERT. The fixed schema has immediate
constraints; INSERT uses default ABORT conflict handling, with the like statement
explicitly doing nothing on a duplicate. SQLite's ABORT handling rolls back that
statement while preserving earlier statements in the surrounding transaction.
This experiment removes SAVEPOINT/RELEASE calls within shared batches only.
Per-request transactions remain explicit BEGIN/COMMIT.

On a constraint failure, the server verifies that the transaction remains active.
Unexpected errors or automatic transaction rollback invalidate the whole frozen
batch: no affected successful write or dependent read can escape with success.
Unknown failures are deliberately not treated as harmless constraint failures.
Missing-post likes finish their unchanged transaction normally.

Primary source checked through Context7: SQLite src/vdbeaux.c sqlite3VdbeHalt,
src/sqliteInt.h OE_Abort, and src/vdbe.c OP_Halt. The measured dependency remains
the pinned SQLite 3.54.0 build; no version/compiler-option comparison was added.

Passed with --savepoints=off: integration (including duplicate likes and process
crash recovery); injected failed COMMIT and transaction-wide automatic rollback
in all three transaction modes; frozen bursts of 32 valid writes + 32 FK-invalid
writes + 32 reads in writes-only and shared-read/write modes, with acknowledged
rows and successful reads checked against the database after SIGKILL.

Three alternating 15-second pairs: savepoints on 48,039 versus off 49,441
requests per server CPU second, about 2.9% improvement. Every pair favoured off,
but trials are short. Raw: results/paired-savepoint-screen.json.

## Transport controls

One ten-second trial each, same image and fresh seed: multishot receive improved
requests per CPU second from 52,534 to 58,460 (about 11.3%). Immediate processing
was 53,090; batch64 51,691; batch128/250us 48,553; batch128/1000us 51,512;
TCP_NODELAY 51,696. Larger waits reduced wall-clock throughput substantially.
The combined multishot/vector/no-savepoint sample was 57,792. These are noisy,
single-trial controls, not capacity scores or proof that vector quoting helps.
Scalar quoting, 32 completions/250us and NODELAY off remain preferred pending
confirmation. Raw: results/transport-controls-screen.json.

Multishot plus no-savepoint passed aggregate input-pressure recovery and a
5,000-connection frozen burst with exact reference JSON. Raw:
results/optimized-pressure.log and results/optimized-burst.log.

## Qualification boundary

All throughput diagnostics above use same-host autocannon outside the Docker
server's one-CPU/2GiB/no-swap quota. The host kernel is 7.0.0-38-generic, not the
organizer's target. No process pinning or host kernel changes. All generator
transport/status/JSON checks passed. The unchanged official k6 workload is used
separately for population confirmation; its check failures are inspected
explicitly. Whole-field maximum ranking and organizer victory remain unproven.

## First 275k population screen

Same-host official k6 script, 60-second ramp and 30-second hold, no warmup,
multishot plus no-savepoint, scalar JSON. 2,373,475 requests; zero HTTP/check
failures, p95 25.604 ms, p99 1,187.630 ms, max 4,166.852 ms. Thus FAILED the
p99 threshold; no 275k capacity claim. Server peak 1,465.1 MiB, no OOM, and no
CPU-quota throttling recorded. Generator peak 76.40 GiB/88 GiB, no OOM, with
brief sampled CPU use near 44 logical cores on this 48-thread host. Generator
contention is a possible confounder, not an established explanation. A matched
one-shot control is required before attributing the failure to receive mode.
Raw: results/optimized-275000-screen-evidence.json and generator resource record.

## Matched one-shot control and chosen candidate

The matched one-shot/no-savepoint 275k short screen passed: 2,412,295 requests, zero HTTP/check failures, p95 17.220 ms, p99 90.902 ms, max 2,777.829 ms; server peak 1,389.9 MiB. One pair cannot establish a general receive-mode ranking, but it does not justify adopting the latency-failing multishot control.

Chosen defaults retain one-shot receive, scalar JSON, reference SQL, 32-completion/250us coalescing and NODELAY off; redundant batch savepoints are disabled. --savepoints=on remains a control.

Current packaged binary SHA-256: e511302626f6f0e06c2981aeb19e4d90923afeebf497ff711373d791f9a8491e.
Image: sha256:4438adff52e1ed4db5e941ae1f3ab5ee0b64d4cfd6ae850694969e4d7186fb16.
Official 42/42 checks passed on io_uring and epoll; unchanged startup runs as normal user in foreground, healthy in under 0.25 seconds, with compile-time I/O counters disabled. The final confirmation completed; see the result below.

## Final confirmation and stop for today

Final 275k confirmation: PASSED. Unchanged bench/load.js, 1,000-user
two-minute warmup, 60-second ramp, five-minute 275,000-user hold and standard
ramp-down. 10,264,833 measured requests, zero HTTP failures and zero check
failures; p95 16.233 ms, p99 65.394 ms, max 2,436.599 ms. Peak server memory
1446.0 MiB/2 GiB; no OOM. The current unchanged
start.sh and default settings were used.
Generator peak 85.14 GiB/92 GiB,
no generator OOM. Percentiles cover the measured ramp/hold/ramp-down together.
This is a selected-count local confirmation, not a maximum search or organizer
score. Exact runtime binary: e511302626f6f0e06c2981aeb19e4d90923afeebf497ff711373d791f9a8491e.
Exact image: sha256:4438adff52e1ed4db5e941ae1f3ab5ee0b64d4cfd6ae850694969e4d7186fb16.
Raw: results/optimized-275000-confirmation-evidence.json, its k6 summary/log,
and generator resource record.

User requested no more tests today. No PR, commit or push was performed.
The local review draft was subsequently transferred into the PR description and removed. Whole-field superiority remains unproven.
