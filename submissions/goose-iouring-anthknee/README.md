# Goose, io_uring and SQLite

Goose handles HTTP, JSON, HS256 authentication and SQLite record decoding. It manages connections, schedules requests, and controls database reads, commits and checkpoints. C calls io_uring and manages the SQLite handles. I kept the receive buffers in C because io_uring needs them longer than Goose can lend them.

| Component | Version |
| --- | --- |
| Language | [Goose 2026.4](https://github.com/aardappel/goose/releases/tag/v2026.4), compiled to C |
| Native compiler | Ubuntu 24.04 GCC 13 |
| Framework | None. Direct HTTP/1.1 on `HOST:PORT`; no Nginx |
| Network | [liburing 2.15](https://github.com/axboe/liburing/releases/tag/liburing-2.15) |
| Database | SQLite 3.54.1 |
| SQLite interface | C API and internal B-tree cursor functions |
| JSON scanner | [goose-simd](https://github.com/sstadick/goose-simd/tree/bef8b31dfa4f0c87a8d0180c1e7fa6cbb1e435e6), fixed source commit |

## Build and run

The server targets Ubuntu 24.04 on CPUs that support `x86-64-v3` and requires io_uring. The challenge runs it directly on `0.0.0.0:80`. For a local run, use a writable copy of the seeded database and start the server from the submission directory.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/absolute/path/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Optimizations

### Network and scheduling

- Eight `SO_REUSEPORT` listeners use multishot accepts. Accepted sockets inherit `TCP_NODELAY` and occupy fixed-file slots in io_uring. As one ring fills, the event loop moves accepts to the next ring. It can use up to 16 rings with up to 65,535 slots each.
- Each ring shares 2,048 receive buffers of 1 KiB across its connections. Multishot receives handle steady traffic. If a receive ends with data or runs out of buffers, nonblocking reads drain the socket before the server rearms it. Each ring allows up to 1,024 recovery reads in flight.
- Each ring uses `IORING_SETUP_SINGLE_ISSUER` and `IORING_SETUP_DEFER_TASKRUN`. The loop batches I/O submissions and buffer returns. It rotates the first ring it drains on each pass. It skips the blocking wait when ready requests, eligible held writes or recovery reads are queued.
- Ready, held and recovery queues advance a head index so removing an entry usually takes no copying. Each pass allows 2,048 request attempts. Before the loop drains new completions, it spends up to 1,024 attempts on older ready requests and eligible held writes.
- If the loop has spent its request budget and has replies pending, it commits and submits sends before the next recovery round. Recovery continues in the same pass.

### Connection memory

- A connection record occupies 29 bytes on Linux x86_64. Eight-byte queue handles identify a slot and its generation so the server can reject stale work when it reuses a slot. Idle connections need no dedicated application buffers.
- C copies incoming bytes into a shared Goose buffer and returns the receive buffer to io_uring. A complete request that can run immediately needs no per-connection input allocation.
- Queued input uses power-of-two native buffers starting at 512 bytes. Active and retained input buffers share a 6 MiB budget. For overflow, `alloc_copy` copies the bytes into an exact-length span in Goose’s `slice_pool<u8>`. The pool coalesces freed spans. A bit in the existing 32-bit input handle identifies the storage, so overflow adds no bytes to the connection record.
- Input and response buffers use separate free lists and reuse exact sizes. Together the lists retain up to 1,024 buffers, each at most 32 KiB. The minimum response buffer size is 256 bytes. Before the first copy, the server selects the smallest power-of-two capacity that fits the HTTP header and body. The buffer stays valid until the final send completes.
- Native buffer capacity and the Goose overflow pool’s backing storage share a 256 MiB limit. Between batches, a drop of at least 16 MiB from peak native buffer capacity triggers an attempt to release unused allocator pages. Attempts are at least five seconds apart. At startup, the server also tries to keep readable mappings of its executable resident with `mlock`.

### Parsing and authentication

- The SIMD scanner finds quotes, backslashes and control bytes in 16- or 32-byte spans. The JSON encoder copies runs of ordinary characters directly into a reusable output buffer. Parsing, JWT decoding and SQLite record decoding also reuse scratch buffers.
- Each authenticated request verifies the token signature and expiry. HS256 reuses the signing key’s precomputed inner and outer SHA-256 states. Like requests validate the username in place. Feed and post lookups skip clock reads during dispatch.

### SQLite

- Feed and post lookups use SQLite B-tree cursors in the current transaction. Goose decodes the copied records. The feed walks the timestamp/ID index, fetches post rows in ascending ID order for locality, and returns them in feed order. Writes and the SQL feed fallback reuse prepared statements.
- Nearby feed posts share one scan of the likes index. C counts supported entries directly in SQLite index leaf cells to avoid copying each record. For entries it cannot decode there, the server falls back to a cursor walk.
- Requests in a batch share a transaction. The server holds their responses until commit, including reads that observed a write in the same batch. SQLite uses WAL with `synchronous=NORMAL`, the `unix-excl` VFS and a 48 MiB page-cache target on the main connection.
- At 4,096 WAL frames, a worker runs a passive checkpoint so the event loop can keep serving reads. The worker requests nice +10. The loop briefly holds writes for the final checkpoint pass. At 65,536 frames it holds new writes until a fresh pass finishes copying the WAL.
- A VFS adapter combines adjacent WAL frame headers and pages into fewer writes. A `PCACHE2` wrapper skips a truncation scan when the tracked page numbers show that it would remove nothing.

### Compilation

Goose’s `c_headers` exposes the native I/O adapters to the C compiler for inlining. GCC builds the generated C, adapters and SQLite with `-O3` and link-time optimization. The build targets `x86-64-v3`, tunes for Cascade Lake and prefers 256-bit vectors. Runtime assertions stay enabled.

## Credits

I adapted the JSON, JWT and SQLite record routines from [goose-iouring-ahmadsh2007 (#291)](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/291) and [sstadick’s changes](https://github.com/sstadick/twelve-dollar-server-challenge/pull/1), and based the native SQLite adapters and io_uring calls on #291 and [rust-iouring-kensac (#264)](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264).

## AI

Done using GPT-6.1 Sol Max and GPT-6 Astra Extra High.
