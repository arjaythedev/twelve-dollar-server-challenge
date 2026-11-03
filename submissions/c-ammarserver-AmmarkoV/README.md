# C + AmmarServer, served direct on :80

| | |
|---|---|
| **Language** | C ( C11, gcc 13, `-O2` ) |
| **HTTP server** | [AmmarServer](https://github.com/AmmarkoV/AmmarServer), pinned by commit in `build.sh`. My own server, so this submission adds the mode it needed rather than working around it |
| **SQLite** | 3.53.0, built from the official amalgamation ( pinned URL + sha256 in `build.sh` ) and linked in, not the apt package |
| **Crypto** | OpenSSL `libcrypto` ( `HMAC`, `CRYPTO_memcmp` ) for HS256 |
| **Nginx or direct** | **Direct**, `0.0.0.0:80`. No Nginx |
| **Transport** | `io_uring`, 6 rings, every connection a *direct descriptor* — so `LimitNOFILE=65535` is no longer the connection ceiling. `IO_URING=0` serves from the epoll event loop instead, which is also where it lands by itself on a kernel older than 5.19 |
| **Threads** | 3, at any load: one event loop serving every connection, one acceptor, one idle `main` |

## Why direct, and what had to change to make it possible

Each simulated user sleeps 11–30 s per iteration and makes ~2.2 requests, so even 10,000 users is only
about 1,000 req/s. The scarce resource is not throughput, it is **concurrent connections**: ~15,000 of
them, each nearly always idle. That is what decides the architecture. This round scores with **mload**,
the Go implementation of the `bench/load.js` user model — one keep-alive connection per user, the same
think times, the same 15% likes and 2% posts.

AmmarServer was a thread-per-connection server with an epoll layer in front of `accept()` that held a
connection thread-free only until its first byte arrived. That is the right design for its usual job —
a worker blocking in its own `recv()` is woken by the kernel directly, with no epoll round trip, and it
measured ~3x faster than handing connections back to epoll between requests when benchmarked on one
cached file at 138k req/s. It is the wrong design here: 15,000 mostly-idle connections would mean 15,000
threads, which 2 GB does not buy.

So this submission adds **API mode** to AmmarServer: one event loop owns every connection from `accept`
to `close`, and answers each request from a single C callback that returns its own status code, content
type and body. An idle keep-alive connection costs a ~32 byte struct instead of a thread. The event loop
round trip per request that was too expensive at 138k req/s is irrelevant at 1,000 req/s.

API mode has two transports under the same callback, same SQLite and same three threads, so that they
are comparable: an epoll event loop ( `src/AmmServerlib/threads/epollAPIServer.c` ) and an `io_uring`
one ( `uringAPIServer.c` ), with the HTTP/1.1 framing they share factored out into `apiProtocol.c`.
**The ring is what ships**, because an epoll connection costs a file descriptor and a ring connection
does not — see the ceiling section — and because it measured faster at every concurrency tried: ~6%
more throughput and ~6% less CPU per request from 50 connections up, with a tail a third shorter. It
falls back to epoll, with a line saying so, if the kernel will not give it a ring.

Going direct then costs nothing and saves the 14–20% of the single vCPU that Nginx was taking, and
there is no second process holding 15,000 sockets' worth of buffers.

## Measurements

On my own machine, not the benchmark droplet, so read these as ratios and not as scores — a droplet
vCPU is slower, and here mload shares the CPU with the server:

All of these were taken with the process's descriptor limit set to the droplet's `65535`, on what the
submission ships ( 6 rings ):

| | |
|---|---|
| **70,000** idle keep-alive connections | 3 threads, **17.7 MB RSS**, **1 CPU tick over a 60 s hold**, and **20 file descriptors** — the connections are not descriptors. Opened in 3.3 s, every one of the 70,000 answered byte-correct, then all 70,000 answered again after the hold |
| `GET /feed`, existing keep-alive connection | avg **55 µs**, p50 55 µs, p99 84 µs idle — avg **62 µs** with 70,000 connections open |
| `GET /health` / `GET /posts/:id`, same | avg **19 µs** / **27 µs** |
| `POST /posts` sustained, one connection | **~5,500 writes/s**, p50 **74–93 µs**, p99 ~190 µs, worst 106–110 ms ( an auto-checkpoint ). Over 20,000 writes, twice, 4 and 7 of them stalled past 16 ms and ~150 past 1 ms. Repeated runs vary ±10% either way and it is the WAL doing the varying, so read the rate as the order of magnitude it is |
| `GET /feed` at 500 concurrent connections | **27,000–29,500 req/s** at one full core, 35–36 µs of CPU per request. Six rings cost nothing against four and if anything help slightly: interleaved three times, 27,953 / 28,517 / 28,391 req/s on six against 27,276 / 27,115 / 27,507 on four |
| the same against **one** ring | also nothing. Interleaved four times at saturation, **56.3 µs** of CPU per request on six rings against **56.8 µs** on one — so the `epoll_wait` + `read(eventfd)` + `io_uring_enter` that several rings make the thread wait through does not show up against the single `io_uring_enter` one ring blocks on. Six rings are therefore free, and they are what lifts the connection ceiling |
| mload, 2,000 users ( 60 s ramp, 5 min hold ) | p95 **219 µs**, p99 **289 µs**, 0.00% errors, 70,960/70,960 requests, ~1.8% of one core |
| mload, 10,000 users ( 60 s ramp, 5 min hold ) | p95 **251 µs**, p99 **430 µs**, 0.00% errors, 354,196/354,196 requests, ~7.5% of one core |
| mload, 60,000 users | **0.000% errors**, 5,817–5,822 req/s, p95 **9.8–10.8 ms**, p99 **29.5–30.7 ms**, ~36% of one core, 60 MB RSS. Before the receive-buffer fix below the same run lost **2,700–3,300 requests to the 60 s timeout** every time, at 0.40–0.49% |
| mload, 100,000 users | **0.000% errors**, 9,203 req/s, p95 **28 ms**, p99 **34 ms**, **57% of one core**, 68 MB RSS |

Per-request cost barely moving between 0 and 70,000 open connections is the property the whole design
rests on, so it is the one worth checking first on the droplet.

The 60,000 and 100,000 user rows ran with a 30–45 s ramp and a 90 s hold, the server under `taskset -c 0`
so that it has the droplet's one vCPU, and mload on the other fifteen cores with its users spread over
eight loopback addresses — below about 25,000 users against a single address the generator runs out of
ephemeral ports before the server runs out of anything. Server CPU is read from the second half of the
samples, because the ramp otherwise drags the figure down by about a third. **150,000 users is past what
this box can measure**: mload itself falls behind there ( 13,568 req/s against the ~15,900 that many users
should ask for, p95 820 ms ) while the server is still only at 60% of its core, so that number says
something about the generator and nothing about the submission.

Every row in this table is the ring transport as shipped. The mload rows used mload at `8f02bc6` with
the original `load.js` think times ( `-think-scale 1`, the default ) and no `-gzip`, because k6 does
not send it; a separate 1,000-user `-gzip` run at ~2,000 req/s had zero errors, since compression is
not implemented and the server ignores `Accept-Encoding`.

### Where the ceiling actually is

Serving direct used to mean one descriptor per connected user, which made **`LimitNOFILE=65535` the
connection limit and therefore the score ceiling**: ~65,527 connections once this process's own handful
of descriptors are counted. That is measured, exactly, and it is what the epoll transport still does —
`IO_URING=0` stops at **connection 65,527** and refuses the next one. Rule 11 says nothing on the box
gets tuned, so the limit itself is taken as given. Two things follow from it: all of it has to be
usable, and then it has to stop being the ceiling.

Making all of it usable:

- The process raises its own `RLIMIT_NOFILE` soft limit to the hard limit at startup and prints both.
  A default shell hands out a soft limit of 1024 against a far higher hard limit, and nothing would
  fail loudly at 1024 connections — `accept()` would just start returning `EMFILE` with tens of
  thousands of descriptors still going spare. Raising a limit inside this process is what rule 11
  permits; it tunes nothing on the box and asks for nothing the service was not given.
- Running *out* of descriptors degrades instead of collapsing. `accept()` returning `EMFILE` leaves
  the unacceptable connection in the backlog with the listening socket readable, so retrying it
  immediately spins the accept loop — and its error logging — against the 65,000 connections still
  being served. Instead it backs off 50 ms and logs at most one line a second. Measured by running a
  server with `ulimit -n 2048` and throwing 3,000 connections at it: **1 CPU tick over 10 s** while
  saturated, and the connections it did accept kept being served.
- Kernel memory is not the constraint at that scale: ~3 KB of unreclaimable slab per connection, so
  ~200 MB at 65,000 — against 2 GB that also holds a ~17 MB server process and a 238 MB database it
  reads through `mmap`.

And then not being bounded by it at all, which is what the `io_uring` transport is for. There are two
ways past 65,535 and this takes the second. Several processes behind `SO_REUSEPORT` each get their own
descriptor budget, which rule 1 permits, at the cost of a second SQLite writer and WAL contention
between them. Or one process stops spending descriptors: a multishot accept with
`IORING_FILE_INDEX_ALLOC` installs each new socket straight into the ring's own registered file table,
giving a **direct descriptor** — an index into that table rather than an entry in the process descriptor
table. `RLIMIT_NOFILE` is never charged for it, and the size of the table becomes the bound instead.

A table is where the multiplier comes from. It may be *declared* no larger than `RLIMIT_NOFILE`, but
the kernel checks that once, at registration, and never again — so N tables are N times the connections
from the same limit, in one process, on one thread, with one SQLite writer. That is why this ships six
rings: **393,210 connection slots under `LimitNOFILE=65535`**. They fill one at a time, so the second
ring costs nothing until the first is full, and an idle ring costs one syscall a second — six of them
cost one CPU tick per minute while holding 70,000 connections, measured.

Measured, with the server's hard limit lowered to 1024 so the whole ladder fits on one machine, every
connection checked answered:

| transport | connections held under a 1024 limit |
|---|---|
| epoll | 1,016 |
| `io_uring`, 1 ring | 1,024 — the whole limit, since the listener, the rings, the database and stdio no longer compete with connections for it |
| `io_uring`, 2 rings | **2,048** |
| `io_uring`, 4 rings | **4,096** |

And at the droplet's own limit: with `RLIMIT_NOFILE` at 65,535, **70,000 concurrent connections were
opened, answered, held for a minute, and answered again** — by a process holding 20 real file
descriptors as shipped, or 12 with two rings. The epoll transport on the same box, same limit, same
client, stopped at 65,527. There is no reason 70,000 is the end; it is what the test asked for.

Two details that this rests on, both of which cost something to get right:

- **The accept moves to the next ring 64 slots before the current one fills**, not when it runs out. A
  multishot accept armed on a ring whose table is full fails *after* the kernel has accepted the socket,
  so that client gets a reset rather than a wait, and moving over at the last slot would lose one
  connection per ring. With the margin, all 4,096 slots above were used and nothing was dropped in
  between; only at genuine saturation does a connection get refused, which is the wall it always was.
- **One thread still waits for all of the rings at once**, through an eventfd per ring and one
  `epoll_wait`. That combination is not obvious: a registered eventfd does wake an `epoll_wait` on a
  ring created with `IORING_SETUP_DEFER_TASKRUN`, while polling the ring's own descriptor — the first
  thing anyone would try — never becomes readable at all under that flag. With one ring the thread
  blocks on the ring itself and there is no eventfd, which is why `IO_URING=1` costs ~3 µs per request
  less than `IO_URING=4` at a concurrency of one, and nothing at all from 50 connections up.

So the descriptor limit is no longer what ends a run. What should end it is the single vCPU. The honest way to size that is
per user rather than per request: this cost ~7.5 µs of CPU per second per simulated user in the
10,000-user mload hold, so one core is ~133,000 users here, and a droplet vCPU being 1.5–2x slower puts
it somewhere around **66,000–89,000**. Kernel memory is further out still: 3.07 kB of unreclaimable slab
per connection, measured over 70,000 of them, so 2 GB does not become the wall until roughly 450,000. Both
are now ahead of the descriptor count instead of behind it.

The client side is worth knowing either way. A connection is identified by its
`(source ip, source port, dest ip, dest port)` tuple, so one load-generator address reaching one
`ip:port` can open at most as many connections as it has ephemeral ports — and `ip_local_port_range`
cannot exceed ports 1024-65535, i.e. **64,512**. Measured on a stock range ( `32768 60999` ): **28,230
connections from one source address, then `EADDRINUSE`**. Any harness that reaches past that is using
several source addresses, so these numbers are not a ceiling on the score — they are the first thing to
rule out when a run ends near 28,000 or 64,500, because there the error is `EADDRINUSE` on the client,
not `EMFILE` on the server.

## Expected performance on the droplet

Everything above was measured on a 16-core machine with mload sharing it, so none of it is a score. What
carries over is the cost per unit of work, and that turned out to be the same figure at both of the loads
worth believing:

| | req/s | server CPU | µs of CPU per request |
|---|---|---|---|
| mload, 60,000 users | 5,820 | 36% of one core | **61.9** |
| mload, 100,000 users | 9,203 | 57% of one core | **61.9** |

Two loads a factor of 1.7 apart agreeing to three digits is what makes the extrapolation worth writing
down at all. Scaling it by a droplet vCPU being 1.5–2x slower than a core here:

| | measured here | expected on the droplet |
|---|---|---|
| CPU per request | **62 µs** | 93–124 µs |
| CPU per user per second | 6.6–7.5 µs | 10–15 µs |
| requests/s at one full core | ~16,100 | **8,000–10,800** |
| users at one full core, `-think-scale 1` | ~133,000–150,000 | **66,000–100,000** |
| RSS at 100,000 connections | 68 MB | the same — memory does not care how fast the core is |

So the expectation is **somewhere between 66,000 and 100,000 users**, and the honest width of that range
is the 1.5–2x guess about the vCPU, not anything about the server. The two ends come from the two ways of
dividing the same runs: 7.5 µs of CPU per user per second in the 10,000 user hold, which is the figure the
section above uses, and 6.6 µs in the 100,000 user one — fixed per-user cost matters less the busier the
core is, which is why the top end here is higher than the 89,000 quoted up there.

Latency should stay unremarkable until the core does. Here p99 was **430 µs** at 10,000 users with the
core 7.5% busy, and **30 ms** at 60,000 with it 36% busy — queueing, not work. On the droplet the same
user counts put the core at roughly 11–15% and 54–72%, so expect p99 in the low milliseconds up to about
20,000 users and in the tens of milliseconds past 50,000, with the knee wherever the core actually runs
out. The 60 s client timeout should not be reached at all: the run that used to reach it was the
receive-buffer stall below, not load.

The walls arrive in this order, which is the point of the section above:

1. **The single vCPU**, at 66,000–100,000 users.
2. **Kernel memory**, at roughly 450,000 connections — 3.07 kB of unreclaimable slab each, measured over
   70,000 of them, against 2 GB.
3. **Descriptors are not a wall any more**: 393,210 direct-descriptor slots under `LimitNOFILE=65535`.

What would make the droplet miss the low end of that range is not throughput but the WAL. `POST /posts`
is ~8% of mload's requests and an auto-checkpoint already costs 106–110 ms here on an NVMe SSD; on
droplet storage that is the one number I would expect to be worse than a straight 1.5–2x, and it lands
on p99 rather than on the user count.

### On a machine with more cores

It would not go faster, and that is a choice rather than a limit: the transport is **one thread** for all
six rings, because the box this is written for has one vCPU and a thread pool there would only add
context switches, locking, and a second SQLite writer to serialise against the first. On 16 cores,
fifteen of them would idle and the ceiling above would not move.

The same 62 µs per request says what threading it would be worth: 16 cores is ~258,000 req/s in theory,
and at the 60–70% that one SQLite writer, softirq and memory bandwidth would realistically leave, about
**1.5–1.7 million users** at these think times. Reaching that is three changes — a ring set and an
`io_uring` thread per core ( `IORING_SETUP_SINGLE_ISSUER` is per ring, so this is the natural shape ),
the HS256 key schedule made thread-local instead of a bare global, and something better than one writer
for `POST`. None of them belong in a submission for a one-vCPU box, so none of them are here. The challenge
author's own sibling [Million Users Challenge](https://github.com/arjaythedev/million-users-challenge) is
where that version would go, and the arithmetic is inviting — a million users there is only ~42,400
req/s, about 2.6 cores of the 16 at this cost per request — but it asks for Postgres on another machine
instead of in-process SQLite, which changes the per-request cost this whole section is built on.

## What is optimized, and why

**One thread, no locks anywhere.** The box has one vCPU, so a thread pool would only add context
switches and contention to work that is already short. Because every request is served on the same
thread, SQLite is compiled `SQLITE_THREADSAFE=0` and uses one connection and one set of prepared
statements — not a single mutex is taken on the way to the data. Nothing in the API code needs locking
either.

**Prepared once** ( rule 5 allows it: a query plan is cached, no data is ). Six statements, prepared
`SQLITE_PREPARE_PERSISTENT` at startup and reused for the life of the process.

**SQL that matches the fixed indexes.** The feed is 20 rows off the front of `posts_created_at_id_idx`
( which is already `(created_at DESC, id DESC)` ), not a sort of 500,000 posts. `like_count` is the
correlated subquery over `likes_post_id_idx`, counted per request.

**SQLite built from the amalgamation**, for the compile-time options apt cannot give: `THREADSAFE=0`,
`DEFAULT_MEMSTATUS=0` ( drops an atomic per malloc ), `OMIT_AUTOINIT`, `DQS=0`, `USE_ALLOCA`,
`OMIT_LOAD_EXTENSION`. It also pins the exact SQLite the benchmark runs.

**`mmap_size` covering the whole ~238 MB database**, so reads come out of the page cache with no copy
into SQLite's own cache. Rule 5 names SQLite's page cache and mmap as fair game. `cache_size` is kept
to 32 MB so the two together stay far from 2 GB.

**One `send()` per response, no copy of the body.** The handler writes its JSON into a buffer that has
256 bytes reserved in front of it; the status line and headers are then written *backwards* into that
reserve, so header and body are one contiguous block. Partial writes are queued and finished on
`EPOLLOUT`.

**JSON written by hand, straight into the response buffer.** No intermediate document, no allocation
per request. Escaping is only what JSON requires ( `"`, `\`, C0 controls ); the spec makes escaping
`<>&/` and non-ASCII optional, and not doing it is both fewer bytes and less work.

I benchmarked this against the obvious alternative — letting SQLite build the whole feed response with
`json_object`/`json_group_array` and just copying the result out — over 20,000 iterations on the warm
seed database:

| building `GET /feed` | per request |
|---|---|
| hand-written into the response buffer | **24.4 µs** |
| `json_object` + `json_group_array` | 29.4 µs |

Both emit byte-identical 5,102-byte responses, so SQLite's version is correct here and would have been
a fair bit less code. It is 1.2x slower because it allocates and reallocs strings inside the VM to
build a shape that is fixed anyway, and the result then still has to be copied into the response
buffer. 5 µs a request is not the difference between scores, so the stronger reason to keep the hand
written version is that the spec pins the key order and the exact bytes, and writing them directly is
the version where that is obvious rather than dependent on how an aggregate orders its input.

**`body` is measured in characters, not bytes** — the schema's own `CHECK` is `length(body) BETWEEN 1
AND 500` and SQLite's `length()` counts characters, so counting bytes would reject a legitimate
500-character body with accents in it and accept one the `CHECK` would then refuse.

**No per-request logging.** API mode never touches the access/error logs; AmmarServer writes one
startup line and nothing after it. Nginx's log would have had the same information anyway, and there
is no Nginx.

**The HS256 key schedule is prepared once.** HMAC pads the key out to a 64 byte block and hashes
`ipad||message`, then `opad||inner`. The first block of each of those two hashes depends on the key
alone, so it is the same block for every token this server will ever see — and OpenSSL's one-shot
`HMAC()` re-derives both of them, and on OpenSSL 3 fetches the MAC out of the provider as well, on
every single call. The two SHA-256 states are now built once and copied per token with
`EVP_MD_CTX_copy_ex`: **1.48 µs a token down to 0.25 µs**, measured over 200,000 tokens, byte-identical
output. Rule 5 is untouched — every signature is still computed over the bytes that arrived and compared
in full with `CRYPTO_memcmp`; what is kept is the *key's* schedule, which is the same kind of thing a
prepared statement keeps about a query. It is worth saying how small this is in the score: `GET /feed`
and `GET /posts/:id` carry no token, so only the ~8% of mload's requests that write ever verify one, and
1.2 µs of those is about **0.1 µs of an average request**. It is free and it is strictly better, but it
is not what moves the number.

### The receive-buffer stall, which is what actually moved the number

At 60,000 users the shipped build dropped **2,700–3,300 requests per run to the 60 s client timeout**,
reproducibly, while using 34% of its one core. Nothing was saturated: it was a lost-wakeup bug in the
ring transport.

A connection's receive is a *multishot* `IORING_OP_RECV` reading into whichever provided buffer is free
when bytes turn up, which is what makes an idle connection cost nothing but its struct. When every
provided buffer is momentarily in hand, that multishot receive completes with `-ENOBUFS` and is **not**
re-armed — correctly, because there is nothing to re-arm it into. The transport noted that and returned.
But a connection in that state has *nothing outstanding on it at all*: no completion will ever arrive
for it again, so nothing would ever notice it needed arming. It was not slow, it was gone, until its
client gave up a minute later. The same silent dead end existed when `ArmRecv` could not get a
submission queue entry.

The fix is the queue the send tickets already had: a connection that could not have its receive armed
waits in a per-ring list, and `RecycleRecvBuffer` arms **one** of them each time a buffer comes back.
One re-arm per returned buffer is what keeps it from spinning — a connection that finds the buffer
already taken simply queues again, and the next attempt waits for the next buffer, which waits for a
request to be served. Closing stays constant time: a connection that closed while queued is skipped
when it comes up rather than unlinked. Slot reuse had to stop clearing the queue links for the same
reason, or a slot that came back into use while queued would cut the queue in two and strand everything
behind it — which the send-ticket queue had been exposed to as well.

Measured, interleaved, two rounds each at 60,000 users:

| | errors | req/s |
|---|---|---|
| as it shipped | 0.40–0.49% , all of them 60 s timeouts | 5,553 / 5,578 |
| the re-arm fix, pool left exactly as it was | **0.000%** | 5,817 / 5,822 |
| the re-arm fix and an eight times larger pool | **0.000%** | 5,819 / 5,820 |

The third row is the one worth reading twice: **enlarging the buffer pool was not the fix and is not
shipped.** It was the first thing that made the timeouts go away, which is exactly why it was worth
testing on its own — the pool had only hidden the bug, and the bug was still there to be hit by any
burst large enough. The re-arm costs nothing and 1,024 buffers is what ships. Throughput going *up* by
5% is the requests that used to be dropped now being answered; p95 rises with it, from 5.2 ms to 9.8 ms,
for the same reason, and that is the honest direction for it to move.

### What I left out, deliberately

**Group commit.** Batching several requests' writes into one transaction is allowed ( rule 6 ), but at
10,000 users only ~8% of requests are writes — about 80 commits/s — and with WAL + `synchronous=NORMAL` a
commit is an append to the WAL with no fsync. Batching 80 commits/s into ~10 saves a fraction of a
percent of one vCPU, and costs a response that must be held back after its handler returns and failed
in a batch if the commit fails. Not worth getting wrong. One commit per write request.

**One range scan over `likes` instead of 20 counted lookups.** The feed's `like_count` is a correlated
subquery per row, so a feed costs 20 separate lookups into `likes_post_id_idx`. The newest 20 posts
have near-consecutive ids ( 499,981-500,000 in the seed ), so all 20 counts can be had from a single
`post_id BETWEEN ? AND ?` range scan instead. I measured it both ways against the seed database, 20,000
iterations each after a warm pass: **20.5 µs** for the 20 subqueries as it stands, **25.2 µs** for the
range scan joined back to the rows in C, and **40.3 µs** for the same thing expressed as one statement
with CTEs ( which materializes two of them, builds an automatic index on the counts and then sorts the
result in a temp b-tree ). The reason it loses is that there was never much there to win: the same rows
with no counts at all take 11.6 µs, so all 20 lookups together cost 8.9 µs — about 0.45 µs each,
because `likes_post_id_idx` is a covering index and 20 adjacent `post_id`s sit in the same few pages of
a database that is entirely `mmap`ed — while one `GROUP BY` statement costs 9.4 µs on its own, before
any of its rows are used. It is the right optimisation when the per-row lookup is a real seek; here it
is a page-cache probe, and the statement overhead is bigger than the work it replaces.

### A note on `-march=native`

Both the amalgamation and the API are compiled `-march=native -mtune=native`. Everything is built from
source on the machine that runs it, so the binary is tuned for that exact CPU. In my own measurements
it made no difference worth reporting — building `GET /feed` took 24.3 / 24.7 / 25.4 µs without it and
24.2 / 24.7 / 25.0 µs with it across three runs, which is inside the run-to-run noise, because SQLite's
hot path is pointer chasing and byte comparison rather than anything a wider register helps with. It
may do better on a different core than mine. The cost is that the binary is only valid on the host that
built it: if the instruction set underneath it ever changed, that would be a `SIGILL` rather than a slow
server.

## Rule notes

- **Rule 5, no caching across requests.** Every response is built from a SQLite read taken while the
  request is being served. Every token's HMAC is recomputed from scratch — nothing about a token is
  remembered. The only things reused between requests are prepared statements and the buffers the
  transport reads and writes through — the ring's provided receive buffers and its 32 response buffers,
  which are pooled rather than per connection and hold nothing once a request has been answered.
- **Rule 6, durable before responding.** WAL + `synchronous=NORMAL`. `INSERT ... RETURNING` is stepped
  all the way to `SQLITE_DONE` before the 201 is written, because with `RETURNING` it is that, not the
  first row, that ends the implicit transaction.
- **Rule 3, schema untouched.** No tables, columns, indexes or triggers added. Foreign keys are left
  off ( SQLite's default ), so whether a post exists is asked outright before inserting a like —
  otherwise a like on a post that never existed would insert happily and answer 201 instead of 404.
- **Rule 10, built from source, nothing prebuilt.** `build.sh` fetches AmmarServer at a pinned commit
  and SQLite at a pinned URL verified by sha256, and links the three static archives it just built by
  full path, so a stale `libAmmarServer.so` in the tree can never be picked up instead.
- **Rule 11, nothing on the box tuned.** No kernel parameters, no CPU pinning, no `nice`. An explicit
  config path is passed to AmmarServer so its automatic lookup cannot pick up a stray
  `ammarServer.conf` or `/etc/ammarServer.conf` and change the port or thread settings underneath the
  benchmark.

## Files

| | |
|---|---|
| `feedapi.c` | the five endpoints, routing, SQLite setup, the response written through AmmarServer's own writers |
| `jwt.c/.h` | HS256 verification: base64url, the distinct 401s the spec asks for, `CRYPTO_memcmp` |
| `install.sh` | apt: `build-essential cmake git libssl-dev curl unzip ca-certificates` |
| `build.sh` | fetches both pinned dependencies, builds the AmmarServer library target only, links `build/feedapi` |
| `start.sh` | `exec build/feedapi` in the foreground, configured only by `SQLITE_PATH` / `JWT_SECRET` / `HOST` / `PORT` |

`build.sh` honours `AMMARSERVER_SRC=/path/to/a/checkout` to build against a local AmmarServer tree
instead of the pinned commit, for developing the two together. CI and the benchmark box never set it.

The AmmarServer side of this is four files, and none of them is about this submission in particular:

| | |
|---|---|
| `threads/apiProtocol.c` | the HTTP/1.1 framing both transports share, lifted out of the epoll loop without changing its behaviour — plus the request and response helpers below |
| `threads/uringAPIServer.c` | the ring transport, compiled to a stub where the kernel headers are too old |
| `tools/json_tools.c` | the JSON a handler needs: read one string field out of a request body, escape text into a response. This used to be the submission's own `json.c` |
| `AmmServerlib.h` | what the above declare |

What moved up there rather than staying here, because none of it is specific to this API — a handler that
writes its answer through `AmmServer_APIResponseWrite*` cannot overrun the buffer it was given, since the
first write that does not fit marks the response and every later one does nothing, so there is one check
at the end instead of one per write:

- `AmmServer_APIResponseWrite` / `WriteText` / `WriteInteger` / `WriteJSONString` / `Reset` / `Overflowed`
- `AmmServer_APIRequestMatch(rqst,"/posts/%lld/like",&id)`, in place of hand written `memcmp` routing
- `AmmServer_APIRequestBearer`, `AmmServer_APIRequestQueryParam`, beside the existing `...RequestHeader`
- `AmmServer_JSONGetStringField`, `AmmServer_JSONEscape`
- `AmmServer_StartAPIOnRings`, and the descriptor soft limit being raised to the hard limit before the
  ring tables are sized — which a caller used to have to know to do for itself, as this one does

The 42 suite checks, the 13 framing checks and a further 24 routing and auth edge cases all answer
byte-identically to the build from before that move.
