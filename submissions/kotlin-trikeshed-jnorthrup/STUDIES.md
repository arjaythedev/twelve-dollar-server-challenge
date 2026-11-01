# Studies log: the Kotlin/Native server

This log records every measurement study made on the KN server (Kotlin/Native, io_uring, stock SQLite). It is not part of the scored entry. Each study has a question, a method, a result and a finding. All numbers are measured, except the lines marked "estimate".

Dates: 2026-10-31 and 2026-11-01. Nothing here is pushed. Challenge branch `sctp` holds the commits.

## 0. Boxes, harness and how to read the numbers

- **kali**: WSL2 guest on an i7-6820HK (4 cores, 8 threads). Server on CPU 1 in the box limits (CPUQuota 100%, 2 GB, no swap, 65535 files). `wrk` on CPUs 2-3. Conntrack and nftables are on.
- **Scored box** (from LEADERBOARD.md): Synteq 1 vCPU / 2 GB, Xeon Platinum 8260, kernel 6.8, no UFW, so no conntrack. Load comes from separate 16-core machines.
- **Rule for reading**: kali micro numbers are the user-space floor. They are not the scored-box numbers. The kernel part differs.
- **Tools** (skill `twelve-dollar-kali-loadtest`, folder `scripts/`): `knee2.sh` (one route, any connection count, optional `perf`), `difftest.sh` (same requests to two binaries, every response compared), `e2eprof.sh` (64k-user run with `perf`), `winners.sh` (ours against the C winners), `e2ew.sh` (64k-user run with cgroup CPU accounting), `mixlat.sh` (read latency while writes run).
- **Pitfalls found**: the `wrk` soft file limit is 1,024 (steps above it silently ran at about 1,000 connections); a Lua `response()` makes `wrk` the bottleneck; a `pgrep -f` wait loop over ssh matches itself; `git add` stops on one bad path.

## S1. Cost of each route (before any fix)

Question: where does the server spend its CPU per route?
Method: `knee2.sh`, 1,024 connections, 8 s, binary from challenge commit 2495dd0. Server µs per request = cgroup CPU time over requests.

| route | req/s | µs/request | server CPU |
|---|---|---|---|
| health | 97,142 | 8 | 85% (`wrk` limits) |
| GET /posts/:id | 66,914 | 14 | 97% |
| GET /feed | 16,914 to 17,188 | 57 | 98% |
| like | 8,854 | 32 | 29% |
| create | 9,832 | 35 | 34% |

The k6 mix is feed 46.1%, post 46.1%, like 6.9%, create 0.9%. Weighted cost: 35 µs per request. The feed is 75% of it.
Write routes hold their reply for the 64 ms commit window. Their req/s is connections divided by latency, so compare µs per request, never req/s.

## S2. Feed knee (before any fix)

| connections | req/s | µs/request | p50 | p99 | process memory |
|---|---|---|---|---|---|
| 16 | 12,014 | 77 | 1.27 ms | 132.6 ms | 39 MB |
| 64 | 15,214 | 61 | 4.27 ms | 101.8 ms | 39 MB |
| 256 | 14,563 | 66 | 17.0 ms | 41.8 ms | 39 MB |
| 1,024 | 16,641 | 58 | 59.6 ms | 74.6 ms | 39 MB |
| 4,096 | 16,456 | 59 | 235 ms | 275.8 ms | 41 MB |
| 16,384 | 14,952 | 65 | 941 ms | 1.09 s | 45 MB |

Finding: the server saturates near 16.6k req/s at 98% CPU. Process memory grew 6 MB from 1,024 to 16,384 connections: about 0.4 KB per connection. The kernel holds the rest.
Open: the p99 near 100 ms at 16 and 64 connections is not explained. One connection alone shows a 3 ms maximum.

## S3. Profile of the feed route (before)

Method: `perf record -e cpu-clock -F 999`, 6 s, 98% CPU. Shares: feed.kexe 73.9%, kernel 21.7%, libc 4.4%.
Top symbols: `SqliteStore#text` 17.8%, `Out#escapeFrom` 14.6%, `sqlite3VdbeExec` 6.8%, `sqlite3BtreeTableMoveto` 6.1%, `_raw_spin_unlock_irqrestore` 4.5%, `memmove` 3.2%.
Finding: the Kotlin text copy and escape cost 32% of the feed. The GC did not appear. Kotlin/Native does not vectorize byte loops.

## S4. Fix 1: `feed_json_text` (commit 7fbf26b)

