# Status

Last updated: 2026-10-31.

## Since the second scored run (commit 3f0c2c1: 230,000 users, 27 us/request, 66% CPU)

The organizer's result for 3f0c2c1: 230,000 users held 5 min at 22,377 req/s,
p95 9.3 ms, p99 37 ms, 0 errors, 66% CPU (27 us/request, down from 38), 1,106 MB
RAM. The 240,000 probe failed on latency (p95 5.8 s) while the memory guard
paused accepting twice (252 MB available). The 320,000 probe ended with the box
rebooting and nothing in the app's log (memory pressure outside the app). This
follow-up (commit 5b86634) changes how the server behaves near the memory wall
and trims CPU and memory. What changed:

- Load shedding replaces the accept pause: a fraction f of responses carries
  `Connection: close`, so users do not hold a socket during their think time.
  f is the larger of a memory law (0 at 450 MB available or more, 1 at 200 MB or
  less) and a connection-count law (0 at 90% of `--conn-cap`, 1 at the cap,
  default 260,000). Accepting pauses only below 96 MB. Closes are rate-limited;
  keep-alives idle over 60 s are closed. The stats line gained `shed_f`,
  `shed_closes`, `idle_closes`, accepts per interval, `avail_mb` and
  `btree_fb`. This is general memory protection for any client.
- Feed and single-post reads go through SQLite's B-tree cursor API
  (`src/db_btree.c`), validated at open, with a per-request fallback to the SQL
  path and `DB_READS=sql` to turn it off. The same technique is used by
  lean-derive-hargup (#153) and c-iouring-public (#158); the organizer's
  rulings on those entries apply here.
- Close-path race fixed: a late shutdown could kill the next client that reused
  a fixed-file slot. The original lost the first request of 25 of 300
  reconnecting clients; the fixed server lost 0 of 3,000.
- Batch window 512 completions or 16 ms (was 32 and 2 ms), up to 512 requests
  per pass (was 256), send pool 1,024 x 72 KiB (was 512), lazily faulted.
- Footprint: no prefault by default, receive buffers 2,048 x 1 KiB per ring
  (was 4,096 x 2 KiB), SQLite cache 4 MiB (was 32 MiB), checkpoint at 1,000 WAL
  frames (was 4,000), write hold at 8,192 frames (was 16,384).
- The date header buffer is sized so the format-truncation warning is gone.

The server prints a stats line to stderr every 10 s by default
(`--stats-interval-s 0` turns it off), including `shed_f`, `avail_mb`, accepts
per interval and `btree_fb`; DESIGN.md explains how to read it to separate
memory pressure from a CPU limit.

Verified (laptop, 2 cores, not the box): unit tests, fuzz 300,000 iterations,
official suite 42 of 42 and black-box suite 177 of 177 under ASAN and UBSAN;
official suite 42 of 42 through `build.sh` with the PGO build; the B-tree path
byte for byte identical to the SQL path over all 500,633 ids and after 2,008
random writes (`tools/dbdiff.c`, 0 fallbacks); 20.7 us/request at F=0 and 27 to
28 at F=0.3 with the 16 ms window; about 22 to 38 us per reconnect; kernel memory
of about 2.5 KB per idle socket in the cgroup. Numbers and methods are in
RESULTS.md section 8. Runs disturbed by other load on the machine are excluded.

Not verified, stated plainly: serving 350,000 users needs about 33,000
requests per second. At the measured per-socket kernel cost the box cannot hold
350,000 sockets, so users must outnumber sockets and about 25 to 28 percent of
responses must carry `Connection: close`. The estimated CPU on the box at that
point is about 24 us per request by the server's counter (the laptop figure
scaled by the 0.9 ratio seen between the box and the laptop on the previous
version). The organizer's total CPU was about 10 percent above the server's own
counter on previous runs, which gives about 85 to 90 percent CPU. This is an
estimate. It has not been tested at box scale, no entry has yet held more than
280,000 users, and local runs only reached 10,000 to 20,000 connections with
shedding. No user-count score is claimed.

