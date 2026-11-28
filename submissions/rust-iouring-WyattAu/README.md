# rust-uring (WyattAu)

Rust, no framework: a single-thread io_uring event loop reading SQLite's
B-trees directly through mmap, writes through the bundled SQLite.

| | |
|---|---|
| Language | Rust 1.94.0 (official standalone tarball, sha256-checked by `install.sh`) |
| Crates | pinned via `Cargo.lock`: rusqlite 0.32 (bundled amalgamation 3.54.x, writes only), io-uring 0.7, sha2/hmac/base64 |
| Transport | [uring-kit](https://github.com/WyattAu/uring-kit) @ `a811d22a1d4500d065e5e7d26402b430690165b7` (estate crate: registered-buffer reads, vectored writes, completion-driven accept) |
| SQLite reads | mmap B-tree walk: table seeks, right-spine backward walk for the feed, in-order index scan for like counts — NO SQL engine on the read path |
| SQLite writes | rusqlite, group commit per pass (BEGIN; INSERTs; COMMIT; responses flush after COMMIT) |
| Nginx or direct | **Direct** (serves HOST:PORT itself) |
| Needs | Linux 6.1+ (io_uring DEFER_TASKRUN-era features). Reads go into per-connection unregistered buffers — no RLIMIT_MEMLOCK requirement, runs under a stock systemd unit. SHA-NI optional — HMAC auto-detects hardware support at runtime |

## The read path (the interesting part)

`GET /feed` walks the posts table's rightmost B-tree spine backward — the
newest posts are the table tail (`id DESC` is order-equivalent to
`created_at DESC, id DESC` for SPEC-legal inserts, checked at runtime with a
fallback to the index walk). Authors resolve by rowid seek; like counts come
from a rightmost walk of `likes_post_id_idx`. The SQL bytecode engine, the
query planner, and the per-value boxing never run on reads.

The B-tree walker is a pure-Rust crate (`sqlite-btree`): mmap pages,
checksum-chain-verified WAL frame index, snapshot reads at the pass
boundary. It is validated by a differential oracle: after random write
batches, checkpoint-restarts, TRUNCATE checkpoints, and SIGKILL-recovery,
every read (seeks, counts, feed order) must equal SQLite's own answers on
the same bytes (12-round synthetic corpus + the real 238 MB seed).

## The hot path

Requests parse against the connection buffer in place (no per-request
buffer copies); authed writes and JWT usernames land in a per-pass arena
that retires after each commit. `GET /posts/:id` emits the SPEC JSON
straight from mmap views in one reader pass (post row, author rowid seek,
like-count index walk). Responses flush as two iovecs — header block and
body — with partial-write progress tracked across write completions.
HMAC-SHA256 verification uses the SHA-NI instructions when the CPU has
them (runtime detection); the fallback is the portable kernel.

## Durability and correctness

- WAL with `synchronous=NORMAL`, autocheckpoint disabled; checkpoints are
  explicit between passes (TRUNCATE), and the reader re-bases its frame
  index on salt changes (checkpoint-restart detection).
- Rule 5 honored: every request's rows are read from the file while served
  (mmap pages, no row caches); the index scanner emits interior-page
  entries exactly once (verified against SQLite on 2M-entry runs).
- Rule 6 honored: writes batch in one transaction per event-loop pass;
  COMMIT lands before the pass's responses flush.
- Degradation ladder: any reader fault falls back to bundled SQLite for
  that request; repeated faults disable the reader for the process.

## Proofs

`proof/` carries Lean 4 theorems over the varint decoder model, all
machine-checked: totality (structural recursion), the 9-byte consumption
bound, consumed-within-buffer (no phantom progress), and the decoded
value bound < 2^64 (the `u64` fit the Rust decoder relies on). The
B-tree walker's bounds remain enforced empirically by the proptest
round-trip over all u64 and the differential oracle.

## Layout

- `crates/sqlite-btree` — decoders (no_std), page/WAL reader, cursors
- `crates/uring-http` — HTTP/1.1 parser (total over arbitrary bytes),
  SPEC-exact JSON writers
- `server` — the five endpoints, rusqlite writer, pass scheduler
- `proof/` — Lean 4 specs (varint model)
