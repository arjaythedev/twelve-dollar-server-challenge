# x86-64 assembly (NASM) + io_uring + SQLite

The whole server is hand-written x86-64 assembly: event loop, HTTP/1.1 parser, router, JSON
reader/writer, base64url, SHA-256/HMAC and JWT verification. SQLite is the only C code in the
binary, as the rules require. It is built from the pinned amalgamation and called from assembly
through the System V ABI. libc is linked only because SQLite needs it. All socket I/O goes
through raw `io_uring` syscalls.

(The directory keeps its old name. Version 1 used epoll; version 2 uses io_uring.)

| | |
|---|---|
| Language | x86-64 assembly, NASM 2.16 (Ubuntu 24.04 package) |
| Framework | none: one io_uring loop, raw syscalls |
| SQLite | 3.50.4 amalgamation, downloaded by `build.sh` and checked against its sha256 |
| C compiler | gcc (only for `sqlite3.c`, `-O3 -march=native`, `SQLITE_THREADSAFE=0`) |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Kernel | needs Linux ≥ 6.1 (`DEFER_TASKRUN`); developed and tested on 6.8 |

About 4,800 lines in `src/`: `server.asm` (connections, HTTP, handlers, auth), `uring.inc`
(ring setup and helpers), `json.inc`, `sha256.inc` and `defs.inc`.

## How it works

One thread runs one event loop over one SQLite connection. Each pass of the loop:

1. Takes up to 2,048 completions from the rings: new connections, received data, finished sends.
   Every complete request in a connection's buffer is parsed and handled. A partial request
   waits for the rest.
2. The first write of the pass (`POST /posts` or a like) opens a transaction. Every later write
   in the pass joins it (**group commit**).
3. Each response is built directly in one pass arena: status line, headers, JSON body.
4. At the end of the pass the transaction is **committed first**, and only then are the
   responses sent. A 201 therefore always means the row is committed, and no read in the pass can
   show a write that might still roll back. If a commit fails, the connections waiting on it are
   closed instead of being answered.

## Why io_uring, and what it changed

Batch 2 scored 155k users with the epoll version. Testing on a 1-vCPU KVM VM (the same kernel
as the benchmark box, 6.8.0-117, with the load coming over a real tap device) showed why it
stopped there:

- **The 65,535 fd limit.** Above ~60k users, every new user got `Connection: close`, so at 155k
  about 60% of all requests opened a new TCP connection. Each one costs ~10 µs of kernel time,
  and that was most of the CPU.
- **Syscalls.** epoll needed `epoll_wait`, a `read` and a `write` per request, plus SQLite's
  `fcntl` locks around every transaction.

Version 2 removes both:

- **Fixed files instead of fds.** Accepted sockets go straight into io_uring's fixed-file table
  (`IORING_FILE_INDEX_ALLOC`) and never get an fd, so `LimitNOFILE` does not apply to them. One
  ring holds ~65k connections. When it fills up, the next ring is created (up to 8), and the
  accepts move there. 350k open keep-alive connections work. No user gets `Connection: close`
  unless they ask for it.
