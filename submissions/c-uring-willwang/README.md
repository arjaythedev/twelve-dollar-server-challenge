# C / io_uring with a live rank check

A C11 HTTP/1.1 server for Ubuntu 24.04 x86-64. It serves the challenge directly on `0.0.0.0:80` using Linux TCP, io_uring and epoll. One thread handles sockets, HTTP and SQLite transactions. A lower-priority thread runs checkpoints on a second SQLite connection. The core idea is to read neighboring posts from SQLite and prove that they form the chronological feed with a single index boundary check. [Database reader](reader.c) · [Network loop](reactor.c)

**The organizer scored revision `0142ba2` at 280,000 simultaneous users.** Its five-minute hold completed 26,747 requests/s with 33.0 ms p95, 146.6 ms p99 and zero errors. At 290,000 and 300,000 users p99 rose to 2.96 s and 3.58 s. Kensac’s published lead is 310,000 users. [Official C result](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/403#issuecomment-6816178650) · [Kensac’s scored revision](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264#issuecomment-6566431071)

An earlier loopback setup held 342,500 users twice with that same C revision. On the revised fixture the archived PR executable also passed at 345,000, **65,000 above its official score**. The fixture separates client TCP work from the server, uses the stock generator and runs every peer under identical conditions. The original PR’s result shows how far the absolute local capacity can diverge from the organizer’s. The submission uses ordinary Linux TCP sockets with the deployment user’s privileges. [Original and revised executable measurements](tests/official/capacity/original-pr-high-loads.json) · [Local fixture and reproduction instructions](tests/official/capacity/README.md) · [Earlier local measurements](tests/official/results.json)

Eight separate local runs repeated the organizer’s published loads and durations. Four matched the official pass or miss. Rkkautsar and Swift missed locally at their official 300,000-user passing load, while the original C PR passed its locally repeated 290,000- and 300,000-user probes. [Verified calibration and forward/reverse comparisons](tests/official/capacity/organizer-capacity-check.json) · [Published leaderboard](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/4bcd75129c0d85f69730138903f49aa94b4266be/LEADERBOARD.md#results)

## Local capacity

**The selected public-PCACHE2 build, ELF `b4ed5678`, qualified in all three 350,000-user runs.** Its two-minute probe reached 425/897 ms p95/p99. Its two five-minute holds reached 241/952 and 175/699 ms. Every shard made its assigned dials and maintained full occupancy on all checks, with zero HTTP errors. All five published leaders missed at 350,000. The strongest peer passed two full holds at 325,000, 25,000 below the selected build’s repeated full-hold result. [Selected build scores](tests/official/capacity/public-pcache-capacity.json) · [Five-competitor comparison](tests/official/capacity/large-table-high-loads.json)

The preceding stock-SQLite build, ELF `1c7ad9b5`, qualified in both 350,000-user five-minute holds at 363/836 and 181/991 ms. Its two 350,000-user probes missed, including the same-block probe at 556/1,062 ms. The selected build’s two-hold median fell from 272/914 to 208/826 ms, with comparable throughput. The application installs the adapter before opening SQLite. It wraps SQLite’s public PCACHE2 provider, keeping one conservative key bound and one exceptional rekey destination per cache to skip empty truncations. The SQLite source, cursor reader, reactor and renderer match the preceding build. [Adapter](pcache.c) · [Paired scores](tests/official/capacity/public-pcache-capacity.json) · [Component validation](tests/official/capacity/public-pcache-results.json)

The selected build also qualified at 345,000 users at 126/499 ms. Its 370,000-user hold reached 451/1,283 ms and missed the p99 limit. Both held all connections on all 59 checks and recorded zero HTTP errors. The normal checkout build reproduced the selected ELF and passed all 42 organizer checks and 13 differential layouts. [Selected build scores](tests/official/capacity/public-pcache-capacity.json) · [Native deployment validation](tests/official/capacity/public-pcache-deploy-validation.json)

The single selected-ELF 400,000-user diagnostic missed at 1,322/2,174 ms, with zero HTTP errors and full occupancy on 58 of 59 checks. It completed about 39,000 requests/s over the final 290 seconds at 99.94% utilization of the shared application and receive core. The highest repeated qualifying load was 350,000. [Selected 400,000-user diagnostic](tests/official/capacity/public-pcache-capacity.json)

The original PR missed its matched 350,000-user hold at 1,105 ms p99, with zero errors and full occupancy on all 59 checks. The PR and the preceding `1c7ad9b5` build served about 36,900 requests/s during the same full-occupancy interval, but the original recorded at least 174,433 requests over one second, compared with at most 678 for `1c7ad9b5`’s first hold. [Original PR control and matched incumbent results](tests/official/capacity/original-pr-high-loads.json)

At 370,000 and 400,000 users the preceding `1c7ad9b5` build held every connection and recorded zero errors, but missed the latency limits. It sustained 38,666 and 38,925 requests/s in the respective measured hold intervals, with the shared application and Linux receive core at 99.73% and 99.97% utilization. At 400,000, p95/p99 reached 1,309/2,262 ms. [Incumbent high-load scores and resource accounting](tests/official/capacity/stock-sqlite-results.json)

The preceding `1c7ad9b5` build passed both five-minute holds at 345,000 users, at 227/765 and 153/535 ms p95/p99. Each recorded zero errors and every generator maintained all 86,250 connections on all 59 checks. The original PR passed a matched hold at 239/924 ms. [Incumbent results](tests/official/capacity/stock-sqlite-results.json) · [Earlier PR at the same load](tests/official/capacity/original-pr-high-loads.json)

The selected build uses the pristine SQLite 3.54.1 amalgamation and the B-tree reader from officially merged PR #403. Compared with that PR, the submission changes the Linux TCP reactor and JSON renderer and installs the public cache adapter. On the earlier 16,384-bucket fixture, `1c7ad9b5` passed **two five-minute holds at 312,500**, at 198/924 and 229/879 ms. It passed one of two at 315,000. All four holds sustained every connection on all 59 checks. Both 312,500-user short probes missed. The strongest of the five peers on that fixture, Pavlyhalim, passed one of two five-minute holds at 292,500. [Stock scores, server hashes and fixture details](tests/official/capacity/local-results.json)

The larger fixture uses the same 524,288-bucket TCP lookup table for each entry. The five leaders below all missed at 330,000 and 350,000. Their highest repeated passing count was Pavlyhalim’s 325,000. New measurements of the production C executable use the same server core, four stock generators, disk, seed and ramp as these trials. [Peer scores](tests/official/capacity/large-table-high-loads.json) · [Production trial schedule and exact executable](tests/official/capacity/stock-sqlite-run-plan.json)

| Published submission | Official users | Local full holds at 325,000 | Local p99 at 330,000 | Local p99 at 350,000 |
| --- | ---: | --- | ---: | ---: |
| Kensac, Rust | 310,000 | 0/1 passed | 13.30 s | 23.22 s |
| Oh-meh, Mojo | 304,000 | 1/2 passed | 1.84 s, 57/59 full | 22.53 s, 40/59 full |
| Rkkautsar, Rust | 300,000 | 0/1 passed | 7.93 s | 11.34 s |
| Akbashev, Swift | 300,000 | 0/1 passed | 15.91 s | 28.90 s |
| Pavlyhalim, C | 295,000 | 2/2 passed | 9.67 s | 13.98 s |

Two experimental builds used a source patch to SQLite’s page cache. One also assigned SQLite’s private cursor position directly. The organizer has ruled against both techniques. Those experiments supply historical measurements only. The submitted build uses SQLite’s own cursor movement functions and compiles its checksummed upstream source directly. [Organizer’s SQLite source ruling](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/63a9b9ea449e686260bd55e6639a1545db18a407/LEADERBOARD.md#L1019-L1028) · [Organizer’s cursor ruling](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/63a9b9ea449e686260bd55e6639a1545db18a407/LEADERBOARD.md#L844-L851) · [Historical experimental results](tests/official/capacity/README.md)

## Why C

The feed reader needs SQLite’s internal B-tree cursors, page references and cell decoders. Building the reader and the pinned C amalgamation in the same translation unit lets Clang inline these operations. The reader can hold a page for the duration of a request and serialize its text directly into the response. Rust could expose those operations through a C shim, as Kensac does. The head-to-head compares two complete server designs. The same-C test isolates the algorithm and measured 5.9% shorter feed time with writes interleaved. [Reader](reader.c) · [Kensac’s shim](../rust-iouring-kensac/csrc/shim.c) · [Controlled comparison](#read-path-measurements)

In the final matched 70 seconds of the grouped-registration build’s 350,000-user probe, each completed request consumed 2.69 µs of application userspace, 11.87 µs in the kernel on behalf of the application and 14.64 µs on the TCP receive thread. All SQLite, HTTP, JSON and authentication work in userspace fits into the first figure. Changing when the server enters the kernel can affect both queueing and CPU use. Hand-written assembly in the server directly targets the smaller userspace component. The JSON scanner already uses AVX2 and SSE2 intrinsics, and SHA-256 uses OpenSSL’s optimized implementation. [350,000-user CPU measurements](tests/official/capacity/350k-registration-and-steady-service.json) · [JSON scanner](buffer.c)

Ninhnv’s assembly entry implements the reactor, HTTP, JSON, SHA-256 and record decoding in NASM, with a C shim for SQLite’s internal interfaces. That entry held 280,000 users on the official machine. The Rust leader held 310,000. Those scores compare complete implementations with different scheduling and storage decisions. [Assembly implementation](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/9a9bee532b087902b5ad67d43adee7a613bbcda2/submissions/asm-btree-ninhnv) · [Published results](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/9a9bee532b087902b5ad67d43adee7a613bbcda2/LEADERBOARD.md#results)

## Build and run

From the challenge repository root, build as the normal deploy user. `install.sh` installs the Ubuntu packages and requires root. `build.sh` downloads the pinned SQLite source and verifies both the archive and the extracted source files. For a local run, create a private copy of the seed so the server can commit writes to it.

```sh
submission=submissions/c-uring-willwang
sudo bash "$submission/install.sh"
bash "$submission/build.sh"
test -f seed/feed.db || bash seed/make-seed.sh
run_dir=$(mktemp -d)
cp seed/feed.db "$run_dir/feed.db"
SQLITE_PATH="$run_dir/feed.db" JWT_SECRET=twelve-dollar-challenge \
  HOST=127.0.0.1 PORT=3000 "$submission/start.sh"
```

`start.sh` runs the server in the foreground. A second terminal can check `curl -fsS http://127.0.0.1:3000/health`. On the challenge droplet the runner supplies `HOST=0.0.0.0`, `PORT=80`, `SQLITE_PATH` and `JWT_SECRET`. [Runtime contract](../../SPEC.md#runtime-contract)

## Optimizations in the submitted server

### Feed and point reads

The posts table is already ordered by rowid. Its last 20 rows can span just a handful of SQLite pages. The chronological index tells us cheaply whether those rows really are the feed.

1. Walk backwards from the end of the posts B-tree. Read each candidate’s ID, body, timestamp and author ID. Track whether the timestamps are nonincreasing. A backwards rowid walk already breaks timestamp ties in the required order.
2. Read the ID at rank 20 of the live `(created_at DESC, id DESC)` index. If the first index leaf contains 20 cells, SQLite can parse cell 19 directly. A short leaf uses normal cursor steps across the boundary.
3. Compare that ID with the worst candidate under `(created_at DESC, id DESC)`. When timestamps are already ordered, this is simply the last row scanned. Equality proves membership. Nineteen distinct candidates precede the global 20th row, and there are exactly nineteen places above it. This works when older rows have arbitrary timestamps and IDs contain gaps. A locally reordered set needs only an in-request sort before rendering.
4. Read the authors and like counts from their live trees. The greatest key in the likes index proves that every larger post ID has zero likes. A single ordered walk counts nearby IDs. On a heavily liked post, two equal keys on the same index leaf prove that all intervening keys are equal, so their cell positions give the count. The reader calls SQLite’s own cursor functions to advance across post and likes leaves. Interior likes-index rows are still counted individually. Large gaps use an indexed seek.
5. If any part of the proof fails, use the general chronological plan. Missing authors require the SQL join to refill the feed. Oversized records can use SQLite’s normal overflow-page handling. When the table has fewer than 20 posts, the scan already contains every candidate.

All candidate rows, page references and borrowed text belong to the executing request. A post or like in the same transaction is visible to subsequent requests. Only schema metadata, prepared plans and SQLite’s own page cache are retained. For an author join, the reader uses the first and last separator on a visited table page to estimate the position of the next rowid. It checks the real separators, narrows the search by up to four cells and uses binary search for the rest. On the leaf it verifies the actual rowid. Each feed can use its own parent-page estimates for twenty authors. Both endpoints serialize borrowed row data directly into the HTTP buffer.

`GET /posts/:id` seeks the requested rowid, looks up its author and counts its likes through the same live indexes. Both read paths check the expected column layout, index order, collation and text encoding before enabling direct B-tree access. Prepared SQL implements the general case. For a feed with interleaved writes, checking the single rank boundary took 2.248 µs per feed versus 2.390 µs when checking all 20 keys in the same C build. [Read implementation](reader.c) · [Paired read measurements](#read-path-measurements)

Kensac also bypasses the SQL virtual machine. It first reads 20 chronological keys and then retrieves the corresponding posts. Scooter verifies successive rowids against individual index entries. Mojo and Hargup’s Lean entry walk the rowid tail after checking a table-wide ordering invariant. Pavlyhalim grouped author lookups and like-count range scans. Those entries motivated comparing the cost of discovering the feed with the cost of proving a candidate feed. [Kensac](../rust-iouring-kensac) · [Scooter](../rust-native-scooter1337) · [Mojo](../mojo-iouring-oh-meh) · [Lean](../lean-derive-hargup) · [Pavlyhalim](../c-iouring-pavlyhalim)

### HTTP, JSON and authentication

- **Parse in the receive buffer.** The HTTP parser represents the method, path, headers and body as slices. A complete request can go directly from a shared receive buffer to dispatch. Fragmented requests, pipelined leftovers and requests waiting for write admission are copied only when their bytes need to outlive that buffer. The JSON parser decodes the fields used by that route and validates the rest of the document. [HTTP parser](app.c) · [JSON parser](json_input.h)
- **Check JSON while copying it.** After reserving space for one row, the renderer copies the body, timestamp and author into the outgoing buffer and tests those same loaded bytes for characters that need escaping. AVX2 handles 32-byte chunks on supported CPUs, and shorter strings use SSE2 or scalar code. When any field needs an escape, the complete JSON encoder rewrites the row. Integers use base-10,000 groups and a two-digit lookup table. The server fills in `Content-Length` after rendering. Across 92,210 same-snapshot feed pairs this reduced median feed read-and-render time by 15.8%. Every pair returned identical bytes. [Response headers](app.c) · [JSON renderer](buffer.c) · [Paired measurements](tests/official/capacity/renderer.json)
- **Reuse the HMAC key schedule.** OpenSSL 3.0.13 prepares the inner and outer SHA-256 states for the configured secret once. Each authenticated request copies those states, hashes its own signed bytes, compares the MAC in constant time, and validates the algorithm, expiry and identity claims. [JWT verification](app.c)

### TCP, io_uring and epoll

The process listens directly on `HOST:PORT`. It adds sparse io_uring fixed-file tables as connections arrive, so the socket count can exceed the process descriptor limit. Each table owns an edge-triggered epoll instance. The server accepts ordinary sockets in groups of up to 64 and can have up to 512 temporary descriptors outstanding. The fixed slots keep sockets and watches alive. An idle connection needs no outstanding receive, input buffer or response allocation.

For a never-used range in a table, the reactor assigns the slot numbers itself. In a single submission it asks Linux to import the sockets with `IORING_OP_FILES_UPDATE` and register their ordinary descriptors with `IORING_OP_EPOLL_CTL`. A readiness event arriving before both operations complete records its generation in the assigned slot. The batch retains its descriptors and event records until every completion arrives. It then activates the imported prefix, services previously observed readiness, closes its temporary descriptors in consecutive ranges and retries any unpublished suffix.

Once it exhausts the never-used range, the reactor waits for outstanding explicit imports before allowing the kernel to allocate slots from the table. In that mode it groups four or more successful watch registrations and handles smaller groups with `epoll_ctl`. Unsupported ring operations fall back to synchronous registration, direct accepts or multishot receives according to kernel support. Environments without io_uring use ordinary epoll within the process descriptor limit. [Admission code](reactor.c) · [Kernel registration and reuse checks](tests/batch-watch.c) · [Completion ordering model](tests/official/capacity/explicit-slot/model.c)

The loop rotates across the tables. It takes up to 64 ready sockets from one kernel queue, reads them, executes the real SQLite work, and submits the replies to that same ring before moving on. This shortens the time between touching a socket for its request and touching it again for its response. The reactor borrows up to 256 buffers of 768 bytes from one shared pool for nonblocking reads. It returns each buffer on completion, even if its connection closed and the slot was reused. Longer requests continue across receives. The kernel-selected buffer group handles multishot reads. Lean #286 demonstrated that a temporary ordinary descriptor can transfer a socket reference to a long-lived io_uring operation. Here it registers the persistent epoll watch. [Lean #286 transport](../lean-verified-soonhokong/runtime/uring.c)

Linux queues unread readiness in arrival order within each table. A separate userspace queue handles a full buffer, a hangup, or another readiness edge arriving while a read is in flight. Its oldest entry always makes progress under backlog. Partial sends and pipelined requests preserve order on their connection.

After two consecutive busy passes the loop suppresses secondary rings’ eventfd notifications. Before sleeping, it restores those notifications, rechecks every ring and arms a one-shot poll on a parent epoll instance that watches the individual readiness queues. A readiness event arriving between the last direct scan and the transition to sleep still wakes the reactor. A separate C entry also suppresses eventfd signals while polling. [Related implementation](../c-rawuring-thehimalayanleo/src/server.c) · [Linux wakeup regression](tests/wake.c)

Eight `SO_REUSEPORT` listeners accept connections. They request `TCP_NODELAY` for short interactive exchanges and Reno congestion control when Linux makes it available. Each ring has a single issuer. The primary ring defers kernel task work until the issuer enters it. While walking ready events, the loop prefetches connection state six entries ahead. It handles up to 512 replies per pass and recycles up to 2,048 output buffers. Recycled buffers larger than 32 KiB release their storage, so one large response does not set the memory cost of later small responses. [Socket setup and buffer ownership](reactor.c) · [Measured TCP options](tests/official/kernel-options.json)

Each listener sets `TCP_QUICKACK=0` **after** `listen()`. On Linux 6.8 a child socket then inherits interactive delayed-ACK behavior during the handshake, including when its first request arrives before the server accepts it. Linux can carry the acknowledgement on the first HTTP response. If the parser needs more bytes to finish a request, the server asks the kernel to acknowledge the fragment immediately. A peer that has its next small write queued behind Nagle’s algorithm can then continue. Linux keeps responsibility for delayed-ACK timers, congestion control and retransmission. [Socket setup and fragment handling](reactor.c) · [Packet, CPU and fragment measurements](tests/official/capacity/README.md#the-first-http-exchange) · [Fragment and pipeline regression](tests/held-writes.c)

### Transactions, WAL and checkpoints

Requests parsed in one pass can share a SQLite transaction. Each read runs independently. The reactor can release reads from an unchanged snapshot at the end of each table’s turn. Once a transaction contains a write, all dependent replies wait for its commit. A rollback or commit failure converts those replies into errors. The first write after an idle period starts immediately. The group-commit window begins at 5 ms, doubles up to 32 ms after a commit with at least eight row changes and returns to 5 ms after a sparse or idle interval. The oldest held writes receive up to half the next open pass, while other sockets keep reading. New posts use the database’s timestamp default. Likes use the existing uniqueness constraint and distinguish a duplicate from a missing post.

Before SQLite first takes a lock, the process opens its own database file, asks Linux to drop its clean file-cache pages with `POSIX_FADV_DONTNEED`, and closes that descriptor. SQLite can then populate the working set on demand. The server uses WAL with `synchronous=NORMAL`. A wrapper over SQLite’s public VFS combines adjacent noncommit WAL frames while SQLite is executing the writer’s COMMIT statement. The commit-marker header flushes the buffered bytes to the underlying VFS, and its payload also reaches the underlying write inside SQLite’s own call. SQLite then publishes the new WAL index. Outside COMMIT, the wrapper can pair one noncommit header with its page. Reads, syncs, truncations and discontinuities flush pending bytes. SQLite’s standalone checksum repairs follow the original write path. The wrapper propagates short-write failures and keeps the checkpoint connection outside the writer’s grouping scope. Pavlyhalim’s WAL measurements motivated trying a narrower grouping boundary. [Pavlyhalim’s VFS](../c-iouring-pavlyhalim/src/db.h)

An independent connection at nice 5 runs `PASSIVE` checkpoints after 16,384 frames. If more frames arrive during a checkpoint, writes can be parked briefly while the worker catches up. Reads on other connections continue. A write can wait for this gap for up to 25 ms, or 100 ms if the WAL has reached 32,768 frames. A pipelined read behind a held write on the same socket keeps its HTTP ordering.

Transaction control, health checks and mutations use prepared statements that are reset and rebound for each operation. Creating a post returns its ID and SQLite-generated timestamp in the insert itself. Inserting a like checks that the post exists and uses the existing unique constraint. A successful insert needs one statement. Only an unchanged insert needs an additional lookup to distinguish a duplicate like from a missing post. [SQLite statements and commit policy](database.c) · [WAL write grouping](wal.c)

Other leading entries illustrate the scheduling choices. Rkkautsar separates the writer and submits completed feeds before point reads. Akbashev bounds the receive work after a buffer shortage. Delneg’s Objective-C and F# servers bound responses per pass, and the F# server drains held writes before fresh work when its write window opens. [Rust](../rust-iouring-rkkautsar) · [Swift](../swift-iouring-akbashev) · [Objective-C](../objc-uring-delneg) · [F#](../fsharp-uring-delneg)

### Compiler and SQLite configuration

Clang 18 builds the server and the pinned SQLite 3.54.1 amalgamation with `-O3`, `-march=native`, `-fno-plt` and whole-program link-time optimization. The release is built on its target CPU. The SQLite build disables allocation statistics, shared-cache mode and loadable extensions. Each thread owns a `SQLITE_OPEN_NOMUTEX` connection. The primary connection gives SQLite a 64 MiB page-cache target, disables database `mmap` and keeps temporary tables in memory. The checkpoint connection uses a 16-page cache. User data and the WAL remain on disk. [Compiler settings](Makefile) · [Pinned source](build.sh) · [SQLite configuration](database.c)

## Correctness checks

The test runner makes a private copy of the seed. Run the checks from the challenge repository root.

```sh
bash seed/make-seed.sh
bash test/run.sh submissions/c-uring-willwang
python3 submissions/c-uring-willwang/tests/differential.py
```

The checkout build reproduced executable SHA-256 `1c7ad9b584a8c0116f74e10d579ad148c09c8886edbfa481d3c9f8469e3027cc` and all seven object files from an independent clean build. The compiler’s recorded dependency is the vendor’s checksummed `sqlite3.c`. The challenge build and start scripts passed all 42 API checks and 13 SQL differential layouts, including reopened writes. The observed service ran as UID 1000 with zero effective capabilities and a live io_uring file descriptor. A separate explicit-slot comparison checked 213,028 HTTP responses. Its Cascade Lake build passed the same 42 API checks and 13 layouts on the local AMD processor. [Checkout validation](tests/official/capacity/stock-sqlite-validation.json) · [Component comparison](tests/official/capacity/explicit-slot/component-results.json) · [Cascade Lake validation](tests/official/capacity/explicit-slot/cascade-lake.json)

The differential suite compares HTTP responses to the reference join on 13 small databases, including sparse IDs, nonmonotonic timestamps, ties, missing authors, overflowing records and likes that cross index pages. It also verifies acknowledged writes in a reopened database after killing the server.

For the earlier scored revision `0142ba2`, two clean Clang builds reproduced executable SHA-256 `a9c98f927fdf6d22ea7aa11e2cb02ce3c8f584426308e30a5a01a2d6fa3f736c`. Grafana k6 2.3.0 ran the repository’s unchanged `bench/load.js` against that build with a 10-second ramp, 1,000 users for two minutes and the standard ramp-down. All 15,688 status checks passed, with p95 0.564 ms and p99 2.705 ms. [Build script](build.sh) · [Warmup result and checksums](tests/official/results.json)

The WAL test injects short writes, failures on the commit-marker header and final page, and process death. It reopens each database with SQLite’s stock VFS. The reactor test drives a burst of writes across admission windows and checks that a stale completion from a closed socket releases its receive slot. A separate kernel test forces a short fixed-file import, observes readiness before its registration completes and checks the release of aliases after a fixed slot is reused. The renderer test places input fields next to inaccessible memory and compares every response byte with the general encoder. It checked 16,384 rows each on AVX2, SSE2 and a sanitized build. Compile and run commands are at the top of each C test. [WAL recovery](tests/wal_recovery.c) · [Reactor](tests/held-writes.c) · [Guarded renderer](tests/render-guard.c)

A second WAL test opens the files with an independent stock reader inside SQLite’s commit hook, before control returns to the writer. Across 24 transactions on 4 KiB and 16 KiB pages, all 72 database snapshots and all 24 live checkpoints contained the expected rows. The test observed 128 native writes, including 22 full buffer groups. Every write needed for the published transaction had already reached the underlying VFS. [Commit-publication regression and build command](tests/wal-publication.c)

## What the earlier local measurements established

The earlier loopback fixture held 342,500 users twice with the exact `0142ba2` executable, at 933 and 862 ms p99. Kensac missed at 38.95 s p99 between those two runs. The organizer scored the same C revision at 280,000 and Kensac at 310,000. In a socket-level check, all 24 loopback clients received responses on the CPU assigned to the server. The current comparison gives the clients their own TCP receive paths and records the CPU that performs the server's receive processing. [Historical Go measurements](tests/official/results.json) · [Network placement](tests/official/tap-split/README.md) · [Current comparison](tests/official/capacity/README.md)

At 400,000 users the earlier C build opened every connection and completed 12,220,158 requests without a request error. Its p95 was 2.153 s and p99 was 3.505 s. At full load it spent 24.59 of 27.59 µs per completed request in the kernel. A separate private-Ethernet run at 350,000 users also missed on latency and exhausted memory when clients disconnected together after cutoff. The server’s SQLite database passed integrity and foreign-key checks after recovery. A deterministic native client drove another set of holds with different pacing and scoring windows. Its best documented C pass was 311,000 users. [Go runs and process survival](tests/official/results.json) · [Ethernet and memory measurements](tests/official/network-topologies.json) · [Native client and results](tests/http/README.md)

A paired socket test measured 0.126 µs less sender CPU per send with Reno. With 5,833 new connections/s, enabling `TCP_DEFER_ACCEPT=1` increased CPU per first exchange by 5.9%. The server requests Reno when available. A separate build gave SQLite's own page cache a 68 MiB huge-page arena. Cold RSS rose from 4.8 to 72.6 MiB, and median CPU over three longer paired replays was 3.10 s with the ordinary allocator and 3.12 s with huge pages. These experiments informed the socket and allocator settings described above. [Paired kernel-option measurements](tests/official/kernel-options.json) · [Full documentation for the scored revision](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/0142ba23cd06969938941dac17e424ca7f3357cd/submissions/c-uring-willwang/README.md)

## Read-path measurements

Each round replays 750,000 operations against separate copies of the verified seed on a private tmpfs. The C and Rust adapters drive the same writes and commit groups of 32, bypassing their HTTP reactors’ write-admission timers. Timing covers live SQLite reads and JSON generation. The table gives the median of three round means on one AMD EPYC Genoa CPU, with alternating execution order and Rust 1.94.0 for Kensac. All 346,203 feeds and 345,422 point reads per round produced the same combined response hash.

| Read | This server | Kensac | Reduction |
|---|---:|---:|---:|
| Feed of 20 posts | 2.810 µs | 3.987 µs | 29.5% |
| One post | 0.705 µs | 0.822 µs | 14.2% |

These timings use the original reader. The same-C control compares the rank-20 certificate with verifying every chronological ID, alternating which plan reads first in each SQLite snapshot. Both use the same joins and SIMD JSON renderer, and every paired response matched byte for byte. Across three rounds per workload, the certificate reduced median feed time from 2.497 to 2.440 µs on a stable feed (2.3%) and from 2.932 to 2.867 µs with writes interleaved (2.2%). Verification of all 20 IDs was slightly faster in one of the three moving rounds. On the `0142ba2` Clang build we repeated the same-control experiment with three more 750,000-operation rounds per workload. The certificate reduced stable feed time from 1.931 to 1.894 µs (1.9%) and moving feed time from 2.390 to 2.248 µs (5.9%). Every paired response matched. The current reader adds request-local separator interpolation. In a separate three-pair replay of 40,000 operations using users and posts from an actual Go run, it lowered feed CPU from 2.499 to 2.215 µs and point CPU from 0.512 to 0.494 µs. [Control source](tests/rank-control.c)

Run these with Kensac’s Rust toolchain on the path. For the measurements above, `TMPDIR` pointed to a private executable tmpfs and `READ_BENCH_CPU` selected CPU 15.

```sh
bash submissions/c-uring-willwang/tests/read-benchmark.sh seed/feed.db 750000 3
bash submissions/c-uring-willwang/tests/read-benchmark.sh --rank seed/feed.db 750000 3
```

The moving replay’s combined SHA-256 is `57851f751a118a50c6729900698c4ee563d545d30073eee4da78a13c76306106`. The separate HTTP comparison measures actual sockets, writes, commits and response latency. [HTTP reproduction](tests/http/README.md)
