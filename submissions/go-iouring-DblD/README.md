# Go + io_uring + SQLite (one thread, direct)

| | |
|---|---|
| Language | Go 1.27.1 (official tarball, sha256-pinned in `install.sh`) |
| HTTP | Hand-written on io_uring through raw system calls. No HTTP or io_uring library. |
| SQLite | 3.54.1, the official amalgamation, sha256-pinned in `build.sh`, unmodified. `db.c` includes it, so it is compiled as part of that file. Writes are plain SQL. Reads use SQLite's own B-tree cursor functions, with the SQL statements kept as a fallback. |
| Front | **Direct** on `0.0.0.0:80`, no Nginx |
| Dependencies | None besides SQLite |

## Design

One thread runs one loop: wait for a batch of events, answer every request in it inside a single SQLite transaction, commit, then send the responses.

- **io_uring, one system call per pass.** Accepts and receives are multishot. Receive buffers come from a pool lent to the kernel (8,192 buffers of 1 KB per ring), so an idle connection holds no buffer. Our own state per connection is 12 bytes.
- **More than 65,535 connections.** Accepted sockets go straight into io_uring's own file table (direct descriptors) and never get a process file descriptor. A table can't be larger than `RLIMIT_NOFILE`, so when a ring's table is nearly full the server adds another ring, up to 16.
- **Batching.** The loop waits for up to 256 completions or 1 ms. Under light load that adds up to 1 ms to a response; under heavy load one wake-up, one system call and one commit serve everything that arrived.
- **Group commit.** The whole pass is one transaction. Responses are built into buffers during the pass and are queued for sending only after `COMMIT` returns. If the commit fails, every connection answered in that pass is closed instead, so no client is told a write succeeded when it didn't.
- **An answer budget per pass.** A pass answers at most 256 requests. A request past the budget is already parsed and its token verified; what waits is a 32-byte record in a preallocated ring, and it is answered in a later pass. Each pass answers its own arrivals first and then the waiting ones from the newest end; one that has waited a second gets a share of every pass, so none waits without bound and none is dropped. Why: every response waits for its pass's commit, and a reply that leaves more than 40 ms after its request costs the kernel an extra acknowledgement packet. Without a budget one long pass made every request behind it dearer and the backlog fed itself; the same build passed or failed 200,000 users from run to run.
- **Overload queues, it doesn't spin or drop.** If the receive pool runs dry, the affected connections go on a list and are re-armed at the end of the pass, never more of them than there are buffers. If every send buffer is in use, the request is kept and retried when one is free.
- **Reads straight from the B-trees.** The feed, a single post and the "does this post exist" check open cursors on the tables and indexes with SQLite's own functions (`sqlite3BtreeFirst`, `Next`, `TableMoveto`, `IndexMoveto`, `PayloadFetch`) and decode the records by hand. Only SQLite's functions touch pages and cursors. At startup the server answers the feed and each of its posts both ways and compares the bytes; only if they match are the direct reads switched on. Otherwise, and on any error later, the SQL statements answer. The queries are the obvious ones: the feed by `ORDER BY created_at DESC, id DESC LIMIT 20`, the like count by a correlated `count(*)`.
- **The feed in key order** (the plan `rust-iouring-kensac` #264 describes)**.** The feed's 20 posts are looked up in ascending id order and their authors in ascending user id order, on cursors that stay in place between lookups: asked for the key after the one it is on, `sqlite3BtreeTableMoveto` steps to the next row instead of walking down from the root. The like counts for the feed's id range come from one seek and one forward walk of `likes_post_id_idx`. What is read is copied aside and written out in feed order. That is about 22 descents where there were 60.
- **Tokens without garbage.** HMAC-SHA256 from the saved hash states of the two key pads, base64 into reused buffers, and a scanner for flat JSON objects. Any token the scanner does not read exactly goes to a standard-library version of the same check. `auth_test.go` runs 78 tokens through both (missing, repeated or mistyped `exp`, `alg` and `sub`, non-canonical base64, wrong signatures, 10 KB tokens) and requires the same answer.
- **Checkpoints off the loop, a few per WAL cycle** (the scheme of `go-iouring-davidefrageri` #354 and `swift-iouring-akbashev` #376, see Credits)**.** SQLite's automatic checkpoint is off. Nothing happens until the WAL reaches 12,288 pages. Then a lower-priority thread (nice 5) runs `PASSIVE` checkpoints with writes still flowing and syncs the database file itself, until what is left to copy is small.
- **Write hold** (as in several entries, see Credits)**.** A checkpoint can only finish, and the WAL only start over, if no commit lands while it runs. So once the early copying has caught up, the loop stops committing writes (reads continue) while a second, normal-priority thread copies the short tail. At 200,000 users that is typically 30 to 70 ms about every four seconds; it is cut off at 250 ms.
- **C writes the response body.** For the feed and a single post, Go writes the response header into the send buffer with the `Content-Length` field left blank, C writes the JSON straight after it from SQLite's pages, and Go fills in the digits. The text is copied once on its way from the page to the socket. Strings are copied and tested for characters to escape 16 bytes at a time. If the body does not fit in what is left of the buffer, C writes nothing usable and reports it, and the request gets a 500; `json_test.go` checks the refusal. Integers are written straight into the output from a table of digit pairs, and a string with nothing to escape takes a path without a tail loop.
- **Compiler flags for the one C file.** `db.c` and the SQLite inside it are built with `-O3 -march=native` and without the stack canary, checked `memcpy`, stack probes and branch-tracking markers that Ubuntu's gcc adds by default (`build.sh`). The server is built on the box it runs on. `build.sh` compiles the file itself into `db.syso`, which the go tool links; `db.c` is marked so that the go tool does not compile it a second time.
- **A profile-guided build of the C side** (allowed by rule 10 since the ruling of 2026-11-15; several entries build this way, among them `c-iouring-pavlyhalim` and `c-iouring-jowinjestine`)**.** `build.sh` compiles `db.c` with counters, runs a training program, and compiles again with what the counters recorded, so gcc knows the usual side of each branch in SQLite and lays the code out for it. The training program (`train.go`, `train.c`, built only for this step) makes its own scratch database with the challenge's schema and rows of the seed's shape, and sends the five endpoints through the server's own parse and execute functions, in process, without a socket. Nothing recorded is committed. Any failure, or a training run over two minutes, ends in the plain build. It adds about a minute to the build.
- **A look at every waiting connection before handling any.** The records of the connections are megabytes, and a completion's connection is anywhere in them, so the first look at each one waits for memory. Before a ring's completions are handled, one loop reads the record of each. Those reads do not depend on one another, so the processor has several of the waits under way at once.
- **A write window** (the idea and the 20 ms are `fsharp-uring-delneg`'s, #398)**.** After a pass that committed writes, the next writes wait up to 20 ms while reads are answered as they come; a write that arrives with the window open is not delayed. One commit then carries about a hundred writes instead of eight, so pages they share (the end of each table and index) go to the WAL once per window instead of once per pass. At 150,000 users that is 4,750 `pwrite` calls a second instead of 7,400 and a third fewer write holds. It costs latency: p95 goes from 3 ms to about 10 ms and p99 from 9 to about 21 ms, well inside the limits and under the 40 ms after which a late reply costs an extra acknowledgement.
- **SQLite's page memory on 2 MB pages.** SQLite takes one block of memory for the contents of its page cache if the application supplies it (`SQLITE_CONFIG_PAGECACHE`, a public option; the cache code stays SQLite's own). Ours is 38 MB (the 32 MB cache, its page headers and a margin), 2 MB-aligned, with `madvise(MADV_HUGEPAGE)`, and is touched once at startup so the kernel finds and clears the memory before the first request. The cache then needs 19 address-translation entries instead of more than 8,000. On a box that has been up for days the free memory is fragmented, and the kernel then has 2 MB pages for part of the block or for none of it (on our box after three and a half days: 0 to 12 MB of the 38; right after a reboot: all 38). The block is therefore touched from the top down, because SQLite hands out its slots from the highest address first, so the 2 MB pages there are go under the pages in use. The startup line says how much of the block the kernel put on 2 MB pages; where it gives none, the block works as ordinary pages.
- **The program's own code on a 2 MB page.** The code that runs per request (SQLite's hot functions, which the profile-guided build puts first, and the Go side) lies in the first 2 MB of the binary's text. This kernel does not put a file's pages on 2 MB pages by itself (`CONFIG_READ_ONLY_THP_FOR_FS` is off), so at startup, before the Go runtime runs, the process copies that 2 MB range of its own text into anonymous memory for which it has asked for a huge page, makes the copy read-and-execute, and moves it over the original addresses with `mremap`. The bytes and the addresses are the same; the range then needs one address-translation entry instead of 512. It is never writable and executable at once. There are three outcomes, and the startup line says which one happened: the range was moved and is on a 2 MB page; it was moved but the kernel gave ordinary pages (reported as "0 MB of it on 2 MB pages"; the program runs the same); or it was left as mapped from the file, with the reason (huge pages are off on the box, the text has no whole 2 MB unit, or the kernel refused a step). The three pages of ELF headers in front of the text lie in the same 2 MB and come along, read-and-execute like the rest. The remainder of the text (about 340 KB of cold SQLite code: planner, DDL, SQL functions) stays on the file's pages. One side effect: `perf` cannot name functions in the moved range, because that range is no longer backed by the file.
- **A commit's WAL frames in one write** (the design is `go-iouring-davidefrageri`'s, #354, who names `c-iouring-pavlyhalim`, #245, for the pattern)**.** SQLite writes every WAL frame with two calls, a 24-byte header and the page, so a commit of 44 pages is 88 `pwrite` calls. A file layer registered through `sqlite3_vfs` sits on top of SQLite's own `unix-excl`: on the WAL file, a write that continues exactly where the last one ended is copied to a 128 KB buffer, and the buffer goes to the file in one call when the page of the commit frame arrives. Every other call on that file (read, sync, truncate, size, lock, file control, close, a write elsewhere such as the WAL header, a write that does not fit) first writes the buffer and then goes through unchanged. So the file receives the same bytes at the same offsets in the same order; the commit frame is in the kernel before SQLite updates the WAL's index and before `COMMIT` returns; a sync happens where SQLite asks for one; and a failed write is reported inside the transaction, as SQLite's own would be. `go test` checks this with a database (`BENCH_DB=<copy of the seed> go test -run WAL .`): the layer hashes every byte and offset SQLite asks to be written and every one it hands to the file, and the two must be equal after each of 279 commits of 1 to 2,000 requests; the test fails if one bit of one frame header is flipped inside the layer. Killing the server with `kill -9` in a burst of 24 parallel writers, three times, lost none of 2,385, 3,559 and 4,759 acknowledged posts, and `integrity_check` was clean. The stats line prints WAL write calls and frames, so a build that stopped joining writes would show.
- **`unix-excl`** (as `rust-iouring-kensac`, #264, and #354)**.** The process takes the lock on the database file once and keeps it; the WAL's index lives in memory shared by our three connections instead of a `-shm` file, and a transaction makes no `fcntl` calls. The price: **no other process can open the database while the server runs** (the `sqlite3` shell answers "database is locked").
- **Collecting a ring's completions without a timer.** Each pass collects what is ready on every ring but the one it waits on. The usual call for that asks for "as many completions as fit, with a zero timeout" (see Credits). The kernel runs the waiting work and then, because fewer than that many are ready, goes on to wait: it arms a timer that is already due. A standalone test on the same kind of machine put that at 3.6 µs per call with nothing to collect, against 0.17 µs for a call that asks for none and passes no timeout. The loop now makes the cheap call, repeats it up to three times while a call brings 16 or more completions or the kernel's flag says work is still waiting (`IORING_SETUP_TASKRUN_FLAG`), and makes the timed call only if that has not emptied the ring. The reason for the care: #343 and #354 found that some 6.8 kernels run only about 20 completions for a call that asks for none; ours (6.8.0-117) runs all of them, and Ubuntu's current 6.8 source runs what was waiting when the call began and flags the rest. The loop is right under all three; on a kernel with the limit, a ring with 56 or more completions waiting (2 % of calls at 290,000 users) pays three cheap calls on top of the timed one.
- **No bare ACK for a new connection's first request** (the idea is `fsharp-uring-delneg`'s, #437)**.** `TCP_QUICKACK` is cleared on the listening sockets and accepted sockets inherit it. The kernel otherwise acknowledges the first data on a new connection at once with a packet of its own; later requests are already acknowledged by their response.
- **Go runtime.** One locked OS thread, raw system calls for `io_uring_enter`, the periodic garbage collector off with a 64 MB ceiling. Everything the kernel reads or writes is in mmap'd memory outside the Go heap. Each request crosses into C once or twice.

## Rules

- **Rule 5, no caching.** Every request reads from SQLite. Nothing of ours is kept between requests: cursors are released after every request. SQLite's own page cache is set to 32 MB (its memory is a block we hand it, see Design; what it holds and when it lets go of a page is SQLite's business as before). Every token's HS256 signature and `exp` are verified on every request.
- **Rule 6, durability.** WAL with `synchronous=NORMAL`. A response is sent only after the commit that contains its write. A read in the same pass as a write can see that write before the commit, but its response waits for the same commit and is dropped if the commit fails.
- **Rule 7.** Nothing depends on the load test's shape. The batching wait, the write window, the checkpoint cycle and the write hold apply to any traffic.
- **Rule 10, the profile-guided build.** The profile is recorded by `build.sh` on the box, during the build, on a scratch database the training program makes itself (the challenge's schema, rows of the seed's shape, content made up). The seed database and the tokens are not read. The training requests are the five endpoints in the proportions the public load script gives them (per round a feed and a post, a like in 15 % and a new post in 2 % of rounds); those four numbers are written into `train.go`. Nothing recorded is committed, and without a profile the build is the plain one.
- **Rule 11.** Nothing on the box is tuned. The process raises its own soft `RLIMIT_NOFILE` to the hard limit it was given, lowers the priority of one of its own threads, and asks for huge pages for two of its own mappings with `madvise` (SQLite's page memory and the copy of its own code; the second also uses `mmap`, `mprotect` and `mremap`, all on the process's own address space). On its own listening sockets it sets `TCP_NODELAY` and clears `TCP_QUICKACK`. It reads `/sys/kernel/mm/transparent_hugepage/enabled` to see whether to try, and writes no kernel setting anywhere.
- **SQLite internals.** The source is not patched. The read path calls SQLite's B-tree cursor functions (allowed by the ruling of 2026-10-31) and builds the search key they take as an argument. It does not write cursor fields or any other private state, does not read pages itself, and does not use B-tree writes or a custom page cache. It registers one file layer of its own (`sqlite3_vfs`, SQLite's public interface for that) on top of SQLite's `unix-excl`; the layer changes nothing but the number of calls in which a commit's bytes reach the WAL file (see Design).

