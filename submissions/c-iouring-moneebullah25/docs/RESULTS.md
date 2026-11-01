# Results

Every number below is **measured** unless marked **[E] estimated**. Machine for
all runs: AMD Athlon Silver 3050U (Zen1, 2 cores, 2.3 GHz, boost off), 14 GB
RAM, Ubuntu kernel 6.8.0-147-generic, gcc 13.3, SQLite 3.54.1, liburing 2.15.
The load generator (`tools/mload.c`) and the server share the two cores and
the loopback interface; the user's desktop was running at the same time. This
is **not** a 1 vCPU / 2 GB box and the client is in the measurement, so:

- "CPU us/request" is the server process's own user+sys time divided by its
  request count (`getrusage`, reported in its stats line). On loopback it also
  includes the kernel's work of delivering the packet into the client's socket,
  which the exploration estimated at a 30-40% overstatement; on a virtio
  droplet the kernel path costs more. Treat it as an upper bound of the
  server's own cost and a lower bound of the droplet's.
- Latency percentiles from `mload` include the generator's own queueing (a
  single thread serving 100k VUs on a shared core). The independent probe
  (`tools/probe.py`, one request every 50 ms on its own connection) is the
  better view of server stalls.
- Memory per connection is the change in `MemAvailable`, which includes both
  ends of every loopback connection and the generator's own per-VU state.

Commands are relative to `submissions/c-iouring-moneebullah25/`. The seed is a
fresh copy of `seed/feed.db` for every run (`tools/bench_local.sh` does that).

## Summary

| What | Measured | Target in the exploration report |
|---|---|---|
| Correctness | 42/42 official, 166/166 own (148 + 18 review regressions), unit + fuzz clean, ASAN/UBSAN clean | 42/42 |
| CPU per request, batched (64 VUs, no think time, ~23-29k req/s) | **29.8-31.1 us** before, **26.9 us** after the review fixes (user+sys, loopback) | <= 20 us on a Xeon 8260 for 350k; best entry 30 us |
| CPU per request, 100k users at ~10k req/s (batches of 2-4) | **36-39 us** before, **33.8 us** after | |
| CPU per request, 10k users at 1k req/s (one request per wake-up) | **60 us** before, **59 us** after | |
| Server RSS (after the review fixes, 72 MiB send pool) | 137 MB idle (prefaulted pools), 141 MB at 10k, 157 MB at 100k connections | ~40 MB app |
| User-space state per connection | 48 B record; 22 B/conn amortized RSS growth 10k -> 100k | 64 B |
| MemAvailable per loopback connection pair incl. generator | ~12-17 KB | 8.3 KB kernel per pair (exploration) |
| Connections held locally | 100k (2 rings) and 200k (4 rings), see below | 300-400k on the box |
| Feed strategy | range scans save 2.7 us per average request vs the reference SQL | -25% on feed |
| PGO | no CPU gain (30.1-31.8 vs 29.8 us), lower throughput; not shipped | +10-15% [E] |
| Checkpoints (4,000-frame trigger) | passive, off-loop; 44 per 90 s at ~600 writes/s, avg 106 ms, max 451 ms; longest COMMIT on the loop 48 ms | 100-770 ms on-loop avoided |
| Largest local hold | 182,765 connections across 4 rings (generator-limited) | 300-400k on the box |

Sections 1 to 7 are historical; section 8 is the state at #228, and section 9
holds the local cgroup test behind the guard-fix follow-up (shedding off by
default, cache-free pressure input).

What this does **not** prove: any user count on the real box, p95/p99 at
300k users over a real network, or behaviour at the 2 GB memory wall. See
`STATUS.md`.

## 1. Correctness (2026-10-24)

```
bash ../../test/test.sh http://127.0.0.1:PORT      # ASAN build: passed=42 failed=0; release build: passed=42 failed=0
python3 test/http_tests.py --server bin/server-asan --seed ../../seed/feed.db   # passed=148 failed=0
make check                                         # unit tests: all passed
make fuzz FUZZ_ITERS=300000                        # no findings (ASAN/UBSAN)
```

The black-box run also found and fixed two real bugs before any tuning:
secondary rings must not use `DEFER_TASKRUN` (their completions were never
posted; the ring-fd poll only fires for posted CQEs), and a multishot accept
that hits `-ENFILE` loses the socket it just accepted (fixed by handing over
to bounded one-shot accepts near a full table).

