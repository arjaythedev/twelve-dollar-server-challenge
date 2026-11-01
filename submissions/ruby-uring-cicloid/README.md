# Ruby + io_uring

Grown from [`ruby-async-cicloid`](../ruby-async-cicloid) (scored 71,000, at the 65,535-descriptor
limit), which stays as it was. This entry holds client connections as io_uring direct descriptors,
outside the descriptor table, with that entry's epoll layer as the fallback, and serves reads where
they arrive. With mload it passes 90,000 users on our test VM.

| | |
|---|---|
| Language | Ruby 4.0.7 with YJIT, built from source with jemalloc and `-march=native` |
| Framework | [async](https://github.com/socketry/async) 2.46.0 reactor on io-event 1.22.1 (io_uring selector, epoll fallback); the HTTP/1.1 layer is our own (`http.rb`), over client connections held as io_uring direct descriptors (`uring_server.rb`, `uring.rb`) with an epoll fallback (`epoll_server.rb`, `epoll.rb`) |
| SQLite driver | extralite 3.1.3, statically linked to SQLite 3.54.0, built from the amalgamation with profile-guided optimization |
| JWT | HMAC-SHA256 from libsodium 1.0.22, called through rbnacl 7.1.2's FFI bindings |
| JSON | built by SQLite (`json_object`); json 3.1.0 parses request bodies and JWT claims through one prebuilt `JSON::Coder` |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

Ruby, SQLite and libsodium are downloaded at pinned versions and checked against their SHA-256 sums.
Every gem, transitive ones included, is pinned with checksums in `Gemfile.lock`.

## Running it

```bash
sudo bash install.sh   # apt build deps, then Ruby, SQLite and libsodium from source (~10 min on 1 vCPU)
bash build.sh          # gems into vendor/bundle
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

### Connection layer: io_uring, with an automatic epoll fallback

The server holds client connections as io_uring *direct descriptors* when the kernel allows it, and
falls back to epoll by itself when it doesn't. Nothing needs configuring; the startup line says which
layer is running:

```
listening on 0.0.0.0:80 (UringServer, 143 iseqs compiled)
```

or, after the fallback:

```
io_uring connections unavailable (Operation not permitted - io_uring_queue_init); using epoll
listening on 0.0.0.0:80 (EpollServer, 141 iseqs compiled)
```

What the io_uring layer needs, all of which a stock Ubuntu 24.04 has:

- **liburing 2.4 or newer**, for `liburing-ffi.so.2` (liburing's inline helpers as exported
  functions, which `uring.rb` binds through the `ffi` gem; nothing is compiled). `install.sh` installs
  `liburing-dev`, which pulls in `liburing2`. Ubuntu 24.04 ships 2.5.
- **Linux 6.0 or newer**, for multishot receive (accepting into direct descriptors needs 5.19).
  Tested on Ubuntu's 6.8.
- **io_uring not blocked.** `sysctl kernel.io_uring_disabled` must be 0 (the default), and no seccomp
  filter or systemd `SystemCallFilter` may deny `io_uring_setup`. Docker's default seccomp profile
  denies it, so in Docker add `--security-opt seccomp=unconfined` or the server runs on epoll.

If any of these is missing, starting the rings raises (`LoadError` when liburing-ffi isn't found,
`SystemCallError` when io_uring is refused) before the listener is bound, and `server.rb` starts the
epoll layer instead, which is the previous, fully tested version of this server. We checked the
fallback by running the server under a systemd unit with `SystemCallFilter=~io_uring_setup`: it
logged the line above and passed `/health`. The test suite passes 42/42 in both modes.

Settings (environment variables, both optional):

| Variable | Default | Effect |
|---|---|---|
| `CONNECTIONS` | unset | `epoll` forces the fallback layer |
| `URING_RINGS` | `2` | io_uring rings; each holds up to the soft `RLIMIT_NOFILE` (at most 65,535) connections |

With `LimitNOFILE=65535` and two rings, the server can hold 131,070 connections while the process
itself keeps about 15 descriptors open.

## Optimizations, and why

The first version ran on Falcon. Under load, its fiber per connection and the objects async-http
keeps for each one cost about 40 KB per connection, and garbage collection had to scan all of that.
At 20k connections each GC paused for about 150 ms, and that pause set the latency floor. This
version keeps the Async reactor and replaces the HTTP server.

- **Connections live outside the descriptor table.** With the scoring box's 65,535-descriptor
  limit, an epoll server can't hold more keep-alive users than that: past it, it has to close idle
  connections so clients reconnect, and each reconnect cost us about twice as much CPU as a request.
  `uring_server.rb` accepts every connection straight into a slot of an io_uring ring's registered
  file table (accept with `IORING_FILE_INDEX_ALLOC`). Those slots aren't process
  descriptors, so they don't count against `RLIMIT_NOFILE`. The kernel caps each ring's table at
  that limit, but every ring has its own table, so two rings hold 131,070 connections. Each
  connection has one multishot receive armed for its whole life, filled from a group of provided
  buffers, so there is no `epoll_wait`, no per-read syscall, and no Ruby `Socket` object per
  connection. One Async task per ring waits on the ring's eventfd through the reactor. Responses are
  queued as sends, in order per connection, and handed to the kernel together. Ubuntu's 6.8 kernel
  rejects registering a buffer ring (`IORING_REGISTER_PBUF_RING`) with `EINVAL`, even from C, so the
  buffers use the older `IORING_OP_PROVIDE_BUFFERS`.
- **Accepts are spread over the rings.** With one multishot accept per ring, the kernel gave every
  connection to the first ring until its table was full, and the switch to the second ring came at
  the end of the load test's ramp, with the core already near saturation: at 80,000 users the event
  loop stalled for 80–250 ms per pass for about 25 seconds, and p99 reached 2.5 s. Now 64
  single-shot accepts stay in flight, each on the ring holding the fewest connections, so both
  tables fill evenly. That run now passes with p99 165 ms.
- **Reads are served where they arrive.** A GET can only suspend while a write batch is open (it
  waits for the COMMIT, see Group commit). So when a connection's new bytes hold only GETs and no
  batch is open, the ring's completion loop serves them itself; everything else goes to a worker
  fiber. About 75% of requests skip the queue push and the two fiber switches.
- **Idle connections own no fiber.** In either layer an idle keep-alive connection owns no Ruby
  fiber. In the epoll fallback it sits in a kernel epoll set. Each connection is registered once, edge-triggered, so there is no `epoll_ctl`
  per request. One Async task waits on that epoll descriptor through the reactor. A pool of 256
  worker fibers (it grows, up to 4096, only when every worker is busy) reads each ready connection,
  answers every complete request in its buffer in order, and writes each response with one `write`.
  Memory is about 3–5 KB per connection (about 130 MB at 40k), and GC drops from about 4 s to about
  0.15 s per minute.
- **A minimal HTTP/1.1 parser.** It reads the request line and the four headers the API uses:
  `Content-Length`, `Transfer-Encoding`, `Connection` and `Authorization`. It supports keep-alive,
  pipelining, partial reads, HTTP/1.0 and `Connection: close`. Chunked request bodies get a 411.
  Response heads are prebuilt frozen strings. The header regex takes each value greedily and trims
  trailing blanks afterwards: the lazy form with a lookahead backtracked along the 200-byte bearer
  token and cost 5.7 µs per POST head against 2.9 µs now.
- **YJIT is warm before the first request.** `start.sh` sets `--yjit-call-threshold=5` (default 30),
  and at startup the server runs the feed, a single post, a JWT verification with a body parse, and a
  like with an invalid id 50 times each through its own parser and app over a socket pair, so the
  whole request path is compiled before it listens. Nothing is written: the body fails validation
  after it is parsed. About 140 iseqs are compiled by the time the "listening" line prints, and the
  first real requests don't pay for compilation. The warm-up token is signed with libsodium, which is
  loaded anyway; loading `openssl` for it cost 8 MB of RSS, and 300 iterations instead of 50 left
  another 27 MB of garbage resident. Idle RSS is 36 MB either way now.
- **io_uring for socket reads and writes in the epoll fallback.** io-event picks io_uring where it
  can. Reads and writes go through the fiber scheduler, so io_uring submits them in batches instead
  of one syscall each. Together with edge-triggered epoll, that cut CPU at 40k users from about 46%
  to 40% on our test VM.
- **Every hot call stays in YJIT.** `Thread::Queue#pop` and `IO#read_nonblock` are defined in Ruby
  with optional arguments, and YJIT ran every call that left one out in the interpreter: 1.7 million
  fallbacks in two minutes at 40,000 users, about two per request. Passing them explicitly
  (`pop(false)`, `read_nonblock(8, nil, exception: false)`) removed them (`--yjit-stats`).
- **About 13 objects per request, from 20.** The request line is checked in place (`match?` with
  `\G` at an offset allocates nothing), so a feed request slices no strings; headers are searched in
  the buffer rather than in a copy of the head; and a response's head, length and body are copied
  straight into a pooled native send buffer, with no response string built first.
- **GC tuned for request garbage.** `start.sh` starts the heap larger and keeps more slots free
  after each GC, so minor GCs run about every 5 s instead of every second, and `server.rb` turns off
  automatic full GCs once warm (one still runs if old objects triple). GC went from about 100 to
  55 ms per 10 s at 72,000 users.
- **No `IO#write`.** `socket.write` holds the socket's write lock while the scheduler's write is in
  flight, and Ruby 4.0 keeps every mutex a thread holds in one linked list that it walks on every
  unlock (`thread_mutex_remove`). With thousands of writes in flight under overload, unlocking took
  11% of the CPU. The epoll layer calls the scheduler's `io_write` directly, and the io_uring layer
  never creates Ruby IO objects for clients.
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
  `nginx.conf` caps out near 16k connections. Responses switch to `Connection: close` only near the
  connection layer's capacity, so clients aren't forced to reconnect under normal load: past it less
  1,280, responses other than the feed close (a client thinks longest after a post, so its slot stays
  free longest), and past it less 256, every response closes. With io_uring that capacity is
  131,070; with epoll it's the descriptor limit.
- **TCP_NODELAY is set on the listener** and inherited by accepted sockets on Linux, so there is
  no `setsockopt` per connection.

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

### What we tried and reverted: serving a new connection's first request from the accept loop

PR #41's benchmark found that past the 65,535-descriptor limit every request arrives on a fresh
connection, and there our CPU per request rose from 50 to 69 µs while PR #84 (PyPy, raw epoll)
stayed at 51. PR #84 sets `TCP_DEFER_ACCEPT`, reads the first request right after `accept`, and
closes a retiring connection without ever registering it in epoll. We implemented the same (queued
to a worker with its bytes, registered in epoll only once idle) and measured it paired against this
tree, on the same two VMs with the target capped at half a core, the C generator at 60 s ramp and
120 s hold, CPU per request being the server process's `utime+stime` over the run divided by requests:

| Regime | before | with the accept path |
|---|---|---|
| 40,000 users, keep-alive | 119.0 µs/req, p99 268 ms | 114.7 µs/req, p99 117 ms |
| 20,000 users, file limit 8,192 (72% of requests reconnect, ~1,000 connects/s) | 209.7 µs/req | 202.3 µs/req |
| 512 clients reconnecting with no think time (~2,300 connects/s, core saturated) | 391 µs/req | 441 µs/req |
| 40,000 users, file limit 8,192 (~2,100 connects/s, overloaded) | fails, 3,114 errors | fails, 13,659 errors |

It wins 3–4% at moderate reconnect rates and loses 13% once the core is saturated by reconnects,
which is the regime it was meant for. Perf put the difference in user space: SQLite's share of
samples rose from 18% to 22% of a larger total, so the same feed query costs more when served in
smaller batches straight from the accept loop than in the cache-hot runs the epoll pass delivers.
`TCP_DEFER_ACCEPT` alone cost 6% in that regime. Two side findings are worth keeping: `TCP_NODELAY` is
inherited from the listener (kept), and a `read_nonblock` on a Ruby socket allocates a 4 KB IO buffer that
lives as long as the connection (`recv_nonblock` doesn't); the scheduler's `io_read` with an
`IO::Buffer` avoids it, which is why memory per connection stays at 3–5 KB.

A perf profile at 40k users: 40% of samples in the kernel (TCP transmit, virtio), about 15% in
SQLite, 1.3% in the regex engine, and only 15k `epoll_wait` calls against 84k requests in 20 s, so
moving the idle set from epoll to io_uring multishot polls would save a few percent at most. That
was true at 40k users. Past the descriptor limit, reconnects dominate instead, which is what the
io_uring layer removes (below).

### k6 with the unmodified `bench/load.js`

Same VMs, 60 s ramp and a 2 min hold (the load VM was raised to 16 GB for the runs above 30k VUs).
Uncapped, at 25k VUs: p95 1.3 ms, p99 10.7 ms, 0 failed of 458,135 requests.

To approximate the droplet's slower core, we capped the target VM at half a core (a CFS quota of
5 ms per 10 ms) and ran the official protocol: 60 s ramp, then a 5-minute hold. k6 was split across
three load VMs (8, 2 and 2 vCPUs) with execution segments, so the generators stayed over 80% idle:

| VUs | p95 | p99 | Failed | CPU per request | Peak RSS | Thresholds |
|---|---|---|---|---|---|---|
| 45,000 | 38 ms | 105 ms | 0% | 154 µs | 179 MB | pass |
| 48,000 | 110 ms | 993 ms | 0% | 156 µs | 199 MB | pass |
| 51,000 | 204 ms | 567 ms | 0% | 157 µs | 212 MB | pass |
| 54,000 | 406 ms | 841 ms | 0% | 156 µs | 235 MB | pass |
| 57,000 | 1.57 s | 1.87 s | 0% | 174 µs | 278 MB | fail (p95, p99) |
| 60,000 | 2.41 s | 2.59 s | 0% | 180 µs | 279 MB | fail (p95, p99) |

The latency comes from the server: its half core had no idle time, while all three load VMs reported
the same percentiles. Past the knee each request costs more CPU (156 → 180 µs), so the queue never
drains. Near the knee single runs vary; other runs of this tree failed at 46k–48k. A half-core cap is
only a stand-in for a droplet vCPU, so treat 54k as an estimate, not a score.

With the cap lifted (one full core), the same protocol passes the video's largest test easily:

| VUs | p95 | p99 | Failed | CPU per request | Peak RSS | Thresholds |
|---|---|---|---|---|---|---|
| 60,000 | 12 ms | 35 ms | 0% | 112 µs | 184 MB | pass |
| 64,000 | 17 ms | 39 ms | 0% | 113 µs | 188 MB | pass |

### Scored the way the leaderboard is: mload

`mload` is the scorer's load generator (`bench/mload` in arjaythedev/million-users-challenge): one
goroutine and one keep-alive connection per user, the `bench/load.js` user model with its think
times, a 60 s ramp and a 5-minute hold, and every request in the run counted. Three load VMs, results
merged, descriptor limit 65,535:

| Users | Result | p95 | p99 | Errors | CPU per request | Peak RSS |
|---|---|---|---|---|---|---|
| 80,000 | pass | 39 ms | 165 ms | 0% | 102.9 µs | 289 MB |
| 85,000 | pass | 145 ms | 465 ms | 0% | 101.3 µs | 345 MB |
| 90,000 | pass | 198 ms | 845 ms | 0% | 101.2 µs | 361 MB |

During the hold at 90,000 users p95 stays at 30–80 ms; the tail comes from the end of the ramp,
when the core runs connection setup and nearly the full request load at once. Before accepts were
spread over the rings and reads served inline, 80,000 failed (p99 2.5 s) and 85,000 failed (p99
2.9 s), with the same p95 in the hold.

### Past the descriptor limit: io_uring direct descriptors

The scored run of `ruby-async-cicloid` (LEADERBOARD.md, Synteq 1 vCPU), the epoll version, gave
71,000 users at 111 µs per request and 83% CPU. On our VM with one full core, the same protocol with
the descriptor limit at 65,535 (k6 split 60/20/20 over three load VMs):

| VUs | Epoll layer | io_uring layer |
|---|---|---|
| 69,000 | pass: 101 / 582 ms, 122 µs, 180k accepts | pass: 76 / 717 ms, 117 µs, 69k accepts |
| 72,000 | fail: 1.27 s / 2.10 s, 128 µs, 255k accepts | **pass: 76 / 565 ms, 115 µs, 72k accepts** |
| 75,000 | fail: 1.26 s / 1.65 s, 134 µs, 334k accepts | fail by 32 ms: 254 ms / 1.03 s on the 60% load VM (690 ms on the others), 116 µs |

With io_uring every user connects once, CPU per request stays flat at 115–117 µs instead of climbing
as the server overloads, and the process holds 15 descriptors with 75,000 connections open (RSS
196 MB). At 80,000 VUs the server held all connections (215 MB) but k6 on the largest load VM ran out
of memory, so we have no result past 75k.

Test suite: 42/42 on x86_64, with either connection layer.

## Trade-offs

- Linux only, because io_uring and epoll are bound through FFI.
- Two connection layers to maintain. The epoll layer is kept because io_uring is often blocked in
  containers and sandboxes, and a server that fails to start scores nothing.
- No idle timeout. Connections stay open until the client closes them, and the capacity guard
  covers the extreme case. In the epoll layer, a worker that picks up a stale edge-triggered event
  waits on that connection until the client sends or closes; the growable pool keeps that from
  starving others. The io_uring layer has no stale events.
- Each response is copied once into native memory for its send, and each receive is copied out of
  its provided buffer into a Ruby string.
- The larger GC heap and per-connection state put peak RSS at 290–360 MB at 80,000–90,000 users.
- Without automatic full GCs, objects that become old and then die stay until old objects triple.
- The epoll layer calls io-event's scheduler `io_read` and `io_write` directly with an `IO::Buffer`,
  which Ruby still marks experimental (it prints a warning at startup). On an epoll-only kernel it
  still works, unbatched.
- The parser covers what this API needs: no chunked request bodies and no `Expect: 100-continue`.

## License

MIT, under the repo's [license](../../LICENSE).
