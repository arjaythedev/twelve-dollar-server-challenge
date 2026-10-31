# C + io_uring + SQLite

A single-threaded HTTP/1.1 server in C11 with SQLite compiled in. **Direct on `0.0.0.0:80`, no Nginx.** It uses io_uring with direct descriptors when the kernel allows it (Linux 6.1 or later) and falls back to epoll when it does not.

| | |
|---|---|
| Language | C11 (Ubuntu 24.04 `gcc` 13, `-O2`; SQLite `-O3`) |
| Framework | none: hand-written HTTP/1.1 server, direct on port 80, no Nginx |
| I/O | io_uring: multishot accept and receive, provided buffers, direct descriptors. Fallback: epoll |
| HTTP | hand-written parser and writer |
| Database driver | the SQLite C API, 3.54.1 amalgamation compiled in, pinned and SHA-256 checked (`deps.env`) |
| Threads | one. A 1 vCPU box gains nothing from more, and one thread needs no locks |
| Dependencies | libc, SQLite and liburing 2.15 (static, pinned, built by `build.sh`). SHA-256, HMAC, base64url, JSON and HTTP are in `src/` |

## Build and test

On Ubuntu 24.04, as the challenge asks: `install.sh` (root), `build.sh`, `start.sh`. `bash test/run.sh submissions/c-iouring-meros` from the repository root runs the 42 checks. At start-up the server prints which backend it chose (`server: io_uring ...` or `server: epoll`) on stderr. `SERVER_IO=epoll` or `SERVER_IO=uring` forces one; the benchmark never sets it.

With Nix, everything is one command (`flake.lock` pins the toolchain and the challenge revision):

| Command | What it does |
|---|---|
| `nix build` | builds `result/bin/server` |
| `nix flake check` | warnings as errors + unit tests; the 42 challenge checks and the protocol tests on both backends; the same under AddressSanitizer and UBSan |
| `nix run .#e2e` | the challenge checks and the protocol tests, both backends, against a local build |
| `nix build .#vm-scale-100k -L` | two virtual machines shaped like the benchmark box, 100,000 users (see *Simulated target environment*); also `-60k`, `-150k`, `-250k`, `vm-k6-fallback` |
| `nix run .#bench` | requests per second of `/feed`, `/posts/:id` and `/health` with the server on one CPU |
| `nix run .#loadtest -- 5000` | the challenge's k6 test at 5,000 users |
| `nix develop` | shell with gcc, gdb, valgrind, perf, wrk, k6, sqlite |

`make test` runs the unit tests without Nix. `make SANITIZE=1 BUILD_DIR=build-san BIN_DIR=bin-san` builds with AddressSanitizer and UBSan. `make fuzz CC=clang` runs four libFuzzer targets (HTTP parser, JSON reader, JWT, UTF-8 trimming).

## How the code is organised

Each file does one job. Dependencies point downwards.

```
main.c ─ config.c                 environment variables
   └──── server.c                 start-up, listening sockets, picks a backend
            ├── server_uring.c    io_uring loop: direct descriptors, rings, accept, receive, send, close
            ├── server_epoll.c    epoll loop (fallback)      ── conn_epoll.c
            └── server_loop.c     what both loops share: batch settling, slow-round log, idle sweep
                     │            (pacer.c decides when a loop lets work pile up, memwatch.c reads MemAvailable)
            session.c             bytes → requests; pipelining, partial requests, back-pressure
              ├── http_parse.c    request parser (zero-copy views)
              └── router.c        endpoint handlers
                    ├── jwt.c ── hmac_sha256.c ── sha256.c, base64url.c, json_read.c
                    ├── http_response.c, post_json.c, writer.c   response building
                    └── batch.c   requests that wait for the database
            conn.c                connection table (the same for both backends)
            store.c               every SQLite call
```

The two backends only differ in how bytes reach `session_on_data` and how a response leaves (`conn_backend`: `send` and `release`). `strview.h` (pointer + length), `writer.h` (bounds-checked output cursor) and `utf8.c` are the shared vocabulary. Tests live in `tests/unit` (the pure modules: parser, JSON, JWT and its crypto, UTF-8, writer, pacer, memwatch), `tests/e2e` (real sockets, both backends) and `tests/fuzz`.

## What makes it fast

