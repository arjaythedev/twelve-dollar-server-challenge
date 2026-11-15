# Lean → C: a derived feed server whose transport drains sockets first

A variant of [`lean-derive-hargup`](../lean-derive-hargup) (#153, scored 280,000 at `fd2c149`) and a
follow-up to [`lean-derive-batched-hargup`](../lean-derive-batched-hargup) (#451, scored 219,000):
the same Lean specification, proofs and generated application layer. The transport keeps the
request backlog in its own memory instead of in kernel socket buffers. Queued connections are read
first, and answers are bounded per batch: 2,048, except that a connection whose pipelined requests
fill 64 KB is answered past the budget. Ring 0 is entered with a timed wait.
Why, and the evidence, are under [Why this version](#why-this-version) and
[Changes](#changes-from-153-and-451).

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
| `Transport.lean` | the per-connection receive state machine (multishot receive, queued, single-shot read), the read policy for queued connections, transport sizing as a dependent record | `transport_gen.h`: the transition tables `RECV_NEXT`/`RECV_ACT` the runtime executes, and the sizing constants (`NBUF`, `READS_PER_PASS`, `JOB_BUDGET`, …) |
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
| `Transport.one_receive`, `issue_one`, `arm_iff` | a connection never has two receives in the kernel: a completion keeps the multishot receive armed only on `more`, and arms it again only when a single-shot read (which has left the kernel) reports the socket empty; issuing a read adds one only to a connection with none |
| `Transport.events_belong` | a read's completion only drives a reading connection and a multishot receive's only an armed one (the events are distinct, so a stale completion of the other operation is dropped) |
| `Transport.live`, `enqueue_iff`, `eof_closes` | no transition but end-of-input leaves an open connection without a receive; a connection is queued exactly on entering `queued`, never from it; end of input closes, whichever operation saw it |
| `Transport.issueRound_bound`, `_fifo`, `_count` | a round of reads stays within its budget, reads a FIFO prefix of the queue, as many as the budget allows |
| `Transport.Sizing` fields, `connId_inj` | the packed identifiers fit their fields (ring 8 bits, slot 16 bits), buffer ring size a power of two ≤ 32,768; a pass's reads cover at least one full round (`reads_round`); the per-pass answer budget is positive and fits the job array (`jobs_pos`, `jobs_fit`) |

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

## Why this version

#451 scored 219,000 against #153's 280,000 on the scoring box. It failed on latency at
~80 % CPU (p99 4.2 s at 240,000, 0 errors), and at 320,000 most connections were refused. #153 and
the low-memory variant #434 show the same pattern at their own limits. A same-host loopback
benchmark could not see it, so a **1-vCPU / 2 GB KVM guest** was used to look for a mechanism:

- the server runs in its own cgroup, as on the box;
- traffic arrives over virtio-net, so the guest's single vCPU also does the receive work, and
  the guest kernel sizes TCP memory for 2 GB;
- the load generator is the same `bench/load.js` replay (harness in `tests/metal/`).

In the guest, at the end of a 60 s ramp to 350,000 users, new connections briefly arrived faster
than the server could answer. #153 and #451 then left each user's next request waiting in its
socket, at ~1.8 KB of kernel memory each. Within about 5 s TCP memory went from ~250 pages to
35–41k (the guest's limit is 45k), and the kernel's OOM killer stopped the server, after which
connections are refused. This is a demonstrated local failure mechanism consistent with the
official symptoms (latency collapse below full CPU, then refused connections). The precise cause
of each official failure is not established.

#264 survived the same ramp with ≤ 14 MB of socket buffers: it reads sockets first and answers a
bounded number of requests per pass, so its backlog sits in its own memory at about the size of
the requests.

This version does the same:

- **Queued connections are drained** with single-shot reads.
- **Answers are bounded per pass.** Requests beyond the budget wait on their connection in user
  memory and are answered first in the next pass, oldest first.

## Changes from #153 and #451

Everything above the transport is byte-for-byte #153's: the specification, the invariant and its
proofs, the read plans, the generated `app_gen.c`, SQLite usage, JWT and HTTP handling. The
changes are in `runtime/uring.c`, `Feed/Transport.lean` and the generated `gen/transport_gen.h`.

| Change | #153 | #451 | here |
|---|---|---|---|
| Receives that end (`ENOBUFS`: the ring's buffers ran out) | all re-armed every pass (an `ENOBUFS` storm) | queued; at most `NBUF` re-armed per ring per pass as multishot receives, which compete with every armed receive for buffers | queued and **drained**: single-shot `MSG_DONTWAIT` reads, at most `NBUF` per ring per round, in rounds within the pass (they complete inside the non-blocking enter; their buffers return before the next round), up to 16,384 per pass; a read that fills its buffer queues the connection again, a shorter read or `EAGAIN` arms the multishot receive |
| Requests answered per pass | all | all (a pass could answer ~20k and read nothing for ~1 s) | at most **2,048** per batch; the rest stay on their connection (about their own size) and are answered first next pass. A connection held only for the budget whose waiting requests reach 64 KB (a client that pipelines) is answered past the budget instead of dropped |
| Pacing after a light pass | `nanosleep` 250 µs | `nanosleep` 1 ms | ring 0 is entered last with `io_uring_enter(min_complete 256, 1 ms timeout)`: the other rings' responses are already submitted and one system call both sends ring 0's and paces the loop (as #264) |
| Idle | eventfd per ring + `epoll_wait` | same | wait on ring 0 for a completion or 1 ms; other rings' work is found through `IORING_SQ_TASKRUN`. No eventfd: the kernel signalled it, and ran the epoll callback, on every completion flush of every ring, also from the receive softirq |
| Held input | 64 KB | 64 KB | 64 KB without a complete request still closes; a held connection (requests waiting for their turn or for a write to commit) may buffer up to 1 MB |
| Submission backlog, ordered close, enter-error retry, buffer-ring `resv[0]` retry | — | yes | yes (from #451) |

Credits: draining queued connections with bounded single-shot reads, bounding answers per pass and
the ring-0 timed wait are #264's (rust-iouring-kensac) design, described in its README. They are
re-implemented here and modelled in `Feed/Transport.lean`.

Nothing tunes to the load test: no request mix, header layout, user count or pass/fail threshold
is known to the server. The budgets are per-pass work bounds and apply to any traffic.

## The rest of the server

* **Transport.** Several io_uring instances, each with a `SO_REUSEPORT` listener and a table
  of direct descriptors:
  * multishot accept/receive, provided receive buffers, `DEFER_TASKRUN`;
  * direct descriptors do not use the process file table, so under `LimitNOFILE=65535` the
    server holds up to 10 × 65,471 connections;
  * idle keep-alive connections are never closed.
  * **Receive recovery** (`Feed/Transport.lean`). When a burst or a stall empties a ring's
    provided buffers, multishot receives end with `ENOBUFS`. An ended receive queues its connection
    on its ring's FIFO, and the queue is drained in the same pass:
    * single-shot `MSG_DONTWAIT` reads, at most as many per ring and round as the ring has buffers,
      in rounds up to 16,384 reads per pass;
    * a read that fills its buffer queues the connection again;
    * a shorter read or `EAGAIN` means the socket is empty, and the multishot receive is armed
      again.

    A request then waits in this process's memory at its own size, not in its socket at ~1.8 KB
    of kernel memory. (#451 re-armed queued connections as multishot receives, which compete for
    buffers with every armed receive. A first read-based version, without the answer budget below,
    recovered worse under a CPU quota on loopback; with the budget a pass stays short and the reads
    keep up.)
  * **Submission.** An entry goes to the kernel's submission queue while it has room, otherwise to
    a per-ring backlog in order; entries are published and `io_uring_enter` is called only at the
    loop's safe points, and the completion queue is reaped even when an enter fails. Momentary
    kernel shortages (`EAGAIN`, `ENOMEM`, `EBUSY`) are retried instead of exiting, which would drop
    every connection.
  * **Close.** A shutdown, then, from its completion, the close of the direct descriptor. They are
    not linked: the kernel may consume only part of what is submitted, and the shutdown runs
    asynchronously, so a linked close could otherwise overtake it.
* **Batches.** The requests of one pass form batches of at most 2,048 (`JOB_BUDGET`; a connection
  whose pipelined requests fill 64 KB is the exception and is answered past it). Each batch is one
  SQLite transaction:
  * reads run first, from the snapshot at batch start; then writes; then `COMMIT`;
  * no response leaves before its batch commits, so a 201 is always committed, and reads never
    see uncommitted rows;
  * requests beyond the budget wait on their connection and go first in the next pass, oldest
    first;
  * after a light pass (fewer than 64 requests) that left nothing behind, the next pass enters
    every other ring and then ring 0 with a timed wait (256 completions or 1 ms). Each pass has
    fixed costs (an `io_uring_enter` per active ring, the SQLite transaction and its WAL-index
    locks), so waiting a little turns the next pass into a batch. A heavy pass never waits.
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
  `runtime/uring.c` against a fake ring under ASan/UBSan (backlog, ordered close, every receive and
  read completion and wrong-operation drops, the queue and its rounds, the answer budget and its
  64 KB exemption); `check153.sh` runs the 42 checks, the SQL
  oracle, receive stress, a freeze/recover test with 16,000 connections, held pipelined writes,
  send backpressure, a foreign writer, and asserts STATS/fault counters for test builds (4 receive
  buffers, a 4-entry SQ, an `io_uring_enter` fault injector); `crash153.sh` kills the server under
  writes and checks every acknowledged write survived.
* `tests/metal/vm153.py` (with `vm_init.sh`, `vm_build.sh`, `runvm153.sh`, `campaign_hold.sh`,
  `vmsum.py`, `vmseries.py`): the 1-vCPU / 2 GB KVM guest used for the evidence below.
  * Guest: Ubuntu 24.04 userland, kernel 6.8, virtio-net with vhost-net on a tap.
  * The server runs as UID 65534 under `LimitNOFILE=65535` in its own cgroup.
  * Per run it reports the app cgroup's CPU per request and memory peak, the guest's TCP memory,
    retransmissions and drops.
  * qemu runs in a container; nothing on the host is changed.

### Checks completed for this version (at submission)

- **Clean `ubuntu:24.04` container, the upstream CI flow** (`install.sh`, then `test/run.sh`, one CPU,
  2 GiB, `LimitNOFILE` 65535) on this folder: 42/42.
- **Lean:** `lake build` (all theorems) passes; `lake exe derive` regenerates `gen/app_gen.c` and
  `gen/transport_gen.h` identically.
- **`transport_cases.c` under ASan/UBSan:** pass.
- **`check153.sh` suites** (SQL oracle, receive stress, 16,000-connection freeze/recover, the 42
  checks, edge cases, held pipelined writes, backpressure, foreign writer), all passing, on four
  builds:
  - the production build;
  - a STATS build;
  - STATS + ASan/UBSan;
  - 4 receive buffers + ASan.
- The remaining test builds (4-entry SQ, `io_uring_enter` fault injector, tight budgets) and the
  crash-durability test were still running when this was submitted.

## Local measurements (not the official score)

**One local VM run, not an official score.** It is not a demonstrated capacity win over #153 or
#264: comparisons on a calibrated (slowed) guest are still running.

Setup:

- **Source:** this folder, as built by `build.sh` (code identical to commit `9a1b217` on the
  development branch).
- **Host:** Xeon E-2176G, Ubuntu 24.04.
- **Guest:** 1 vCPU (the host thread pinned to one core, its SMT sibling idle, so the guest runs
  faster per request than the scoring VPS); 2,048 MB; kernel 6.8.0-100-generic; virtio-net with
  vhost-net on a tap.
- **Server:** UID 65534, `LimitNOFILE=65535`, its own cgroup, fresh seed copy.
- **Load:** a native generator replaying `bench/load.js` (user model, `ramping-vus`, one keep-alive
  connection per user) from the host: 60 s ramp, 300 s hold, 30 s ramp-down.
- **Latency:** request duration excluding connect (k6's `http_req_duration`).

| Users | Errors | Requests | Hold p95 / p99 | Whole run p95 / p99 | req/s (hold) | App cgroup CPU (µs/req) | App cgroup peak |
|---|---|---|---|---|---|---|---|
| 325,000 | 0 | 12.12 M | 12.1 / 143.1 ms | 15.5 / 543.7 ms | 34,379 | 66 % (19.2) | 788 MB |

In the same guest (2-minute runs at 350,000 users), this transport kept every connection without an
OOM kill (≤ 27 MB of socket buffers) while #153 and #451 were OOM-killed at the end of the ramp.
At 325,000 users in 2-minute runs, #153 passed and this version did not in its first run. So no
capacity ranking between them is claimed here.

## Earlier same-host loopback measurements (#451's transport, not this version)

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
| #451's transport (C5)¹ | 14.76, 14.84, 14.59 | 54.5 k, 54.7 k, 61.9 k | 4.5 / 11.5, 6.0 / 13.5, 3.8 / 8.5 |
| #264 (rust-iouring-kensac, `4298101`) | 14.92, 15.07 | 58.6 k, 54.4 k | 7.1 / 15.3, 8.1 / 16.4 |

#451's 1 ms pacing cost latency at light load: at 20,000 users p95 / p99 were 2.7–3.0 / 3.5–3.7 ms,
against 1.1 / 1.4–1.5 ms for #153.

¹ Measured before the buffer-ring registration retry was added; that code runs only on a kernel
that refuses the first registration, and this host (6.8.0-100) accepts it.

Also on this host: #153, an earlier version of #451's transport (without the 1 ms pacing) and #264
each held 550,000 users for 5 minutes, and all three failed 600,000 on CPU (2-minute probes).
After a forced 2 s freeze of the server under a 55 % CPU quota at 260,000 users, the worst
one-second p99 was 2.7 and 3.2 s with #451's receive recovery against 5.0 and 6.0 s for #153.
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
