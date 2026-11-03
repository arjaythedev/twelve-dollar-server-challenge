# Isolated bounded-timer experiment — 2026-10-14

Production src/, bin/ and shared build outputs remain unchanged. The proposal is
still unapplied to the measured baseline. Scratch lives under
`build/experiments/bounded-timers/` (ignored generated artifacts).

## Exact artifacts

Scratch server SHA-256:
`118b7fb0ae11ce49e74b3ae09e76a1a9ddedd89329e0b9c27fd09528d222a03a`.

Changed scratch sources:
- server.asm `bab499ce25bffb294e662c931a9d81980dd70c57b3fbb872703a6ffc01a4ceac`
- uring.inc `3b139e074413ac8b0d3ce0e3a1057831d4b83097ac7f6d3d5080e3cd648987b8`

All other seven source hashes match the measured baseline. The new BSS timer
state occupies 16 bytes including padding, preserving old qword alignment.
Reused SQLite object, without recompiling SQLite:
`44e0a3b1ba9200da9a3f7c7dffe7f40f90600287cb52bf2cc7ff9694dcc35d17`.
NASM 2.16.01 and GCC Ubuntu 13.3.0 linked with -no-pie -flto -lm.
Full identities: scratch `identity.json`. The updated unapplied patch includes
this padding and reproduces the tested source changes.

## Passed focused checks

The test-only shared-library harness extracts the exact scratch timer functions,
with a mock close function; production hooks were not added. It verifies:

- 320 synchronized expired records, <=128 closes per call and same-second resume.
- 320 future-only records, <=128 inspections per call even when no node expires.
- Future head with due tail: due tail expires and future head survives.
- Cursor touch, successor unlink, old ID close/reuse: no early new-generation expiry.
- Billion-second gap:32 ticks per call, one wheel,8 rounds, no duplicate closes.
- Callee-saved registers/stack/DF for empty and close paths.
- Guard pages around connection storage and unchanged unused-tail canary.

Existing primitives passed 2,000 randomized JSON cases plus SHA/HMAC/base64url,
NumericDate, ABI, guard-page and canary checks in the isolated directory.
Existing integration passed on both native io_uring and epoll with tiny canonical
schema: authentication, Unicode, FK recovery, duplicate likes, fragmented/chunked
input, 100-continue, framing and acknowledged-write SIGKILL recovery.

Native real-time timeout check, both backends together:
- 300 partial-header clients per backend had retired by 33.1 seconds.
- A partial header refreshed at 28 seconds survived the old deadline and completed
  successfully after 33 seconds.
- An ordinary idle keepalive connection remained reusable after 33 seconds.
- 1,282 independent health requests had zero errors; maximum observed latency
  was 6.773 ms while the synchronized timeout population drained.

Raw scratch files: timer-harness-results.json, native-validation-results.json,
primitives.log, uring/epoll-integration.log, uring/epoll-native.log. Two initial
harness setup assumptions were corrected (health JSON shape and --epoll flag);
these were test-harness errors, not server failures.

## Limits and adoption gate

These focused tests do not prove a capacity gain. No performance/capacity run was
launched by this agent. The real-time test bounds retirement by 33 seconds but does
not establish precise millisecond timeout scheduling. 120-second idle expiration,
30-second genuinely blocked sends, physical storage faults, sustained slot reuse
and full constrained population confirmation remain additional gates.

The native synchronized test supports continued I/O progress; source review
confirms pending work skips coalescing and idle sleep in io_uring and forces zero
idle-wait timeout in epoll. A syscall-trace proof of every sleep path was not run.

Review/apply to a separate candidate only after preserving baseline evidence,
then run official 42 checks and resource-limit/capacity qualification under a new
artifact identity. Historical 275k success belongs to e511, not this scratch build.
The fresh root-controlled matched 275k short rerun failed for both NASM and #75;
this patch has not yet been measured against that regression. No winner claim.

## Baseline reachability clarification

The future-head/due-tail unit fixture is defensive coverage for interleaving I/O
with the proposed resumable scan. It does not demonstrate a reachable bug in the
original unbounded loop: original expiry drains before I/O, and its 120-second
maximum timeout is smaller than the 256-bucket revolution. No real baseline
reproducer was constructed. The definite baseline issue is unbounded expiry
work before returning to I/O. The 128-record and 32-tick caps are count bounds only,
not a hard elapsed-time guarantee; no periodic wall-clock check was added.
