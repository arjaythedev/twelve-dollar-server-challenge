# Status

Last updated: 2026-10-24. Everything here was checked on a 2-core AMD Athlon
Silver 3050U workstation (Zen1, 14 GB, Ubuntu kernel 6.8.0-147, gcc 13.3), not
on a 1 vCPU / 2 GB droplet. See RESULTS.md for the numbers.

## What works (tested)

- The challenge suite `test/test.sh`: 42/42, with the ASAN/UBSAN build and the
  release build, started through the upstream `test/run.sh` (build.sh +
  start.sh).
- `test/http_tests.py`: 166 checks (148 plus 18 regressions from the
  independent review), all passing under ASAN and under UBSAN: exact bytes
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
- The memory guard thresholds (192 MB / 96 MB) were only exercised by reading
  the counters; they never triggered on a 14 GB machine. The cgroup headroom
  check was verified under `systemd-run --user --scope -p MemoryMax=`.
- Port 80 with `CAP_NET_BIND_SERVICE`: not possible without root here; the
  server binds whatever `HOST:PORT` says and was tested on high ports.
- Behaviour when the box's disk makes a checkpoint take seconds: the hold
  gives up after 500 ms and writes continue on a growing WAL; the WAL then only
  restarts on a later successful hold.
- PGO on the target CPU (Cascade Lake or Broadwell): the profile is collected on
  the box itself by `make pgo`, but the default build is plain LTO because PGO
  showed no gain here.

## Known risks

1. **CPU per request on the real box.** The batch-2 winner spent 30 us/request
   on the Xeon 8260. The local numbers in RESULTS.md are on a slower Zen1 core
   with the loopback client's cost mixed in; the real figure is unknown until a
   droplet run.
2. **Kernel socket memory at 300k+.** ~4 KB per established socket means the
   2 GB box is full near 350-400k regardless of this process; the memory guard
   turns that into a paused accept rather than an OOM kill, but the exact
   ceiling is the kernel's.
3. **Multishot-to-oneshot accept handoff** at ring boundaries was exercised
   3 times per local run (100k and 183k connections over 2-4 rings) with zero
   accept errors after the fix; before it, each boundary lost one connection.
4. **One unexplained 129 s request** in the 183k-connection run, while the
   generator was also stalled; not reproduced at 100k (six runs).
5. **A single thread means any stall is global.** The only blocking calls on
   the loop are `COMMIT` (WAL append, no fsync) and the one-shot `io_uring_enter`.
   A slow disk shows up as commit latency, not as a hang.
6. **Rule 7 and PGO:** training uses a synthetic database and secret generated
   by `tools/synthdb.c`; the seed is never read during the build.

## Questions for the organizer (deliberately not implemented)

- A direct B-tree page reader for the feed (technique 20 of the exploration)
  would be the largest remaining CPU lever. It arguably violates rule 2 ("the
  database is SQLite, used in-process") because it is another driver; not
  built.
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