## Measured

On a 1 vCPU / 2 GB VPS rented from the same provider as the scoring box: Xeon Platinum 8260, Ubuntu 24.04, kernel 6.8.0-117, 1,967 MB, no swap. The server runs as a systemd unit, user `deploy`, `LimitNOFILE=65535`. Load comes from a second VPS (8 vCPU, five addresses) with our own generator, which follows `bench/load.js`: the same user model, a 60 s ramp, a 5 minute hold. On this pair `go-nethttp-tbsvttr` passes 53,000 (its official score) and fails 60,000, and `go-iouring-davidefrageri` (official 270,000) passes 235,000 and fails 250,000 and 270,000. So the pair matches the scorer at the low end and is harder than it at the top: both VPSes are shared-CPU guests that their hosts stop for 50 to 200 ms now and then, and a stopped load generator stops every user at once. **These are our numbers, not official ones.**

| Users | Hold | Result | p95 | p99 | Failed | VM CPU |
|---:|---|---|---:|---:|---:|---:|
| 150,000 | 1 min | pass, 3 of 3 | 3 to 5 ms | 9 to 11 ms | 0 % | 46 to 48 % |
| 200,000 | 5 min | pass | 9.7 ms | 26 ms | 0 % | 61 % |
| 200,000 | 5 min | pass | 7.7 ms | 20 ms | 0 % | 60 % |
| 200,000 | 5 min | pass (timers on) | 9.9 ms | 25 ms | 0 % | 62 % |
| 235,000 | 5 min | pass | 16 ms | 229 ms | 0 % | 70 % |
| 235,000 | 5 min | fail | 21 ms | 1,245 ms | 0 % | 73 % |
| 250,000 | 5 min | fail | 336 ms | 1,638 ms | 0 % | 80 % |

