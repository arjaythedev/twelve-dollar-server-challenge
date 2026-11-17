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
- **C writes the response body.** For the feed and a single post, Go writes the response header into the send buffer with the `Content-Length` field left blank, C writes the JSON straight after it from SQLite's pages, and Go fills in the digits. The text is copied once on its way from the page to the socket. Strings are copied and tested for characters to escape 16 bytes at a time. If the body does not fit in what is left of the buffer, C writes nothing usable and reports it, and the request gets a 500; `json_test.go` checks the refusal.
- **Compiler flags for the one C file.** `db.c` and the SQLite inside it are built with `-O3 -march=native` and without the stack canary, checked `memcpy`, stack probes and branch-tracking markers that Ubuntu's gcc adds by default (`build.sh`). The server is built on the box it runs on. `build.sh` also names the pinned SQLite release in a flag, because Go does not look at an included C file when deciding whether to compile again.
- **Go runtime.** One locked OS thread, raw system calls for `io_uring_enter`, the periodic garbage collector off with a 64 MB ceiling. Everything the kernel reads or writes is in mmap'd memory outside the Go heap. Each request crosses into C once or twice.

## Rules

- **Rule 5, no caching.** Every request reads from SQLite. Nothing of ours is kept between requests: cursors are released after every request. SQLite's own page cache is set to 16 MB. Every token's HS256 signature and `exp` are verified on every request.
- **Rule 6, durability.** WAL with `synchronous=NORMAL`. A response is sent only after the commit that contains its write. A read in the same pass as a write can see that write before the commit, but its response waits for the same commit and is dropped if the commit fails.
- **Rule 7.** Nothing depends on the load test's shape. The batching wait, the checkpoint cycle and the write hold apply to any traffic.
- **Rule 11.** Nothing on the box is tuned. The process raises its own soft `RLIMIT_NOFILE` to the hard limit it was given, and lowers the priority of one of its own threads.
- **SQLite internals.** The source is not patched. The read path calls SQLite's B-tree cursor functions (allowed by the ruling of 2026-10-31) and builds the search key they take as an argument. It does not write cursor fields or any other private state, does not read pages itself, and does not use B-tree writes, a custom VFS or a custom page cache.

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

At 200,000 the process and its share of kernel memory peak at about 520 MB and about 900 MB is still available on the box; at 250,000 about 730 MB is.

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

| Per request, user space | #487 | this version | `go-iouring-davidefrageri` | `rust-iouring-kensac` |
|---|---:|---:|---:|---:|
| Instructions | 46,500 | 31,490 | 28,200 | 29,400 |
| Cycles | 29,000 | 23,430 | 17,200 | 21,100 |

Kernel instructions per request are the same for all four (21,000 to 24,000). Whole-VM CPU at 150,000 users, five runs of each version in alternation: median 49.0 % before, 46.2 % now; the server process 30.3 % before, 27.2 % now.

| Step | User instructions per request after it |
|---|---:|
| C writes the feed and post JSON into the send buffer | 40,800 |
| Strings 16 bytes at a time, integers two digits at a time | 39,300 |
| The feed tries the reverse order before sorting ids; header lookup line by line | 37,970 |
| `-O3 -march=native` and no hardening instrumentation for `db.c` | 33,770 |
| SQLite 3.54.1 (reads the row key inline in `sqlite3BtreeTableMoveto`; 3.53.4 called `sqlite3GetVarint` for every probe) | 31,490 |

**We make no level claim for this version.** On our machines `go-iouring-davidefrageri` (270,000 official), `rust-iouring-kensac` (310,000 official) and this entry all pass 235,000 and fail 250,000, so the pair cannot rank entries above about 235,000. It has also turned out noisier at some hours than at others: the first version passed 200,000 three times one evening and failed it once of two the next morning, with several times as many host pauses in the failing hour.

## What is new in this entry

Only what we did not find in another entry's README, and measured ourselves.

