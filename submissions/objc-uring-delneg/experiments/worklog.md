# ObjC #154: B-tree reads on stock SQLite (worklog, 2026-10-31)

Local numbers only (Synteq 8 vCPU Xeon 8260 box, kernel 6.8.0-147; server in Docker on 1 CPU / 2 GB, kload
`bench/load.js` model on 4 other CPUs, host network). Not the organizer's numbers.

## Setup

- Head `d02f205` (SQLite patch removed). On this box's 6.8.0-147 kernel it exited at startup:
  `setup_buf_ring: Invalid argument` (Ubuntu's inverted reserved-words check, Launchpad #2162843; the grader ran
  6.8.0-117). Fix `39bcc9d`: register the buffer ring by hand and retry with `resv[0]=1`. 42/42.
- Builds on the box host with the pinned clang 23.1.4 (official tarball, sha256 checked), libobjc2/liburing reused from
  the earlier build; the server runs in `ubuntu:24.04` with the build mounted.

## Study: the B-tree entries (#217 C, #191 V, #224 Mojo, #153 Lean)

- All compile a C file that `#include`s the stock amalgamation, so SQLite's file-private B-tree calls
  (`sqlite3BtreeCursor`, `TableMoveto`, `IndexMoveto`, `First/Last/Next/Previous`, `PayloadFetch`) are reachable.
  Root pages come from `sqlite_schema` at startup; #217 also checks the schema cookie per call.
