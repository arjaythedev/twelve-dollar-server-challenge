# c-iouring-Londopy

**C on raw io_uring and SQLite without SQL: no SQL statement on any request (just `BEGIN`/`COMMIT` around each batch of writes), and a server that naps on purpose.**

At this scale almost all of a request's CPU time is spent in the kernel's TCP stack, not in your code, so this server's job is to stay out of the way: no per-request allocation, no SQL compiler or bytecode VM on any request (reads or writes), no idle buffers, and no stalls. Two things set it apart:

- **SQLite with no SQL at all.** Reads *and* writes go straight through SQLite's B-tree: a new post or like is written into its table and every index on it, in SQLite's own record format, inside SQLite's own transaction and WAL. In normal operation the only SQL that ever runs is the `BEGIN IMMEDIATE`/`COMMIT` around each batch of writes. Even `/health` reads a page through a cursor. The reference SQL is kept only as a fallback for a record that doesn't decode, for `VERIFY=1` and for `WRITES=sql`.
- **A server that tunes its own nap.** Pausing a busy loop for a couple of milliseconds lets completions and sends go through the kernel in batches, which on a virtio-net VM cut CPU per request by about 40%. The server measures how busy it is and how many requests each pass collects, and picks the pause itself: none when quiet, longer at medium load, none again at saturation.

| | |
|---|---|
| Language | C (gnu17), gcc from Ubuntu 24.04's `build-essential` |
| Framework | none: raw `io_uring` syscalls (no liburing) |
| Database | SQLite 3.54.1 amalgamation, compiled in (SHA-256 pinned in `build.sh`) |
| Front | **Front: direct** (serves `0.0.0.0:80` itself; no Nginx) |
| Author | Londopy |

## How it works

