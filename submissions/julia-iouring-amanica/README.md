# julia-iouring-amanica

Julia 1.13.3, compiled ahead of time (`juliac --trim`) · no framework · SQLite 3.54.1 (amalgamation compiled from source, called with `ccall`) ·
JWT HMAC from Ubuntu's OpenSSL 3 (`libssl3t64`) · **direct** (no Nginx: serves `0.0.0.0:80`)

One Julia thread runs an io_uring event loop through raw syscalls; SQLite is the pinned amalgamation,
used through its public C API only (no B-tree reads). Locally (1 CPU / 2 GB container, wrk-based users
with think time) it holds 225,000–275,000 users and passes 300,000 about half the time, depending on how
busy the laptop is; running a leaderboard entry through the same harness on the same nights suggests
roughly 250–290k on the benchmark box. See "Running locally" for how that was measured.

## How it works

- Single thread, no libuv, no Tasks. Transport (`TRANSPORT` in `config.sh`, default `auto`):
  - **io_uring** through raw syscalls (no liburing). Sockets are direct descriptors in the rings' file
    tables, not process fds, so `LimitNOFILE=65535` doesn't cap connections: a new ring opens when one is
    full. Multishot receives into a provided-buffer ring (an idle connection holds no buffer), 4096
    accepts in flight, sends from refcounted arenas, close = shutdown linked to close.
  - **epoll** fallback (level-triggered, raw fds) when io_uring is unavailable (seccomp, old kernel); capped
    at ~65k connections by the fd limit.
- HTTP/1.1 parsed in place in one read buffer shared by all connections; responses written straight into
  a shared output buffer (no intermediate objects, no JSON library). An idle connection holds no buffers;
  0 bytes allocated per request, so the GC does not run under load.
- No Julia packages: the server uses only Base and `ccall`.
- **Compiled ahead of time** with `--trim` (`build.sh`): Julia's own juliac build script, which ships in the
  pinned Julia tarball (`share/julia/test/trimming/juliac-buildscript.jl`; JuliaC.jl is the packaged front
  end to it), compiles `src/app.jl` and everything reachable from it into a 2.5 MB executable, linked
  against libjulia. Built for the CPU it is built on (`--cpu-target=native`, as the JIT would; generic
  x86-64 was ~6% slower). Boot to a healthy `/health` 3.6–4.3 s → 72–91 ms; server RSS at 250k users
  480 → 234 MB, GC time per minute 66 → 5 ms; req/s the same (blast medians 50.6k JIT, 51.4k AOT).
  To be trimmable, logging writes straight to fd 1 instead of `println(stdout, …)` (`stdout` is an
  abstract `IO`). The dev-only `JR_*` switches are constants, so they are fixed when it's built. If the
  build fails, `start.sh` runs `src/main.jl` on the JIT instead (`JR_AOT=0` forces that).
- JWT HS256 verified on every request with libcrypto's HMAC: one `HMAC_CTX` keyed once at startup and
  re-initialised per token (the key pads are hashed once), so each token still gets its full HMAC but
  skips OpenSSL 3's per-call setup in one-shot `HMAC()`: 2.0 → 0.66 µs per token (`dev/ct.sh jwtbench`).
- SQLite through its public C API only: prepared statements for every read and write (no B-tree internals).
  WAL checkpoints run on a background thread, so their fsyncs don't block the event loop.
- Group commit (io_uring): all writes of one event-loop pass share one transaction, committed before the
  loop hands any reply to the kernel, so each 201 still waits for its commit (rule 6). Max req/s went up
  ~48% (blast 30k → 45k); at 150k users p95 fell from 4–27 ms to 1.5–3.7 ms.
- Reads share the pass's transaction too: one WAL snapshot per pass instead of one per statement (~3 per
  GET /feed). Each request still runs its own queries (rule 5). /feed alone: 18–20 → 12–13 µs; at 200k
  users, CPU at the same request rate 89–98% → 83–93% (3 alternating pairs, lower in each).
- GET /feed: when the 20 newest posts have consecutive ids (the usual case), their like counts come from
  one range scan instead of 20 lookups; otherwise the plain query. Same JSON, live data on every request.
  Blast ~+12% req/s; at 200k users p95 fell from 12–43 ms to 4–18 ms.

