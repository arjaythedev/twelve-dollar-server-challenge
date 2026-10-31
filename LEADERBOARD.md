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

**Commits scored.** Each PR's head at the time of its run. zig-iouring-marler8997 #125 was updated afterwards, and
its merged code is newer than what was scored (`a08923d`). go-epoll-MegaPanchamZ is scored at its follow-up PR #144
(`e66a931`, not merged yet); its earlier #72 run (`9df7449`) held 165,000.

**Re-runs.** The first submissions (#2–#29) were scored on a DigitalOcean $12 droplet, which is about 2× slower for
CPU-bound servers, so those numbers aren't comparable. All of them have now been re-run on this box (finished
2026-10-27). The old numbers are in [`benchmarks/batch-1.md`](benchmarks/batch-1.md).

## Results

CPU % and µs/request are the app's own cgroup over the scored hold (µs = app CPU time ÷ requests). RAM = app cgroup peak.

| # | Submission | PR | Language | Users held 5 min | req/s | p95 ms | p99 ms | err % | CPU % | µs/req | RAM MB | Scored |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1 | rust-epoll-scooter1337 | #82 | Rust (io_uring) | **264,000** ˣ | 25,147 | 259 | 499 | 0.00 | 84 | 30 | 630 | 2026-10-21 |
| 2 | fsharp-kestrel-delneg | #142 | F# (Kestrel, io_uring) | **260,000** | 24,999 | 32 | 322 | 0.00 | 78 | 28 | 761 | 2026-10-28 |
| 3 | asm-epoll-umitaltintas | #152 | x86-64 asm (io_uring) | **240,000** | 22,985 | 366 | 633 | 0.00 | 83 | 33 | 545 | 2026-10-28 |
| 4 | c-iouring-meros | #137 | C (io_uring) | **235,000** | 22,602 | 294 | 397 | 0.00 | 80 | 32 | 1,537 | 2026-10-28 |
| 5 | cpp-eastl-io_uring-pgo-hendy643 | #32 | C++ (io_uring, EASTL) | **210,000** | 20,436 | 322 | 672 | 0.00 | 73 | 33 | 633 | 2026-10-26 |
| 6 | go-epoll-MegaPanchamZ | #144 | Go (raw epoll) | **185,000** | 18,086 | 288 | 479 | 0.00 | 99 | 51 | 1,286 | 2026-10-28 |
| 7 | c-iouring-raoz | #26 | C (io_uring) | **172,000** | 16,875 | 98 | 125 | 0.00 | 69 | 38 | 419 | 2026-10-27 |
| 8 | asm-nasm-melroy89 | #86 | x86-64 asm (io_uring) | **171,000** | 16,777 | 87 | 142 | 0.00 | 78 | 43 | 358 | 2026-10-21 |
| 9 | c-iouring-ssmurfgg04-gif | #112 | C (io_uring) | **150,000** | 14,611 | 385 | 524 | 0.00 | 78 | 50 | 413 | 2026-10-21 |
| 10 | go-epoll-kaikegm | #151 | Go (raw epoll, cgo SQLite, PGO) | **145,000** | 14,088 | 29 | 116 | 0.21 | 96 | 64 | 909 | 2026-10-28 |
| 11 | c-ammarserver-AmmarkoV | #54 | C (AmmarServer, io_uring) | **135,000** | 13,239 | 95 | 470 | 0.00 | 76 | 53 | 320 | 2026-10-27 |
| 11 | go-epoll-brady1408 | #111 | Go (raw epoll, cgo SQLite) | **135,000** | 13,169 | 366 | 962 | 0.00 | 100 | 70 | 296 | 2026-10-27 |
| 13 | odin-odinhttp-wanderingaura | #139 | Odin (odin-http) | **130,000** | 12,644 | 429 | 924 | 0.00 | 82 | 60 | 624 | 2026-10-28 |
| 14 | go-epoll-champ3oy | #76 | Go (io_uring) | **128,000** | 12,604 | 3 | 23 | 0.00 | 77 | 57 | 385 | 2026-10-21 |
| 15 | cpp-uwebsockets-v2-tbsvttr | #15 | C++ (uWebSockets) | **125,000** | 12,188 | 204 | 306 | 0.00 | 89 | 68 | 182 | 2026-10-26 |
| 15 | go-epoll-Antheurus | #65 | Go (raw epoll, 3 processes) | **125,000** | 12,274 | 116 | 665 | 0.00 | 98 | 74 | 292 | 2026-10-27 |
| 15 | go-lowlevel-janso123 | #91 | Go (io_uring, PGO + ThinLTO) | **125,000** | 12,284 | 68 | 261 | 0.00 | 95 | 72 | 500 | 2026-10-27 |
| 18 | kotlin-nio-3x3xX3N0N | #83 | Kotlin (NIO, 2 JVMs) | **120,000** | 11,759 | 246 | 524 | 0.00 | 100 | 79 | 1,380 | 2026-10-21 |
| 18 | asm-epoll-HauLTCE | #130 | x86-64 asm (epoll) | **120,000** | 11,747 | 144 | 627 | 0.00 | 88 | 70 | 221 | 2026-10-28 |
| 20 | cpp-uwebsockets-tbsvttr | #6 | C++ (uWebSockets) | **115,000** | 10,940 | 261 | 652 | 0.00 | 87 | 74 | 215 | 2026-10-26 |
| 21 | zig-usockets-tbsvttr | #25 | Zig (uSockets) | **110,000** | 10,442 | 335 | 721 | 0.00 | 90 | 80 | 188 | 2026-10-26 |
| 22 | c-epoll-TheFirstIstari | #64 | C (epoll) | **107,000** | 10,493 | 212 | 409 | 0.94 | 78 | 69 | 169 | 2026-10-21 |
| 23 | java-nio-stohirov | #88 | Java (FFM epoll) | **102,000** | 10,023 | 86 | 128 | 0.85 | 66 | 61 | 555 | 2026-10-21 |
| 24 | v-epoll-i098 | #113 | V | **100,000** | 9,848 | 1.5 | 2.9 | 0.51 | 76 | 72 | 384 | 2026-10-21 |
| 25 | node-uwebsockets-zahidcakici | #102 | Node (uWebSockets) | **97,000** | 9,471 | 264 | 484 | 0.00 | 92 | 90 | 336 | 2026-10-21 |
| 26 | typescript-bun-xxmohamed012 | #3 | Bun | **90,000** | 8,826 | 140 | 359 | 0.00 | 83 | 87 | 301 | 2026-10-21 |
| 26 | scala-netty-jacksonhoran | #107 | Scala (Netty) | **90,000** | 8,801 | 484 | 757 | 0.00 | 87 | 92 | 825 | 2026-10-21 |
| 26 | python-pypy-epoll-pranavds-codes | #84 | PyPy (epoll) | **90,000** | 8,801 | 280 | 474 | 0.00 | 89 | 94 | 270 | 2026-10-21 |
| 26 | java-springboot-bogdan-blaj | #124 | Java (Spring Boot, Netty, FFM SQLite) | **90,000** | 8,804 | 342 | 888 | 0.00 | 85 | 90 | 1,172 | 2026-10-28 |
| 30 | lua-openresty-yar0slav | #94 | Lua (OpenResty) | **88,000** | 8,477 | 434 | 591 | 0.04 | 92 | 101 | 267 | 2026-10-21 |
| 30 | asm-epoll-FlavouredTux | #75 | x86-64 asm (epoll) | **88,000** | 8,628 | 170 | 438 | 0.00 | 85 | 92 | 189 | 2026-10-27 |
| 32 | rust-axum-tbsvttr | #17 | Rust (Axum) | **76,000** | 7,434 | 280 | 551 | 0.00 | 85 | 106 | 1,118 | 2026-10-26 |
| 33 | python-stdlib-shawal-mbalire | #118 | Python (stdlib, PGO CPython) | **75,000** | 7,327 | 342 | 514 | 0.00 | 90 | 114 | 326 | 2026-10-27 |
| 34 | zig-iouring-marler8997 ᵖ | #125 | Zig (io_uring) | **71,000** ᶠ | 6,559 | 1.0 | 1.5 | 0.93 | 31 | 44 | 199 | 2026-10-21 |
| 34 | typescript-deno-JakeAve | #119 | Deno | **71,000** ᶠ | 6,558 | 5.6 | 13 | 0.93 | 47 | 67 | 412 | 2026-10-21 |
| 34 | dart-rawsocket-C-Hatton | #46 | Dart | **71,000** ᶠ | 6,550 | 34 | 80 | 0.93 | 94 | 134 | 475 | 2026-10-21 |
| 34 | cpp-epoll-khalefa-ow | #8 | C++ (epoll) | **71,000** ᶠ | 6,558 | 19 | 37 | 0.93 | 94 | 134 | 184 | 2026-10-21 |
| 34 | go-fib-sirkostya009 | #34 | Go (fib, epoll) | **71,000** ᶠ | 6,552 | 109 | 248 | 0.93 | 61 | 87 | 674 | 2026-10-26 |
| 34 | cpp-epoll_inmem-khalefa-ow | #9 | C++ (epoll) | **71,000** ᶠ | 6,555 | 30 | 148 | 0.93 | 92 | 131 | 353 | 2026-10-26 |
| 34 | c-h2o-einhornimmond | #39 | C (H2O) | **71,000** ᶠ | 6,558 | 0.8 | 4.9 | 0.93 | 39 | 56 | 331 | 2026-10-26 |
| 34 | c-epoll-benclawbot | #42 | C (epoll) | **71,000** ᶠ | 6,555 | 29 | 53 | 0.93 | 94 | 134 | 409 | 2026-10-26 |
| 34 | go-stdlib-vkrishna03 | #44 | Go (stdlib, epoll) | **71,000** ᶠ | 6,547 | 74 | 251 | 0.93 | 38 | 54 | 231 | 2026-10-26 |
| 34 | php-swoole-mattsplat | #24 | PHP (Swoole) | **71,000** ᶠ | 6,552 | 45 | 102 | 0.93 | 66 | 94 | 1,025 | 2026-10-26 |
| 34 | go-epoll-damianoneill | #52 | Go (raw epoll) | **71,000** ᶠ | 6,551 | 50 | 78 | 0.93 | 41 | 59 | 157 | 2026-10-26 |
| 34 | c-iouring-einhornimmond | #55 | C (io_uring) | **71,000** ᶠ | 6,556 | 2.7 | 10 | 0.92 | 36 | 51 | 190 | 2026-10-27 |
| 34 | php-swoole-tzimpel | #28 | PHP (Swoole) | **71,000** ᶠ | 6,552 | 17 | 175 | 0.93 | 71 | 101 | 325 | 2026-10-27 |
| 34 | c-iouring-27bhd | #68 | C (io_uring) | **71,000** ᶠ | 6,554 | 30 | 44 | 0.93 | 84 | 120 | 186 | 2026-10-27 |
| 34 | go-nethttp-mattn-vedic | #100 | Go (raw epoll, cgo SQLite) | **71,000** ᶠ | 6,552 | 36 | 50 | 0.93 | 55 | 78 | 332 | 2026-10-27 |
| 34 | rust-axum-mjc | #114 | Rust (Axum) | **71,000** ᶠ | 6,556 | 2.2 | 3.5 | 0.93 | 40 | 57 | 1,551 | 2026-10-27 |
| 34 | java-micronaut-Clickin | #116 | Java (Micronaut, JDBC SQLite) | **71,000** ᶠ | 6,546 | 78 | 148 | 0.93 | 85 | 121 | 1,293 | 2026-10-27 |
| 34 | c-libco-ssmurfgg04-gif | #121 | C (libco coroutines, epoll) | **71,000** ᶠ | 6,546 | 141 | 210 | 0.93 | 52 | 74 | 717 | 2026-10-27 |
| 34 | java-springboot-reactor-mohkamfer | #138 | Java (Spring Boot, Reactor Netty, FFM SQLite) | **71,000** ᶠ | 6,555 | 11 | 56 | 0.93 | 62 | 88 | 503 | 2026-10-28 |
| 34 | rust-axum-connorw-dev | #140 | Rust (Axum) | **71,000** ᶠ | 6,554 | 6.1 | 11 | 0.93 | 52 | 74 | 1,131 | 2026-10-28 |
| 34 | c-fio-tbsvttr | #5 | C (facil.io) | **71,000** | 6,959 | 130 | 621 | 0.00 | 81 | 108 | 646 | 2026-10-26 |
| 34 | ruby-async-cicloid | #71 | Ruby (async, YJIT) | **71,000** | 6,967 | 121 | 349 | 0.00 | 83 | 111 | 362 | 2026-10-27 |
| 56 | python-fastapi-granian-shawal-mbalire | #41 | Python (FastAPI, Granian) | **70,000** | 6,801 | 465 | 862 | 0.00 | 94 | 129 | 1,192 | 2026-10-26 |
| 56 | fortran-microhttpd-tbsvttr | #16 | Fortran (libmicrohttpd) | **70,000** | 6,866 | 141 | 401 | 0.00 | 89 | 120 | 1,134 | 2026-10-26 |
| 58 | trux-http-claudioscheer | #145 | Trux (io_uring) | **69,000** ᵗ | 6,514 | 1.6 | 234 | 0.59 | 49 | 70 | 1,043 | 2026-10-28 |
| 59 | fsharp-oxpecker-delneg | #108 | F# (Oxpecker) | **65,000** ᵍ | 6,392 | 84 | 229 | 0.00 | 55 | 80 | 1,255 | 2026-10-21 |
| 59 | go-gnet-Haris0059 | #38 | Go (gnet) | **65,000** ⁿ | 6,396 | 39 | 54 | 0.00 | 42 | 61 | 350 | 2026-10-26 |
| 59 | kotlin-ktor-C-Hatton | #45 | Kotlin (Ktor, Netty, FFM SQLite) | **65,000** | 6,374 | 198 | 713 | 0.00 | 65 | 95 | 1,273 | 2026-10-26 |
| 59 | go-gnet-kaikegm | #48 | Go (gnet) | **65,000** ⁿ | 6,394 | 88 | 251 | 0.00 | 42 | 61 | 323 | 2026-10-26 |
| 59 | go-fasthttp-b3nhard | #56 | Go (fasthttp, cgo SQLite) | **65,000** ⁿ | 6,399 | 4.8 | 19 | 0.00 | 77 | 112 | 547 | 2026-10-27 |
| 59 | cpp-drogon-hendy643 | #29 | C++ (Drogon) | **65,000** | 6,398 | 36 | 60 | 0.00 | 52 | 76 | 814 | 2026-10-27 |
| 59 | java-netty-lfeagan | #63 | Java (Netty, FFM SQLite) | **65,000** | 6,401 | 1.9 | 28 | 0.00 | 62 | 90 | 1,494 | 2026-10-27 |
| 59 | csharp-aspnetcore-vijay-highlo | #77 | C# (ASP.NET Core, Native AOT) | **65,000** | 6,389 | 87 | 319 | 0.00 | 56 | 82 | 1,278 | 2026-10-27 |
| 59 | node-net-barrard | #78 | Node (node:net, node:sqlite) | **65,000** | 6,387 | 61 | 236 | 0.00 | 76 | 111 | 433 | 2026-10-27 |
| 59 | cpp-iouring-edwin-2024 | #98 | C++ (io_uring) | **65,000** ˢ | 6,399 | 33 | 55 | 0.00 | 30 | 44 | 200 | 2026-10-27 |
| 59 | java-raw-agrison | #101 | Java (raw NIO, FFM SQLite) | **65,000** | 6,387 | 99 | 443 | 0.00 | 81 | 118 | 731 | 2026-10-27 |
| 70 | csharp-epoll-gichamba ᵖ | #74 | C# (Native AOT epoll) | **64,000** ᵉ | 6,300 | 23 | 48 | 0.00 | 31 | 46 | 163 | 2026-10-21 |
| 70 | bun-elysia-shashishsoni | #59 | Bun (Elysia) | **64,000** | 6,300 | 14 | 75 | 0.00 | 89 | 131 | 1,507 | 2026-10-27 |
| 72 | csharp-aspnetcore-maks-march | #27 | C# (ASP.NET Core) | **63,000** | 6,136 | 417 | 757 | 0.00 | 87 | 131 | 925 | 2026-10-27 |
| 73 | pypy-twisted-perkinslr | #79 | PyPy (Twisted) | **62,000** | 6,017 | 474 | 757 | 0.00 | 73 | 113 | 563 | 2026-10-27 |
| 74 | java-netty-itsnaveenk | #57 | Java (Netty) | **58,000** | 5,665 | 349 | 519 | 0.00 | 83 | 136 | 879 | 2026-10-27 |
| 75 | typescript-bun-tbsvttr | #4 | Bun | **57,000** | 5,593 | 103 | 291 | 0.00 | 86 | 143 | 301 | 2026-10-25 |
| 75 | java-quarkus-panama-raisinsfried | #73 | Java (Quarkus, FFM SQLite) | **57,000** | 5,607 | 9.6 | 359 | 0.00 | 60 | 100 | 725 | 2026-10-27 |
| 77 | haskell-warp-staticfx | #104 | Haskell (Warp) | **55,000** | 5,411 | 2.5 | 198 | 0.00 | 62 | 107 | 1,587 | 2026-10-21 |
| 77 | go-nethttp-fi5HiE | #99 | Go (net/http) | **55,000** | 5,382 | 425 | 679 | 0.00 | 90 | 156 | 1,465 | 2026-10-21 |
| 79 | go-nethttp-tbsvttr | #18 | Go (net/http) | **53,000** | 5,165 | 374 | 659 | 0.00 | 93 | 168 | 624 | 2026-10-26 |
| 79 | python-pyrustapi-rajboopathiking | #147 | Python (PyRustAPI, Rust/Hyper core) | **53,000** | 5,185 | 294 | 429 | 0.00 | 90 | 162 | 1,232 | 2026-10-28 |
| 81 | typescript-fastify-melroy89 | #51 | TypeScript (Fastify, Node) | **52,000** | 5,089 | 359 | 862 | 0.00 | 81 | 148 | 1,007 | 2026-10-26 |
| 82 | csharp-aspnetcore-andrexx | #20 | C# (ASP.NET Core) | **49,000** | 4,815 | 84 | 234 | 0.00 | 88 | 170 | 846 | 2026-10-26 |
| 83 | python-fastapi-tejasnafde | #10 | Python (FastAPI) | **48,000** | 4,701 | 303 | 465 | 0.00 | 89 | 176 | 583 | 2026-10-21 |
| 83 | swift-hummingbird-denizak ᵖ | #43 | Swift (Hummingbird) | **48,000** | 4,700 | 210 | 300 | 0.00 | 97 | 191 | 1,269 | 2026-10-21 |
| 85 | python-granian-tbsvttr | #13 | Python (Granian) | **45,000** | 4,400 | 202 | 812 | 0.00 | 93 | 197 | 709 | 2026-10-26 |
| 85 | go-nethttp-NaveedZahoorKhan | #129 | Go (net/http, cgo SQLite) | **45,000** ᵏ | 4,429 | 33 | 303 | 0.00 | 67 | 141 | 1,507 | 2026-10-28 |
| 87 | node-fastify-gmr458 | #61 | TypeScript (Fastify, Node) | **44,000** | 4,285 | 489 | 915 | 0.00 | 86 | 187 | 812 | 2026-10-27 |
| 88 | ruby-iodine-tbsvttr | #14 | Ruby (iodine) | **43,000** | 4,223 | 89 | 175 | 0.00 | 86 | 189 | 1,096 | 2026-10-26 |
| 89 | java-spring-boot-nb-99 | #33 | Java (Spring Boot WebFlux) | **32,000** | 3,151 | 9.9 | 27 | 0.00 | 81 | 239 | 763 | 2026-10-26 |
| 90 | java-quarkus-wolches | #67 | Java (Quarkus) | **29,000** ᵈ | 2,838 | 306 | 451 | 0.00 | 50 | 164 | 542 | 2026-10-21 |
| 91 | erlang-cowboy-tbsvttr | #11 | Erlang (Cowboy) | **28,000** | 2,751 | 76 | 174 | 0.00 | 85 | 287 | 1,214 | 2026-10-26 |
| 91 | csharp-aspnetcore-devv-guru | #95 | C# (ASP.NET Core) | **28,000** | 2,747 | 216 | 788 | 0.02 | 67 | 227 | 510 | 2026-10-27 |
| 93 | go-gin-lincolng4 | #103 | Go (Gin) | **27,000** ᵐ | 2,658 | 1.2 | 4.8 | 0.00 | 42 | 147 | 1,600 | 2026-10-27 |
| 94 | java-flash-relism | #133 | Java (Flash) | **25,000** | 2,449 | 280 | 991 | 0.00 | 38 | 144 | 1,464 | 2026-10-28 |
| 95 | lua-lunet-simbo1905 | #126 | Lua (lunet), behind Nginx | **24,000** ˡ | 2,362 | 20 | 38 | 0.93 | 93 | 366 | 38 | 2026-10-28 |
| 96 | typescript-effect-SeeThruHead | #49 | TypeScript (Effect, Node) | **23,000** | 2,229 | 413 | 765 | 0.00 | 96 | 399 | 623 | 2026-10-26 |
| 97 | php-symfony-pklebba | #40 | PHP (Symfony, FrankenPHP) | **21,000** | 2,061 | 186 | 274 | 0.00 | 95 | 428 | 915 | 2026-10-26 |
| 98 | pascal-horse-viniabreu | #134 | Pascal (Horse) | **20,000** | 1,966 | 81 | 111 | 0.00 | 90 | 426 | 270 | 2026-10-28 |
| 99 | csharp-aspnetcore-larsholm | #47 | C# (ASP.NET Core) | **19,000** | 1,854 | 381 | 943 | 0.00 | 57 | 286 | 439 | 2026-10-26 |
| 99 | crystal-kemal-krthr | #146 | Crystal (Kemal) | **19,000** ᵐ | 1,869 | 27 | 153 | 0.00 | 24 | 119 | 1,631 | 2026-10-28 |
| 101 | kotlin-springboot-Mindstorm1750 | #66 | Kotlin (Spring Boot, Tomcat, virtual threads) | **18,000** | 1,760 | 397 | 870 | 0.00 | 61 | 323 | 792 | 2026-10-27 |
| 102 | python-litestar-przwr | #23 | Python (Litestar) | **16,000** | 1,569 | 259 | 788 | 0.00 | 66 | 391 | 256 | 2026-10-26 |
| 103 | kotlin-ktor-jvm-syorito-hatsuki | #36 | Kotlin (Ktor, JVM) | **14,000** | 1,370 | 370 | 728 | 0.00 | 79 | 536 | 603 | 2026-10-26 |
| 104 | kotlin-ktor-graalvm-noamm9 | #53 | Kotlin (Ktor, GraalVM native) | **10,500** | 1,027 | 356 | 906 | 0.00 | 79 | 715 | 314 | 2026-10-26 |
| 105 | ruby-rails-gambitboy | #2 | Ruby (Rails), behind Nginx | **9,000** | 876 | 322 | 443 | 0.00 | 97 | 1,031 | 257 | 2026-10-25 |
| 105 | ruby-rails-tbsvttr | #12 | Ruby (Rails) | **9,000** | 875 | 409 | 562 | 0.00 | 98 | 1,043 | 305 | 2026-10-26 |
| 107 | bash-socat-framegrace | #89 | Bash (socat, behind Nginx) | **3,500** | 344 | 18 | 43 | 0.00 | 64 | — | 107 | 2026-10-21 |
| — | nim-io_uring-NioHast | #132 | Nim (io_uring) | **failed** (hangs) | | | | | | | | 2026-10-21 |
| — | v-fasthttp-ulises-jeremias | #69 | V (fasthttp) | **failed** (install OOM) | | | | | | | | 2026-10-27 |
| — | goose-native-tbsvttr | #143 | Goose (native) | **failed** (install OOM) | | | | | | | | 2026-10-28 |

