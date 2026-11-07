# Competitive Analysis — BinaryCore

This document answers the competitive questions from the master engineering prompt (§3). Every claim is supported by source code, official benchmark data (LEADERBOARD.md), or a reproducible measurement reported in an existing submission's README.

Sources referenced:
- `submissions/c-iouring-pavlyhalim/` — top C io_uring implementation (295,000 users, #3 on leaderboard). Files: `src/sys.h`, `src/server.c`, `src/app.h`, `src/db.h`, `src/btree.h`, `src/http.h`, `src/json.h`, `src/jwt.h`, `src/sha256.h`, `build.sh`, `README.md`.
- `submissions/c-iouring-meros/` — simpler C io_uring implementation (238,000 users, #24). Uses standard SQL, no B-tree reads.
- `LEADERBOARD.md` — official scored results on the Synteq 1 vCPU / 2 GB box.
- `README.md`, `SPEC.md`, `schema.sql`, `test/test.sh`, `bench/load.js` — official challenge files.

---

## 1. Why is the current leader faster?

The leader is `rust-iouring-kensac` at **310,000 users** (LEADERBOARD.md:59), 29,284 req/s, 25 µs/req, 82% CPU, 1,114 MB RAM. It uses Rust io_uring with SQLite B-tree reads.

The top C entry, `c-iouring-pavlyhalim`, is at **295,000 users** (LEADERBOARD.md:61), 28,013 req/s, 26 µs/req, 81% CPU, 1,536 MB RAM. It uses the same architectural class: C io_uring + SQLite B-tree reads.

The gap between #1 (310k) and #3 (295k) is ~5%. Contributing factors:

**a) Per-request CPU cost:** 25 µs/req (Rust) vs 26 µs/req (C pavlyhalim) — a 4% difference. At 28,000 req/s on one vCPU, that's ~0.7 vs ~0.73 CPU seconds per second of wall time. The difference is small but measurable.

**b) Language/runtime:** Rust avoids some C footguns (no manual memory management bugs, better inlining across crates) but the gap is not Rust-specific — `c-iouring-unorig-username` (C, 280k) and `c-iouring-shivanshthapliyal` (C, 276k) are close behind. The leader's edge is marginal, not structural.

**c) Memory:** leader at 1,114 MB vs pavlyhalim at 1,536 MB. The leader likely uses a smaller page cache and/or frees memory more aggressively, letting it hold more connections before hitting the kernel-socket-memory wall (~4.3 KB/socket on kernel 6.8, pavlyhalim README.md:57).

**d) B-tree reads:** both leader and pavlyhalim use SQLite B-tree cursor reads (skipping the bytecode interpreter). c-iouring-meros (238k, #24) does NOT — it uses standard prepared SQL — confirming B-tree reads are worth roughly 50k+ users (295k vs 238k, same language family).

**e) The real ceiling is memory, not CPU:** pavlyhalim README.md:81-82: "the server is not stopped by CPU but by memory: the kernel's per-socket memory runs the machine out." At ~4.3 KB/socket kernel memory + server overhead, 2 GB supports roughly 300k-400k connections. The leader packs more connections into the same RAM.

**Conclusion:** The leader is faster by a small margin (5%), primarily through slightly lower per-request CPU and better memory packing. It is not fundamentally a different architecture. A C implementation matching pavlyhalim's full stack plus better memory tuning can be competitive.

---

## 2. Why does the current C implementation consume more memory?

`c-iouring-pavlyhalim` reports 1,536 MB RAM at 295,000 users (LEADERBOARD.md:61). The leader uses 1,114 MB.

**a) Page cache size:** pavlyhalim defaults to 8 MB page cache (`build.sh:26` `CACHE_KIB=8192`), and its README.md:194 says "8 MB is kept: it frees 24 MB against 32 MB." But the working set is ~34 MB (README.md:188-189). At 8 MB, misses are 0.053/request; at 32 MB, 0.0017/request. The leader likely tunes this differently — possibly a smaller cache accepting more page-cache hits from the kernel's own page cache (which is free memory, not charged to the process cgroup).

**b) Ring and buffer pre-allocation:** pavlyhalim creates up to 16 rings (`server.c:13` `MAX_RINGS 16`), each with 65,536 CQ entries (`server.c:317`), 4,096 × 1 KB receive buffers (`server.c:922`), and a 65,471-slot connection table (`server.c:914`). Even sparsely populated, the mmap'd regions reserve address space. On kernels charging ring memory to `RLIMIT_MEMLOCK` (8 MB under systemd, README.md:53-56), this is constrained, but the cgroup measurement includes everything.

**c) mlocked code + data:** pavlyhalim `mlock`s its entire static binary (~2.1 MB, README.md:85) plus arenas and stack pre-faults (README.md:115-116). This pins memory the kernel cannot reclaim.

**d) Arena pool:** 64 arenas × 1 MB (`server.c:14-15` `ARENA_CAP 1MiB, ARENA_POOL 64`) = 64 MB reserved, plus batch arenas up to 256 × 1 MB = 256 MB worst case (though normally far less).

