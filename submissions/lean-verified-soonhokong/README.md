# Lean 4: the verified handler is the one that runs

Every request is parsed, routed, authenticated, validated, planned and rendered by Lean code
compiled by the Lean compiler. The theorems in `Feed/Proofs/` are about those same functions: the
handler is written once, generic in its storage operations, and the build checks the proofs about
it on a model of the tables and compiles it against SQLite. A small C runtime (SQLite, the storage
operations, byte and crypto leaves, the io_uring transport) is trusted; each C leaf has a Lean
definition that the proofs use and that the build tests it against.

| | |
|---|---|
| Language | Lean 4.37.0 nightly 2026-10-30 (handler and proofs), compiled to C by the Lean compiler; that C and the C runtime built by gcc `-O3 -march=native`, profile-guided |
| Framework | none |
| Database | SQLite 3.54.1 amalgamation in-process: reads through SQLite's B-tree cursors, writes as prepared SQL |
| Transport | io_uring (Linux ≥ 6.1), one thread, HTTP/1.1 keep-alive |
| **Nginx** | **no: serves directly** on `HOST:PORT` (`0.0.0.0:80`) |
| Dependencies | Lean toolchain and SQLite, pinned by version and SHA-256 |

## What runs and what is proved

`Feed/Handler.lean` and `Feed/Server.lean` are generic in a storage monad:

```lean
class Store (m : Type → Type) where
  feedPage : Nat → m Page   -- the first n posts in (created_at DESC, id DESC) order,
                            -- each with its author's name (if the user exists) and like count
  postPage : Nat → m Page   -- the post with this id, with its author and like count (or no row)
  postExists : Nat → m Nat
  insertLike : (uid pid : Nat) → m Nat
  insertPost : (uid : Nat) → (body : ByteArray) → m (ByteArray × Nat)
  …
```

* The server runs `Store IO`: each operation is a C function over SQLite (`runtime/db.c`).
* The proofs run `Store Model`: the same operations on the specification's tables, a state monad
  over lists of rows (`Feed/Model.lean`). Each C function is written to that one-line contract.

A `Page` is one byte array of text and one array of numbers, eight per row; `Page.view` reads a
row back as the post object it renders (none if the author row is missing). A request makes one
storage call: the C side reads the posts, their authors and their like counts together, and the
Lean side decides which rows are rendered, in what order and how.

`Feed/Spec.lean` is SPEC.md as Lean: tables as lists, `ORDER BY created_at DESC, id DESC LIMIT 20`
as a sort, the join and the like count as list functions, the five endpoints, the routes, the
error messages, the JSON and the HTTP response bytes. The theorems (all checked by `lake build`
in `build.sh`; no `sorry`; axioms `propext`, `Classical.choice`, `Quot.sound`):

| Theorem | File | Statement |
|---|---|---|
| `feedBody_correct` | `Proofs/Plan.lean` | `GET /feed`'s plan (a page of the newest posts with their authors and like counts, retrying further down the order while posts lack an author) produces `feedJson db.feed` byte for byte, and reads only |
| `postBody_correct` | `Proofs/Plan.lean` | `GET /posts/:id` produces `postJson v` exactly when `db.getPost id = some v`, else "not found" |
| `feedFromPage_congr`, `postFromPage_congr` | `Proofs/Plan.lean` | the response to a request depends on its page only through `Page.Same` (the rows as post objects), which is what the self-test checks of the C pages |
| `perform_like`, `perform_create` | `Proofs/Write.lean` | a like / a new post answers (status, body) and changes the tables as `DB.like` / `DB.create` |
| `Escape.decode_escape` | `Proofs/Escape.lean` | the JSON string escape is reversible: decoding `escape s` gives `s` |
| `Jwt.verify_ok_sound` | `Proofs/Jwt.lean` | an accepted token's third part is the base64url HMAC-SHA256 of its first two under the key, its header has `"alg":"HS256"`, its `exp` is after now |
| `route_spec` | `Proofs/Route.lean` | routing on byte ranges of the input equals `Spec.route` on the path's bytes (5 endpoints, `:id` a positive integer in one segment) |
| `decide_spec` | `Proofs/Decide.lean` | one request: the specification's response for its route, or the write it asks for; token first, then id or body; never changes the snapshot |
| `runBatch_spec` | `Proofs/Batch.lean` | a batch is a serial execution: every answered read is answered from the batch's snapshot, the writes run in connection order each on the database the earlier ones left, every connection gets its reads' answers then its write's answer, and the database ends as the snapshot with the writes applied |