- **One thread, eight rings.** The kernel caps an io_uring's direct-descriptor table at `RLIMIT_NOFILE` (65,535 on the box), so 300k+ connections need several tables. Each ring gets its own `SO_REUSEPORT` listener on the same port, the kernel spreads new connections across them, and one loop services all eight. Ring flags: `SINGLE_ISSUER | DEFER_TASKRUN | TASKRUN_FLAG | SUBMIT_ALL | NO_SQARRAY`, with the ring fd registered. The loop only enters a ring that has work queued (it reads `IORING_SQ_TASKRUN` from shared memory), so idle rings cost no syscalls. Each ring asks for a 4,096-entry submission queue and a 65,536-entry completion queue. Kernels before 6.10 allocate a ring as one physically contiguous block, though, and on a machine with fragmented memory that fails with `ENOMEM` even with plenty free. So the server halves the queues until the kernel can place them (down to 1,024 completion entries) and says so at startup. A burst beyond the completion queue waits on the kernel's overflow list, so nothing is lost.
- **Idle connections cost 40 bytes of user memory.** Each connection has a multishot `recv` pulling from a shared pool of provided buffers. Buffers go back right after the request is parsed, so an idle keep-alive connection holds no buffer at all. Each ring's pool starts at 256 buffers of 2 KB and doubles whenever a receive finds it empty, up to 2,048. The kernel takes buffers in ring order, so every buffer handed over ends up resident; growing on demand keeps that to what the largest burst needed. Multishot `accept` puts new sockets straight into the ring's descriptor table.
- **Receive buffers that survive Ubuntu's 6.8 kernels.** The server prefers a registered buffer ring. Ubuntu kernels 6.8.0-136 to -147 (LP #2162843) invert the ring registration's reserved-field check: they reject the correct all-zero struct with `EINVAL`. So after an `EINVAL` the server retries with `resv[0] = 1`, which those kernels accept and a correct kernel never sees. If no ring registers at all, it falls back to legacy `IORING_OP_PROVIDE_BUFFERS`: each pass hands freed buffers back with one SQE per run of consecutive ids, flagged `IOSQE_CQE_SKIP_SUCCESS`, and a failed hand-back is retried rather than lost. The startup line says which mode is active, and `LEGACY_BUFS=1` forces the fallback.
- **One send in flight per connection.** io_uring doesn't order two SENDs on one socket, so later responses wait in a short queue behind the current one. A client that stops reading stops being served once it owes 256 KB, and gets dropped if it then buffers 1 MB of requests.
- **Shutdowns are careful about slot reuse.** The kernel runs `IORING_OP_SHUTDOWN` from a worker thread and only looks up the fixed slot when it gets there. So a slot is never closed, and can't be handed to a new connection, until its SHUTDOWN completion has arrived.
- **Graceful closes shut down first.** On 6.8, a fixed-file `CLOSE` doesn't release the socket (so no FIN goes out) until every request armed before it on that ring has finished, and the long-lived multishot recvs of idle connections can hold that off indefinitely. So when a client half-closes, the server sends its answers, then a `SHUTDOWN` (the FIN goes out at once), then the `CLOSE`.
- **Reads skip SQL entirely.** `src/db.c` `#include`s the amalgamation and positions SQLite's internal cursors (`sqlite3BtreeTableMoveto`, `sqlite3BtreeIndexMoveto`, `Next`/`Previous`) on `posts`, `users`, `posts_created_at_id_idx` and `likes_post_id_idx`, then decodes records in place. It's still SQLite reading its own pages through its page cache and WAL on every request, with no compile step and no bytecode VM. The feed takes the first 20 entries of the `(created_at DESC, id DESC)` index, walks consecutive post ids with `Previous`, and counts all 20 posts' likes in one forward walk of `likes_post_id_idx`. Anything unexpected falls back to the reference SQL from SPEC.md, and `VERIFY=1` compares every read against that SQL.
- **Writes skip SQL too.** A post goes into `posts` and into `posts_created_at_id_idx` and `posts_user_id_idx`; a like goes into `likes` and into `sqlite_autoindex_likes_1` and `likes_post_id_idx`. That is everything `INSERT` does for this schema (no triggers, and foreign keys are off, SQLite's default). Records are built in SQLite's format with its smallest integer encodings, a new rowid is the largest plus one (as SQLite assigns it without `AUTOINCREMENT`), and `created_at` is the column default's format, `strftime('%Y-%m-%dT%H:%M:%fZ','now')`. Every constraint is checked before anything changes: the body's length, the post's existence, and the `(user_id, post_id)` key, probed in the unique index. The body must also be valid UTF-8 with no NUL, or it gets a 400, because that's where counting characters matches SQLite's `length()` in the `CHECK`. With all that checked up front, a write can only fail halfway on an I/O or memory error, and then the whole batch rolls back and each of its writes gets a 500. Checked against the reference `INSERT`s by running the same 2,500 writes through both: identical responses, identical rows and index entries, and `PRAGMA integrity_check` ok. `WRITES=sql` switches back to the `INSERT`s.
- **One read transaction per loop pass.** Every request still reads from SQLite while it's served. The transaction is shared by the requests in one pass and closed before the pass commits its writes.
- **Group commit.** Likes and posts are queued and committed together, at most every 5 ms, and each response goes out only after `COMMIT`. WAL with `synchronous=NORMAL`. Committing once per loop pass looked cheaper but wasn't: almost every commit held a single like, rewrote the same tail pages, and grew the WAL about 4× faster.
- **The server tunes its own nap.** A busy loop pass pauses before the next one. Before it does, every ring's queued sends are submitted, so completions and sends go through the kernel in batches instead of one or two at a time. On a virtio-net VM (the replica box at 150k users), a fixed 2 ms pause cut the server's CPU per request from about 25 µs to 15 µs and the whole guest's from about 36 µs to 24 µs. A fixed pause has two costs, though: a quiet server answers late, and a client that sends its next request as soon as it gets an answer is held to one request per pause (about 500 a second). So three feedback loops, measured in 50 ms windows, pick the pause:
  - *whether to nap:* the share of the time the loop isn't asleep. It starts napping above 20% and stops below 8%, so a quiet server answers at once.
  - *whether napping is hurting:* users with think time keep arriving at the same rate whatever the server does, but clients that wait for each answer (a benchmark loop, a smoke test) slow down when it pauses. So when napping starts, the server compares the request rate of the next 150 ms with the window before. If the rate fell by more than a quarter and the server went nearly idle, the clients were waiting on its naps: it stops, and stays off for 1 s (doubling up to 16 s while that keeps happening). A server that stays busy never backs off, and the check is off above 20,000 requests a second, so it can't misfire where the load decides the score. That matters on a VM, where a stall (the guest frozen while requests pile up outside) looks just like throttled clients.
  - *how long:* while napping, it watches how many requests each pass collects (an average over recent passes) and stretches the pause when passes come in under 64, shrinks it when they come in over, between 0.5 and 3 ms. At saturation a pass outlasts any pause, so the loop stops sleeping by itself.

  Under user load it naps the whole time: at 150k–220k users on the replica VM it settles near its 3 ms ceiling with 40–65 requests a pass, at the same CPU per request as a fixed 2 ms (at 220k: 14.0 µs against 14.9 µs for the server, 24.9 against 25.2 for the whole guest, p99 6.8 ms). A fixed 2 ms nap holds one closed-loop client to 479 requests a second. Below 20,000 requests a second the check catches that: with the same check and no cap, one loopback client ran at 21,177 req/s against 24,718 with no nap. Over loopback, though, clients that send their next request the instant an answer arrives can top 20,000 a second together, past where the check is off. They then see about half of the no-nap rate (14,039 against 26,656 req/s for one client), so use `PACE_US=0` for that kind of stress test. Only passes that received requests pause, and a pause never runs past a due commit. `PACE_US=<µs>` fixes the pause instead, and `PACE_US=0` turns it off.
- **The loop never spins.** With nothing ready, it sleeps for at most 1 ms (or until the next commit is due). A loop that spins while writes wait starves the nice-19 checkpoint thread those writes are waiting on. On one vCPU, that turned a 5 ms p99 into 70 ms. Any ring with new work wakes it at once: one eventfd is registered on all eight rings, and its signal stays switched off (`IORING_CQ_EVENTFD_DISABLED`) except while the loop sleeps, so a busy loop pays next to nothing for it. One 6.8 quirk: the kernel can leave `IORING_SQ_TASKRUN` set with no work queued, and only running task work clears it, so a loop that trusts the flag doesn't sleep again until that ring next sees work. When an enter turns up nothing and the flag is still set, the server clears it and enters once more.
- **Checkpoints off the loop.** `wal_autocheckpoint=0`. A nice-19 thread with its own connection runs `PASSIVE` checkpoints. If the WAL passes `HOLD_FRAMES` while a checkpoint is catching up, writes wait (up to `HOLD_MS`) so the next commit can restart the log instead of growing it. Reads never wait.
- **Responses are built in place.** The body goes into a 4 MB arena chunk first, then the header (status line, `Content-Type`, `Content-Length`) is written directly in front of it, so there's no copy and no length placeholder. Chunks are reference-counted by in-flight sends. JSON escaping checks 32 bytes at a time with AVX2 (8 bytes at a time without it).
- **JWT:** HS256 is verified on every request (signature, `alg`, `exp`). Only the HMAC key schedule (SHA-256 of `key^ipad` and `key^opad`) is computed once at startup, since it depends only on `JWT_SECRET`.

## Configuration

`start.sh` needs only the challenge's variables (`SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`). Optional tuning variables, with defaults:

| Variable | Default | What it does |
|---|---|---|
| `RINGS` | `8` | io_uring rings (65,535 connections each) |
| `CKPT_FRAMES` | `2048` | WAL frames before the checkpoint thread starts a pass |
| `HOLD_FRAMES` | `8192` | WAL frames past which writes wait for the checkpoint |
| `HOLD_MS` | `250` | longest a write waits for a checkpoint |
| `COMMIT_US` | `5000` | minimum time between group commits (0 = every loop pass) |
| `CACHE_KIB` | `8192` | SQLite page cache in KiB. The OS page cache serves the misses, about 0.05 per response at 8 MB |
| `PACE_US` | `auto` | the pause after a busy loop pass: `auto` tunes it (see above), a number fixes it in µs, `0` turns it off |
| `WRITES` | `btree` | `sql` = write with the reference `INSERT` statements instead of straight into the B-tree |
| `LEGACY_BUFS` | `0` | `1` = skip the buffer ring and use legacy provided buffers |
| `TCP_CC` | kernel default | congestion control set on the listeners (inherited by connections) |
| `VERIFY` | `0` | `1` = check every B-tree read against the reference SQL (testing only) |
| `DEBUG` | `0` | `1` = log connection errors and 404/500s, plus counters every 5 s (build with `EXTRA_CFLAGS=-DPROF` to add per-stage timings) |

## Building from source

```bash
sudo bash install.sh   # build-essential, curl
bash build.sh          # fetches and verifies SQLite, builds bin/server
SQLITE_PATH=/path/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Layout

```
src/server.c   event loop, rings, HTTP parsing, routing, group commit
src/db.c       SQLite (amalgamation included here): B-tree reads and writes, checkpoint thread
src/jwt.h      SHA-256, HMAC, base64url, HS256 verification
src/json.h     JSON escaping, integer formatting, a strict JSON reader
```