**Direct descriptors.** The service gets `LimitNOFILE=65535`, one file descriptor per connection. A server on plain descriptors serves more users than that by closing connections after each response; in the simulation below, at about 9,000 new connections a second that churn made the tail latency collapse at 150,000 users. A direct descriptor lives in a table registered with an io_uring ring and takes no slot in the process's descriptor table. One ring holds up to 65,535 connections; the server opens up to five rings (327,675 connections), all driven by the one thread. The simulation shows 14 open descriptors for 150,000 users. A ring is never filled completely: with an accept armed on a full table the kernel takes the socket and then drops it, which resets the client. So a ring keeps room free (an eighth of its slots, at most 8,192): it stops accepting when half of that is left, starts again when the room is back, and the next ring opens when the last has twice the room left. The connection table's capacity is what the open rings can hold, so the `Connection: close` mode for users beyond that capacity always begins before accepting stops.

**Receiving and sending.** Each connection has one multishot receive that fills 2 KB buffers from a provided-buffer ring; the buffer is returned as soon as the request is parsed. A response is sent as one `send` per connection (one in flight at a time, so the bytes stay in order). The loop waits for a batch of 32 completions or one pacer window (1 ms) when busy, and for the first completion when idle. Accepting is multishot too, on four sockets that share the port (`SO_REUSEPORT`), which gives four accept queues instead of one; the deepest queue in the 150,000-user run held 7 connections.

**Memory.** No allocation on the receive path: requests are parsed straight out of the provided buffer, and only a *partial* request is copied into a per-connection buffer, freed when the request completes. Read responses are built in one shared scratch buffer, and each write gets one of 128 reply slots in the batch, with the headers written backwards in front of the body, so every response leaves in a single send. The connection table is an array indexed by connection id (about 50 bytes per entry, untouched zero pages cost nothing). 20,000 idle connections added 0.8 MB of anonymous memory.

**Group commit.** Writes do not run when they arrive. Each round collects them (up to 128), runs them inside one `BEGIN … COMMIT`, and only then sends any of the responses. If `COMMIT` fails, or SQLite rolls back by itself, every response in the group is a 500. A pipelined request behind a queued write waits for its response, so ordering never breaks. `synchronous=NORMAL` in WAL mode commits with one `write()` and no `fsync`; the WAL is flushed into the database by checkpoints, which run between rounds after the responses went out (`wal_autocheckpoint=0`), so a checkpoint never delays a request. A checkpoint runs once the log holds 4,096 pages (`STORE_CHECKPOINT_FRAMES`): hot pages such as the end of `posts` and `likes` are rewritten many times in the log and copied into the database once.

**Batching rhythm (`pacer.c`).** At ordinary load every wake-up handles one event, and each request pays for its own sleep. When events arrive faster than 3,000 per second the loop waits up to 1 ms for more, so the work arrives in rounds of ten or more events. A longer window (4 or 10 ms) changed nothing in CPU and made latency worse.

**Feed.** Three range scans instead of 60 B-tree descents: the newest ids from the `created_at` index, then the posts and the likes of that id range, merged in one pass (the likes come as plain index entries, one per like, and are counted in C: that costs fewer SQLite steps than `GROUP BY` and `count(*)`). If the ids have gaps, or a post's author row is missing, it runs the reference query instead, so the answer is always the one the reference query gives (`tests/e2e/protocol_test.py` checks this against the challenge's own SQL).

**Feed sharing: built, off by default.** `make COALESCE_FEEDS=1` makes all `GET /feed` requests of one round share one query and one rendering. It is correct (the query runs after the requests arrived, so no client sees stale data, and nothing outlives the round) and it would save roughly a tenth of the capacity. It is off because rule 5 says "no response caches, query-result caches", and one query result serving many requests can be read as exactly that, even though it is the read-side twin of the group commit that rule 6 allows. The maintainer can decide; the switch is one line.

