# Go + SQLite (io_uring)

A single-threaded HTTP/1.1 server on raw `io_uring` syscalls, with SQLite compiled in through cgo.
No framework, no `net/http`, no `net` listener: the event loop, the HTTP parser, the JSON writer and
the JWT verifier are all in this folder.

| | |
|---|---|
| Language | Go 1.27.3 (official tarball) and C (the compiler from `build-essential`) |
| Framework | none |
| I/O | hand-written `io_uring` event loop on raw syscalls (`io_uring_setup`/`enter`/`register`), 8 rings, registered descriptor tables, multishot accept and receive, provided buffer rings |
| SQLite | 3.54.1 amalgamation. `build.sh` downloads it, verifies both its SHA-256 and the SHA3-256 that sqlite.org publishes, then compiles it itself |
| Dependencies | none outside the Go standard library and SQLite |
| **Nginx or direct** | **Direct**: the server binds `HOST:PORT` itself (`0.0.0.0:80` on the box) |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, ca-certificates, curl, unzip, openssl; Go 1.27.3 to /usr/local/go
bash build.sh          # downloads + verifies SQLite, builds it twice (profile-guided), builds bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` pins a Go SHA-256 for amd64 and arm64 both, chosen from `dpkg --print-architecture`.

## Why these choices

**One thread owns everything.** `GOMAXPROCS=1`, `runtime.LockOSThread`, and SQLite built with
`SQLITE_THREADSAFE=0`, which removes every mutex from SQLite and makes it unsafe to enter from a
second thread. The loop thread opens the database and serves every request itself, so there is no
handoff, no channel and no lock anywhere on the serving path.

**Direct, not behind Nginx.** Nginx's `worker_connections` would cap the entry below the connection
count this design aims at, and Nginx takes CPU from the one vCPU.

### The event loop

**Connections are registered descriptors, not file descriptors.** Accept is multishot with
`IORING_FILE_INDEX_ALLOC`, so an accepted socket lands in the ring's own descriptor table and never
occupies a process fd. This is what gets past `RLIMIT_NOFILE`: the kernel checks a table's size
against the soft limit once, at registration, and the table belongs to the ring rather than to the
process, so 8 rings hold 8 x `RLIMIT_NOFILE` connections in one process.

`raiseNOFILE()` lifts the soft limit to the hard one at startup, so capacity depends on the hard
limit rather than on who started the process: a single systemd `LimitNOFILE` sets both alike, but a
stock unit ships `1024:524288`.

**Accept moves between rings before a table fills, not after.** A table-full accept is terminated
with `ENFILE` *after* the kernel has already accepted the socket, so the kernel closes it and that
client sees a reset. The loop arms accept on the next ring and cancels the current one while a
margin of slots is still free; the `ENFILE` path remains as the backstop. The margin has to fit
inside what shedding leaves free, or it can never be satisfied and every accept lands in a table
about to fill. When no table has room nothing is armed at all, so the arrival waits in the
listening backlog instead of being accepted and reset.

**Arming an accept is idempotent.** Several completion paths lead to it, and more than one can run
while an accept is live on another ring - the planned handoff arms the next ring *before*
cancelling this one, so the cancel completes with accept already armed. A second multishot accept
is one no handoff can reach, because the handoff test only fires for the ring accept is recorded
on; it runs until its own table fills, resets a client, and the backstop arms another. That
cascades.

**The cross-ring wakeup is a one-shot poll, re-armed by hand.** A *multishot* poll on another ring's
fd reports `IORING_CQE_F_MORE` and then delivers exactly one event, ever - the same with
`IORING_POLL_ADD_LEVEL` set, so it is not edge-triggering, and the same against a registered
eventfd. That is measured on every variant, including the negative results, rather than inferred
from the documentation, which says a multishot poll rearms itself.

**Receives are multishot into provided buffer rings**, so request bytes are read straight out of the
kernel's buffer and never copied on the fast path. A partial request is the only case that copies,
into a pending buffer held only while the request is incomplete.

**Closing is two-phase.** A connection's response buffer goes back to the pool only once the close
has completed *and* no send is still in flight, because the kernel reads a send buffer after
`io_uring_enter` returns. Releasing it earlier handed live bytes to the next connection.

**A shed connection waits for the client's FIN rather than sending one.** Closing first makes this
side the active closer, and the socket then sits in `FIN_WAIT_2` still holding the response it just
sent, charged to this cgroup until the client acknowledges - and that memory is the unreclaimable
kind. Waiting costs a slot in this server's own table for the millisecond a client takes to close,
and a slot is not a socket buffer. A lingering connection is swept after 10 s, and anything it
sends after being told the connection is ending is dropped rather than answered.

**Shedding, so capacity degrades instead of falling over.** The load model leaves a user idle 3-15 s
between requests, so slots bound concurrent *connections*, never concurrent *users*. Two pressures
drive it, and they pull different levers, because the two resources are freed by different means:

- **Slots**, at 95% and 98% of `rings x slots`, drive **retention**. A slot given up by an idle user
  is a slot a waiting one can have, so closing frees the resource that is short: above the soft
  threshold answers carry `Connection: close` and keep-alive clients cycle out on their own, and
  above the hard one the idle sweep stops being housekeeping and starts reclaiming. The thresholds
  sit close to full on purpose - set low, they do not degrade capacity, they cap it.