- #191/#217 (no invariants): walk `posts_created_at_id_idx` from the first entry (it is DESC, so newest first) for
  the 20 ids, one `likes_post_id_idx` range scan for the like counts when the ids sit in a narrow span, then a rowid
  seek per post (#191 steps back with `Previous` when ids are consecutive) and a rowid seek into `users`.
- #224/#153: the feed is the last 20 rows of `posts` by rowid when ids are dense and `created_at` never decreases with
  id; checked at startup with SQL and after every insert; a boolean flips to the SQL plan for good if a check fails.
  #224 opens cursors once per batch.
- Reads run in the batch's transaction; #191/#224 call `sqlite3BtreeBeginTrans(bt, 0, 0)` themselves.
- Other ideas: #224 VFS `unix-excl` (WAL index in heap), checkpoint thread; one-shot accepts (#224) instead of multishot.

## Experiments

| # | Change | Result | Kept |
|---|---|---|---|
| 0 | Baseline `d02f205` + buffer-ring fix (A1), 300k users × 3 | 28.70 / 29.14 / 27.54 µs/req, CPU 88–92 %, 31.3–31.8k req/s; pass 2 of 3 (one p95 717 ms) | — |
| 1 | B-tree reads for `/feed` and `/posts/:id` (`src/td_btree.c`, B1) | wrk `/feed` 23.1–25.9 → 18.3–19.8 µs/req (−21 %), instructions 127k → 85k; `/posts/:id` 10.2 → 9.55 µs (−6.5 %), 29.1k → 25.9k ins. Diff test vs `--btree=0`: 711 requests (reads, 404s, writes, likes, unicode/escapes), 0 differences | yes |
| 2 | Write-only holds: during a WAL hold, reads are answered; a write and every later request on its connection stay at the front of the batch (B3) | 300k users: hold p95 19.8 / 23.4 ms, p99 47.9 / 56.1 ms against B1 62.2 / 288.8 ms; CPU 27.06 / 27.47 µs/req against 26.77 (+1–2 %, batches after a hold are smaller). Diff test with `--hold=8` (214 holds), pipelined writes and `Connection: close`: 0 differences in 1,191 requests | yes |
| 3 | WAL hold at 16,384 frames instead of 8,192 (`--hold=16384`) | 300k users: 28.43 / 26.47 µs/req against 26.32 / 27.69 (noise is about ±3 % between runs); p99 75 / 54 ms against 51 / 69 | no |
| 4 | WAL hold at 4,096 frames (`--hold=4096`) | 300k users: 26.29 / 26.46 µs/req against 26.11; p99 50 / 50 against 54 ms | no |
| 5 | SQLite page cache 16 MB instead of 32 MB (`--cache=16384`; smaller hash for SQLite's per-commit truncate scan) | 300k users: 26.51 / 26.17 µs/req against 25.81 | no |
| 6 | Batching window 2 ms / 256 completions instead of 1 ms / 128 (`--bwait=2000 --batch=256`) | 300k users: 28.09 / 27.52 µs/req against 27.28 / 26.98; p99 71 / 93 against 43 / 56 ms | no |
| 7 | Read cursors stay open for the batch (closed before COMMIT); the newest post found with `sqlite3BtreeLast` (B5) | wrk: `/feed` 85.4k → 84.8k instructions, `GET /posts/:id` 26.0k → 22.9k (−12 %); cycles too noisy to read. Diff tests 0/1,191 (also with `--hold=8`) | pending |

Profile of B1 `/feed` (wrk): kernel 57 %, server 41 %. Kernel `clear_page_erms` 4.2 % is `tcp_sendmsg` →
`skb_page_frag_refill` (Ubuntu zeroes allocated pages, init_on_alloc): scales with bytes sent.

E1 with the bench/load.js model, 300k users, interleaved: B1 26.54, 26.04 µs/req (CPU 84 %, 82.5 %; p99 268, 171 ms)
against A1 28.49 (90 %, p99 272 ms). Kept.

Hold log (`TD_CKLOG=1`, B2 = B1 + counters), 300k users: 102 WAL holds in ~4.5 min, one every ~2.7 s, each stopping
every request: p50 51 ms, p90 116 ms, max 206 ms, plus one 250 ms timeout. 1,083 receives ended in `ENOBUFS`.

From E3 on the test server runs with `--security-opt apparmor=unconfined`: Docker's default AppArmor profile put
`apparmor_socket_sendmsg/recvmsg` and `aa_inet_msg_perm` at 4.4 % of CPU, which a plain systemd service (the grader)
does not pay. B3 went from 27.06 / 27.47 to 26.32 / 27.69 µs/req.

Profile of B3 at 300k users (perf, 15 s of the hold): kernel 72 %, server 24 %, libc 4 %. Top server symbols: feed 6.4 %,
feed JSON 2.4 %, `sqlite3BtreeIndexMoveto` 1.9 %, `GET /posts/:id` 1.8 %, `TableMoveto` 1.6 %, `onCQE` 1.6 %,
`pcache1TruncateUnsafe` 1.4 %, flush 1.3 %; libc `memmove` 2.4 %. Kernel: `__inet_lookup_established` 6.0 % (600k
loopback sockets in the established hash), TCP send/ack. `pcache1TruncateUnsafe` is SQLite's own commit path
(`pager_end_transaction` → `sqlite3PcacheTruncate`): after `balance_nonroot` rekeys a page to the pending-byte page
number, the next commit scans the whole page-cache hash. That is what the removed patch avoided; stock SQLite pays it.

Line profile of `/feed` (debug-info build, saturated wrk): inside the feed function, SQLite's `sqlite3BtreeTableMoveto`
binary search (findCell + varint key per probe) is ~19 % plus ~9 % varint decoding: 20 author seeks into `users` per
feed, each from the root (TableMoveto only shortcuts the same key, the next key, or a cursor at the last row). JSON
output (`feed_sink`) 10 % of the samples, mostly the 8-byte escape scan. `likes_count` and `getCellInfo` (SQLite's cell
parse per index entry) about 9 %.

Validation of `1fe6941` (B3): 300k users with a 5-minute hold pass (26.54 µs/req, 84 % CPU, p95 20 ms, p99 52 ms,
0 failed). 340k (2-minute hold) pass at 95 % CPU, p95 346 / p99 502 ms. 370k fail at 98.5 % CPU (p95 2.1 s), 0 errors and
no refused connections: the local knee is CPU, about 340k on this box.

## E8: JSON escaper that copies while it scans (B6 = B5 + E8), dropped

`td_buf_json_str` tested 8 bytes, then copied runs with `td_buf_put`; B6 stores each clean 8-byte block as it tests it
and goes byte by byte through a block that holds an escape. Same output as the old escaper on 2M random strings (ASan
clean), 42/42, B-tree vs SQL diff 0 of 1,191 (difftest now also sends control bytes and many quotes). wrk `/feed`,
4 interleaved rounds: B5 84.59–84.65k instructions per request, B6 86.17–86.28k (+1.9 %); cycles noisy (52.8–58.3k
vs 54.7–59.7k). The stores per block cost more than the one `memcpy` per run. Reverted.

Not tried, rule question for Opt: the `pcache1TruncateUnsafe` cost (1.4 %) could be avoided without editing SQLite
through `sqlite3_config(SQLITE_CONFIG_PCACHE2)`, a wrapper around pcache1 that parks `balance_nonroot`'s swap page
under a free key just above the cache's largest key instead of the pending-byte page number. It is public API, but
it does what the removed patch did, and #158 lost its row over a pcache change, so it needs Arjay's word first.

## E7 result: users A/B, B5 vs B3 (300k users, 2 interleaved pairs)

B5 24.27 / 25.95 µs/req (CPU 77.0 / 82.3 %, hold p99 42 / 120 ms), B3 25.09 / 25.75 (79.6 / 81.7 %, 47 / 93 ms). Mean
−1.2 %, inside the run-to-run noise; instructions say B5 is cheaper (`GET /posts/:id` −12 %, `/feed` −0.7 %). Kept, and
measured again together with E9 (chain-u8, 3 more pairs against B3).

## E9: header end found by stepping memchr('\r') instead of memmem (B7 = B5 + E9)

`memmem(p, n, "\r\n\r\n", 4)` was 0.65 % of the 300k profile (glibc's general memmem). `find_crlf2` jumps from one
`\r` to the next with memchr and checks the next three bytes: the same first match (5M random strings against memmem,
ASan clean). 42/42, diff 0 of 1,191. wrk `GET /posts/1`, 4 rounds: B5 23.24–23.26k instructions, B7 23.03–23.07k
(−0.9 %); cycles noisy. Kept.

## E7+E9 users A/B (chain-u8, 300k users, 3 interleaved pairs, B7 vs B3)

B3 27.24 / 27.90 / 26.23 µs/req (hold p95 27.0 / 22.1 / 16.5 ms, p99 83 / 47 / 48 ms); B7 27.36 / 26.96 / 25.85
(p95 29.1 / 21.1 / 15.8, p99 74 / 66 / 60). Mean −1.5 % CPU, tails mixed. The box drifted ~2 µs between chains, more
than the effect. `--cache` (E5's flag) taken out again; it was never kept.

From 1 Nov the bar for a push (Opt): clearly lower p95/p99 at load, with at most ~2 % more µs/req, over 2+ runs, 42/42.
So the next A/B runs at the knee (340k), where the tails come from CPU.

## E10: 340k A/B, B8 (= 4b18d6d: E7 + E9) vs B3 (1fe6941); E11: checkpoint pass every 2000 frames (`--ckpt=2000`)

At 340k the run is a closed loop at the CPU limit: response time is about N/X − think time, so a 1 % capacity shortfall
turns straight into latency and runs come out bimodal (they keep up or tip over). Hold p95/p99 ms:

| run | B3 | B8 | B8 `--ckpt=2000` |
|---|---|---|---|
| 1 | 26.19 µs, 93.5 %, 416 / 766 | 25.16 µs, 90.4 %, 32 / 95 | 23.78 µs, 85.5 %, 31 / 144 |
| 2 | 26.60 µs, 95.0 %, 140 / 545 | 26.12 µs, 91.9 %, 979 / 3,064 FAIL (steal 74 ticks vs ~35) | 25.47 µs, 91.3 %, 183 / 338 |
| 3 | | 26.35 µs, 93.5 %, 553 / 1,028 | |
| 4 | | 25.87 µs, 92.8 %, 130 / 222 | |

TCP retransmits and timeouts are the same in every run (10–20 SYN retransmits). `--ckpt` is the number of new WAL frames
between passive checkpoint requests to the checkpointer process; 250 was chosen when checkpoints still ran inside the
loop (shorter stalls). In the child, each pass costs a WAL read, page writes and two fsyncs in the same 1-CPU cgroup,
and a longer interval copies a hot page once instead of once per pass. chain-u11: `--ckpt=4000`, B8, `--ckpt=2000`.

## E11 result and push: fac5f79 (B9 = B8 + `--ckpass=2000`)

More 340k runs: B8 26.10 / 25.84 (p95 432 / 35); `--ckpt=2000` 24.88 / 26.26 (p95 88 / 440); `--ckpt=4000` 24.37 /
25.73 (p95 41 / 202). The interval became its own flag, `--ckpass` (default 2000); `--ckpt` stays 250 for the inline
fallback. 42/42; diff 0 of 1,191, also with `--hold=40` (54 holds).

Final interleaved A/B at 340k (chain-u12), B9 vs B3 (`1fe6941`, the pushed head), µs/req, CPU, hold p95 / p99 ms:
pair 1 25.50, 91.6 %, 41 / 117 vs 27.01, 96.9 %, 61 / 165; pair 2 26.42, 94.9 %, 55 / 112 vs 27.33, 97.9 %, 161 / 251
(steal 98 ticks in that B3 run); pair 3 25.53, 91.7 %, 52 / 212 vs 26.97, 96.8 %, 71 / 216. All pass. Lower p95 and p99
in every pair and −4.7 % µs/req: meets the 1 Nov bar. Pushed.

## 1 Nov, after the merge: the accept side (PR #154 scored 280k at 1fe6941; 290k/300k/320k: ~77 % "connection refused")

What the grader's notes say: the same ~77 % refused shape was seen for 10067c1 at 260k+, and for asm #207 it came with
"app cgroup memory down to 0", i.e. the app died; a dead process leaves no listener, so every later connect is refused.
~77 % errors is what a 60 s ramp + 120 s probe gives if serving stops near the end of the ramp. Mojo #224 passed a 290k
probe, so the load side reaches past 290k; #233 (790 MB) and Lean #153 also collapse to refused at 280k+.

Checked on Synteq (fac5f79 + TD_STATS diagnostics, B10):
- RLIMIT_NOFILE: main() raises soft to hard; under systemd LimitNOFILE=65535 that is 65,535 = slots per ring (the
  kernel caps a fixed-file table at RLIMIT_NOFILE). Accepted sockets never enter the fd table.
- listen(65535) is capped by somaxconn (4096 stock).
- 8 rings × 65,535 fixed files, high-water 61,439 per ring: 340k uses 6 rings.
- CQ 16,384 entries per ring: at 340k no pass ever saw IORING_SQ_CQ_OVERFLOW, 0 CQEs dropped.
- Accept errors at 340k: 5, all -ECANCELED (the high-water hand-off); multishot accept is re-armed after any ending.
- No conntrack on the grader (no UFW); the server uses no ephemeral ports.
- 300k, 340k pass locally; 370k fails on CPU with 0 errors. The refused collapse does not reproduce on this 16 GB box.

Memory at 340k users (MEMSTAT, 30 s into the hold): app cgroup anon 168 MB, file 50 MB, kernel 552 MB (slab 545:
sock_inode, dentry, filp, io_kiocb). Outside the cgroup the kernel's TCP slab holds 768 MB for the 340k server
sockets (2.26 KB each; on 6.8 an accepted socket's tcp_sock is allocated in softirq and not charged to the app).
Idle-connection test (100k): our process adds 78 B per idle connection (TDConn) and ~130 B after the first request.
On the 2 GB grader the app peaked at 1,245 MB at 280k; with ~2.3 KB/connection of TCP slab outside the cgroup that is
about the whole box, so the likely death is the global OOM killer (largest process) or our own exit(1) on an
io_uring wait error (-EBADR when the kernel cannot queue overflowing CQEs, -ENOMEM). Not confirmed: needs the grader's
kernel log / memory.stat for a 290k probe.

E12: io_uring wait errors are no longer fatal (logged; back-off 1 ms if they repeat). TD_STATS=1 prints a line a
second: live connections, accept errors, CQ-overflow passes, dropped CQEs, ENOBUFS.

E13: SQLite's pcache1 behind SQLITE_CONFIG_PCACHE2 (allowed since 31 Oct, like #233): skips xTruncate when no page at or
past the limit can be cached (balance_nonroot's pending-byte parking otherwise makes the next commit walk the whole
hash). TD_PCHECK=1 verifies every skip against pcache1's hash: a 150k users run never aborted, integrity_check ok
(514k posts, 2.1M likes); diff vs --pcache=0 0 of 1,191. Write-only wrk (70 % likes, 30 % creates): with the old PGO
profile no gain (the profile's indirect-call promotion inlined pcache1Fetch; the wrapper's fetch went out of line,
pcache1Fetch 4.4 % against pcache1TruncateUnsafe 5.3 % without the wrapper); trained with the wrapper (pgo/train.m
installs it), 4 rounds: 20.5 / 19.9 / 18.0 / 20.2 µs/req against 20.1 / 20.2 / 21.6 / 22.5 with --pcache=0.

E12+E13 at 340k (B12 = fac5f79 + E12 + E13, PGO trained with the wrapper), interleaved with B9 (fac5f79), µs/req, CPU,
hold p95 / p99 ms: B12 25.42, 90.7 %, 346 / 553; 24.75, 88.0 %, 528 / 1,155 FAIL; 26.26, 93.6 %, 403 / 791. B9 25.35,
90.8 %, 127 / 399; 25.44, 91.3 %, 92 / 222; 26.23, 93.8 %, 218 / 350. Same binary, wrapper off (`--pcache=0`, B12n) vs on:
off 26.48, 95.0 %, 97 / 173 and 25.51, 91.4 %, 179 / 297; on 25.61, 88.8 %, 1,630 / 5,538 FAIL (cgroup 737 → 1,061 MB
2.5 min in with RSS flat: socket buffers piling up behind a stall). Four wrapper runs, all with worse tails, two fails;
CPU per request the same. Cause not found (every skipped truncate verified at 150k). E13 dropped; E12 kept (no effect
locally, it only matters when the kernel reports an error).
Fifth wrapper run (u14-b12-340k-2): 24.75 µs, 88.9 %, 42 / 210, pass. Wrapper: 2 fails in 5; without: 0 in 5.