ˣ **Rust #82 ran out of load generators first.** One target IP × 4 load generators caps at ~240,000 connections (source
ports). It passed 240,000 at 79 % CPU, so it was re-searched with 3 extra IPv4s on the box and a 5th load generator
(cap ~1.2M): 300,000 failed on latency (p99 888 ms 2-min, CPU 89 %), 293,000/278,000 failed the 5-minute hold on p99,
**264,000 passed**. 360k/480k probes got the app OOM-killed (2 GB box). The 240,000 run on the original setup is consistent
(23,166 req/s, 31 µs/req). Its follow-up **#141** (`b1efee9`, background WAL checkpoints; held, not merged: it
conflicts with the merged #82 folder) scored **230,000** on the standard 4-source-IP setup (22,384 req/s, p95 14 / p99
62 ms, 0 err, 91 % CPU, 37 µs/req, 931 MB, 2026-10-28) and is CPU-bound, not source-port-bound: 240,000 failed at 95 % CPU
(p99 1.5 s). The row above stays on #82's merged code until #141 is rebased and merged.

ᶠ **The 65,535 file-descriptor limit, not speed.** These twenty serve 65,535 keep-alive users and the rest wait in the
listen queue until mload's 60 s timeout. Each scored run shows exactly ~21,900 timeouts = 0.93 % of requests, just under the
1 % rule — so 71,000 is "full fd table + 1 % slack", and the same number for a C++ server at 94 % CPU and a Zig one at 31 %
(Go fib #34: 61 % CPU, flat from 70k to 78k users while timeouts grow; C H2O #39: 39 % CPU, 4.9 ms p99, and its README
says it doesn't shed connections at the limit; C epoll #42: 39 % CPU at 60k, 93 % from 70k as it accepts and closes
the connections past the limit; Go stdlib #44: 38 % CPU, flat from 70k to 78k users, and its README says accept
pauses at the limit; PHP Swoole #24: 66 % CPU, flat from 75k to 78k users while timeouts grow; Go epoll #52: 41 % CPU, 78 ms p99, it
stops accepting at the limit and never closes idle keep-alives; C io_uring #55: 36 % CPU, 10 ms p99, request latency
flat from 20k to 78k users while connect timeouts grow; PHP Swoole #28: 71 % CPU, flat from 75k to 78k users while
timeouts grow; C io_uring #68: 84 % CPU from 70k, 44 ms p99, request latency flat from 70k to 80k users while connect
timeouts grow; Go epoll #100: 55 % CPU, 50 ms p99, CPU and req/s flat from 70k to 78k users while timeouts grow; Rust Axum #114: 40 % CPU, 3.5 ms p99, CPU and request
latency flat from 70k to 80k users while connect timeouts grow; Java Micronaut #116: 85 % CPU, 148 ms p99, CPU and req/s
flat from 70k to 78k users while timeouts grow; C libco #121: 52–54 % CPU, 210 ms p99, CPU and req/s flat from 70k to 80k
users while timeouts grow; Java Reactor Netty #138: 62–65 % CPU, 56 ms p99, CPU and req/s flat from 70k to 78k users
while timeouts grow; Rust Axum #140: 52–54 % CPU, 11 ms p99, CPU and req/s flat from 70k to 80k users while timeouts grow).
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

ˢ **C++ io_uring #98 stalls at the fd limit.** At 65,000 it was clean at 30 % CPU (55 ms p99). At 68k/70k/80k it went to
100 % CPU and timed out 21–30 % of requests (~1,780 req/s), likely because a failed accept (EMFILE) ends the multishot
accept and the loop re-arms it straight away, so it spins on accept errors (server.cpp:281–326).

ᵐ **Go Gin #103 is memory-bound.** Latency stays low (4.8 ms p99 at 42 % CPU), but memory grows with users: 1.6 GB at
27,000. At 28,000+ (5-minute hold) and 29k/30k/40k it was OOM-killed and later connections were refused (50–75 %
errors). Its first run, before the rig capped the app at 1,750 MB, used all 2 GB at ~28k and froze the box.
Crystal Kemal #146 is the same: 27 ms p95 / 153 ms p99 at 24 % CPU, but 1.6 GB at 19,000; at 20,000+ (including the
20,000 5-minute hold) the app was OOM-killed and restarted every 1–4 minutes, and connections in between were refused
(40–93 % errors at 4–21 % CPU).

