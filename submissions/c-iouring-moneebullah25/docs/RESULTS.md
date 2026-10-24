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
| Correctness | 42/42 official, 148/148 own, unit + fuzz clean, ASAN/UBSAN clean | 42/42 |
| CPU per request, batched (64 VUs, no think time, ~23k req/s) | **29.8-31.1 us** (user+sys, loopback) | <= 20 us on a Xeon 8260 for 350k; best entry 30 us |
| CPU per request, 100k users at ~10k req/s (batches of 2-4) | **36-39 us** | |
| CPU per request, 10k users at 1k req/s (one request per wake-up) | **60 us** | |
| Server RSS | 95 MB idle (prefaulted pools), 117 MB at 100k connections | ~40 MB app |
| User-space state per connection | 48 B record; 22 B/conn amortized RSS growth 10k -> 100k | 64 B |
| MemAvailable per loopback connection pair incl. generator | ~12-17 KB | 8.3 KB kernel per pair (exploration) |
| Connections held locally | 100k (2 rings) and 200k (4 rings), see below | 300-400k on the box |
| Feed strategy | range scans save 2.7 us per average request vs the reference SQL | -25% on feed |
| PGO | no CPU gain (30.1-31.8 vs 29.8 us), lower throughput; not shipped | +10-15% [E] |
| Checkpoints (4,000-frame trigger) | passive, off-loop; 44 per 90 s at ~600 writes/s, avg 106 ms, max 451 ms; longest COMMIT on the loop 48 ms | 100-770 ms on-loop avoided |
| Largest local hold | 182,765 connections across 4 rings (generator-limited) | 300-400k on the box |

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

## 5. Build reproducibility

`fetch-deps.sh` verifies SQLite's zip against the SHA3-256 published on
sqlite.org (`92281c86...8e30`) and liburing's tarball against its SHA-256
(`8d052f26...120a`, the same value another entry pinned independently).
`make release` on this machine takes about 2 minutes (SQLite at -O3 with LTO
dominates); `make pgo` about 6 minutes including the training run.