A C function in `feedc.def` copies and escapes one text column (eight bytes tested per step, one `memcpy` per clean run).
- Test first: `server/src/nativeInterop/feedjson_test.c` cuts the function out of the .def and holds it to a byte-wise reference: 5,728,965 cases (every byte at every position to length 40 against 13 fillers, 3M random strings). It is clean on arm64 and x86_64 and under AddressSanitizer with exact-size buffers. Mutants (wrong backslash mask, wrong control threshold, read past the end) fail.
- Differential: 734 posts with special characters sent to the old and new binary, feed and 757 reads compared: 0 differences. The 42 checks pass.
- Result (old, new, old, new): feed 16,939 and 17,047 req/s at 57 µs became 21,492 and 21,557 req/s at 45 µs. Post fetch 65.3k became 70.3k req/s (15 to 13 µs).
- Profile after: `text` 9.5%, `refTo` pinning plus GC allocation and sweep about 6.5%.

## S5. Fix 2: `feed_row_json` (commit 06182a8)

One C call writes a whole feed row (the Kotlin side made about twelve cinterop calls per row; each costs about 13 ns).
- Test first: the same C test holds it to SQLite itself: 3,013 rows (ids from LLONG_MIN to LLONG_MAX, NULL and empty texts, random bytes, a 1,201-like count, 5,000-byte bodies that all grow six-fold), exact-size buffers, AddressSanitizer, on arm64 and on x86_64 against the server's SQLite 3.54.0. Six mutants fail. One mutant (a smaller fixed bound) cannot fail: it needs a 20-digit like count.
- Differential: 0 differences over 757 reads. The 42 checks pass.
- Result, order old, fix 1, fix 2, twice:

| route | old | fix 1 | fix 2 |
|---|---|---|---|
| GET /feed | 17,188 and 16,914 req/s, 57 µs | 20,727 and 21,411, 47 and 45 µs | 24,097 and 24,157, 40 µs |
| GET /posts/:id | 66,507 req/s, 14 µs | 71,074, 13 µs | 73,300, 12 µs |

Profile after fix 2 (feed): feed.kexe 61.1%, kernel 34.4%, libc 4.4%. SQLite functions about 41% (16.5 µs): `sqlite3VdbeExec` 9.4%, `BtreeTableMoveto` 8.6%, `BtreeIndexMoveto` 4.2%, `btreeInitPage` 3.5%. Our code: `row` 3.0%, `feed_json_text` 2.8%.

## S6. Whole run at 64,000 users (load generator in the same VM)

Method: `kn.sh load 64000` (60 s ramp, 60 s hold, 6,718 to 6,771 req/s).

| binary | user % | sys % | total | µs/request (user + sys) | p50 / p95 / p99 | cgroup peak |
|---|---|---|---|---|---|---|
| old | 25.00 | 32.36 | 57.4% | 85.4 (37.2 + 48.2) | 64.5 / 126 / 151.6 ms | 525 MB |
| fix 1 | 23.64 | 32.45 | 56.1% | 83.5 (35.2 + 48.3) | 64.5 / 126 / 151.6 ms | 519 MB |
| fix 2 | 20.55 | 33.09 | 53.6% | 79.8 (30.6 + 49.2) | 64.5 / 127 / 151.6 ms | 518 MB |

(User/sys ticks from /proc; the cgroup peak includes page cache.) A second run of fix 2 with cgroup accounting: 21.0% + 30.8% = 51.8%, 77.1 µs (31.2 + 45.8).
Profile at 64k (fix 2): kernel 59.5%, feed.kexe 38.2%, libc 2.1%. `_raw_spin_unlock_irqrestore` alone is 25.4% of all samples. SQLite: `BtreeTableMoveto` 3.9%, `VdbeExec` 3.0%, `pcache1Fetch` 1.9%, `walFindFrame` 1.4%. GC: mark 1.8%, allocate 1.4%, sweep 1.0%. Pinning (`Pinned` init, `refTo`): 1.6%.
Finding: user CPU per request fell 18% (37.2 to 30.6 µs). The kernel part did not change.
Inference (not proven): the 25% in one function is the load generator waking threads across virtual CPUs in the same VM. The Mac-driven run of the same server needed 40% CPU (59 µs) at 64k users.

## S7. Component bench, native against JVM (Mac arm64)

| case | native ns/op | JVM ns/op |
|---|---|---|
| known-schema JWT payload | 246 and 252 | 81 and 87 |
| known-schema POST body | 287 and 293 | 172 and 181 |
| LongTable put+get+remove | 34 and 36 | 60 and 51 |
| 64 KiB file round trip | 98,005 and 105,261 | 107,435 and 118,596 |

