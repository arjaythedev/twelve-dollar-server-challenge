# Profiling phase 12

Where the CPU and the memory go after phase 12, to choose phase 13. The question was: is Doctrine's hydration holding
the write endpoints (`POST /posts/{id}/like`, `POST /posts`) back? Measured 2026-10-19 (memory) and
2026-10-21 (CPU), same setup as LADDER.md.

**In short**: no. Hydration is 11% of a like, about 1% of PHP's CPU. Doctrine as a whole is about half of a like and
a third of a post, 4.8% of PHP's CPU in worker mode. And worker mode doesn't stop on CPU: its memory is Caddy's,
about 31 KiB per open keep-alive connection, with PHP at about 40 MiB whatever the load.

## CPU

Excimer samples the PHP threads' CPU clock every 0.1 ms (see `profile/excimer.php`), under the real load mix of
`bench/load.js`: per user loop, 2 reads, a like 15% of the time and a post 2% of the time. Without warm-up, 30 s
ramp-up, 2 min hold.

```sh
docker build -f local-bench/profile/Dockerfile -t twelve-dollar-php-symfony-profile local-bench/profile
IMAGE=twelve-dollar-php-symfony-profile \
  SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker:/app/Caddyfile:ro" bash local-bench/bench.sh start
WARMUP=0 RAMP_UP=30s DURATION=2m bash local-bench/bench.sh load 30000 profile-worker
mkdir profile-worker && docker cp twelve-dollar-server:/tmp/profile/stacks.txt profile-worker/
python3 -I local-bench/profile/analyze.py profile-worker PostController::like PostController::create
python3 -I local-bench/profile/split.py profile-worker PostController::like
python3 -I local-bench/profile/zoom.py profile-worker PostController::like
```

### Worker mode, 30,000 users

Well below its limit (77,500): 61% hold CPU, p95 3.6 ms, 2,639 requests/s, 720,563 samples.

| Share of PHP samples | |
|---|--:|
| `HttpKernel::handle` (the rest: FrankenPHP's request loop, response sending, worker boot) | 45.9% |
| `GET /feed` | 12.4% |
| `GET /posts/{id}` | 4.3% |
| `POST /posts/{id}/like` | 8.5% |
| `POST /posts` | 1.0% |
| Firewall (POST only) | 2.8% |

Inside the write controllers (`split.py`: a sample ending in PDO counts as SQLite's, even inside hydration or a
flush):

| Share of the controller | Like | Post |
|---|--:|--:|
| SQLite: PDO commit, execute, fetch | 39.9% | 57.6% |
| Hydration (`AbstractHydrator::hydrateAll`) | 10.6% | 0.0% |
| `find()`/`findOneBy()` besides hydration (SQL, parameters, identity map) | 18.0% | |
| `flush()` besides SQLite (change sets, insert order, persister) | 13.8% | 18.4% |
| Reading back the generated `created_at` after the insert | 5.4% | 4.5% |
| `persist()`, `getReference()` | 4.5% | 8.2% |
| JSON response | 6.7% | 9.7% |

So in worker mode, of all PHP samples:

- hydration: 8.5% × 10.6% = **0.9%** (2.0% of `HttpKernel::handle`);
- all of Doctrine's own work in the writes (hydration, find, flush, created_at, persist): **4.8%** (10% of
  `HttpKernel::handle`). That's the most that DBAL writes could save, before Caddy's and the kernel's share of the
  server's CPU, which Excimer doesn't see.

### Classic mode, 8,000 users

52% hold CPU, p95 7.1 ms, 704 requests/s (profile Caddyfile: `profile/Caddyfile`).

Likes are 56.1% of PHP samples and posts 10.1%, 92% and 97% of it in PDO, mostly `PDO\Connection::commit`: about
0.68 ms of CPU per like, against 0.19 ms in worker mode (a like is 15% of the k6 iterations). Classic mode opens a
new SQLite connection per request.

