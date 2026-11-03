# Ideas

- [x] Read the feed through SQLite's own B-tree cursors, pager, WAL and cache, skipping the VM. Keep SQL for point reads.

- [x] Count feed likes in request-local ID order, scanning at most 16 unrelated entries before seeking across gaps.

- [x] Decode index fields from SQLite's local payload; retain the overflow fallback and consume views before any further B-tree call.

- [x] Share 1,024 receive buffers of 4 KiB per ring and handle exhaustion, rearming and cancellation. `M_pool ~= R*1024*4096` replaces `N*4096` receive storage, with R rings and N connections.

- [x] Send headers and the moved response body with one scatter-gather operation; release body storage after the final send completion.

- [x] Use a busy-only 32-CQE / 4-ms wait with idle, accept and shutdown bypasses. An 8-ms wait did not reduce CPU further.

## First: transport and memory

- [x] Remove the admission experiment after the 300k/5m regression. If revisited, prevent fresh receive CQEs from overtaking deferred work and bound queued-request age. Its reservation floor permitted at most `128 MiB / 256 KiB = 512` active Exchanges.

- [ ] Separate backlog from leaks with `anon`, socket, file and kernel memory through load, drain and a quiet period. Check live ownership and bounded pool retention; reaching the cgroup limit alone does not identify a leak. Compare `sock/N_live` and `slab/N_live` at equal connection counts.

- [x] Bound ENOBUFS retries by returned-buffer credits, capped at 1,024 per ring, plus one no-progress probe per ring every 4 ms. Keep FIFO and the 256-retry turn limit. A single refill must resume multiple retry waves without further buffer returns.

- [x] Compare earlier send submission after each ring drain. No CPU or latency gain at 70k; retain submission at the next wait.

- [x] Use CQ 8,192 with SQ 1,024 and unchanged work budgets ([Rust](submissions/rust-iouring-kensac/src/main.rs), [C](submissions/c-iouring-pavlyhalim/src/sys.h)). It avoids overflow in the 4,096-completion probe and passes 300k/5m without a clear steady-load gain. Extra CQE storage is 96 KiB per ring; CQ 16,384 gave no added burst benefit.

- [x] Skip poll-first only for deferred ENOBUFS retries. Four quiet forced-recovery pairs: CPU -8.3%, recovery p50 -1.8%, retry submissions +0.2%; ordinary receives keep poll-first.

