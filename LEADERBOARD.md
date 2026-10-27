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

**Re-runs.** The first submissions (#2–#29) were scored on a DigitalOcean $12 droplet, which is about 2× slower for
CPU-bound servers, so those numbers aren't comparable. All of them have now been re-run on this box (finished
2026-10-27). The old numbers are in [`benchmarks/batch-1.md`](benchmarks/batch-1.md).

## Results

CPU % and µs/request are the app's own cgroup over the scored hold (µs = app CPU time ÷ requests). RAM = app cgroup peak.

| # | Submission | PR | Language | Users held 5 min | req/s | p95 ms | p99 ms | err % | CPU % | µs/req | RAM MB | Scored |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1 | rust-epoll-scooter1337 | #82 | Rust (io_uring) | **264,000** ˣ | 25,147 | 259 | 499 | 0.00 | 84 | 30 | 630 | 2026-10-21 |
| 2 | cpp-eastl-io_uring-pgo-hendy643 | #32 | C++ (io_uring, EASTL) | **210,000** | 20,436 | 322 | 672 | 0.00 | 73 | 33 | 633 | 2026-10-26 |
| 3 | c-iouring-raoz | #26 | C (io_uring) | **172,000** | 16,875 | 98 | 125 | 0.00 | 69 | 38 | 419 | 2026-10-27 |
| 4 | asm-nasm-melroy89 | #86 | x86-64 asm (io_uring) | **171,000** | 16,777 | 87 | 142 | 0.00 | 78 | 43 | 358 | 2026-10-21 |
| 5 | go-epoll-MegaPanchamZ | #72 | Go (raw epoll) | **165,000** | 16,165 | 194 | 241 | 0.00 | 89 | 51 | 187 | 2026-10-21 |
| 6 | asm-epoll-umitaltintas | #117 | x86-64 asm (epoll) | **155,000** | 15,182 | 208 | 294 | 0.00 | 89 | 55 | 211 | 2026-10-21 |
| 7 | c-iouring-ssmurfgg04-gif | #112 | C (io_uring) | **150,000** | 14,611 | 385 | 524 | 0.00 | 78 | 50 | 413 | 2026-10-21 |
| 8 | c-ammarserver-AmmarkoV | #54 | C (AmmarServer, io_uring) | **135,000** | 13,239 | 95 | 470 | 0.00 | 76 | 53 | 320 | 2026-10-27 |
| 9 | go-epoll-champ3oy | #76 | Go (io_uring) | **128,000** | 12,604 | 3 | 23 | 0.00 | 77 | 57 | 385 | 2026-10-21 |
| 10 | cpp-uwebsockets-v2-tbsvttr | #15 | C++ (uWebSockets) | **125,000** | 12,188 | 204 | 306 | 0.00 | 89 | 68 | 182 | 2026-10-26 |
| 10 | go-epoll-Antheurus | #65 | Go (raw epoll, 3 processes) | **125,000** | 12,274 | 116 | 665 | 0.00 | 98 | 74 | 292 | 2026-10-27 |
| 12 | kotlin-nio-3x3xX3N0N | #83 | Kotlin (NIO, 2 JVMs) | **120,000** | 11,759 | 246 | 524 | 0.00 | 100 | 79 | 1,380 | 2026-10-21 |
| 13 | cpp-uwebsockets-tbsvttr | #6 | C++ (uWebSockets) | **115,000** | 10,940 | 261 | 652 | 0.00 | 87 | 74 | 215 | 2026-10-26 |
| 14 | zig-usockets-tbsvttr | #25 | Zig (uSockets) | **110,000** | 10,442 | 335 | 721 | 0.00 | 90 | 80 | 188 | 2026-10-26 |
| 15 | c-epoll-TheFirstIstari | #64 | C (epoll) | **107,000** | 10,493 | 212 | 409 | 0.94 | 78 | 69 | 169 | 2026-10-21 |
| 16 | java-nio-stohirov | #88 | Java (FFM epoll) | **102,000** | 10,023 | 86 | 128 | 0.85 | 66 | 61 | 555 | 2026-10-21 |
| 17 | v-epoll-i098 | #113 | V | **100,000** | 9,848 | 1.5 | 2.9 | 0.51 | 76 | 72 | 384 | 2026-10-21 |
| 18 | node-uwebsockets-zahidcakici | #102 | Node (uWebSockets) | **97,000** | 9,471 | 264 | 484 | 0.00 | 92 | 90 | 336 | 2026-10-21 |
| 19 | typescript-bun-xxmohamed012 | #3 | Bun | **90,000** | 8,826 | 140 | 359 | 0.00 | 83 | 87 | 301 | 2026-10-21 |
| 19 | scala-netty-jacksonhoran | #107 | Scala (Netty) | **90,000** | 8,801 | 484 | 757 | 0.00 | 87 | 92 | 825 | 2026-10-21 |
| 19 | python-pypy-epoll-pranavds-codes | #84 | PyPy (epoll) | **90,000** | 8,801 | 280 | 474 | 0.00 | 89 | 94 | 270 | 2026-10-21 |
| 22 | lua-openresty-yar0slav | #94 | Lua (OpenResty) | **88,000** | 8,477 | 434 | 591 | 0.04 | 92 | 101 | 267 | 2026-10-21 |
| 22 | asm-epoll-FlavouredTux | #75 | x86-64 asm (epoll) | **88,000** | 8,628 | 170 | 438 | 0.00 | 85 | 92 | 189 | 2026-10-27 |
| 24 | rust-axum-tbsvttr | #17 | Rust (Axum) | **76,000** | 7,434 | 280 | 551 | 0.00 | 85 | 106 | 1,118 | 2026-10-26 |
| 25 | zig-iouring-marler8997 ᵖ | #125 | Zig (io_uring) | **71,000** ᶠ | 6,559 | 1.0 | 1.5 | 0.93 | 31 | 44 | 199 | 2026-10-21 |
| 25 | typescript-deno-JakeAve | #119 | Deno | **71,000** ᶠ | 6,558 | 5.6 | 13 | 0.93 | 47 | 67 | 412 | 2026-10-21 |
| 25 | dart-rawsocket-C-Hatton | #46 | Dart | **71,000** ᶠ | 6,550 | 34 | 80 | 0.93 | 94 | 134 | 475 | 2026-10-21 |
| 25 | cpp-epoll-khalefa-ow | #8 | C++ (epoll) | **71,000** ᶠ | 6,558 | 19 | 37 | 0.93 | 94 | 134 | 184 | 2026-10-21 |
| 25 | go-fib-sirkostya009 | #34 | Go (fib, epoll) | **71,000** ᶠ | 6,552 | 109 | 248 | 0.93 | 61 | 87 | 674 | 2026-10-26 |
| 25 | cpp-epoll_inmem-khalefa-ow | #9 | C++ (epoll) | **71,000** ᶠ | 6,555 | 30 | 148 | 0.93 | 92 | 131 | 353 | 2026-10-26 |
| 25 | c-h2o-einhornimmond | #39 | C (H2O) | **71,000** ᶠ | 6,558 | 0.8 | 4.9 | 0.93 | 39 | 56 | 331 | 2026-10-26 |
| 25 | c-epoll-benclawbot | #42 | C (epoll) | **71,000** ᶠ | 6,555 | 29 | 53 | 0.93 | 94 | 134 | 409 | 2026-10-26 |
| 25 | go-stdlib-vkrishna03 | #44 | Go (stdlib, epoll) | **71,000** ᶠ | 6,547 | 74 | 251 | 0.93 | 38 | 54 | 231 | 2026-10-26 |
| 25 | php-swoole-mattsplat | #24 | PHP (Swoole) | **71,000** ᶠ | 6,552 | 45 | 102 | 0.93 | 66 | 94 | 1,025 | 2026-10-26 |
| 25 | go-epoll-damianoneill | #52 | Go (raw epoll) | **71,000** ᶠ | 6,551 | 50 | 78 | 0.93 | 41 | 59 | 157 | 2026-10-26 |
| 25 | c-iouring-einhornimmond | #55 | C (io_uring) | **71,000** ᶠ | 6,556 | 2.7 | 10 | 0.92 | 36 | 51 | 190 | 2026-10-27 |
| 25 | php-swoole-tzimpel | #28 | PHP (Swoole) | **71,000** ᶠ | 6,552 | 17 | 175 | 0.93 | 71 | 101 | 325 | 2026-10-27 |
| 25 | c-iouring-27bhd | #68 | C (io_uring) | **71,000** ᶠ | 6,554 | 30 | 44 | 0.93 | 84 | 120 | 186 | 2026-10-27 |
| 25 | c-fio-tbsvttr | #5 | C (facil.io) | **71,000** | 6,959 | 130 | 621 | 0.00 | 81 | 108 | 646 | 2026-10-26 |
| 25 | ruby-async-cicloid | #71 | Ruby (async, YJIT) | **71,000** | 6,967 | 121 | 349 | 0.00 | 83 | 111 | 362 | 2026-10-27 |
| 41 | python-fastapi-granian-shawal-mbalire | #41 | Python (FastAPI, Granian) | **70,000** | 6,801 | 465 | 862 | 0.00 | 94 | 129 | 1,192 | 2026-10-26 |
| 41 | fortran-microhttpd-tbsvttr | #16 | Fortran (libmicrohttpd) | **70,000** | 6,866 | 141 | 401 | 0.00 | 89 | 120 | 1,134 | 2026-10-26 |
| 43 | fsharp-oxpecker-delneg | #108 | F# (Oxpecker) | **65,000** ᵍ | 6,392 | 84 | 229 | 0.00 | 55 | 80 | 1,255 | 2026-10-21 |
| 43 | go-gnet-Haris0059 | #38 | Go (gnet) | **65,000** ⁿ | 6,396 | 39 | 54 | 0.00 | 42 | 61 | 350 | 2026-10-26 |
| 43 | kotlin-ktor-C-Hatton | #45 | Kotlin (Ktor, Netty, FFM SQLite) | **65,000** | 6,374 | 198 | 713 | 0.00 | 65 | 95 | 1,273 | 2026-10-26 |
| 43 | go-gnet-kaikegm | #48 | Go (gnet) | **65,000** ⁿ | 6,394 | 88 | 251 | 0.00 | 42 | 61 | 323 | 2026-10-26 |
| 43 | go-fasthttp-b3nhard | #56 | Go (fasthttp, cgo SQLite) | **65,000** ⁿ | 6,399 | 4.8 | 19 | 0.00 | 77 | 112 | 547 | 2026-10-27 |
| 43 | cpp-drogon-hendy643 | #29 | C++ (Drogon) | **65,000** | 6,398 | 36 | 60 | 0.00 | 52 | 76 | 814 | 2026-10-27 |
| 43 | java-netty-lfeagan | #63 | Java (Netty, FFM SQLite) | **65,000** | 6,401 | 1.9 | 28 | 0.00 | 62 | 90 | 1,494 | 2026-10-27 |
| 43 | csharp-aspnetcore-vijay-highlo | #77 | C# (ASP.NET Core, Native AOT) | **65,000** | 6,389 | 87 | 319 | 0.00 | 56 | 82 | 1,278 | 2026-10-27 |
| 43 | node-net-barrard | #78 | Node (node:net, node:sqlite) | **65,000** | 6,387 | 61 | 236 | 0.00 | 76 | 111 | 433 | 2026-10-27 |
| 52 | csharp-epoll-gichamba ᵖ | #74 | C# (Native AOT epoll) | **64,000** ᵉ | 6,300 | 23 | 48 | 0.00 | 31 | 46 | 163 | 2026-10-21 |
| 52 | bun-elysia-shashishsoni | #59 | Bun (Elysia) | **64,000** | 6,300 | 14 | 75 | 0.00 | 89 | 131 | 1,507 | 2026-10-27 |
| 54 | csharp-aspnetcore-maks-march | #27 | C# (ASP.NET Core) | **63,000** | 6,136 | 417 | 757 | 0.00 | 87 | 131 | 925 | 2026-10-27 |
| 55 | pypy-twisted-perkinslr | #79 | PyPy (Twisted) | **62,000** | 6,017 | 474 | 757 | 0.00 | 73 | 113 | 563 | 2026-10-27 |
| 56 | java-netty-itsnaveenk | #57 | Java (Netty) | **58,000** | 5,665 | 349 | 519 | 0.00 | 83 | 136 | 879 | 2026-10-27 |
| 57 | typescript-bun-tbsvttr | #4 | Bun | **57,000** | 5,593 | 103 | 291 | 0.00 | 86 | 143 | 301 | 2026-10-25 |
| 57 | java-quarkus-panama-raisinsfried | #73 | Java (Quarkus, FFM SQLite) | **57,000** | 5,607 | 9.6 | 359 | 0.00 | 60 | 100 | 725 | 2026-10-27 |
| 59 | haskell-warp-staticfx | #104 | Haskell (Warp) | **55,000** | 5,411 | 2.5 | 198 | 0.00 | 62 | 107 | 1,587 | 2026-10-21 |
| 59 | go-nethttp-fi5HiE | #99 | Go (net/http) | **55,000** | 5,382 | 425 | 679 | 0.00 | 90 | 156 | 1,465 | 2026-10-21 |
| 61 | go-nethttp-tbsvttr | #18 | Go (net/http) | **53,000** | 5,165 | 374 | 659 | 0.00 | 93 | 168 | 624 | 2026-10-26 |
| 62 | typescript-fastify-melroy89 | #51 | TypeScript (Fastify, Node) | **52,000** | 5,089 | 359 | 862 | 0.00 | 81 | 148 | 1,007 | 2026-10-26 |
| 63 | csharp-aspnetcore-andrexx | #20 | C# (ASP.NET Core) | **49,000** | 4,815 | 84 | 234 | 0.00 | 88 | 170 | 846 | 2026-10-26 |
| 64 | python-fastapi-tejasnafde | #10 | Python (FastAPI) | **48,000** | 4,701 | 303 | 465 | 0.00 | 89 | 176 | 583 | 2026-10-21 |
| 64 | swift-hummingbird-denizak ᵖ | #43 | Swift (Hummingbird) | **48,000** | 4,700 | 210 | 300 | 0.00 | 97 | 191 | 1,269 | 2026-10-21 |
| 66 | python-granian-tbsvttr | #13 | Python (Granian) | **45,000** | 4,400 | 202 | 812 | 0.00 | 93 | 197 | 709 | 2026-10-26 |
| 67 | node-fastify-gmr458 | #61 | TypeScript (Fastify, Node) | **44,000** | 4,285 | 489 | 915 | 0.00 | 86 | 187 | 812 | 2026-10-27 |
| 68 | ruby-iodine-tbsvttr | #14 | Ruby (iodine) | **43,000** | 4,223 | 89 | 175 | 0.00 | 86 | 189 | 1,096 | 2026-10-26 |
| 69 | java-spring-boot-nb-99 | #33 | Java (Spring Boot WebFlux) | **32,000** | 3,151 | 9.9 | 27 | 0.00 | 81 | 239 | 763 | 2026-10-26 |
| 70 | java-quarkus-wolches | #67 | Java (Quarkus) | **29,000** ᵈ | 2,838 | 306 | 451 | 0.00 | 50 | 164 | 542 | 2026-10-21 |
| 71 | erlang-cowboy-tbsvttr | #11 | Erlang (Cowboy) | **28,000** | 2,751 | 76 | 174 | 0.00 | 85 | 287 | 1,214 | 2026-10-26 |
| 72 | typescript-effect-SeeThruHead | #49 | TypeScript (Effect, Node) | **23,000** | 2,229 | 413 | 765 | 0.00 | 96 | 399 | 623 | 2026-10-26 |
| 73 | php-symfony-pklebba | #40 | PHP (Symfony, FrankenPHP) | **21,000** | 2,061 | 186 | 274 | 0.00 | 95 | 428 | 915 | 2026-10-26 |
| 74 | csharp-aspnetcore-larsholm | #47 | C# (ASP.NET Core) | **19,000** | 1,854 | 381 | 943 | 0.00 | 57 | 286 | 439 | 2026-10-26 |
| 75 | kotlin-springboot-Mindstorm1750 | #66 | Kotlin (Spring Boot, Tomcat, virtual threads) | **18,000** | 1,760 | 397 | 870 | 0.00 | 61 | 323 | 792 | 2026-10-27 |
| 76 | python-litestar-przwr | #23 | Python (Litestar) | **16,000** | 1,569 | 259 | 788 | 0.00 | 66 | 391 | 256 | 2026-10-26 |
| 77 | kotlin-ktor-jvm-syorito-hatsuki | #36 | Kotlin (Ktor, JVM) | **14,000** | 1,370 | 370 | 728 | 0.00 | 79 | 536 | 603 | 2026-10-26 |
| 78 | kotlin-ktor-graalvm-noamm9 | #53 | Kotlin (Ktor, GraalVM native) | **10,500** | 1,027 | 356 | 906 | 0.00 | 79 | 715 | 314 | 2026-10-26 |
| 79 | ruby-rails-gambitboy | #2 | Ruby (Rails), behind Nginx | **9,000** | 876 | 322 | 443 | 0.00 | 97 | 1,031 | 257 | 2026-10-25 |
| 79 | ruby-rails-tbsvttr | #12 | Ruby (Rails) | **9,000** | 875 | 409 | 562 | 0.00 | 98 | 1,043 | 305 | 2026-10-26 |
| 81 | bash-socat-framegrace | #89 | Bash (socat, behind Nginx) | **3,500** | 344 | 18 | 43 | 0.00 | 64 | — | 107 | 2026-10-21 |
| — | nim-io_uring-NioHast | #132 | Nim (io_uring) | **failed** (hangs) | | | | | | | | 2026-10-21 |
| — | v-fasthttp-ulises-jeremias | #69 | V (fasthttp) | **failed** (install OOM) | | | | | | | | 2026-10-27 |

