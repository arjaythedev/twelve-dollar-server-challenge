# Performance

What was measured, how to reproduce it, and what the numbers do and do not mean.

## What the challenge measures

- **users** = k6 virtual users. Each user runs `bench/load.js`: `GET /feed`,
  think 3-7 s, `GET /posts/:id`, think 3-8 s, then 15 % like / 2 % create, think
  5-15 s, repeat. That is **~2.2 requests per ~20.5 s per user**, each holding one
  keep-alive connection.
- **score** = the most users that survive a **5-minute hold** with
  **p95 < 500 ms**, **p99 < 1 s**, **< 1 % failed**.
- Because the request rate is low, the bottleneck is *holding connections* and
  *tail latency*, not request throughput.

## How to reproduce

Correctness (fast, no external services):

```bash
bash install.sh          # pinned Nim toolchain
bash build.sh            # compiles bin/server (SQLite vendored in-tree)
bash test/run.sh submissions/nim-io_uring-NioHast  # 42/42, byte-exact
```

Load (the repo's own harness):

```bash
# server
SQLITE_PATH=<copy-of-feed.db> JWT_SECRET=twelve-dollar-challenge \
  HOST=127.0.0.1 PORT=3000 bash start.sh
# load (k6), on another machine pointing at the server
k6 run -e VUS=20000 -e DURATION=60s -e RAMP_UP=60s bench/load.js
```

Two practical notes:

- **k6 needs a lot of RAM per VU** (~250 KB-5 MB), so a single small box cannot
  generate hundreds of thousands of users; the organiser used several separately
  provisioned hosts. Use as many generator machines as you have (each also adds a
  source IP, which raises the per-machine connection ceiling).
- For very high connection counts, a **lean custom generator** that binds many
  source addresses (`127.0.0.x` on loopback, or extra IPs / an IPv6 prefix over a
  real network) can hold far more connections than k6 on the same hardware.

## What was measured (local development box)

The dev box is a 4 vCPU / ~3.8 GB WSL2 machine. These are **indicative only**,
the generator and the server share one machine, over loopback.

Official suite: **42/42** for the io_uring transport.

Connection capacity (server, one thread, **dev box, loopback**):

| Test | Result |
|---|---|
| Hold idle keep-alive connections (idle sweep effectively off) | **hundreds of thousands** simultaneously on loopback, `ss` confirmed, `/health` 200, RSS peak ≈ 75 MiB |
| Absorb connections with churn | connections established continuously while tens of thousands are held at once |
| Per-connection memory | ≈ 0.25 KiB idle |

**These are loopback figures.** On loopback the kernel's conntrack table is not engaged, so the test
exercises the socket and memory path but **not** the conntrack path. It says the server can hold a very
large number of connections with tiny per-connection cost; it does **not** say the target box can,
because on the target box the practical ceiling is kernel connection bookkeeping (see
[Honest limitations](#honest-limitations)), which loopback bypasses.

Load with the repo's `bench/load.js` (k6, co-located, server pinned to one core):

| VUs | p95 | p99 | failed | verdict |
|---:|---:|---:|---:|---|
| 2,500 | 58 ms | 212 ms | 0 % | pass |
| 5,000 | 170 ms | 660 ms | 0 % | pass |
| 8,000 | 116 ms | 299 ms | 0 % | pass |
| 10,000 | 2.3 s | 4.0 s | 0 % | fail (generator-bound) |

At 10k the k6 process (and the shared box) saturates before the server does; the
server is around one core and ~100 MiB. That is why the absolute ceiling here is
a property of the *generator and the box*, not of the server.

Robustness battery (still green after the hardening): fragmented and pipelined
requests, 200 concurrent slow readers (a separate health-check client stays under
~30 ms), 800 random malformed requests (server stays up), and 12,000-connection
churn (RSS grows by a few MiB, fd count stable).

## Why it is fast

- **No reverse proxy.** Nginx costs 14-20 % of the single core.
- **One event loop, no worker pool.** No thread hand-off or per-request objects.
- **Statements prepared once** (`SQLITE_PREPARE_PERSISTENT`, `SQLITE_ENABLE_QPSG`); SQL never re-parses.
- **mmap + a small page cache** (`SQLITE_MMAP_SIZE`, default 1 GiB, `cache_size=-1000`) so reads come
  from the OS page cache.
- **WAL checkpoint off the hot path** (`wal_autocheckpoint=0` + a ~1/s PASSIVE checkpoint): no request
  pays a checkpoint fsync.
- **One `send` per response**, with send re-submission for slow readers and a 64 KiB output cap
  (`MaxOutBuf`) for deep pipelines.
- **Runtime-sized registered-fd table** (sized from `RLIMIT_NOFILE`) so held connections are not capped
  by the process fd limit, plus **CQ headroom** via `IORING_SETUP_CQSIZE` and SQ/CQ overflow handling,
  and a **64 x 8 KiB provided-buffer pool** so idle connections hold no recv buffer.
- **Dense-range feed scan** (three cheap range lookups when the top-20 ids are dense) with the original
  ordered query kept as the decline path, and **zero-allocation JSON writes**.
- **Vendored SQLite with trims** (`THREADSAFE=0`, `OMIT_*`, JSON1) and a
  `-d:release -d:lto -march=native` build.
- **O(n) request parsing** (cursor-based, no per-request buffer shift).
- **Profile-guided optimization (PGO)** on the Nim build (two-pass, synthetic training, with a
  plain-build fallback when PGO is unavailable).
- **Write group commit** (default on, `GROUP_COMMIT=0` to disable): writes issued during one
  event-loop pass share a single transaction that commits before any response is sent.

### Transport note: io_uring receive and accept modes

The io_uring transport uses **multishot** recv over a shared provided-buffer pool and
**multishot + direct** accept, which is what delivers the lower CPU at 10k-20k users. Both keep a
one-env-var escape hatch if a tail-latency regression ever appears: `IOURING_RECV=oneshot` re-arms a
one-shot `IORING_OP_RECV`, and `IOURING_ACCEPT=oneshot` does the same for accept. The older NASM
submission #86 saw multishot recv fail a 275,000-user p99 screen (1,187.6 ms) while a one-shot control
passed at p99 90.9 ms - a population this box cannot reach - which is exactly why the one-shot escape
hatch is kept, while multishot stays the default for the scored workload.

## Honest limitations

- The numbers above are **not the official score**. The official figure comes from
  the organiser running k6 against a 1 vCPU / 2 GB droplet with load from
  separate machines.
- Local runs share one box and use loopback, where the kernel conntrack table is
  not engaged and the CPU is faster than a droplet vCPU, so local numbers are
  **optimistic and environment-limited**.
- On the real box the ceiling is set by kernel connection bookkeeping, not by the
  server: `LimitNOFILE = 65535` bounds the registered fixed-file table (sized
  `min(hard - 64, 65536)`), and the machine-wide conntrack table is about 64k on a
  2 GB droplet. Registered fds lift the per-connection process-fd wall but **not**
  conntrack. Because loopback does not exercise conntrack, that ~64k ceiling is
  argued from the target environment rather than measured locally.