ˡ **Lua lunet #126's errors are writes cut off near full CPU.** The 0.93 % in its scored hold (93 % CPU) are EOFs
(connection closed with no response), almost all on POSTs: 12 % of likes and 10 % of new posts failed, 0 reads. They
grow with load (0.26 % at 23k, 1.45 % at 25k); at 30k+ Nginx also returns 502/504s.

ᵏ **Go net/http #129 is memory-bound.** At 45,000 it was clean at 67 % CPU (303 ms p99), but its cgroup peaked at 1.5 GB.
At 47k/48k/50k/60k/80k it was OOM-killed during the ramp (kernel log) and most later connections were refused (54–93 %
errors, 6–36 % CPU).

ᵗ **Trux #145 is connection-capped, not CPU-bound.** At 69,000 it ran at 49 % CPU with a 1.6 ms p95; its 0.59 % errors
are 60 s timeouts on `GET /feed` (new connections not served), and they grow with users while CPU stays at 48–52 %:
0.41 % at 70k, 0.68 % at 73k (2-min), 2.0 % at 75k. 73,000 failed the 5-minute hold at 1.26 %.

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
- **Goose #143 — install failed** (`1454fa6`): `install.sh` builds the pinned Goose compiler from source (cmake/ninja),
  and that build runs the 1 vCPU / 2 GB box out of memory: mu-c1 froze twice (07:47 and after the 09:02 re-run), and
  the install hit its 1-hour timeout, so the app never built (1/42 tests). Not patched.
