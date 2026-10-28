# Ada on a $12 server

We wanted to bring a less usual language to the challenge, so we picked Ada
and started squeezing. That turned into a small HTTP server, some time spent
chasing allocations, and a lot more attention to idle sockets than expected.

The implementation uses **Ada 2022**, with no web framework: HTTP and JSON are
written in Ada, sockets use Linux libc bindings, SQLite uses direct C API
bindings, and OpenSSL handles HS256 verification. It targets Ubuntu 24.04 x86_64
and **serves directly on `0.0.0.0:80`**.

## What we squeezed

We started with one event loop. Then we tried keeping a few hundred thousand
connections open, ran into the per-process file limit, and kept going.

- Six worker processes now run nonblocking **epoll** loops with `SO_REUSEPORT`
  listeners. Each has room for 65,000 connections within the 65,535 file limit.
  All six share the same CPU and RAM allowance.
- Getting the workers to take turns mattered. A shared mutex with priority
  inheritance serializes ready-event batches and avoids the long waits we saw
  when one worker repeatedly took the lock back. No CPU affinity or process
  priorities are configured.
- Went direct to save the proxy overhead. Keep-alive and pipelining stay in the
  Ada server, with a small request budget per connection. Idle connections stay
  open for 75 seconds; unfinished requests and responses get a separate deadline.
- Input buffers grow only when needed. Idle connections keep no input buffer,
  and each worker reuses one empty 4 KiB allocation. Ordinary JSON string runs
  are copied in bulk, and SQLite prepared statements are reused.
- Feed requests find the newest 20 IDs, then use bounded range queries for rows
  and like counts when those IDs are close together. A single temporary row
  buffer saves more allocations. Sparse IDs or orphaned rows fall back to the
  original query. Every request reads its data again in one read transaction.
- A foreground supervisor shuts down the workers if one fails. Database startup
  takes the same shared lock, which fixed a race while enabling WAL.

SQLite uses WAL with `synchronous=NORMAL`, a 256 MiB mapping, and checkpoints
after 4,000 WAL pages. Writes commit before success responses, the schema and
indexes are unchanged, and no response or query results are cached across
requests. JWT signatures are verified on every authenticated request.

## How far did we get locally?

We got a little carried away with the load testing, too. Our lightweight Python
diagnostic passed a five-minute hold at **332,800 users**. At 336,000, latency
crossed the line. The unchanged challenge k6 script passed **72,000 users**
locally.

**Neither number is an official score.** The larger diagnostic helped us work
on the server after k6's memory use became a problem on the workstation. It
follows the user request mix and think times, but it is a different generator.

| Workload | Users | Hold | p95 | p99 | HTTP errors | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Unchanged k6, three synchronized segments | 72,000 | 5 min | 146.21 ms | 890.24 ms | 0 | Pass in every segment |
| Python diagnostic | 320,000 | 5 min | 116.21 ms | 171.25 ms | 0 | Pass |
| Python diagnostic | 332,800 | 5 min | 428.14 ms | 481.11 ms | 0 | Pass |
| Python diagnostic | 336,000 | 5 min | 554.41 ms | 675.69 ms | 0 | Fails p95 |

The k6 columns show the worst segment percentile across ramps and hold; the
Python columns use merged hold samples, including connection time. All 332,800
users stayed active throughout the hold, with 10.3 million requests, no response
check failures, clean client exits, and 1.40 GiB peak server cgroup memory.

These runs used an x86_64 binary under Rosetta on Apple Silicon, with the server
container limited to one CPU and 2 GiB RAM, no swap, and a private loopback
network shared with the generators. That does not model every OS and network
cost of a separate one-vCPU VM. Larger k6 runs failed latency checks amid
generator GC pauses and host compression pressure.

The actual challenge VM, with k6 on a separate machine, still gets the final
say. Looking forward to seeing how Ada does there.

## Build and run

Package versions are pinned in [versions.env](versions.env):

| Dependency | Ubuntu package version |
| --- | --- |
| GNAT/GCC and libgnat | 13.3.0-6ubuntu2~24.04.1 |
| SQLite | 3.45.1-1ubuntu2.8 |
| OpenSSL | 3.0.13-0ubuntu3.16 |

From this directory, install once as root, then build as a normal user:

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/absolute/path/to/private-feed-copy.db \
JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` runs in the foreground. Use a writable copy of the seed database,
with permission to create its WAL and SHM files. The challenge service provides
`CAP_NET_BIND_SERVICE` for port 80. For an unprivileged local run, use
`HOST=127.0.0.1 PORT=3000`; these are also the defaults.

## Local verification

`bash validate-container.sh` rebuilds and runs the official 42-check API suite,
HTTP and JSON edge cases, idle keep-alive tests, Nginx integration, worker
supervision, repeated fresh starts, and feed comparisons after external database
edits. API and extended correctness checks passed on x86_64 and native ARM;
Nginx integration also passed on x86_64. The challenge target remains x86_64.

`bash local.sh up` starts a local server at `http://127.0.0.1:3001` with its own
database copy. The tools in `bench/` run the unchanged k6 workload or the separate
Python diagnostic and retain source hashes, resource limits, and client results
under the ignored `benchmark-results/` directory. Raw measurements and build
output stay out of the source submission.
