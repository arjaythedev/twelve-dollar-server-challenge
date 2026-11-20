# Ideas from the current leaders

Status: B1 (bounded single-shot accepts) and U1 (buffer-ring registration retry) retained as
correctness and portability changes; no speed gain is claimed for either. Five other techniques
were studied and not adopted. Source baseline: `d7db355` (production C identical to the
lower-memory variant at `b9a1e99`). Candidate: `747564d` + `dc6bab8` (production C), plus the
test fix `d04e091`.

## Sources

Read from upstream `main` at `f4485ff` (2026-11-13), the leaderboard in `LEADERBOARD.md`
and each entry's README and source:

| Entry | Score | What was read |
|---|---:|---|
| `rust-iouring-kensac` #264 | 310,000 | README; `src/main.rs` accept depth/margin, `SO_REUSEPORT` listeners, buffer-ring registration, read recovery |
| `mojo-iouring-oh-meh` #257 | 304,000 | README; one-shot accepts bounded by free slots, provided-buffer fallback |
| `rust-iouring-rkkautsar` #355 | 300,000 | README; hybrid multishot/single-shot receive |
| `fsharp-uring-delneg` #398 | 300,000 | `src/Ring.fs` buffer-ring registration (list of affected Ubuntu kernels) |

Only general mechanisms were considered, as in [09](09-pr158-ideas.md): nothing ordered by,
trained on or sized to `load.js` or the scoring thresholds.

## B1 bounded single-shot accepts: retained

**Problem.** A multishot accept with `IORING_FILE_INDEX_ALLOC` takes a connection off the
listen queue before it looks for a free direct-descriptor slot. When the table is full, the
kernel completes the accept with `ENFILE` and closes the socket the client already sees as
established. The baseline kept one multishot accept armed per ring and moved to another ring
2,048 slots before the table filled, cancelling the old accept. That is safe only while fewer
than 2,048 connections arrive between a loop iteration's check and the cancellation taking
effect, and once all 16 tables were full it kept accepting into a full table. #257's README
reports exactly this loss with multishot accept ("one connection per ring"); #264 and #257
both use single-shot accepts bounded by free slots.

**Change.** Each accept is single-shot and reserves one table slot from submission until the
transport consumes its completion: `nopen + accepts ≤ nslots` on every ring, always. Up to
`min(slots/16, 512)` accepts are in flight on the accepting ring. Replenishment moves to
another ring, or opens one, when the current ring is within twice that depth of full; accepts
already in flight on the old ring finish there and fit in its reservation, so nothing is
cancelled. A new ring opens only when every open ring is within its margin. Once all 16 rings
exist, any single free slot anywhere is used. At total capacity no accept is armed and new
connections wait in the listen queue until a close releases a slot. The now-unused cancel
constructor was removed from `spec/Feed/Sqe.lean` and the generated `sqe_gen.h`.

The depth bounds how many accepts complete per loop iteration. #264 measured that 64 in flight
could not keep up with a ramp of a few thousand new connections per second on a 1 vCPU box and
that 512 did; this transport uses the same 512 on its single listener.

**Tests.**
- `tests/metal/accept_cases.c` (ASan/UBSan) runs the transport's own `fill_accepts`,
  `accept_balance`, `ring_init` and completion handling against a mocked kernel boundary
  (io_uring syscalls, ring mappings, eventfd). Single ring: table sizes 1–1,024 with 2–32-entry
  submission queues, failed accepts releasing their reservation, three fill/close/reuse cycles.
  Sixteen rings, table sizes 1, 3, 16, 64 and 300: on-demand opening (an emptied ring is
  refilled before a new one opens), every table filling completely, no accept armed at full
  capacity, a slot freed in a non-accepting ring taking the next connection, and 20,000 steps of
  random accepts, failures, closes and close completions. Every step checks the reservation
  invariant, that every successful accept finds a free slot, and that some accept is outstanding
  while any slot is free. Seven deliberately broken variants of the transport (no free-slot
  bound, an off-by-one bound, no reservation release, no final scan, switching to a full ring,
  opening rings eagerly, no registration retry) each fail the test.
- `tests/metal/accept_live.py` runs the real server in a private network namespace under
  `RLIMIT_NOFILE=256`: 16 rings × 256 slots, 4,096 persistent connections. The 4,097th
  connection must stay queued while the tables are full and be served once a connection in the
  first ring (not the accepting one) closes; then all 4,096 connections are healthy.

**Measurements.** Interleaved A/B/B/A on bigmachine (Xeon E-2176G, kernel 6.8.0-100), the same
harness as earlier phases: one server CPU, 2 GiB, no swap, 65,535 descriptors, UID 65534, a
fresh seed copy per run, load on the other cores over loopback in a private network namespace,
no other load generator or compiler running. A = `d7db355` (multishot accept), B = `dc6bab8`.
Production C at `d04e091` (the final test fix) is identical to `dc6bab8`.

