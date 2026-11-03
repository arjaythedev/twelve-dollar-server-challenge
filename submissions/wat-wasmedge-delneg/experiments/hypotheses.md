# Hypotheses (TdscWat4, from 3 Nov). APPEND-ONLY: lines are never edited or deleted; a status change is a new line.

Denis asked for this (3 Nov): "try other solutions, getting some data (conclusions)", challenge the current
assumptions, a hypothesis list written and locked before any experiment, results in worklog.md (E53 on).
The ids here (H1, H2...) are this file's. The worklog's older "H1-H4" sections are 5-minute holds, a different thing.
Each entry gives the idea, the assumption it challenges, the expected effect and why, the rule check and the cost to test.
Phase 1 sources: our own README/worklog/app.wat/host.c/build.sh, the top entries' code (rust-iouring-kensac shim.c,
c-iouring-pavlyhalim db.h, mojo-iouring-oh-meh shim.c), LEADERBOARD.md, SQLite 3.54.1's os_unix.c WASI section,
`wasmedge compile --help`, a DeepSeek consult (session 1103-205423-0386, $0.023).
The two survey/compiler subagents could not be started (only Opt can spawn), so that survey was done in-line and kept small.

## Facts gathered in Phase 1 that change the picture
- F1. The scorer's own numbers: df9da40 at 200k used 68 % CPU (19.6k req/s, ~34.7 us/req of the box); 210k and 220k failed
  on p99 at 69-75 % CPU with 0 errors. ~30 % of the CPU was idle while p99 broke, so on the scorer the cliff is not CPU.
- F2. Harness v2 without the spin already costs ~35 us/req of CPU 0 at 240k (h4: busy 98 % = user+sys 70 + softirq 28, of
  which the 10 us spin is ~22 points, at 21.7k req/s -> (98-22)/21.7k = 35 us), about the scorer's 34.7. The SPIN_NS=10000
  calibration matched the scorer's tail by adding CPU the scorer doesn't spend. So "240k is CPU-bound" holds for the
  harness with spin, not necessarily for the scorer. The scorer's tail is more likely the checkpoint stall, longer on its disk.
- F3. Every top entry checkpoints from a second connection on another thread: rust-iouring-kensac (shim.c:118-150, a
  "bulk" PASSIVE on the thread, then writes held and a "tail" PASSIVE so the WAL is fully copied and the next write
  restarts it, shim.c:280-305), c-iouring-pavlyhalim (db.h:105, :498), mojo-iouring-oh-meh (shim.c:554, :605). We are the
  only top-40 entry that fsyncs a checkpoint on the event loop.
