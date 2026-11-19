# F# / io_uring with a C reader that proves the feed order

This server speaks HTTP/1.1 directly on port 80 using ordinary Linux TCP. One F# thread runs the network loop, verifies requests and owns the SQLite transaction. On a feed or point read it calls C once. The C reader walks the current SQLite snapshot and writes the JSON body directly into the same pinned array that F# will send. The service uses .NET 11, GCC and the upstream SQLite 3.54.1 amalgamation, built from source on the host. [Build](build.sh) · [F# dispatch](src/Db.fs) · [Native reader](native/rank.c)

## Why combine these two implementations?

At 350,000 local users, Delneg’s F# network loop spent 0.97–1.39 µs less in Linux receive processing per completed request than our submitted C build under the same workload. It passed twice at 319 and 266 ms p99. Our C server passed at 952 and 699 ms. Both received about 2.2 TCP segments per request. We kept Delneg’s network and commit code, then used our independently developed feed algorithm to reduce work per read. The organizer scored Delneg’s original build at 300,000, our earlier C build in #403 at 280,000 and the C update in #428 at 257,000. This combined build has no official score. [Delneg’s source](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/8921ed960228f8d3fcd0464999aeb997eead2174/submissions/fsharp-uring-delneg) · [Our C reader](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/0142ba23cd06969938941dac17e424ca7f3357cd/submissions/c-uring-willwang) · [Official #428 result](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/428#issuecomment-6867008225)

## Reading and rendering

- **Prove the feed with one index boundary.** Read the 20 greatest rowids from the posts table. Look up rank 20 in the live chronological index. If the least recent candidate occupies that rank, the other 19 candidates occupy all 19 higher ranks. If timestamps arrive out of order, sort those 20 within the request. If the proof fails, use the prepared SQL query for the full join and ordering. [Feed proof and SQL fallback](native/rank.c) · [Prepared queries](src/Db.fs)
- **Visit fewer pages for likes and authors.** The rightmost likes-index key proves that posts with higher IDs have zero likes in this snapshot. Nearby candidate IDs share an index walk. Large gaps use a fresh seek. A range of equal keys on one leaf can be counted from their positions. For authors and point lookups, the reader estimates the row slot from the actual SQLite page and checks its rowid. Sparse keys retain binary search. [SQLite record and index reader](native/rank.c)
- **One managed-to-native call per ordinary read.** C uses the connection that F# already opened. It validates the schema, index order, collation and encoding, checks schema versions during reads, and releases every cursor and borrowed page before returning. Overflow records, missing authors, unsupported values or a changed schema use the prepared SQL path in the request’s transaction. [Native context and guards](native/rank.c) · [F# ownership](src/Db.fs)
- **Produce the wire body in place.** F# passes the free tail of a pinned response array. The native writer checks for JSON escapes as it copies UTF-8, formats signed integers directly and handles short tails inside the supplied buffer. If the result needs more room, F# discards the provisional length, grows its own buffer and uses SQL. The HTTP header occupies the reserved bytes ahead of the body. [Bounded native encoder](native/bounded-json.h) · [Managed buffers](src/Json.fs)

## Networking and durable writes

- Eight reuse-port listeners distribute incoming connections. The main thread drives multiple io_uring instances with sparse fixed-file tables under the deployment process’s descriptor limit. Multishot receives use provided buffers. An idle client holds no request buffer. After buffer exhaustion the loop re-arms queued sockets in bounded batches. It preserves request order within each connection and gives old requests a growing share when a backlog persists. [Connection lifecycle and fairness](src/Server.fs) · [io_uring operations](src/Ring.fs)
- Eligible reads share the issuer’s SQLite transaction. Writes join a group commit, and a successful reply waits for the commit to finish. A prepared write queue lets the issuer continue serving reads while writes wait. The WAL runs with `synchronous=NORMAL`. A low-priority thread checkpoints through a second connection, while the VFS combines adjacent WAL frame writes up to SQLite’s publication boundary. [Transactions and checkpoints](src/Db.fs) · [WAL writes](src/Vfs.fs)
- SQLite owns the page cache. A public PCACHE2 adapter avoids a full-cache scan after a temporary key used during B-tree page reordering. Every authenticated request independently checks its signature, algorithm and expiry. Only the secret’s reusable SHA-256 key schedule is prepared in advance. [Page cache](src/Pcache.fs) · [JWT verification](src/Jwt.fs)

C lets the reader compile beside SQLite’s internal page and cursor functions and borrow text for just one call. F# owns the output allocation, HTTP scheduling and durable write boundaries. GCC emits SIMD instructions from the bounded copy code. The measured comparison changes the read algorithm and its interface as a unit. [Source attribution and license](native/CREDITS.md) · [Native build flags](build.sh)

## Measurements

Eight alternating pairs replayed 2,097,152 requests through the original and combined database handlers. They used the route proportions captured in the two 350,000-user F# runs, real SQLite writes and the returned JSON. All eight pairs used less issuer CPU, with a median saving of **1.93 µs per mixed request** and a range of 1.61–2.03 µs. The median batch p99 was 0.676 times the original. The replay used deterministic post text and short usernames for the 0.90% of operations that created a post. TCP, the live request queue and background checkpoint contention are measured separately in the stock runs.

The release passed all 42 challenge HTTP checks. On the canonical seed and 13 additional database layouts, both implementations produced 516 HTTP status/body pairs each matching the independent SQLite oracle byte for byte. Further checks covered a 510,541-byte escaped response, a pinned older snapshot, independent commits, rollback, schema replacement, a temporary shadow, bounded output canaries and acknowledged writes reopened after killing the process.

