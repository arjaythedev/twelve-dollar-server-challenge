# x86-64 assembly (NASM) + epoll + SQLite

The whole server is hand-written x86-64 assembly: event loop, HTTP/1.1 parser, router, JSON
reader/writer, base64url, SHA-256/HMAC and JWT verification. SQLite is the only C code in the
binary, as the rules require. It is built from the pinned amalgamation and called from assembly
through the System V ABI. libc is linked only because SQLite needs it. All socket I/O uses raw
syscalls.

| | |
|---|---|
| Language | x86-64 assembly, NASM 2.16 (Ubuntu 24.04 package) |
| Framework | none: one epoll loop, raw syscalls |
| SQLite | 3.50.4 amalgamation, downloaded by `build.sh` and checked against its sha256 |
| C compiler | gcc (only for `sqlite3.c`) |
| Nginx or direct | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

About 3,200 lines in `src/`: `server.asm` (loop, HTTP, handlers, auth), `json.inc`,
`sha256.inc` and `defs.inc`.

## How it works

One thread runs an epoll loop over one SQLite connection. Each `epoll_wait` batch goes through
four steps:

1. Every ready socket is read once (level-triggered) and all complete requests in the buffer are
   parsed and handled. Partial requests are kept per connection until the rest arrives.
2. The first write of the batch (`POST /posts` or a like) opens a transaction. Every later write in
   the batch joins it. This is **group commit**.
3. Each response's JSON body is built in a scratch buffer, then copied with its status line and
   headers into one batch arena.
4. At the end of the batch the transaction is **committed first**, and only then are all responses
   sent. A 201 therefore always means the row is committed, and no read in the batch can show a
   write that might still roll back. If a commit fails, the connections waiting on it are closed
   instead of being answered.

## Optimizations, and why

- **No Nginx, no framework.** A request costs about 4 µs of server CPU on `/health`, so the
  per-request overhead is in the kernel's TCP stack and in SQLite, not in this code.
- **Responses come out of SQLite without intermediate objects.** Column text is escaped directly
  into the output buffer (`w_jstr`), with no row objects or serializer in between.
- **Prepared statements are created once and reused** (`SQLITE_PREPARE_PERSISTENT`). Like counts
  come from a correlated `count(*)` on the covering `likes_post_id_idx`. I also measured a
  one-range-scan variant (a CTE plus `BETWEEN min AND max`); it was about 30% slower, so the
  simple query stayed.
- **JWT verification without allocation.** The HMAC inner and outer states are computed once from
  the secret at startup, so each token costs 3–4 SHA-256 compressions. Every token is still
  verified on every request. The header (`alg` must be `HS256`) and the payload are parsed with a
  strict RFC 8259 reader.
- **Checkpoints are kept off the request path, and the WAL is kept small.** This one mattered
  most. `wal_autocheckpoint` is off. A background thread with its own connection runs `PASSIVE`
  checkpoints, which does the page copying and the fsync. Under constant writes a background
  checkpoint never quite reaches the end of the WAL, and SQLite only rewinds the WAL after a
  complete checkpoint. Without the next step the WAL grew to 247 MB in 90 seconds, and `/feed`
  slowed from 37 µs to 123 µs, because every page lookup probes every WAL hash segment. So once the
  WAL reaches 4096 frames (16 MB), the event loop finishes the checkpoint itself right after
  sending a batch's responses. Nothing is writing at that moment, so the checkpoint completes and
  the next commit starts the WAL from the beginning. `journal_size_limit` truncates the file.
- **Pragmas** (all allowed by rule 6): `journal_mode=WAL`, `synchronous=NORMAL`,
  `mmap_size=1 GiB` (reads come from the OS page cache), and a small 8 MB SQLite page cache.
- **Keep-alive has a budget, so clients past it are still served.** A keep-alive connection
  that is thinking for 3–15 s holds two scarce things the whole time: an fd on the server
  (`LimitNOFILE=65535`) and a local port on the client. A Linux client has about 28,000 ports by
  default for one server address, so one machine (a NAT, a proxy, a load generator) running more
  users than that fails to connect at all ("cannot assign requested address"), however fast the
  server is. So keep-alive has two budgets: 60,000 open connections in total, and 20,000 per
  client address (counted in 65,536 hashed buckets). New connections past either budget are
  answered with `Connection: close` (one request each); existing keep-alive connections stay.
  - Deciding per connection matters: when I tried it per request, the open count hovered at the
    cap, nearly every response closed its connection, and the churn doubled.
  - A closing response is sent with `MSG_MORE` and the socket closed right after, so the FIN
    rides on the last data segment. The client always sees data and FIN together and closes
    second, so `TIME_WAIT` (60 s) stays on the server, where it costs no fd and no client port.
    Before this, about 2% of short connections raced into `TIME_WAIT` on the client, and at 64k
    users from one address those pinned ports ran the client out.
  - A short connection costs about 10 µs of extra kernel time.
  - If the fd table fills up anyway, the listen socket is taken out of epoll until a connection
    closes, so the loop doesn't spin on `EMFILE`; new clients wait in the kernel's accept queue.
- **Small memory footprint.** Each connection is a 64-byte record indexed by fd. Buffers are
  allocated only for a partial request or for output the socket could not take. The server's RSS
  stays around 25 MB with 64k open connections.

## Results (local, not the benchmark box)

### Native x86-64

