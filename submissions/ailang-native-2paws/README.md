# AILANG Native SQLite Feed

A submission written in AILANG, a programming language designed for AI-assisted systems programming.

| | |
|---|---|
| **Language** | AILANG, compiled to a standalone x86-64 Linux binary |
| **Framework** | None. HTTP/1.1 with persistent connections, event-driven (`Library.HTTP1`) |
| **SQLite driver** | None. `Library.SQLite` reads and writes the SQLite file directly using memory mapping and write-ahead logging. No `libsqlite3`, no shared libraries |
| **Authentication** | `Library.Crypto`: JWT HS256, signature and expiry checked on every authenticated request |
| **Nginx or direct** | **Direct**. Listens on `$HOST:$PORT` (use `0.0.0.0:80`) |
| **Compiler** | AILANG optimizing compiler `ailang-opt11.x`, bundled in `compiler/` (see below) |

## How this was built

This submission was written entirely by AI systems under human direction. The human author, Sean Collins ([2 Paws Machine and Engineering](https://github.com/AiLang-Author)), set the goals, chose the design direction, ran the benchmarks and decided what to keep. The AI wrote the server code and the libraries, and ran the tests.

AILANG is a language made for this kind of work, and this is its first entry in the challenge. It is submitted so that anyone using AILANG has a working, tested reference implementation of a real HTTP + database service. The author is happy with either outcome, accepted or not.

The folder [`triumvirate-engineering/`](triumvirate-engineering/) holds the working method used: three AI roles (an implementer, a performance and compliance checker, and a code reducer) that review each other's work. It is included for transparency and for anyone who wants to reuse it. It is not part of the server.

## Build and run

```bash
bash install.sh      # does nothing; there is nothing to install
bash build.sh        # compiles feed_server.ailang with the bundled compiler
SQLITE_PATH=/path/to/copy-of-feed.db JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`HTTP_NSLOT` sets the maximum number of open connections (default 32,768, enough for the ~15,000 the challenge expects). For 120,000 concurrent users, set it to 131,072 or higher.

Check it with the challenge's own tests (from the repository root):

```bash
bash test/run.sh submissions/ailang-native-2paws
```

### About the bundled compiler

Rule 10 asks for builds from source with no prebuilt binaries. AILANG's compiler is written in AILANG and compiles itself, so it can't be built from a package manager or a plain source tarball. This folder therefore includes one compiler binary, `compiler/ailang-opt11.x`.

- It is an optimizing build that the server needs: `Library.Hash` uses `Crc32String`, which the compiler currently published at [AiLang-Author/Ailang-Self-Hosting-](https://github.com/AiLang-Author/Ailang-Self-Hosting-) (`ailang.x`) does not have yet.
- **An update is coming:** the optimizer is not yet pushed to that repository. When it is, `build.sh` can use the published compiler with `AILANG_COMPILER`, and the bundled copy can be removed.
- sha256 of the bundled compiler: `eb705f6e60ece1bca1c1e8d3103b1a65be162b6fc63c690d85e1675f2efca407`
- The compiler binary belongs to the AILANG project and is not covered by this repository's MIT license.
- The server binary `feed_server.x` is built by `build.sh` and is not committed.

If this exception to rule 10 is not acceptable, the submission can simply be declined.

## How it works

- Implements all five endpoints. The `/feed` response is byte-for-byte what the tests expect (5,102 bytes).
- The feed reads the 20 newest posts by walking the posts table in reverse primary-key order. No indexes were added.
- Usernames are found through a lookup table of the users table's own B-tree page boundaries, rebuilt at startup. This is the database's structure, not a copy of its data. There is no cache of like counts, usernames, query results or responses (rule 5), and every JWT is verified on every request.
- **Batched writes (group commit):** new posts and first-time likes are not committed one by one. All writes handled in one pass of the event loop share a single WAL commit, and their responses are sent only after the commit finishes, so a 201 always means the row is durable (rule 6). A repeated like (200) changes nothing and is answered immediately.

## Test results

**Test machine:** AMD FX-8320 (Bulldozer family, 3.7 GHz, released 2012), server pinned to one core with `taskset -c 0`, load generated on the other cores. This is **not** the official DigitalOcean droplet and **not** the official k6 test. The official score will come from the maintainer's benchmark, and may be higher or lower than anything below.

The official test suite passes: **42 out of 42**.

### Sustained 5-minute tests

k6 runs out of memory above about 60,000 users on this machine, so high-concurrency tests used the C load generator from the `c-iouring-raoz` submission (`tests/loadgen.c`), which follows the same request pattern as `bench/load.js` including the pause between requests. Extra loopback addresses (`127.0.0.2` to `.8`) and a wider local port range were set on the test machine only, to let it open that many connections.

| Concurrent users | 95th percentile | 99th percentile | Errors | Result |
|---:|---:|---:|---:|---|
| 80,000 | 7.8 ms | 95 ms | 0 | pass |
| 100,000 | 1.6 ms | 74 ms | 0 | pass |
| 110,000 | 2.1 ms | 65 ms | 0 | pass |
| **120,000** | **10.4 ms** | **91 ms** | **0** | **pass** |
| 150,000 | failed during ramp-up (99th percentile 1.2 s) | | 0 | fail |
| 200,000 | 4.5 s | 17 s | 0.63% | fail |

On this machine the limit is between 120,000 (pass) and 150,000 (fail). Before batched writes it was between 100,000 and 110,000.

### Peak throughput, GET /feed

32 connections for 15 seconds, no pause between requests, one pinned core, same machine:

| Server | Requests/sec | Median latency |
|---|---:|---:|
| AILANG (this submission) | 19,468 to 19,552 | 0.196 ms |
| Python FastAPI (`python-fastapi-cknutson12`) | 2,932 | 0.513 ms |

Memory use is about 6 MB when idle and about 1.4 GB at 150,000 connections, almost all of it per-connection buffers.

## Files

| | |
|---|---|
| `feed_server.ailang` | The server |
| `Librarys/` | The AILANG libraries it uses (HTTP, SQLite, JSON, crypto, and so on) |
| `compiler/` | The bundled AILANG compiler (see above) |
| `build.sh`, `start.sh`, `install.sh` | The challenge scripts |
| `triumvirate-engineering/` | Description of the AI working method used, for reference |