**e) Kernel socket memory (~4.3 KB/socket)** is counted in the cgroup and is the dominant term: 295,000 × ~4 KB ≈ 1.2 GB just for sockets. The server's own additions are small (40 bytes/conn, README.md:34).

**Opportunity:** Reduce page cache to 4 MB (misses go 0.053 → 0.071, README.md:193-194, but each miss is a kernel page-cache read, not a process allocation), shrink ring sizes, don't mlock everything, and the server could match the leader's 1,114 MB while holding similar or more connections.

---

## 3. Which operations dominate CPU time?

From pavlyhalim's cpu-clock profile at 160,000 users (README.md:276-278):

| Operation | % CPU |
|---|---|
| `__inet_lookup_established` (kernel socket hash-chain lookup per packet) | 14% |
| Checkpoint thread | 5.6% |
| SQLite page-cache lookup | 2.8% |
| Author lookups (btree) | 2.2% |
| Feed (btree) | 1.8% |
| JSON escaping | 1.7% |

The kernel socket lookup (14%) is outside our control — it's the kernel's TCP stack. Everything else is tunable.

From system-call counting at 120,000 users (README.md:178-180): 0.23 syscalls/request total — `io_uring_enter` 0.05, `pwrite` 0.095 (including checkpoint thread), everything else ≈ 0.08. This is already extremely low.

**Dominant CPU operations we can optimize:**
1. SQLite B-tree reads (skip bytecode interpreter) — already done by pavlyhalim, ~4% combined for feed + author lookups.
2. JSON escaping — 1.7%, SIMD vectorized (json.h:295-307).
3. Checkpoint thread — 5.6%, nice 19 + batched checkpoints.
4. HTTP parsing — not in the profile (already cheap), but can be tightened.

---

## 4. Which operations dominate memory?

1. **Kernel socket structures (~4.3 KB/conn)** — dominant, ~1.2 GB at 295k. Not reducible by application code; only reducible by closing idle connections (Connection: close) or reducing connection count.
2. **SQLite page cache** — 8 MB default, tunable 4–32 MB. Misses at 8 MB: 0.053/request (each a kernel page-cache read, free memory). At 32 MB: 0.0017/request (pavlyhalim README.md:189-194).
3. **io_uring rings** — 65,536 CQ entries × 16 rings potential, but rings are created on demand (server.c:315-334) and only as needed (README.md:52).
4. **Provided buffer rings** — 4,096 × 1 KB = 4 MB per ring.
5. **Connection tables** — 40 bytes/conn × 65,471 slots per ring ≈ 2.6 MB per ring.
6. **Arenas** — up to 256 MB worst case, normally much less.
7. **mlock'd code** — ~2.1 MB static binary.

---

## 5. Which optimizations are available to us?

All of the following are within the official rules and demonstrated by existing submissions:

| Optimization | Evidence | Risk | Expected gain |
|---|---|---|---|
| Raw io_uring (no liburing) | pavlyhalim sys.h:1-415 | Medium (ABI stability) | Large (0.23 syscalls/req) |
| Direct descriptors (bypass 65k fd limit) | pavlyhalim sys.h:298, server.c:914 | Low | Enables >71k users |
| SO_REUSEPORT ×8 listeners | pavlyhalim server.c:288-289 | Low | Holds ~32k through stalls vs 4k |
| Multishot poll (not multishot recv) | pavlyhalim sys.h:307, README.md:42-44 | Low | Avoids ENOBUFS storm under burst |
| Provided buffer rings | pavlyhalim sys.h:210-234 | Medium (kernel EINVAL workaround needed) | Zero-copy recv |
| Group commit (1 txn/batch) | pavlyhalim server.c:997, db.h:522-533 | Low | Fewer fsyncs, higher write throughput |
| SQLite B-tree cursor reads | pavlyhalim btree.h, db.h:589-595 | High (internal APIs) | ~50k users (295k vs 238k) |
| Custom VFS (batched WAL pwrite) | pavlyhalim db.h:258-436 | Medium | pwrite/req 0.24 → 0.095 |
| Custom page-cache wrapper | pavlyhalim db.h:169-243 | Medium | Skips full-cache truncate scan (1.8% CPU) |
| Background checkpoint at nice 19 | pavlyhalim db.h:82-116 | Low | Avoids 300-430 ms loop stalls |
| WAL + synchronous=NORMAL + wal_autocheckpoint=0 | pavlyhalim db.h:145-147 | Low (spec-mandated) | Durable writes, no per-commit fsync |
| PGO (profile-guided optimization) | pavlyhalim build.sh:42-57, train.sh | Low | Measurable IPC gain |
| mlock of code + data | pavlyhalim server.c:868-891 | Low | Avoids page-fault stalls under memory pressure |
| Memory pre-warming | pavlyhalim server.c:164-186, 294-305 | Low | Avoids first-touch stalls at the wall |
| Custom SHA-256 + HMAC (precomputed key schedule) | pavlyhalim sha256.h | Low | Faster JWT verification |
| SIMD JSON escaping (16-byte vector) | pavlyhalim json.h:295-307 | Medium (GCC vector extensions) | 1.7% CPU → less |
| Static binary, -O3, march=x86-64-v3 | pavlyhalim build.sh:21-34 | Low (portable x86-64-v3) | Better codegen |
| Memory valve (off by default) | pavlyhalim server.c:445-469 | Low | Safety valve against OOM |