ˣ **Rust #82 ran out of load generators first.** One target IP × 4 load generators caps at ~240,000 connections (source
ports). It passed 240,000 at 79 % CPU, so it was re-searched with 3 extra IPv4s on the box and a 5th load generator
(cap ~1.2M): 300,000 failed on latency (p99 888 ms 2-min, CPU 89 %), 293,000/278,000 failed the 5-minute hold on p99,
**264,000 passed**. 360k/480k probes got the app OOM-killed (2 GB box). The 240,000 run on the original setup is consistent
(23,166 req/s, 31 µs/req).

ᶠ **The 65,535 file-descriptor limit, not speed.** These fourteen serve 65,535 keep-alive users and the rest wait in the
listen queue until mload's 60 s timeout. Each scored run shows exactly ~21,900 timeouts = 0.93 % of requests, just under the
1 % rule — so 71,000 is "full fd table + 1 % slack", and the same number for a C++ server at 94 % CPU and a Zig one at 31 %
(Go fib #34: 61 % CPU, flat from 70k to 78k users while timeouts grow; C H2O #39: 39 % CPU, 4.9 ms p99, and its README
says it doesn't shed connections at the limit; C epoll #42: 39 % CPU at 60k, 93 % from 70k as it accepts and closes
the connections past the limit; Go stdlib #44: 38 % CPU, flat from 70k to 78k users, and its README says accept
pauses at the limit; PHP Swoole #24: 66 % CPU, flat from 75k to 78k users while timeouts grow; Go epoll #52: 41 % CPU, 78 ms p99, it
stops accepting at the limit and never closes idle keep-alives; C io_uring #55: 36 % CPU, 10 ms p99, request latency
flat from 20k to 78k users while connect timeouts grow; PHP Swoole #28: 71 % CPU, flat from 75k to 78k users while
timeouts grow; C io_uring #68: 84 % CPU from 70k, 44 ms p99, request latency flat from 70k to 80k users while connect
timeouts grow).
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