## Credits

The code is our own, in Julia, but most of the design follows what other entries found and wrote up. Thanks to:

- **The video walkthrough of the top 10** ("218 Developers COMPETE to max out a $12 server") and its
  maintainer: the overall lessons (io_uring direct descriptors over several rings to get past the 65k fd
  limit, serving directly without Nginx, bounded loop passes, group commit, checkpoints off the event loop,
  no allocation on the request path).
- **[fsharp-kestrel-delneg](../fsharp-kestrel-delneg/)**: checkpoints on their own thread and connection,
  holding writes (reads go on) once the WAL reaches 15,000 frames, letting that thread restart the WAL so
  the header `fsync` stays off the loop, and the Ubuntu 6.8.0-147 workaround for
  `IORING_REGISTER_PBUF_RING` (retry with `resv[0]=1`).
- **[rust-epoll-scooter1337](../rust-epoll-scooter1337/)**: the feed range scan: find the 20 newest ids,
  fetch rows and like counts with range scans when they are close together, and fall back to the plain
  query otherwise. Ours is a narrower variant (only consecutive ids; the counts come from one scan).
  Also, with crystal-epoll-faustinoaq: one transaction per loop pass for the reads as well as the writes.
- **[crystal-epoll-faustinoaq](../crystal-epoll-faustinoaq/)**: no garbage collection on the request path,
  multishot receives into a provided-buffer ring (an idle connection holds no buffer), and responses sent
  from arenas that stay pinned until the kernel has finished sending.
- **[asm-epoll-umitaltintas](../asm-epoll-umitaltintas/)**: single-shot accepts kept in flight instead of
  one multishot accept, which stalled under bursts.
- **[c-iouring-meros](../c-iouring-meros/)** and **[c-iouring-pavlyhalim](../c-iouring-pavlyhalim/)**:
  not borrowed from, but run through our local harness to compare this machine with the leaderboard.

## Files

| File | |
|---|---|
| `config.sh` | pinned versions (Julia, SQLite) and build switches |
| `install.sh` | root: build-essential, curl, unzip, openssl; the official Julia tarball (sha256 checked) |
| `build.sh` | user: downloads the SQLite amalgamation (SHA3-256 checked), compiles `build/libjr.so`, then the server ahead of time into `build/server` |
| `start.sh` | runs `build/server` (or `src/main.jl` on the JIT if there is none) |
| `src/` | the server |
| `csrc/shim.c` | WAL checkpoints on a background thread (public SQLite API), compiled into `build/libjr.so` |
| `dev/` | **development only**: local test and benchmark tooling (podman, k6/wrk, `tools.jl`), see below |
| `stress-results.csv` | every local stress run, one row each (written by `dev/`) |

## Running locally

Everything in `dev/` (and `stress-results.csv`) exists only to test and benchmark while developing. The
submission itself is `install.sh`, `build.sh`, `start.sh`, `config.sh` and `src/`: none of them use `dev/`,
and the benchmark box never runs it. The dev tools' Julia package (JSON.jl) lives in `dev/`'s own project
environment; the server has no package dependencies.

Needs podman (rootless is fine), Julia on the host for `dev/tools.jl` (records runs and builds the table
below; its own pinned environment in `dev/Project.toml`, installed on first use; juliaup's `1.13` channel
is used if present, or set `JULIA_DEV`), plus `seed/feed.db` (from the repo root: `bash seed/make-seed.sh`).
Everything runs from this folder, in containers that mimic the box: Ubuntu 24.04, server limited to
1 CPU / 2 GB / no swap / nofile 65535. Each run copies the submission fresh into the container, so local
build output never leaks in.

```bash
dev/ct.sh test                                    # build + start + the 42 checks, as CI does
LABEL="what changed" dev/ct.sh stress 10000 1m    # one k6 run with 10k users → a row in stress-results.csv
LABEL="what changed" dev/ct.sh ladder 2500 20000  # double users until a run fails, then bisect to within 5%
GEN=blast LABEL="what changed" dev/ct.sh stress 256 30s  # cheap max-throughput run, 256 connections
GEN=blast LABEL="what changed" dev/ct.sh ladder          # connections 16 → 16384 (mostly shows queueing, see caveats)
GEN=users LABEL="what changed" dev/ct.sh --debug stress 80000 1m # 80k users with think time; --debug checks they all connected
GEN=users LABEL="what changed" dev/ct.sh ladder         # users 10k → 320k
dev/ct.sh report                                  # rebuild the results tables below from the CSV
dev/ct.sh shell                                   # shell inside the Ubuntu 24.04 dev image
dev/ct.sh clean                                   # remove stress containers left by an interrupted run
```

