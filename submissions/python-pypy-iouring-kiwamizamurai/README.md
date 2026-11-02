# Python on PyPy, raw io_uring, SQLite's B-tree cursors

| | |
|---|---|
| Language | Python (3.9 dialect) on **PyPy 7.3.15**, Ubuntu 24.04's `pypy3` package |
| Framework | none: the HTTP/1.1 server and the io_uring rings are Python in `server.py` |
| SQLite | unmodified 3.54.1 amalgamation, SHA-256 pinned, compiled by `build.sh` into a cffi module |
| JWT | HMAC-SHA256 through OpenSSL's one-shot `HMAC()` via cffi |
| Third-party packages | none beyond PyPy's bundled cffi |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.4+ (multishot timeout) with io_uring enabled. There is no epoll fallback |

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## What is C and what is Python

All request handling, ring handling and record decoding is Python, compiled by PyPy's JIT. The C in the process is:

- SQLite itself, unpatched. `-DSQLITE_PRIVATE=` makes its internal B-tree functions linkable; it is a compile flag.
- The wrappers cffi generates, plus about 25 lines in `build_sqlite.py` that this submission does write by hand:
  syscall wrappers, two `Mem`/`CollSeq` accessors and atomic loads and stores. They hold no server logic.
- OpenSSL's `HMAC()`.

## Design

1. **io_uring from Python.** SQEs and CQEs are read and written as `uint64` arrays over the mapped rings. Completions
   are handled in batches and each batch's submissions go in one `io_uring_enter`.
2. **Direct descriptors.** Sockets live in fixed-file tables, so `LimitNOFILE=65535` caps a table, not the server. Up to
   8 rings of about 65,000 slots are created as connections need them. No connection is recycled in normal operation.
3. **Multishot accept and receive with provided buffers.** A connection holds no Python object while idle, only a few
   bytes in per-ring bytearrays. A receive that ends with `ENOBUFS` goes on a queue and is re-armed in bounded groups.
4. **Reads through SQLite's B-tree cursors** (allowed by the organizer's 2026-10-31 ruling), inside one read transaction
   per batch. `/feed` takes the 20 newest ids from `posts_created_at_id_idx`, then does one range scan of
   `likes_post_id_idx` and one of `posts`. If a record isn't shaped as expected, the same request runs the SQL statements.
5. **Group commit.** A write that arrives alone in its read runs in the batch's transaction, and its response is queued
   only after `COMMIT`. Pipelined writes commit one at a time so their responses stay in order.
6. **Nothing is cached across requests.** Every read goes to SQLite, and every token's signature and `exp` are checked
   on every request. A token without `exp` is rejected.

Pragmas: `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `locking_mode=EXCLUSIVE`, `mmap_size=1GiB`,
`cache_size=32MiB`.

## Results (not the official box)

All numbers come from one arm64 Mac running Linux 7.2 under OrbStack, over loopback, with the load generator from
`python-pypy-epoll-pranavds-codes` (`bench/connection_load.c`, which replays `bench/load.js`'s user loop). This is not k6
and not the benchmark VM, so only the comparison between the two rows means anything. Absolute numbers are far higher
than official ones: #84 officially holds 90,000 and held 150,000 here.

5-minute hold after a 60 s ramp, server limited to one CPU and 2 GB by its cgroup, load from a separate container:

| Submission | Users | req/s | p95 | p99 | Errors | Connects | Peak cgroup memory |
|---|---:|---:|---:|---:|---:|---:|---:|
| this submission | 250,000 | 26,338 | 140 ms | 308 ms | 0 / 7,901,301 | 250,000 | 299 MB |
| python-pypy-epoll #84 | 150,000 | 15,790 | 60 ms | 611 ms | 24 / 4,737,027 | 4,364,200 | 561 MB |

Peak memory may understate the box: top entries report about 1 GB at similar user counts there, and this setup differs in
kernel and network path. It hasn't been measured on a real NIC.

60-second holds with the server pinned to one CPU and no memory limit. These ran before the `Date` header and the other
HTTP fixes, and neither limit has been searched with a 5-minute hold:

| Users | #84 | this submission |
|---:|---|---|
| 200,000 | fail (p99 3.4 s) | not run |
| 250,000 | fail (p99 13 s) | pass (p99 384 ms) |
| 285,000 | not run | pass (p99 757 ms) |
| 320,000 | not run | fail (p99 4.0 s) |

Closed-loop `wrk`, 256 connections, one server CPU, same early build:

| Mix | #84 req/s | this submission req/s |
|---|---:|---:|
| mixed | 40,638 | 51,346 |
| `GET /feed` | 23,389 | 29,722 |
| `GET /posts/:id` | 110,964 | 198,661 |
| like | 38,280 | 64,384 |
| create | 12,362 | 47,374 |

## Known limits

- **Not run on x86_64 yet.** It builds there, but the only x86_64 environment used so far is Rosetta, where
  `io_uring_setup` returns `ENOSYS`, so serving has only been exercised on arm64.
- **Not run on kernel 6.8,** the benchmark box's kernel. The test kernel was 7.2. The registration retry for Ubuntu's
  6.8.0-136+ kernels (below) has not been exercised.
- **New-connection-per-request load is slower than #84:** 22,638 against 33,942 req/s (`Connection: close`, 64
  connections, early build), with the server at 73 % CPU. The cause isn't known. k6 keeps connections open, so the
  scored load doesn't take this path.

## Tests

On a clean `ubuntu:24.04` container (arm64): `install.sh`, then `test/run.sh` as a non-root user: 42/42. The HTTP edge
cases in `python-pypy-epoll-pranavds-codes/bench/http_edge.py`: 25/25. Also checked by hand: 40 concurrent likes by one
user give one 201 and 39 200s, idle connections are closed and their slots reused.

## Credits

- [python-pypy-epoll-pranavds-codes](../python-pypy-epoll-pranavds-codes) (#84): this submission's HTTP parsing, strict
  JSON loading, JWT checks and SQL statements are adapted from `server.py`, and its load generator and edge-case tests
  were used for the numbers above. Its load generator comes from #13 (tbsvttr).
- [c-iouring-pavlyhalim](../c-iouring-pavlyhalim) (#245): the B-tree read plan, several listeners sharing the port,
  several rings of direct descriptors, the accept roll-over thresholds, and the retry with a nonzero reserved byte for
  the provided-buffer registration on Ubuntu's 6.8.0-136+ kernels (Launchpad #2162843). The code here is Python, written
  from those descriptions.

MIT, under the repo's [license](../../LICENSE).