- **Fewer syscalls.** Receiving uses multishot `recv` into a provided buffer ring. Sending uses
  `send` with `MSG_DONTWAIT` and completes inside the same `io_uring_enter` call. Waiting happens
  in that call too. Measured: **~0.9–1.1 syscalls per request** (0.3–0.45 `io_uring_enter`, the
  rest is SQLite's WAL `pwrite`), against more than 3 before.
- **Ring 0 uses `DEFER_TASKRUN`, the others don't.** The loop waits on ring 0, whose completion
  work runs only when the loop asks for it, in one batch. Under load the wait returns after 32
  completions or 250 µs, so each pass has a decent batch. The other rings (`COOP_TASKRUN`) post
  their completions as task work on the way back from any syscall, so they need an
  `io_uring_enter` only to submit. Ring 0 has a multishot poll on each of their fds, so their
  completions also wake the wait. (With `DEFER_TASKRUN` on every ring, each ring needed its own
  enter per pass, and data waited in the sockets until then: at 335k connections that was 72 MB
  of socket buffers during the ramp. The idea is from the Objective-C entry, #154.)

### Kernel 6.8 details that mattered

- **A fixed-file close does not send FIN right away.** On 6.8 the socket is only released after
  every older request on that ring has finished. The multishot `recv` keeps running, so the
  client never saw the close. The fix: `IORING_OP_SHUTDOWN` first, then the close (linked). The
  FIN now goes out at once, and the shutdown also ends the receive (it sees EOF). An
  `ASYNC_CANCEL` by fd is not needed, and it was expensive: it scans every armed poll on the ring,
  the 256 waiting accepts included, on every close.
- **`SQ_TASKRUN` can stay set with no work queued** (seen on 6.8 and 7.2). The loop reads that
  flag to know which rings need an `io_uring_enter`, so a quiet ring with a stale flag made it
  spin at 90% CPU while idle. When an enter posts no completions, the loop now clears the flag
  itself. That could race with new work arriving at that moment, so every ring is also entered
  once a millisecond.
- **Provided buffer rings fail on some 6.8 builds.** `IORING_REGISTER_PBUF_RING` returns `EINVAL`
  on 6.8.0-147 (it works on 6.8.0-117). The server then falls back to
  `IORING_OP_PROVIDE_BUFFERS`. Both paths are tested.
- **An eventfd doesn't help with `DEFER_TASKRUN`.** It woke the loop several milliseconds late,
  so the loop waits in `io_uring_enter` on ring 0 instead. When idle with several rings, the wait
  also times out after 1 ms.
- **Receive buffers: 2,048 of 1 KB per ring.** With 512 per ring, connect bursts ran out
  (`ENOBUFS`), and the data waited in the sockets. 1 KB is larger than a request here; a longer
  one spans buffers.
- **Multishot accept stalled under bursts**, so there are 256 single-shot accepts in flight
  per ring. Fewer than that let the listen queue overflow when 300k clients connect within a
  minute.
- **Passes are bounded.** A pass takes at most 2,048 completions. When the submission queue is
  full, it is submitted without waiting for completions. Before this change one pass during
  the ramp ran for 13 s, and no responses went out until it ended.

## SQLite

- **Pragmas** (all allowed by rule 6): `journal_mode=WAL`, `synchronous=NORMAL`,
  `locking_mode=EXCLUSIVE` (one connection in one process: no `fcntl` lock syscalls per
  transaction), `mmap_size=0` with a 16 MB page cache, `wal_autocheckpoint=0`,
  `journal_size_limit=64 MB`.
- **Checkpoints run in the loop, right after a pass's responses are sent.** Once the WAL
  reaches 4,096 frames, a `PASSIVE` checkpoint runs. Nothing is writing at that point, so it
  always completes and the next commit restarts the WAL from the beginning. A small WAL matters,
  because every page lookup probes every WAL hash segment.
- **The page cache, not mmap.** With `mmap_size=0`, a page that is in SQLite's own cache is found
  without searching the WAL index. That was measurably faster than mmap.
- **`/feed` in two queries.** One join returns the 20 newest posts with their authors. Then one
  range scan over `likes_post_id_idx` (`post_id BETWEEN min AND max`) counts all their likes
  into an array. Before, there were 20 separate `count(*)` lookups. If the 20 ids are spread
  over more than 4,096, it falls back to a count per post.
- **Prepared statements are created once and reused** (`SQLITE_PREPARE_PERSISTENT`). Column text
  is escaped straight into the response, 16 bytes at a time with SSE2. Numbers are formatted
  with a multiply instead of a divide.
- **JWT verification without allocation.** The HMAC inner and outer states are computed from the
  secret once at startup, so each token costs 3–4 SHA-256 compressions. Every token is verified
  on every request. The header (`alg` must be `HS256`) and the payload are parsed with a strict
  RFC 8259 reader. `exp` is **required**: a token without it gets 401.

No response or query result is cached anywhere.

## Results (local, not the benchmark box)

KVM VM with 1 vCPU (pinned to one host core) and 2 GB RAM (`MemTotal` 1,968 MiB), Ubuntu 24.04,
kernel 6.8.0-117, the server started by systemd with `LimitNOFILE=65535` as on the benchmark
box. The load came from a Go reimplementation of `bench/load.js` (same user loop, think times
and probabilities). It ran on the host over a tap device, as 13–15 processes, each from its own
source address. The host CPU is an AMD Ryzen 7 255, not the Xeon 8260 of the benchmark box, so
absolute numbers will differ.

| | v1 (epoll, batch 2) | v2 (io_uring) |
|---|---|---|
| server CPU per request | 40–52 µs | 16–19 µs |
| syscalls per request | > 3 | 0.9–1.1 |
| 155k users, 2 min hold | fails: p95 3.5 s, p99 5.4 s | passes |
| 300k users, 120 s ramp, 5 min hold | — | passes: p95 10.5 ms, p99 21 ms |
| 340k users, 120 s ramp, 5 min hold | — | passes: p95 20.6 ms, p99 51 ms, 0 failed of 10.8 M requests |
| **350k users, 130 s ramp, 5 min hold** | — | **passes**: p50 0.6 ms, p95 31 ms, p99 143 ms, 0 failed of 11.1 M requests |

At 350k users (~37,000 requests/s) the server process uses 69% of the vCPU (18.7 µs per
request, 0.9 syscalls). Most of the rest is the kernel's TCP stack and network interrupts.
(Per-request CPU moved by ±1.5 µs between VM boots, for the same build.)

**The limit on this box is memory, not CPU.** Every open TCP connection costs the kernel about
4.2 KB, whether it is busy or idle: the socket (2.3 KB), its inode, file and dentry (1.3 KB),
the armed multishot receive (0.35 KB). At 350k that is about 1.5 GB of slab on a 1.97 GB
machine. The server itself adds about 50–80 MB (connection table, receive buffers, SQLite's
16 MB cache). The app's cgroup peaked at about 1 GB: it is charged for part of the kernel memory
and for the pages of the database it writes. 360k and 370k passed one-minute holds right after
a VM boot, and ran the machine out of memory during the ramp in later runs (the kernel's slabs
fragment over repeated runs), so 350k is the dependable limit here.

With a 60 s ramp, some runs had a few failed load processes at the end of the ramp. In those
runs the host running the load generators was at ~90% CPU. The server's longest pass stayed
under 50 ms and the listen queue didn't overflow. With a 120 s ramp, every run passed.

### Tried and dropped: a keep-alive budget

To go past the memory limit, I tried a keep-alive budget computed from the machine's RAM:
connections past it got `Connection: close`, so their users reconnect for every request instead
of holding a socket. It needs its own ring for those connections: on 6.8 a closed fixed file is
only released once every older request on its ring has finished, and the keep-alive
connections' receives never finish. It worked, but a connection per request costs ~85 µs of VM
CPU here (SYN, FIN and their interrupts), against ~20 µs for a request on a kept-alive
connection. At 350k users with a 320k budget the CPU ran out and p95 went to 2 s, while plain
keep-alive passed 350k with p99 48 ms. So there is no budget: every connection is kept alive.

## Tests

- `test/run.sh`: 42/42.
- A differential test (113 raw-HTTP cases: escaping, pipelining, split requests, slow readers,
  auth edge cases) compares v2 byte for byte against v1. The only difference is the intended
  one: a token without `exp` is now rejected.

## Trade-offs

This is a server specialised for this API. It implements only the parts of HTTP/1.1 the API
needs: no chunked request bodies and no `Expect: 100-continue`, and request bodies over 32 KB are
rejected with 400. It runs only on x86-64 Linux with io_uring enabled. `synchronous=NORMAL` means
a committed write can be lost on power failure, though not on a process crash. That is the
trade-off rule 6 allows.