- [ ] Compare process-local `SO_RCVBUF` and `SO_SNDBUF` caps only if charged socket-buffer bytes grow; healthy holds mostly charge fixed kernel/slab memory. Measure retransmits, zero-window events and p99; configured caps are not preallocated bytes and explicit settings disable TCP autotuning ([Linux](https://github.com/torvalds/linux/blob/v6.8/Documentation/networking/ip-sysctl.rst)).

- [ ] Compare one versus eight `SO_REUSEPORT` listeners with the same total pending accepts ([sys.h](submissions/c-iouring-pavlyhalim/src/sys.h)). Target connection ramps and churn separately from established-request throughput.

- [x] Scale direct descriptors across bounded io_uring rings on one thread ([sys.h](submissions/c-iouring-pavlyhalim/src/sys.h)). Direct accept avoids installing ordinary process FDs.
  Keep fixed-file slot indices separate from OS descriptors; a slot index must never reach `Descriptor` or `close()`. Route operations by ring/slot and recycle a slot only after its close CQE.
  The official `RLIMIT_NOFILE=65535` caps each ring's sparse table. Add rings lazily, up to 16, to exceed the ordinary FD limit without raising it. Bound total ring/queue/pool memory.

- [x] Keep 64 reserved-slot direct accepts pending on one listener. Burst tests reduced recovery latency while handlers were blocked; tickets retain their ring/slot through cancellation and late successful accepts.

- [x] Compare idle multishot polls with bounded nonblocking reads. At 300k, CPU/request rose from 28.42 to 30.26 us and p99 from 8.03 to 4,170.84 ms; keep pending receives.

- [ ] Compare lazy ring packing with balanced 4/8/12-ring connection shards where descriptor capacity permits, at the same load and total buffer budget. One issuer and one vCPU remain; compare CPU/request, wakeups, empty CQ scans and p99. More rings add capacity and change locality, not CPU cores.
  In the 300k/5m trace, rings 0-3 exhausted buffers; ring 4 did not. Ring 3 produced 51.2% of ENOBUFS completions.
  Place new accept tickets by live plus reserved slots first, then compare buffer occupancy and deferred-request age as weights. Keep established sockets on their owning ring; migration needs separate lifetime handling. Normalize ENOBUFS by receive attempts before inferring load from the historical share.

- [ ] Compare an exact non-empty-ring bitmap if ring selection becomes hot. Free-slot allocation is already O(1); a Bloom filter adds hashes and false positives before a direct slot lookup.

- [x] Compare a bounded ready queue with round-robin handler dispatch. At 100k, CPU/request changed from 31.05 to 31.32 us and p99 from 27.42 to 32.71 ms; keep inline dispatch.

- [ ] Rotate which ring starts CQ draining each turn, keeping the 256-CQE cap and inline handlers. Compare current per-ring receive/defer metrics; preserve per-connection completion/response order, ring growth during accepts and primary GETEVENTS progress.

- [x] Collect handler, database-method, commit and send-preparation-to-CQE histograms, with per-ring CQ depth/overflow and five-second client/scheduling windows. Send timing includes batching and CQ service delay; CPU samples omit I/O waits.
  A 300k trace captured 2.53- and 1.43-second foreground commits followed by full CQs and CPU saturation. Client scheduling stayed near 1 ms; foreground WAL-header sync under NORMAL is a candidate to measure next.

- [ ] Bound CQ draining and handler work before extending completion waits further. Network/socket paths account for 40.2% and io_uring for 12.0% of sampled CPU in the [100k profile](src/profiles/exchange-100000.json).
  Measure requests/enter, CQ batch sizes and CPU/request: `C ~= C_enter/requests_per_enter + C_other`. Adapt batching to queue depth and oldest response age; cap CQ drains by count or elapsed time, submit pending sends promptly and avoid waiting while application work is ready.
  Budget socket reads and response work separately: `M_queue ~= Q_kernel*m_kernel + Q_app*m_app`. Drain input within a byte budget before expensive handlers; cap queued input and output, pause new work at a high watermark and resume below a lower one.
  Start with FIFO; compare byte-deficit scheduling for uneven response sizes, with request-age promotion to prevent starvation. Preserve connection/write order and read fresh data when dispatched.
  Test multishot accept separately to reduce accept SQEs and rearming. Count accepted sockets and terminal CQEs correctly, close arrivals during shutdown and rearm after termination.
- [x] Enable primary-only `SINGLE_ISSUER`/`DEFER_TASKRUN`, with GETEVENTS on the ready-satellite shortcut. Receive, partial send and stop progress passed with continuously ready satellites; satellite wakeups stay unchanged. Short 70k pair: CPU/request -3.4%, p99 unchanged.

- [x] Use a fixed 16-byte CQE cursor, preserving acquire/release ordering, wraparound and the 256-CQE bound. Dispatch code shrank by 32 bytes; native timing gain remains inconclusive.

- [x] Snapshot the stable ring span in `wait()`, outside accept handling. Repeated vector loads disappeared and code shrank by 22 bytes; native timing gain remains inconclusive.

- [x] Compare direct receive-buffer parsing against the 4-ms / 32-CQE baseline. The first pair saved memory without a CPU gain; keep the current path for now.
  Parse leased receive bytes directly with llhttp; copy only unconsumed tails across callbacks or sends. Release the lease before auth/handler callbacks and preserve early auth errors, fragments and pipeline order.
  Measure request-field allocations separately before adding views. Fields that survive a receive lease need request-owned storage before the lease is released.

- [x] Measure a hybrid send threshold: contiguous `send()` for small responses, one `sendmsg()` for larger bodies. At 100k, 256-byte bodies gave no clear CPU gain (32.54 to 32.52 us/request); retain scatter-gather.
  Compare `C_send ~= a_send + B*c_copy` with `C_sendmsg ~= a_sendmsg`; copying B bytes can cost less than vector setup for a small body. Find the crossover on mixed response sizes.
  Compare header headroom with moving the body and inserting the header; insertion still moves B bytes. Keep one operation per attempt and release large storage after completion so idle connections retain the accepted memory savings.
  Track partial sends across header/body boundaries; keep storage alive through completion or confirmed cancellation. Finish the database read before sending 200 so failures can still return 500.
  Consider a zero-copy send only for a measured large-response case; account for notification CQEs and the longer buffer lifetime.

## Then: storage and measured hot paths

- [ ] If byte-budget admission is revisited, avoid repeated capacity scans: the discarded implementation scanned six times on an ordinary request. Account at scheduling boundaries and preserve low-water recovery for unchanged charges; direct accounting CPU was small in the backlog profile.

- [x] Share immutable parser callbacks and use small parser/connection FSMs. API handlers already live for the server's lifetime; request state and JWT verification remain per request.
- [ ] Test an event bus only if multiple event consumers justify its routing and lifetime costs.
- [x] Move cold parser/buffer/send state behind a lazily owned Exchange ([Rust](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/4298101/submissions/rust-iouring-kensac/src/main.rs#L133)). Current storage: `56*N + 608*(A+P)`, plus buffers and allocator costs; N is open connections, A is active exchanges, P is pooled exchanges. Keep each parser alive through fragments, auth, pipeline tails and terminal I/O CQEs.
  Bound recycled Exchange/output capacity and discard oversized buffers; a slow send must not pin unrelated responses.
- [x] Reuse request slots through pooled Exchanges and cleared parser strings. Keep the 8-KiB retention limit, reset identity/body/framing and verify JWTs again for every request.
- [x] Reuse bounded response storage at the producer and reserve 6 KiB for feeds. Warm bounded replies need no C++ allocations; the 5-KiB probe feed retains 6,419 bytes within the unchanged 8-KiB Exchange cap. Discard large bodies after terminal sends and read SQLite afresh.
  No extra read CPU gain over the smaller reserve alone was established; retained memory rises.
- [x] Reuse 16 fixed write-outcome slots and clear used results after publishing replies. Preserve rollback isolation, queued-data ownership and commit-before-response behavior.
- [ ] Compare create-command string ownership/reuse separately.
- [ ] Measure request-local auth/JSON scratch: fixed-size signature decoding, HMAC key-context reuse and one UTF-8 trim/count pass. Verify every token again; keep malformed input and Unicode behavior unchanged.
- [ ] Compare encoded CQE identities `(event, ring, slot, generation)` with the two Operation records ([C](submissions/c-iouring-pavlyhalim/src/server.c)). Preserve cancellation matching, generation wrap and stale-buffer returns.

- [x] Move PASSIVE checkpoints to a worker with its own SQLite connection. At 100k, p99 fell from 27.42 to 4.71 ms while CPU/request rose from 31.05 to 32.70 us.
  Coalesce 1,000-frame wakeups, defer partial/failed checkpoints and join before closing connections. Keep commits on the issuer and preserve `synchronous=NORMAL`; confirm WAL backlog and latency at 300k.

- [ ] Compare nice 19 for the checkpoint thread only ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)); measure issuer latency together with checkpoint duration and WAL backlog.

- [x] Group up to 16 queued writes without added delay: `C(b) ~= s + a/b`.
  Own job data, isolate failures with savepoints and release responses after commit. Keep reads outside the write transaction.

- [x] Reuse prepared transaction/savepoint statements instead of parsing control SQL per write. Keep failure injection on `sqlite3_step` and verify recovery before reuse.

- [x] Prepare ROLLBACK before admission and quarantine unresolved transactions. Test preparation failure, failed read/write rollback, blocked subsequent access and clean reopening.

- [ ] Tune batch size or a collection window from the measured write arrival rate: average collection wait `~= (b-1)/(2*lambda)`.
  Compare `INSERT ... WHERE EXISTS ... ON CONFLICT` for likes and insert + rowid/timestamp lookup against `RETURNING` ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)).

