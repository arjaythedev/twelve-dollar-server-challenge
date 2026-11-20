# AILANG Native SQLite Feed

A submission written in AILANG, a programming language designed for AI-assisted systems programming.

| | |
|---|---|
| **Language** | AILANG, compiled to a standalone x86-64 Linux binary |
| **Framework** | None. HTTP/1.1 with persistent connections, on io_uring (`Library.HTTP1`, `Library.IoUring`) |
| **SQLite driver** | None. `Library.SQLite` reads and writes the SQLite file directly using memory mapping and write-ahead logging. No `libsqlite3`, no shared libraries |
| **Authentication** | `Library.Crypto`: JWT HS256, signature and expiry checked on every authenticated request |
| **Nginx or direct** | **Direct**. Listens on `$HOST:$PORT` (use `0.0.0.0:80`) |
| **Compiler** | The stock AILANG compiler `ailang.x` from [AiLang-Author/Ailang-Self-Hosting-](https://github.com/AiLang-Author/Ailang-Self-Hosting-), installed by `install.sh` (see below) |

## How this was built

This submission was written entirely by AI systems under human direction. The human author, Sean Collins ([2 Paws Machine and Engineering](https://github.com/AiLang-Author)), set the goals, chose the architecture, and ran the benchmarking. The AI wrote the server and the libraries and ran the tests.

AILANG is a language made for this kind of work, and this is its first entry in the challenge. It is submitted so that anyone using AILANG has a working, tested reference implementation of a real server that speaks the challenge's exact API.

The folder [`triumvirate-engineering/`](triumvirate-engineering/) holds the working method used: three AI roles (an implementer, a performance and compliance checker, and a code reducer) that review each other's work.

## Build and run

```bash
sudo bash install.sh # installs the pinned AILANG compiler to /usr/local/bin/ailang.x
bash build.sh        # compiles feed_server.ailang
SQLITE_PATH=/path/to/copy-of-feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

Each user holds one keep-alive connection, so the open-descriptor limit is the connection limit. On the official box `LimitNOFILE=65535` applies to each process:

- `HTTP_NSLOT` (default 65,520) is the connection table size of each process. It sits just under the descriptor limit, leaving a few descriptors for the server's own files, so a connection beyond the table is accepted and closed at once instead of waiting in the kernel backlog. Raise it only together with the descriptor limit.
- `HTTP_DIRECT` (default 1) keeps accepted sockets as io_uring direct descriptors, in io_uring's own file tables instead of the process's, so one process holds more connections than its descriptor limit: up to 8 rings of `HTTP_NSLOT` each, opened as needed.
- `HTTP_RXMULTI` (default 1) receives with multishot receives into a shared ring of 4,096 × 2 KB buffers per io_uring ring, so an idle connection holds no receive buffer.
- With `HTTP_DIRECT=0`, `HTTP_FRONTS` (default 3) front processes accept connections instead, each with its own descriptor limit, and pass requests to the database process through shared memory (the earlier design, kept as a fallback).

Check it with the challenge's own tests (from the repository root):

```bash
bash test/run.sh submissions/ailang-native-2paws
```

### The compiler

AILANG is self-hosting and has no apt package. Its compiler is one static binary, `ailang.x`, published in the AILANG repository at [AiLang-Author/Ailang-Self-Hosting-](https://github.com/AiLang-Author/Ailang-Self-Hosting-) together with its source.

- `install.sh` downloads `ailang.x` from that repository at a pinned commit (`ed228e9e`, branch `master`), checks its sha256 (`7cc4128adaf3c1be31de15007aca1f4d4e187187026a6618c509d2f9c50f2ec4`) and installs it to `/usr/local/bin/ailang.x`.
- `build.sh` compiles the server with it. Set `AILANG_COMPILER` to use a different compiler.
- No compiler or server binary is committed here. The compiler belongs to the AILANG project and is not covered by this repository's MIT license.

## How it works

- Implements all five endpoints. The `/feed` response is byte-for-byte what the tests expect (5,102 bytes).
- The feed reads the 20 newest posts by walking the posts table in reverse primary-key order. No indexes were added.
- Usernames are found through a lookup table of the users table's own B-tree page boundaries, rebuilt at startup. This is the database's structure, not a copy of its data. There is no cache of likes or of users.
- **Batched writes (group commit):** new posts and first-time likes are not committed one by one. All writes handled in one pass of the event loop share a single WAL commit, and their responses are delayed until that commit completes.
- **io_uring, one process:** one thread runs accepts, receives and sends through io_uring. Accepted sockets are direct descriptors (multishot accept into a sparse registered file table), closed by a linked shutdown and close, so they never use the process's descriptor limit. When a ring's table is nearly full, accepting moves to another ring (up to 8). Receives are multishot into a provided buffer ring; if the buffers run out, the affected connections are re-armed a bounded number per pass so the loop does not stall.
- **I/O process, `synchronous=NORMAL`:** the database process never writes a file. It appends each commit's WAL frames to a ring in shared memory, and an I/O process writes them in order and reports back; the responses for a commit go out once its frames are in the WAL file. Every 4 MB of WAL, a checkpoint writes the pages committed since the last one back into the database file, and the server drops its own copies of them. Every 8 MB the database file is synced and the WAL starts over from the beginning, as SQLite does. Checkpoints run in a separate checkpoint process with its own ring: it syncs the WAL once the I/O process has written the checkpoint's frames, then writes the pages back, then syncs the database, so WAL appends never queue behind a checkpoint's disk work. A disk stall therefore delays only the writes waiting on it; reads keep going. If the database process dies, the I/O process finishes the queued writes before it exits, so a crash loses nothing that was acknowledged (tested with `kill -9` under load).
- **Lookups:** pages are searched by binary search, an insert finds its place the same way, and a feed's 20 like counts come from one walk over the `likes(post_id)` index, counting runs that span whole pages without decoding them.
- **Memory:** requests and responses use two shared buffers; a connection only borrows a buffer from a pool while a request or response is incomplete. An idle connection costs its slot (256 bytes) and the kernel's socket memory.

## Test results

**Test machine:** AMD FX-8320 (Bulldozer family, 3.7 GHz, released 2012). This is **not** the official DigitalOcean benchmark machine. The official test suite passes: **42 out of 42**.

k6 runs out of memory above about 60,000 users on this machine, so load was generated with the C load generator from the `c-iouring-raoz` submission (`tests/loadgen.c`), which follows the user loop of `bench/load.js`, on the other seven cores. Pass/fail uses its whole-run figures, ramp-up included, as k6's thresholds do.

The load generator spreads its connections over 16 loopback source addresses (`SRC_IPS=127.0.0.2,…,127.0.0.17`). With 8, the kernel's search for a free local port in its own `connect()` calls took all seven of its cores near 126,000 connections, and the late responses it then recorded were its own delay, not the server's.

### Under the official box's limits

**local, FX-8320, 1 core, MemoryMax=1800M, no swap, LimitNOFILE=65535**

The server ran in a transient systemd scope (`MemoryMax=1800M`, `MemorySwapMax=0`, `CPUQuota=100%`, one core, open files 65,535), on a fresh copy of the seed database each run (on the machine's SATA SSD unless noted), with a 100 s ramp. RAM is the scope's peak memory, CPU is the scope's CPU use during the hold. These limits are part of the test harness only; `build.sh` and `start.sh` set none of them. All rows use the default `start.sh` settings and the compiler `install.sh` installs.

| Users held 5 min | req/s | p95 ms | p99 ms | err % | CPU % | RAM MB | Result |
|---:|---:|---:|---:|---:|---:|---:|---|
| **220,000** | **23,244** | **27** | **583** | **0.000** | **93** | **1,290** | **pass** |
| 200,000 | 21,146 | 37 | 233 | 0.000 | 92 | 1,109 | pass |
| 220,000, database on tmpfs | 23,300 | 4.9 | 9.1 | 0.000 | 92 | 1,103 | pass |
| 240,000, database on tmpfs | 25,387 | 253 | 2,168 | 0.000 | 96 | 1,266 | fail |

The 200,000 row predates the checkpoint process; at 220,000 without it the hold failed (p99 7.4 s) on checkpoint syncs. The p99 at 220,000 on the SSD comes from a few stalls of 1 to 3 s: logging showed the WAL writes blocked in the kernel's dirty-page throttling while the disk was busy with checkpoint syncs. With the database on tmpfs those stalls disappear. At 240,000 the CPU is full: the hold itself ran at about 25,400 req/s with p99 near 15 ms, but the end of the ramp pushed latency past the limit.

The earlier design (three front processes, `HTTP_DIRECT=0`) held 200,000 users on the same harness with 19,769 req/s, p95 355 ms, p99 625 ms and 0.4% errors.

**Above the connection limit:** each io_uring ring accepts at most `HTTP_NSLOT` connections and the server opens up to 8 rings. A connection beyond that is accepted and closed at once, so the client sees it closed, and the server keeps serving the connections it has.

### The Rust entry on the same harness

For comparison, the Rust entry `rust-iouring-kensac` (PR #264, built from that PR on 17 November 2026) was run on the same machine, harness and load generator:

| Users held 5 min | req/s | p95 ms | p99 ms | err % | CPU % | RAM MB | Result |
|---:|---:|---:|---:|---:|---:|---:|---|
| 200,000 | 19,080 | 3,531 | 5,309 | 0.000 | 58 | 1,217 | fail |

It used only 58% of its core during the hold, and its latency alternated between healthy 10-second intervals (p99 under 50 ms) and stalls of 2 to 7 s. That pattern points at the disk, not the CPU: its design holds writes while a checkpoint copies the WAL's last frames, and falls back to checkpointing on the event loop when its low-priority checkpoint thread falls behind. On this machine's slow SATA SSD both take much longer than on the droplet's disk, so this result says more about the test machine than about that server. In a CPU-only profile it used about 30 µs of CPU per request against about 43 µs for this server. If `HTTP_NSLOT` is set above the descriptor limit, new connections wait in the kernel's backlog instead and the server spends its time retrying `accept`, which is why the default stays under it.

### Without memory or descriptor limits (earlier build)

These were measured on an earlier single-process build with `HTTP_NSLOT=131072`, no memory limit, no descriptor limit, and the server pinned to one core with `taskset -c 0`. They are not reachable on the official box in that form: 120,000 connections need more than the 65,535 descriptors one process may hold there, which is what the front processes now provide. That build also had bugs fixed since (see below).

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
- Every JWT verification still leaked 192 bytes: the JSON objects of its header and payload came from an allocator call that bypassed the scratch arena. Each new like or post also leaked its record and cell buffers.
- After a WAL restart, closing the old WAL descriptor made ext4 flush the file inside the event loop, stalling every request for up to a second.
- WAL recovery applied every frame with the right salts. It now stops at the last commit frame of an unbroken checksum chain, as SQLite does.

## Files

| | |
|---|---|
| `feed_server.ailang` | The server |
| `Librarys/Library.IoUring.ailang` | io_uring setup, submission and completion |
| `Librarys/Library.HTTP1Front.ailang` | The front processes for `HTTP_DIRECT=0` |
| `Librarys/` | The AILANG libraries it uses (HTTP, SQLite, JSON, crypto, and so on) |
| `build.sh`, `start.sh`, `install.sh` | The challenge scripts |
| `triumvirate-engineering/` | Description of the AI working method used, for reference |
