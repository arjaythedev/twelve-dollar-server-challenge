# Go + fasthttp + SQLite (amalgamation via cgo), direct on :80

| | |
|---|---|
| **Language** | Go 1.27.1. Official tarball, sha256-checked by `install.sh`. |
| **Framework** | [`valyala/fasthttp`](https://github.com/valyala/fasthttp) v1.75.0. No router, no middleware. |
| **Driver** | None. SQLite 3.54.0 amalgamation, sha256-checked by `build.sh`, compiled to a static library, called through a 60-line C shim (cgo). No `database/sql`. |
| **Nginx or direct** | **Direct.** The server listens on `0.0.0.0:80` itself. |
| **Optimizations** | See [Design](#design). One paragraph per decision, with the reason. |

## Run

```bash
sudo bash install.sh   # build-essential, curl, unzip; Go 1.27.1 into /usr/local/go
bash build.sh          # fetches SQLite 3.54.0, builds bin/server for this CPU
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`test/run.sh` passes 42/42.

## Design

The CPU budget under load is roughly 71 % SQLite, 17 % kernel TCP, 12 % Go (see [Profiles](#profiles)).
Every decision below keeps the Go share small and the SQLite share honest.

### Data path

1. **SQLite renders the JSON.** Each read is one prepared statement. Its only column is the complete
   response body, built with `json_object` and `json_group_array`. `/feed` aggregates a 20-row
   subquery of plain columns. Building the object inside the aggregate avoids a `json()` re-parse,
   which cost 22 % of feed CPU. The bytes are copied once, from SQLite into the response buffer.
2. **One cgo call per request.** `shim.c` binds the parameters, steps, copies column 0 and resets the
   statement in one call. There are no row objects, no Go JSON encoder and no per-column crossings.
3. **One connection, one goroutine, group commit.** SQLite is compiled with `THREADSAFE=0`. One
   goroutine owns the connection. Handlers send a pooled request struct through a channel. Each pass
   drains up to 256 requests, answers reads at once, and runs all queued likes and posts in one
   `BEGIN IMMEDIATE … COMMIT`. A writer gets its response only after that COMMIT returns, so a 201
   means the row is committed (rule 6). A failed commit fails the whole batch with 500.
4. **Small WAL, explicit checkpoints.** `wal_autocheckpoint=0`. A `sqlite3_wal_hook` counts frames.
   When the queue is empty and the WAL is past 1,000 frames, the DB goroutine runs a `TRUNCATE`
   checkpoint. The WAL stays under 5 MB under write stress. `PASSIVE` checkpoints on a side thread
   were rejected: under steady writes they never catch up and the WAL grows without bound.
5. **Pragmas.** `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), `mmap_size=1 GiB` (the 250 MB
   database is read from the OS page cache without copies), `cache_size=-2048`, `temp_store=MEMORY`.
   Nothing is cached across requests (rule 5).

### Runtime

1. **fasthttp for idle keep-alive connections.** `ReduceMemoryUsage` returns buffers to pools while a
   connection waits in think time. Read buffer 4 KiB, write buffer 8 KiB (a full feed fits in one
   write). No `Server`, `Date` or default `Content-Type` headers. Header names are not normalised, so
   `Authorization` is matched case-sensitively; k6, curl and browsers send it that way. No read or
   idle timeouts, because SPEC asks for idle connections to stay open.
2. **No allocation per request.** Error bodies are static. Response buffers come from a pool. The
   JWT is decoded and HMAC-verified (HS256, precomputed pads, constant-time compare) into a pooled
   scratch buffer. The create body is unescaped in place and trimmed with JavaScript's `trim()`
   whitespace set. A heap profile under a 60k-user hold shows no allocation on the request path.
3. **GC off, `GOMEMLIMIT=1GiB`.** `start.sh` exports `GOGC=off` and `GOMEMLIMIT=1GiB`; both can be
   overridden. Nothing is allocated per request, so the collector has no work. The limit is the only
   remaining GC trigger, a safety fuse. It is 1 GiB and not 2 because the limit covers only Go-managed
   memory (heap, goroutine stacks, runtime data). The rest of the 2 GB box is taken: about 200 MB for
   Ubuntu and systemd, the 250 MB database in the page cache through `mmap`, and kernel socket state
   for tens of thousands of connections. Measured Go memory is 5.7 KB per connection, mostly goroutine
   stacks: 340 MB at 60k, 870 MB at 140k. `GODEBUG=gctrace=1` during a 140k hold showed no GC cycle.
   A limit that the live set reaches would make the GC run continuously on the one vCPU.
4. **Compiled for the box.** `build.sh` sets `GOAMD64=v3` and `-march=x86-64-v3` when the CPU has
   AVX2 and BMI2, else `v2`. SQLite is built with the recommended `OMIT_*`, `DEFAULT_MEMSTATUS=0`,
   `USE_ALLOCA` and `MAX_EXPR_DEPTH=0` options.
5. **Profile-guided optimisation.** Two profile files sit next to the source. Both were recorded under
   the k6 workload on the target. `default.pgo` is Go PGO; `go build` picks it up by itself.
   `sqlite#sqlite3.gcda` with `gcc-version` is gcc PGO for the SQLite amalgamation; `build.sh` applies
   it when the box's gcc major version matches and skips it otherwise, so the build also works on CI.
   Both files are profile data, not build output. Expect single-digit percent gains: the hot path is
   SQLite and the kernel.

## Measured

Setup: AWS `c7a.medium` (1 vCPU on a dedicated AMD EPYC "Zen 4" core, 2 GiB, Ubuntu 24.04, kernel
untouched, direct on :80). k6 ran on a separate box in the same AZ. Each row is a 5-minute hold of
`bench/load.js`. Two caveats make these upper bounds, not leaderboard numbers:

- `LimitNOFILE` was raised to 1,048,576 for the rows above 65k. Upstream benchmarks at 65,535.
- A dedicated vCPU is faster than the DigitalOcean shared vCPU.

| VUs | verdict | rps | p95 | p99 | errors | CPU | RSS (anon) |
|---:|---|---:|---:|---:|---:|---:|---:|
| 20,000 | PASS | 1,868 | 0.4 ms | 15 ms | 0 % | 19 % | 118 MB |
| 40,000 | PASS | 3,736 | 1 ms | 13 ms | 0 % | 33 % | 231 MB |
| 60,000 | PASS | 5,601 | 6.1 ms | 18 ms | 0 % | ~45 % | ~340 MB |
| 80,000 | PASS | 7,468 | 7.1 ms | 21 ms | 0 % | 56 % | 459 MB |
| 120,000 | PASS | 11,171 | 117 ms | 187 ms | 0 % | 80 % | 703 MB |
| 135,000 | PASS | 12,477 | 184 ms | 644 ms | 0 % | 85 % | 795 MB |
| 140,000 | FAIL / PASS | 12,800 | 491 / 246 ms | 1,820 / 825 ms | 0 % | 86–89 % | 869 MB |

135,000 users confirmed. The limit is tail latency, not CPU, errors or memory. At this load there
are about 1,000 writes per second, so the inline checkpoint runs about once a second. Each checkpoint
is an fsync on the one DB goroutine, and every request behind it waits. No GC ran during the 140k hold.

For scale: an A/B build of this server with a pure-Go SQLite binding (zombiezen / modernc, removed
since) needed 86 % CPU for the same 80k load, with p99 154 ms. The C shim needed 56 % and 21 ms.

## Profiles

Source: the PGO training hold (60,000 users, 151 s). SQLite was gcc-instrumented for that run, so
its share is somewhat inflated. These are the `default.pgo` and heap profile that ship with the source.

**CPU** (`go tool pprof -top`; 75 s of samples, 50 % of the vCPU):

| flat | share | function | what it is |
|---:|---:|---|---|
| 53.8 s | 71 % | `runtime.cgocall` | SQLite, the queries. Go cannot see inside C, so all of it lands on the call site |
| 13.1 s | 17 % | `syscall.Syscall6` | the kernel TCP stack: one `read` and one `write` per request |
| 0.46 s | 0.6 % | `internal/poll.(*FD).Read` | Go's side of the socket read |
| 0.30 s | 0.4 % | `fasthttp.(*Server).serveConnCounted` | the per-connection loop |
| 0.23 s | 0.3 % | `fasthttp.acquireByteReader` | waiting for a connection's first byte |

All Go code together (fasthttp, scheduler, netpoller, handlers, JWT) is about 12 %. Inside the 71 %
SQLite share: 91 % is the two read statements, 5 % group commit, 2 % likes, 1 % checkpoints.

**Heap** (`-sample_index=inuse_space`; 65 MB live at 60k connections; 77 MB allocated over the whole
run, about 1.3 KB per connection and nothing per request):

| live | share | allocated by | when |
|---:|---:|---|---|
| 29.3 MB | 45 % | `runtime.mallocgc` (inlined small objects) | connection setup |
| 8.5 MB | 13 % | `syscall.anyToSockaddr` | `accept`: peer address |
| 7.0 MB | 11 % | `net.newFD` | `accept`: file descriptor struct |
| 7.0 MB | 11 % | `fasthttp.(*workerPool).Start.func1` | one worker goroutine per connection |
| 6.1 MB | 9 % | `main.init.func2` | the pool's 16 KB response buffers, about 400, recycled |

Nothing in this list runs per request. Goroutine stacks hold most of the per-connection memory; they
are not heap and do not appear in this profile.

## License

MIT, under the repo's [license](../../LICENSE).