The theorems hold for every database with fewer than 20·2⁶³ posts: no assumption about ids (the
plans do not rely on rowids being distinct or positive) or about how ids and timestamps relate.

`runBatch_spec` is about the function the transport calls (`Feed.batch` is `runBatch` at `IO`). It
is what makes group commit and shared snapshots safe here: a connection's requests keep their
order, a write in a batch is the first request of its connection in that batch, nothing after a
write is answered in the same batch, and no answer leaves before the commit. It holds whether or
not the batch may write (`writesOk`), which is what lets the transport put writes into fewer
batches than reads (below) without a new proof.

### The trust base

Not proved, and how it is checked instead:

* **The C storage operations** (`runtime/db.c`): each implements the meaning of its `Store Model`
  operation. `build.sh` runs `bin/server --selftest`, which writes a random database (ties in
  `created_at`, gaps in ids, posts whose author row is missing, a `users` table of three B-tree
  levels), runs every operation on random arguments before and after random writes, and compares
  each result with the model's: a page by `Page.Same` (as many rows, each reading back as the same
  post object; `feedFromPage_congr` and `postFromPage_congr` prove the handler answers the same
  from such pages, so the C side may lay the text out in its own order). It also compares the
  grouped rowid search below with SQLite's own on every key of `users`, rolls back a transaction
  that grew the file, and runs SQLite's integrity check. Any difference fails the build.
* **The C leaves** (`runtime/leaves.c`): byte operations (`appendEsc`, `appendNat`, `findByte`, …,
  `Feed/Bytes.lean`), one post object (`postJsonAfter`, `Feed/Handler.lean`), HMAC-SHA256
  (`Hmac.mac`, `Feed/Sha256.lean`) and base64url (`Jwt.b64Encode`, `Jwt.b64Decode`). Each is a
  Lean definition that the proofs use; `@[extern]` makes the binary call the C version. The
  self-test compares every leaf with its Lean definition on 20,000 random inputs, and checks that
  this catches mistakes: a flipped bit of the MAC, a wrong padding length, a wrong base64
  character, a misspelled key, an off-by-one in the rowid search or the like count all fail it.
* **SHA-256** (`Feed/Sha256.lean`): the Lean definition (the reference for the C HMAC) is checked
  against the FIPS 180-4 and RFC 4231 vectors by the self-test; not proved equal to the standard.
