# Go + SQLite (io_uring)

A single-threaded HTTP/1.1 server on raw `io_uring` syscalls, with SQLite compiled in through cgo.
No framework, no `net/http`, no `net` listener: the event loop, the HTTP parser, the JSON writer and
the JWT verifier are all in this folder.

| | |
|---|---|
| Language | Go 1.27.3 (official tarball) and C (the compiler from `build-essential`) |
| Framework | none |
| I/O | hand-written `io_uring` event loop on raw syscalls (`io_uring_setup`/`enter`/`register`), 8 rings, registered descriptor tables, multishot accept and receive, provided buffer rings |
| SQLite | 3.54.1 amalgamation. `build.sh` downloads it and verifies both its SHA-256 and the SHA3-256 that sqlite.org publishes, then compiles it itself |
| Dependencies | none outside the Go standard library and SQLite |
| **Nginx or direct** | **Direct**: the server binds `HOST:PORT` itself (`0.0.0.0:80` on the box) |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, ca-certificates, curl, unzip, openssl; Go 1.27.3 to /usr/local/go
bash build.sh          # downloads + verifies SQLite, builds it twice (profile-guided), builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` picks the Go tarball from `dpkg --print-architecture` and pins a SHA-256 for amd64 and
arm64 both: the benchmark box is x86-64 and local development is on arm64.

## Design

**One thread owns everything.** `GOMAXPROCS=1`, `runtime.LockOSThread`, and SQLite built with
`SQLITE_THREADSAFE=0`, which removes every mutex from SQLite and makes it unsafe to enter from a
second thread. The loop thread opens the database and runs every request itself. There is no
handoff, no channel and no lock anywhere on the serving path.

**Direct, not behind Nginx.** Nginx's `worker_connections` would cap the entry below the connection
count this design aims at, and Nginx takes CPU from the one vCPU.

### The event loop

**Connections are registered descriptors, not file descriptors.** Accept is multishot with
`IORING_FILE_INDEX_ALLOC`, so an accepted socket lands in the ring's own descriptor table and never
occupies a process fd. This is what gets past `RLIMIT_NOFILE`: the kernel checks a table's size
against the soft limit once, at registration, and the table belongs to the ring rather than to the
process, so **8 rings hold 8 x RLIMIT_NOFILE connections in one process**. Verified locally: 2,499
connections held and still answering over 960-slot tables, on **16 process file descriptors**.

`raiseNOFILE()` lifts the soft limit to the hard one at startup. Under a systemd unit with
`LimitNOFILE=65535` that changes nothing, because a single systemd value sets soft and hard alike;
under a stock unit, which ships `1024:524288`, it is the difference between a thousand connections
per ring and sixty-five thousand. It makes capacity depend on the hard limit rather than on who
started the process.

**Accept moves between rings before a table fills, not after.** A table-full accept is terminated
with `ENFILE` *after* the kernel has already accepted the socket, so the kernel closes it and that
client sees a reset. The loop arms accept on the next ring and cancels the current one while a
margin of slots is still free. The `ENFILE` path remains as the backstop.

The margin has to fit inside what shedding leaves free, or it can never be satisfied and every
accept lands in a table that is about to fill; shedding holds back a twentieth of each table, so
the margin reserves an eighth of that. When no table has room, nothing is armed at all: the
arrival waits in the listening backlog for the microseconds until a close frees a slot, which
costs it queueing time instead of an error.

**Arming an accept is idempotent**, which matters because several completion paths lead to it and
more than one can run while an accept is live on another ring - the planned handoff arms the next
ring *before* cancelling this one, so the cancel completes with accept already armed. A second
multishot accept on the listening socket is one no handoff can reach, because the handoff test
only fires for the ring accept is recorded on. It runs until its own table fills, resets the
client whose socket the kernel had already accepted, and the backstop then arms another. That is
a cascade and not a one-off: without the guard, 97% of handoffs were forced by a full table and a
5-minute hold reset 2,080 clients; with it, every handoff is planned and the same hold resets
none.

