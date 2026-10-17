# C + raw epoll + SQLite

| | |
|---|---|
| Language | C (GNU C11, `gcc -O3`, LTO, profile-guided optimization) |
| Framework | None. Single-threaded HTTP/1.1 on edge-triggered `epoll`, written for this challenge (~2,000 lines, no third-party code) |
| Libraries | SQLite 3.53.x amalgamation (pinned tarball, SHA-256 verified, compiled with size/speed trims). No OpenSSL, no JSON library, no HTTP library: SHA-256/HMAC, JSON and HTTP are hand-written (~900 lines, unit-tested) |
| **Nginx or direct** | **Direct**: serves `$HOST:$PORT` itself (tested on `0.0.0.0:80` semantics; no Nginx) |

## Run

```bash
sudo bash install.sh   # build-essential + curl, once as root
bash build.sh          # fetches pinned SQLite, PGO-trains it, links bin/server (a few minutes, one core)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

Only the four SPEC variables configure the server. Startup is instant; a restart
loop recovers from an OOM-kill (committed writes survive in the WAL).

## Why this architecture

A think-time workload (this challenge's k6 loop: ~2.2 requests per 11-30 s
iteration) is bound by **concurrent idle connections, not requests/sec**.
15,000 mostly-idle keep-alive sockets fit easily in 2 GB, until the
`LimitNOFILE=65535` file-descriptor ceiling turns into the score ceiling at
~64-70k users. Nearly every submission stops there, using 12–20% CPU.

This server's whole design is about spending that last 80% of CPU on real work
while sailing past the descriptor limit honestly:

- **Pressure-based eviction of idle keep-alives.** The server tracks its own
  open-socket count (O(1), no syscalls). Past ~60k fds it answers the oldest
  idle connections with a fully-drained `Connection: close`; past ~63k every
  response closes and accepts pause until pressure drops below ~55k
  (hysteresis, no oscillation). Clients transparently reconnect on their next
  request, which costs one accept (~100 µs) instead of one permanent fd. No
  API data is stored in the process: eviction keeps transport metadata only,
  every request still reads live SQLite data (rule 5).
- **One thread, no queues, no locks.** Reads run on the loop thread against a
  single `NOMUTEX` SQLite connection with persistent prepared statements: a
  feed is a few B-tree hits in the page cache. Writes of one `epoll_wait`
  batch share a single transaction (**group commit**); no response of the batch
  is sent before `COMMIT` returns, and a failed commit rolls back and converts
  exactly those responses to 500s (rule 6). Verified with a commit-fault test.
- **Idle connections hold no buffers** (64 B struct + kernel socket). Request
  parsing is in-place, responses are serialized once into a single buffer and
  sent with one call. Timeouts run on a small hierarchical timing wheel
  (75 s keep-alive, 30 s stall, 64 KB unsent cap with read backpressure).
- **SQLite tuned, schema untouched**: WAL + `synchronous=NORMAL` +
  `locking_mode=EXCLUSIVE`, 1 GiB mmap cap, 8 MiB page cache, WAL-hook
  passive checkpoints at 1,000 frames, `busy_timeout` 5 s, `TEMP_STORE=MEMORY`,
  compile trims (`THREADSAFE=0`, `MEMSTATUS=0`, `OMIT_*`, `USE_ALLOCA`), and a
  PGO build trained on a synthetic database. `INSERT` + `SELECT created_at`
  avoids `RETURNING`'s ephemeral table.
- **Paranoid input handling, boring crypto.** Hand-rolled SHA-256/HMAC-SHA256
  (RFC 4231 vectors), base64url, strict JSON, constant-time signature compare,
  HS256-only with per-request `exp` check, `auth-before-id` error ordering,
  `100-continue` answered from headers, duplicate `Content-Length` rejected,
  16 KB request cap, `EMFILE` backoff (never accept-spin), soft `RLIMIT_NOFILE`
  raised to the hard limit at startup (in-process, rule 11).

## Measured (CachyOS x86_64, Zen 5, droplet-limits container: 1 CPU, 2 GB, no swap, nofile=65535)

### Where is the ceiling?
Proven: **110k users PASS with 1000x margin** (4.45M checks, 9 fails, p95
370us, p99 531us, official-shaped 2-min warmup + 5-min hold, final binary),
plus 70k/85k/100k PASS. At 120k the p95 sits right on the 500 ms limit
(510 ms: fail), and it climbs steeply after (~772 ms at 150k) — so the honest
max on equivalent hardware is **~110k users**, and the binding constraint is
**p95 latency**, not errors or memory. What sets it:
- **FDs are not the ceiling** (eviction holds ~60k by design) and **RAM is not
  the ceiling** (~250 MB at 150k, 8x headroom).
- Past ~60k users every excess user reconnects per iteration (~5k reconnects/s
  at 150k). Sheds are token-bucket paced (10k/s, no bursts); without pacing,
  mass closes overflow the listen backlog and TCP retransmit backoff avalanches
  (measured). The remaining cost is per-reconnect accept + iteration work.
- Our Docker harness adds kernel packet overhead the droplet will not have
  (125k conntrack entries from churned loopback connections; stock Ubuntu has
  no iptables rules forcing conntrack). Expect equal-or-better on the droplet.
- Bisected: 110k passes with 1000x headroom, 120k touches the limit, 135-150k exceed it.
  The knee is sharp (queueing cliff as eviction churn grows);
  the CPU wall sits a little beyond that. k6 itself cannot verify past ~85k on
  one box (~265 KB/VU); higher numbers use `dev/capgen.c` (k6-exact loop,
  ~100 bytes/VU, calibrated against k6).

| Test | Result |
|---|---|
| Official `test/test.sh` | **42/42** (plus 39 API-level + ~50 unit checks, ASan/UBSan clean) |
| Saturation, 64 conns, mixed 100:100:15:2 | **~112k req/s**, p99 ~1.3 ms (independent 4000-request validator: 4000/4000 byte-exact; k6 reports ~8% failed at this rate with zero timeouts or warnings, a tight-loop accounting quirk, see below) |
| k6 capacity, official `bench/load.js` | **70,000 users, 5-min hold: PASS**: 2.61M checks, 0 failed, p95 ~113 µs, p99 ~5.6 ms (final binary) |
| k6 capacity limit search | **85,000 users, 2-min hold: PASS** (1.56M checks, 0 failed, p95 ~400 µs, p99 ~8.5 ms); k6 RAM (~265 KB/VU) is the next wall, not the server |
| capgen limit search (k6-exact loop) | **110k/2-min PASS**, 120k marginal (p95 510 ms), 135k/150k over (see ceiling analysis) |
| Same-box competitors (saturation) | Rust/epoll ~107k · C/io_uring ~94k · C++/uWS ~83k · C/epoll ~81k · Zig ~80k · C/fio ~67k |
| Head-to-head capacity (70k, 5-min) | Champion C/io_uring passes too (p95 ~5 ms, p99 ~10.6 ms, ~330 MB); ours at p95 ~120 µs, p99 ~5.3 ms |

Caveats: a tuned Zen 5 core is faster than the droplet vCPU, so absolute numbers
won't transfer; rankings and limiters should. k6 needs ~265 KB/VU and one
ephemeral port per user: past ~28k users the load itself needs multiple
loopback IPs (used here) or the champion's lean load generator.

## Reproducing our measurements (`dev/`)

All numbers above were measured on CachyOS x86_64 (Ryzen 9 9955HX) with each
server in a `ubuntu:24.04` Docker container limited to `--cpuset-cpus=N
--cpus=1 --memory=2g --memory-swap=2g --ulimit nofile=65535:65535` (the
droplet envelope), k6 v2.3.0 on other cores, loopback. Two load-generator
facts matter past ~28k users: Linux gives one ephemeral-port range
(28k ports) per destination IP, so multi-IP targets (`127.0.0.2`, …) are
needed; and k6 needs ~265 KB/VU.

- `dev/saturate.sh <container> <subpath> <port> <label>`: 2x20 s mixed
  saturation (needs `TOKENS` pointing at the challenge `seed/tokens.json`).
- `dev/capacity.sh <vus> <ramp_s> <hold_s> <ip> [ip...]`: splits VUs across
  one k6 instance per dst IP (fresh DB, one server, official `bench/load.js`).
- `dev/confirm.sh <vus> <ip> [ip...]`: the official README protocol: 2-min
  warmup at 1k users, 60 s ramp, 5-min hold.
- `dev/evict_test.py <port>`: fd-pressure check with a small `ulimit -n`
  (tripwires scale down; proves shedding + clean reconnects).
- `tests/unit.c`, `tests/api_test.c`: portable unit tests and golden-exact
  API tests (`api_test` byte-compares `test/golden/`).

## Files

`src/server.c` loop + connections + eviction · `src/conn.h` state + wheel ·
`src/http.c` parser · `src/api.c` endpoints · `src/db.c` SQLite ·
`src/jwt.c` SHA-256/HMAC/JWT · `src/json.c` strict JSON + escaping ·
`tests/unit.c` portable unit tests · `tests/api_test.c` golden-exact API tests
(byte-compares `test/golden/`) · `dev/` harness scripts and probes.

## License

MIT, under the challenge repository's MIT license.
