# lean-c-iouring-loafecho

| | |
|---|---|
| Language | C (gnu11), gcc 13.3 |
| I/O | io_uring via liburing 2.5 (Linux 6.8), no framework |
| SQLite | 3.54.1 amalgamation compiled into the binary (stock, unpatched; build.sh downloads it pinned by SHA-256) |
| Nginx | **No: direct on `0.0.0.0:80`** |
| Spec | Lean 4 (v4.35.0) in `spec/` |

## Layout

- `src/server.c` — HTTP/1.1 server, routing, auth, request validation, group commit.
- `src/sha256.h`, `src/jwt.h`, `src/json.h` — SHA-256/HMAC, HS256 JWT verification, a strict JSON validator. Hand-written, no libraries.
- `src/db.c`, `src/db.h` — the SQLite layer (one translation unit with `deps/sqlite3.c`).
- `spec/` — the Lean 4 model and proofs.

## What is proved, and what is only tested

`spec/` models the tables and proves that two cheap read plans give the same answer as the reference SQL in SPEC.md,
as long as an invariant **Inv** holds. Inv: post ids are dense `1..n`, `created_at` never decreases as the id grows, and
every post's author exists.

- `feed_eq_tail`: under Inv, "20 newest by `(created_at DESC, id DESC)`" is the last 20 rows reversed.
- `feedPlan_eq`, `postPlan_eq`: the B-tree walk plans equal the reference feed and post lookups.
- `checkInv_sound`: the executable check implies Inv.
- `inv_addPost`: an insert that passes `insertOk` (new id = old max + 1, `created_at` ≥ the last one, author exists) keeps Inv.

The C code mirrors those plans: `checkInv` runs once at open, `insertOk` after every insert, and `PRAGMA data_version`
is checked at the start of every batch, so a write by another process is noticed. Any failure switches the connection
to `plan=sql` (the reference SQL) for the rest of its life. The server prints the plan line (`invariant=ok plan=btree`)
at startup and again if it changes. The C is **tested, not proved**: the 42 checks in `test/test.sh`, plus
`bin/server --selfcheck <db>`, which serves the feed, posts 1, 250000, 500000 and 10,000 random ids with both plans and
compares the bytes (it exits 0 only with zero mismatches).

Build and check the spec (on a dev box with elan; build.sh does not touch it):

```bash
cd spec && lake build && lake env lean Spec/Axioms.lean
```

## Optimizations and why

- **One request thread, one io_uring.** The box has one vCPU, so threads would only add context switches and locking.
  The ring uses `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`, so completions are handled in batches with no
  interrupts in between.
- **Direct descriptors, multishot accept and recv, provided buffers.** Accepted sockets go straight into the ring's
  fixed-file table (up to 65,536 slots), so there is no fd per connection and no per-request file lookup. Each connection has
  one multishot recv, re-armed only when the kernel ends it. Data lands in a shared ring of 4096 × 2 KB buffers
  instead of a buffer per connection, so ~15,000 idle keep-alive connections cost almost no memory. Ubuntu's 6.8 kernel
  rejects a normal buffer-ring registration with EINVAL (Launchpad #2162843; the reserved-field check is inverted).
  The server retries once with the reserved field set, and if that also fails it falls back to a 2 KB buffer per
  connection and plain recv. The log says which path it took. Idle connections are never timed out.
- **Group commit.** Every batch of completions runs between `db_batch_begin` and `db_batch_commit`. All writes in a batch
  share one `BEGIN IMMEDIATE … COMMIT` (WAL, `synchronous=NORMAL`). No response is handed to the kernel until the
  commit returns, so a 201 always means the row is committed. If the commit fails, every write response in that batch
  becomes a 500. WAL checkpoints run between batches (`wal_autocheckpoint=0`, passive checkpoint past 1000 frames).
- **B-tree read plans.** The feed is 20 rows walked back from the end of the `posts` B-tree, and a post is a rowid seek.
  Each plan adds an author rowid lookup and a like count from `likes_post_id_idx` (the feed counts all 20 in one index range walk). That avoids sorting 500k rows by
  `created_at`. The plans are correct under Inv (proved above), and the server falls back to SQL whenever Inv can't be
  shown.
- **JWT: the HMAC pads are computed once.** The SHA-256 states after `key⊕ipad` and `key⊕opad` are built at startup, so
  verifying a token costs two short SHA-256 runs. Every token is still verified in full, every time (no caching).
- **Hand-rolled HTTP and JSON.** Requests are parsed in place from the receive buffer. Pipelined requests and requests
  split across reads are handled, and `Expect: 100-continue` is answered. Bodies are checked by a strict RFC 8259
  validator that decodes `\uXXXX` and surrogate pairs. Responses are written straight into the connection's send buffer
  with an exact `Content-Length`.
- No response caches, no table copies in memory: every request reads SQLite through its own 256 MiB page cache (`cache_size=-262144`, `mmap_size=0`), so hot B-tree pages keep their parsed headers between fetches.
- **A pass-through wrapper around SQLite's own page cache** (public `SQLITE_CONFIG_PCACHE2`; it forwards everything and stores no data itself) skips a COMMIT's page-cache truncate when its per-cache bitmap of page numbers shows nothing to drop: after a B-tree split moves a page through page number 262,145, the built-in cache otherwise walks its whole hash table on every COMMIT to drop nothing (`dbtest --pcache` tests it).

## Scripts

- `install.sh` (root): apt installs gcc-13 `13.3.0-6ubuntu2~24.04.1`, liburing-dev `2.5-1build1`, make, curl and
  ca-certificates. If those exact versions have left the archive, it installs whatever noble currently ships.
- `build.sh`: downloads and verifies the amalgamation if `deps/` doesn't already have it, then compiles `src/db.c` and
  `src/server.c` and links `bin/server`. C only.
- PGO (default; `PGO=0` skips it): `build.sh` builds an instrumented binary, runs `bin/server --pgo-train` (in-process, no network or seed: a small temp DB built from the schema, ~70k raw HTTP requests in the load.js mix through the real parser, handlers and batches, 35k JWT checks; the DB is deleted after), then rebuilds with `-fprofile-use`.
- `start.sh`: `exec ./bin/server` in the foreground. Env: `SQLITE_PATH`, `JWT_SECRET`, `HOST` (default `0.0.0.0`), `PORT`
  (default `80`).