- **D #123** (vibe.d) — dropped before the run: fails test.sh (HTML default 404 body; a bad `sub` crashes with 500).

## Rule flags from code review (nothing malicious found; please fix in a follow-up PR)

- **Tokens without `exp` accepted** (SPEC requires rejecting them): java-nio #88, quarkus #67 (SmallRye
  grace), kotlin #83, scala #107, node-uws #102, dart #46, go-champ3oy #76, pypy #84 (deliberately), csharp #74, bash #89,
  go-gnet #38, kotlin-ktor #36, python-fastapi-granian #41, go-stdlib #44, kotlin-ktor-C-Hatton #45, typescript-fastify #51, go-epoll-damianoneill #52,
  kotlin-ktor-graalvm #53, go-fasthttp #56, java-netty #57, bun-elysia #59, node-fastify #61, java-netty-lfeagan #63, c-iouring-27bhd #68, node-net #78, pypy-twisted #79, go-nethttp-mattn #100, java-raw #101, python-stdlib #118, java-springboot-reactor #138, trux-http #145 (all held open until fixed). cpp-eastl #32 fixed it in `4d5e225`, c-iouring-meros #137 in `b2a5207`, python-pyrustapi #147 in `82abaab` and asm-epoll #117 in its follow-up #152 (`ad7a72c`) (re-scored and merged).