Finding: native is not faster by default. Byte-scanning code is 1.6 to 3 times slower in Kotlin/Native than on the JVM JIT. Table code is faster.

## S8. The two C winners on the same box

Method: `winners.sh`. Same box limits, same database, same harness, 1,024 connections, 8 s. Binaries built on kali by the entries' own `build.sh` (#1 profile-guided, #5 with LTO and liburing 2.5). Not run: #2 to #4 (Lean, Mojo, Objective-C).

| route | ours | #1 pavlyhalim | #5 shivanshthapliyal |
|---|---|---|---|
| health | 8 µs | 6 | 7 |
| GET /feed | 40 | 17 | 20 |
| GET /posts/:id | 13 | 7 | 7 |
| like | 32 | 19 | 21 |
| create | 34 | 20 | 25 |
| k6 mix average | 27.0 | 12.6 | 14.1 |

Gap to #1 by route (mix-weighted): feed 10.6 µs, post 2.8, like 0.9, create 0.1; total 14.4.
Feed profiles (6 s `perf`): kernel share ours 34% (13.7 µs), #1 54% (9.2 µs), #5 52% (10.5 µs). #1's user time is 7.8 µs: B-tree walk about 4.6 µs, JSON about 1.9 µs. No SQLite VM function appears in either winner. #1 reports `avg_batch=328.7` requests per wake.
Finding: the I/O path is not the gap (health 8 against 6 to 7 µs). The gap is SQL work per route, then kernel batching, then Kotlin glue and GC.

## S9. The winners at 64,000 users

Method: `e2ew.sh`, cgroup CPU accounting (user and system), same load.

| server | req/s | user % | sys % | total | µs/request (user + sys) | p50 / p95 / p99 |
|---|---|---|---|---|---|---|
| #1 | 6,769 | 7.23 | 31.14 | 38.4% | 56.7 (10.7 + 46.0) | 2.5 / 3.8 / 4.8 ms |
| #5 | 6,771 | 9.77 | 34.70 | 44.5% | 65.7 (14.4 + 51.2) | 0.2 / 0.2 / 0.6 ms |
| ours | 6,720 | 21.00 | 30.81 | 51.8% | 77.1 (31.2 + 45.8) | 64 / 127 / 151.6 ms |

Finding: the kernel part is the same for all three (about 46 µs on kali). The user part is 31.2 against 10.7 and 14.4 µs.
Estimate (not a measurement): #1 costs 26 µs per request on the scored box, about 11 of it user-space. Ours would cost about 43 to 47 µs. At the 72 to 82% CPU where neighbours pass, that is 160k to 190k users: rank about 31 to 38 of 164. The JVM entry is rank 76 (71k users).

## S10. Do reads wait for the commit window?

Method: `mixlat.sh`. One `GET /feed` connection alone, then while 64 connections post likes.

| server | alone | while 64 post likes |
|---|---|---|
| ours | 6,360 req/s, p50 139 µs | 15 req/s, p50 65.8 ms, p99 123.6 ms (likes: 868 req/s) |
| #1 | 7,694 req/s, p50 119 µs | 636 req/s, p50 1.38 ms, p99 6.11 ms (likes: 13,735 req/s) |
| #5 | 7,868 req/s, p50 116 µs | 289 req/s, p50 3.44 ms, p99 7.32 ms (likes: 22,838 req/s) |

Cause (Xio.kt, `emit`): while a write transaction is open, every response is parked until COMMIT, GETs included. The window is 64 ms (`COMMIT_WINDOW_NANOS`). In the k6 mix writes are 7.8% of requests, so a window is nearly always open. That is why p50 is 64 ms in every run.
Reason (inferred from the code, not tested): one SQLite connection with `locking_mode=EXCLUSIVE` reads its own uncommitted rows, so parking every response keeps uncommitted rows invisible. The 64 ms was chosen for CPU: the README (Xio.kt lines 61-66) records 67.1 to 69.6 µs per request with one COMMIT per pass, 59.7 to 62.1 with 32 ms, 56.2 to 59.0 with 64 ms, 56.3 to 56.4 with 128 ms (JVM era). Not yet measured on the KN server.

## S11. Builds