* **Parsing**: HTTP framing (`Feed/Http.lean`) and the JSON of bodies and tokens (`Feed/Json.lean`)
  are Lean code. The theorems take their results as given (`decide_spec` is stated for any parsed
  request; `verify_ok_sound` in terms of the parser's fields).
* **The transport** (`runtime/uring.c`): moves bytes between sockets and `Feed.batch`; does not
  parse HTTP. The WAL shim (`runtime/walvfs.c`) only merges the write calls of one commit; the
  page cache wrapper (`runtime/db.c`) only skips a scan that would find nothing.
* SQLite itself, Lean's compiler, runtime and kernel, gcc, Linux.

## Design

**Batches.** Each event-loop iteration hands the new input of every connection to Lean in one call
(`Feed.batch`). The batch is one SQLite transaction: all reads use one snapshot, through B-tree
cursors opened once per batch; the writes follow in connection order as prepared statements;
then one commit. Answers are sent after the commit, so a 201 is always durable (WAL,
`synchronous=NORMAL`). While busy, an iteration starts at most every 2 ms, so a batch collects
everything that arrived meanwhile (one wakeup, one transaction, one Lean call for many requests);
under saturation iterations run back to back.

**Writes in fewer batches.** A batch may write at most every 10 ms (`writesOk` is false in the
batches between); a write waits for that batch, reads do not. A commit then carries the writes of
several periods, and the pages they share (the end of `likes`, the start of the feed index, the
recent part of `likes_post_id_idx`) go to the log once per commit instead of once per period:
0.5 fewer log frames per write, 5 % less CPU for the handler. A write is answered within about
12 ms, well inside the peer's delayed-ACK timer.

**Overload.** When more input arrives than an iteration can serve in about 6 ms, the batch is
capped: the input that arrived since the last iteration goes first and connections left over
from earlier iterations (a backlog, oldest first) fill the remaining room. The cap follows a
running estimate of the time per connection, so the first batch after a burst is capped too.
New requests are then still answered within an iteration or two, before the kernel's
delayed-ACK timer (40 ms) adds a packet to each of them: a server whose responses take longer
sends two packets per request instead of one, which is how the overloaded runs of other entries
below end. Without a backlog, batches stay far below the cap.

**No caching across requests.** Every request reads its rows from SQLite while it is served (the
feed reads the index, the rows, each author and the like counts); every token's signature is
computed.

**Reads.** One storage call per request. The feed reads the first entries of
`posts_created_at_id_idx`, then the rows by one backward scan of `posts` when their ids are
consecutive (each checked against the index; otherwise one seek each), the like counts by one
range scan of `likes_post_id_idx` when the ids are close together, and the authors by one
descent of `users` for all of them together: on each level the child of every key is chosen
before the next level's pages are fetched, each page once, so the cache misses of different keys
overlap; within a page the search starts where a line through the page's first and last keys
puts the key (rowids are dense), and interior pages stay referenced for the rest of the batch's
reads, as an open cursor's pages do. Pages come through SQLite's pager, page cache and WAL; only
the bytecode engine is skipped.

**Writes.** `INSERT OR IGNORE` with `created_at` bound in the schema default's format: for these
values the only constraint that can fail is the like's primary key, which it ignores as
`ON CONFLICT DO NOTHING` would, so no statement can abort halfway and SQLite keeps no statement
journal; a new post's id comes from `sqlite3_last_insert_rowid` instead of `RETURNING`.
Checkpoints run on a second thread with its own connection every 500 frames (passive, so the
request thread never waits on an fsync). SQLite restarts the WAL only when a writer finds it fully
checkpointed, which never happens under constant writes; past 8,192 frames, writes pause (reads
go on) until a checkpoint has copied what is left, at most 300 ms (a few milliseconds on a fast disk;
more where writes per second are limited, as on a cloud volume capped at 3,000 writes per second). A VFS shim
(`runtime/walvfs.c`) issues each commit's WAL frames as one write instead of two per page.

**Page cache.** SQLite's own (`SQLITE_CONFIG_PCACHE2`), wrapped: at commit SQLite drops cached
pages past the end of the file, and the default cache scans its whole hash table for them once
any key at or past that point was seen. `balance_nonroot` parks a page under the lock-byte
page's number for a moment when it reorders new pages, so after every page split the next commit
scanned all 16,000 pages. The wrapper tracks the largest key a page of the cache holds and passes
the truncation on only when it can find something (2.5 % of the handler's CPU).

**Compiling.** gcc builds both the C that Lean emits and the C runtime with `-O3 -march=native`,
profile-guided: `build.sh` builds an instrumented binary, runs it on a made-up database and
workload (`runtime/train.c`: its own users and posts, no seed data; the user model of
`bench/load.js` and some rejected requests), then builds the final binary with the profile
(9 % less CPU for the handler than Lean's clang at `-O3`). If any step fails it builds without the
profile.

**Transport.** Several io_uring rings, each with a `SO_REUSEPORT` listener and a table of direct
socket descriptors (a ring's table is limited by `RLIMIT_NOFILE`, so the rings together hold more
than 65,535 connections); multishot accept; `DEFER_TASKRUN`. An idle connection waits on a
multishot poll, which holds no buffer; a connection the poll reports readable joins a read queue,
and after the iteration's completions are in, the loop issues one read per queued connection
(never more on a ring than its 512 provided buffers) and the reads complete during the submit.
A burst of any size waits in the queue, and the kernel keeps less memory per idle connection than
for an armed multishot receive. Kernels newer than 6.8 charge ring memory to `RLIMIT_MEMLOCK`
(8 MB under systemd); there a ring that does not fit is made smaller, or the server runs on fewer
rings. Input a batch did not consume waits in a heap buffer only until
the next batch; kept for the connection's lifetime (one for every connection that had ever
waited, which includes every writer outside a write window), these buffers grew the heap by
117 MB during a 2-minute hold at 360,000 users.

**Memory.** On a 2 GB machine like the box the limit is memory, not CPU (see the measurements
below): the kernel keeps about 4 KB per open connection (TCP socket, inode, file, poll), most of it
outside the server's cgroup, and near 350,000 connections the page cache that is left is too small
for the database's working set, so requests start waiting on disk reads. So the server keeps little
of its own: SQLite's cache is 8 MB (a miss is a `pread` from the page cache; at 340,000 users 8 MB
used no more CPU than 16), 512 receive buffers of 1 KB per ring, completion queues larger than a
ring's connections (an overflow would allocate per completion when memory is shortest), and the
first 8 MB of the code locked in memory as it first runs (reclaim would otherwise take the program's
own pages back and the loop would stall reading them from disk). Checkpoints run at nice 19, so on
one vCPU they take the CPU only while the loop sleeps between batches. Ubuntu's 6.8 kernels from
6.8.0-136 on (and the cloud-image kernel 6.8.0-1067) reject the registration of provided-buffer rings with a zero
reserved field (Launchpad #2162843); the server retries with a nonzero one, as c-iouring-pavlyhalim
#245 found, which those kernels accept and a correct kernel never sees. Its notes on the memory wall
led to most of this paragraph.

## Measurements

### On a cloud VM set up like the box

The leaderboard's method on a machine made as close to the box as a cloud VM allows. Server: a
16-vCPU VM booted with `nr_cpus=1 mem=2048M` (one 2.5 GHz Xeon Platinum vCPU with AVX-512,
1,903 MB, no swap), Ubuntu 24.04 with its 6.8 kernel (6.8.0-1067), a network volume capped at
3,000 writes per second, four private addresses. `install.sh` ran as root (63 s) and `build.sh`
as `deploy` (172 s, 669 MB peak); systemd runs the server as `deploy` with `LimitNOFILE=65535` and
`CAP_NET_BIND_SERVICE`; the 42 checks pass. Load: five 16-vCPU VMs in the same subnet
running k6 1.3.0 with `bench/load.js`, four k6 processes each (one per server address). Every
probe starts the server on a fresh copy of the seed; pass is
k6's thresholds in every process. c-iouring-pavlyhalim #245, second on the leaderboard at 295,000,
ran the same probes on the same VMs.

| | this | c-iouring-pavlyhalim #245 |
|---|---|---|
| 2-minute probe, 340,000 users | pass: p99 14 ms | pass: p99 19 ms |
| 2-minute probe, 350,000 users | fail: p99 2.3 s (with a 16 MB SQLite cache: pass, p99 71 ms) | pass: p99 373 ms |
| 2-minute probe, 360,000 users | fail | fail |
| 5-minute hold, 350,000 users | **pass**: p99 77 ms, no errors | fail: out of memory, 90 % of requests failed |
| 5-minute hold, 340,000 users | **pass**: p99 13 ms, no errors | |
| 5-minute hold, 333,000 users | | pass: p99 62 ms |

The leaderboard's search on this VM gives this version 340,000 or 350,000 and #245 333,000.
What stops both is memory: the kernel keeps about 4 KB per connection, and at 360,000 the server
is killed for memory or stalls on disk reads. On this VM the two use the same CPU (at
340,000, 53 % and 54 % of the vCPU in their cgroups, 60 % and 59 % in all). Its vCPU is faster
than the box's: #245 uses 15–16 µs per request here and 26 µs on the box, where its notes put its
limit at memory as well.

### Local harness

The server runs in a container pinned to one core of a Xeon Platinum (Sapphire Rapids) with a 1.75 GB memory
limit, its packets handled on the same core. A C load generator follows `bench/load.js` (feed,
think 3–7 s, post, think 3–8 s, 15 % like, 2 % new post, idle 5–15 s) from 32 source addresses:
60 s ramp, 120 s hold, 30 s ramp-down. Pass means p95 < 500 ms, p99 < 1 s over the whole run and
under 1 % errors, as on the box. Four other entries ran in the same harness: the three in the first
table below, which held 280,000 on the box (#224 is the earlier code of mojo-iouring-oh-meh), and
mojo-iouring-oh-meh #257, first on the leaderboard at 304,000, in the second.

**Box speed** is a CFS quota of 57.5 % of the core (1.15 ms every 2 ms): objc-uring-delneg uses
15.3 µs of CPU per request here at full core and 27 µs on the box. This kernel (100 Hz, no IRQ
time accounting) charges a quota only at its ticks, so from that quota a process that never sleeps
got about 40 % of the core and one that sleeps often got 57.5 %: a server that fell behind lost
30 % of its CPU and stayed behind. A small process in the server's cgroup that wakes every
millisecond makes the scheduler charge time as it is used (a busy loop then gets 57.6 %). Network
work done while the server sleeps falls outside the quota, so the table also gives the core's busy
time divided by 0.575 ("box load"), an estimate of the box's CPU load.

**The load generator** waits for the previous run's sockets to leave TIME_WAIT (60 s) before it
starts. With them still there, `connect()` scanned past them, and once a source address had no
even-numbered port left it scanned all of them on every call (about 3 ms), which stopped the
generator for seconds from about 190,000 connections and looked like a stall of the server. It
also grows its descriptor table before the ramp (growing it later blocked `socket()` for up to
200 ms).

| | this | objc-uring-delneg #154 | mojo-iouring-oh-meh #224 | lean-derive-hargup #153 |
|---|---|---|---|---|
| full core, 280,000 users: µs/req, cgroup / core | **13.2 / 13.7** | 15.3 / 16.9 | 15.3 / 16.3 | 17.5 / 18.4 |
| full core, 280,000 users: memory peak | 1,213 MB | 1,230 MB | 1,246 MB | 1,322 MB |
| box speed, 280,000 users: p99, box load | pass: 11 ms, 71 % | pass: 412 ms, 86 % | pass: 37 ms, 90 % | pass: 4.8 ms, 97 % |
| box speed, 300,000 users | pass: 11 ms, 81 % | fail: 3.0 s, 95 % | pass: 51 ms, 86 % | pass: 8.6 ms, 96 % |
| box speed, 320,000 users | pass: 11 ms, 83 % | pass: 971 ms, 96 % | fail: 16 s, 105 % | pass: 16 ms, 112 % |
| box speed, 340,000 users | pass: 12 ms, 91 % | | | |
| box speed, 360,000 users | pass: 32 ms, 108 %, 1,613 MB | | | |
| box speed, 380,000 users | pass: 167 ms, at the 1,750 MB limit | | | |

On the box these three hold 280,000 and fail above it. Here they pass up to 300,000 or 320,000,
and objc-uring-delneg failed at 300,000 and then passed at 320,000: the end of the ramp, when the
new connections and the full request rate arrive together, decides such runs, and this harness is
easier than the box. At full core this version uses 14 % less CPU per request than
objc-uring-delneg and mojo-iouring-oh-meh in its cgroup (16–19 % less of the core) and 25 % less
than lean-derive-hargup.

In a later session (1 November 2026) this version and mojo-iouring-oh-meh #257 (`a9df151`) ran one
after the other. Per request, user space is the server's own code (here the handler, SQLite and the
transport) and kernel is system calls plus packet processing, from the core's tick counts.

| | this | mojo-iouring-oh-meh #257 |
|---|---|---|
| full core, 280,000 users: µs/req, cgroup / core | **12.5 / 13.7** | 15.3 / 16.2 |
| full core, 280,000 users: user space / kernel, µs/req | 4.0 / 9.4 | 3.9 / 11.6 |
| box speed, 280,000 users: p99 | | pass: 12 ms |
| box speed, 300,000 users | | pass: 130 ms |
| box speed, 320,000 users | | pass: 185 ms |
| box speed, 340,000 users | **pass**: 15 ms | fail: 1.1 s (a second try: 22 s) |
| box speed, 360,000 users | **pass**: 28 ms, 1,598 MB | |
| box speed, 380,000 users | fail: 6.9 s, at the 1,750 MB limit | |

The two spend the same time in user space; the difference is in the kernel. At 380,000 users this
version passed in the first session and failed in the second, both at the container's memory limit.

Against the previous version of this entry most of the CPU saving is user space: the handler went
from 4.9 to 3.5 µs per request in the in-process benchmark (one storage call per request, the
grouped author lookup, the HMAC and base64 leaves, the post-object leaf, inserts without a
statement journal, the page cache wrapper, the write window and the profile-guided build); the
rest is kernel time (fewer log writes and commits, reads issued in batches). The write window
costs a write up to 10 ms of latency.

## Build and run

* `install.sh` (root, once): build-essential, curl, unzip, zstd; Lean `nightly-2026-10-30` from the
  official nightly release, checked against its SHA-256.
* `build.sh` (user): downloads the pinned SQLite amalgamation (SHA-256 checked); `lake build`, which
  checks every proof and compiles `Feed/` to C; gcc builds that C with the C runtime, an
  instrumented binary first, its training run, then the profile-guided `bin/server`; runs the
  self-test.
* `start.sh`: `bin/server` in the foreground, configured by `SQLITE_PATH`, `JWT_SECRET`, `HOST`,
  `PORT`.