- **Memory**, at 75% and 87.5% of the cgroup limit, drives **admission**: it stops arming the
  accept. A shed connection is replaced by the client that reconnects, so shedding holds the
  population level and frees no memory; what it frees is a slot, which was never the scarce
  resource. Stopping admission is the only thing here that bounds how far the population can grow.
  At the critical threshold it sheds as well, to drain rather than merely stop growing, but it
  deliberately does not tighten the idle sweep: evicting users faster only raises the rate at which
  they reconnect, and each reconnection is a new socket.

The shed decision counts the connections it can still act on, not the slots occupied. A lingering
connection holds a slot but has already been told to go, so counting it would make the decision
react to its own output.

Memory is read from `memory.stat` every tick, and only the unreclaimable fields count - `anon`,
`slab_unreclaimable`, `sock`, `kernel_stack`, `pagetables`, `percpu`. Page cache holding the WAL is
the largest thing in the cgroup and the kernel reclaims it under pressure rather than OOM-killing
over it, so counting it would shed against a number that is not pressure. The probe allocates
nothing: the file is opened once and re-read with `pread` at offset zero into a fixed buffer,
compared without converting bytes to strings and parsed without `strconv`.

**The idle sweep is a timer wheel keyed on when each connection falls due** - `lastSeen` plus the
timeout for its state - so a tick walks the one bucket coming due and the sweep costs what it
evicts rather than what the table holds. Walking a slice of the table instead cost a load and a
compare per slot, on the one vCPU, exactly when the machine was already saturated.

The wheel also makes reclaiming LRU-ordered: the buckets after the one just swept hold the
connections closest to falling due, so it walks forward from there and takes the most idle first.
Dropping every connection's timeout at once evicted indiscriminately, and a user mid-think-time was
as likely to go as one who had vanished. 256 buckets, one per second, which must exceed the longest
timeout so a deadline cannot wrap onto a bucket not yet swept.

### Memory

**One `mmap` for everything the kernel or SQLite can see**, sized up front with `MAP_POPULATE`, so an
exhausted arena is a startup failure and never a serving one. The connection table holds no
pointers, so the garbage collector never walks it. `GOGC=off` with a `GOMEMLIMIT` backstop, both set
by `start.sh` so the box runs what was measured.

**No allocation on the serving path.** Responses are built by appending bytes to an arena buffer: no
`encoding/json`, no `fmt`, no `reflect` on any route. The JSON scanner returns views into the input
rather than decoding into maps. JWT verification reuses one HMAC hasher and fixed scratch buffers.
`parseRequest` fills a struct the loop owns and reuses, and the request reaches the router by
pointer so nothing copies it per request.

**Responses are framed without copying the body.** 128 bytes are reserved at the front of the
buffer, the body is rendered after it, and the header is then written *backwards* into the gap, so
the send is one buffer and the body never moves.

The response buffer is sized from the largest response the spec permits - 20 feed posts of 500
characters each, escaped - and not from the inbound `Content-Length` ceiling. Those two have nothing
to do with each other, and sizing the response from the request answers 500 to a perfectly legal
feed once bodies get long.

### Database

**Nine statements prepared once** with `SQLITE_PREPARE_PERSISTENT`, reset between requests. That
caches the query plan, not data (rule 5). Every response is read from SQLite as it is served, and
every JWT signature is recomputed per request.

**The feed is three range scans instead of one query with twenty subqueries.** The reference query
costs twenty correlated `count(*)` subqueries, each an index seek into `likes`, plus twenty
scattered rowid lookups. The twenty newest posts are almost always the twenty highest ids, so when
they are contiguous the same answer comes from one index-only scan for the ids, one sequential rowid
range over `posts`, and one grouped index range over `likes`. The like counts are grouped in SQL
rather than tallied in Go, because ungrouped they would cost a cgo crossing per row to produce
twenty integers. When the ids are not contiguous and descending within a bounded window the
reference query answers instead, so this is an optimisation and not a behaviour.

**Text columns cross cgo once, not twice.** A C helper returns a column's pointer and length
together; reading them separately cost two crossings per column, and a rendered post has three.
`reset` does not clear bindings, because every statement either takes no parameters or rebinds all
of them before each use.

**Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), `wal_autocheckpoint=0`,
`temp_store=MEMORY`, 8 MiB page cache. Autocheckpointing is off so no request pays for the copy; the
loop runs a `PASSIVE` checkpoint on its one-second tick instead, which is what bounds the log.

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
`default.pgo` the same run produced. `PGO=0` skips it, and the compiler's support is probed rather
than inferred from its version string, because Debian's `cc` does not say "gcc".

### Correctness details worth stating

**JWT**: HS256 only, `alg` checked before the signature, `exp` **required** - a token without one is
rejected. `crypto/hmac` with a constant-time comparison.

**The 500-character body limit counts characters, not bytes.** `posts.body` carries
`CHECK (length(body) BETWEEN 1 AND 500)` and SQLite's `length()` counts characters, so a byte count
would turn a long multibyte body into a 500 instead of the 400 the spec asks for.

**Likes check the post exists first.** Foreign keys are off, so `INSERT INTO likes` against a missing
post would otherwise succeed and answer 201 where the spec wants 404.

## License

MIT, under the repo's [license](../../LICENSE).
