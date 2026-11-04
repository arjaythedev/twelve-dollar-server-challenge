# AILANG Native SQLite Feed

A submission written in AILANG, a programming language designed for AI-assisted systems programming.

| | |
|---|---|
| **Language** | AILANG, compiled to a standalone x86-64 Linux binary |
| **Framework** | None. HTTP/1.1 with persistent connections, event-driven (`Library.HTTP1`), plus one front process (`Library.HTTP1Front`) |
| **SQLite driver** | None. `Library.SQLite` reads and writes the SQLite file directly using memory mapping and write-ahead logging. No `libsqlite3`, no shared libraries |
| **Authentication** | `Library.Crypto`: JWT HS256, signature and expiry checked on every authenticated request |
| **Nginx or direct** | **Direct**. Listens on `$HOST:$PORT` (use `0.0.0.0:80`) |
| **Compiler** | AILANG optimizing compiler `ailang-opt11.x`, bundled in `compiler/` (see below) |

## How this was built

This submission was written entirely by AI systems under human direction. The human author, Sean Collins ([2 Paws Machine and Engineering](https://github.com/AiLang-Author)), set the goals, chose the architecture, and ran the benchmarking. The AI wrote the server and the libraries and ran the tests.

AILANG is a language made for this kind of work, and this is its first entry in the challenge. It is submitted so that anyone using AILANG has a working, tested reference implementation of a real server that speaks the challenge's exact API.

The folder [`triumvirate-engineering/`](triumvirate-engineering/) holds the working method used: three AI roles (an implementer, a performance and compliance checker, and a code reducer) that review each other's work.

## Build and run

```bash
bash install.sh      # does nothing; there is nothing to install
bash build.sh        # compiles feed_server.ailang with the bundled compiler
SQLITE_PATH=/path/to/copy-of-feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

Each user holds one keep-alive connection, so the open-descriptor limit is the connection limit. On the official box `LimitNOFILE=65535` applies to each process:

- `HTTP_NSLOT` (default 65,520) is the connection table size of each process. It sits just under the descriptor limit, leaving a few descriptors for the server's own files, so a connection beyond the table is accepted and closed at once instead of waiting in the kernel backlog. Raise it only together with the descriptor limit.
- `HTTP_FRONTS` (default 1) is the number of front processes. Each has its own descriptor limit and connection table, so the default holds up to 131,040 connections. Set it to 0 for a single process.

Check it with the challenge's own tests (from the repository root):

```bash
bash test/run.sh submissions/ailang-native-2paws
```

### About the bundled compiler

This submission includes the AILANG compiler binary in `compiler/ailang-opt11.x` because AILANG is a self-hosting language and its compiler is not currently distributed through a standard Ubuntu/Debian package or a stable official toolchain repository. In other words, this is not a normal third-party runtime dependency or an off-the-shelf binary that changes the server's behavior; it is the project's own bootstrap compiler used to build the server.

The compiler source is available upstream in the AILANG self-hosting project, and a source-based build is possible if someone wants to rebuild it locally. The reason the bundled copy is included here is that the optimizer used by `Library.Hash` (specifically `Crc32String`) is not yet published in the upstream self-hosting repo, so the build path is currently a bootstrap convenience rather than a normal package-install flow.

- The bundled binary is an optimizing build that the server needs: `Library.Hash` uses `Crc32String`, which the compiler currently published at [AiLang-Author/Ailang-Self-Hosting-](https://github.com/AiLang-Author/Ailang-Self-Hosting-) does not yet include.
- When the upstream optimizer is published, `build.sh` can switch to the public compiler and the bundled copy can be removed.
- sha256 of the bundled compiler: `eb705f6e60ece1bca1c1e8d3103b1a65be162b6fc63c690d85e1675f2efca407`
- The compiler binary belongs to the AILANG project and is not covered by this repository's MIT license.
- The server binary `feed_server.x` is built by `build.sh` and is not committed.

This is a deliberate bootstrap exception for a self-hosting language whose compiler is not available via normal package channels. The source is available if needed, and the server itself is still built from source via `build.sh`.

## How it works

- Implements all five endpoints. The `/feed` response is byte-for-byte what the tests expect (5,102 bytes).
- The feed reads the 20 newest posts by walking the posts table in reverse primary-key order. No indexes were added.
- Usernames are found through a lookup table of the users table's own B-tree page boundaries, rebuilt at startup. This is the database's structure, not a copy of its data. There is no cache of likes or of users.
- **Batched writes (group commit):** new posts and first-time likes are not committed one by one. All writes handled in one pass of the event loop share a single WAL commit, and their responses are delayed until that commit completes.
- **Front process:** one process owns the database and runs every request. A front process, forked before the database opens, accepts connections on its own `SO_REUSEPORT` socket (the kernel spreads new connections over the two), parses each request and passes it through a ring in shared memory. The database process runs it on a virtual connection, with the same handler and group commit, and the response goes back through a second ring. If the database process falls behind, the front queues requests rather than failing them.
- **Checkpoints, `synchronous=NORMAL`:** every 4 MB of WAL, the pages committed since the last checkpoint are copied back into the database file after the WAL is synced, and the server drops its own copies of them. Every 64 MB the database file is synced and the WAL restarts. A small syncer process does the fdatasync calls, so commits do not wait on the disk. Each response still waits for its commit.
- **Memory:** requests and responses use two shared buffers; a connection only borrows a buffer from a pool while a request or response is incomplete. An idle connection costs its slot (256 bytes) and the kernel's socket memory.

## Test results

**Test machine:** AMD FX-8320 (Bulldozer family, 3.7 GHz, released 2012). This is **not** the official DigitalOcean benchmark machine. The official test suite passes: **42 out of 42**.

k6 runs out of memory above about 60,000 users on this machine, so load was generated with the C load generator from the `c-iouring-raoz` submission (`tests/loadgen.c`), which follows the user loop of `bench/load.js`, on the other seven cores. Pass/fail uses its whole-run figures, ramp-up included, as k6's thresholds do.

### Under the official box's limits

**local, FX-8320, 1 core, MemoryMax=1800M, no swap, LimitNOFILE=65535**

The server ran in a transient systemd scope (`MemoryMax=1800M`, `MemorySwapMax=0`, `CPUQuota=100%`, one core, open files 65,535), on a fresh copy of the seed database each run, with a 60 s ramp. RAM is the scope's `memory.peak`, CPU is the scope's CPU use during the hold. These limits are part of the test harness only; `build.sh` and `start.sh` set none of them.

| Users held 5 min | req/s | p95 ms | p99 ms | err % | CPU % | RAM MB |
|---:|---:|---:|---:|---:|---:|---:|
| **120,000** | **12,685** | **63** | **416** | **0.000** | **94** | **823** |

126,000 users failed the same hold on the 99th percentile (1,028 ms, 0 errors, 96% CPU), so the limit on this machine is between 120,000 and 126,000, set by the CPU. No run had an OOM kill or `memory.high` throttling. After the 120,000 hold, SQLite's own `PRAGMA integrity_check` on the database and WAL returned `ok`, with 38,372 new posts and 287,305 new likes in it.

Memory at 120,000 connections: 823 MB peak, against 33 MB idle, or about 6.6 KB per connection. About 3.8 KB of that is the kernel's own socket memory; the rest is mostly database pages in the page cache.

**Above the connection limit:** each process accepts at most `HTTP_NSLOT` connections. A connection beyond that is accepted and closed at once, so the client sees it closed, and the server keeps serving the connections it has. With a single process (`HTTP_FRONTS=0`) that limit is 65,520; it held 65,500 users with 0 errors. If `HTTP_NSLOT` is set above the descriptor limit, new connections wait in the kernel's backlog instead and the server spends its time retrying `accept`, which is why the default stays under it.

### Without memory or descriptor limits (earlier build)

These were measured on an earlier single-process build with `HTTP_NSLOT=131072`, no memory limit, no descriptor limit, and the server pinned to one core with `taskset -c 0`. They are not reachable on the official box in that form: 120,000 connections need more than the 65,535 descriptors one process may hold there, which is what the front process now provides. That build also had bugs fixed since (see below).

| Concurrent users | 95th percentile | 99th percentile | Errors | Result |
|---:|---:|---:|---:|---|
| 80,000 | 7.8 ms | 95 ms | 0 | pass |
| 100,000 | 1.6 ms | 74 ms | 0 | pass |
| 110,000 | 2.1 ms | 65 ms | 0 | pass |
| **120,000** | **10.4 ms** | **91 ms** | **0** | **pass** |
| 150,000 | failed during ramp-up (99th percentile 1.2 s) | | 0 | fail |
| 200,000 | 4.5 s | 17 s | 0.63% | fail |

### Peak throughput, GET /feed

32 connections for 15 seconds, no pause between requests, one pinned core, same machine, earlier build:

| Server | Requests/sec | Median latency |
|---|---:|---:|
| AILANG (this submission) | 19,468 to 19,552 | 0.196 ms |
| Python FastAPI (`python-fastapi-cknutson12`) | 2,932 | 0.513 ms |

### Fixed while measuring

Checking the database with SQLite itself after load runs found bugs in the earlier build. The challenge's tests passed throughout, since none of these shows up in a fresh database:

- The WAL header had the wrong magic number, so SQLite ignored the WAL and saw none of the server's writes. It also never got a checkpoint, so it grew for as long as the server ran.
- Page 1's page count was never updated, so pages added after the seed were past the end of the file for SQLite.
- Moving a table-leaf cell left out its rowid varint and cut rows short, which corrupted post bodies after a page split.
- An index leaf split kept the separator in the leaf as well as the parent, counting some likes twice; the like check missed likes stored in interior pages.
- Every write leaked memory (the WAL, page copies never released, JWT verification buffers, hash-table bookkeeping), which made long runs slower and larger. A hash table full of deleted entries could lose an insert.

## Files

| | |
|---|---|
| `feed_server.ailang` | The server |
| `Librarys/Library.HTTP1Front.ailang` | The front process and the shared-memory rings |
| `Librarys/` | The AILANG libraries it uses (HTTP, SQLite, JSON, crypto, and so on) |
| `compiler/` | The bundled AILANG compiler (see above) |
| `build.sh`, `start.sh`, `install.sh` | The challenge scripts |
| `triumvirate-engineering/` | Description of the AI working method used, for reference |