**The cross-ring wakeup is a one-shot poll, re-armed by hand.** A *multishot* poll on another ring's
fd reports `IORING_CQE_F_MORE` and then delivers exactly one event, ever. That is measured, not
assumed, and the measurement includes the control that makes it surprising: it behaves the same with
`IORING_POLL_ADD_LEVEL` set, so it is not edge-triggering, and the same against a registered
eventfd. `poll_linux_test.go` asserts all six variants, including the negative results, so a kernel
that ever starts delivering them will fail the test rather than silently leave this code in place.

**Receives are multishot into provided buffer rings**, so the request bytes are read straight out of
the kernel's buffer and never copied on the fast path. A partial request is the only case that
copies, into a pending buffer held only for as long as the request is incomplete.

**Closing is two-phase.** A connection's response buffer is returned to the pool only once the close
has completed *and* no send is still in flight, because the kernel reads a send buffer after
`io_uring_enter` returns. Releasing it earlier handed live bytes to the next connection.
`close_linux_test.go` pins that.

**A shed connection waits for the client's FIN rather than sending one.** Closing first makes this
side the active closer, and the socket then sits in `FIN_WAIT_2` still holding the response it just
sent, charged to this cgroup until the client acknowledges. A 5-minute hold that shed 77,000
connections grew kernel socket memory by 300 MB that way with the connection count flat, and that
memory is the unreclaimable kind. Two measurements pin it down. A control hold of 4,000
connections with nothing closing kept `sock` at exactly zero and `orphan` at zero for five minutes,
so the memory tracked the closing and not the holding. Repeating the shedding hold with the wait in
place kept `sock` at zero through 21,409 sheds and took the peak from 913 MB to 574 MB. Every one
of those connections was closed by the client and none hit the wait's own timeout.

Waiting costs a slot in this server's own table for the
millisecond a client takes to close, and a slot is not a socket buffer. A lingering connection is
swept after 10 s, and anything it sends after being told the connection is ending is dropped rather
than answered.

**Shedding, so capacity degrades instead of falling over.** The load model leaves a user idle 3-15 s
between requests, so slots bound concurrent *connections*, never concurrent *users*.

Two pressures drive it, and they pull different levers, because the two resources are freed by
different means:

- **Slots**, at 95% and 98% of `rings x slots`, drive **retention**. A slot given up by an idle
  user is a slot a waiting one can have, so closing is what frees the resource that is short:
  above the soft threshold answers carry `Connection: close` and keep-alive clients cycle out on
  their own, and above the hard one the idle sweep stops being housekeeping and starts reclaiming.
  The thresholds sit close to full on purpose. At 75% capacity does not degrade gracefully, it is
  *capped*: the connection count pins itself to the threshold for the whole hold and the last
  quarter of the machine goes unused.
- **Memory**, at 75% and 87.5% of the cgroup limit, drives **admission** instead - it stops arming
  the accept. Closing gives memory back only in the sense that the client reconnects, so shedding
  holds the population level and frees nothing; what it frees is a slot, which was never the
  scarce resource. Stopping admission is the only thing in this design that bounds how far the
  population can grow. At the critical threshold it sheds as well, to drain rather than merely
  stop growing, but it deliberately does not tighten the idle sweep: evicting users faster only
  raises the rate at which they reconnect, and each reconnection is a new socket.

The shed decision counts the connections it can still act on, not the slots occupied. A lingering
connection holds a slot but has already been told to go, so counting it would make the decision
react to its own output and stay latched for as long as lingerers arrive at the rate shedding
creates them.

Memory is read from `memory.stat` every tick, and only the unreclaimable fields count - `anon`,
`slab_unreclaimable`, `sock`, `kernel_stack`, `pagetables`, `percpu`. Measured on a 5-minute hold,
396 MB of a 556 MB peak was page cache holding the WAL, which the kernel reclaims under pressure
rather than OOM-killing over, against 140 MB of everything else. Counting it would shed against a
number that is not pressure. That hold predates the tick checkpoint, so it is what
`wal_autocheckpoint=0` does on its own; the log is now copied back once a second. The probe
allocates nothing: the file is opened once and re-read with `pread` at offset zero into a fixed
buffer, compared without converting bytes to strings and parsed without `strconv`.