- kali box build (`b2g.sh`, 1 CPU, 2 GB cgroup): 374 s; `feed.kexe` 4,095,232 bytes. The peak of 2,047 MB counts page cache.
- RunPod pod (4 GB, 2 vCPU, Ubuntu 24.04 rootfs): 252 s on 2 vCPU, 305 s pinned to 1. Non-cache memory peak 1.77 to 1.81 GB (Gradle JVM alone 1.68 GB at -Xmx1g). Disk peak 3.0 GB. The pod was left clean (1% disk, only sshd).
- A cold `build.sh` in the box (helper G, unfinished): at 1,788 s still running, anon 1,368 MB, no OOM kill.
- Incremental server build on idle kali (8 CPUs): 92 to 94 s.
- Open: get the link under 1.4 GB non-cache memory before the scored `build.sh` builds KN. The box has 1,967 MB and no swap.

## S12. Earlier TCP runs, Mac as load client (stored)

| run | users | req/s | errors | p95 | server CPU | µs/request |
|---|---|---|---|---|---|---|
| n64 | 64,000 | 6,732 | 0.00% | 110 ms | 47% | 70 |
| f64x16 | 64,000 | 6,735 | 0.00% | 116 ms | 40% | 59 |
| c64 | 64,000 | 6,605 | 0.00% | 184 ms (p99 4.65 s, fails) | 43% | 65 |
| c128 | 128,000 | 5,047 | 3.18% | 40 s | 37% | 73 |
| g128x32 | 128,000 | 3,094 | 13.20% | 112 s | 22% | 72 |

Every 128k run failed with the server at 22 to 37% CPU, zero listen or backlog drops, and 1.05 to 1.49 million TCP retransmits (37k in the passing 64k run). The loss is on the Mac-to-kali path. The 128k results say nothing about the server. A 128k run needs a Linux load generator.

## S13. SCTP over UDP (bench transport; k6 scores over kernel TCP)

- Emulated ring, 32k users: 3,353 req/s, about 190 µs per request.
- Native datagrams, 4 MB socket buffers, RFC 9002 loss recovery: 32k users 3,367 req/s, 0 failed, p50 / p95 / p99 50 / 102 / 137 ms, about 154 µs per request, no UDP drops.
- 64k users (`s64q`): failed. Peak established 62,243; 2,148 failed; 821,879 UDP receive-buffer errors.
- Profile of the 32k run: software CRC32c 4.8%, GC about 8%, the HashMap, Enum.equals, iterator and ArrayDeque families 5.7% together (HashMap alone 3.7%), unwinding 2.6%, kernel 27%.
- Hash-map audit (helper B): the TCP request path has no hash map or set. The SCTP path has 18 entries (five Long-keyed maps per request).
- Re-run of 32k with the current server (`s32r`, after fix 2): 3,367 req/s, 0 failed, p50 / p95 / p99 49.2 / 98.3 / 131.1 ms, server CPU user 36% + sys 14% = 50%, about 148 µs per request (before: 38% + 14% = 52%, 154 µs). No UDP drops on either end. Cgroup peak 131 MB. Finding: the faster row path saves about 4% on SCTP. The cost is in the transport (CRC32c, GC, hash maps, kernel), not in SQLite.

## S14. What the five winners do that we did not (from their READMEs, 2026-11-01)

1. B-tree cursor reads for /feed and /posts/:id (S8 confirms the size of this gap).
2. Waiting to form a batch when busy (#1: 64 completions or 2 ms).
3. Eight SO_REUSEPORT listeners (#1 counted 1.8 million accept-queue overflows with one).
4. WAL checkpoints off the loop.
5. Memory discipline (no multishot receive on idle connections in #1; fixed batch caps; code locked in RAM).
Their own rules checks found no rule break. #2 (hargup) depends on dense ids and a non-decreasing `created_at`: do not copy that part.

## Next steps, in order of measured size

1. B-tree cursor reads in C for feed, post and like counts: about 8 µs per mix request.
2. Reads must not wait for the commit window. Measure the window knee (64, 16, 4, 2, 1 ms) with `mixlat.sh` and `knee2.sh like`, then a 64k run.
3. Batch formation (2 ms or 64 completions): about 3 µs per mix request.
4. Pin once and recycle windows (Poller arrays, `Out` buffers): about 2.5 µs, and the GC share falls.
5. WAL read path (`walFindFrame`, `pcache1Fetch` 3.3%) and the checkpoint off the loop.
6. Task P follow-up: the other three winners need their own toolchains.
7. 128k run with a Linux load generator; SCTP 64k.

## State

- Challenge repo, branch `sctp`: 06182a8 (`feed_row_json`), 7fbf26b (`feed_json_text`), 2495dd0 (lz in commonMain, TrikeShed pin a7d4caec1). `main` is the PR head 87b974c. Push guard on. Nothing pushed.
- Shelved: mingw lines in `trikeshed/build.gradle.kts` (uncommitted).