## Since the first scored run (commit 065c235: 209,000 users, 38 us/request, 83% CPU)

The organizer's result put this entry 8th at 38 us of CPU per request, with the
best p95 on the board (51 ms) and the entries above it at 28-33 us. Our own
RESULTS.md had already measured 27-30 us with passes of ~30 requests and 34-39
us with passes of 2-4, and the box ran four rings at 209k users: the loop was
waking on every secondary ring's completion (a multishot poll on the ring fd
fires on every pass while that CQ holds anything) and skipping the batch wait
whenever any ring had a completion, so it ran passes of a few requests, each
with an `io_uring_enter` per ring and a read and a write transaction. This
follow-up changes that and a few things around it:

- Time-paced passes: 32 CQEs or 2 ms once busy; only queued requests skip the
  wait; one-shot ring polls re-armed after a drain; every ring reaped once per
  pass. (The entries at 28-31 us pace at 1-2 ms.)
- Like counts for the feed tallied in C from one index row per like instead
  of `GROUP BY count(*)`: about 30% off the likes scan locally.
- SSE2 scan for bytes that need JSON escaping.
- 4,096 receive buffers per ring; send pool 512 x 72 KiB (36 MiB instead of
  72); write holds at 16,384 frames; memory guard at 256/128 MB every 100 ms.
- The SQ-full path recovers from a CQ overflow (`-EBUSY`) instead of aborting.
- The stats line splits CPU per request into user and system.

Unchanged: everything rule-related (no caching, commit before send, WAL +
NORMAL, pinned and hash-verified deps, direct on :80, nothing outside the
entry folder). Unverified on the box: all of the above; local numbers are in
RESULTS.md section 7. A 65,536-entry CQ was tried and dropped: ring memory
counts against the 8 MB `RLIMIT_MEMLOCK` a service gets, and seven such rings
do not fit. Everything here was checked on a 2-core AMD Athlon
Silver 3050U workstation (Zen1, 14 GB, Ubuntu kernel 6.8.0-147, gcc 13.3), not
on a 1 vCPU / 2 GB droplet. See RESULTS.md for the numbers.

## What works (tested)

- The challenge suite `test/test.sh`: 42/42, with the ASAN/UBSAN build and the
  release build, started through the upstream `test/run.sh` (build.sh +
  start.sh).
- `test/http_tests.py`: 177 checks (including 18 regressions from the
  independent review and the close-path race regression), all passing under ASAN and under UBSAN: exact bytes
  and key order, auth-before-id ordering, pipelining (including POST then GET
  on one connection), byte-by-byte and split requests, `Expect: 100-continue`,
  keep-alive reuse (3 s; 66 s with `--long`), 500 idle connections, malformed
  and oversized requests (400/413 + close), HTTP/1.0 and `Connection: close`,
  slowloris timeouts, 30 JWT edge cases (bad signature, wrong key, alg none,
  HS512 header with HS256 signature, expired, exp == now, missing/str/null exp,
  2 and 4 parts, padding, non-canonical base64, non-JSON header/payload, sub
  0/negative/float/number/empty/missing, username missing/number/null, header
  prefix variants), like idempotency (201, 200, 200), 40 concurrent likers, 300
  concurrent creates read back identically, feeds well-formed and ordered during
  concurrent writes, 1,500 connect/close cycles, 300 mid-request resets,
  SIGKILL right after a 201 (post and like survive restarts), 4 rings x 16
  slots and 3 rings derived from `RLIMIT_NOFILE=40`, connections beyond the
  table served once slots free up, 2,400 creates through write holds and
  checkpoints with p99 < 1 s, and identical feeds from the range-scan and the
  reference query.
