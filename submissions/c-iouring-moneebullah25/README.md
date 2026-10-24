# C + io_uring + SQLite (direct, port 80)

| | |
|---|---|
| Language | C (gnu17), gcc, `-O3 -march=native -flto` (PGO available as `make pgo`, not used: no measured gain) |
| Framework | none: liburing 2.15 (static, built from the pinned release tarball) |
| Database | SQLite 3.54.1 amalgamation, `THREADSAFE=2`, in-process |
| Nginx or direct | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the box) |
| Threads | one event-loop thread plus one `SCHED_IDLE` checkpoint thread |

`install.sh` installs build-essential, curl, unzip and openssl; `build.sh`
fetches and checksum-verifies SQLite and liburing into `third_party/` and runs
the LTO release build (`make release`); `start.sh` runs
`bin/server`, configured only by `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`.

## What it does

- **One io_uring per 65,535 connections.** The kernel caps a ring's
  fixed-file table at the hard `RLIMIT_NOFILE`, so accepted sockets go into
  direct descriptors across 7 rings (450k slots by default) and never become
  process fds. Multishot accept hands over to bounded one-shot accepts near the
  end of a table so no accepted socket is lost to `-ENFILE`.
- **Multishot receive from provided buffers**, 2 KiB each; an idle connection
  owns no buffer and costs 48 bytes of user-space state. Responses go into a
  pool of 1,024 x 72 KiB send buffers, sized for the largest legal feed. `ENOBUFS` victims are
  re-armed only after buffers return, bounded by how many returned.
- **One batch per wake-up**: wait for 32 completions or 250 us, handle up to
  256 requests, run all reads in one deferred transaction, all writes in one
  `BEGIN IMMEDIATE ... COMMIT`, and only then submit the sends (rule 6: a 201
  is sent after its commit returned; a failed commit turns every write of the
  batch into a 500).
- **Feed as three range scans** (20 newest ids from the covering index, one
  likes range grouped by post, one posts+users range) with the reference query
  as the fallback; `GET /posts/:id` is the reference query. Every value is read
  on the request that serves it; nothing is cached across requests (rule 5),
  and every token's HMAC is recomputed.
- **SQLite**: WAL, `synchronous=NORMAL`, `mmap_size=0`, 32 MiB page cache,
  `unix-excl` VFS (heap WAL index, no fcntl per transaction),
  `wal_autocheckpoint=0`; passive checkpoints on the idle thread every 4,000
  frames, and a short *write hold* (reads continue) every 8,192 frames so the
  WAL restarts instead of growing without bound.
- **Memory guard**: pauses accepting below 192 MB `MemAvailable` (or cgroup
  headroom) and sheds idle keep-alives below 96 MB, so running out of memory
  becomes a failed probe instead of an OOM kill.
- HTTP/1.1 keep-alive with pipelining, partial requests, half-close after a
  request, `Expect: 100-continue`, `Date` header, 8 KiB / 16 KiB limits (413),
  30 s timeout for incomplete requests and unread responses, TCP keep-alive
  probes for vanished peers, idle connections never timed out.

`docs/DESIGN.md` explains each decision and the rule it respects.

## What was measured, and on what

Everything was measured on a 2-core AMD Athlon Silver 3050U (Zen1, 2.3 GHz,
14 GB, Ubuntu kernel 6.8.0-147), with the load generator on the same machine,
which is slower per core than the batch-2 Xeon 8260 and cannot emulate 1 vCPU /
2 GB. **No user-count score is claimed.** The numbers in `docs/RESULTS.md`
(each with its command and date) cover: correctness, CPU microseconds per
request by endpoint, memory per idle connection, how many connections were
held locally, latency percentiles, and checkpoint/hold behaviour. The entries
labelled "estimated" are extrapolations and say so.

Short version: see `docs/RESULTS.md`, section "Summary".

## Testing

```
bash ../../test/run.sh submissions/c-iouring-moneebullah25   # the official 42 checks
test/run_all.sh asan        # unit tests, fuzz pass, official suite, 148 black-box checks, all under ASAN/UBSAN
test/run_all.sh release --long   # same with the release binary and the 66 s keep-alive check
```

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
- `docs/STATUS.md` lists what remains unverified and the known risks.
