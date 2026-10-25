# Leaderboard

Every submission, scored on the same box with the same load test. New PRs are benchmarked as they come in and
added here, so this is the only leaderboard.

**The machine.** A **Synteq 1 vCPU / 2 GB VPS** (KVM, Xeon Platinum 8260 @ 2.4 GHz with AVX-512, 1,967 MB RAM, no
swap, Ubuntu 24.04, kernel 6.8), stock: no UFW (so no conntrack table) and no kernel tuning.

**How each entry is scored.** Built from source with its own install.sh (root) / build.sh (user `deploy`), started by
systemd as `deploy` with LimitNOFILE=65535 and CAP_NET_BIND_SERVICE on a fresh copy of the seed, and checked with the
42-check test suite before any load. Load comes from separate 16-core load generators in the same datacenter (0.25 ms
RTT) running the `bench/load.js` user model (feed → think → post → think → 15 % like, 2 % post). Search: 2-minute probes
doubling, then bisecting to ~5 %, then a 5-minute hold, stepping down 5 % until one passes. **Score = most users that
passed a 5-minute hold** with p95 < 500 ms, p99 < 1 s, errors < 1 %. Every probe restarts the app on a fresh database.

**Commits scored.** Each PR's head at the time of its run. Two PRs were updated afterwards, and their merged code is
newer than what was scored: go-epoll-MegaPanchamZ #72 (scored `9df7449`) and zig-iouring-marler8997 #125 (scored
`a08923d`).