- [x] Compare point lookups with bounded range scans for feed posts. Native feed CPU fell from 10.58 to 9.06 us; whole-server CPU stayed nearly unchanged at 300k.
  `C_point ~= n*a_seek + k_target*s_scan`; `C_range ~= a_seek + k_span*s_scan`.
  Start with a small decision tree of measured scan/seek thresholds; an ML forest is unnecessary for this choice. Bound skipped rows before seeking across gaps; a narrow ID span can still contain many likes.
  Collect IDs within one fresh feed snapshot, scan in ID order and restore timestamp order with request-local slots, including gaps and reordered timestamps.

- [x] Deduplicate author IDs within one feed and read them in ID order. Copy post records and author names into request-local arenas; retain the SQL point-read path and remove the unused B-tree point-read API.

- [ ] Compare locality-aware scheduling of already queued reads by endpoint and nearby post/user IDs. Each request performs its own fresh SQLite read; reuse only SQLite's page cache, never query results.
  Bound queue size and request age, preserve connection order and write/read barriers. Compare FIFO, page-cache misses, CPU/request and p99; locality gains must exceed sorting and waiting costs.

- [ ] Compare a bounded read transaction for already queued HTTP reads, with a separate SQLite query for every request. Close batch membership before opening its snapshot, keep new arrivals for the next batch and preserve write barriers and connection order.
  Amortize snapshot setup without sharing results. `unix-excl` already removes the lock syscalls; measure remaining transaction cost before adding a queue. Verify snapshot visibility with concurrent writes, then compare queue cost and p99.

