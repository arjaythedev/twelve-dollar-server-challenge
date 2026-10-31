# C + io_uring + SQLite (direct, port 80)

| | |
|---|---|
| Language | C (gnu17), gcc, `-O3 -march=native -flto` with PGO trained on a synthetic database at build time (falls back to plain LTO if training fails) |
| Framework | none: liburing 2.15 (static, built from the pinned release tarball) |
| Database | SQLite 3.54.1 amalgamation, `THREADSAFE=2`, in-process |
| Nginx or direct | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the box) |
| Threads | one event-loop thread plus one nice-10 checkpoint thread |

`install.sh` installs build-essential, curl, unzip and openssl; `build.sh`
fetches and checksum-verifies SQLite and liburing into `third_party/` and runs
the PGO build (`make pgo`, training on a synthetic database); `start.sh` runs
`bin/server`, configured only by `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`.

## What it does

- **One io_uring per 65,535 connections.** The kernel caps a ring's
  fixed-file table at the hard `RLIMIT_NOFILE`, so accepted sockets go into
  direct descriptors across 7 rings (450k slots by default) and never become
  process fds. Multishot accept hands over to bounded one-shot accepts near the
  end of a table so no accepted socket is lost to `-ENFILE`.
- **Multishot receive from provided buffers**, 2,048 x 1 KiB per ring; an idle
  connection owns no buffer and costs 48 bytes of user-space state. Responses go
  into a pool of 1,024 x 72 KiB send buffers (faulted in lazily), sized for the
  largest legal feed.
  `ENOBUFS` victims are re-armed only after buffers return, bounded by how many
  returned.
- **Time-paced passes**: once busy, the loop waits for 512 completions or 16 ms,
  then reaps every ring once and handles up to 512 requests: all reads in one
  deferred transaction, all writes in one `BEGIN IMMEDIATE ... COMMIT`, and
  only then the sends (rule 6: a 201 is sent after its commit returned; a
  failed commit turns every write of the batch into a 500). Secondary rings
  wake ring 0 through a one-shot poll re-armed only after the ring is drained,
  so completions on other rings never shorten a pass. (The scored version woke
  on any ring's completion and ran passes of a few requests once several rings
  were busy: see `docs/RESULTS.md`.)
- **Feed and single-post reads through SQLite's B-tree cursor API**
  (`src/db_btree.c`, compiled into the amalgamation's translation unit): the 20
  newest ids from the created-at index, one likes-index range scan tallied in C,
  post and author rows by rowid seek, JSON written from the page bytes. Any
  unusual record (overflow payload, fewer than 20 rows, id span over 1,024,
  missing author, schema change) falls back to the prepared SQL path for that
  request, and `DB_READS=sql` turns the direct path off. Writes stay prepared
  SQL. Every value is read from SQLite's pages on the request that serves it;
  nothing is cached across requests (rule 5), and every token's HMAC is
  recomputed. Two other entries (#153, #158) use the same technique; the
  organizer's rulings on those entries apply here too.
- **SQLite**: WAL, `synchronous=NORMAL`, `mmap_size=0`, 4 MiB page cache,
  `unix-excl` VFS (heap WAL index, no fcntl per transaction),
  `wal_autocheckpoint=0`; passive checkpoints on the nice-10 thread every
  1,000 frames, and a short *write hold* (reads continue) every 8,192 frames
  so the WAL restarts instead of growing without bound.
- **Shedding**: instead of pausing accepts when memory runs low, responses
  carry `Connection: close` for a fraction f of requests, so a user holds no
  socket during its think time and reconnects for its next request. f is the
  larger of a memory law (0 at or above 450 MB available, 1 at or below
  200 MB) and a connection-count law (0 at 90% of `--conn-cap`, 1 at the cap;
  default cap 260,000). Accepting pauses only below 96 MB available. Closes are
  rate-limited, and keep-alives idle longer than 60 s are closed
  (`--idle-timeout-s`). This is general memory protection for any client; HTTP
  clients reconnect transparently after `Connection: close`.
- **Stats line**: every 10 s (`--stats-interval-s`, 0 turns it off) the server
  writes one line to stderr (the journal under systemd): open connections,
  request count, cpu_us per request with user and system parts, rss, available
  memory, `shed_f`, `shed_closes`, `idle_closes`, accepts (total and per
  interval), `avail_mb`, WAL frames, holds and `btree_fb`.
- HTTP/1.1 keep-alive with pipelining, partial requests, half-close after a
  request, `Expect: 100-continue`, `Date` header, 8 KiB / 16 KiB limits (413),
  30 s timeout for incomplete requests and unread responses, TCP keep-alive
  probes for vanished peers, idle keep-alives closed after 60 s.

`docs/DESIGN.md` explains each decision and the rule it respects.

## What was measured, and on what

Everything was measured on a 2-core AMD Athlon Silver 3050U (Zen1, 2.3 GHz,
14 GB, Ubuntu kernel 6.8.0-147), with the load generator on the same machine,
which is slower per core than the batch-2 Xeon 8260 and cannot emulate 1 vCPU /
2 GB. **No user-count score is claimed.** The numbers in `docs/RESULTS.md`
(each with its command and date) cover: correctness, CPU microseconds per
request by endpoint, memory per idle connection, how many connections were
held locally, latency percentiles, and checkpoint/hold behaviour. The entries
labelled "estimated" are extrapolations and say so. Shedding, the 16 ms batch
window and the B-tree reads were measured locally only (10,000 to 20,000
connections); nothing was measured at box scale since they were added, and the
box figures for them in `docs/STATUS.md` are estimates.

Short version: see `docs/RESULTS.md`, section "Summary".

## Testing

```
bash ../../test/run.sh submissions/c-iouring-moneebullah25   # the official 42 checks
test/run_all.sh asan        # unit tests, fuzz pass, official suite, 177 black-box checks, all under ASAN/UBSAN
test/run_all.sh release --long   # same with the release binary and the 66 s keep-alive check
```

`tools/dbdiff.c` compares the B-tree read path with the SQL path byte for byte
(feed and every post id, before and after random writes).

`tools/mload.c` is a closed-loop load generator with the `bench/load.js` user
model that holds 100k+ idle keep-alive connections from one machine (rotating
127.0.0.x source addresses); `tools/k6.sh` downloads a pinned k6 and runs the
official script.

## Honesty notes

- Built and tested on kernel 6.8.0-147; the box runs 6.8.0-117 (same series).
  This kernel rejects `IORING_REGISTER_PBUF_RING`, so the classic
  `PROVIDE_BUFFERS` path is the one shipped and tested.
- The multi-ring path was tested with small tables (16, 40 and 4,096 slots);
  the 65,535-slot layout follows from the measured kernel limit, not from a
  65k-connection run on an fd-limited process.
- Shedding and the larger batch window were measured on the 2-core laptop only,
  at 10,000 to 20,000 connections. The box figures for them are estimates.
- `docs/STATUS.md` lists what remains unverified and the known risks.
