# C / io_uring with a live rank check

A C11 HTTP/1.1 server for Ubuntu 24.04 x86-64. It serves the challenge directly on `0.0.0.0:80` using Linux TCP, io_uring and epoll. One thread handles sockets, HTTP and SQLite transactions. A lower-priority thread runs checkpoints on a second SQLite connection. The core idea is to read neighboring posts from SQLite and prove that they form the chronological feed with a single index boundary check. [Database reader](reader.c) · [Network loop](reactor.c)

**The submission passed twice locally at 342,500 simultaneous users.** The organizer’s unmodified Go generator measured p95 **370 and 389 ms**, p99 **933 and 862 ms**, and **zero errors in 23,969,109 requests** across two independent 60-second ramps and five-minute holds. One CPU served every connection. Server memory peaked at 870 and 826 MiB during scoring, and 1,666 and 1,613 MiB through disconnect. Both original processes answered a fresh database health check after every client had closed. Both cgroups recorded zero memory-limit and OOM events. The published leader held 310,000 on the organizer’s separate VPS. [Local measurements](tests/official/results.json) · [Organizer’s leaderboard, checked November 12](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/8b20e650f3a8bdc9a20cefa1d154ba555e8566e5/LEADERBOARD.md#results)

The release also opened all 350,000 and 400,000 connections locally. Its p99 was 1.362 s and 3.505 s respectively, above the organizer’s one-second limit. [Full scores](#local-runs-with-the-organizers-go-load-generator)

## Why C

The feed reader needs SQLite’s internal B-tree cursors, page references and cell decoders. Building the reader and the pinned C amalgamation in the same translation unit lets Clang inline these operations. The reader can hold a page for the duration of a request and serialize its text directly into the response. Rust could expose those operations through a C shim, as Kensac does. The head-to-head compares two complete server designs. The same-C test isolates the algorithm and measured 5.9% shorter feed time with writes interleaved. [Reader](reader.c) · [Kensac’s shim](../rust-iouring-kensac/csrc/shim.c) · [Controlled comparison](#read-path-measurements)

At 400,000 users the release spent 24.59 of 27.59 µs per completed request in the kernel. The remaining 3.00 µs covered SQLite, HTTP, JSON and authentication together. That guided the choice to change the number and grouping of kernel operations. Hand-written assembly in the server would directly target that smaller userspace share. The JSON scanner already uses SSE2 intrinsics and SHA-256 uses OpenSSL’s optimized implementation. [CPU measurements](tests/official/results.json) · [JSON scanner](buffer.c)

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
4. Read the authors and like counts from their live trees. The greatest key in the likes index proves that every larger post ID has zero likes. A single ordered walk counts nearby IDs. On a heavily liked post, two equal keys on the same index leaf prove that all intervening keys are equal, so their cell positions give the count. Large gaps use an indexed seek.
5. If any part of the proof fails, use the general chronological plan. Missing authors require the SQL join to refill the feed. Oversized records can use SQLite’s normal overflow-page handling. When the table has fewer than 20 posts, the scan already contains every candidate.

All candidate rows, page references and borrowed text belong to the executing request. A post or like in the same transaction is visible to subsequent requests. Only schema metadata, prepared plans and SQLite’s own page cache are retained. For an author join, the reader uses the first and last separator on a visited table page to estimate the position of the next rowid. It checks the real separators, narrows the search by up to four cells and uses binary search for the rest. On the leaf it verifies the actual rowid. Each feed can use its own parent-page estimates for twenty authors. Both endpoints serialize borrowed row data directly into the HTTP buffer.

`GET /posts/:id` seeks the requested rowid, looks up its author and counts its likes through the same live indexes. Both read paths check the expected column layout, index order, collation and text encoding before enabling direct B-tree access. Prepared SQL implements the general case. For a feed with interleaved writes, checking the single rank boundary took 2.248 µs per feed versus 2.390 µs when checking all 20 keys in the same C build. [Read implementation](reader.c) · [Paired read measurements](#read-path-measurements)

Kensac also bypasses the SQL virtual machine. It first reads 20 chronological keys and then retrieves the corresponding posts. Scooter verifies successive rowids against individual index entries. Mojo and Hargup’s Lean entry walk the rowid tail after checking a table-wide ordering invariant. Pavlyhalim grouped author lookups and like-count range scans. Those entries motivated comparing the cost of discovering the feed with the cost of proving a candidate feed. [Kensac](../rust-iouring-kensac) · [Scooter](../rust-native-scooter1337) · [Mojo](../mojo-iouring-oh-meh) · [Lean](../lean-derive-hargup) · [Pavlyhalim](../c-iouring-pavlyhalim)

### HTTP, JSON and authentication

- **Parse in the receive buffer.** The HTTP parser represents the method, path, headers and body as slices. A complete request can go directly from a shared receive buffer to dispatch. Fragmented requests, pipelined leftovers and requests waiting for write admission are copied only when their bytes need to outlive that buffer. The JSON parser decodes the fields used by that route and validates the rest of the document. [HTTP parser](app.c) · [JSON parser](json_input.h)
- **Render straight into the send buffer.** The server prepares the static part of each HTTP status header at startup and fills in `Content-Length` after rendering. SSE2 examines 16 bytes at a time for quotes, backslashes and control characters. A row of ordinary UTF-8 needs one reservation and bulk copies. The general path escapes strings as required. A digit-pair table formats IDs and counts two digits at a time. [Response headers](app.c) · [JSON renderer](buffer.c)
- **Reuse the HMAC key schedule.** OpenSSL 3.0.13 prepares the inner and outer SHA-256 states for the configured secret once. Each authenticated request copies those states, hashes its own signed bytes, compares the MAC in constant time, and validates the algorithm, expiry and identity claims. [JWT verification](app.c)

### TCP, io_uring and epoll

The process listens directly on `HOST:PORT`. It adds sparse io_uring fixed-file tables as connections arrive, so the total socket count can exceed the process file-descriptor limit. Each table owns a separate edge-triggered epoll instance. The server accepts ordinary sockets and imports groups of up to 64 into the next sparse file table using `IORING_OP_FILES_UPDATE`. For each published slot it registers the socket with that table’s epoll instance. It closes consecutive ranges of temporary descriptors once per pass and keeps at most 512 temporary descriptors outstanding. A partially published batch retains its original descriptors and retries only the suffix. The fixed slot keeps the socket and its watch alive. Idle connections need no outstanding receive, input buffer or response allocation. A kernel without the required bulk registration uses direct accepts and temporarily installs an ordinary descriptor to register each watch. When installation is unavailable, it uses multishot receives. Environments without io_uring use ordinary epoll within the process descriptor limit.

The loop rotates across the tables. It takes up to 64 ready sockets from one kernel queue, reads them, executes the real SQLite work, and submits the replies to that same ring before moving on. This shortens the time between touching a socket for its request and touching it again for its response. The reactor borrows up to 256 buffers of 768 bytes from one shared pool for nonblocking reads. It returns each buffer on completion, even if its connection closed and the slot was reused. Longer requests continue across receives. The kernel-selected buffer group handles multishot reads. Lean #286 demonstrated that a temporary ordinary descriptor can transfer a socket reference to a long-lived io_uring operation. Here it registers the persistent epoll watch. [Lean #286 transport](../lean-verified-soonhokong/runtime/uring.c)

Linux queues unread readiness in arrival order within each table. A separate userspace queue handles a full buffer, a hangup, or another readiness edge arriving while a read is in flight. Its oldest entry always makes progress under backlog. Partial sends and pipelined requests preserve order on their connection.

After two consecutive busy passes the loop suppresses secondary rings’ eventfd notifications. Before sleeping, it restores those notifications, rechecks every ring and arms a one-shot poll on a parent epoll instance that watches the individual readiness queues. A readiness event arriving between the last direct scan and the transition to sleep still wakes the reactor. A separate C entry also suppresses eventfd signals while polling. [Related implementation](../c-rawuring-thehimalayanleo/src/server.c) · [Linux wakeup regression](tests/wake.c)

Eight `SO_REUSEPORT` listeners accept connections. They request `TCP_NODELAY` for short interactive exchanges and Reno congestion control when Linux makes it available. Each ring has a single issuer. The primary ring defers kernel task work until the issuer enters it. While walking ready events, the loop prefetches connection state six entries ahead. It handles up to 512 replies per pass and recycles up to 2,048 output buffers. Recycled buffers larger than 32 KiB release their storage, so one large response does not set the memory cost of later small responses. [Socket setup and buffer ownership](reactor.c) · [Measured TCP options](tests/official/kernel-options.json)

### Transactions, WAL and checkpoints

Requests parsed in one pass can share a SQLite transaction. Each read runs independently. The reactor can release reads from an unchanged snapshot at the end of each table’s turn. Once a transaction contains a write, all dependent replies wait for its commit. A rollback or commit failure converts those replies into errors. The first write after an idle period starts immediately. The group-commit window begins at 5 ms, doubles up to 32 ms after a commit with at least eight row changes and returns to 5 ms after a sparse or idle interval. The oldest held writes receive up to half the next open pass, while other sockets keep reading. New posts use the database’s timestamp default. Likes use the existing uniqueness constraint and distinguish a duplicate from a missing post.

Before SQLite first takes a lock, the process opens its own database file, asks Linux to drop its clean file-cache pages with `POSIX_FADV_DONTNEED`, and closes that descriptor. SQLite can then populate the working set on demand. The server uses WAL with `synchronous=NORMAL`. A wrapper over SQLite’s public VFS combines adjacent noncommit WAL frames while SQLite is executing the writer’s COMMIT statement. The commit-marker header flushes the buffered bytes to the underlying VFS, and its payload also reaches the underlying write inside SQLite’s own call. SQLite then publishes the new WAL index. Outside COMMIT, the wrapper can pair one noncommit header with its page. Reads, syncs, truncations and discontinuities flush pending bytes. SQLite’s standalone checksum repairs follow the original write path. The wrapper propagates short-write failures and keeps the checkpoint connection outside the writer’s grouping scope. Pavlyhalim’s WAL measurements motivated trying a narrower grouping boundary. [Pavlyhalim’s VFS](../c-iouring-pavlyhalim/src/db.h)

An independent connection at nice 5 runs `PASSIVE` checkpoints after 16,384 frames. If more frames arrive during a checkpoint, writes can be parked briefly while the worker catches up. Reads on other connections continue. A write can wait for this gap for up to 25 ms, or 100 ms if the WAL has reached 32,768 frames. A pipelined read behind a held write on the same socket keeps its HTTP ordering.

Transaction control, health checks and mutations use prepared statements that are reset and rebound for each operation. Creating a post returns its ID and SQLite-generated timestamp in the insert itself. Inserting a like checks that the post exists and uses the existing unique constraint. A successful insert needs one statement. Only an unchanged insert needs an additional lookup to distinguish a duplicate like from a missing post. [SQLite statements and commit policy](database.c) · [WAL write grouping](wal.c)

Other leading entries illustrate the scheduling choices. Rkkautsar separates the writer and submits completed feeds before point reads. Akbashev bounds the receive work after a buffer shortage. Delneg’s Objective-C and F# servers bound responses per pass, and the F# server drains held writes before fresh work when its write window opens. [Rust](../rust-iouring-rkkautsar) · [Swift](../swift-iouring-akbashev) · [Objective-C](../objc-uring-delneg) · [F#](../fsharp-uring-delneg)

### Compiler and SQLite configuration

Clang 18 builds the server and unmodified SQLite 3.54.1 amalgamation with `-O3`, `-march=native`, `-fno-plt` and whole-program link-time optimization. The release is built on its target CPU. The SQLite build disables allocation statistics, shared-cache mode and loadable extensions. Each thread owns a `SQLITE_OPEN_NOMUTEX` connection. The primary connection gives SQLite a 64 MiB page-cache target, disables database `mmap` and keeps temporary tables in memory. The checkpoint connection uses a 16-page cache. User data and the WAL remain on disk. [Compiler settings](Makefile) · [Pinned source](build.sh) · [SQLite configuration](database.c)

## Correctness checks

The test runner makes a private copy of the seed. Run the checks from the challenge repository root.

```sh
bash seed/make-seed.sh
bash test/run.sh submissions/c-uring-willwang
python3 submissions/c-uring-willwang/tests/differential.py
```

The differential suite compares HTTP responses to the reference join on 13 small databases, including sparse IDs, nonmonotonic timestamps, ties, missing authors, overflowing records and likes that cross index pages. It also verifies acknowledged writes in a reopened database after killing the server.

The normal checkout build and the independent clean Clang build produced the same executable, SHA-256 `a9c98f927fdf6d22ea7aa11e2cb02ce3c8f584426308e30a5a01a2d6fa3f736c`. Grafana k6 2.3.0 ran the repository’s unchanged `bench/load.js` against this build with a 10-second ramp, 1,000 users for two minutes and the standard ramp-down. All 15,688 status checks passed, with p95 0.564 ms and p99 2.705 ms. [Build script](build.sh) · [Warmup result and checksums](tests/official/results.json)

The WAL test injects short writes, failures on the commit-marker header and final page, and process death. It reopens each database with SQLite’s stock VFS. The reactor test drives a burst of writes across admission windows and checks that a stale completion from a closed socket releases its receive slot. Compile and run commands are at the top of each file. [WAL recovery test](tests/wal_recovery.c) · [Reactor test](tests/held-writes.c)

## Local runs with the organizer’s Go load generator

The organizer’s unmodified Go `mload` includes the 60-second ramp in its score. We used its standard think times, 60-second request timeout, uncompressed HTTP responses and one TCP connection per user. Four client processes ran on separate CPUs of the same host. The server had one CPU, a 1,750 MiB cgroup limit, a hard 65,535-descriptor limit and swap disabled. Each run started on a fresh disk-backed seed. The public leaderboard uses a separate load generator and a 1-vCPU Xeon VPS. [Go reproduction](tests/official/README.md) · [Recorded runs](tests/official/results.json) · [Official environment](../../LEADERBOARD.md)

At **342,500 users**, we ran the shipping Clang build, Kensac’s pinned Rust 1.94.0 build, then a fresh copy of the same Clang executable. Kensac measured 29,553 requests/s, p95 137 ms and p99 38.95 s. The C runs measured 33,343 and 33,237 requests/s, p95 370 and 389 ms, and p99 933 and 862 ms. Across the whole ramp and hold the C release completed 12.5–12.8% more requests. After every connection opened, the C runs used 10.9–11.3% less server CPU per request. Scored memory peaked at 826–870 MiB for C and 1,432 MiB for Kensac. All three runs recorded zero request errors, drained all 342,500 connections and answered `/health` on their original process. [Matched raw-score summaries and comparison](tests/official/results.json)

At **338,750 users on the same host**, the preceding GCC build of this C server completed 33,011 requests/s at p95 272 ms and p99 906 ms. Kensac’s scored revision, built with Rust 1.94.0, completed 29,947 requests/s at p95 141 ms and p99 29.19 s. Both opened every connection and reported zero request errors. The C server completed 10.2% more requests/s over the whole run and consumed 8.9% less CPU per request after every connection had opened. Its scored memory peak was 1,035 MiB compared with Kensac’s 1,439 MiB. During the synchronized disconnect the peaks reached 1,852 and 1,666 MiB respectively. The local 310,000-user Kensac run produced p95 114 ms and p99 4.11 s. The organizer measured 62 ms and 190 ms at that count. Linux loopback charged part of the clients’ TCP receive processing to the server CPU. In a socket-level control, all 24 client responses arrived on that CPU. [Matched measurements](tests/official/results.json) · [Published score](../../LEADERBOARD.md#results)

| C implementation | Users | Ramp + hold | Full p95 | Full p99 | Requests/s after all sockets opened | Errors |
|---|---:|---:|---:|---:|---:|---:|
| Earlier GCC build with individual socket registration | 335,000 | 60 + 300 s | 200 ms | 812 ms | 35,294 | 0 |
| Same GCC executable | 338,750 | 60 + 300 s | 272 ms | 906 ms | 35,552 | 0 |
| Same executable | 342,500 | 60 + 300 s | 425 ms | 1,597 ms | 35,778 | 0 |
| Same executable | 350,000 | 60 + 300 s | 567 ms | 2,733 ms | 36,109 | 0 |
| Same executable | 400,000 | 60 + 300 s | 1,988 ms | 3,796 ms | 36,263 | 0 |
| Single epoll queue, reads and sends grouped by table | 350,000 | 60 + 75 s | 2,089 ms | 2,426 ms | 34,438 | 0 |
| One epoll queue per table | 350,000 | 60 + 75 s | 1,446 ms | 2,549 ms | 36,086 | 0 |
| Per-table queues, pooled receives, adaptive commits | 350,000 | 60 + 75 s | 1,233 ms | 2,262 ms | 36,529 | 0 |
| Per-table queues, fixed write window | 340,000 | 60 + 300 s | 451 ms | 1,052 ms | 35,376 | 0 |
| Clang LTO, batched socket registration, pooled reads and Reno | 345,000 | 60 + 300 s | 315 ms | 1,209 ms | 36,111 | 0 |
| Same Clang executable | 350,000 | 60 + 300 s | 524 ms | 1,535 ms | 36,184 | 0 |
| **Current release with one-time file-cache advice** | **342,500** | **60 + 300 s** | **370 ms** | **933 ms** | **35,904** | **0** |
| **Independent repeat of the same release** | **342,500** | **60 + 300 s** | **389 ms** | **862 ms** | **35,753** | **0** |
| Same current release | 345,000 | 60 + 300 s | 315 ms | 1,041 ms | 36,165 | 0 |
| Same current release | 350,000 | 60 + 300 s | 484 ms | 1,362 ms | 36,215 | 0 |
| Same current release | 400,000 | 60 + 300 s | 2,153 ms | 3,505 ms | 36,187 | 0 |
| Application and SQLite compiled with a generated profile | 350,000 | 60 + 300 s | 434 ms | 1,376 ms | 36,470 | 0 |

The 335,000-user run and the 350,000-user pooled-receive pilot opened every connection by the 65-second sample. At 335,000 users the GCC executable’s SHA-256 was `dda50531693829782f8047a6cfb3c9ad33ef81ed8ef7b921063d71521a44fa38`. Its 32,766 requests/s over the entire ramp and hold rose to 35,294 after all sockets opened. The 350,000-user pilot used the same design before cleanup, with 27.31 µs of CPU per request from seconds 65–130, including 24.53 µs in the kernel. The full-run latency columns include ramp-up and the workload that follows it. The organizer’s limits are 500 ms at p95 and one second at p99. [Per-run intervals and histogram counts](tests/official/results.json)

At 350,000 the current Clang release completed 12,117,252 requests. It recorded 206,848 response times of at least one second. The worst five-second p99 was 3.44 s around the end of the ramp. Between seconds 100 and 355 every shard’s five-second p99 was at most 557 ms. After the run, the server drained every disconnect, returned a fresh database health response and reached a lifetime peak of 1,644.5 MiB with zero memory-limit events. [Full score and shutdown check](tests/official/results.json)

An independent build trained Clang on generated ordered and sparse databases, successful HTTP requests, invalid requests and writes. Two fresh builds produced the identical executable. Across the full 350,000-user run it completed 12,193,647 requests with zero errors and survived the final disconnect. Profile-guided compilation improved p95 by 50 ms, while p99 moved from 1,362 to 1,376 ms. The submission uses ordinary link-time optimization. [Matched conditions and per-endpoint measurements](tests/official/results.json)

At **400,000 users**, the shipping executable served 12,220,158 requests with zero errors. Every socket was established by the 65-second sample. Full-run p95 was 2.153 s and p99 was 3.505 s. Across seconds 65–355 the server used 99.86% of its CPU and completed 36,187 requests/s. Each request cost 27.59 µs, including 24.59 µs in the kernel. Scored memory peaked at 1,142 MiB. During disconnect the kernel counted 53,446 charges against the memory limit and a transient 1,877 MiB peak, with zero OOM events. The original process answered `/health` 9.96 s after scoring ended. With the scorer’s 20.5 seconds of average think time and 2.17 requests per browsing iteration, 400,000 users would issue about 42,341 requests/s with instantaneous replies. [400,000-user measurements and shutdown check](tests/official/results.json) · [Browsing loop](tests/official/mload/main.go)

### Kernel options

The largest measured kernel-level improvement here came from giving each fixed-file table its own epoll readiness queue and issuing its replies before advancing to the next table. This needs ordinary process privileges and works with standard Linux TCP. A separate socket experiment saved a median 0.126 µs per send with Reno on twelve paired runs against CUBIC. In three paired first-request trials at 5,833 new connections/s, `TCP_DEFER_ACCEPT=1` increased median server CPU per new connection and first HTTP exchange by 5.9%. The release requests Reno on the listener. If unavailable, Linux keeps its configured congestion control. It leaves deferred accept at the system default. [Banked application measurements](tests/official/results.json) · [Paired TCP measurements](tests/official/kernel-options.json) · [Linux TCP options](https://man7.org/linux/man-pages/man7/tcp.7.html)

Transparent huge pages can also be requested by the application. A 68 MiB aligned arena for SQLite’s own page cache received the full 68 MiB of huge pages. It saved a median 0.184 µs per mixed request across two shorter replay pairs, while increasing cold RSS from 4.8 to 72.6 MiB. Three longer 750,000-operation pairs had median total CPU of 3.10 s with the stock allocator and 3.12 s with the huge-page arena. The submission uses SQLite’s ordinary allocator. [Paired allocator measurements](tests/official/kernel-options.json)

In four controlled disconnect bursts, 8,028 sockets per run had an unread FIN. Each held 960 bytes of receive memory and 3,136 bytes of forward allocation. A separate 64-socket test returned all 256 KiB as soon as the application read EOF, even though all 64 descriptors stayed open. The memory available when clients disconnect together therefore matters. A one-time `POSIX_FADV_DONTNEED` hint on the server’s own database reduced file-cache charges from 251.3 to 46.2 MiB after 16,384 mixed application requests on paired fresh copies. The first feed took 6.74 ms with the hint and 0.20 ms with the warm copy. The resulting rows and response hashes matched. [Socket memory and file-residency measurements](tests/official/kernel-options.json)

A second local test used private virtual Ethernet links, a 1,500-byte MTU, and separate CPUs for server and client TCP receive processing. All 24 placement probes on each end landed on that end’s designated CPU. At 350,000 users the earlier GCC build accepted all 350,000 connections and returned 7,485,848 successful responses. The full p95 was 8.67 s and p99 was 12.04 s. From seconds 65–355 the server CPU was 99.95% busy, including 68.18% in softirq. The virtual interfaces recorded over 440,000 receive-queue drops over the hold. Two and a half seconds after the scored window, the clients closed their sockets together and the server’s cgroup was OOM-killed. The cgroup peak rose from 1,691 MiB in the last sample before cutoff to 1,911 MiB at shutdown. This setup exposes packet-receive and teardown costs that the loopback result does not measure in the same way. [Topology, CPU, socket and memory measurements](tests/official/network-topologies.json) · [Private Ethernet reproduction](tests/official/veth/README.md)

Repeating the private Ethernet test with the startup file-cache hint reduced median file charges from 338 to 121 MiB after all connections opened. It completed 7,561,898 requests at p95 8.33 s and p99 11.80 s, with zero request errors. The kernel killed the process 2.27 seconds after the cutoff. By the first observed kill the cgroup held 1.25 GiB of socket buffers and just 152 KiB of file cache. The application’s database passed two independent full integrity and foreign-key checks after recovery. Both runs verified the original seed before starting the application. The hinted run used a nonfaulting residency check on the live files so the measurement itself would preserve the effect of the hint. [Paired private Ethernet runs](tests/official/network-topologies.json)

A trial read each clean FIN promptly and deferred the final socket close so other ready sockets could release their receive memory. With 8,192 connections closing at roughly 124,000 per second, this reduced peak socket memory from 27.3–27.8 MiB to 6.9–7.0 MiB in two paired runs. At 350,000 over the same private Ethernet setup, the ordinary Clang build and the modified build each reached p99 10.68 s and approximately 21,180 requests/s. Deferring the close reduced peak socket memory by only 1.5%. Two seconds after cutoff both servers had more than 344,000 connections in `ESTABLISHED` or `CLOSE_WAIT`, while the client count had already fallen to zero. Both server processes died under memory pressure. Both recovered databases passed two integrity and foreign-key checks. The release closes clean sockets immediately. [Small disconnect trials](tests/official/kernel-options.json) · [Matched 350,000-user disconnects](tests/official/network-topologies.json)

The challenge allows settings inside the server process and specifies a stock VM with no kernel tuning. The service runs as the deploy user with permission to bind port 80. Creating an AF_XDP socket requires `CAP_NET_RAW` in the network namespace. Directing a NIC receive queue to it also requires a configured XDP program and its socket map. A DPDK network driver needs access to the device or a supported virtual interface. The scored runtime provides neither setup. Both would also require a TCP implementation that handles retransmissions, acknowledgments, congestion control and hundreds of thousands of live sessions. Any gain would need a separate measurement on hardware that supports that deployment. [Challenge rules](../../README.md#rules) · [Organizer environment](../../LEADERBOARD.md) · [Linux AF_XDP socket creation](https://github.com/torvalds/linux/blob/v6.8/net/xdp/xsk.c) · [AF_XDP documentation](https://docs.kernel.org/networking/af_xdp.html) · [DPDK Linux driver setup](https://doc.dpdk.org/guides/linux_gsg/linux_drivers.html)

## Paired five-minute HTTP holds with the native client

The local head-to-head uses a deterministic C port of the browsing loop. It pins the server to one AMD EPYC Genoa vCPU under Linux 6.8.0-147, with a 2 GiB cgroup limit, swap disabled, a hard 65,535-descriptor limit and a fresh, disk-backed copy of the challenge seed. Four client shards on other CPUs browse real feed responses and maintain one keep-alive connection per user. Each server runs alone. The 300-second window begins after a 60-second ramp and another 30 seconds at full load. The organizer's Go scorer includes the ramp in its percentiles and uses Go's TCP keepalive defaults. Both clients have reproduction scripts. [Native client](tests/http/README.md) · [Organizer’s Go scorer](tests/official/README.md) · [Public leaderboard](../../LEADERBOARD.md#results)

| Implementation | Connected users | Requests/s | p95 | p99 | Errors | Peak cgroup memory | Result |
|---|---:|---:|---:|---:|---:|---:|---|
| Earlier C build, single readiness queue | 311,000 | 32,344 | 440 ms | 618 ms | 0 | 980 MiB | Passed |
| Kensac #264, Rust 1.94.0 | 311,000 | 31,119 | 134 ms | 15,281 ms | 0 | 1,386 MiB | Missed p99 |
| Earlier C build, single readiness queue | 315,000 | 32,298 | 743 ms | 924 ms | 0 | 992 MiB | Missed p95 |
| C, per-table queues and pooled receives | 350,000 | 34,719 | 1,029 ms | 1,093 ms | 0 | 1,149 MiB | Missed p95 and p99 |

At 311,000 users, the earlier C hold completed 9,703,289 requests. Its longest read took 740 ms, its longest write took 738 ms, and the oldest request still outstanding at cutoff was 293 ms. Kensac completed 9,335,729 requests, of which 354,064 took at least one second. Both servers kept all 311,000 connections established. The 350,000-user C hold used the tighter 1,750 MiB limit. It completed 10,415,677 requests and kept every connection open for the entire measured five minutes. [Local measurements](tests/http/results.json) · [Published leaderboard](../../LEADERBOARD.md#results)

Kensac’s event loop runs a checkpoint itself when its WAL reaches 65,536 frames. In the matched native-client run, 14 one-second intervals had 3,611–6,673 read syscalls on that thread. The other 285 complete intervals had three in total. In Kensac’s separate 310,000-user Go run, its five slowest five-second p99 windows recorded zero foreground reads. Those observations establish the checkpoint behavior and leave the cause of its worst latency open. The C server runs checkpoints on its second connection while reads continue. [Kensac checkpoint code](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/4298101/submissions/rust-iouring-kensac/csrc/shim.c#L285-L296) · [Native intervals](tests/http/results.json)

## Read-path measurements

Each round replays 750,000 operations against separate copies of the verified seed on a private tmpfs. The C and Rust adapters drive the same writes and commit groups of 32, bypassing their HTTP reactors’ write-admission timers. Timing covers live SQLite reads and JSON generation. The table gives the median of three round means on one AMD EPYC Genoa CPU, with alternating execution order and Rust 1.94.0 for Kensac. All 346,203 feeds and 345,422 point reads per round produced the same combined response hash.

| Read | This server | Kensac | Reduction |
|---|---:|---:|---:|
| Feed of 20 posts | 2.810 µs | 3.987 µs | 29.5% |
| One post | 0.705 µs | 0.822 µs | 14.2% |

These timings use the original reader. The same-C control compares the rank-20 certificate with verifying every chronological ID, alternating which plan reads first in each SQLite snapshot. Both use the same joins and SIMD JSON renderer, and every paired response matched byte for byte. Across three rounds per workload, the certificate reduced median feed time from 2.497 to 2.440 µs on a stable feed (2.3%) and from 2.932 to 2.867 µs with writes interleaved (2.2%). Verification of all 20 IDs was slightly faster in one of the three moving rounds. On the current Clang build we repeated the same-control experiment with three more 750,000-operation rounds per workload. The certificate reduced stable feed time from 1.931 to 1.894 µs (1.9%) and moving feed time from 2.390 to 2.248 µs (5.9%). Every paired response matched. The current reader adds request-local separator interpolation. In a separate three-pair replay of 40,000 operations using users and posts from an actual Go run, it lowered feed CPU from 2.499 to 2.215 µs and point CPU from 0.512 to 0.494 µs. [Control source](tests/rank-control.c)

Run these with Kensac’s Rust toolchain on the path. For the measurements above, `TMPDIR` pointed to a private executable tmpfs and `READ_BENCH_CPU` selected CPU 15.

```sh
bash submissions/c-uring-willwang/tests/read-benchmark.sh seed/feed.db 750000 3
bash submissions/c-uring-willwang/tests/read-benchmark.sh --rank seed/feed.db 750000 3
```

The moving replay’s combined SHA-256 is `57851f751a118a50c6729900698c4ee563d545d30073eee4da78a13c76306106`. The separate HTTP comparison measures actual sockets, writes, commits and response latency. [HTTP reproduction](tests/http/README.md)