**When descriptors or memory run short.** The epoll fallback has 65,535 descriptors, so when the connection table is within a reserve of that limit (capacity/16, at most 4,096) responses carry `Connection: close` and users beyond the limit reconnect for each request. Both backends use the same mechanism when `memwatch.c` sees `MemAvailable` fall (the box has no swap, and a process the kernel kills loses the benchmark). Below a quarter of RAM (about 500 MB on 2 GB) a growing share of responses carries `Connection: close`; below an eighth (about 250 MB) every response does, new connections wait in the kernel's queue, and idle connections are evicted at a bounded rate. The window is wide because memory runs out fast: at 200,000 simulated users `MemAvailable` fell from 233 MB to 156 MB in four seconds, and a version that started shedding at an eighth of RAM was killed by the kernel; with this window the box bottoms out at 165 MB. With 175,000 users or fewer the lowest value was 562 MB, so none of it triggers. A second meter on kernel TCP memory was tried and removed: it shed during the ramp-up, and the churn pushed the kernel over its own TCP memory limit, which made things worse. After a `Connection: close` response the server does not close the socket itself: the client closes first (Go's HTTP client does this as soon as it has read the response), so the client keeps the TIME_WAIT socket, and a server that closed 9,000 connections a second overflowed the kernel's TIME_WAIT table in the simulation. If the client has not closed within 250 ms, the server closes.

**SQLite.** Compiled with `THREADSAFE=0`, no extensions, no deprecated APIs (`Makefile`). `locking_mode=EXCLUSIVE` removes every file-lock system call and keeps the WAL index in heap memory. The database file is memory-mapped (1 GB limit) and read once at start-up so the first requests take no page faults. Statements are prepared once and reused (rule 5 allows this: they cache the plan, not data).

## Rules

| Rule | How it is met |
|---|---|
| 2, 3 | SQLite in-process, schema untouched |
| 5 | Every response is built from a query that ran for that request. JWTs are verified on every request (HMAC key pads are computed once at start-up; the signature check is not skipped or remembered). The challenge's tokens have one header and one payload layout, so `jwt.c` first matches those byte for byte and reads the claims without a JSON pass; any token of another shape (whitespace, escapes, other claims) goes through the general reader and gets the same answer. A token without `exp` is rejected. No table copies. Feed sharing is off; see above |
| 6 | WAL + `synchronous=NORMAL`; a 201 is sent after `COMMIT` returned. `tests/e2e` kills the server with `SIGKILL` right after 60 concurrent creates and finds all 60 rows |
| 7 | Nothing detects the load test |
| 9 | Direct on port 80; idle connections are closed after 10 minutes (Nginx needs 65 s) |
| 11 | In-process settings only: `SO_REUSEPORT`, `TCP_DEFER_ACCEPT`, `TCP_NODELAY`, `RLIMIT_NOFILE` raised to its hard limit. No kernel parameter is touched |

The server needs Linux 6.1 or later with io_uring enabled for the fast path (Ubuntu 24.04 ships 6.8), and about 2 MB of `RLIMIT_MEMLOCK` for its five rings (the systemd default is 8 MB and ring memory counts against it, so a fifth server started by the same user at the same time finds the limit used up and falls back to epoll). Where the kernel, a container or a limit refuses it, the server falls back to epoll and still passes every check; in the simulation that fallback holds 100,000 users but not 150,000.

One limit of the accept design: a burst of new connections larger than the room a ring keeps free (an eighth of its slots, at most 8,192) within one loop round makes the kernel drop the surplus sockets, which resets those clients. The benchmark's arrival rate is about 2,500 per second, so this needs a stall of several seconds; a deliberate burst of 4,500 connections at once on 4,000-slot rings reset 4 (one per listener).

## Not done, and why

- **Threads.** One vCPU.
- **Profile-guided optimization and `-march=native`.** PGO of SQLite, trained on the benchmark queries, changed the post lookup from 458 to 452 ns and the feed not at all. `-march=native` needs a known CPU.
- **Compiler hardening.** The stack protector, FORTIFY_SOURCE, CET and PIE are off (`NO_HARDENING` in the `Makefile`), . Switching them off made SQLite about 5% faster on the query micro-benchmark and 2% end to end, which is inside the noise. This is the one place where safety is traded for speed: the process faces the network and runs without stack-smashing protection. The `Makefile` keeps the choice in one variable; building with `NO_HARDENING=` restores the defaults. SQLite itself is built with `-O3`, 4-5% faster than `-O2` on the post and feed queries.
- **Lower kernel memory per connection.** About 5 KB per connection in the kernel (980 MB of slab at 200,000 users) is not reachable from the process.

## Simulated target environment