Three load generators (`GEN`):

| `GEN` | What it runs | N means | Generator memory | Use it for |
|---|---|---|---|---|
| `k6` (default) | `bench/load.js`, the scoring's own script | virtual users | ~0.3 MB per user: this 32 GB host tops out near 20k | the real traffic shape, small scale |
| `blast` | wrk + `dev/blast.lua`, no think time | open connections | a few MB | max req/s at 1 CPU, comparing optimizations |
| `users` | wrk + `dev/users.lua`, with think time | users (one connection each) | ~22 KB per user | many open connections at realistic req/s |

How they differ:

- **`k6`** is what the scoring runs. Each virtual user is one person on one keep-alive connection, with
  its own token, following a fixed sequence: GET /feed → think 3–7 s → GET /posts/:id (a post from that
  feed) → think 3–8 s → like it (15%) → write a post (2%) → idle 5–15 s → repeat. That is ~2.17 requests
  per ~20.5 s loop, about 0.106 req/s per user. Users ramp up linearly over `RAMP_UP`. Each user is a
  JavaScript VM, hence the memory: it can't reach the user counts this server handles.
- **`blast`** is closed-loop: each connection sends its next request the moment the previous reply
  arrives. Every request is drawn at random with the same weights as a k6 loop (feed 1 : post 1 :
  like 0.15 : create 0.02), with a post id from the last feed that wrk thread saw and a token from a
  random sample of 1,000 users per thread. It keeps the server saturated, so it measures throughput;
  its latency is mostly queueing (latency ≈ connections ÷ req/s).
- **`users`** is `blast` plus a random pause of 3–16 s (mean 9.5 s) before every request, so one
  connection sends about 0.105 req/s, like a k6 user. It's a cheap stand-in for k6 at 100k+ users, with
  two differences: wrk's Lua state is per thread, not per connection, so there is no per-user sequence
  (the pause and request type are drawn independently each time); and each wrk process opens all its
  connections at once, so the ramp is a series of bursts (one per shard, spread over `RAMP_UP`) rather
  than k6's smooth line. The memory is mostly wrk itself: an 8 KB receive buffer per connection plus the
  response buffers it keeps (the script reads feeds for post ids), and ~4 KB of kernel socket state.

All three run on this host, outside the server's 1-CPU/2 GB container, so they don't use the server's
CPU budget, but they do compete for the host's memory (see caveats).

Every run starts a fresh server on a fresh copy of the seed database (like the scoring), checks the same
thresholds (p95 < 500 ms, p99 < 1 s, errors < 1%), samples server CPU / RSS every 2 s, and records the boot
time (`start.sh` → first healthy `/health`). Raw output goes to `results/<time>-<gen><N>/` (gitignored).

wrk runs as several parallel processes (shards) in one generator container: `CONNS_PER_SHARD` (5000) or
`SHARDS` sets how many, `WRK_THREADS` (4) the threads in each. Each shard connects to its own `127.0.0.x`
(the server binds `0.0.0.0`), so one client address never runs out of its ~64k ephemeral ports. For `users`,
shard start times are spread over `RAMP_UP`, because wrk opens all its connections at once.

`--debug` (first argument, e.g. `dev/ct.sh --debug stress 60000`) also counts the server's open connections
(`users` runs then fail if fewer than 99% of users connected) and makes the server log allocation/GC stats
every 10 s (`JR_DEBUG=1`; bytes per request should stay 0). Off by default so measuring costs nothing. CPU
and memory are always sampled from the host side (cgroup files, `/proc`), outside the server's 1-CPU budget.
The CSV's `debug` column says which mode a run used.