## 2. CPU per request: tight loop (2026-10-24)

`tools/bench_local.sh --server build/server-plain --vus 64 --duration 20 --ramp 2 --think-scale 0 --src-ips 1`

64 keep-alive connections, no think time (the k6 mix: 46% feed, 46% post,
6.9% like, 0.9% create). Batches of ~30 requests per wake-up.

| Build / flag | CPU us/req (server, user+sys) | req/s (both processes on 2 cores) | mload p99 |
|---|---|---|---|
| plain LTO, run 1 | 29.8 | 23.4k | 8.9 ms |
| plain LTO, run 2 | 29.8 | 24.3k | 7.6 ms |
| PGO, run 1 | 31.8 | 15.9k | 20.6 ms |
| PGO, run 2 | 30.1 | 19.3k | 9.2 ms |
| plain, `--feed-reference` | 33.2 | 20.2k | 9.3 ms |
| plain, `--no-batch-read-txn` | 29.8-30.0 | 24.7k | 7.0 ms |
| plain, default (repeat) | 30.5-31.1 | 16.5-19.6k | 11-16 ms |

Throughput varies +-25% between identical runs (desktop load); CPU/request is
stable to about +-1 us and is the number used for decisions. Decisions: keep
the range-scan feed (-2.7 us), keep the batch read transaction (within noise,
better snapshot semantics), ship plain LTO instead of PGO.

## 3. Users with the real think times (2026-10-24)

`tools/bench_local.sh --server build/server-plain --vus N --duration 60..120 --ramp 15..60 --think-scale S --src-ips 2..12`

| Users | think scale | req/s | rings used | server CPU us/req | server RSS | MemAvailable drop | probe p50 / p99 / max | mload p99 |
|---|---|---|---|---|---|---|---|---|
| 10,000 | 1 | 1.0k | 1 | 59.6 | 101 MB | n/a | | 3.9-18 ms |
| 50,000 | 2 | 2.3k | 1 | 54.4 | 111 MB | 640 MB (13 KB/pair) | | 144 ms |
| 100,000 | 1 | 10.3k | 2 | 35.6-38.6 | 117 MB | 1.2-1.75 GB (12-17 KB/pair) | 0.6-0.9 ms / 95-495 ms / 1-7 s | 1.6-7 s (generator-bound) |
| 100,000, `--ckpt-frames 4000` | 1 | 10.5k | 2 | 36.8 | 117 MB | | 0.6 ms / 105 ms / 899 ms | 521 ms |
| 200,000 attempted | 2 | 1.6k | 4 | 56 (mostly idle wake-ups) | 114 MB | | 0.3 ms / 78 ms / 1.1 s | generator stalled, see below |

At 1-2k req/s nearly every request gets its own `io_uring_enter` (iterations ~=
requests), which is where the 55-60 us comes from; at 10k req/s batches of
2-4 form and the cost drops to 36-39 us; at 23k req/s with batches of ~30 it
is 30 us. The real box at 300k users would run ~30k req/s. [E] The per-request
cost there is therefore expected to be in the 30 us region before the virtio
and faster-CPU corrections, which pull in opposite directions.

The multi-second probe maxima at 100k coincide with the generator falling
behind (mload "active" counts of 50-77k requests in flight) and with slow
checkpoints on this laptop disk (2-7 s per passive checkpoint while mload and
the desktop compete for the disk); the server's own longest loop iteration was
115-377 ms (256 requests that took 83-115 ms, i.e. the thread was descheduled)
and its longest COMMIT 32-40 ms.

**200k attempt** (`--vus 200000 --duration 120 --ramp 60 --think-scale 2 --src-ips 12`):
the server held **182,765 connections at once across 4 rings** (65,535 +
65,535 + 51,695) with 114 MB RSS, but the single-threaded generator fell
behind during the ramp (81k requests "in flight" from its point of view, one
request measured at 129 s, the run overran to 178 s) and never reached 200k.
The server's own longest iteration was 808 ms during that ramp and its probe
p99 78 ms. This run shows the multi-ring tables working at that size; it is
**not** a latency or capacity result, and the cause of the generator stall
(client CPU, SYN backlog retries, or something in the server) was not
isolated. Next step on a real setup: several generator processes.

## 4. WAL, checkpoints and holds