- Regression checks from the independent review, in `test/http_tests.py`
  ("regressions from the review"): feeds of 20 maximal posts (emoji, quotes,
  control characters), 192 request+`shutdown(SHUT_WR)` clients all answered,
  no spurious `100 Continue` on incomplete headers, invalid UTF-8 and NUL
  bodies answered 400 (never a CHECK-constraint 500), HTTP/1.0 keep-alive
  announced, `Date` header, query strings ignored for routing.
- Unit tests (`make check`) for SHA-256 (FIPS vectors), HMAC (RFC 4231),
  base64url, JSON, JWT, HTTP; a 300k-iteration mutation fuzz pass over the
  three parsers under ASAN/UBSAN (`make fuzz`), no findings.
- Clean exit on SIGTERM with no leaks reported by LeakSanitizer.
- `cppcheck`/`clang-tidy` are not installed here and cannot be installed
  without root; `clang-format` came from a pip wheel. Not run.
- Warning-free under `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wvla
  -Wstrict-prototypes -Wmissing-prototypes` (third-party headers are
  `-isystem`). `clang-format` (config in `.clang-format`) applied to every
  source file.

## Not verified here (needs the real box)

- Any user count. This workstation shares 2 cores with the desktop and has
  14 GB; it cannot reproduce 1 vCPU / 2 GB, the 0.25 ms RTT network, virtio, or
  a 300k-connection test. The local connection holds in RESULTS.md show the
  server's own memory and CPU per request, not a score.
- 200k and beyond: the local generator stalled at ~183k connections (see
  RESULTS.md); the server held them, but no latency conclusion is possible
  above 100k from this machine.
- `LimitNOFILE=65535` sizing: emulated with `--rlimit-nofile` (the kernel caps
  fixed-file tables at the hard limit, which the probe on this kernel
  confirmed), so 7 rings x 65,535 is what the box will get; the multi-ring path
  was exercised with 16-, 40- and 4,096-slot rings, not 65,535.
- Kernel 6.8.0-117 on the box versus 6.8.0-147 here: same series. The
  `IORING_REGISTER_PBUF_RING` EINVAL seen here is the reason classic
  `PROVIDE_BUFFERS` is used; both kernels support it.
- The shedding thresholds (450 / 200 MB, conn cap 260,000, accept floor 96 MB)
  were exercised locally with `--shed-force` and a cgroup limit, never on the
  box's real memory wall. The cgroup headroom
  check was verified under `systemd-run --user --scope -p MemoryMax=`.
- Port 80 with `CAP_NET_BIND_SERVICE`: not possible without root here; the
  server binds whatever `HOST:PORT` says and was tested on high ports.
- Behaviour when the box's disk makes a checkpoint take seconds: the hold
  gives up after 500 ms and writes continue on a growing WAL; the WAL then only
  restarts on a later successful hold.
- PGO on the target CPU (Cascade Lake or Broadwell): the profile is collected on
  the box itself by `make pgo`, but `build.sh` falls back to plain LTO if training fails;
  PGO showed no gain on the laptop.

## Known risks

1. **CPU per request on the real box.** The batch-2 winner spent 30 us/request
   on the Xeon 8260. The local numbers in RESULTS.md are on a slower Zen1 core
   with the loopback client's cost mixed in; the real figure is unknown until a
   droplet run.
2. **Kernel socket memory at 300k+.** ~4 KB per established socket means the
   2 GB box is full near 350-400k regardless of this process; shedding
   and the accept floor turn that into closed connections rather than an OOM
   kill, but the exact ceiling is the kernel's (the 320,000 probe rebooted the
   box).
3. **Multishot-to-oneshot accept handoff** at ring boundaries was exercised
   3 times per local run (100k and 183k connections over 2-4 rings) with zero
   accept errors after the fix; before it, each boundary lost one connection.
4. **One unexplained 129 s request** in the 183k-connection run, while the
   generator was also stalled; not reproduced at 100k (six runs).
5. **A single thread means any stall is global.** The only blocking calls on
   the loop are `COMMIT` (WAL append, no fsync) and the one-shot `io_uring_enter`.
   A slow disk shows up as commit latency, not as a hang.