---

## 6. Which optimizations are too risky or unlikely to help?

| Optimization | Why risky / unlikely to help |
|---|---|
| **Bypassing SQLite entirely (custom storage)** | Violates rule 3 (README.md:37-38) and rule 5 (no in-memory databases). Disqualifying. |
| **synchronous=OFF / journal_mode=MEMORY** | Violates rule 6 (README.md:49). Writes must be durable before response. |
| **Response / query-result caching** | Violates rule 5 (README.md:44-47). Every request reads SQLite. |
| **Remembered JWT verification** | Violates rule 5 (README.md:46). Verify every token's signature. |
| **Multishot recv on every connection** | pavlyhalim README.md:42-44: caused up to 700,000 ENOBUFS re-arms in 10 s, OOM. Avoid. |
| **mmap of database (mmap_size > 0)** | pavlyhalim README.md:186: every page fetch probes the WAL index; slower than page cache. |
| **512-bit AVX-512 vectorization** | pavlyhalim build.sh:25: on Cascade Lake, 512-bit lowers core frequency for ~0.5 ms. Cap at 256-bit (`-mprefer-vector-width=256`). |
| **Multiple worker threads / processes** | One vCPU; threading adds context-switch and lock overhead without adding compute. Pavlyhalim uses one event-loop thread. |
| **Reading raw DB pages bypassing pager** | Violates rule (master prompt §4.5): "Never read arbitrary database pages as if they were stable application records." Must go through SQLite's pager/WAL/page cache. |
| **liburing wrapper** | Adds a dependency and indirection; raw syscalls (pavlyhalim sys.h) are faster and have no external dependency. |
| **Connection: close on every response** | Defeats keep-alive; forces new TCP handshakes per request, increasing latency and kernel socket churn. Only as a memory-pressure valve. |
| **Aggressive page cache (32+ MB)** | pavlyhalim README.md:191-194: at the wall, every MB is ~240 sockets. 32 MB costs 7,680 sockets. Misses at 8 MB are 0.053/request — each is a kernel page-cache read (free memory). Not worth the socket cost. |

---

## 7. Architectural Advantages and Weaknesses of Top Implementations

### c-iouring-pavlyhalim (top C, 295k)
**Advantages:**
- Complete, integrated stack: io_uring + B-tree + custom VFS + custom page cache + group commit + PGO + mlock.
- Proven at 295,000 users on the exact target box.
- Direct descriptors bypass the fd limit; SO_REUSEPORT ×8 handles stall bursts.
- Read queue with bounded reads per batch prevents ENOBUFS storms.

**Weaknesses:**
- 1,536 MB RAM — highest among top C entries. Memory is the ceiling.
- Complex B-tree cursor code (btree.h:127-516) depends on SQLite internal APIs; schema verification + SQL fallback mitigates but adds code paths.
- 16-ring eventfd design is complex; simpler at lower connection counts.
- B-tree reads are compiled into the same translation unit as sqlite3.c (server.c:8) — ties to a specific SQLite amalgamation version (3.54.1).

### c-iouring-meros (238k, no B-tree reads)
**Advantages:**
- Simpler: standard SQL, no internal SQLite APIs, easier to verify and maintain.
- Still 238,000 users — competitive but not first-place.

**Weaknesses:**
- ~57k fewer users than pavlyhalim, confirming B-tree reads are the biggest single win.

### rust-iouring-kensac (310k, leader)
**Advantages:**
- Highest score. Same io_uring + B-tree class.
- 1,114 MB RAM — best memory efficiency among leaders.

**Weaknesses:**
- Rust toolchain dependency (cargo). Not relevant to our C implementation.

---

## 8. Competitive Positioning for BinaryCore

**Target:** Beat or match 295,000 users (current top C), ideally approach 310,000 (overall leader).

**Strategy:**
1. Implement the full pavlyhalim stack (io_uring, direct descriptors, SO_REUSEPORT ×8, multishot poll + provided buffers, group commit, custom VFS, custom page cache, checkpoint thread, PGO, mlock) — gets us to ~295k.
2. Tune memory below 1,114 MB: smaller page cache (4 MB), no over-mlocking, right-sized rings — this lets us hold more connections before the socket-memory wall.
3. Add B-tree reads with SQL fallback for safety — the +50k user win.
4. Profile and iterate: one variable at a time, preserve correctness.

**Differentiator vs pavlyhalim:** Better memory packing (target < 1,114 MB RAM) and equivalent or better tail latency (target p99 < 200 ms vs pavlyhalim's 117 ms — already good, just preserve it).

**Risk posture:** B-tree reads are the highest-risk, highest-reward item. Implement standard SQL first (phase 1), verify all 42 tests, then add B-tree reads (phase 3) with the SQL fallback path always available.