Closed loop, 4 threads × 32 keep-alive connections, mixed workload, 30 s:

| | A | B |
|---|---:|---:|
| Requests/s | 178,386; 171,370 | 179,194; 175,864 |
| Server CPU per request | 5.58; 5.82 µs | 5.59; 5.69 µs |
| Errors | 0; 0 | 0; 0 |

Native `load.js` replay, 300,000 users, 60 s ramp (about 5,100 new connections per second),
90 s hold:

| | A | B |
|---|---:|---:|
| Hold CPU per request | 14.01; 15.09 µs | 14.28; 14.61 µs |
| Hold p95 / p99 | 1.76 / 9.01; 2.08 / 7.35 ms | 2.08 / 9.84; 1.86 / 7.14 ms |
| Whole run p99 (includes the ramp) | 10.06; 8.62 ms | 10.61; 8.09 ms |
| Worst 1-second p99 during the ramp | 81.8; 54.2 ms | 114.3; 37.8 ms |
| Most connections waiting to connect | 10; 4 | 3; 2 |
| All 300,000 connected at (each user's first request waits 0–5 s) | 65 s; 65 s | 65 s; 65 s |
| Errors (connect, timeout, reset, status) | 0; 0 | 0; 0 |
| Peak cgroup memory | 531.9; 530.3 MiB | 531.8; 534.8 MiB |
| Rings opened | 5; 5 | 5; 5 |

Both are equivalent within the run-to-run noise: single-shot accepts at a depth of 512 keep up
with this ramp, and the ring handover happens at about 63,940 connections per ring. No
throughput, CPU or memory change is claimed.

One five-minute hold of B, 300,000 users, 60 s ramp, 300 s hold (the scored hold's length;
everything else about it is local): 10,809,960 requests, 0 errors, all 300,000 connections
held, 31,749 req/s, hold CPU 15.25 µs per request, hold p95 / p99 2.17 / 9.04 ms, worst
1-second p99 107 ms, peak cgroup memory 580.1 MiB (the database's cached file pages grow over
the longer run), 5 rings. Raw summaries: [results/current-leaders.json](results/current-leaders.json).

## U1 buffer-ring registration retry: retained

Some Ubuntu 6.8 kernels (`6.8.0-136` and later per #264; #398 lists -132, -135…-142,
-145…-149) carry an inverted check in `io_register_pbuf_ring`: they refuse a registration
whose reserved words are all zero and accept a nonzero `resv[0]` ([Launchpad
#2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)). The scoring box is a
Synteq VPS on Ubuntu 24.04's 6.8 kernel, and #201's README reports the retry exercised on a
Synteq VPS running `6.8.0-147`. On an affected kernel the baseline exits at startup with
`register buffer ring: Invalid argument`.

`register_buffers` first registers with the specified ABI (all reserved words zero). Only if
that fails with `EINVAL` does it retry once with `resv[0] = 1`, then restores the field; any
other error is reported as before. A correct kernel never sees the retry. The test emulates a
correct kernel, an inverted one, and kernels failing with `EPERM` or always `EINVAL`. It was
not reproduced on an affected kernel: bigmachine runs the unaffected `6.8.0-100`.

## Studied, not adopted

- **Several `SO_REUSEPORT` listeners** (#264: eight). Each listen queue is capped at
  `net.core.somaxconn` (4,096), so eight queues absorb a longer accept stall. This server's
  loop has no long stalls from checkpoints (they run on their own thread), and the local
  300k ramps above had at most 10 connections waiting to connect and no connect errors. Not
  adopted without evidence of a backlog overflow.
- **Bounded read recovery after `ENOBUFS` and drain-first serving** (#264, #355). They recover
  from stalls in which every connection's multishot receive runs out of provided buffers. Here a
  receive that ends with `ENOBUFS` is re-armed after the batch; without a measured stall on this
  server the extra read queue and per-pass bookkeeping were not added.
- **Write holds around checkpoints** (#264). This server's checkpoint thread runs `PASSIVE`
  checkpoints and the log restarts when a write begins after a complete checkpoint; the 64 MB
  last-resort checkpoint never ran in the measured runs ([09](09-pr158-ideas.md)).
- **Waiting on ring 0 for 256 completions or 1 ms** (#264). This server batches by pausing
  250 µs after a light iteration; the earlier delay sweep
  ([optimization trials](../tests/optimization-results.md)) found the alternatives within
  control variance, and no new comparison was made.
- **Page-cache reads with mmap off** (#264's 64 MB cache). Measured and rejected in
  [09](09-pr158-ideas.md) P1 for this server (no gain at 300k, and it costs the memory this
  variant saves).