6. **Shedding raises the connect rate.** Each reconnect costs about 22 to 38 us
   of mostly kernel time, so at 25 to 28 percent of responses closing the CPU
   per request rises by roughly 6 to 10 us over the F=0 figure. Measured on the
   laptop only.
7. **The 16 ms batch window raises latency** (p95 20 to 35 ms locally against 3
   to 4 ms at 2 ms) in exchange for 10 to 15 percent less CPU.
8. **The B-tree path depends on SQLite 3.54.1 internals**, pinned by checksum.
   It validates the schema at open and falls back to SQL for any unusual record
   or schema change; `DB_READS=sql` disables it. Whether it is acceptable under
   rule 2 is the organizer's call (see the rulings on #153 and #158).
9. **Untested at box scale:** shedding, the larger window and the B-tree reads
   were only run locally at 10,000 to 20,000 connections.
10. **Rule 7 and PGO:** training uses a synthetic database and secret generated
   by `tools/synthdb.c`; the seed is never read during the build.

## Questions for the organizer (deliberately not implemented)

- The direct B-tree reads (`src/db_btree.c`) are built and on by default. They
  read SQLite's own pages through SQLite's own code, but they are a different
  API from SQL, so they arguably stretch rule 2 ("the database is SQLite, used
  in-process"). Same technique as #153 and #158; the organizer's rulings on
  those entries apply, and `DB_READS=sql` turns it off.
- Trimming: the SPEC says "empty after trimming whitespace". This server trims
  ASCII whitespace (space, \t, \n, \r, \v, \f). JavaScript's `trim()` also
  removes NBSP and other Unicode spaces; the load test sends plain ASCII.
- Character count: "500 characters" is counted in Unicode code points (what
  SQLite's `length()` and the CHECK constraint count). JavaScript's `.length`
  counts UTF-16 code units, which differs only for astral characters.
- Tokens with `exp` in the past by less than clock skew are rejected (no
  leeway), as the SPEC reads.

## Review findings deliberately left as they are

- **Pipelined input beyond 24 KiB** (unprocessed bytes on one connection) is
  closed without a 413. Only `max_head + max_body` of a connection's unread
  input is buffered; a client that pipelines a hundred requests in one write
  gets cut off at that point. k6 and curl never pipeline; answering 413 there
  would need a response while another may be in flight.
- **Send-pool exhaustion** by peers that stop reading is bounded by the 30 s
  sweep (a stalled send now counts like an incomplete request) rather than a
  per-send deadline, and parked connections wait rather than get an error.
  Under `tcp_mem` pressure at 300k+ sockets this is still the one mechanism
  that can slow every connection at once; untested here.
- **Write holds at 5k writes/s** are unmeasured (local runs reached ~600
  writes/s). The checkpointer now runs at nice 10 instead of `SCHED_IDLE` so a
  saturated loop cannot starve it completely; a hold still gives up after
  500 ms and the WAL keeps growing until the next successful hold.
- **A 5xx on a failed `COMMIT`** after a 50 ms busy wait (was 2 s): the loop
  never waits long; with PASSIVE checkpoints the write lock is never contended.

## Root-only steps on the benchmark box (not needed here, listed for completeness)

Nothing in this entry needs root beyond what the harness already does:
`install.sh` runs apt as root; the service runs as `deploy` with
`LimitNOFILE=65535` and `CAP_NET_BIND_SERVICE`. If you want to reproduce the
local load tests on a box of your own:

```
# the load generator raises its own soft RLIMIT_NOFILE to the hard limit (1,048,576 on this
# workstation, so no root was needed here); on a box whose hard limit is lower, raise it in
# /etc/security/limits.conf. The server never needs a higher limit: its fixed-file tables are
# sized from the hard limit it inherits.
# optional: emulate the 2 GB box for the server process without root
systemd-run --user --scope -p MemoryMax=2G -- env SQLITE_PATH=... ./bin/server
```
No sysctl, no conntrack, no pinning: rule 11.
