# c-iouring-PROGABBOMALA

C server on raw io_uring with SQLite 3.54.1 (official amalgamation, not patched).

- Language: C (gnu11), gcc from Ubuntu 24.04, `-O3 -march=native`
- Framework: none. io_uring syscalls directly (no liburing), my own HTTP parser, JSON and HS256
- SQLite: amalgamation 3.54.1, sha256 checked in `build.sh`, opened with the `unix-excl` VFS
- **Direct, no Nginx**: listens on `HOST:PORT`
- One process, one thread for requests, two helper threads for WAL checkpoints

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
- **Batching.** Ring 0 sleeps until 128 completions or 1 ms, then the other rings are collected,
  so what reached them during the sleep is answered by this pass. Their sends are submitted at
  the end of the pass, ring 0's with its next sleep. So there are few passes and few
  transactions, whichever ring the traffic is on. A ring is entered again while an enter still
  brings many completions, for kernels that run only a few deferred completions per call.
- **Feed.** 20 ids from `posts_created_at_id_idx` and one scan of `likes_post_id_idx` for all the
  like counts. Each of the 20 posts gets its own pair of SQLite cursors (post row and author row),
  picked by post id and kept until the reads of the pass are done. The next feed request of the
  same pass asks the same cursors for the same rows, and `sqlite3BtreeTableMoveto` returns at
  once when the cursor is already on that row, so there is no search from the root again. The
  text is read where it lies in SQLite's page, every time, nothing is copied aside or kept after
  the transaction. The cursors are closed before the writes of the pass.
- **WAL.** PASSIVE checkpoints off the request thread. When the log reaches 4096 frames a nice 19
  thread copies it twice and syncs the database file after each copy, while writes go on. Then
  POSTs are held for a moment (250 ms max, GETs continue) and a second thread at normal priority
  copies the few frames left, so the next write restarts the log from the beginning. The sync
  before the hold matters: a PASSIVE copy that doesn't reach the end of the log leaves its pages
  unsynced, and the held copy would have to flush the whole cycle while writes wait.
- **File sizes stay put.** The WAL file is not truncated after a restart, the next cycle writes
  over it. The database file grows in 64 MiB steps (`SQLITE_FCNTL_CHUNK_SIZE`, built with
  `HAVE_POSIX_FALLOCATE`). A file that changes size makes ext4 commit its journal at the next
  sync, and a commit on the request thread that extends the log at that moment has to wait for it.
- No printf in the request path.

`RING_SLOTS` and `CKPT_FRAMES` are env vars I use for testing with small values. `start.sh` does
not set them.

## Changes since #406 (250k)

- `journal_size_limit` is gone. I added it in #406 and it was a mistake: with the log cut to 1 MiB
  after every restart, each cycle extended the file again while the checkpoint thread was syncing
  it. On my machine that gave 5 to 12 commits over 2 ms every 5 seconds (up to 21 ms), without it
  0 to 2, and the commits cost about 30% less CPU. On the box #406 passed 250k at 77% CPU and
  failed 260k on p99, the same picture asm-btree had with this pragma (260k, against 280k
  without).
- Feed with a cursor pair per post: 5.5 to 3.0 µs per feed request on my machine.
- Order of the rings inside a pass, and a ring is entered again while it still brings a lot.
- Database file grown in 64 MiB steps.

Server alone on one core, organizer's `mload` with 250,000 users on the other cores of the same
machine, think times scaled down until the server saturates: #406 answers 60k requests/s, this
version 73-75k.

## Changes since #399 (220k)

- The loop did not batch once connections were on more than one ring: any completion on another
  ring skipped the 1 ms sleep, so passes were tiny and almost every write had its own commit.
  Now ring 0 always does the sleep unless requests are already read and waiting.
- Rings are created at startup instead of during the ramp.
- Bounded reads after a burst and drain first (before, all the ended receives were armed again
  together and reading stopped at the answer limit).
- Checkpoints: database synced before the write hold, the held copy on a normal priority thread,
  WAL file cut back after each restart.
- JWT signature compared in constant time.

## Credits

Code is written from scratch, but these ideas come from other submissions:

- #264 rust-iouring-kensac: bounded single-shot accepts, the batched wait on the first ring,
  bounded reads after a burst and drain first, checkpoint in two
  phases with a short write hold, the `IORING_REGISTER_PBUF_RING` retry for Ubuntu 6.8 kernels
- #245 c-iouring-pavlyhalim: the layout of the B-tree helpers in `btree.h` (record decoder,
  payload fetch, index seek, like range count), sending from shared arenas with `MSG_WAITALL`,
  header written in front of the body
- #250 c-iouring-shivanshthapliyal: entering a ring only when `IORING_SQ_TASKRUN` is set
- #388 / #390 asm-btree-ninhnv: syncing the database before the write hold, the held copy on a
  normal priority thread. Its runs with and without `journal_size_limit` are what made me look
  at mine
- #354 go-iouring-davidefrageri: the order of the rings inside a pass (sleep on the first ring,
  then collect the others, their sends at the end of the pass) and growing the database file
  with fallocate
- #343 go-iouring-trungdlp: entering a ring again when one call did not bring everything
- #326 objc-uring-delneg: B-tree cursors kept per feed position

## Build

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=seed/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 bash start.sh
```