**Waiting for a re-run.** The first submissions (#2–#29) were scored on a DigitalOcean $12 droplet, which is about 2×
slower for CPU-bound servers, so those numbers aren't comparable. They're listed at the bottom until they've been
re-run on this box. The old numbers are in [`benchmarks/batch-1.md`](benchmarks/batch-1.md).

## Results

CPU % and µs/request are the app's own cgroup over the scored hold (µs = app CPU time ÷ requests). RAM = app cgroup peak.

| # | Submission | PR | Language | Users held 5 min | req/s | p95 ms | p99 ms | err % | CPU % | µs/req | RAM MB | Scored |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1 | rust-epoll-scooter1337 | #82 | Rust (io_uring) | **264,000** ˣ | 25,147 | 259 | 499 | 0.00 | 84 | 30 | 630 | 2026-10-21 |
| 2 | cpp-eastl-io_uring-pgo-hendy643 | #32 | C++ (io_uring, EASTL) | **219,000** | 21,312 | 104 | 221 | 0.00 | 68 | 29 | 568 | 2026-10-25 |
| 3 | asm-nasm-melroy89 | #86 | x86-64 asm (io_uring) | **171,000** | 16,777 | 87 | 142 | 0.00 | 78 | 43 | 358 | 2026-10-21 |
| 4 | go-epoll-MegaPanchamZ | #72 | Go (raw epoll) | **165,000** | 16,165 | 194 | 241 | 0.00 | 89 | 51 | 187 | 2026-10-21 |
| 5 | asm-epoll-umitaltintas | #117 | x86-64 asm (epoll) | **155,000** | 15,182 | 208 | 294 | 0.00 | 89 | 55 | 211 | 2026-10-21 |
| 6 | c-iouring-ssmurfgg04-gif | #112 | C (io_uring) | **150,000** | 14,611 | 385 | 524 | 0.00 | 78 | 50 | 413 | 2026-10-21 |
| 7 | go-epoll-champ3oy | #76 | Go (io_uring) | **128,000** | 12,604 | 3 | 23 | 0.00 | 77 | 57 | 385 | 2026-10-21 |
| 8 | kotlin-nio-3x3xX3N0N | #83 | Kotlin (NIO, 2 JVMs) | **120,000** | 11,759 | 246 | 524 | 0.00 | 100 | 79 | 1,380 | 2026-10-21 |
| 9 | c-epoll-TheFirstIstari | #64 | C (epoll) | **107,000** | 10,493 | 212 | 409 | 0.94 | 78 | 69 | 169 | 2026-10-21 |
| 10 | java-nio-stohirov | #88 | Java (FFM epoll) | **102,000** | 10,023 | 86 | 128 | 0.85 | 66 | 61 | 555 | 2026-10-21 |
| 11 | v-epoll-i098 | #113 | V | **100,000** | 9,848 | 1.5 | 2.9 | 0.51 | 76 | 72 | 384 | 2026-10-21 |
| 12 | node-uwebsockets-zahidcakici | #102 | Node (uWebSockets) | **97,000** | 9,471 | 264 | 484 | 0.00 | 92 | 90 | 336 | 2026-10-21 |
| 13 | typescript-bun-xxmohamed012 | #3 | Bun | **90,000** | 8,826 | 140 | 359 | 0.00 | 83 | 87 | 301 | 2026-10-21 |
| 13 | scala-netty-jacksonhoran | #107 | Scala (Netty) | **90,000** | 8,801 | 484 | 757 | 0.00 | 87 | 92 | 825 | 2026-10-21 |
| 13 | python-pypy-epoll-pranavds-codes | #84 | PyPy (epoll) | **90,000** | 8,801 | 280 | 474 | 0.00 | 89 | 94 | 270 | 2026-10-21 |
| 16 | lua-openresty-yar0slav | #94 | Lua (OpenResty) | **88,000** | 8,477 | 434 | 591 | 0.04 | 92 | 101 | 267 | 2026-10-21 |
| 17 | zig-iouring-marler8997 ᵖ | #125 | Zig (io_uring) | **71,000** ᶠ | 6,559 | 1.0 | 1.5 | 0.93 | 31 | 44 | 199 | 2026-10-21 |
| 17 | typescript-deno-JakeAve | #119 | Deno | **71,000** ᶠ | 6,558 | 5.6 | 13 | 0.93 | 47 | 67 | 412 | 2026-10-21 |
| 17 | dart-rawsocket-C-Hatton | #46 | Dart | **71,000** ᶠ | 6,550 | 34 | 80 | 0.93 | 94 | 134 | 475 | 2026-10-21 |
| 17 | cpp-epoll-khalefa-ow | #8 | C++ (epoll) | **71,000** ᶠ | 6,558 | 19 | 37 | 0.93 | 94 | 134 | 184 | 2026-10-21 |
| 21 | fsharp-oxpecker-delneg | #108 | F# (Oxpecker) | **65,000** ᵍ | 6,392 | 84 | 229 | 0.00 | 55 | 80 | 1,255 | 2026-10-21 |
| 22 | csharp-epoll-gichamba ᵖ | #74 | C# (Native AOT epoll) | **64,000** ᵉ | 6,300 | 23 | 48 | 0.00 | 31 | 46 | 163 | 2026-10-21 |
| 23 | typescript-bun-tbsvttr | #4 | Bun | **57,000** | 5,593 | 103 | 291 | 0.00 | 86 | 143 | 301 | 2026-10-25 |
| 24 | haskell-warp-staticfx | #104 | Haskell (Warp) | **55,000** | 5,411 | 2.5 | 198 | 0.00 | 62 | 107 | 1,587 | 2026-10-21 |
| 24 | go-nethttp-fi5HiE | #99 | Go (net/http) | **55,000** | 5,382 | 425 | 679 | 0.00 | 90 | 156 | 1,465 | 2026-10-21 |
| 26 | python-fastapi-tejasnafde | #10 | Python (FastAPI) | **48,000** | 4,701 | 303 | 465 | 0.00 | 89 | 176 | 583 | 2026-10-21 |
| 26 | swift-hummingbird-denizak ᵖ | #43 | Swift (Hummingbird) | **48,000** | 4,700 | 210 | 300 | 0.00 | 97 | 191 | 1,269 | 2026-10-21 |
| 28 | java-quarkus-wolches | #67 | Java (Quarkus) | **29,000** ᵈ | 2,838 | 306 | 451 | 0.00 | 50 | 164 | 542 | 2026-10-21 |
| 29 | ruby-rails-gambitboy | #2 | Ruby (Rails), behind Nginx | **9,000** | 876 | 322 | 443 | 0.00 | 97 | 1,031 | 257 | 2026-10-25 |
| 30 | bash-socat-framegrace | #89 | Bash (socat, behind Nginx) | **3,500** | 344 | 18 | 43 | 0.00 | 64 | — | 107 | 2026-10-21 |
| — | nim-io_uring-NioHast | #132 | Nim (io_uring) | **failed** (hangs) | | | | | | | | 2026-10-21 |
| — | cpp-epoll_inmem-khalefa-ow | #9 | C++ (epoll) | waiting for a re-run on this box | | | | | | | | |
| — | cpp-uwebsockets-tbsvttr | #6 | C++ (uWebSockets) | waiting for a re-run on this box | | | | | | | | |
| — | cpp-uwebsockets-v2-tbsvttr | #15 | C++ (uWebSockets) | waiting for a re-run on this box | | | | | | | | |
| — | zig-usockets-tbsvttr | #25 | Zig (uSockets) | waiting for a re-run on this box | | | | | | | | |
| — | c-iouring-raoz | #26 | C (io_uring) | waiting for a re-run on this box | | | | | | | | |
| — | rust-axum-tbsvttr | #17 | Rust (Axum) | waiting for a re-run on this box | | | | | | | | |
| — | cpp-drogon-hendy643 | #29 | C++ (Drogon) | waiting for a re-run on this box | | | | | | | | |
| — | php-swoole-mattsplat | #24 | PHP (Swoole) | waiting for a re-run on this box | | | | | | | | |
| — | php-swoole-tzimpel | #28 | PHP (Swoole) | waiting for a re-run on this box | | | | | | | | |
| — | fortran-microhttpd-tbsvttr | #16 | Fortran (libmicrohttpd) | waiting for a re-run on this box | | | | | | | | |
| — | c-fio-tbsvttr | #5 | C (facil.io) | waiting for a re-run on this box | | | | | | | | |
| — | csharp-aspnetcore-maks-march | #27 | C# (ASP.NET Core) | waiting for a re-run on this box | | | | | | | | |
| — | csharp-aspnetcore-andrexx | #20 | C# (ASP.NET Core) | waiting for a re-run on this box | | | | | | | | |
| — | go-nethttp-tbsvttr | #18 | Go (net/http) | waiting for a re-run on this box | | | | | | | | |
| — | python-granian-tbsvttr | #13 | Python (Granian) | waiting for a re-run on this box | | | | | | | | |
| — | erlang-cowboy-tbsvttr | #11 | Erlang (Cowboy) | waiting for a re-run on this box | | | | | | | | |
| — | ruby-iodine-tbsvttr | #14 | Ruby (iodine) | waiting for a re-run on this box | | | | | | | | |
| — | python-litestar-przwr | #23 | Python (Litestar) | waiting for a re-run on this box | | | | | | | | |
| — | ruby-rails-tbsvttr | #12 | Ruby (Rails) | waiting for a re-run on this box | | | | | | | | |
ˣ **Rust #82 ran out of load generators first.** One target IP × 4 load generators caps at ~240,000 connections (source
ports). It passed 240,000 at 79 % CPU, so it was re-searched with 3 extra IPv4s on the box and a 5th load generator
(cap ~1.2M): 300,000 failed on latency (p99 888 ms 2-min, CPU 89 %), 293,000/278,000 failed the 5-minute hold on p99,
**264,000 passed**. 360k/480k probes got the app OOM-killed (2 GB box). The 240,000 run on the original setup is consistent
(23,166 req/s, 31 µs/req).