ⁿ **Go gnet #38 and #48 exit at the fd limit.** When `accept` fails with "too many open files", gnet v2.10.0 shuts the
engine down and `gnet.Run` returns, so the process exits (journal, 80k probe) and every later connection is refused
(86–90 % errors at 68k/70k/80k, 6–10 % CPU). At 65,000 both were clean at 42 % CPU. Go fasthttp #56 does the same
(`accept4: too many open files`, exit status 1, at 68k/70k/80k); at 65,000 it was clean at 77 % CPU, 19 ms p99.

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
- **V #69 — install failed** (`913da3f`): `make v` bootstraps the V compiler from source, and its `v1` stage reached
  1.78 GB RSS and was OOM-killed on the 2 GB box (kernel log, exit 137), so `v` was never installed and the app
  couldn't build. CI passed on GitHub's bigger runner. Not patched (it isn't a one-line fix).
- **D #123** (vibe.d) — dropped before the run: fails test.sh (HTML default 404 body; a bad `sub` crashes with 500).

## Rule flags from code review (nothing malicious found; please fix in a follow-up PR)

- **Tokens without `exp` accepted** (SPEC requires rejecting them): asm-epoll #117, java-nio #88, quarkus #67 (SmallRye
  grace), kotlin #83, scala #107, node-uws #102, dart #46, go-champ3oy #76, pypy #84 (deliberately), csharp #74, bash #89,
  go-gnet #38, kotlin-ktor #36, python-fastapi-granian #41, go-stdlib #44, kotlin-ktor-C-Hatton #45, typescript-fastify #51, go-epoll-damianoneill #52,
  kotlin-ktor-graalvm #53, go-fasthttp #56, java-netty #57, bun-elysia #59, node-fastify #61, java-netty-lfeagan #63, c-iouring-27bhd #68, node-net #78, pypy-twisted #79 (all held open until fixed). cpp-eastl #32 fixed it in `4d5e225` (re-scored and merged).
