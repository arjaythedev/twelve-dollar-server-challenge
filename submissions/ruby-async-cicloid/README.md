# Ruby + Async

| | |
|---|---|
| Language | Ruby 4.0.7 with YJIT, built from source with jemalloc and `-march=native` |
| Framework | [async](https://github.com/socketry/async) 2.46.0 reactor on io-event 1.22.1 (io_uring selector, epoll fallback); the HTTP/1.1 layer is our own (`http.rb`, `epoll.rb`) |
| SQLite driver | extralite 3.1.3, statically linked to SQLite 3.54.0, built from the amalgamation with profile-guided optimization |
| JWT | HMAC-SHA256 from libsodium 1.0.22, called through rbnacl 7.1.2's FFI bindings |
| JSON | built by SQLite (`json_object`); json 3.1.0 parses request bodies and JWT claims |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

Ruby, SQLite and libsodium are downloaded at pinned versions and checked against their SHA-256 sums.
Every gem, transitive ones included, is pinned with checksums in `Gemfile.lock`.

## Running it

```bash
sudo bash install.sh   # apt build deps, then Ruby, SQLite and libsodium from source (~10 min on 1 vCPU)
bash build.sh          # gems into vendor/bundle
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

The first version ran on Falcon. Under load, its fiber per connection and the objects async-http
keeps for each one cost about 40 KB per connection, and garbage collection had to scan all of that.
At 20k connections each GC paused for about 150 ms, and that pause set the latency floor. This
version keeps the Async reactor and replaces the HTTP server.

- **Idle connections own no fiber.** An idle keep-alive connection sits in a kernel epoll set and
  owns no Ruby fiber. Each connection is registered once, edge-triggered, so there is no `epoll_ctl`
  per request. One Async task waits on that epoll descriptor through the reactor. A pool of 256
  worker fibers (it grows, up to 4096, only when every worker is busy) reads each ready connection,
  answers every complete request in its buffer in order, and writes each response with one `write`.
  Memory is about 3–5 KB per connection (about 130 MB at 40k), and GC drops from about 4 s to about
  0.15 s per minute.
- **A minimal HTTP/1.1 parser.** It reads the request line and the four headers the API uses:
  `Content-Length`, `Transfer-Encoding`, `Connection` and `Authorization`. It supports keep-alive,
  pipelining, partial reads, HTTP/1.0 and `Connection: close`. Chunked request bodies get a 411.
  Response heads are prebuilt frozen strings.
- **io_uring for socket reads and writes.** io-event picks io_uring on the droplet's kernel. Reads
  and writes go through the fiber scheduler, so io_uring submits them in batches instead of one
  syscall each. Together with edge-triggered epoll, that cut CPU at 40k users from about 46% to 40%
  on our test VM.
- **SQLite builds every response.** `json_object` and `group_concat` produce the exact response bytes,
  so Ruby never allocates a hash per row or serializes JSON on reads. All statements are prepared once.
  The single-post query binds its id as `p.id=+?`: with a plain `?` in that join, SQLite treated the
  plan as depending on the bound value and recompiled the statement on every request. The `+` keeps
  the same plan and output and halves that query's time (14 to 7 µs).
- **Group commit.** The first write in a reactor pass opens `BEGIN IMMEDIATE`, every write in that pass
  joins the transaction, and one `COMMIT` runs on the next pass. No write response goes out before its
  `COMMIT` returns, and reads that ran while a batch was open wait for it too. That gives one WAL
  append per batch instead of one per write.
- **Pragmas**: `locking_mode=EXCLUSIVE`, so the WAL index lives in heap memory rather than the `-shm`
  file; `journal_mode=WAL`; `synchronous=NORMAL` (rule 6); `wal_autocheckpoint=0`, with a passive
  checkpoint every second, off the commit path. Every statement is reset as soon as its row has been read, because a statement left mid-step holds a read transaction and blocks checkpoints. Before that fix the WAL grew past 300 MB under load, and WAL lookups (`walFindFrame`) took 30% of the CPU; 1 GiB `mmap_size`; 64 MiB page cache.
- **SQLite compiled for this workload.** It's a static library built with sqlite.org's recommended
  options (`DQS=0`, `DEFAULT_MEMSTATUS=0`, `OMIT_DEPRECATED`, `OMIT_SHARED_CACHE`, `USE_ALLOCA`, and so
  on). It's then profile-guided: `sqlite_train.c` runs the server's own statements in the load test's
  mix against a synthetic database, and the final build uses that profile. That made reads about 7%
  faster.
- **One thread, and SQLite never releases the GVL.** Every query is an index lookup that takes
  microseconds, so releasing and reacquiring the GVL would cost more than it saves.
- **JWT verified on every request**, with no caching (rule 5). libsodium's one-shot HMAC-SHA256 takes
  1.6 µs, compared with 3.5 µs through OpenSSL's HMAC in Ruby, and signatures are compared in
  constant time. The standard `{"alg":"HS256","typ":"JWT"}` header is compared as a string, and any
  other header is parsed and must say `HS256`. `exp`, `nbf`, `sub` and `username` are checked.
- **Likes in one statement**: `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`. A
  post-existence check runs only when nothing was inserted, to tell "already liked" from "no such post".
- **Direct instead of Nginx.** On one core, Nginx's CPU comes out of Ruby's budget, and the provided
  `nginx.conf` caps out near 16k connections. Responses switch to `Connection: close` only within 256
  descriptors of the process's file limit, so clients aren't forced to reconnect under normal load.

## Results on a 1-vCPU test VM

These ran on a Proxmox VM: 1 dedicated Zen 3 vCPU, 2 GB RAM, Ubuntu 24.04, io_uring. Load came from a
separate 8-vCPU VM running the C connection generator from PR #14 (8 processes, 60 s ramp, 120 s
hold, the `bench/load.js` request mix). That vCPU is roughly twice as fast as the droplet's, so the
numbers are relative. Other entries ran on the same VM, built with their own scripts. CPU is the whole
VM, and p95/p99 are the worst across the generator processes.

| Server (droplet score in the video) | 14k users | 25k users | 40k users |
|---|---|---|---|
| **This server** | not rerun (22%, 125 MB, 1 / 4 ms before the io_uring changes) | 27%, 117 MB, 2 / 10 ms | 40%, 130 MB, 6 / 28 ms |
| Python + Granian, PR #41 (not in the video) | 31%, 267 MB, 2 / 13 ms | 47%, 428 MB, 5 / 26 ms | 70%, 661 MB, 169 / 1,660 ms |
| Go net/http, PR #18 (22k) | 23%, 461 MB, 1 / 7 ms | 59%, 536 MB, 7 / 23 ms | 93%, 578 MB, 194 / 1,718 ms |
| Python lean FastAPI, PR #10 (22k) | 34%, 188 MB, 2 / 6 ms | 58%, 289 MB, 10 / 149 ms | 88%, 421 MB, 370 / 2,887 ms |
| Python FastAPI, PR #1 (11k) | 48%, 232 MB, 3 / 14 ms | 80%, 367 MB, 82 / 1,436 ms | 99%, 12k failed requests |

At 40k users, the generator itself ran up to 1 s late, which inflates every p99 in that column
(about 0.3 s for this server's run). At 60k users this server used 56% CPU and 166 MB with no
errors; the generator lagged too much there for its latencies to mean anything.

### k6 with the unmodified `bench/load.js`

Same VMs, 60 s ramp and a 2 min hold (the load VM was raised to 16 GB for the runs above 30k VUs).
Uncapped, at 25k VUs: p95 1.3 ms, p99 10.7 ms, 0 failed of 458,135 requests.

To approximate the droplet's slower core, we then capped the target VM at half a core (Proxmox
`cpulimit=0.5`) and stepped the VU count until a threshold broke:

| VUs | p95 | p99 | Failed | req/s | RSS | k6 thresholds |
|---|---|---|---|---|---|---|
| 35,000 | 16 ms | 48 ms | 0 of 640,662 | 2,911 | 137 MB | pass |
| 40,000 | 216 ms | 526 ms | 0 of 729,584 | 3,315 | 159 MB | pass |
| 45,000 | 1.15 s | 1.96 s | 0 of 776,390 | 3,528 | 201 MB | fail (p95, p99) |

A half-core cap is only a stand-in for a droplet vCPU, so treat 40k as an estimate, not a score.

Test suite: 42/42 on x86_64.

## Trade-offs

- Linux only, because epoll is bound through FFI.
- No idle timeout. Connections stay open until the client closes them, and the descriptor guard
  covers the extreme case. A worker that picks up a stale edge-triggered event waits on that
  connection until the client sends or closes; the growable pool keeps that from starving others.
- Reads call io-event's scheduler `io_read` directly with an `IO::Buffer`, which Ruby still marks
  experimental (it prints a warning at startup). On an epoll-only kernel it still works, unbatched.
- The parser covers what this API needs: no chunked request bodies and no `Expect: 100-continue`.

## License

MIT, under the repo's [license](../../LICENSE).