The server's database copy lives on a per-run podman volume on the host's disk (`DATA=volume`, the default).
`DATA=overlay` puts it on the container's own fuse-overlayfs instead, where throughput measured about twice
as high (probably cheaper checkpoint fsyncs, not verified), so numbers there may flatter the server: runs
before 2026-11-08 used it.

Other settings: `NOTES` (free text in the row), `RAMP_UP` (30s, k6 and users), `WARMUP` (5s, blast),
`CPUS` / `MEM` (1 / 2g), `SECCOMP=default` (podman's default profile, which blocks io_uring).

Caveats:
- Don't run two stress runs at once, and close heavy apps first: the generator shares the host. If the
  host runs short of memory, the generator stalls, and so does the server: its first touch of new memory
  waits behind the host's swap writes (seen at 200k users, 2026-11-11). The run then fails for reasons
  unrelated to the server. A note is printed when a run fails while server CPU stayed below 85%. A low
  `vm.swappiness` (e.g. 10) makes the host drop file cache before swapping, which helps a little; record
  host changes like that in `NOTES`.
- Keep k6 users below about (free MB ÷ 0.3) and `users` below about (free MB ÷ 0.022) (`free -m`),
  leaving a few GB for the server and page cache.
- Blast's "est. users" (req/s × 10.6, the leaderboard ratio) is inflated: this desktop core is faster than
  the benchmark's shared Xeon vCPU, and blast does not include the cost of holding hundreds of thousands of
  idle connections. Compare blast numbers with each other only.
- Blast is closed-loop, so once the server is saturated, more connections only add queueing:
  latency ≈ connections ÷ req/s. The useful blast number is the saturated req/s (a single run at
  256 connections is enough). The blast ladder's "highest passing" just marks where that queue passes 500 ms.

### Calibrating against the leaderboard

`ENTRY=<folder> dev/ct.sh stress ...` runs another submission through the same harness (its rows go to
`calibration.csv`, not to the table below). With a ramp like `load.js`'s (`RAMP_UP=60s
CONNS_PER_SHARD=2000 WRK_THREADS=2`), 1-minute `users` steps on this laptop, on two nights:

| entry | leaderboard | 2026-11-15: passes / fails | 2026-11-16: passes / fails |
|---|---:|---|---|
| c-iouring-pavlyhalim (#245) | 295,000 | 275,000 / 300,000 | 275,000 and 300,000 / – |
| julia-iouring-amanica (this) | – | 225,000 (2 of 2), 250,000 (1 of 2) / 250,000 (1 of 2) | 275,000 (all), 300,000 (3 of 5) / 300,000 (2 of 5: p95 516, 673 ms) |
| c-iouring-meros (#137) | 235,000 | – / 175,000, with idle CPU | not run |

The second night the host had more memory to spare and both entries passed more; ours moved up more
(the 300,000 runs include the HMAC change below, which passed 2 of 2). Taking each night's gap to
pavlyhalim: roughly 250–290k on the box. At 300,000 pavlyhalim's CPU averaged 60%, ours 80%: most of
that gap is its B-tree reads, which this entry doesn't use.
pavlyhalim's local limit matches its score, so the laptop and this ramp are a fair stand-in for the box.
meros fails here far below its score while its CPU is mostly idle, so something in this harness doesn't
suit it, and it isn't used for calibration. With the default, burstier ramp (5,000 connections per wrk
shard at once) both C entries fail lower still, while this entry passed 275,000: its accept path copes
with bursts, so burst-ramp numbers flatter it. Our failures at 225–250k are single stalls (one write
blocked for 1–4 s while the whole host stalled), not steady overload.

### Tried and dropped

Measured one at a time, kept only if they helped: SQLite at `-O3`, `-march=x86-64-v3`, extra
`SQLITE_OMIT_*` options, SQLite PGO (0–7% even as an upper bound), `mmap_size=0` (no throughput change,
worse p95), a larger page cache, a smaller write-hold threshold (4,000 WAL pages: 3× the holds, worse
everywhere), restarting the WAL from the checkpoint thread after every catch-up (more fsyncs, worse
latency), prefaulting buffers (moves page faults, doesn't remove them). A writer thread (a C pthread with
its own connection commits the POSTs in batches while the loop keeps serving reads; replies still wait for
the commit) was a draw against group commit on the loop: 8 of 9 runs passed each way at 225–275k users,
and it used 3–4 points more CPU. It stays in the code behind `WRITER=on` (`JR_WRITER=on`), off by default.
Reading text columns through one `sqlite3_column_value` instead of `sqlite3_column_text` + `_bytes` made
no difference to GET /feed (18.4 → 17.9 µs, noise).

## Progress

<!-- results:begin -->
Best local result per change. Raw runs: [stress-results.csv](stress-results.csv); how to run: above.
Blast req/s (max throughput at 1 CPU): median of the passing runs (they vary ~10%), with % change against the
previous change and the number of runs. Changes under ~10% are noise unless they repeat.
k6 / wrk users = highest passing run (not always a full ladder). k6 is capped near 20k users on this host;
wrk users (`GEN=users`, think time per connection) are cheap, so they reach the connection limits.

| change | k6 users | wrk users (server RSS) | blast req/s |
|---|--:|--:|--:|
| epoll baseline | 19,350 | – | – |
| epoll + heap-hint 512M | 10,000 | 60,000 (990 MB) | 23,832, n=12 |
| P3: shared buffers | – | 60,000 (315 MB) | 21,288 (-11%), n=6 |
| P4: io_uring | – | 175,000 (396 MB) | 24,699 (+16%), n=4 |
| P5.2: checkpoint auto | – | – | 11,448 (-54%), n=3 |
| P5.2: checkpoint thread | – | – | 32,668 (+185%), n=3 |
| P5.3: write hold | – | 225,000 (460 MB) | 30,557 (-6%), n=1 |
| P5.3b: restart loop | – | 175,000 (426 MB) | – |
| P5.3b: restart thread | – | 175,000 (428 MB) | – |
| P5.3c: restart hold | – | 175,000 (433 MB) | – |
| P5.3c: restart catchup | – | – | – |
| P5.4: group off | – | 150,000 (418 MB) | 30,472 (-0%), n=3 |
| P5.4: group on | – | 150,000 (420 MB) | 45,018 (+48%), n=3 |
| P5.4: users step | – | 200,000 (451 MB) | – |
| P5.7: hold_pages 15000 | – | 150,000 (415 MB) | 46,284 (+3%), n=3 |
| P5.7: hold_pages 4000 | – | 150,000 (418 MB) | 40,831 (-12%), n=3 |
| P5.5: feed plain | – | 200,000 (453 MB) | 43,079 (+6%), n=6 |
| P5.5: feed range | – | 200,000 (442 MB) | 50,609 (+17%), n=5 |
| P7: 5 min hold | 15,000 | 175,000 (442 MB) | – |
| P5.7: mmap 256M | – | 225,000 (467 MB) | 47,735 (-6%), n=3 |
| P5.7: mmap off | – | 225,000 (430 MB) | 48,335 (+1%), n=3 |
| P5.9: writer smoke | – | 20,000 (372 MB) | – |
| P5.9: writer thread | – | 275,000 (446 MB) | – |
| P5.9: writes on loop | – | 275,000 (507 MB) | – |
| calib: ours smooth | – | 275,000 (498 MB) | – |
| P7.1: hmac ctx | – | 300,000 (506 MB) | 54,773 (+13%), n=3 |
| P7.1: hmac oneshot | – | 300,000 (497 MB) | 52,991 (-3%), n=3 |
| P2: aot generic cpu | – | – | 48,639 (-8%), n=3 |
| P2: jit | – | 250,000 (480 MB) | 51,076 (+5%), n=6 |
| P2: aot native | – | 250,000 (234 MB) | 51,396 (+1%), n=3 |
| P8.2: read txn per pass | – | 250,000 (234 MB) | 52,437 (+2%), n=2 |
| P8.2: read txn off | – | 250,000 (238 MB) | 50,815 (-3%), n=3 |
| P8.5: batch 32 | – | 200,000 (197 MB) | – |
| P8.5: batch off | – | 200,000 (196 MB) | – |
| P8.5: batch 128 | – | 200,000 (211 MB) | – |
| P8.6: journal_size_limit=0 | – | 225,000 (204 MB) | – |
| P8.6: journal_size_limit default | – | 225,000 (199 MB) | – |
| HEAD aot+8.2 | – | 240,000 (209 MB) | – |
<!-- results:end -->