The same pair of machines, the same night and generator, `go-iouring-davidefrageri`: 200,000 pass (p99 43 ms, 53 % CPU), 235,000 pass (p99 170 ms, 64 %), 250,000 fail (p99 3.7 s, 73 %). Its 200,000 and 235,000 runs were taken one to two hours before ours, before our last two changes; the 250,000 runs were minutes apart.

All but one of these runs had the box's periodic maintenance timers (apt, sysstat, man-db and the like) stopped, because `apt-daily` took 40 % of the CPU in the middle of one earlier run. The row marked "timers on" is a stock box.

At 200,000 the process and its share of kernel memory peak at about 520 MB and about 900 MB is still available on the box; at 250,000 about 730 MB is. (Those figures are from the second version; the 32 MB page cache of this one adds 13 to 22 MB at 150,000 and 6 MB at 250,000.)

Changes that were measured on this box, each against the version before it, whole-VM CPU at 150,000 users unless it says otherwise:

| Change | Before | After |
|---|---|---|
| Checkpoints in long cycles instead of whenever the WAL passed 1,000 pages | p99 200 to 740 ms, 68 % | p99 38 to 110 ms, 61 % |
| Reads from the B-trees instead of SQL | 61 % | 51 % |
| An answer budget of 256 per pass (200,000 users, a forced 50 ms stall every 30 s) | 34 of 159 seconds in the delayed-ACK state | 7 of 159 |
| Checkpoint copier at nice 5 instead of 19 (200,000 users) | hold timeouts 6 and 0, WAL up to 31,111 pages | 0 and 0, 16,263 pages |
| Token check without allocation (200,000 users, 5 min) | 16 garbage collections | 0 |
| Feed in key order, likes in one walk | 52.5 % | 48.9 % |
| JSON strings scanned eight bytes at a time | 48.9 % | 46.9 % |

### Official result of the first version

PR #487 (commit `7200286`) was scored at **228,000 users**: 22,143 requests/s, p95 14.0 ms, p99 211.9 ms, no errors, 72 % CPU, 30 µs of CPU per request. That is the scorer's number, not ours. The table above is that version on our machines; it predicted the score (200,000 three times of three, 235,000 once of two).

### This version against the first, by counters

Since #487 the work has been on CPU per request in our own process, measured with `perf stat` on the server process at 150,000 users (16,000 requests/s), user space apart from kernel:

| Per request, user space | #487 | #494 | this version | `go-iouring-davidefrageri` | `rust-iouring-kensac` |
|---|---:|---:|---:|---:|---:|
| Instructions | 46,500 | 31,490 | 31,400 | 28,200 | 29,400 |
| Cycles | 29,000 | 23,430 | 20,700 | 17,200 | 21,100 |
| Server process, share of one CPU (plain runs) | 30.3 % | 27.2 % | 24.4 % | 23.6 to 24.8 % | 26.5 to 27.5 % |

Kernel instructions per request are the same for all four (21,000 to 24,000). Whole-VM CPU at 150,000 users, five runs of each version in alternation: median 49.0 % before, 46.2 % now; the server process 30.3 % before, 27.2 % now.

| Step | User instructions per request after it |
|---|---:|
| C writes the feed and post JSON into the send buffer | 40,800 |
| Strings 16 bytes at a time, integers two digits at a time | 39,300 |
| The feed tries the reverse order before sorting ids; header lookup line by line | 37,970 |
| `-O3 -march=native` and no hardening instrumentation for `db.c` | 33,770 |
| SQLite 3.54.1 (reads the row key inline in `sqlite3BtreeTableMoveto`; 3.53.4 called `sqlite3GetVarint` for every probe) | 31,490 |

