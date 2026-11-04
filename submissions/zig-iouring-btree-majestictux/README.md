# zig-iouring-btree-majestictux

Feed API on a single io_uring event loop with SQLite in-process and group commit per
completion batch. **Serves directly on `HOST:PORT`** (no Nginx).

- Language: Zig 0.17.0 (pinned in `install.sh`, SHA-256 verified)
- SQLite: 3.54.1 amalgamation (pinned, SHA-256 verified), compiled from `src/sqlite_bt.c`
- Base: `zig-iouring-unorig-username` (same repo, MIT), ported to Zig 0.17 and merged with a
  B-tree read layer adapted from `c-iouring-shivanshthapliyal`'s `sqlite_bt.c` (same repo, MIT),
  with the leaf-cell like counter following `rust-iouring-kensac`'s shim (same repo, MIT)

## What changed vs the 235k Zig entry, and why

The best Zig entry (235,000 users, 32 µs/req) lost to the top 10 (275–310k, 25–28 µs/req)
mainly in the database read path: it reads through the SQL layer while nine of the top ten
read through SQLite's own B-tree cursors. This entry keeps the original's proven event loop
design and fixes that, plus the loop-level items the top entries share:

**Read path (the big one)**
- `/feed` and `/posts/:id` read via B-tree cursors (`sqlite3BtreeCursor/TableMoveto/
  IndexMoveto/First/Next` + manual record decode) inside the batch transaction — no SQL
  parser, planner, VDBE, or `sqlite3_column_*` per request. Writes stay on prepared SQL.
- Feed plan: walk `posts_created_at_id_idx` for the 20 newest (created_at, id); load post
  rows in **ascending id order** (cursor fast path stays on one leaf); authors likewise;
  like counts from **one range walk of `likes_post_id_idx`** when the id span is < 512,
  counting leaf cells directly, per-post seeks otherwise.
- Schema layout is verified at startup; any surprise (missing author row, overflow page,
  unexpected type) falls back to the original SQL plans (the original `feedRange` plan,
  then the reference query), so JOIN semantics are preserved exactly.
- Local in-process measurement on the seed DB: B-tree feed 6.6–7.9 µs vs 12.7–15.2 µs for
  the best SQL plan and ~20–21.5 µs for the reference query; `/posts/:id` 0.8–1.0 µs vs
  1.35–1.38 µs SQL. End-to-end (epoll twin of this exact handler/DB code, 1 core, official
  k6 mix): 33.9 → 28.8 µs/req median (−15%).

**SQLite build / config**
- `-O3 -march=native` (the build runs on the benchmark box) instead of `-O2 -march=x86-64-v2`,
  with GCC PGO whose training run also exercises the B-tree reads.
- `mmap_size=0` (was 1 GiB): with mmap, every page fetch pays a WAL-index lookup and header
  re-parse; the top entries all run mmap off. Page cache 32 MB (was 16 MB) — the measured
  knee for this working set; the extra RAM is worth more as connection headroom.

**Event loop (from the 0.17 port)**
- Hand-written io_uring ABI via `syscall()`/`mmap()` (Zig 0.17 removed the std API this used
  before); encodings verified field-by-field against Zig 0.15.2's std implementation.
- Bounded reads after a burst: connections whose multishot receive ends are re-read with
  single-shot `MSG_DONTWAIT` reads, at most half the buffer ring per pass, oldest first —
  instead of re-arming every multishot at once and draining the buffer ring again.
- 8 `SO_REUSEPORT` listeners (accept queues total ~32k instead of 4k), 512 single-shot
  accepts in flight, spread over the listeners with fewest in flight.
- Send buffers recycled through a LIFO pool instead of malloc/free per response.
- Checkpointer is a helper process at nice(19); batching wait 2 ms / 64 completions.
- Two type errors in the 0.17 port as written are fixed (`bind_text` many-pointer,
  ring-flags cast); the port's own unit tests (SIMD JSON escaper, HMAC vs std) pass.

**Kept from the original, deliberately**
- Group commit: one deferred transaction per completion batch; no response is sent before
  its batch's `COMMIT`; a failed commit closes the batch's connections.
- JWT verified on every request (own SHA-256/HMAC, ipad/opad states precomputed once —
  x86-64-v2 has no SHA-NI; the challenge box is the same class).
- Checkpoint helper *process* (not the top entries' thread + write-hold): changing the
  checkpoint topology is the one top-10 technique not adopted. Under the official write mix
  (~2% posts) the WAL stays in the KB range in local tests; the 8,192-frame write gate is
  the known remaining gap to #1, along with its WAL-coalescing VFS (~0.2 µs/req, skipped
  for durability conservatism).

## Verification done locally
- `test/run.sh` 42/42 via an epoll twin that shares this handler/DB code byte-for-byte
  (io_uring is blocked in the dev sandbox, so the uring binary itself is compile-verified,
  unit-tested, and smoke-started against the real seed DB — `bt_init` resolves the schema,
  listeners bind, it stops only at ring creation).
- Official `bench/load.js`, 2,000 users, real think times, 1 core: 19,687 requests, 0
  failures, 100% checks.
- Write-heavy stability (like/post probabilities ~10× the official mix): 472,945 requests,
  0 failures; `PRAGMA integrity_check` ok; all writes present afterwards.

## Projected score
Applying the measured −15% CPU/req to the original entry's published 32 µs/req gives
≈27 µs/req, i.e. ≈26,800 req/s and roughly **276,000 users held** — top-10 territory,
from 235,000. The loop improvements above (bounded reads, REUSEPORT, send pool) target
stall recovery and the memory wall rather than steady-state CPU, so they may add more on
the real box; they are not in the projection.