**The idle sweep is a timer wheel keyed on when each connection falls due** - `lastSeen` plus the
timeout for its state - so a tick walks the one bucket coming due and the sweep costs what it
evicts rather than what the table holds. Walking a slice of the table instead cost a load and a
compare per slot, and at eight rings of 65,471 slots the reclaiming path was half a million of
each inside a single tick, on the one vCPU, exactly when the machine was already saturated.

The wheel is also what makes that path LRU-ordered. The buckets after the one just swept hold the
connections closest to falling due, so reclaiming walks forward from there and takes the most idle
first. Dropping every connection's timeout to a few seconds instead evicted indiscriminately, and
a user in the middle of a think time was as likely to go as one who had vanished. 256 buckets, one
per second, which must exceed the longest timeout so a deadline cannot wrap onto a bucket that has
not been swept yet.

### Memory

**One `mmap` for everything the kernel or SQLite can see**, sized up front with `MAP_POPULATE`, so an
exhausted arena is a startup failure and never a serving one. The connection table is 40 bytes per
connection and holds no pointers, so the garbage collector never walks it. `GOGC=off` with a
`GOMEMLIMIT` backstop, both set by `start.sh` so the box runs what was measured.

**No allocation on the serving path.** Responses are built by appending bytes to an arena buffer:
no `encoding/json`, no `fmt`, no `reflect` on any route. The JSON scanner returns views into the
input rather than decoding into maps. JWT verification reuses one HMAC hasher and fixed scratch
buffers. `parseRequest` fills a struct the loop owns and reuses, and the request is passed to the
router by pointer, so nothing copies 136- and 96-byte structs per request. `http_test.go` asserts
the parser allocates zero times per call.

**Responses are framed without copying the body.** 128 bytes are reserved at the front of the
buffer, the body is rendered after it, and the header is then written *backwards* into the gap, so
the send is one buffer and the body never moves.

The response buffer is sized from the largest response the spec permits - 20 feed posts of 500
characters each, escaped - and not from the inbound `Content-Length` ceiling. Those two have nothing
to do with each other, and sizing the response from the request is a bug that answers 500 to a
perfectly legal feed once bodies get long. Seed bodies average 130 characters, so no benchmark finds
it.

### Database

**Nine statements prepared once** with `SQLITE_PREPARE_PERSISTENT`, reset between requests. That
caches the query plan, not data (rule 5). Every response is read from SQLite as it is served and
every JWT signature is recomputed per request.

**The feed is three range scans instead of one query with twenty subqueries.** The reference query
costs twenty correlated `count(*)` subqueries, each an index seek into `likes`, plus twenty scattered
rowid lookups. The twenty newest posts are almost always the twenty highest ids, so when they are
contiguous the same answer comes from one index-only scan for the ids, one sequential rowid range
over `posts`, and one grouped index range over `likes`. The like counts are grouped in SQL rather
than tallied in Go: ungrouped, twenty posts return about eighty rows and cost about eighty cgo
crossings to produce twenty integers. When the ids are not contiguous and descending within a
bounded window, the reference query answers instead, so this is an optimisation and not a behaviour.

**Text columns cross cgo once, not twice.** A C helper returns a column's pointer and length
together; reading them separately cost two crossings per column, and a rendered post has three.
`reset` does not clear bindings, because every statement either takes no parameters or rebinds all
of them before each use, so clearing them was a second crossing that could never be observed.

**Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), `wal_autocheckpoint=0`,
`temp_store=MEMORY`, 8 MiB page cache. Autocheckpointing is off so no request pays for the copy;
the loop runs a `PASSIVE` checkpoint on its one-second tick instead, which is what bounds the log.