- [ ] Compare batched lookups by B-tree level ([bt_seek_many](submissions/c-iouring-pavlyhalim/src/btree.h)).
  Interleave independent descents and fetch shared pages once. Try interpolation from the current page's endpoint keys, with bounded correction and a correct binary-search fallback.
  Keep all lookup state within the request. Pin author pages until serialization; copy post bytes before moving cursors.
  Keep navigation in SQLite's pager/B-tree; logical IDs and timestamps do not identify stable file offsets.

- [ ] Compare a direct B-tree point reader with prepared SQL for `GET /posts/:id`. Include snapshot/cursor setup, author lookup, current like counts and cleanup when measuring VM savings; retain SQL fallback for unsupported cases.

- [ ] Compare stable connection slabs when connection churn, allocation or connection memory is material.
  CQEs already carry `Connection*`, and each ring owns a table indexed by direct slot. Measure construction and destruction before replacing per-connection allocations.
  `M_slots ~= rings*slots_per_ring*sizeof(slot)` plus connection storage. Reuse slab slots only after all operations and cancellation completions are finished; preserve stable addresses or validate generation-tagged handles.

- [ ] Inspect field offsets and padding in Connection, Exchange and operation records before changing layouts. At 300k connections, saving 8 bytes each saves only 2.29 MiB; retain stable addresses and terminal-CQE ownership.

- [ ] Compare receive-pool sizes such as 256 x 4 KiB and 512 x 2 KiB against 1,024 x 4 KiB.
  `M_pool ~= K*B`; measure buffer occupancy, extra receives for fragmented requests, `ENOBUFS`, rearm work and latency during bursts. Reducing the current 4-MiB pool alone saves little of the full server's memory.
  Try dynamic sizing only if the static comparisons show a useful tradeoff. Bound total capacity and keep buffer IDs and leased storage valid through resizing.
  Compare an aggregate retained-byte cap for pooled Exchanges separately from active-request admission; a per-ring object-count cap alone scales retained capacity with ring count.