- **The delayed-acknowledgement spiral, measured.** `objc-uring-delneg` (#326) and `fsharp-uring-delneg` (#398) describe the cause and the cure (a budget per pass, the backlog newest first). What we add is the measurement on a second machine and the budget comparison. At 200,000 users with a 50 ms stall forced every 30 s, the server spent 34 of 159 seconds in the state where the client's kernel sends more than 1,000 extra acknowledgements a second, and p99 was 852 ms. With a budget of 256 answers per pass: 7 seconds and 59 ms (p99 from our first load generator, see the last point). A budget of 128 gave 11 seconds and 512 gave 15. Without the budget the same build passed or failed 200,000 from run to run.
- **A token check that allocates nothing, with the standard library as the judge.** The fast path handles the tokens it can read exactly; anything else goes to a plain standard-library version. A test feeds 78 hostile tokens to both and requires the same answer. In a 5 minute run at 200,000 users the garbage collector ran 16 times before and not once after, and those collections had been the trigger of small latency bursts.
- **Authors in key order, repeats skipped.** Reading the feed's posts in ascending id order is `rust-iouring-kensac`'s plan (#264). We do the same for the 20 authors, and an author who wrote several of the 20 posts is read once.
- **A calibration of the rig against a scored entry.** `go-iouring-davidefrageri` (#354, official 270,000) run on the same two machines the same night: numbers under Measured. It says how far our numbers are from the scorer's, which our own runs alone could not.
- **Host pauses on shared-CPU guests.** Both rented machines are stopped by their hosts for 50 to 200 ms now and then, and it does not show as steal time. Three observers caught the same pause in the same second: a 100-per-second ping from the other machine, a 10 ms heartbeat process on the target, and the server's own longest `io_uring_enter`. A pause on the load generator stops every user at once and looks like a server stall in the results.
- **A load generator's garbage collector can fail a server.** Our first generator's collections stalled all its users for up to seconds and inflated p99; a second version with pooled buffers was checked against `go-nethttp-tbsvttr`'s official score (#18: passes 53,000, fails 60,000). Every level result under Measured is from the second version. The before-and-after rows for the earlier changes (checkpoint cycles, B-tree reads, the answer budget, the copier's priority) were taken with the first, so their p99 values are less certain than their CPU and counter values.

- **Instructions per request as the yardstick.** Whole-VM CPU on a shared host differs by up to four points between identical runs, and most single changes are worth one or two. User-space instructions per request, counted on the process, repeat within about one percent and do not move when the host pauses the guest. Every step in the table above was decided by that number in one four-minute run. A second counter, cycles per instruction by function, showed where the remaining gap is not instruction count but memory: the table seek and the page cache.
- **A Go build trap with an included C file.** `db.c` includes `sqlite/sqlite3.c`. Go decides whether to compile `db.c` again from `db.c` and the flags, not from the file it includes, and keeps an existing binary whose build id matches. After changing the SQLite pin, `build.sh` kept producing a binary with the old SQLite inside, even after `go clean -cache`. A fresh clone is not affected. `build.sh` now passes the pinned release as a flag.

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

The page cache agrees with `go-iouring-davidefrageri`'s README, which found SQLite's own cache as fast as a direct-indexed one on the scoring box. A profile had made it look promising: `pcache1Fetch` takes 2,640 cycles per request for 1,810 instructions. With our cache in its place those cycles moved into the next function that touches the page; the wait is for the page's memory, not for the lookup. At 150,000 users the 16 MB cache misses about 470 times a second, one request in thirty. The single-enter loop made 1,200 fewer system calls a second and was slower all the same.

## Known gaps

- User-space cycles per request are still about a third above `go-iouring-davidefrageri` (23,430 against 17,200) with nearly the same instruction count: our instructions run slower. A cycle profile puts it in `sqlite3BtreeTableMoveto` (a binary search over pages that are not in the CPU's cache, 22 % of our cache misses) and in fetching pages. Theirs reads keys from the page below the cursor functions, which we do not do.
- The Go side (request parsing, the token's JSON and base64) is about 3,100 instructions per request against about 900 in `go-iouring-davidefrageri`.
- The WAL is written with two `pwrite` calls per frame (7,400 a second at 150,000 users); `unix-excl` and a write-combining VFS are not used yet.
- Near 235,000 users a host pause of 100 ms or so can open a backlog of 30,000 requests that takes seconds to drain, which is what failed one of the two runs there. With less CPU per request the same pause passes.
- Under memory pressure the kernel reclaims the file cache in lumps (the WAL's pages are written once and never read again); we do nothing about that yet.
- New posts still go through the standard library's JSON decoder, and writes are plain SQL.
- The loop logs a statistics line every 10 seconds.

## Credits

"Idea" means we read it in that entry's README or code and wrote our own; "value" means a number they measured and we use; "code" means our code has their structure.

| What | From | What we took | What we changed |
|---|---|---|---|
| Multi-ring loop: wait on ring 0 then visit the others, the margin before a table is full (`min(max(slots/8, 64), 1024, slots/4)`), the tag layout, 8 listeners, up to 16 rings, 256 completions with a floor of 32 | `go-iouring-davidefrageri` #354 | code: about 60 to 70 lines of `main.go` follow its `server.go` closely (same structure, our names) | a full ring's accepts are cancelled in the accept handler; the starved-connection list; error handling in `enter` |
| Polling with a large `min_complete` and a zero timeout; buffer-ring layout; the retry for Ubuntu's registration quirk | `go-iouring-davidefrageri` #354, also documented by `go-iouring-trungdlp` #343 | code: about 50 to 70 lines of `uring.go` follow its `uring.go` closely | nothing of substance |
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
| Compiler flags for the C file: `-O3 -march=native`, no stack protector, fortify, stack-clash or CET instrumentation, given through `CGO_CFLAGS` | `go-iouring-davidefrageri` #354 (`build.sh`), `rust-iouring-kensac` #264 (`build.rs`) | the flag set | `-DPINNED_SQLITE` added |
| C writes the response from the rows | `go-iouring-davidefrageri` #354 | idea | our own writer; Go still writes the header and one call is made per request, not per pass |
| Trying the reverse of the feed order before sorting the ids | `rust-iouring-kensac` #264 (`src/db.rs`) | idea | none |
