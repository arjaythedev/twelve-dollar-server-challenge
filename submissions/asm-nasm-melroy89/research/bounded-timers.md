# Proposed bounded timer expiry — unapplied

Patch: bounded-timers.patch. Production source/bin/build remain unchanged.
`git apply --check --directory=submissions/asm-nasm-melroy89` passed against the
current original checkout. No assembly build or runtime test has been performed
for this proposal; review and validation are required before adoption.

## Why

Current expire_timers drains an entire expired bucket before returning to I/O.
A synchronized population can therefore queue hundreds of thousands of closes
in one round. This unbounded owner-loop operation is a definite fairness gap.
The original loop stops scanning a bucket when its head is in the future, but
no reachable original-API counterexample has been established: it drains all
expiry work before I/O, and timers span at most 120 seconds on a 256-bucket wheel.
A future head above an overdue tail becomes a relevant defensive case after
introducing resumable expiry, because I/O can refresh/insert timers between
slices. The synthetic unit fixture tests the proposed traversal; it is not a
reproduction of an existing baseline defect.
Merely limiting a counter is insufficient: last_tick is advanced before the
bucket completes, so the remainder must be resumed even when now_sec is unchanged.

## Mechanism and invariants

- Budget each call to 128 examined connection records and 32 bucket advances.
  Future entries also consume the node budget; empty/catch-up buckets consume
  the tick budget. These initial constants are experimental controls, not proven
  optimal values or hard wall-clock latency guarantees.
- Persist ID-plus-one cursor and active-bucket flag. last_tick identifies the
  active bucket until scanning finishes. Process suspended work before checking
  last_tick against now_sec, so same-second rounds resume correctly.
- Advance cursor before close_connection can clear/recycle a record. timer_remove
  advances a matching suspended cursor before unlinking it, covering timer_touch,
  external close and eventual ID/generation reuse between rounds. No raw pointer
  survives reuse. The owner thread serializes every list/cursor mutation.
- A newly inserted head is not part of the suspended traversal. Its 30/120-second
  deadline is in the future, so waiting for its deadline bucket is appropriate.
  Removing any other pending node repairs neighbor links normally. Removing and
  readding the cursor advances past it before insertion, so it cannot form a
  stale cursor or cause a refreshed connection to expire early.
- Skip future nodes and continue scanning, rather than abandoning the bucket.
  Compare each actual deadline with current monotonic now_sec before closing.
- For a gap exceeding 256 seconds, start the most recent full wheel revolution.
  Every linked node lies in one of these 256 buckets, so historical empty ticks
  need not be visited. The 32-bucket-per-call limit still applies. This assumes
  the present maximum 120-second timer horizon and fixed 256-bucket wheel.
- timer_work_pending suppresses the epoll fallback's one-second idle sleep and
  io_uring completion coalescing/idle sleep while backlog remains. Each round
  still services I/O before the next bounded expiration slice. Otherwise a
  275k bucket could take thousands of seconds to clear despite bounded slices.

No timeout duration changes: timer_touch still chooses ordinary idle120 and
stalled input/output30. Retirement may occur after its deadline while backlog
is drained, never before it. Existing io_uring cancellation/direct-close ordering
and reference-lifetime retirement remain intact.

## Required targeted validation

1. Tiny deterministic timer-list harness: more than128 expired IDs in one bucket;
   call expire_timers repeatedly with exactly unchanged now_sec. Assert at most128
   examined records per call, eventual full expiry, no duplicate close and valid
   intrusive links. Include empty buckets and a multi-hour clock jump; assert no
   more than32 bucket advances per call and eventual catch-up within one wheel.
2. Put a future head ahead of expired tail entries in the same modulo bucket.
   Assert expired tail closes and future head survives until its real deadline.
3. Between slices, touch the cursor connection, remove its successor, remove a
   preceding future node, then close/reuse the cursor's old ID with a new generation.
   Assert refreshed/new connections survive, remaining due nodes expire exactly
   once, and cursor/list references never target the removed generation.
4. Public-wire synchronized expiry with several thousand idle/stalled clients,
   concurrently issue health/feed requests from an active control connection.
   Check120-second ordinary idle and30-second partial-input timeout bounds with
   an explicit tolerance for scheduling; assert active requests progress between
   expiry slices. Separately fill response send buffers so stalled sends exercise
   the same30-second path and resources drain after cancellation.
5. Timeout extension: touch idle just before120 and continue a partial upload
   just before30; each must survive its old deadline and retire at its refreshed
   deadline if no more progress occurs. Complete admitted uploads during pressure
   and confirm input/output counters and fixed slots recover.
6. Both epoll and one-shot io_uring official/integration checks, then low-nofile
   and generation-reuse stress. Build a distinct new artifact identity and only
   afterwards run a matched performance confirmation; retain historical275k
   e511 result without transferring it to the changed binary.

This proposal targets a known unbounded owner-loop operation. It is not evidence
of higher capacity until implemented, validated and measured.

## Reachability qualification

Under original sequencing, creating a deadline t+256 with a 30/120-second timeout
requires an I/O round at least 136 seconds after t. That round first updates the
clock and fully drains the wheel through its current time, retiring a due tail
at t before insertion. Likewise, no I/O runs partway through the original drain.
This supports the invariant that the synthetic future-head/due-tail combination
is not shown reachable in the baseline. The new resumable scan intentionally
relaxes this sequencing and therefore must tolerate mutations between slices.

The 128-record/32-tick budgets bound counts, not elapsed time. close_connection can
perform variable work, and io_uring SQ management/kernel activity may stall. No
hard wall-clock guarantee is claimed. If profiles justify it, a later separate
proposal could check CLOCK_MONOTONIC every 16 records and yield after a measured
time budget while retaining the same cursor invariants. No such clock check is
implemented in this patch.