- [ ] Compare process-local TCP options independently: `TCP_NODELAY` for small-response latency, `TCP_DEFER_ACCEPT` for connection churn and explicitly uncorked `TCP_CORK` for deliberate coalescing ([TCP options](https://man7.org/linux/man-pages/man7/tcp.7.html)).
  Measure packets, CPU and p99 as well as throughput. The current single header/body send already coalesces application output; corking can add delay, and deferred accept mainly affects connection establishment rather than established keep-alive traffic. Keep system-wide kernel settings unchanged.
  A narrow receive experiment can skip poll-first when `IORING_CQE_F_SOCK_NONEMPTY` reports unread data; keep the current path for idle connections and test fragments, pipelining and terminal completions.

## Larger or conditional experiments

- [ ] Measure request-local SQLite cache counters, then trace fetch/unpin/rekey/truncate calls and occupancy through a delegating PCACHE2 wrapper (`SQLITE_CONFIG_PCACHE2`, [C](submissions/c-iouring-pavlyhalim/src/db.h)). `pcache1Fetch` self CPU (~2.6%) does not establish a miss rate.
  Start with `CACHE_HIT/MISS/USED/SPILL` deltas on each connection's owning thread. Compare the built-in `SQLITE_CONFIG_PAGECACHE` pool before replacing eviction policy; register any PCACHE2 wrapper before SQLite initialization.
  Compare bounded pools, allocators or eviction policies only if cache costs are material; preserve pin/unpin, rekey, truncate and both-handle lifetimes. Truncate scans previously cost only 0.2-0.3% of total CPU. Custom replacement-cache rule status remains unconfirmed.

- [ ] Consider bounded, best-effort executable/read-only `mlock` only after major faults coincide with stalls ([Mojo](submissions/mojo-iouring-oh-meh/src/shim.c)). Saved normal 300k runs recorded zero major faults; locked pages reduce reclaimable memory.

- [ ] Compare a lazy Exchange-owned scratch arena only if allocation measurements justify it. Keep fragmented input, queued commands and send bytes valid through their actual lifetimes; `pmr` does not redirect ordinary strings, yyjson or SQLite allocations automatically.

- [ ] Test request-local or negotiated HTTP compression only for large retained replies. Include CPU and scratch memory; the native generator currently cannot decode compressed responses. Hashes can identify payloads but cannot replace the bytes required by the API ([HTTP](https://www.rfc-editor.org/rfc/rfc9110.html#field.accept-encoding)). Direct slot indexing already avoids hash-table lookups for connections.

- [x] Use `unix-excl` on both database handles ([Kensac](submissions/rust-iouring-kensac/csrc/shim.c), [Pavly](submissions/c-iouring-pavlyhalim/src/db.h)). A small SQLite probe reduced 67,080 measured fcntl64 calls to zero. The short 70k pair used 2.5% less CPU/request with nearly unchanged p99. External SQLite processes must wait until shutdown; keep the checkpoint handle and NORMAL sync.

- [ ] Compare contiguous WAL-write coalescing separately over the stock Unix VFS, retaining external access ([engine.c](submissions/c-iouring-public/components/engine.c)). Target pwrite calls; preserve commit visibility, short-write/error handling and NORMAL sync.

- [ ] Compare linked shutdown/close with selective CQE suppression only after preserving pending counts, failure paths and final ownership retirement. Target connection churn; ordinary keep-alive requests avoid this work already.

- [x] Build a small native load generator matching `bench/load.js`: shared tokens, per-user connections and state, the same ramps, think times, write probabilities, checks and latency definitions.
  Use an event loop and timers instead of a JavaScript runtime per user. Compare small runs against k6 before using it for capacity experiments; keep k6 as the reference.

- [x] Bound the load generator's due-timer pass to 256 services / 1 ms, checked between calls. Record time from epoll return to the next call separately from dispatch lag. A single blocking syscall can still exceed the budget; causation of earlier server tails remains unproven.

- [ ] Test multishot receive after the simpler accept and work-budget comparisons.
  It needs separate stable receive/send operation identities, terminal-CQE tracking and bounded `ENOBUFS` rearming; the current one-operation-per-connection model cannot track concurrent receive/send operations.
  Bound queued input while sends are outstanding, preserve HTTP response order and return each selected buffer exactly once, including shutdown and cancellation.

- [ ] Compare a narrow HTTP fast path for fully buffered, ordinary requests with llhttp fallback for everything else, only if parsing remains hot.
  Recognize a proven subset without consuming or mutating parser state before fallback. Preserve content-length/chunked framing, header handling, path decoding, auth-first errors, fragments and pipelining; five routes alone do not make the HTTP grammar small.
  Measure saved parsing and allocations against the extra checks and duplicated protocol logic. Keep the direct llhttp receive path as the reference.

- [ ] Benchmark an isolated epoll transport with nonblocking `recv()`/`send()` against io_uring, using the same API/database handlers and request semantics.
  Keep io_uring as the chosen architecture while testing this alternative. Hold connection count, receive/send storage, work budgets, socket options and load setup constant; compare CPU/request, tail latency, memory and errors.
  Include `EAGAIN`, short I/O, disconnects, early auth rejection, pipelining and shutdown. Count syscall and readiness costs rather than assuming either I/O API wins for small requests.

- [x] Compare SQLite's own page-cache sizes with `mmap_size=0`: `N ~= (RAM - fixed_cost - cache - queues)/memory_per_connection`. SQLite's page cache is explicitly allowed by [rule 5](README.md#rules).
  Use a 16-MiB budget; random post reads improved without the larger cache's feed regression.
  Keep one database at `SQLITE_PATH`; shard transport metadata and work queues, not database files or schema. Extra connections can isolate I/O waits but share one vCPU and one WAL writer.

- [ ] Compare PGO trained on fresh synthetic data with plain optimization ([build.sh](submissions/c-iouring-pavlyhalim/build.sh)).
  SSE2 JSON/UTF-8 scanning already exists and is baseline on x86_64; test wider SIMD for measured hot loops. AVX2/AVX-512 need matching target support or runtime dispatch.
  `S <= 1/((1-p) + p/s)`, with p measured against total CPU including kernel work.
  JSON is currently 1.8% of sampled CPU, so use a fresh profile before spending time on vector width or more encoding specializations.

- [ ] Compare loop unrolling (`-funroll-loops`, `-funroll-all-loops` and unroll limits), then revisit `-flto` and PGO combinations after hot-path changes.
  Change one option at a time; compare CPU/request, p99 and code size, and inspect the hot-path assembly for instruction-cache costs.

- [ ] Compare stack-protector, `_FORTIFY_SOURCE` and allocator-hardening settings independently, including disabled variants.
  Keep correctness and sanitizer checks; record measured gains and which protections each variant removes.

- [ ] Compare ISA-specific kernels for the Xeon Platinum 8260 (Cascade Lake) after checking the VPS feature mask ([GCC](https://gcc.gnu.org/onlinedocs/gcc-13.1.0/gcc/x86-Options.html)). The local `-mtune=cascadelake` comparison is complete.
  Compare SSE2, AVX2 and AVX-512 with runtime dispatch; keep surrogate and bounds handling.

- [ ] Test C++26 when a specific feature simplifies a hot path; keep compiler upgrades separate from language changes ([standard support](https://gcc.gnu.org/projects/cxx-status.html)).

- [ ] Compare throughput, latency, errors and CPU/request together:
  `requests/s ~= 2.17*N/(20.5 + HTTP_wait_per_cycle)`.
  User count is not simultaneous request count.