`nix build .#vm-scale-100k -L` boots two virtual machines under KVM on a virtual network (`nix/scale-test.nix`): the server with 1 vCPU, 2 GB RAM, no swap, as an unprivileged systemd service with `LimitNOFILE=65535` and `CAP_NET_BIND_SERVICE` on port 80, and a client (4-6 vCPUs, 6 GB) running a load generator that follows `bench/load.js` (`tests/loadgen/loadgen.c`, which comes from pull request #26 of the challenge repository and keeps its licence). The kernel clock is `kvm-clock` as on a droplet; the test framework's default (`acpi_pm`) made the network stack twice as expensive and is overridden. The load generator is not k6, except in `vm-k6-fallback`. The host CPU was shared with other tenants (steal time 1-9% in most runs, up to 29% in one), and its cores are faster than a droplet's, so read these as relative, not as droplet predictions. The same virtual machines ran the two strongest other submissions (#26 C and #32 C++, both io_uring), hand-built from their pull requests, in alternating order.

Server vCPU busy in the steady phase (lower is better):

| Users | This server, io_uring | This server, epoll | #32 C++ io_uring | #26 C io_uring |
|---|---|---|---|---|
| 100,000 | **51.7%**, p95 15 ms, p99 211 ms | 60.4%, p95 18 ms, p99 43 ms | 51.3%, p95 213 ms, p99 227 ms | 49.3%, p95 21 ms, p99 218 ms |
| 150,000 | **66.4%**, p95 31 ms, p99 231 ms | 80.3%, p95 0.9 s, p99 10 s, fails | 64.7%, p95 227 ms, p99 441 ms | 61.5%, p95 213 ms, p99 243 ms |
| 200,000 | 99.5%, p95 42 s, p99 55 s, fails; not killed | not run | no result | 67.8%, 1.5% failed, p95 4.8 s, fails |

All rows are 0% failed requests unless stated. The three io_uring servers are in one class: within 0.4-5 points of vCPU at 100k and 150k users, 15% below the epoll fallback at 100k. At 200,000 users the box is out of memory for everyone (free memory fell to 281-306 MB with about 1 GB of kernel socket memory); that is the ceiling for a 2 GB machine. This server passed 150,000 users and failed 175,000 twice (54-55% busy, no failed requests, but p95 7.5-7.8 s): the kernel's TCP memory passed its own limit there (69,000-93,000 pages against a hard limit of 44,724), and everything slows down. The claim is 150,000 users. This server is 3-5 points above the two others in user-space CPU (SQLite's feed query is most of it) and level in the kernel.

More runs, all with this server:

- 60,000 users: 40-45% busy, p95 5-15 ms, p99 20-31 ms (epoll).
- 100,000 users on epoll failed twice, in the two runs where the host took 16% and 25% of the vCPU (the server is 59% busy when it passes, so a fifth of the CPU taken away puts it at the cliff). A Basic droplet is a shared-CPU machine, so this can happen there too.
- 150,000 users at half the request rate passed on epoll (p99 51 ms); at full rate it did not. The io_uring server passes at full rate.
- About 80% of the busy time is the kernel's network stack. The largest single item in a profile at 150,000 users is the kernel's socket hash lookup (`__inet_lookup_established`, 6.7% of the vCPU), whose table is sized at boot from RAM.
- Real k6 (not the C load generator) against the epoll fallback with only 1,500 descriptors and 4,000 users (`nix build .#vm-k6-fallback`): 0 failed requests of 41,473, p95 25 ms, p99 80 ms; about 2,600 users were served through `Connection: close`.

Not tested: real k6 above a few thousand users, a real droplet, and the io_uring server under real k6.

## Loopback numbers

On loopback, with the server on one CPU, CPU time per request at 60,000 simulated users was 16-17.7 µs for the epoll server without feed sharing, against 14.4 µs (#26) and 15.6 µs (#32). Loopback leaves out the cost of a real network path, which in the virtual machines was several times larger, and it showed no advantage for io_uring because it measures system call cost, not the descriptor limit. `tests/bench/query_bench.c` times the read queries alone: the post lookup takes 0.5-1.2 µs and the feed 3.4-9 µs, depending on how full the write-ahead log is.

**JSON escaping.** `writer_json_escaped` checks 16 bytes at a time (SSE2) for a byte below 0x20, `"` or `\`, and copies the clean run in one piece. Post bodies are long and almost always plain text. `tests/unit/test_writer.c` compares it with a byte-at-a-time escaper for every length up to 70 and every position of eleven special bytes.
