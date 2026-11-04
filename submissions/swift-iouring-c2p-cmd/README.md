# Swift, no framework: io_uring + SQLite B-tree reads

A Swift port of [`rust-iouring-kensac`](../rust-iouring-kensac): one thread drives an io_uring event
loop, and SQLite (the pinned, unmodified amalgamation, linked in statically) is read through its
B-tree cursors instead of its SQL engine. The logic (HTTP framing, JSON, JWT, routing, the record
decoder and the feed plan) is Swift over raw pointers; the byte-moving parts (the ring, SQLite's
internals, the escaper, SHA-256) are C in one SwiftPM C target.

| | |
|---|---|
| Language | Swift 6.4.1 (Swift 5 language mode), `-Ounchecked`, no Foundation, stdlib linked statically; `install.sh` downloads the official tarball and checks its SHA-256 |
| Framework | none: liburing 2.15 (built from source, SHA-256 checked) |
| SQLite | 3.54.1 amalgamation, SHA-256 checked, compiled into the C target with `-O3 -march=native` and the same `SQLITE_*` defines as the Rust entry |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ for `DEFER_TASKRUN`; older kernels fall back to plain task work. Like the Rust entry, there is no epoll fallback, so it won't start where io_uring is blocked (e.g. Docker's default seccomp profile) |

## Layout

| Path | What it is |
|---|---|
| `Sources/CShim/shim.c` | The Rust entry's shim, unchanged in design: `#include`s the amalgamation and exposes cursor seeks, `td_payload`, the range like-count walk, SQL writes, the checkpoint thread and the batch begin/commit |
| `Sources/CShim/loop.c` | The io_uring event loop (rings, receive buffers, batching, held writes). It calls back into Swift once per request |
| `Sources/CShim/util.c` | `td_esc` (SSE2, AVX2 when built with it, scalar fallback), a strict UTF-8 check, HMAC-SHA256 with pre-hashed key pads |
| `Sources/Server/DB.swift` | `Db`: the four cursors, the record decoder (varint, serial types, `columns`), `post`, `feed`, writes |
| `Sources/Server/{JSON,HTTP,JWT,App}.swift` | The JSON validator, HTTP framing, HS256 verification and the routes |

The loop is in C (liburing), with Swift behind one `lp_handler` callback: `lp_handler(buf, len, out, used)`
parses and answers the request at the start of `buf`, appending to the connection's output buffer. It
returns "answered", "incomplete", "bad" or "a write that must wait out a checkpoint hold".

## What it does

Everything the Rust entry's README describes holds here, so only the short version follows.

- **Reads go through the B-tree.** `GET /feed` walks `posts_created_at_id_idx` for the 20 newest
  `(created_at, id)` pairs, looks the posts up in ascending id order, then the 20 authors, then counts
  likes with one range walk of `likes_post_id_idx` (a seek per post when the ids span more than 512).
  `GET /posts/:id` is the same plan for one row. Any surprise (a post with no user, an index entry with
  no row) hands the request to the plain SQL query. The schema's column order is checked at startup.
- **Live reads (rule 5).** Every request reads SQLite while it is served. Only prepared statements,
  cursors and the four root page numbers are reused; the JWT signature is verified on every request.
- **Writes go through SQL**, prepared `INSERT` statements inside the batch's transaction.
- **Group commit (rule 6).** One SQLite transaction per event-loop pass (WAL, `synchronous=NORMAL`,
  `unix-excl`). No response from a pass is handed to the kernel until its `COMMIT` returns; if it fails,
  every answered connection gets a 500 and closes.
- **Checkpoints** run on a second `nice 19` thread (PASSIVE). Writes are held for at most 250 ms while it
  copies the log's tail; the loop checkpoints itself past 65,536 frames.
- **Ring setup.** Registered sparse file table, single-shot accepts into direct descriptors on 8
  `SO_REUSEPORT` listeners (512 in flight), one multishot receive per connection into a provided-buffer
  ring (1024 x 2 KB, so idle connections hold no receive buffer), `ENOBUFS` handled with bounded
  single-shot reads, 256 completions or 1 ms per pass, at most 2048 answers per pass. The buffer ring
  registration retries with `resv[0] = 1` for Ubuntu 6.8.0-136+ kernels
  ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)).
- **No per-request allocation on the common path.** Requests are parsed in place in the kernel's
  buffers; output goes into pooled buffers; the feed is written into one reservation with an
  up-front bound.

## Differences from the Rust entry

- **One ring.** The Rust entry adds rings (up to 16) when the descriptor table fills. Here a single ring
  has as many slots as `RLIMIT_NOFILE` allows (65,535 on the benchmark box), and accepting pauses when
  the table is nearly full. That covers the ~15,000 connections the load test opens.
- **SHA-256 is portable C**, not the `sha2` crate's SHA-NI path.
- **The escaper is C** (`util.c`) rather than Rust intrinsics; same algorithm.
- Body length uses the same rule as the reference: JS-style trim, then Unicode scalars.

## Status

Verified on Linux (aarch64, kernel 7.0, in Docker with seccomp unconfined): builds from `build.sh`,
passes all 42 checks of `test/test.sh`, and survives 6,000 concurrent `POST /posts` plus 4,000 concurrent
`GET /feed` with the WAL checkpointing underneath. **Not yet** benchmarked with `bench/load.js` or run
on the x86_64 Ubuntu 24.04 box, so there is no users-held number and `install.sh` has not been
exercised on a clean machine. The Rust entry's design is the reference; expect the speed to come from
that design, not from the language.