The two steps since #494 do not change the instruction count; they were judged by cycles and by alternated runs:

| Step since #494 | Measured at 150,000 users | Before | After |
|---|---|---:|---:|
| Write window of 20 ms | cycles per request, whole machine | 84,500 | 81,100 |
| | server process, median of five alternated runs | 26.8 % | 25.0 % |
| | `pwrite` calls a second | 7,400 | 4,750 |
| | p95 / p99 | 2.9 / 9.3 ms | 10.5 / 20.7 ms |
| Page memory on 2 MB pages | user cycles per request, two runs each | 22,000 and 22,460 | 20,340 and 21,040 |
| | cycles spent walking page tables for data, per request | 1,530 | 1,130 |
| | server process, median of five alternated runs | 24.7 % | 24.4 % |

### Official result of the second version

PR #494 (commit `94d68a9`) was scored at **260,000 users**: 24,995 requests/s, p95 20.9 ms, p99 366.2 ms, no errors, 78 % CPU, 28 µs of CPU per request. Above that it failed by little: 270,000 at p99 1.08 s and 280,000 at p99 1.03 s. Those are the scorer's numbers. What those two failures are is under Known gaps.

### This version against the first, on our machines

Alternated runs, 60 s ramp, 5 minute hold, the first version (`8622e90`) and this one taking turns in the same hour:

| Users | Version | Result | p95 | p99 | VM CPU | Server process |
|---:|---|---|---:|---:|---:|---:|
| 200,000 | first | fail | 31 ms | 1,851 ms | 68.0 % | 45.3 % |
| 200,000 | this | pass | 16 ms | 47 ms | 56.7 % | 33.2 % |
| 200,000 | first | pass | 17 ms | 418 ms | 64.4 % | 42.1 % |
| 200,000 | this | pass | 15 ms | 26 ms | 56.7 % | 32.5 % |
| 235,000 | first | fail | 65 ms | 1,409 ms | 75.5 % | 52.1 % |
| 235,000 | this | fail | 21 ms | 1,040 ms | 65.1 % | 38.9 % |
| 235,000 | first | fail | 28 ms | 1,098 ms | 75.2 % | 52.2 % |
| 235,000 | this | pass | 21 ms | 328 ms | 65.2 % | 39.1 % |

This version: 200,000 two of two, 235,000 one of two. The first version: one of two and none of two. No errors in any run. We claim no more than that table; on these machines the 270,000 and 310,000 entries also pass 235,000 and fail 250,000.

**Beyond that table we make no level claim.** On our machines `go-iouring-davidefrageri` (270,000 official), `rust-iouring-kensac` (310,000 official) and this entry all pass 235,000 and fail 250,000, so the pair cannot rank entries above about 235,000. It has also turned out noisier at some hours than at others: the first version passed 200,000 three times one evening and failed it once of two the next morning, with several times as many host pauses in the failing hour.

**Correction, 2026-11-18.** Most of what made this pair harder than the scorer was ours. A wait loop left running on our side logged into the target every 33 seconds for a day; every second login ran Ubuntu's login banner script, which took about 0.7 s of the one CPU, and our sampler listed every socket near the end of each hold, another 0.7 s. Each of those cost a backlog of 14,000 to 17,000 requests at 235,000 users. So "235,000 one of two" in the tables above measured our tooling as much as the entry, and the level claim drawn from it was too low. With no logins during a run, the banner script moved aside and the box's periodic timers stopped, this version's rule holds 250,000 for 5 minutes (three of three) and 270,000 (one of one; seven of twelve 2-minute probes) and fails 290,000 (none of eight probes) on the same machines. The scorer put it at 280,000. `rust-iouring-kensac` (310,000 official) still fails 270,000 here, so the pair now follows the scorer for this entry and not for every entry.

### Official result of the third version

PR #498 (commit `5fecd54`) was scored at **280,000 users**: 26,707 requests/s, p95 32.0 ms, p99 803.8 ms, no errors, 80 % CPU, 27 µs of CPU per request. That is the scorer's number.

### This version against the third

Three things are new, and each was kept on a counter of its own, measured on the process at 150,000 users on the same pair of machines, in alternated runs:

| Change | Counter, per request | Without | With |
|---|---|---:|---:|
| JSON writers (integers, strings with nothing to escape) | user-space instructions (two alternated pairs) | 31,120 and 31,265 | 30,600 and 30,455 |
| Profile-guided build of the C side | user-space instructions (two alternated pairs) | 30,775 and 30,845 | 28,708 and 28,315 |
| | user-space cycles (two alternated pairs) | 22,700 and 21,700 | 21,100 and 21,150 |
| A look at every waiting connection before handling any | stall cycles with a cache miss outstanding, in the completion handler and the new loop together | 569 and 618 | 252 and 250 |

Whole-VM CPU and latency did not decide any of them: identical runs on this pair differ by more than these changes are worth (process cycles per request moved between 48,500 and 52,600 across one afternoon of baseline runs). The same goes for the level. At 290,000 users, two-minute probes, the two versions taking turns, nothing attached to the server:

| Version | Passed | Median p99 | Requests over 1 s | Server process |
|---|---:|---:|---:|---:|
| Third (`5fecd54`) | 0 of 6 (0 of 20 over two days) | 1,425 ms | 4.0 % | 57.5 % |
| This | 3 of 6 | 809 ms | 1.8 % | 51.6 % |

The three passes were at p99 446 to 504 ms; the three failures at 1,114 to 2,687 ms. Whether a probe passes depends on whether one of the pile-ups described under Known gaps falls into it: every probe of this version from 10:57 to 11:11 passed and one of four after 11:20 did. 

Five-minute holds, the same afternoon, same machines:

| Users | Version | Result | p95 | p99 | Requests over 1 s | Server process |
|---:|---|---|---:|---:|---:|---:|
| 290,000 | this | pass | 32 ms | 762 ms | 0.92 % | 49.2 % |
| 290,000 | this | pass | 29 ms | 664 ms | 0.80 % | 49.4 % |
| 290,000 | this | pass | 30 ms | 942 ms | 0.99 % | 49.4 % |
| 280,000 | this | fail | 32 ms | 1,442 ms | 1.59 % | 49.1 % |
| 280,000 | third | fail | 623 ms | 4,456 ms | 4.68 % | 56.5 % |
| 280,000 | this | pass | 26 ms | 434 ms | 0.32 % | 47.6 % |
| 280,000 | third | fail | 1,294 ms | 6,357 ms | 6.60 % | 56.9 % |