- **Kotlin Ktor #36 (and #53, based on it) commit `gradle/wrapper/gradle-wrapper.jar`**. Arjay ruled (2026-10-26) that
  the standard Gradle wrapper jar is fine and not a prebuilt binary under rule 10. Kotlin Ktor #36 also returned a few HTTP 500s on writes under load (1–5 per probe,
  0 in the scored hold); its StatusPages handler doesn't log the exception. kotlin-springboot #66 commits the same
  jar and was merged on that ruling.
- **Past ~327k connections, cpp-eastl #32 evicts the connection it just served** because "the think-time loop visits
  users in the same order every time" (its README) — tuned to the load test's order (rule 7 question). Its score
  (210k) is below that point, so it doesn't affect the number.
- **Reads can see uncommitted writes** in the same batch: Deno #119 (also serves a snapshot up to 20 ms stale), Zig #125.
- **start.sh restart loops** (the app restarts itself instead of letting systemd see a crash): Deno #119, c-epoll #64,
  c-iouring-raoz #26 (restarts after a signal exit), go-epoll-Antheurus #65 (worker 0 re-spawns dead workers).
- **Unchecksummed toolchain downloads in install/build**: Deno (curl | sh), Go #72, Haskell (ghcup), Swift, .NET SDK,
  rustup (#82, also writes /usr/local/bin wrappers), Zig (anyzig).
- Bash #89 builds SQL by string concatenation (quotes doubled) — works, fragile.
- c-iouring #112's README describes a different design than the code (THREADSAFE=2, per-write commit); it sheds load
  based on /proc/meminfo (c-iouring-raoz #26 also reads /proc/meminfo, server.c:1314). python-fastapi-granian #41 likewise reads the box-wide `/proc/self/net/sockstat` to send
  `Connection: close`; the load test reconnected ~4.5× per user even at 40k users (180k dials), with 0 errors.

## Crashes during probes that failed anyway

The app journal showed OOM kills only in probes that failed anyway: Rust #82 at 360k/480k, asm-nasm #86 at 200k/240k,
go-nethttp #99 at 57k, Haskell #104 at 58k, php-symfony #40 at 40k (FrankenPHP at 1.5 GB), rust-axum #17 at 160k/120k, go-nethttp-tbsvttr #18 at 80k, csharp-aspnetcore-andrexx #20 at 80k (.NET "Out of memory." abort), csharp-aspnetcore-maks-march #27 at 80k/70k (.NET `PAL_SEHException` abort past the fd limit, like F# ᵍ), cpp-eastl #32 at 320k, c-iouring-raoz #26 at
320k/240k/200k and in the 190k/181k 5-minute holds. Scored holds were all clean.