Before the fix in commit "fix checkpoint trigger" the checkpointer compared the
WAL's total frame count with the threshold; the total stays above it until
the WAL restarts, so under load it ran a checkpoint (two fsyncs) on every
100 ms wake-up: 2,242 checkpoints in 90 s, one taking 7.2 s, and loop
iterations of 377 ms as the ext4 journal stalled the WAL append. With the fix
(count only frames not yet backfilled) the same run does 137 checkpoints,
average 108 ms, and the loop's longest iteration drops to 148 ms. With
`--ckpt-frames 4000` (now the default): 44 checkpoints in 90 s, average 106 ms,
max 451 ms, longest iteration 106 ms, longest COMMIT 48 ms, mload p99 521 ms
instead of 2.8 s, probe p99 105 ms.

Write holds (WAL >= 8,192 frames): 2-5 per 90 s run at ~600 writes/s, longest
530 ms (one hit the 500 ms timeout: `hold_to`), reads unaffected. The black-box
suite's burst test (16 writers, 2,400 creates, `--hold-frames 300
--ckpt-frames 100`) completes with p99 < 1 s and no errors.

## 5. Re-measurement after the review fixes (2026-10-24, same commands)

The independent review (3 major, 9 minor findings) led to: a 72 KiB send
buffer sized for the largest legal feed (pool 1,024 x 72 KiB instead of 2,048
x 16 KiB), half-close handling, a zeroed `http_req` on every parse path,
strict UTF-8/NUL body validation, `Date` and HTTP/1.0 keep-alive headers,
stalled sends swept, partial buffers sized on demand, TCP keep-alive on the
listener, a nice-10 checkpointer instead of `SCHED_IDLE`, and a 50 ms busy
timeout. Re-measured with the release build of that tree:

| Run | CPU us/req | RSS | notes |
|---|---|---|---|
| 64 VUs tight loop, 20 s | 26.9 | 137 MB | 29.3k req/s, mload p99 5.1 ms, probe p99 10.5 ms |
| 10k users, 60 s | 59.4 (50.9 in the steady window) | 141 MB | mload p99 0.6 ms, probe p99 0.5 ms, longest iteration 24 ms |
| 100k users, 90 s | 38.6 (33.8 in the steady window) | 157 MB | 39 checkpoints (avg 155 ms, max 1.9 s), 5 holds (max 509 ms), longest iteration 215 ms; probe p99 58 ms but max 8.6 s during a generator stall at 65-80 s, mload p99 10 s (generator-bound as before) |

The 2-4 us drop in CPU per request between the two measurement sets is
within the run-to-run spread seen earlier and is not attributed to any one
change. Official suite 42/42 through `test/run.sh`, own suite 166 checks
(148 + 18 review regressions) under ASAN and UBSAN, unit tests, 500k fuzz
iterations with the new PARTIAL contract: all clean.

## 6. Build reproducibility

`fetch-deps.sh` verifies SQLite's zip against the SHA3-256 published on
sqlite.org (`92281c86...8e30`) and liburing's tarball against its SHA-256
(`8d052f26...120a`, the same value another entry pinned independently).
`make release` on this machine takes about 2 minutes (SQLite at -O3 with LTO
dominates); `make pgo` about 6 minutes including the training run.

## 7. After the scored run: pacing, like tally, SIMD escaping (2026-10-29)

Official result at `065c235`: 209,000 users, 20,460 req/s, 38 us/request at
83% CPU, p95 51 ms / p99 309 ms, 1,132 MB (organizer's comment on PR #156).
The box ran four rings at that count; the loop woke on every secondary ring's
completion and skipped the batch wait whenever any ring held one, so passes
were a few requests long, the regime that section 3 had measured at 34-39 us.

Local before/after on the same laptop (server on CPU 0, mload on CPU 1, 30-45 s
runs, desktop load 7-9 so latency is noisy; CPU per request is stable to about
2 us). "Before" is the scored code; "after" is this follow-up (time-paced
passes with one-shot ring polls, 2 ms window, likes tallied in C, SSE2 JSON
escaping, 4,096 receive buffers, 512 send chunks).

| Regime | Before us/req (usr / sys) | After us/req (usr / sys) | Passes per request |
|---|---|---|---|
| 64 connections, no think time (batches of ~30) | 33.6 (21.8 / 11.7) | 28.2 (16.5 / 11.6) | 0.05 -> 0.035 |
| 10k users, think x0.1 (~10k req/s; batches of a few, like the box) | 33.0-34.5 (15-16 / 18) | 29.0-31.4 (12.8-13.7 / 16.4-17.7) | 0.065 -> 0.03 |
| 10k users, real think times (~1k req/s; one request per wake-up) | 67-77 (32-36 / 36-41) | 68-70 (31-32 / 37-38) | 1.3 -> 0.5 |

Details from the same runs:

- Batch window alone (flag, both binaries): 250 us -> 2,000 us saves about 2 us
  per request (half user, half system); 1,000 us gets most of it; 64 CQEs is no
  better than 32. The scored code at 1-2 ms windows ran into ENOBUFS storms
  (21k-60k per run, throughput down to 4-6k req/s); the follow-up shows zero
  ENOBUFS at every window and buffer count (the deferred re-arm now runs after
  a full pass instead of after every wake-up).
- Syscalls per request at ~10k req/s (`strace -c` as the server's parent):
  `io_uring_enter` 0.042, `pwrite64` 0.40 (WAL frames), `pread64` 0.071,
  `fdatasync` 0.00007, `futex` 0.07; the memory guard's `openat`/`read` 0.001;
  `clock_gettime` is vDSO. Completions per request 2.1-2.2 (receive + send).
- SQLite cache 256 MiB vs 32 MiB: no change in CPU (0.067 vs 0.071 pread per
  request); the hot window fits the 32 MiB cache. A separate SQLite-only
  microbenchmark (same statements, unix-excl) measured the three-range feed at
  16.5 us vs 28.6 us for the reference query with a 4,000-frame WAL, `mmap`
  making the feed 1.2-1.8x slower, and the likes scan tallied in C at 11.7 us
  vs 16.5 us for `GROUP BY count(*)`; `GET /posts/:id` on the newest 2,000 ids
  3.7 us with zero preads.
- 16 rings vs 1 ring at 10k users: within noise locally (29.6-31.3 vs
  29.2-31.7), so the multi-ring cost the box showed is not reproducible on
  loopback at this request rate; the pacing change removes the mechanism
  regardless.
- Where the ~30 us of the box-like regime go locally: user space ~13.5
  (SQLite and our code), kernel receive/send inside io_uring ~14-15, wake-ups
  and enters ~1.5-2, WAL writes ~0.5-1, preads ~0.1-0.2 (the kernel figures are
  estimates from syscall counts; `perf` is not permitted on this machine).

What this predicts for the box, honestly: 10-16% less CPU per request than the
scored 38 us, i.e. the low thirties, if the loopback ratios transfer; the
entries at 28-31 us are the ones to beat and this follow-up does not claim to
reach them. PGO is now part of `build.sh` (trained on a synthetic database on
the box itself, falling back to the plain build if training fails): the F#
entry measured 2-4.5% from it on the box; our own laptop measurement was
inconclusive (section 2).

## 8. After the second scored run (2026-10-30 and 31)

Everything in this section except the first subsection was measured on the
2-core laptop (Athlon Silver 3050U, kernel 6.8.0-147), with the server pinned to
core 0 and `tools/mload.c` on core 1. Nothing was measured at box scale and no
user-count score is claimed.

### 8.1 Organizer result for commit 3f0c2c1 (PR 203, on the box)

- 230,000 users held 5 min: 22,377 req/s, p95 9.3 ms, p99 37 ms, 0 errors, 66%
  CPU (27 us/request by the server's counter, was 38), 1,106 MB RAM.
- 240,000 users (2 min probe): failed on latency (p95 5.8 s) while the memory
  guard paused accepting twice (252 MB available).
- 320,000 users: the app logged nothing and the box rebooted (memory pressure
  outside the app).

The Summary at the top predates this section; its numbers are those of earlier
versions.

### 8.2 Method

Unless a row says otherwise: 10,000 users, `--think-scale 0.2`, 40 s, a fresh
copy of `seed/feed.db` per run, server on core 0 and `mload` on core 1. CPU per
request is the whole-run user+sys total from the server's `getrusage` divided by
its request count. `F` is the fraction of responses carrying `Connection:
close`, fixed with the server's `--shed-force F` flag. Runs during which other
work on the machine raised the load average above 6, where p95 went above 1 s,
are discarded and excluded from every number below.

### 8.3 Batch window

Us per request at F=0 and F=0.3, and p95 (CQEs / time before a pass):

| Window | F=0 | F=0.3 | p95 |
|---|---|---|---|
| 32 / 2 ms (previous default) | 22.7-23.5 | 32.4-34.2 | 3-4 ms |
| 128 / 4 ms | 21.0-21.4 | 28.3-30.2 | 5-9 ms |
| 256 / 8 ms | 21.1-21.2 | 27.9-29.0 | 10-18 ms |
| 512 / 16 ms (adopted) | 20.7 (5 runs) | 27.2-28.3 | 20-35 ms |

1,024 / 32 ms was not adopted: its shedding runs were disturbed by machine load
(p95 over 1 s) and are excluded. The adopted window trades up to about 35 ms of
p95 for 10 to 15 percent less CPU.

### 8.4 Reconnect cost

Earlier build, 2 clean runs per cell except F=0.5 (one clean run):

| F | us/request | p95 |
|---|---|---|
| 0 | 23.15 | 3.1-4.0 ms |
| 0.2 | 30.85 | 3.1-4.0 ms |
| 0.5 | 37.3 | 3.1-4.0 ms |
| 1.0 | 45.5 | 3.1-4.0 ms |

One reconnect (accept, handshake, shutdown, close; almost all kernel time) costs
about 22 us at F=1.0, 28 us at 0.5 and 38 us at 0.2: the marginal cost is highest
at low F.

### 8.5 Where the CPU goes at F=0

Sampled with rdtsc counters (since removed), cycles per request at about 2.1
GHz: HTTP parse 0.56k; `app_handle` 14.3k, of which the feed read 11.1k (row and
author load 7.2k, JSON 2.2k, ids 0.84k, likes scan 0.45k) and a single post
2.1k; commit in `end_batch` 4.5k (mostly kernel I/O); building and submitting
sends 0.09k; reaping 0.28k. About 8 us of the 20.7 us is user time; the rest is
the kernel's TCP work.

### 8.6 B-tree reads versus SQL

SQLite-only microbenchmark (seed database, pinned, warm, 32 reads per shared
read transaction, best of 3 x 20,000): feed 14.2 and 14.4 us versus 32.8 and
30.3 us; single post 3.6 and 3.4 us versus 4.2 and 4.4 us.

Differential test (`tools/dbdiff.c`): the feed and all 500,005 post ids were
byte for byte identical to the SQL path; after 2,008 random likes and posts in
130 batches, the feed and 5 posts were compared after every batch and were
identical; a final comparison over all 500,633 ids after the writes was
identical. 0 fallbacks in the whole run.

### 8.7 SQLite knobs

10,000 users, think-scale 0.25, 45 s, clean runs only (load average under 3):

| Setting | us/request | Memory |
|---|---|---|
| cache 32 MiB | 29.2, 30.4 | anonymous 60 MB |
| cache 8 MiB | 29.5 | 54 MB |
| cache 4 MiB (adopted) | 30.1 | 49 MB |
| `mmap_size` 256 MB or 1 GiB, added | +2 to +5 (user time) | no change in cgroup memory (same file-backed pages) |
| checkpoint at 1,000 frames, hold at 8,192 (adopted) | 28.8 (one clean run) | cgroup 145 MB against 174 MB |

### 8.8 Kernel memory per idle socket

Kernel 6.8.0-147, loopback, averaged over both endpoints: about 2.5 to 2.6 KB
charged to the cgroup (all slab: about 1.45 KB unreclaimable plus about 1.05 KB
reclaimable dentry and inode cache) and about 3.6 to 3.9 KB system-wide slab.
Socket buffer memory is 0 while a socket is idle and stays 0 after a 200-byte
request has been read. `SO_RCVBUF` and `SO_SNDBUF` of 4096, and io_uring
fixed-file sockets, change nothing measurable. Arming a multishot receive adds
about 0.26 KB in the cgroup and about 0.5 KB system-wide. A multishot or
one-shot poll costs the same in the cgroup and saves about 0.2 KB system-wide,
so poll arming was not adopted; a multishot poll also pins a socket after
`close_direct` unless it is cancelled explicitly. A mass close of 20,000 sockets
raised slab by at most about 16 to 25 MB for 0.1 to 0.3 s. The organizer's own
estimate for the box is about 4.3 KB per socket.

### 8.9 Things tried and not kept

Linked shutdown plus `close_direct` submission saved under 1 us per request.
Cancelling the armed receive and then closing is invalid: a fixed-file close
does not release the socket while a receive is armed. `TCP_DEFER_ACCEPT` gave no
gain.

### 8.10 Close-path race

The original server lost the first request of 25 of 300 reconnecting clients;
the fixed server lost 0 of 3,000 (DESIGN.md, "Close-path race").

### 8.11 Tests on the final tree

Unit tests, fuzz (300,000 iterations), the official suite (42 of 42) and the
black-box suite (177 of 177), all under ASAN and UBSAN. The official suite also
passed 42 of 42 through `build.sh` with the PGO build (3 m 43 s, 10 profile
files).

## 9. Guard fix: local cgroup test (2026-10-31 and 2026-11-01)

**Local evidence only: one 2-core laptop, loopback, a systemd user unit with a
300 MB cgroup. This is not box evidence and no user count on the box follows
from it.**

*Why.* #228 scored 228,000 held and its 240,000 probe failed with connection
refused; the process was OOM-killed in its cgroup (reproduced locally). The
forecast for #228 (about 31% of responses closing, about 94% CPU at 350,000)
was contradicted by that result.

*Setup.* `MemoryMax=300M`, `MemorySwapMax=0`, server pinned to CPU 0, `mload` on
CPU 1 with 32 source IPs, think-scale 1, ramp 60 s, hold 240 s, fresh seed copy
per run, started only when the 1-minute load was under 2.5. Scripts were
`run1.sh` style (systemd-run, sampler of memory.stat and sockstat).

*Finding 1: the guard counted page cache as pressure.* Under a cgroup limit the
cgroup's `memory.current` includes page cache, which fills to the limit by
itself and is dropped on demand. The guard used `memory.max - memory.current` as
headroom, so it reported little headroom when memory was not short and shed or
paused accepting early. In a 120,000-user run of the new build the cgroup held
about 35 MB anon, 195 MB kernel (slab) and 70 MB file cache; the cache-free
figure (anon + kernel + sock, about 230 MB) is what the guard now uses.

*Finding 2: closing and reconnecting did not help.* With the cache-free input,
the plain server (no shedding) survived 60,000 to 120,000 users in the 300 MB
cgroup with 0 errors; the same server with shedding thresholds scaled to the
cgroup (84 / 37 MB, conn cap 74,400) added connect errors from 80,000 up.

Results, one run per cell unless marked (x2 = two runs with the same result):

| users | old guard, shedding on | old guard, no shedding | cache-free input, shedding on | cache-free input, no shedding |
|---|---|---|---|---|
| 60,000 | | | 0 errors (x2) | 0 errors |
| 70,000 | | | 0 errors (x2) | 0 errors (x2) |
| 80,000 | OOM-killed at 82 s | OOM-killed at 121 s | 0.50% errors (x2) | 0 errors (x2) |
| 100,000 | OOM-killed at 61 s | OOM-killed at 85 s | 1.50 and 1.53% errors | 0 errors |
| 120,000 | OOM-killed at 55 s | OOM-killed at 72 s | 2.5% errors (x2) | 0 errors |

"Old guard" is the #228 build with `memory.current` as input; shedding on is
84 / 37 MB, accept floor 18 MB, conn cap 74,400 (scaled to 300 MB); no shedding
is `--shed-force 0 --shed-hi-mb 0 --shed-lo-mb 0 --conn-cap 4000000
--accept-floor-mb 1 --idle-timeout-s 0`. The "cache-free, shedding on" column
also includes a slewed and budgeted shed fraction that is not part of this
follow-up. The "cache-free, no shedding" runs used an accept floor of 1 MB;
the shipped default is 96 MB, so they are not the default configuration. Why
the old build was killed with shedding disabled, when the new build was not, was
not isolated. Latency in the no-shedding column at 120,000: p95 93 ms,
p99 274 ms (about 24.7 us per request by the server's counter); with shedding
p95 was 23 to 26 ms. At 160,000 and 200,000 users the plain server was
OOM-killed in this cgroup (at 105 s and 51 s); a hand-scaled shedding variant
was also killed at both. Each of those was a single run.

*What follows.* Pressure is now measured without page cache; shedding is off by
default; `--hard-conn-cap` is available and off. Whether any of this helps on
the 2 GB box is not known.