So on this pair, this version held 290,000 three times of three, each time with little room (0.8 to 1.0 % of requests over a second against a limit of 1 %), and 280,000 once of two; the failed hold had one pile-up of 86,000 requests. The third version failed 280,000 twice here, which is the level it holds on the scorer: the pair is harder than the scorer at these levels, and what the table shows is the difference between the two versions, not a level. No errors in any run.

The build takes about 130 seconds on this box (72 for the plain build): the C side is compiled twice with a training run in between.

### Official result of this version before its last commit

PR #523 (commit `a0a8a72`) was scored at **266,000 users**: 28,139 requests/s, p95 21.1 ms, p99 191.8 ms, no errors, 72 % CPU, 26 µs of CPU per request, 1,348 MB. The scorer's search: 320,000 failed (p99 3.83 s), 240,000 passed, a probe at 280,000 passed, 300,000 and 290,000 failed (p99 1.40 s and 1.02 s), and the hold at 280,000 failed at p99 1.07 s. That is below the third version's 280,000. The counters above are lower than the third version's and the scorer's own figures agree (26 µs per request against 27, 72 % CPU against 80 % at a higher request rate), but the level did not follow: at 280,000 and 290,000 this version and the third both sit where one pile-up in the hold decides the run, and our pair's three holds at 290,000 did not carry over to the scorer. The table above shows a difference between two versions on one pair of machines; it was not a level, and here it was not even a lower bound.

### The last commit: the program's code on a 2 MB page

One change on top of `a0a8a72`, kept on its counter. At 150,000 users, two alternated pairs and a third run of the build with the change, counted on the server process, per request:

| Counter, per request | `a0a8a72` | With the code on a 2 MB page |
|---|---:|---:|
| user-space instruction-TLB walk cycles | 303 and 305 | 207, 205 and 210 |
| user-space cycles | 19,128 and 18,899 | 18,907, 18,733 and 19,491 |
| user-space instructions | 28,080 and 27,946 | 28,087, 27,970 and 28,069 |
| peak memory of the server's cgroup | 376 and 372 MB | 376, 370 and 373 MB |

The walk counter is the measurement: about 100 cycles per request less, the same in every run. User cycles as a whole do not show it: they were lower in both pairs and higher in the third run, because they differ by several hundred between identical runs. So the claim is the 100 walk cycles and nothing more. Memory does not change: 2 MB of anonymous memory replace 2 MB of file pages. Startup to listening took 15 to 34 ms against 11 to 46 ms (seven starts each). The counters were taken on builds of the same code files as this commit; a profile-guided build is not byte-identical from one run of `build.sh` to the next, so they are not the same bytes as any one later build. This is half a percent of the user-space cycles of a request and about a fifth of a percent of all cycles; we do not expect it to move the level and make no claim about one.


### Official result with the code on a 2 MB page

PR #523 at commit `a3c80ab` was scored at **280,000 users**: 26,714 requests/s, p95 25.4 ms, p99 811.9 ms, no errors, 76 % CPU, 26 µs of CPU per request, 740 MB. 290,000 failed its probe at p99 1.12 s. That equals the third version's 280,000. `a0a8a72` and `a3c80ab` differ by about 100 cycles per request and were scored 14,000 users apart on consecutive days, so we read that difference as what one hold at this level varies by, not as the effect of the commit.

### This commit: six changes, each on a counter of its own

All measured on the same pair of machines, in alternated runs, on the server process or the box's own counters.

