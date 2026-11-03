# Go (io_uring, epoll fallback) + C hot path

| | |
|---|---|
| Language | Go 1.27.2 (official tarball) and C (gcc from Ubuntu 24.04's `build-essential`) |
| Framework | none: a hand-written event loop and HTTP/1.1 parser |
| I/O | io_uring through liburing 2.5 (Ubuntu 24.04's `liburing-dev`); epoll when io_uring isn't available |
| SQLite | 3.54.0 amalgamation, compiled into the binary through cgo (`build.sh` downloads it and checks its SHA-256) |
| Dependencies | none outside the Go standard library, SQLite and liburing |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl, unzip, liburing-dev; Go 1.27.2 to /usr/local/go
bash build.sh          # downloads the pinned SQLite amalgamation, go build -> bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

The log's first lines say which I/O backend is in use. `SERVER_IO=epoll` forces the fallback.

## Past the file-descriptor limit, in one process

Every user holds a keep-alive connection, and the service gets `LimitNOFILE=65535`. An ordinary server
therefore stops at about 65k users. This one doesn't use file descriptors for client sockets at all.

**Connections are io_uring direct descriptors.** They're accepted with a multishot accept straight into
a ring's registered file table and never get a file descriptor. The kernel limits each table to
`RLIMIT_NOFILE` entries, but the limit applies per table, not in total. So the server runs **two rings on
one thread**, giving 2 × 65,535 connection slots. New connections go to one ring until its table is
nearly full, then to the next. A ring other than the first signals an eventfd that the first ring
polls, so the loop only ever blocks in one place. With 100,000 connections open, the process had 48
file descriptors.

**It also saves system calls.** Receives are multishot and land in a shared pool of provided buffers,
so idle connections hold no buffer. Everything one loop iteration queues (sends, re-armed receives,
closes) goes to the kernel in a single `io_uring_enter`. Closing a connection is a hard-linked
cancel-then-close, because a pending multishot receive keeps its own reference to the socket.

**Fallback.** If io_uring can't be set up (disabled by sysctl, blocked by seccomp as in Docker's
default profile, or an older kernel), the same server runs on epoll. It then has one descriptor per
connection, so it holds about 65k. Both backends share everything above the socket: parsing, routing,
rendering and the group commit.

## The rest of the design

**One event loop on one thread.** There's no goroutine per connection. An idle connection is a small
struct plus its kernel socket.

**Direct, not behind Nginx.** Nginx's `worker_connections 16384` would cap the server at about 16k
users, and Nginx would take CPU away from it.

**The SQLite work is in C, with one cgo call per request.** `fastdb.c` binds, steps and resets the
prepared statement, then writes the JSON body straight into the output buffer. A feed costs one Go→C
crossing instead of about 120 (a step and five column reads per row with a typical driver).

**Group commit.** Likes and new posts are collected while the loop works through one batch of events.
They run in a single `BEGIN IMMEDIATE … COMMIT`, and only after the commit are their responses sent
(rule 6).

**Checkpoints off the event loop.** After a commit pushes the WAL past 1,000 frames, a second thread
runs a PASSIVE checkpoint on its own connection. Meanwhile reads keep flowing, but the next group
commit waits for the checkpoint to finish. Two dead ends led here:

- An inline checkpoint's fsyncs stalled every request for 35–60 ms each time.
- A free-running background checkpointer let the WAL grow without bound (300 MB in 3 minutes), because
  SQLite only rewinds the WAL once a checkpoint has caught up with every frame.

The WAL stays around 4 MB.

**Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 32 MiB page cache.
**SQLite compile options** come from SQLite's recommended list: `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`,
`OMIT_DEPRECATED`, `OMIT_SHARED_CACHE`, `OMIT_PROGRESS_CALLBACK`, `OMIT_DECLTYPE`, `OMIT_LOAD_EXTENSION`,
`USE_ALLOCA`, `DQS=0`, `MAX_EXPR_DEPTH=0`.

**JWT**: HS256 only. The signature is checked on every request with `crypto/hmac` (rule 5: nothing
about tokens is remembered), and `exp`/`nbf` are checked.

## Measured locally

The server ran in Docker on an Apple-silicon Mac, pinned to one core with 2 GB, no swap and
`nofile=65535`. A load generator that copies `bench/load.js` user behaviour (one keep-alive connection
per user, same think times and like/post probabilities) ran in separate containers on other cores.

| Users | Backend | p95 | p99 | Errors | Result |
|---|---|---|---|---|---|
| 20,000 | epoll | 1 ms | 6 ms | 0 | pass |
| 60,000 | epoll | 4–11 ms | 129–235 ms | 0 | 3 of 5 runs pass (shown: the passing ones) |
| 60,000 | io_uring | 41–188 ms | 573–884 ms | 0 | 3 of 5 runs pass (shown: the passing ones) |
| 100,000 | io_uring | 15–19 s | 38 s | 0.2% | fail, but see below |

The 100,000-user run held every connection, but this machine couldn't run it fairly. The two load
generators pushed the VM's load average to 11 on 10 cores, and the server thread was involuntarily
preempted about 1,000 times a second. The 60k runs also varied a lot from run to run for the same
reason. A single M-series core is faster than a droplet vCPU, and kernel network work partly ran on
other cores here, so none of this predicts the droplet well. **How many users past 65k one vCPU can
actually serve hasn't been measured on real hardware yet.**

## License

MIT, under the repo's [license](../../LICENSE).