- F4. Why we couldn't: SQLite's WASI build has no shared memory (os_unix.c: `unixShmMap 0` under SQLITE_WASI; default VFS
  unix-dotfile). We use "unix-none" with locking_mode=EXCLUSIVE, so the wal-index is in the heap of one connection.
  But xShmMap/xShmLock/xShmBarrier/xShmUnmap are io-method slots a VFS wrapper may fill (public API), and the host can
  map the same physical pages into two wasm linear memories (memfd + mmap MAP_FIXED inside WasmEdge's in-place memory).
- F5. Only likes (15 % per iteration) and posts (2 %) carry a JWT: ~7.8 % of requests (0.17 of 2.17). The HMAC key pads
  are already computed once at start (`$hmac_init`).
- F6. The SQLite part of the merged module has 318 `memory.copy` and 159 `memory.fill` sites (clang inlines memcpy/memset
  as bulk-memory ops); WasmEdge's AOT turns each into a runtime call (README). perf M3: libwasmedge 1.1 %, libc 1.3 %.
- F7. `wasmedge compile` options: --generic-binary (we don't use it, so native CPU), --interruptible (off), --enable-threads,
  --disable-* for proposals, --optimize. No bounds-check switch.

## Hypotheses

H1. A second SQLite connection on a second wasm instance runs the checkpoints, so the event loop never waits for the
  checkpoint's fsyncs. The host starts a pthread running a second WasmEdge VM on the same app.so (its own linear memory,
  its own copy of SQLite's globals, so SQLITE_THREADSAFE=0 stays safe); a memfd is mapped MAP_SHARED|MAP_FIXED into a
  page-aligned slice of each instance's memory and serves as the wal-index; a VFS wrapper around unix-none (glue C, public
  sqlite3_vfs/io_methods API) adds xShmMap (slices of that region), xShmLock (8 process-wide lock words, CAS through host
  imports, per-instance held masks), xShmBarrier (host fence), xShmUnmap (no-op). locking_mode NORMAL. Protocol after the
  Rust #1: WAL >= T frames -> the checkpointer does a bulk PASSIVE; while the frames written since exceed TAIL it does
  another; then the loop runs one small inline PASSIVE between batches (nothing open, so it copies everything), and the next
  write restarts the WAL.
  Challenges: "WASI has no shm, so no second connection"; "nothing within the rules takes the fsync off the loop".
  Expected: the ~140 ms stall every ~2 s becomes a few ms (tail: tens of pages + two syncs; F-floor ~0.8 ms per sync here);
  p99 at 210-220k back under ~500 ms; on the scorer (F1, ~30 % idle) the next limit moves to memory/CPU, maybe 240-260k.
  Costs on the loop: NORMAL locking (a read-mark lock and header check per transaction, through host calls), ~1-3 %.
  Rules: SQLite in-process (same process, second thread: rule 1 allows threads), stock amalgamation, public VFS API only,
  synchronous=NORMAL unchanged and SQLite does every sync itself, no kernel tuning. C grows (~80 lines glue + ~50 host)
  for the experiment; a WAT port of the VFS can follow if it wins. Cost: high (1-2 days of agent time compressed: 2-3 builds).
H1b. Fallback for H1 if two VMs in one process fail: fork() in the embedder before _start, child runs the checkpointer
  role; the shared pages are a MAP_SHARED mapping placed into wasm memory before the fork. Same expected effect, more risk
  (fd inheritance, COW, two processes counted by the OOM killer). Only if H1 is blocked.
H1c. If H1's inline tail still costs (e.g. the scorer's fsync floor is high): hold writes and let the checkpointer do the
  tail too (as the Rust #1), so no sync ever runs on the loop. Needs write-deferral in the WAT's group commit. Only if H1's
  tail shows in the p99.
H2. Second harness calibration: no spin, the disk slowed instead (systemd IOWriteIOPSMax / IOWriteBandwidthMax on the
  server unit, cgroup io.max), fitted so df9da40 holds 200k and fails 210k as on the scorer. Challenges "the harness matches
  the scorer" (F2: the spin adds CPU the scorer doesn't spend). Expected: tells whether the scorer's limit is the stall
  (then H1's gain is larger than the spin harness shows) or CPU. Rules: harness only. Cost: medium (3-5 2-minute probes).
H3. Measure the checkpoint fsync cost against the number of dirty pages on Synteq's disk (floor and slope), with the same
  file and page pattern. Challenges DeepSeek's worry that the cost is a fixed device floor (then H1's inline tail would be
  as slow as today's checkpoint). Expected: ~0.8 ms floor + ~65 us/page (worklog E51 numbers, to be confirmed). Cost: low.
H4. Checkpoint by wall time or by dirty pages instead of by frames (DeepSeek: the index grows over 5 minutes, so a fixed
  frame count is non-stationary). Without H1: E11/E51 already found smaller, more frequent checkpoints worse (fsync floor +
  per-page cost); no new reason to retest. With H1: the checkpointer's cadence (T, TAIL) is the knob; tuned inside H1.
H5. SQLite's 318 memory.copy / 159 memory.fill sites each become a WasmEdge runtime call. Building SQLite with
  -mno-bulk-memory makes clang call wasi-libc's memcpy/memset (wasm loops, compiled by the AOT like the rest), or inline
  small fixed-size copies as loads/stores. Challenges "SQLite's C build is as good as it gets" (E18/E19 only tried -O3 and
  ObjC's defines). Expected: ~0.2-0.6 us/req (perf F6 bounds it near 2 % of CPU). Rules: compile flags of stock SQLite: OK.
  Cost: low (one build, A/B).
H6. More binaryen passes on the merged module (-O4, --converge, -O3 twice, --inlining-optimizing with higher limits)
  before the AOT. Challenges "wasm-opt -O3 then LLVM -O3 already catches everything". Expected: 0-2 %. Cost: low.
H7. Offload the JWT check (WasmEdge wasi_crypto plugin, or a native HMAC host import). Challenges "JWT in WAT is a cost".
  Expected: negligible: only ~7.8 % of requests carry a JWT (F5), ~3 SHA-256 blocks each with the pads precomputed, so even a
  2x faster native SHA-256 saves < 0.1 us per request on average. The Xeon 8260 has no SHA-NI. A plugin also has to be pinned
  and installed. Verdict expected by arithmetic plus one microbench of the WAT SHA-256; no build unless the microbench
  surprises. Cost: low.
H8. "Compression": HTTP response compression (gzip/deflate) of /feed. Challenges nothing we assumed, but Denis asked. Needs
  Accept-Encoding from k6 (to check: bench/load.js sets no headers; k6's default) and costs deflate CPU (~10+ us for a ~6 KB
  feed in wasm) against fewer TCP segments/ACKs. Expected: loses on CPU, or isn't allowed if k6 doesn't ask (rule 4/7). Other
  readings of "compression" (SQLite page density / smaller page size, WAL frame size) are H13 / H1. Cost: low (check + estimate).
H9. host.c and the glue in Rust instead of C. Challenges "C must stay C". Expected: no speed change (the host runs ~1-2 % of
  the CPU: ring_go copies; the same LLVM code either way), no fewer lines, plus a pinned Rust toolchain in install.sh. Verdict
  by profile share and line count. Cost: low (analysis), a port only if Denis wants it for its own sake.
H10. Compiler output: our WAT's hot routines (SHA-256 block, JSON string escape, integer formatting, request-line parse,
  record decode) against clang -O3 / rustc opt-level=3 wasm of the same routines, after wasm-opt and the AOT: patterns the
  compilers use that we don't (loop shapes, br_table, select, local reuse, SIMD). Challenges "hand-written WAT is as fast as
  compiled" (E4 measured parity on the whole server, not per routine). Expected: < 0.5 us/req (our own code is ~9 of ~28 us,
  much of it SQLite's C). Cost: medium; done as a small microbench on opt-box.
H11. /feed's 20 author lookups (sqlite3BtreeTableMoveto 5.8 %, mostly these): seek them in ascending user-id order on one
  users cursor, so neighbouring seeks share the upper levels, or skip the seek when the cursor is already on that id.
  Challenges "the B-tree read path is done". Expected: ~0.5-1 us per /feed (~46 % of requests). Rules: same B-tree read
  path, nothing cached across requests. Cost: medium (WAT change + 42/42).
H12. NORMAL locking's CPU cost on the loop (part of H1): measured on its own by running H1's build with the checkpointer
  disabled. Cost: low once H1 builds.
H13. Bigger page size (VACUUM at start into 8 or 16 KiB pages) so the likes autoindex has 2-4x fewer leaves and a
  checkpoint writes fewer distinct pages. Challenges "~2,400 leaves per checkpoint is fixed". Rule risk: rule 3 ("schema and
  data are fixed") and rule 10 (healthy within 60 s; a 243 MB VACUUM on 1 vCPU in wasm may take longer); also every WAL
  frame grows 2-4x. Not tested without Denis; if H1 fails, write the case for him.
H14. io_uring registered ring fd (IORING_REGISTER_RING_FDS + IORING_ENTER_REGISTERED_RING). Expected negligible (one
  enter per loop pass, thousands per second, not per request). Verdict by the S12 census numbers. Cost: low (analysis).
H15. Socket memory at 240k+: on the scorer 240k died with "connection refused" (an OOM kill per E39). Challenges "memory
  is fixed per connection". With H1 the backlog that blows up TCP memory during stalls should go; a smaller SO_RCVBUF on
  the listener (inherited by accepted sockets, a setting inside the process) would cap what a stalled socket queues.
  Expected: matters only past H1; test if H1's 5-minute holds at 240k+ fail on memory. Cost: low (env knob).
H16. Wasm threads / shared memory in WasmEdge with SQLITE_THREADSAFE=2 and both connections in one instance. Rejected in
  favour of H1: needs a threads build of wasi-libc and SQLite with mutexes on every page allocation (DeepSeek: pcache1's
  global list has no mutex with THREADSAFE=0), more CPU per request; H1 gets the same with two instances. Not tested.
H17. SQLite compile-time options with a reason ObjC's set (E19) didn't have: SQLITE_OMIT_SHARED_CACHE (drops the
  shared-cache table-lock checks on every B-tree access), SQLITE_DEFAULT_LOOKASIDE tuning, SQLITE_MAX_EXPR_DEPTH=0,
  SQLITE_OMIT_DEPRECATED, SQLITE_DQS=0. Expected: 0-2 %. Rules: compile options of stock SQLite: OK. Cost: low, bundled
  with H5 as one A/B.
H18. PGO for the wasm (clang profile of SQLite + LLVM AOT). Untested: wasi-sdk has no profile runtime for wasm32-wasip1 and
  WasmEdge's AOT takes no profile; too costly for this round.
H19. Likes through the B-tree layer (write path): skip SQL's bytecode for the like's exists check and insert. Expected
  small (~7 % of requests, ~3-5 us each -> ~0.3 us/req), and inserting through internal B-tree calls skips constraint and
  index maintenance that SQL does for us (two indexes): risky against rule 4 and "public plugin APIs". Not tested.
H20. "240k is pure capacity" (TdscWat3) on the scorer: F1/F2 say otherwise; H1 + H2 test it. No separate experiment.

--- LIST LOCKED 2026-11-03T19:01Z ---

Ranking by expected impact / cost (highest first). Order of work: H3, H1 (+H12 inside it), H2, then the cheap CPU A/Bs.
1. H3  (low cost; decides whether H1's tail is cheap)
2. H1  (largest expected impact: the stall the scorer fails on)
3. H12 (comes free with H1's build)
4. H2  (medium; tells how much of H1's gain the scorer would see)
5. H5 + H17 (one build, low cost, a few %)
6. H6  (low cost, 0-2 %)
7. H11 (medium, ~1 us on /feed)
8. H7, H8, H9, H14 (verdicts by arithmetic + one check each)
9. H10 (medium, small expected)
10. H15, H1c, H1b (only if H1's results call for them)
11. H13, H16, H18, H19 (not tested: rule risk or cost; reasons above)