Take this profile with care: it shows less Doctrine time per like than worker mode does, which can't be, so short
frames are undercounted (about 0.2 samples per feed request, Symfony's boot at 0.2%). The kernel checks the timer on
its tick, about every 1 ms here, and each request starts its own: whatever CPU a request uses after its last tick is
never sampled, a large part of a 1-2 ms request. What it does show is that SQLite commits dominate the writes in
classic mode, not Doctrine.

## Memory

LADDER.md: worker mode is memory-bound from phase 6 (the 2 GB are full at its limit). Snapshots with
`profile/memory.sh`, idle, then 90 s into the hold at 60,000 users (under the rules' 65,535 open files):

```sh
SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker-pprof:/app/Caddyfile:ro" bash local-bench/bench.sh start
bash local-bench/profile/memory.sh mem-idle
WARMUP=0 RAMP_UP=60s DURATION=3m bash local-bench/bench.sh load 60000 mem-worker &
sleep 150; bash local-bench/profile/memory.sh mem-hold
docker run --rm -v "$PWD/mem-hold:/p:ro" golang:1.25.3-alpine go tool pprof -top -sample_index=inuse_space /p/heap.pb.gz
```

| Container (cgroup `memory.stat`) | Idle | 60,000 users |
|---|--:|--:|
| Total | 594 MiB | 1,952 MiB |
| Page cache (reclaimable) | 552 MiB | 77 MiB |
| Anonymous (heaps, stacks) | 41 MiB | 1,828 MiB |
| Shared memory (OPcache) | 29 MiB | 32 MiB |
| Socket buffers | 0 | 0 |
| Goroutines | 15 | 60,018 |
| Open files | 13 | 60,014 |

Go's `runtime.MemStats` at 60,000 users accounts for all but about 30-40 MiB of the anonymous memory:

| Go memory | Total | Per connection |
|---|--:|--:|
| Goroutine stacks (one per connection; 59,832 share one stack, the idle ones) | 547 MiB | 9.3 KiB |
| Live heap | 631 MiB | 10.8 KiB |
| of which `net/http`'s `bufio` reader and writer (4 KiB each, kept for the connection's life) | ~480 MiB | 8 KiB |
| Heap kept but unused (GC headroom: `HeapIdle` - `HeapReleased`) | 427 MiB | 7.3 KiB |
| Runtime metadata (spans, GC, other) | ~70 MiB | |
| **Resident, all of Go** | **~1,800 MiB** | **~31 KiB** |

The heap profile: `bufio.NewWriterSize` 42%, `bufio.NewReaderSize` 40%, then contexts, `net.newFD` and
`http.(*Server).newConn` for the rest, all under `net/http.(*conn).serve` (91%).

So:

- worker mode's limit is about 31 KiB per open keep-alive connection, all of it Caddy's and Go's: 2 GB is full near
  60,000 connections. PHP (2 workers, Symfony, Doctrine, SQLite's caches) is the ~40 MiB left, at idle as under load.
  No change to the PHP code moves this limit;
- near the limit, the kernel drops the page cache (552 to 77 MiB), which holds the database file: SQLite's reads
  probably start going to disk;
- the kernel's socket buffers aren't charged to the container here (OrbStack); on the droplet they come out of the
  same 2 GB, so it has less room still;
- the GC headroom could only shrink by collecting more often: phase 10's `GOMEMLIMIT=1100MiB` did that and lost
  users (65,500 to 51,500). FrankenPHP's own limit, 90% of the container's memory, is already the better trade;
- Caddy has no setting for the `bufio` sizes or the goroutine stacks;
- what's left: fewer open connections. A k6 user is idle 5-15 s between loops and 3-8 s between requests, so with an
  idle timeout of a few seconds most users would hold no connection most of the time, for a TCP handshake per
  reconnect.

The full goroutine dump (`goroutine?debug=2`) isn't in `memory.sh`: building that 64 MB text inside the server, at
1.95 GB, got it OOM-killed.

## Group commit, not measured

Some submissions group commits: one process gathers several requests' writes into one transaction. Not tried here,
for these reasons:

- classic mode can't: each request is its own PHP run, with its own SQLite connection, and nothing is shared between
  requests. Grouping would take a separate writer process, which is neither classic mode nor idiomatic Symfony;
- worker mode has little to group. The submissions that do it are async (Swoole, FastAPI), with thousands of requests
  in one event loop, many of them writing at the same time. FrankenPHP's 2 workers handle one request each, so a
  batch holds 2 writes at most. And commits are about 2.1% of PHP's CPU (22% of a like, likes being 8.5%), in a mode
  stopped by memory, not CPU;
- commits are already cheap: in WAL mode with `synchronous=NORMAL`, a commit doesn't fsync (only checkpoints do).
  Saving fsyncs is what usually makes group commit pay; here it would only save some writes to the WAL file;
- it's outside the line this submission keeps: no group commit, like no custom HTTP server or hand-written JSON.

What a single user, with no one else, would see depends on how the batch is built:

- a time window (wait up to N ms for more writes, then commit): every write waits N ms, for nothing. That one only
  pays under a benchmark;
- a leader and followers (the first writer commits whatever is queued, and writes arriving meanwhile go in the next
  commit): a lone write commits at once, so a single user pays nothing. Batches only form when writes pile up, so the
  gain grows and shrinks with the load.

Either way, each response must wait for its own write's commit (answering first and committing later breaks the
durability rule), and one failing write, a duplicate like on the unique key for instance, must not roll back the
others in its batch (`ON CONFLICT DO NOTHING` or savepoints).

## For phase 13

- Doctrine-free writes (DBAL): at most ~10% of `HttpKernel::handle` in worker mode, where CPU isn't the limit; little
  in classic mode, where SQLite's commits are the cost.
- Caddy's idle timeout: the one change that could move the worker limit, since memory goes with open connections.
- Group commit: no (see above).
