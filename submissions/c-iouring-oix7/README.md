# C + io_uring + SQLite

Single-process HTTP/1.1 server in C11. Serves directly on `0.0.0.0:80` (no Nginx).

| Component | Details |
|---|---|
| Language / Compiler | C11 (`gcc -O3 -march=native -pthread`, PGO) |
| Framework | None |
| Database | SQLite 3.54.1 amalgamation, in-process (`WAL`, `synchronous=NORMAL`) |
| Transport | Linux `io_uring` (direct descriptors, multishot accept/recv, provided-buffer rings) |

## Design

- **Batch transactions.** One SQLite transaction per event-loop batch; reads share a snapshot and writes group-commit before responses are sent.
- **Direct B-tree reads.** `/feed` and `/posts/:id` read via SQLite B-tree cursors and serialize directly into pooled output buffers, with a single `likes_post_id_idx` range scan per feed and SQL fallback if schema/rowid invariants change.
- **Bounded receive drain.** Sockets whose multishot receive exhausts the buffer ring move to a bounded FIFO of nonblocking single-shot reads.
- **Background WAL checkpointer.** A `nice 19` thread runs passive checkpoints every 2,000 frames; the event loop finishes the tail at a batch boundary when within 128 frames.
- **PGO build.** `build.sh` trains on a 6 s replay of the request mix before the final compile.

## Credits

Builds on techniques from `lean-derive-hargup` (#153: single-TU B-tree read structure and batching) and `c-iouring-pavlyhalim` (#245: multi-key B-tree descent, `SQLITE_CONFIG_PCACHE2` truncate wrapper, WAL-coalescing VFS, and bounded receive drain).
