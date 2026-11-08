# Rust io_uring feed server

**Language:** Rust 1.94.0, `io-uring` 0.7.15, libc.
**Transport:** raw Linux io_uring with direct file descriptors. `SHARDS` independent listeners/rings, all polled by one event-loop thread.
**Driver:** vendored SQLite 3.46.1 amalgamation, called directly through the C API and SQLite B-tree cursors. No `rusqlite`.
**Serving:** direct on `HOST:PORT`; no Nginx.

## Architecture

- One event-loop thread owns HTTP parsing, JWT verification, SQLite reads, and all ring I/O. A writer thread batches database writes. `SHARDS` controls the number of io_uring rings/listeners, not thread count.
- Each ring has 4,096 submission entries, 16,384 completion entries, a `SO_REUSEPORT` listener, direct-descriptor slots, and a 2 MiB provided-buffer pool. By default (`RECV_MODE=hybrid`), idle connections use persistent multishot receives. After `ENOBUFS` or another terminal multishot completion, bounded single-shot reads drain the socket before multishot is rearmed. Connection state is indexed directly by registered-file slot; accepted sockets do not consume the process `RLIMIT_NOFILE` budget.
- A pass drains completions, collects writer results, refreshes B-tree cursors, then resolves reads and releases responses. Write responses are queued only after the writer commits. Batch cursors close with their read transaction; each batch reopens them, and `refresh` only updates cursors that remain open.
- Each `/feed` and `/posts/:id` request executes its own B-tree read while it is served. There is no cross-request read or response coalescing and no response/query-result cache.
- Writes use one transaction per writer batch and `json_each` batch inserts with `RETURNING`. A like is an `INSERT ... ON CONFLICT DO NOTHING`; post-existence is checked only for like jobs that produced no row.
- `/feed` and `/posts/:id` use SQLite B-tree cursors, not the SQL VM. Consecutive feed post IDs advance the table cursor; gaps reseek. Like counts come from a direct walk over `likes_post_id_idx`: dense post-id ranges are walked once, while large gaps reseek.
- JSON strings without escapable bytes use a SIMD or word-at-a-time scan before copying; strings requiring escaping use the full encoder.
- Writer prepared statements are cached across batches; parameters are rebound and statements reset for each batch.
- io_uring completions are processed directly, without a per-ring staging vector.
- SQLite runs in WAL mode with `synchronous=NORMAL`. Reads use the SQLite page cache (`READ_CACHE_MB`) and its mmap window (`READ_MMAP_MB`; set `0` to disable mmap).

The adaptive receive-recovery design is informed by `rust-iouring-kensac`
([#264](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264));
the SQLite B-tree approach was compared with `c-iouring-pavlyhalim`
([#245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245)).

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `SQLITE_PATH` | required | Database path |
| `JWT_SECRET` | required | HS256 secret |
| `HOST`, `PORT` | `127.0.0.1`, `3000` | Listen address |
| `SHARDS` | `6` | io_uring rings/listeners polled by the single event loop |
| `RECV_MODE` | `hybrid` | `hybrid`: multishot receive with bounded recovery; `poll`: readiness poll plus single-shot receive; `direct`: per-connection receive buffer |
| `STATS` | unset | `1` enables latency counters |
| `PASS_STATS` | unset | Any value prints per-100-pass timing to stderr |
| `READ_CACHE_MB` | `64` | SQLite page cache for the read connection |
| `READ_MMAP_MB` | `0` | SQLite mmap window for the read connection; `0` disables |
| `WAL_AUTOCHECKPOINT` | `10000` | SQLite automatic checkpoint threshold in WAL pages |

With WAL and `synchronous=NORMAL`, increasing the checkpoint threshold can extend
the window of committed transactions vulnerable to a power loss or system crash.
Set `WAL_AUTOCHECKPOINT=1000` for the prior checkpoint cadence.

## Build and run

```bash
bash install.sh   # Rust 1.94.0, build-essential
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` verifies the pinned Rust toolchain tarball with SHA-256 before installing it.

## Verification

```bash
bash test/run.sh submissions/rust-iouring-rkkautsar
```

For a local capacity run on a 12-CPU Docker host, build the release binaries and
run `USERS=320000 bash submissions/rust-iouring-rkkautsar/bench-240k.sh` from the
repository root. The script uses a fresh seed database, one server CPU, 11 client
workers, a shared ramp and a 30-second measurement window by default. After the
hold, sockets are released over 30 seconds rather than all at once.
`USERS=300000` selects 300,000 connections. `HOLD_SECONDS=300` selects a
five-minute hold. The server binary defaults to six rings; the benchmark selects
its own count when SHARDS is unset. With
`VERIFY_CONNECTIONS=1`, kernel socket snapshots from all namespaces must match
by TCP four-tuple; snapshots alone do not establish continuous liveness.
The server, client and network/observer helpers share a Docker-host cgroup
limited by default to 6 GiB RAM, no swap and eight CPUs' aggregate bandwidth.
An OOM at the aggregate limit kills the cgroup as a unit. A host available-memory
check runs before startup, but there is no continuous host-memory monitor.
`BENCH_MEMORY_LIMIT_MB`, `MIN_HOST_AVAILABLE_MB`, and `BENCH_CPU_LIMIT` override
the limits; the CPU quota must leave at least two CPUs' worth of host capacity.
These limits reduce resources available to the clients and server and may lower
measured capacity. The 11 client processes share one container and network
namespace, with a distinct veth and source IP per process. The setup helper uses
the local `lgipimg:latest` image with `ip`, `ss`, and `nsenter`; it and the
client container are privileged to configure links and client receive steering.
The server container is not privileged. Results include aggregate latency and
client scheduling/packet-queue diagnostics. Server and clients share the host
kernel, so this local capacity measurement is not an official benchmark score.

A local 320,000-connection run with six rings and a 10,000-page WAL autocheckpoint
passed twice at a 30-second hold (p95 99.1 ms / p99 133.8 ms, then p95 129.4 ms /
p99 164.9 ms, zero errors, over 1,000,000 requests each) and once at a
five-minute hold using the binary's default ring count with no SHARDS override
(p95 142.3 ms, p99 256.0 ms, zero errors, 10,091,643 requests). In every run all
workers held the full 320,000 active target and six independent kernel snapshots
matched 320,000 client/server socket tuples. The server uses one CPU; the
aggregate cgroup allowed eight CPUs and 6 GiB. These are local measurements, not
official scores or proof of continuous kernel socket occupancy between snapshots.

The submission passes all 42 checks on a fresh seed database. Every request reads its own data from SQLite while served; there is no cross-request coalescing or response cache.