ᶠ **The 65,535 file-descriptor limit, not speed.** These four serve 65,535 keep-alive users and the rest wait in the
listen queue until mload's 60 s timeout. Each scored run shows exactly ~21,900 timeouts = 0.93 % of requests, just under the
1 % rule — so 71,000 is "full fd table + 1 % slack", and the same number for a C++ server at 94 % CPU and a Zig one at 31 %.
Zig (1.5 ms p99) and Deno (13 ms p99, 47 % CPU) would go much further without the rule-mandated limit. Worth considering a
rules note: the < 1 % error budget lets an fd-capped server "hold" ~8 % more users than it can actually connect.

Entries above 71,000 get there by closing idle keep-alive connections (`Connection: close` or RST near the limit) so the
client reconnects — c-epoll, Java NIO and V show this as EOF/reset errors under 1 %; the io_uring entries manage up to
~524k fixed-file slots/rings.

ᵈ **Disk-bound, not CPU-bound.** Quarkus never sets `synchronous`, so SQLite defaults to FULL: an fsync per commit, one
commit per request — 26–39 % iowait during its probes (37 % over the scored hold).

ᵍ **F# crashes past ~65k connections.** At 68,000 and 70,000 users the .NET runtime aborts (`PAL_SEHException`, core dump)
about a minute into the ramp, as connections pass the 65,535 fd limit, and every later connection is refused (86 % errors).
Not disk-bound: iowait was 1–6 %; it sets `wal_autocheckpoint = 0` and checkpoints on its own thread (Db.fs:123).
*(Corrected 2026-10-22: an earlier version of this report misread the monitor columns — it reported idle time as iowait —
and blamed F#'s limit on auto-checkpoint I/O.)*

ᵉ **C# crashes at the fd limit.** When `accept` returns EMFILE, its error path writes to `Console.Error`, whose first use
opens a new stderr handle — which also fails with "Too many open files", unhandled, and the process aborts (core dump).
Every probe above 64,000 (66k–120k) died this way.

## Patches (ᵖ) and failures

Each patch is one line, doesn't touch runtime code, and was verified to build on the same Xeon first. All of them are
committed in this repo.

- **Zig #125** — build failed: `evaluation exceeded 10000 backwards branches` in `src/UringIo.zig` `clobbers()`. On an
  AVX-512 CPU it loops over 32 zmm registers at compile time (16 on AVX2, likely the author's machine). Patch:
  `@setEvalBranchQuota(10_000)` → `100_000`.
- **C# #74** — build failed: Native AOT `ilc` exits with `error : Vector256` when `IlcInstructionSet=native` on this
  AVX-512 Cascade Lake. Patch: `native` → `x86-64-v3` (AVX2) in Feed.csproj.
- **Swift #43** — install failed: the toolchain URL is missing the `swift-6.2.1-RELEASE/` folder → HTTP 404 (it can't have
  worked on a clean box). Patch: the corrected URL.
- **Nim #132 — failed, twice.** First run: passed /health, then wedged during the test suite — 0 s CPU, blocked in
  `io_uring_enter` (io_cqring_wait), 192 connections stuck in CLOSE-WAIT: its multishot accept stopped delivering. Retry
  (fresh build; its PGO build fell back to a plain build): stuck at the suite's first read. Kernel 6.8.0-117.
- **D #123** (vibe.d) — dropped before the run: fails test.sh (HTML default 404 body; a bad `sub` crashes with 500).

## Rule flags from code review (nothing malicious found; please fix in a follow-up PR)

- **Tokens without `exp` accepted** (SPEC requires rejecting them): asm-epoll #117, java-nio #88, quarkus #67 (SmallRye
  grace), kotlin #83, scala #107, node-uws #102, dart #46, go-champ3oy #76, pypy #84 (deliberately), csharp #74, bash #89,
  cpp-eastl #32 (held open until fixed).
- **Past ~327k connections, cpp-eastl #32 evicts the connection it just served** because "the think-time loop visits
  users in the same order every time" (its README) — tuned to the load test's order (rule 7 question). Its score
  (219k) is below that point, so it doesn't affect the number.
- **Reads can see uncommitted writes** in the same batch: Deno #119 (also serves a snapshot up to 20 ms stale), Zig #125.
- **start.sh restart loops** (the app restarts itself instead of letting systemd see a crash): Deno #119, c-epoll #64.
- **Unchecksummed toolchain downloads in install/build**: Deno (curl | sh), Go #72, Haskell (ghcup), Swift, .NET SDK,
  rustup (#82, also writes /usr/local/bin wrappers), Zig (anyzig).
- Bash #89 builds SQL by string concatenation (quotes doubled) — works, fragile.
- c-iouring #112's README describes a different design than the code (THREADSAFE=2, per-write commit); it sheds load
  based on /proc/meminfo.

## Crashes during probes that failed anyway

The app journal showed OOM kills only in probes that failed anyway: Rust #82 at 360k/480k, asm-nasm #86 at 200k/240k,
go-nethttp #99 at 57k, Haskell #104 at 58k. Scored holds were all clean.