The scored configuration uses SQLite’s `unix-excl` VFS and owns the database for the process lifetime. Read-only passes may carry their transaction into the next batch. The HTTP test that wrote through a separate process used `VFS=unix CARRY=0`, allowing the writer to acquire the database and each new batch to observe its commit. The native snapshot test also verifies that a read transaction already in progress keeps its older view until it ends.

At **350,000 users**, four unmodified stock generators completed 13,155,655 requests with zero errors. All 59 hold samples had full occupancy. The combined server passed at **188 ms p95 and 828 ms p99**. The service was OOM-killed 2.847 seconds after the score cutoff, 0.229 seconds after the final generator exited. Scored memory peaked at 1,088 MiB within a 1,750 MiB limit, with zero memory stalls or page scans. At the later OOM Linux charged 1,123 MiB to sockets.

At **360,000 users**, the clean five-minute run passed at **186 ms p95 and 820 ms p99**, with 13,578,244 requests, zero failures, and exact connections on 59/59 checks. An earlier attempt at this load lost packets on four TAP interfaces and recorded only 58 synchronized checks. The replacement stayed alive until the harness stopped it 123.55 seconds after cutoff. At the last active drain sample, however, the server still counted **359,890 established sockets** after all client endpoints had gone away. The fixture observed 137,331 additional client-side drops by the separate client-exit snapshot. Their exact timing relative to cutoff is unknown. This run establishes a passing score and process survival. Full socket cleanup required the harness stop. [Scores, exclusions and shutdown counts](tests/local-results.json)

At **370,000 users**, all 13,658,362 HTTP requests succeeded. The run recorded 499 ms p95 and 8.09 s p99, with packet loss late in the hold. Before the earliest possible loss, at least 173,208 requests had already exceeded one second, above the full-run allowance of 136,583. That independently establishes a p99 miss. The registered 380,000- and 400,000-user attempts required a clean 370,000-user pass. The best F# competitor and this build both have a demonstrated 360,000-user stock pass. [Load ladder and per-attempt receipts](tests/local-results.json)

During 290 seconds at full occupancy the combined server used 13.86 µs of application CPU per completed request, compared with 14.52 and 14.38 µs for original F#. The Linux receive thread on the same core used 12.67 µs, compared with 11.25 and 10.94 µs. The whole core spent 26.58 µs per completion, compared with 25.85 and 25.43 µs. These measurements explain why the database replay alone overestimated the improvement on this TCP workload.

A fresh build from this submission passed all 42 stock checks and four native snapshot tests. Building the same checkout sources at the scored path reproduced all seven measured release artifacts byte for byte. In the ordinary Git checkout the SDK also embedded its Git revision and SourceLink metadata. Across those builds, all 658 managed method bodies matched and all 21,551 decoded native instructions matched after rebasing instruction-relative addresses.

### The top five on this local machine

Each entry used the same large-table TCP fixture with four stock generators. A qualified scoring window requires clean wire counters, exact dials, full sampled occupancy and a stock passing score during the ramp and five-minute hold. The next column additionally requires that the service stay alive until the harness asks it to stop. These are the highest **tested** passing loads. The final column compares entries at the same 350,000-user load. [Official leaderboard](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/f4485ff56545d0c12e2283aad651fce16497ec48/LEADERBOARD.md#L69-L80)

| Entry | Official | Local scoring window | Alive at harness stop | Local p99 at 350k |
| --- | ---: | ---: | ---: | ---: |
| Kensac, Rust | 310,000 | 310,000 (2/3) | 310,000 | 23,216 ms |
| Oh-meh, Mojo | 304,000 | 320,000 | 320,000 | 22,533 ms |
| Rkkautsar, Rust | 300,000 | 260,000 | 260,000 | 11,341 ms |
| Akbashev, Swift | 300,000 | 280,000 | 280,000 | 28,897 ms |
| Delneg, F# | 300,000 | 360,000 (1/1) | 350,000 (2/2) | 319, 266 ms |
| Our C update, #428 | 257,000 | 360,000 (1/2) | 345,000 | 952, 699 ms |
| This F#/C build | Unscored | 360,000 | 360,000 (drain time limit) | 828 ms |

Mojo also passed the stock HTTP thresholds at 325,000 with 702 connections missing on the first hold check. Delneg and #428 both met the stock thresholds and full occupancy at 360,000, then exhausted memory during disconnect. The organizer's server reports a Xeon Platinum 8260 vCPU and 1,967 MB RAM. The local machine reports AMD EPYC Genoa and runs the service and its Linux receive thread on the same core, with a 1,750 MiB application memory limit. Both faster and slower local results appear in the table, so converting local users to an official score with a single percentage would give misleading rankings.

All five peer builds passed the 42 organizer checks. Rkkautsar’s scored revision needed a startup compatibility adjustment on the local Ubuntu kernel. If registering the provided-buffer ring returned `EINVAL`, it retried during setup with the reserved field that Ubuntu 6.8.0-147 expects.

## Build and run

From the challenge checkout on Ubuntu 24.04, run the install step once as root. It installs the pinned .NET SDK 11.0.100 and the C compiler. The normal build verifies the SQLite download and publishes the ReadyToRun assembly. The server runs in the foreground as the deployment user.

```sh
entry=submissions/fsharp-uring-willwang
sudo bash "$entry/install.sh"
test -f seed/feed.db || bash seed/make-seed.sh
bash test/run.sh "$entry"
python3 "$entry/tests/rank_snapshot.py"
python3 submissions/c-uring-willwang/tests/differential.py --server "$entry/start.sh"
work=$(mktemp -d)
cp seed/feed.db "$work/feed.db"
SQLITE_PATH="$work/feed.db" JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 "$entry/start.sh"
```