**SQLite compile options**, all from SQLite's own recommended list, in `sqlite_cfg.h` so the library
and the cgo header agree: `THREADSAFE=0`, `OMIT_AUTOINIT`, `DEFAULT_MEMSTATUS=0`,
`DEFAULT_WAL_SYNCHRONOUS=1`, `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`, `OMIT_DECLTYPE`,
`OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`, `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`,
`USE_ALLOCA`.

### The build

**Profile-guided, both halves.** `build.sh` compiles the amalgamation itself rather than letting cgo
do it, because GCC looks a profile up by the object file's path and cgo's object paths are
temporary. It builds SQLite instrumented, runs a training workload that drives the real request mix
through the real handlers, then rebuilds SQLite against that profile and builds Go with the
`default.pgo` the same run produced. `PGO=0` skips it; the compiler's support is probed rather than
inferred from its version string, because Debian's `cc` does not say "gcc".

### Correctness details worth stating

**JWT**: HS256 only, `alg` checked before the signature, `exp` **required** - a token without one is
rejected. `crypto/hmac` with a constant-time comparison.

**The 500-character body limit counts characters, not bytes.** `posts.body` carries
`CHECK (length(body) BETWEEN 1 AND 500)` and SQLite's `length()` counts characters, so a byte count
would turn a long multibyte body into a 500 instead of the 400 the spec asks for.

**Likes check the post exists first.** Foreign keys are off, so `INSERT INTO likes` against a missing
post would otherwise succeed and answer 201 where the spec wants 404.

## Measured

Nothing on the benchmark hardware yet. These come from an arm64 Docker container pinned to one CPU
with 2 GB and no swap, so they are **relative measurements**: not comparable to the x86-64 box, and
not the challenge's µs/req budget.

30 s ramp, 90 s hold, 4,000 VUs across four load generators, against the fastest Go entry from the
last published batch built and run on the same rig:

| | this server | `go-epoll-MegaPanchamZ` |
|---|---:|---:|
| µs/req | 156.7 | 133.3 |
| p95 | 0.6 ms | 0.5 ms |
| p99 | 0.8 ms | 0.9 ms |
| errors | 0 | 0 |
| peak RSS | 412 MB | 256 MB |

µs/req counts the ramp and the start-up as well as the hold, so only runs with the same ramp and
hold compare at all. Run-to-run spread on this rig reaches 16% on an identical configuration, so
the figure above is the median of three runs (156.0, 156.7, 163.9) rather than a single one.

The loop's own counters over those runs report no pressure anywhere: no response buffer
exhaustion, no submission-queue overflow, no provided-buffer starvation, no stalled accept, and no
shedding, which is what should happen at a connection count this far below the thresholds.

Capacity, measured directly rather than extrapolated: **2,499 connections held and still answering
over 960-slot registered tables, on 16 process file descriptors**, across 3 ring handoffs.

A 5-minute hold at 8,000 VUs with the registered tables deliberately cramped - `LimitNOFILE=1024`,
so 960 slots per ring and 7,680 in all - because that is the only way to make the shedding and
handoff paths execute on this rig at all:

| | |
|---|---:|
| connections held | 7,295 of 7,680 |
| connections cycled | 29,868 accepted, 22,573 closed, every one of them closed by the client |
| µs/req | 141.2 |
| p95 / p99 | 0.7 / 1.5 ms |
| errors | 0 |
| peak cgroup memory | 573 MB, of which 328 MB is reclaimable page cache |
| ring handoffs | 194, none of them forced by a full table |

The scoring box runs `LimitNOFILE=65535`, which is 65,471 slots per ring and 523,768 in all, so
none of those paths is expected to run there. They are measured because a path that only executes
at the ceiling is a path that only executes when it is too late to find out it is wrong.

All 42 checks pass, on arm64. The x86-64 build is verified by this repo's CI.

## License

MIT, under the repo's [license](../../LICENSE).
