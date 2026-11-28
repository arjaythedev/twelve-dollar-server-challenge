# Goose with io_uring and SQLite

This Goose 2026.4 server lets clients read posts, publish posts and add likes. It serves HTTP directly on `0.0.0.0:80` on the challenge VM, without a web framework or Nginx. [API specification](../../SPEC.md)

C adapters use liburing 2.15 for network I/O and SQLite 3.54.1 for storage. Goose verifies HS256 tokens on authenticated requests and scans JSON strings with [goose-simd](https://github.com/sstadick/goose-simd/tree/bef8b31dfa4f0c87a8d0180c1e7fa6cbb1e435e6).

## Build and run

Run from this directory on Ubuntu 24.04 with GCC 13 and an `x86-64-v3` CPU. For a local run, set `SQLITE_PATH` to a writable copy of `seed/feed.db`.

```sh
sudo ./install.sh
./build.sh
SQLITE_PATH=/path/to/feed-copy.db JWT_SECRET=twelve-dollar-challenge \
  HOST=127.0.0.1 PORT=3000 ./start.sh
```

## Optimizations

### Feed and post reads

- The feed takes up to 20 posts from SQLite’s timestamp index. It fetches rows in ID order to keep nearby reads together, then returns the posts newest first.
- Batched author lookups share users-table pages. A parent key can predict a row’s position. The server verifies the ID before copying just the username. Misses use binary search. Records that span pages or have long headers use the normal cursor. Goose reads short username fields directly and inlines its general column decoder.
- Nearby posts share one scan of the likes index. A second cursor reads its highest post ID in the current transaction. Posts above that ID have no likes. The server counts supported entries directly on index pages and uses the normal cursor for other formats. [Feed reader](src/db.goose) · [SQLite adapter](native/sqlite.c)

### Writes and checkpoints

- Posts and likes share a 16 ms write window and reuse prepared statements. Reads can continue. Replies that depend on a write wait for commit.
- SQLite uses WAL with `synchronous=NORMAL` and a 128 MiB page cache target. The file adapter combines adjacent WAL writes. The page cache skips scans that would remove nothing.
- During a full checkpoint, reads continue and new writes pause. A low-priority worker groups up to 16 adjacent pages per write (64 KiB maximum). Lock contention switches it to individual writes. Below 65,536 WAL frames, the server asks a checkpoint still running after 250 ms to stop.

### Connections and memory

- Eight `SO_REUSEPORT` listeners enable `TCP_NODELAY`. Multishot accepts and receives reduce system calls. Each ring has fixed socket slots and 1,024 shared 2 KiB buffers. More rings open as slots fill.
- The event loop batches I/O and buffer returns, rotates between rings and checks the others before waiting on the primary ring. Queues advance by index and older requests get time each pass. Nonblocking reads drain sockets when receive buffers run out. When the request budget runs out, committed replies go out before the next round of reads. [Event loop](src/transport.goose)
- A connection record uses 29 bytes on Linux x86-64. Idle connections need no private request buffer. Queued input and replies reuse buffers by size. Input beyond the 6 MiB native pool uses exact-sized Goose slices within a combined 256 MiB budget. The server tries to keep its code in RAM and release unused heap pages after bursts. [Buffer pools](src/input_buffers.goose) · [Native I/O](native/io.h)

### Parsing and compilation

- SIMD finds JSON characters that need escaping so the encoder can copy plain text in blocks. Parsers reuse scratch buffers. Goose precomputes the signing key’s hash state and verifies each token’s signature and expiry. Like requests validate the username in place. Feed and post reads skip the clock lookup. [JSON](src/json.goose) · [JWT](src/jwt.goose)
- Goose exposes the native I/O code for inlining. GCC builds the server and SQLite with `-O3` and link-time optimization for `x86-64-v3`, tuned for Cascade Lake. Runtime assertions stay enabled. [Build script](build.sh)

## Credits

- The JSON, JWT and record codecs build on the original Goose submission and sstadick’s follow-up. [goose-iouring-ahmadsh2007 (#291)](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/291) · [sstadick’s changes](https://github.com/sstadick/twelve-dollar-server-challenge/pull/1)
- The native SQLite and io_uring adapters build on the same Goose submission and Kensac’s Rust implementation. [rust-iouring-kensac (#264)](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264)
- The batched author lookup, WAL frame grouping and page-cache scan shortcut follow Pavly Halim’s implementation. The rowid shortcut follows Will Wang’s reader. [c-iouring-pavlyhalim (#245)](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245) · [Will Wang’s reader](../c-uring-willwang/reader.c)

Built with GPT-6.1 Sol Max and GPT-6 Astra Extra High.
