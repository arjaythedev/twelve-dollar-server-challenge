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

Profile of B1 `/feed` (wrk): kernel 57 %, server 41 %. Kernel `clear_page_erms` 4.2 % is `tcp_sendmsg` →
`skb_page_frag_refill` (Ubuntu zeroes allocated pages, init_on_alloc): scales with bytes sent.

E1 with the bench/load.js model, 300k users, interleaved: B1 26.54, 26.04 µs/req (CPU 84 %, 82.5 %; p99 268, 171 ms)
against A1 28.49 (90 %, p99 272 ms). Kept.

Hold log (`TD_CKLOG=1`, B2 = B1 + counters), 300k users: 102 WAL holds in ~4.5 min, one every ~2.7 s, each stopping
every request: p50 51 ms, p90 116 ms, max 206 ms, plus one 250 ms timeout. 1,083 receives ended in `ENOBUFS`.