| Change | Counter | Without | With |
|---|---|---:|---:|
| A token's three claims read in one pass, UTF-8 checked only where a byte is not ASCII | instructions per token check (benchmark on the box) | 18,480 | 13,593 |
| | user-space instructions per request at 150,000 | 27,842 and 28,054 | 27,598 and 27,729 |
| Collecting a ring's completions without a timer | timers armed per second by the process at 150,000 | 2,762 and 2,800 | 1,235 and 1,205 |
| | time inside the collect calls, per second, at 150,000 (median of twelve 10 s windows; one binary switched between the two) | 22.0 ms | 16.5 ms |
| | the same at 250,000, windows without a backlog | 42 to 46 ms | 37 to 40 ms |
| A commit's WAL frames in one write, on `unix-excl` | `pwrite` calls per second, all threads, at 150,000 | 4,975 and 4,876 | 913 and 919 |
| | `fcntl` calls per second | 1,703 and 1,740 | 0 and 0 |
| | kernel cycles per request inside `pwrite` and `fcntl` (profile) | 1,353 and 1,289 | 766 and 708 |
| SQLite's page cache at 32 MB instead of 16 | page-cache misses per second on the loop's connection (SQLite's own counter) at 150,000 | 306 and 303 | 0 and 0 |
| | the same at 250,000 | 615 | 0 |
| | user-space data-TLB walk cycles per request at 150,000, 32 MB against 16 MB, block fully on 2 MB pages | 773 | 743 |
| | the same with only 8 MB of the block on 2 MB pages (two pairs) | 843 and 1,050 | 1,050 and 1,074 |
| The cache block touched from the top down | user-space data-TLB walk cycles per request at 150,000 with 8 or 16 MB of the block on 2 MB pages, at the bottom against at the top (32 MB cache; 16 MB cache) | 1,124 and 1,037; 1,078 and 875 | 1,050 and 924; 843 and 789 |
| No bare ACK for a new connection's first request | bare ACKs sent per new connection, in a ramp of 4,830 new connections a second | 1.15 and 1.15 | 0.17 and 0.14 |
| | share of the box busy over that ramp | 53.2 and 51.9 % | 50.1 and 50.1 % |
| All six together, against `a3c80ab` | kernel instructions per request, whole process, at 150,000 (five alternated pairs) | 20,640 to 21,742 | 18,471 to 20,312 |

How to read the second item: its evidence is the timer count and a clock around the calls, not cycles. Most of what the call wasted was the virtual machine leaving to its host to program the timer, and a guest's cycle counter does not count time spent there; the process's kernel cycles per request differ by 2,000 between identical runs and showed nothing. For the third item the whole-process figure shows nothing either (`go-iouring-davidefrageri` reports the same for theirs); the two system calls' own slices of a profile do. The fourth is worth about 150 cycles per request by arithmetic (a miss is a 4 KB read on the loop thread) and was not measured in cycles; it also removes the only file reads the loop made between commits. For the larger cache on a box with few 2 MB pages, and for the touch order, the walk counter is the evidence and it is coarse: the same configuration measured twice (16 MB cache, 8 MB of the block on 2 MB pages at the top) gave 843 and 1,050. The larger cache was 207 worse in one such pair and 24 worse in the other, and 30 better with the block fully on 2 MB pages; we keep it for the misses it removes. The touch order was better in all four pairs, by 74 to 235, and costs nothing. The sixth changes nothing once connections exist: 0.105 bare ACKs per data segment in the hold either way.

By these counters the six together are about 0.6 µs of the 26 µs a request costs at 290,000 users (0.05, 0.2, 0.27 and 0.06 for the first four; the last two are not per request). The whole-process row agrees in direction: in five alternated pairs at 150,000 this commit ran 1,400 to 2,500 fewer kernel instructions per request than `a3c80ab`, every time. In kernel cycles the two pairs of the second night show 3,400 and 4,100 fewer; the three pairs taken in the first two and a half hours after the box had been rebooted show 400 to 1,600 fewer, because this commit's kernel cycles read high in that time (28,600 to 33,500 against 25,600 to 28,700 before the reboot and 27,000 after it), while `a3c80ab` read the same before and after (31,200 and 33,100 before; 30,300 to 33,600 after). We have no explanation for those first hours.

**We could not show any of this at 290,000, and make no claim about a level.** Thirty-four two-minute probes at 290,000 on our pair of machines, over two days, with a load generator whose garbage collector was off: builds of this commit, of its parts and of `a3c80ab` are indistinguishable in them. Before the box was rebooted, 7 of 18 passed (this commit's code 2 of 4 in the one alternated set, `a3c80ab` 1 of 5). After the reboot we ran two alternated sets of four runs each: none of the sixteen passed. In the first set, 25 to 65 minutes after boot, the box cost about 16 % more network-interrupt time per packet even in seconds where nothing was late (p99 2.0 to 7.2 s for both builds). In the second, eight hours after boot, quiet seconds and the stalls in the hold read exactly as before the reboot, and the runs failed at the end of the ramp instead (its last 20 seconds and the 20 after them): p99 1.2 to 4.3 s for `a3c80ab` and 1.4 to 2.9 s for this commit, p95 35 to 135 ms. Before the reboot the ramp's end went wrong in half the runs and every run where it did failed; since the reboot it has gone wrong in 15 of 16. We do not know whether that is the reboot or the hour of day (the earlier runs were all at 17 to 18 UTC, the later ones at 20 and at 3 to 4 UTC). What decides a run at this level on our machines is the end of the ramp first and the stall described under Known gaps second, for every version so far; none of the six changes touches either.

One open point is recorded rather than hidden. In the first eight runs with the WAL layer, at 150,000 users, four had a request waiting over 300 ms at the end of the ramp, against none of thirteen runs without it. One of the four coincided with a 0.85 s pause of our load generator. The layer's own calls were then timed (no WAL write over 10 ms, no commit over 16 ms, in fifteen runs with and without it), six further runs with it showed nothing, and at 290,000 such waits occur in every run of both builds, with sizes that overlap. We found no mechanism and could not reproduce it, and kept the layer.

## What is new in this entry

Only what we did not find in another entry's README, and measured ourselves.

- **The delayed-acknowledgement spiral, measured.** `objc-uring-delneg` (#326) and `fsharp-uring-delneg` (#398) describe the cause and the cure (a budget per pass, the backlog newest first). What we add is the measurement on a second machine and the budget comparison. At 200,000 users with a 50 ms stall forced every 30 s, the server spent 34 of 159 seconds in the state where the client's kernel sends more than 1,000 extra acknowledgements a second, and p99 was 852 ms. With a budget of 256 answers per pass: 7 seconds and 59 ms (p99 from our first load generator, see the last point). A budget of 128 gave 11 seconds and 512 gave 15. Without the budget the same build passed or failed 200,000 from run to run.
- **A token check that allocates nothing, with the standard library as the judge.** The fast path handles the tokens it can read exactly; anything else goes to a plain standard-library version. A test feeds 78 hostile tokens to both and requires the same answer. In a 5 minute run at 200,000 users the garbage collector ran 16 times before and not once after, and those collections had been the trigger of small latency bursts.
- **Authors in key order, repeats skipped.** Reading the feed's posts in ascending id order is `rust-iouring-kensac`'s plan (#264). We do the same for the 20 authors, and an author who wrote several of the 20 posts is read once.
- **A calibration of the rig against a scored entry.** `go-iouring-davidefrageri` (#354, official 270,000) run on the same two machines the same night: numbers under Measured. It says how far our numbers are from the scorer's, which our own runs alone could not.
- **Host pauses on shared-CPU guests.** Both rented machines are stopped by their hosts for 50 to 200 ms now and then, and it does not show as steal time. Three observers caught the same pause in the same second: a 100-per-second ping from the other machine, a 10 ms heartbeat process on the target, and the server's own longest `io_uring_enter`. A pause on the load generator stops every user at once and looks like a server stall in the results. Later measurement: the larger episodes are not stops. A probe that reads one word from random pages of a 400 MB block shows, on an idle guest, the cost of touching a page left alone for about 10 s rising from 100 ns to 1,500 ns for 30 to 40 seconds and falling back as the pages are touched again: the host has taken the guest's cold memory back, and each first touch pays for its return. Under load the probe shows such an event about once a minute (30 in 42 minutes), and every backlog above 15,000 requests in those runs falls on one. A server with 250,000 mostly idle connections touches cold kernel memory on every incoming segment, so it is about the most exposed guest there can be. Steal time shows none of it.
- **A load generator's garbage collector can fail a server.** Our first generator's collections stalled all its users for up to seconds and inflated p99; a second version with pooled buffers was checked against `go-nethttp-tbsvttr`'s official score (#18: passes 53,000, fails 60,000). Every level result under Measured is from the second version. The before-and-after rows for the earlier changes (checkpoint cycles, B-tree reads, the answer budget, the copier's priority) were taken with the first, so their p99 values are less certain than their CPU and counter values.

- **Instructions per request as the yardstick.** Whole-VM CPU on a shared host differs by up to four points between identical runs, and most single changes are worth one or two. User-space instructions per request, counted on the process, repeat within about one percent and do not move when the host pauses the guest. Every step in the table above was decided by that number in one four-minute run. A second counter, cycles per instruction by function, showed where the remaining gap is not instruction count but memory: the table seek and the page cache.
- **A Go build trap with an included C file.** `db.c` includes `sqlite/sqlite3.c`. Go decides whether to compile `db.c` again from `db.c` and the flags, not from the file it includes, and keeps an existing binary whose build id matches. After changing the SQLite pin, `build.sh` kept producing a binary with the old SQLite inside, even after `go clean -cache`. A fresh clone is not affected. (Since this version `build.sh` compiles `db.c` itself, so the trap no longer applies to us; it stays here for others who let cgo compile a file that includes another.)

- **Huge pages for SQLite's own cache, without a cache of our own.** Entries that put page memory on 2 MB pages do it inside a page cache they wrote. `SQLITE_CONFIG_PAGECACHE` gets the same effect with SQLite's cache untouched: one prefaulted block, 20 lines. A cycle profile had shown our instructions running slower than `go-iouring-davidefrageri`'s (1.34 per cycle against 1.63) with the wait in the table seek and the page fetch; this took 1,500 of those cycles per request. Our first attempt showed what to avoid: a cache that takes fresh huge-page memory during the run spends its gain, and more, on the kernel clearing 2 MB at a time.

### Tried and not kept

Each alone, against the version before it, on the same machines in the same half hour.

| Change | Measured | With it | Without it |
|---|---|---|---|
| `GOMAXPROCS=1`, async preemption off, SQLite at `-O3`, `GOAMD64=v3` | VM CPU at 150,000, three runs | 53.7, 52.4, 53.9 % | 55.4, 54.6, 52.5 % |
| Batching window of 4 ms instead of 1 ms | VM CPU at 150,000, mean of three | 52.0 % (p95 10 ms) | 52.5 % (p95 5 ms) |
| Database file grown in 64 MB steps | seconds in the delayed-ACK state, 200,000 for 5 min | 16 and 22 | 21 |
| No write hold while the copier is still at work | 225,000 users | no difference | |
| Our own page cache through `SQLITE_CONFIG_PCACHE2` (an array indexed by page number, clock eviction) | VM CPU at 150,000, three runs in alternation | 47.5, 48.5, 47.6 % | 49.7, 45.2, 46.4 % |
| The same page cache, on this version | user cycles per request | 23,490 | 23,330 |
| One `io_uring_enter` per ring per pass (the other rings submit and collect in one call before ring 0 waits) | 150,000, five runs in alternation, medians | process 31.4 %, p95 5.2 ms | process 29.2 %, p95 3.0 ms |
| One pass over the request head instead of four searches | user instructions per request | 31,390 | 31,370 to 31,400 |
| Our page cache with its memory on 2 MB pages, taking fresh memory during the run | one run at 150,000 | failed, p99 1.3 s; 9 % of all CPU clearing pages | |
| Answers sent before the end of a pass, in groups of 4, 8 or 32, in passes without a write (hoping to find the socket still in the CPU's cache) | process cycles per request at 150,000 | 74,600 (groups of 4), 67,000 (8), 49,600 (32) | 48,500 to 52,600; the socket was as cold either way |
| Receive pool of 2,048 buffers on 2 MB pages instead of 8,192 on 4 KB pages | share of process cycles in receiving; times the pool ran dry at 250,000 | 9.0 to 9.1 %; 4,633 in one run | 8.7 to 8.9 %; never |
| Connection records on 2 MB pages | user-space data-TLB walk cycles per request | 669 | 729 (two runs: 803 and 655) |
| On the listening sockets: reno instead of cubic; send and receive buffers fixed at 131,072 and 8,192 bytes | kernel instructions per request at 150,000, medians of eight alternated runs each | 20,595 (reno), 20,800 (fixed buffers) | 20,638; counted by function, cubic costs 38 instructions per request and reno 5 |
| Refusing a request that has waited 750 ms with a 503, under a cap of 0.7 % of requests, as a running share and as a token bucket | two-minute probes passed at 290,000 | 4 of 12 | 2 of 12; one pile-up is 50,000 to 60,000 requests, the size of the whole 1 % budget, and the thresholds would have been the load test's |

The page cache agrees with `go-iouring-davidefrageri`'s README, which found SQLite's own cache as fast as a direct-indexed one on the scoring box. A profile had made it look promising: `pcache1Fetch` takes 2,640 cycles per request for 1,810 instructions. With our cache in its place those cycles moved into the next function that touches the page; the wait is for the page's memory, not for the lookup. At 150,000 users the 16 MB cache misses about 470 times a second, one request in thirty. The single-enter loop made 1,200 fewer system calls a second and was slower all the same. `unix-excl` did remove the locking calls; what they cost is below what the counters resolve. The one-pass head scan lost to the code it replaced because `IndexByte` has a fixed cost per call, and one call per line pays it eight times. The failed huge-page run is why the block is now fixed in size and touched at startup.

Considered and not built:

- **No completion for a successful send** (`IOSQE_CQE_SKIP_SUCCESS`). A send buffer goes back to the pool when its send completes. Without a completion we cannot tell a send that finished at once from one the kernel has queued and is still reading from the buffer. It needs buffers owned by connections, which is a different design.
- **A read transaction kept open across passes that only read.** A pass serves about 22 requests, so what a `BEGIN` and `COMMIT` cost per pass is 20 to 40 instructions per request, and the change would touch the checkpoint handshake.
- **Zero-copy sends.** The copy and the clearing of fresh pages on the send side measured about 1,460 cycles per request; page pinning and a second completion per send would take much of that back, and a server started from a systemd unit may lock 8 MB of memory, which rules out registered buffers.
- **The feed's author lookups interleaved, with a prefetch between them.** The 20 lookups are a quarter of user-space cycles (5,140 per request) but run at 1.87 instructions per cycle; only about 400 of those cycles wait for memory. The waiting is elsewhere: in the lookup of the one post of `GET /posts/:id` and in the write path.

## Known gaps

- **Two seconds without an answer, and a hump at one second.** This is what fails our 235,000 runs. It is in every version so far, this one included. (On the scorer #494 failed 270,000 with p99 1.08 s and 280,000 with p99 1.03 s; whether that is the same thing we cannot tell from those two numbers.) (1) Now and then one `io_uring_enter` that collects completions without sleeping takes 0.6 to 2 s and returns 16,000 to 27,000 requests, while nothing is answered. In the worst one measured the thread had 0.8 s of CPU in 2 s and was switched out 363 times, never asleep and with no major fault, so something else on the one CPU had the rest. Sampling every task on the box named it: in those runs it was our own tooling (the login banner script, see the correction under Measured), not network receive, our checkpoint threads or the host. With the tooling gone, the long calls that remain fall on the host's cold-memory events (see Host pauses); one such call during a ramp took 1.94 s. A short hiccup (a write hold, a 30 ms host pause) is enough to start one. We first blamed the request for the whole completion queue (`min_complete` = 65,536 with a zero timeout) and tried 256: the long calls remained (2.0 s, 0.9 s and 0.6 s in three of five further 5-minute runs), each still returning thousands of completions, so that was not the cause and the change is not in. (2) Requests that then have to wait are answered newest first, and the oldest get a share of each pass only once they have waited one second, which is also the limit the test applies: so nearly every request that waits at all takes just over a second. Serving a waiting request ahead of new ones from 650 ms passed forced stalls of 100, 200 and 500 ms with a tenth to a third of the requests over one second, but in plain runs it passed two of three and its failure was the worst of the night (a backlog of 89,000), so it is not in either. Later alternated pairs at 270,000 to 290,000 (a share of 32 or 64 answers per pass for requests past 650 ms, the plain rule's share from 750 ms instead of one second, 2,048 answers per pass) did not pass a level that the plain rule fails; at 290,000 the reads alone exceed the one percent allowed past a second in every run, so the order of answers is not what is missing there.

- User-space cycles per request were about a third above `go-iouring-davidefrageri` in #494 (23,430 against 17,200) with nearly the same instruction count: our instructions ran slower. A cycle profile puts it in `sqlite3BtreeTableMoveto` (a binary search over pages that are not in the CPU's cache, 22 % of our cache misses) and in fetching pages. Theirs reads keys from the page below the cursor functions, which we do not do.
- The Go side (request parsing, the token's JSON and base64) is about 3,100 instructions per request against about 900 in `go-iouring-davidefrageri`.
- User-space cycles per request are now 20,700 against 17,200; what is left is mostly the table seek's cache misses.
- Under memory pressure the kernel reclaims the file cache in lumps (the WAL's pages are written once and never read again); we do nothing about that yet.
- New posts still go through the standard library's JSON decoder, and writes are plain SQL.
- The loop logs a statistics line every 10 seconds.

## Credits

"Idea" means we read it in that entry's README or code and wrote our own; "value" means a number they measured and we use; "code" means our code has their structure.

| What | From | What we took | What we changed |
|---|---|---|---|
| Multi-ring loop: wait on ring 0 then visit the others, the margin before a table is full (`min(max(slots/8, 64), 1024, slots/4)`), the tag layout, 8 listeners, up to 16 rings, 256 completions with a floor of 32 | `go-iouring-davidefrageri` #354 | code: about 60 to 70 lines of `main.go` follow its `server.go` closely (same structure, our names) | a full ring's accepts are cancelled in the accept handler; the starved-connection list; error handling in `enter` |
| Polling with a large `min_complete` and a zero timeout; buffer-ring layout; the retry for Ubuntu's registration quirk | `go-iouring-davidefrageri` #354, also documented by `go-iouring-trungdlp` #343 | code: about 50 to 70 lines of `uring.go` follow its `uring.go` closely | since this version the timed call is the last resort after up to three calls that ask for none (see Design); the rest unchanged |
| A commit's WAL frames in one write: a file layer over `unix-excl` that buffers continuing writes and writes them when the commit frame's page arrives; the commit-frame test on bytes 4 to 7 of the frame header | `go-iouring-davidefrageri` #354 (`shim.c`), which names `c-iouring-pavlyhalim` #245 for the pattern | code: the layer in `db.c` follows its structure closely | the buffer is allocated when the WAL file is opened; write calls and frames are counted for the stats line; a test hashes what SQLite asks for against what reaches the file |
| `unix-excl` for all connections | `rust-iouring-kensac` #264, `go-iouring-davidefrageri` #354 | idea | none |
| `TCP_QUICKACK` off on the listening sockets, inherited by accepted ones | `fsharp-uring-delneg` #437 | idea | measured here by bare ACKs per new connection |
| Answer budget per pass, backlog newest first, and the reason (delayed acknowledgement) | `objc-uring-delneg` #326, `fsharp-uring-delneg` #398; `go-iouring-trungdlp` #343 also caps answers per pass | idea | the value 256 (measured against 128 and 512); requests parsed and verified on arrival; a one second bound for the oldest |
| Write hold: writes stop for up to 250 ms while reads go on, so a checkpoint can finish and the WAL restart | `rust-iouring-kensac` #264, `swift-iouring-akbashev` #376, `fsharp-uring-delneg` #398, `go-iouring-davidefrageri` #354 | idea, and the value 250 ms | two threads instead of one, driven by SQLite's WAL hook; a second thread at normal priority finishes the hold; thresholds of 12,288 and 20,480 pages and a 384 page tail |
| Early copying: up to four `PASSIVE` rounds, stop when the tail is short, sync the database file ourselves on a separate descriptor before the hold | `go-iouring-davidefrageri` #354, `swift-iouring-akbashev` #376 | idea, the value 4, and about 15 lines of `db.c` that follow theirs | the thresholds; the hand-over to the finishing thread |
| Copier at nice 5 | `go-iouring-davidefrageri` #354, `swift-iouring-akbashev` #376 | value | none |
| Including the amalgamation in the shim's C file to reach the B-tree cursor functions | `go-iouring-davidefrageri` #354 and others | idea | reads stay with the cursor functions |
| Like counts for a range of posts in one index walk | `c-iouring-pavlyhalim` #245 (`bt_range_counts`) | code: the signature and about 5 lines are as there | the seek reuses one key; an entry below the range is an error; a separate single-post count |
| The span limit of 512 for that walk | `rust-iouring-kensac` #264 | value | none |
| Feed posts looked up in ascending id order for `TableMoveto`'s next-row step | `rust-iouring-kensac` #264 | idea | authors in id order too, repeats skipped; a per-post fallback |
| Record decoding: the names `col_t` and `rec_cols` | `go-iouring-davidefrageri` #354 | names | the body uses SQLite's `getVarint32` and `sqlite3VdbeSerialTypeLen` |
| HMAC from saved hash states of the key pads | `go-iouring-trungdlp` #343 | idea | done through the standard library's state marshalling; a standard-library fallback and a test |
| SQLite 3.54.1 and its checksum | `rust-iouring-kensac` #264 | value | none (the first version used `go-nethttp-tbsvttr` #18's pin of 3.53.4) |
| Compiler flags for the C file: `-O3 -march=native`, no stack protector, fortify, stack-clash or CET instrumentation | `go-iouring-davidefrageri` #354 (`build.sh`), `rust-iouring-kensac` #264 (`build.rs`) | the flag set | given to gcc directly by `build.sh`, which compiles the file itself |
| A profile-guided build with the profile recorded during the build on a scratch database | ruling of 2026-11-15 (rule 10); `c-iouring-pavlyhalim` #245, `c-iouring-jowinjestine` and others have one in their `build.sh` | idea | the training runs inside the server's own request functions, in process, in the proportions of the public load script; the object is linked as `db.syso` because cgo compiles under a new name each time; a plain build on any failure |
| C writes the response from the rows | `go-iouring-davidefrageri` #354 | idea | our own writer; Go still writes the header and one call is made per request, not per pass |
| A write window: writes committed at most once per 20 ms while reads are answered at once | `fsharp-uring-delneg` #398 | idea and the value 20 ms | a write arriving with the window open is not delayed; the window shares its queue with the write hold |
| Huge pages for the page cache's memory | `go-iouring-davidefrageri` #354 does it inside its own page cache | idea | SQLite's own cache, given a prefaulted block through `SQLITE_CONFIG_PAGECACHE` |
| Trying the reverse of the feed order before sorting the ids | `rust-iouring-kensac` #264 (`src/db.rs`) | idea | none |