Measured on an x86-64 KVM server (AMD EPYC-Milan, 8 vCPU, Ubuntu 24.04, kernel 6.8). The server
ran in a container pinned to one vCPU (`--cpuset-cpus`, 2 GiB, `nofile=65535`), with the database
on local disk. The load came from the Go reimplementation of `bench/load.js` described below:
four processes on four other vCPUs, each connecting from its own loopback address (127.0.0.2–5),
inside the server's network namespace. There is no network card in between, so the kernel's
network cost is lower than on a droplet. 90 s ramp, **2 min hold** (scoring uses 5 min).

| VUs | p50 | p95 | p99 | failed | server CPU (one vCPU) |
|---|---|---|---|---|---|
| 32,000 | 0.3 ms | 0.7 ms | 12–25 ms | 0% | 32% |
| 64,000 | 0.2 ms | 0.7 ms | 37–45 ms | 0% | 36% |
| 96,000 | 0.3 ms | 1.0–1.2 ms | 42–47 ms | 0% | 55% |
| 128,000 | 0.4 ms | 15–17 ms | 52–57 ms | 0% | 74% |
| 160,000 | 0.7 ms | 30–32 ms | 61–67 ms | 0% | 89% |
| 192,000 | 1.9–2.0 ms | 54–57 ms | 83–89 ms | 0% | 95% |

At 192k (about 16,400 requests/s) the core is at 95%, so the ceiling on this machine is a
little above that. No run had an accept-queue overflow or a SYN retransmission. The container
used about 110 MiB at both 96k and 192k (including page cache). CPU per request falls as load
grows, because each `epoll_wait` returns more events: 32k users cost 32% of the core and 64k
only 36%.

### Rosetta-emulated (Apple Silicon)

Measured on an Apple Silicon Mac. The server ran in a `linux/amd64` Ubuntu 24.04 container
pinned to one core (`--cpuset-cpus=0 --memory=2g`, `nofile=65535`), so it was
**Rosetta-emulated**. The database was on the Linux VM's own disk. (On a macOS bind mount,
occasional slow fsyncs caused multi-second stalls that had nothing to do with this code.) The
load came from a Go reimplementation of `bench/load.js` (same user loop, think times and
probabilities, a new connection after `Connection: close`), split over four containers on the
other cores, because k6 itself became the bottleneck at around 15k VUs on this machine. There was
no real network in between, and the kernel's work for the load generators' packets ran on their
cores, not the server's. The numbers are only useful for comparing changes to this code; they
are not a prediction for the droplet.

| VUs (90 s ramp, 5 min hold) | p50 | p95 | p99 | failed | result |
|---|---|---|---|---|---|
| 96,000 | 0.6 ms | 34 ms | 165–176 ms | 0% | pass |
| 112,000 | 1.6 ms | 790–850 ms | 1.8–2.0 s | 0% | fail (CPU) |

At 96k, 60k clients stay on keep-alive connections and the other 36k open a new connection per
request; the server's RSS stays around 25 MB. At 112k the single core runs out at about
10,400 requests/s: no request fails, but queueing pushes p95 and p99 past the limits. Before the
keep-alive cap, everything above about 64k VUs failed outright, because the 65,536th connection
could not be accepted.

**From one client address, as from a single untuned load generator.** Every user above is
spread over four client addresses with a widened port range. From one address with the default
Linux port range (about 28,000 ports), 40,000 users failed with 16% "cannot assign requested
address" errors before the per-address keep-alive budget, all on the client side. With the
budget and FIN-on-the-last-segment they passed: 40k with 0 errors, and 48k with 0 errors, p95 7 ms
and p99 42 ms (2 min holds). Beyond about 50k from one address, the load generator itself ran out
of CPU on this machine: the server stayed under half a core while the generator's own latencies
went up to seconds. So that is as far as I could measure.

The 96k/112k table above was measured before the per-address budget. With four client addresses
the budget doesn't change much: each address carries 24k users at 96k, so 4k of them per address
get short connections, instead of 36k in total under the 60k cap alone. A short connection's
cost dropped in an alternating A/B (`/health` with a new connection per request, three rounds
each): 14–16 µs of kernel time before, 10–16 µs after, because the FIN no longer needs its own
segment. Full-load runs in this environment drifted too much later in the session to compare the
two builds at 96k again.

Taking new connections every 128 events within a batch (not just once per `epoll_wait`) mattered
near the limit. In one 96k run it cut the kernel's accept-queue overflows from 49,442 to 2,840,
and SYN retransmissions from 565 to 0. Each dropped SYN costs the client at least 1 s.

**Tried and dropped: one fd table per thread.** `LimitNOFILE` limits each fd table, not the
process. So I/O threads that call `unshare(CLONE_FILES)`, each with its own `SO_REUSEPORT`
listen socket and epoll, can keep more than 65k connections alive in one process; 112k worked.
But POSIX locks are owned per fd table, so SQLite had to run in `locking_mode=EXCLUSIVE` with no
background checkpoint connection, and all SQLite work had to go through one mutex. It was no
faster: at 96k p99 was about 300 ms instead of 170 ms, and at 112k both versions were CPU-bound.
A new connection costs about 10 µs of kernel time, so serving the clients past 60k with
`Connection: close` is the cheaper and simpler option.

## Trade-offs

This is a server specialised for this API. It implements only the parts of HTTP/1.1 the API
needs: no chunked request bodies and no `Expect: 100-continue`, and request bodies over 32 KB are
rejected with 400. It runs only on x86-64 Linux. `synchronous=NORMAL` means a committed write can
be lost on power failure, though not on a process crash. That is the trade-off rule 6 allows.