- **Kotlin Ktor #36 (and #53, based on it) commit `gradle/wrapper/gradle-wrapper.jar`**. Arjay ruled (2026-10-26) that
  the standard Gradle wrapper jar is fine and not a prebuilt binary under rule 10. Kotlin Ktor #36 also returned a few HTTP 500s on writes under load (1–5 per probe,
  0 in the scored hold); its StatusPages handler doesn't log the exception. kotlin-springboot #66 commits the same
  jar and was merged on that ruling.
- **Past ~327k connections, cpp-eastl #32 evicts the connection it just served** because "the think-time loop visits
  users in the same order every time" (its README) — tuned to the load test's order (rule 7 question). Its score
  (210k) is below that point, so it doesn't affect the number.
- **go-lowlevel #91 builds with committed PGO profiles** (`profiles/go.pprof`, `profiles/sqlite.profraw`, on by default)
  that the author trained on their own load runs; the author asked for organizer review (rule 7/10 question: profile
  data tuned to the benchmark, and a non-source input to the build). Scored as submitted (125k); held open for Arjay.
  go-epoll-kaikegm #151 does the same: `go build -pgo=auto` with a committed `default.pgo` recorded from the author's own
  load runs (its `PGO_RECORD` hook). Scored as submitted (145k); held open for Arjay.
