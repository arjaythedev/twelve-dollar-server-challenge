# c-iouring-PROGABBOMALA

C server on raw io_uring with SQLite 3.54.1 (official amalgamation, not patched).

- Language: C (gnu11), gcc from Ubuntu 24.04, `-O3 -march=native`
- Framework: none. io_uring syscalls directly (no liburing), my own HTTP parser, JSON and HS256
- SQLite: amalgamation 3.54.1, sha256 checked in `build.sh`, opened with the `unix-excl` VFS
- **Direct, no Nginx**: listens on `HOST:PORT`
- One process, one thread for requests, one low priority thread for WAL checkpoints

This replaces my earlier PR #241.

## How it works

The loop waits for completions, turns what arrived into a batch of requests (max 2048), runs the
batch in one SQLite transaction and then sends the answers.

- Reads of a batch run before its writes, then `COMMIT`, then the sends. So a 201 is only sent
  after its commit, and a read never returns something that is not committed yet. If the commit
  fails the writes of that batch get a 500.
- WAL, `synchronous=NORMAL`, group commit per batch.
- Nothing is cached between requests. Feed and post are read from SQLite every time, like count
  included. The JWT signature is computed on every request (only the HMAC key pads are prepared at
  startup).
- Reads go through SQLite's own B-tree functions (`sqlite3BtreeTableMoveto`,
  `sqlite3BtreeIndexMoveto`, `sqlite3BtreeFirst/Next`, `sqlite3BtreePayloadFetch`) inside a read
  transaction. I only call them, I don't write any cursor field. If a record looks different from
  what the schema says, the request falls back to the normal SQL statement.

## Optimizations

- **Several rings.** Connections are direct descriptors, but one ring's table is limited by
  `RLIMIT_NOFILE` (65535), so there are 8 rings. All of them are created at startup: a ring needs
  2 MiB of contiguous kernel memory for its completion queue, which is easy to get on an empty
  box and not when it is full of sockets (a smaller queue is tried if that fails). A ring is
  entered only when the kernel flags pending work on it.
- **Accepts.** Single-shot, 64 per listener in flight and never more than the free slots of the
  ring, so the table can't overflow. 8 `SO_REUSEPORT` listeners for a bigger accept queue during
  the ramp.
- **Receive.** Multishot recv with a provided buffer ring. An idle connection uses 24 bytes in the
  server and no buffer.
- **After a burst.** A multishot recv that ends (the buffers ran out) is not armed again right
  away. The connection goes on a queue and gets one `MSG_DONTWAIT` read at a time, never more in
  flight on a ring than half its buffers, until its socket is empty. Then the multishot recv is
  armed again.
- **Drain first.** Every pass reads everything that arrived but answers at most 2048 requests.
  The rest is copied to its connection and answered by the next passes, oldest first, so a
  backlog sits in user memory and not in kernel socket buffers.
- **Send.** The response is written once into an arena (header right before the body) and sent
  from there with `MSG_WAITALL`. An arena is reused when all its sends are done.
- **Batching.** The other rings submit and collect without sleeping, then ring 0 sleeps until 128
  completions or 1 ms. So there are few passes and few transactions, whichever ring the traffic
  is on.
- **Feed.** 20 ids from `posts_created_at_id_idx`, then the rows in ascending id order (SQLite
  steps to the next cell instead of searching again), and one scan of `likes_post_id_idx` for all
  the like counts.
- **WAL.** PASSIVE checkpoints from the second thread (nice 19). When the log reaches 4096 frames
  the thread copies most of it, then POSTs are held for a moment (250 ms max, GETs continue) so
  the rest can be copied and the next write restarts the log from the beginning.
- No printf in the request path.

`RING_SLOTS` and `CKPT_FRAMES` are env vars I use for testing with small values. `start.sh` does
not set them.

## Changes since #399 (220k)

- The loop did not batch once connections were on more than one ring: any completion on another
  ring skipped the 1 ms sleep, so passes were tiny and almost every write had its own commit.
  Now ring 0 always does the sleep unless requests are already read and waiting.
- Rings are created at startup instead of during the ramp.
- Bounded reads after a burst and drain first (before, all the ended receives were armed again
  together and reading stopped at the answer limit).
- JWT signature compared in constant time.

## Credits

Code is written from scratch, but these ideas come from other submissions:

- #264 rust-iouring-kensac: bounded single-shot accepts, the batched wait on the first ring,
  bounded reads after a burst and drain first, posts in ascending id order, checkpoint in two
  phases with a short write hold, the `IORING_REGISTER_PBUF_RING` retry for Ubuntu 6.8 kernels
- #245 c-iouring-pavlyhalim: the layout of the B-tree helpers in `btree.h` (record decoder,
  payload fetch, index seek, like range count), sending from shared arenas with `MSG_WAITALL`,
  header written in front of the body
- #250 c-iouring-shivanshthapliyal: entering a ring only when `IORING_SQ_TASKRUN` is set

## Build

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=seed/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 bash start.sh
```
