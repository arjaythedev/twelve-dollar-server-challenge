# Lean → C: a derived feed server, batched transport

A variant of [`lean-derive-hargup`](../lean-derive-hargup) (#153, scored at `fd2c149`): the same
Lean specification, proofs and generated application layer, with a reworked transport — bounded
receive recovery driven by a Lean-generated state machine, a safe submission backlog, ordered
close, and passes paced at 1 ms. What changed and why is under [Changes from #153](#changes-from-153).

The API is specified in **Lean 4**: the domain, a global invariant over the stored data, the
read plans (with proofs that they compute the specified results), and the wire contract.
A Lean program then **generates the C application layer** from that specification. It is
compiled with SQLite and a small io_uring runtime into one binary. The build runs on the
benchmark box: Lean's kernel checks the theorems, and the generator writes the C that gets
compiled.

The premise: a framework commits to generality before it knows the application, and every
request pays for it. Here the application is known completely, so routing, validation,
serialization and even query execution are specialized to it.

| | |
|---|---|
| Language | Lean 4.35.0 (specification, proofs, generator) → C (gcc, `-O2 -march=native`) |
| Framework | none: the application layer is generated |
| Database | SQLite 3.54.1 amalgamation, compiled into the binary (`THREADSAFE=2`) |
| Transport | io_uring (Linux ≥ 6.1), one thread, HTTP/1.1 keep-alive |
| **Nginx** | **no: serves the internet directly** on `HOST:PORT` (`0.0.0.0:80`) |
| Dependencies | Lean toolchain and SQLite, pinned by version and SHA-256; nothing else |

## What is derived

`spec/` is a Lean package with no dependencies beyond Lean's core library.

| Lean (`spec/Feed/…`) | What it contains | What it becomes in `gen/app_gen.c` |
|---|---|---|
| `Model.lean` | users, posts, likes; `feed`, `getPost`, `newPost`, `like` as plain functions | the meaning of every endpoint; also the test oracle |
| `Invariant.lean` | `Db.Inv`, the startup check `checkInv`, the post-insert check `insertOk` | `app_check_inv`, the check after `INSERT … RETURNING` |
| `Plan.lean` | `feedPlan`, `postPlan` over B-tree primitives (tail scan, rowid seek, one index range scan) | `feed_tail`, `post_by_id`, `range_counts`, `user_name` |
| `Contract.lean` | routes, check order, statuses, error messages, JSON shapes | route matcher; every error response as complete constant bytes (Content-Length computed in Lean); serializers with all constant JSON merged |
| `Schema.lean` | `schema.sql` as data | record field positions and type checks; SQL text; a startup check of `PRAGMA table_info` |
| `Transport.lean` | the per-connection receive state machine, the re-arm policy for ended receives, transport sizing as a dependent record | `transport_gen.h`: the transition tables `RECV_NEXT`/`RECV_ACT` the runtime executes, and the sizing constants |
| `Emit.lean` | the generator | `app_gen.c` (filled into `Feed/templates/app.c`) and `transport_gen.h` |

`build.sh` regenerates `app_gen.c` and `transport_gen.h` and refuses to build if either differs
from the checked-in copy in `gen/`. The checked-in copy is there to be read; the compiled one always comes from
the specification.

### The invariant, and what it buys

`Db.Inv` says three things about the stored data that the schema does not enforce:

* `dense`: post ids are exactly 1…n. Posts are only appended, and SQLite gives a new row
  `max(rowid)+1`.
* `mono`: `created_at` never decreases in rowid order. New posts get `now`.
* `authors`: every post's author row exists. Users are never written.

Under it, the feed order (`created_at DESC, id DESC`) is reverse rowid order. The feed is
then: read the last 20 rows of the posts B-tree, plus **one** range scan of
`likes_post_id_idx` for all 20 like counts. The reference plan instead walks the
`created_at` index, seeks each row, and counts each post's likes separately.

Proved in Lean (kernel-checked on every build; no `sorry`, only the standard axioms):

| Theorem | Statement |
|---|---|
| `Db.feed_eq_tail` | `Inv db → db.feed = (db.posts.reverse.take 20).filterMap db.view` |
| `Db.feedPlan_eq` | `Inv db → db.feedPlan = db.feed`: the generated feed plan computes the feed |
| `Db.postPlan_eq` | `db.postPlan id = db.getPost id`, with no invariant needed |
| `Db.checkInv_sound` | `db.checkInv = true → Inv db` (the startup scan) |
| `Db.inv_addPost` | `Inv db → db.insertOk p = true → Inv (db.addPost p)` (the per-insert check) |
| `Db.inv_like` | likes preserve `Inv` |
| `Transport.one_receive`, `issue_one` | completions never add a receive (one stays armed only on `more`); re-arming adds one only to a connection with none |
| `Transport.live`, `enqueue_iff`, `eof_closes` | no transition but end-of-input leaves an open connection without a receive; a connection is queued exactly on entering `queued`, never from it; end of input closes |
| `Transport.issueRound_bound`, `_fifo`, `_count` | the re-arm policy stays within its per-pass budget, re-arms a FIFO prefix of the queue, as many as the budget allows |
| `Transport.Sizing` fields, `connId_inj` | the packed identifiers fit their fields (ring 8 bits, slot 16 bits), buffer ring size a power of two ≤ 32,768 |

The transport theorems are about the generated table and the policy as functions; they are not a
proof of the C runtime, whose classification of kernel completions, generation checks and
re-arm budget are tested (`tests/metal/transport_cases.c`, `tests/metal/check153.sh`).

At startup the server runs `checkInv` once, a single pass over posts. After every insert it
runs `insertOk` on the new row. These checks cover every write as long as this process is the
only writer. That is verified at the start of every batch: SQLite's data version changes only
when another connection commits.

The server switches to the specification's SQL for good when:
* the stored layout differs from `Schema.lean`;
* the invariant check ever fails (say the clock steps back): the feed switches;
* another connection writes: every read switches. The plans are proved against the
model; the C emitted for them is tested, not proved (see Tests).

### Native read plans on SQLite's B-tree layer

The read plans run on SQLite's own B-tree API (`sqlite3BtreeTableMoveto`,
`sqlite3BtreeIndexMoveto`, `Next`/`Previous`, payload fetch). They are compiled in the same
translation unit as the amalgamation. Every request reads its rows from SQLite's pages through
SQLite's pager, WAL and locking, inside a SQLite transaction. What is skipped is the bytecode
interpreter, since each query is known ahead of time. Writes are ordinary SQL (prepared
statements). Measured on the seed with the same JSON output (byte-identical to the SQL
results): feed 16.7 µs with the reference SQL → 5.4 µs.

## Changes from #153

Everything above the transport — the specification, the invariant and its proofs, the read plans,
the generated `app_gen.c`, SQLite usage, JWT and HTTP handling — is byte-for-byte #153's
(`gen/app_gen.c` is identical). The changes are in `runtime/uring.c`, the new `Feed/Transport.lean`
and its generated `gen/transport_gen.h`:

| Change | Why (measured on one 3.7 GHz core, loopback, `bench/load.js` replay; details below) |
|---|---|
| Ended receives queue and are re-armed at most `NBUF` per ring per pass, oldest first; transitions from a Lean-generated table | #153 re-armed every receive ended with `ENOBUFS` on every pass. After a 2 s stall at 300,000 users that was 248,102 endings and re-arms in 10 s, quadratic in the backlog; now each connection ends once and is re-armed once (8,441 for 8,441) |
| SQ entries beyond the ring go to an ordered backlog; `io_uring_enter` only between passes; the CQ is reaped even when an enter fails; `EAGAIN`/`ENOMEM`/`EBUSY` retried | #153 exited on any enter error and on a full SQ after a flush: one transient kernel shortage dropped every connection and refused new ones until a restart |
| Shutdown, then close from the shutdown's completion (no linked pair) | a partial submission can split a linked pair, and the asynchronous shutdown could then be overtaken by the close |
| Held connections: generation-tagged, growable list | #153's fixed array had no bound across passes, and a reused slot kept a stale flag |
| A refused provided-buffer registration is retried once with `resv[0]=1` | Ubuntu's 6.8.0-136+ kernels invert the check on the reserved words ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)); #153 would exit at startup there. Same workaround as #264 and #434 |
| 1 ms pacing after a light pass that left nothing behind (#153: 250 µs, and it also waited with completions still queued) | every pass pays an `io_uring_enter` per active ring, a SQLite transaction with ~4.6 WAL-index `fcntl` locks, and the wait: at 300,000 users #153 ran ~1,600 passes/s of ~20 requests. 1 ms cut `io_uring_enter` from 0.43 to 0.18 and `fcntl` from 0.23 to 0.11 per request, and cycles per request by ~8 % |

Credits: the receive-recovery idea (queue ended receives instead of re-arming them all), the
ring-0-style pacing and the buffer-ring workaround come from studying #264 (rust-iouring-kensac);
the workaround is also in #434. They are re-implemented here.

Nothing tunes to the load test: no request mix, header layout, or pass/fail threshold is known to
the server, and a pass of 64 or more requests never waits.

## The rest of the server

* **Transport.** Several io_uring instances, each with a `SO_REUSEPORT` listener and a table
  of direct descriptors:
  * multishot accept/receive, provided receive buffers, `DEFER_TASKRUN`;
  * direct descriptors do not use the process file table, so under `LimitNOFILE=65535` the
    server holds up to 10 × 65,471 connections;
  * idle keep-alive connections are never closed.
  * **Receive recovery** (`Feed/Transport.lean`). When a burst or a stall empties a ring's
    provided buffers, multishot receives end with `ENOBUFS`. Re-arming them all at once only
    repeats that, pass after pass (measured after a 2 s freeze at 300,000 users: 248,102 ended
    receives and as many re-arms in 10 s). Instead an ended receive queues its connection on its
    ring's FIFO, and each pass re-arms at most as many queued connections per ring as the ring has
    buffers, oldest first: a recovery costs work linear in the backlog, and a re-armed receive
    reads the waiting request and stays armed. (A first version read queued connections with
    separate bounded reads before re-arming; under a CPU quota it cost an extra submission per
    request while behind and recovered worse than the original, so it was replaced.)
  * **Submission.** An entry goes to the kernel's submission queue while it has room, otherwise to
    a per-ring backlog in order; entries are published and `io_uring_enter` is called only at the
    loop's safe points, and the completion queue is reaped even when an enter fails. Momentary
    kernel shortages (`EAGAIN`, `ENOMEM`, `EBUSY`) are retried instead of exiting, which would drop
    every connection.
  * **Close.** A shutdown, then, from its completion, the close of the direct descriptor. They are
    not linked: the kernel may consume only part of what is submitted, and the shutdown runs
    asynchronously, so a linked close could otherwise overtake it.
* **Batches.** All requests from one round of completions form one batch, which is one SQLite
  transaction:
  * reads run first, from the snapshot at batch start; then writes; then `COMMIT`;
  * no response leaves before its batch commits, so a 201 is always committed, and reads never
    see uncommitted rows;
  * after a light round (fewer than 64 requests) the loop waits 1 ms so that the next round is a
    batch. Each round has fixed costs: an `io_uring_enter` per active ring, the SQLite transaction
    (BEGIN, cursors, COMMIT, and the WAL-index locks, each an `fcntl`) and the wait itself. With the
    earlier 250 µs, a 300,000-user load ran ~1,600 rounds a second of ~20 requests each; a round
    of 64 or more requests never waits, so the window does not apply when the CPU is the limit.
* **SQLite.** WAL, `synchronous=NORMAL`, 1 GiB mmap, small page cache. Built multi-threaded
  (`SQLITE_THREADSAFE=2`); each connection is used by one thread.
  * Requests run on the main connection, whose automatic checkpoints are off. When the log
    passes 1,000 frames, a WAL hook wakes a second thread, which runs `PASSIVE` checkpoints on
    its own connection.
  * Checkpoints copy the log into the database and fsync it, so a slow disk now stalls only
    that thread, never responses. A commit's durability does not depend on checkpoints (WAL,
    `synchronous=NORMAL`). Every response still waits for its commit.
  * The log starts over only when a write begins after every frame has been checkpointed. If
    it still grows past 64 MB, the request thread checkpoints the remainder between batches.
    That is one fsync stall, so it is a last resort; in the measured run it never ran.
* **JWT.** HS256 is verified on every request (signature, `alg`, `exp` required and in the
  future). The HMAC key pads are computed once from `JWT_SECRET`; that is the key schedule,
  not a verification result.
* **Requests.** HTTP/1.1 keep-alive, pipelining in order, `Connection: close`, partial
  requests. Input JSON is fully validated.

## Rules

1–3. SQLite file at `$SQLITE_PATH`, in-process. The schema is untouched; no tables, columns,
indexes or triggers are added.

4. `test/test.sh`: 42/42 (CI flow, run in a clean Ubuntu 24.04 container).

5. No caching across requests. Every response is built from SQLite pages read while the request is
being served. The only state kept between batches is two facts about the data, not data: whether the
invariant still holds, and whether another connection has written (SQLite's data version
number).

6. WAL + `synchronous=NORMAL`; group commit; each response waits for its commit.

7. Nothing depends on the load test. The invariant is checked, not assumed.

8–9. Direct on `0.0.0.0:80`. It needs no file descriptors beyond the listeners (see Transport).

10. Built from source on the box. Lean 4.35.0 and SQLite 3.54.1 are pinned by version and SHA-256.

11. Nothing on the box is tuned. The process raises its own soft `RLIMIT_NOFILE` to the hard
limit.

## Tests

* `test/test.sh` (42 checks) through `test/run.sh`, exactly as CI runs it.
* Native plans vs SQL: the feed and 20,000 random posts are byte-identical on the seed.
* Load: a native load generator reproducing `bench/load.js` (user model, think times, mix,
  `ramping-vus`, one keep-alive connection per user). It is not part of this submission.
* `tests/metal/` (development only, not used by the build; host paths are those of the
  development machine): `validate.sh` builds the test variants and runs everything below;
  `transport_cases.c` runs the real
  `runtime/uring.c` against a fake ring under ASan/UBSan; `check153.sh` runs the 42 checks, the SQL
  oracle, receive stress, a freeze/recover test with 16,000 connections, held pipelined writes,
  send backpressure, a foreign writer, and asserts STATS/fault counters for test builds (4 receive
  buffers, a 4-entry SQ, an `io_uring_enter` fault injector); `crash153.sh` kills the server under
  writes and checks every acknowledged write survived.

## Local measurements (not the official score)

Same host for every server: Xeon E-2176G (3.7 GHz), Ubuntu 24.04, kernel 6.8. The server runs in
a systemd scope on one core (`MemoryMax=2G`, no swap, `LimitNOFILE=65535`, UID 65534) on a fresh
seed copy; a native generator replays `bench/load.js` (user model, `ramping-vus`, one keep-alive
connection per user; 60 s ramp, 120 s hold, 30 s ramp-down) on other cores over loopback in a
private network namespace. CPU per request is the server cgroup's CPU over the hold divided by the
requests in it; cycles per request come from `perf stat` hardware counters on the server's threads
over 60 s of the hold. Runs are interleaved.

300,000 users, 0 errors in every run:

| Server | µs / request | cycles / request (user + kernel) | hold p95 / p99 ms |
|---|---|---|---|
| #153 (`fd2c149`) | 16.01, 15.80 | 59.1 k, 59.4 k | 3.5 / 11.2, 2.7 / 9.4 |
| this submission¹ | 14.76, 14.84, 14.59 | 54.5 k, 54.7 k, 61.9 k | 4.5 / 11.5, 6.0 / 13.5, 3.8 / 8.5 |
| #264 (rust-iouring-kensac, `4298101`) | 14.92, 15.07 | 58.6 k, 54.4 k | 7.1 / 15.3, 8.1 / 16.4 |

The pacing costs latency at light load: at 20,000 users p95 / p99 were 2.7–3.0 / 3.5–3.7 ms,
against 1.1 / 1.4–1.5 ms for #153.

¹ Measured before the buffer-ring registration retry was added; that code runs only on a kernel
that refuses the first registration, and this host (6.8.0-100) accepts it.

Also on this host: #153, an earlier version of this transport (without the 1 ms pacing) and #264
each held 550,000 users for 5 minutes, and all three failed 600,000 on CPU (2-minute probes).
After a forced 2 s freeze of the server under a 55 % CPU quota at 260,000 users, the worst
one-second p99 was 2.7 and 3.2 s with the new receive recovery against 5.0 and 6.0 s for #153.
The differences above are a few percent from a handful of runs on a fast core over loopback:
they do not predict an official score or ranking. The scoring VPS is about
twice as slow per request, and over loopback the client does part of the server's receive work.

## Layout

```
spec/         Lean specification, proofs and generator (lake exe derive <dir>)
gen/          app_gen.c and transport_gen.h as generated from spec/ (build.sh regenerates and compares)
runtime/      application-independent C: io_uring transport, HTTP framing, JSON input,
              HS256, output buffers, SQLite B-tree cursor primitives; server.c includes it all
tests/blast.c closed-loop throughput probe used during development
tests/metal/  same-host test and measurement harness (see Tests)
```
