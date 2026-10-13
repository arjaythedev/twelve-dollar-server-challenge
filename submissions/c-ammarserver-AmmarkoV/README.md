# C + AmmarServer, served direct on :80

| | |
|---|---|
| **Language** | C ( C11, gcc 13, `-O2` ) |
| **HTTP server** | [AmmarServer](https://github.com/AmmarkoV/AmmarServer), pinned by commit in `build.sh`. My own server, so this submission adds the mode it needed rather than working around it |
| **SQLite** | 3.53.0, built from the official amalgamation ( pinned URL + sha256 in `build.sh` ) and linked in, not the apt package |
| **Crypto** | OpenSSL `libcrypto` ( `HMAC`, `CRYPTO_memcmp` ) for HS256 |
| **Nginx or direct** | **Direct**, `0.0.0.0:80`. No Nginx |
| **Threads** | 3, at any load: one event loop serving every connection, one acceptor, one idle `main` |

## Why direct, and what had to change to make it possible

Each k6 VU sleeps 11–30 s per iteration and makes ~2.2 requests, so even 10,000 VUs is only about
1,000 req/s. The scarce resource is not throughput, it is **concurrent connections**: ~15,000 of them,
each nearly always idle. That is what decides the architecture.

AmmarServer was a thread-per-connection server with an epoll layer in front of `accept()` that held a
connection thread-free only until its first byte arrived. That is the right design for its usual job —
a worker blocking in its own `recv()` is woken by the kernel directly, with no epoll round trip, and it
measured ~3x faster than handing connections back to epoll between requests when benchmarked on one
cached file at 138k req/s. It is the wrong design here: 15,000 mostly-idle connections would mean 15,000
threads, which 2 GB does not buy.

So this submission adds **API mode** to AmmarServer ( `AmmServer_StartAPI`, `src/AmmServerlib/threads/
epollAPIServer.c` ): one non-blocking epoll event loop owns every connection from `accept` to `close`,
and answers each request from a single C callback that returns its own status code, content type and
body. An idle keep-alive connection costs an fd and a ~64 byte struct instead of a thread. The epoll
round trip per request that was too expensive at 138k req/s is irrelevant at 1,000 req/s.

Going direct then costs nothing and saves the 14–20% of the single vCPU that Nginx was taking, and
there is no second process holding 15,000 sockets' worth of buffers.

## Measurements

On my own machine, not the benchmark droplet, so read these as ratios and not as scores — a droplet
vCPU is slower, and here k6 shares the CPU with the server:

| | |
|---|---|
| 15,000 idle keep-alive connections | 3 threads, **7.0 MB RSS**, **0 CPU ticks over 12 s** ( no busy loop ) |
| **70,000** idle keep-alive connections | 3 threads, **10.4 MB RSS**, **0 CPU ticks over a 60 s hold**, 70,008 fds. Opened in 4.0 s, all 70,000 answered byte-correct |
| `GET /feed`, existing keep-alive connection | avg **54 µs**, p99 94 µs idle — **65 µs** with 15,000 open, **74 µs** with 70,000 open |
| New connection accepted + adopted | ~67 µs of CPU |
| k6, 2,000 VUs | p95 **300 µs**, p99 **401 µs**, 0.00% errors, 27,064/27,064 checks |
| k6, 10,000 VUs | p95 **293 µs**, p99 **4.7 ms**, 0.00% errors, 183,547/183,547 checks, ~6% of one core |

Per-request cost barely moving between 0 and 70,000 open connections is the property the whole design
rests on, so it is the one worth checking first on the droplet.

### Where the ceiling actually is

Serving direct means one descriptor per connected user, so **`LimitNOFILE=65535` is the connection
limit and therefore the score ceiling**: ~65,527 connections once this process's own handful of
descriptors are counted. Rule 11 says nothing on the box gets tuned, so that limit is taken as given —
what this submission does is make sure all of it is usable:

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
- Kernel memory is not the constraint: ~3 KB of unreclaimable slab per connection, so ~200 MB at
  65,000 — against 2 GB that also holds a 10 MB server process and a 238 MB database it reads through
  `mmap`.

70,000 connections were held on one process in testing, so nothing in the server gives out before the
descriptor limit does.

Going past 65,535 would need several processes behind `SO_REUSEPORT`, which rule 1 permits. This
submission does not, because the other end runs out first. A connection is identified by its
`(source ip, source port, dest ip, dest port)` tuple, so one load-generator address reaching one
`ip:port` can open at most as many connections as it has ephemeral ports — and `ip_local_port_range`
cannot exceed ports 1024-65535, i.e. **64,512, which is already below this server's ~65,527**. Measured
on a stock range ( `32768 60999` ): **28,230 connections from one source address, then `EADDRINUSE`** —
less than half of what one process here serves. So a second process could only ever help if the k6 box
ran several source addresses, and the ceiling it would raise is one the client cannot reach anyway.
Against that, a second process would mean a second SQLite writer, WAL contention between them and a
worse p99 under write load, for nothing.

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

### What I left out, deliberately

**Group commit.** Batching several requests' writes into one transaction is allowed ( rule 6 ), but at
10,000 VUs only ~8% of requests are writes — about 90 commits/s — and with WAL + `synchronous=NORMAL` a
commit is an append to the WAL with no fsync. Batching 90 commits/s into ~10 saves a fraction of a
percent of one vCPU, and costs a response that must be held back after its handler returns and failed
in a batch if the commit fails. Not worth getting wrong. One commit per write request.

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
  remembered. The only things reused between requests are prepared statements and the two buffers the
  event loop reads and writes through.
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
| `feedapi.c` | the five endpoints, routing, SQLite setup, JSON written into the response buffer |
| `jwt.c/.h` | HS256 verification: base64url, the three distinct 401s the spec asks for, `CRYPTO_memcmp` |
| `json.c/.h` | validating parser for `{"body":...}` ( "not JSON" and "no usable body" are different errors ) and the output escaper |
| `install.sh` | apt: `build-essential cmake git libssl-dev curl unzip ca-certificates` |
| `build.sh` | fetches both pinned dependencies, builds the AmmarServer library target only, links `build/feedapi` |
| `start.sh` | `exec build/feedapi` in the foreground, configured only by `SQLITE_PATH` / `JWT_SECRET` / `HOST` / `PORT` |

`build.sh` honours `AMMARSERVER_SRC=/path/to/a/checkout` to build against a local AmmarServer tree
instead of the pinned commit, for developing the two together. CI and the benchmark box never set it.