- **python-pyrustapi #147 serves its reads from the `pyrustapi` package's native SQL tier** (`sql=` route options),
  installed as a prebuilt PyPI wheel; the author says it's their own open-source framework and builds from source
  with `--no-binary` (rule 10 question). Scored as submitted (53k); held open for Arjay.
- **odin-odinhttp #139 drops the box's page cache at startup**: `pagecache.odin` walks /usr, /var, /etc (and its own
  folder) and calls `posix_fadvise(DONTNEED)` on each file (rule 11 question: it acts on files outside its folder).
  Scored as submitted (130k); held open for Arjay.
- **Reads can see uncommitted writes** in the same batch: Deno #119 (also serves a snapshot up to 20 ms stale), Zig #125.
- **start.sh restart loops** (the app restarts itself instead of letting systemd see a crash): Deno #119, c-epoll #64,
  c-iouring-raoz #26 (restarts after a signal exit), go-epoll-Antheurus #65 (worker 0 re-spawns dead workers).
- **Unchecksummed toolchain downloads in install/build**: Deno (curl | sh), Go #72, Haskell (ghcup), Swift, .NET SDK,
  rustup (#82, also writes /usr/local/bin wrappers), Zig (anyzig).
- Bash #89 builds SQL by string concatenation (quotes doubled) — works, fragile.
- c-iouring #112's README describes a different design than the code (THREADSAFE=2, per-write commit); it sheds load
  based on /proc/meminfo (c-iouring-raoz #26 also reads /proc/meminfo, server.c:1314; c-libco #121 reads it once to pick its process count). python-fastapi-granian #41 likewise reads the box-wide `/proc/self/net/sockstat` to send
  `Connection: close`; the load test reconnected ~4.5× per user even at 40k users (180k dials), with 0 errors.

## Crashes during probes that failed anyway

The app journal showed OOM kills only in probes that failed anyway: Rust #82 at 360k/480k, asm-nasm #86 at 200k/240k,
go-nethttp #99 at 57k, Haskell #104 at 58k, php-symfony #40 at 40k (FrankenPHP at 1.5 GB), rust-axum #17 at 160k/120k, go-nethttp-tbsvttr #18 at 80k, csharp-aspnetcore-andrexx #20 at 80k (.NET "Out of memory." abort), csharp-aspnetcore-maks-march #27 at 80k/70k (.NET `PAL_SEHException` abort past the fd limit, like F# ᵍ), cpp-eastl #32 at 320k, c-iouring-raoz #26 at
320k/240k/200k and in the 190k/181k 5-minute holds. Scored holds were all clean.
