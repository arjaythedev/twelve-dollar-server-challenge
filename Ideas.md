# Ideas

Unchecked items are experiments; checked items include completed work and discarded comparisons.

## Top priority: review follow-ups

### 1. CQ waits and current profiling

- [ ] Measure the current split between user space, kernel networking, io_uring and filesystem work at low and high load. The [historical 350k profile](src/runs/legacy/profile-unix-excl-350000/summary.json) sampled 30.19 of 65.64 CPU-seconds in kernel networking; Docker loopback costs may differ from a real NIC.
  Record enter duration, requested/returned CQ batch sizes, pending-SQ age and completion-to-dispatch delay separately. Existing send-preparation-to-CQE timing combines submission, kernel and CQ-service waits; CPU samples exclude blocked time. Use kernel tracing when exact CQ-post timing is needed.

- [ ] Compare one versus two generator workers if higher loads exhaust the optimized single worker. Use a shared start gate, disjoint VUs/source IPs and fixed worker seeds; merge latency samples before calculating percentiles.
  Preserve total load and API checks. Reject late activation, incomplete response coverage and workflow failures; report client CPU, timer lag and epoll service lag separately. Repeat server comparisons with the same worker split.

- [ ] Reduce the CPU cost of `wait_nr=1` in [Reactor::wait](src/server/net/reactor.cpp) while preserving its latency benefit and ready-CQE bulk drain. Compare 50/100/250/500 us collection budgets and track write-batch sizes as well as CPU/request.
  On stock Linux 6.8, test a bounded two-stage wait: collect up to 32 CQEs briefly, then wait for one if none are ready, within the remaining 4-ms outer deadline. Preserve idle/accept/shutdown bypasses and primary GETEVENTS progress. Native `IORING_FEAT_MIN_TIMEOUT` needs Linux 6.12+ and is unavailable on the target VPS ([liburing](https://github.com/axboe/liburing/blob/liburing-2.15/man/io_uring_submit_and_wait_min_timeout.3)).

- [ ] Compare bounded handler/CQ work and age-aware batching after separating wait costs. The current CQ drain already caps each ring at 256 entries; compare elapsed-time limits and oldest-response age without waiting while application work is ready.
  `C ~= C_enter/requests_per_enter + C_other`. Submit pending sends promptly; the earlier per-ring submission comparison found no gain.
  Budget socket input and handler/output work separately: `M_queue ~= Q_kernel*m_kernel + Q_app*m_app`. Start with bounded FIFO and high/low watermarks; try byte-deficit scheduling with age promotion only for measured size imbalance. Preserve connection/write order and fresh reads.

### 2. Common-request CPU

- [ ] Compare a direct B-tree point reader with prepared SQL for `GET /posts/:id` ([current SQL path](src/server/storage/database.cpp), [feed cursor API](src/server/sqlite_read.h)). Post reads were about 46% of requests in the historical 350k profile; confirm their current CPU share.
  Include fresh snapshot/cursor setup, author lookup, current like counts and cleanup. Reuse request-local feed primitives, preserve missing-post/error behavior and retain SQL fallback for unsupported cases; compare route CPU and whole-server CPU/request.

- [ ] Skip storing irrelevant header values in [Parser callbacks](src/server/http/parser.cpp), while llhttp still validates every byte and handles framing. Match fragmented, case-insensitive `authorization` fields and copy only required request data.
  Preserve existing field/value size limits, duplicate-header behavior, early auth errors, fragments, chunking and pipelining. Measure copies/allocations before considering a second HTTP parser.

- [ ] Revisit direct receive-buffer parsing only for a measured frequent case of a complete ordinary request, with the current Session-copy path for fragments and retained tails ([receive handler](src/server/server.cpp)).
  The broader leased-buffer experiment found no CPU gain. Keep llhttp, request-owned fields before releasing the lease, early auth errors and pipeline order; compare copies, CPU and memory before expanding it.

- [ ] Compare the satellite eventfd `POLL_ADD` plus `read()` path with `IORING_OP_READ` ([complete_wake](src/server/net/reactor.cpp)). Probe kernel behavior first: `EFD_NONBLOCK` may complete with `EAGAIN` instead of waiting.
  Keep stable counter storage, bounded rearming, wakeup/shutdown progress and no missed notifications. Historical sampled `read` CPU was only ~0.24 of 65.64 seconds, so check its current share before adding machinery.

### 3. Failure-path robustness

- [ ] Bound and rate-limit repeated request/write-batch error output in [server.cpp](src/server/server.cpp), retaining aggregate and suppressed-error counts. Synchronous `std::cerr` can stall the sole issuer under logging backpressure; this has not been shown in successful runs.
  Test a repeated failure with a slow stderr consumer; keep memory bounded and response/rollback behavior intact.

- [ ] Reject invalid JWT signatures before decoding/parsing header and claims JSON in [authenticate](src/server/auth.cpp), after bounded token-shape and signature decoding checks. Keep fixed HS256 verification, constant-time comparison, claim/expiry validation and exact error behavior.
  Measure malformed/invalid-token cost separately from the valid-token workload. Verify every request; this reduces unauthenticated parsing work, with no claimed authentication bypass.

## Next: transport and memory

### Transport scheduling and socket costs

- [ ] Compare lazy ring packing with balanced 4/8/12-ring connection shards where descriptor capacity permits, at the same load and total buffer budget. One issuer and one vCPU remain; compare CPU/request, wakeups, empty CQ scans and p99. More rings add capacity and change locality, not CPU cores.
  Old 300k/5m trace: rings 0-3 each served about 2M send attempts, but ring 3 had 2.21M ENOBUFS versus 0.33M on ring 2. Current 350k diagnostics: six rings, zero ENOBUFS, no deferred receives or CQ overflow in four full-load intervals. No balancing change yet.
  Place new accept tickets by live plus reserved slots first, then compare buffer occupancy and deferred-request age as weights. Keep established sockets on their owning ring; migration needs separate lifetime handling. Normalize ENOBUFS by receive attempts before inferring load from the historical share.

- [ ] Compare one versus eight `SO_REUSEPORT` listeners with the same total pending accepts ([sys.h](submissions/c-iouring-pavlyhalim/src/sys.h)). Target connection ramps and churn separately from established-request throughput.

- [ ] Compare multishot accept separately to reduce accept SQEs and rearming. Count accepted sockets and terminal CQEs correctly, close arrivals during shutdown and rearm after termination.

- [ ] Compare process-local TCP options independently: `TCP_NODELAY` for small responses, `TCP_DEFER_ACCEPT` for churn and explicitly uncorked `TCP_CORK` for deliberate coalescing ([TCP options](https://man7.org/linux/man-pages/man7/tcp.7.html)).
  Accepted sockets use direct descriptors: never pass a fixed-file slot to `setsockopt()`. Check supported io_uring socket commands or listener-option inheritance first. Measure packets, CPU and p99; the existing single header/body send already coalesces output, corking can delay it, and deferred accept mainly affects establishment. Keep system-wide kernel settings unchanged.
  Separately skip poll-first when `IORING_CQE_F_SOCK_NONEMPTY` reports unread data; retain the idle path and test fragments, pipelining and terminal completions.

- [ ] Compare process-local `SO_RCVBUF` and `SO_SNDBUF` caps only if charged socket-buffer bytes grow; healthy holds mostly charge fixed kernel/slab memory. Measure retransmits, zero-window events and p99; configured caps are not preallocated bytes and explicit settings disable TCP autotuning ([Linux](https://github.com/torvalds/linux/blob/v6.8/Documentation/networking/ip-sysctl.rst)).
  Use the same direct-descriptor-safe option path as the TCP comparison above.

- [ ] Compare an exact non-empty-ring bitmap if ring selection becomes hot. Free-slot allocation is already O(1); a Bloom filter adds hashes and false positives before a direct slot lookup.

- [ ] Compare linked shutdown/close with selective CQE suppression only after preserving pending counts, failure paths and final ownership retirement. Target connection churn; ordinary keep-alive requests avoid this work already.
  Targeted cancellation and direct closes are already present: normal EOF skips cancellation; other closes target the owning Operation and retire only after the close CQE ([Ada PR 211](https://github.com/visseranalytics/twelve-dollar-server-challenge/blob/7ff01df614ba46dfe89c6388aab52ed176538873/submissions/ada-native-visseranalytics/src/engine.adb#L252)).

### Memory and ownership

- [ ] Separate backlog from leaks with `anon`, socket, file and kernel memory through load, drain and a quiet period. Check live ownership and bounded pool retention; reaching the cgroup limit alone does not identify a leak. Compare `sock/N_live` and `slab/N_live` at equal connection counts.

- [ ] Compare stable connection slabs when connection churn, allocation or connection memory is material.
  CQEs already carry `Connection*`, and each ring owns a table indexed by direct slot. Measure construction and destruction before replacing per-connection allocations.
  `M_slots ~= rings*slots_per_ring*sizeof(slot)` plus connection storage. Reuse slab slots only after all operations and cancellation completions are finished; preserve stable addresses or validate generation-tagged handles.

- [ ] Inspect field offsets and padding in Connection, Exchange and operation records before changing layouts. At 300k connections, saving 8 bytes each saves only 2.29 MiB; retain stable addresses and terminal-CQE ownership.

- [ ] Compare receive-pool sizes such as 256 x 4 KiB and 512 x 2 KiB against 1,024 x 4 KiB.
  `M_pool ~= K*B`; measure buffer occupancy, extra receives for fragmented requests, `ENOBUFS`, rearm work and latency during bursts. Reducing the current 4-MiB pool alone saves little of the full server's memory.
  Try dynamic sizing only if the static comparisons show a useful tradeoff. Bound total capacity and keep buffer IDs and leased storage valid through resizing.
  Compare an aggregate retained-byte cap for pooled Exchanges separately from active-request admission; a per-ring object-count cap alone scales retained capacity with ring count.

- [ ] Revisit byte-budget admission only after identifying retained backlog and fixing work ordering. The discarded 300k/5m variant regressed; its `128 MiB / 256 KiB = 512` reservation floor was restrictive and six capacity scans ran on an ordinary request.
  Account at scheduling boundaries, prevent fresh receives from overtaking deferred work, bound request age and preserve low-water recovery for unchanged charges. Direct accounting CPU was small in the backlog profile.

- [ ] Compare encoded CQE identities `(event, ring, slot, generation)` with the two Operation records ([C](submissions/c-iouring-pavlyhalim/src/server.c)). Preserve cancellation matching, generation wrap and stale-buffer returns.

- [ ] Compare create-command string ownership/reuse separately.

- [ ] Compare a lazy Exchange-owned scratch arena only if allocation measurements justify it. Keep fragmented input, queued commands and send bytes valid through their actual lifetimes; `pmr` does not redirect ordinary strings, yyjson or SQLite allocations automatically.

## Next: SQLite reads, writes and WAL

- [ ] Compare nice 19 for the checkpoint thread only ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)); measure issuer latency together with checkpoint duration and WAL backlog.

- [ ] If WAL backfill falls behind, compare a bounded write gate ([Ada PR 211](https://github.com/visseranalytics/twelve-dollar-server-challenge/blob/7ff01df614ba46dfe89c6388aab52ed176538873/submissions/ada-native-visseranalytics/src/database.adb#L28)). Keep independent reads moving; release held writes after the requested checkpoint pass or a timeout.
  Add explicit worker retry and reactor wakeup: our worker currently waits for another commit, which a gate could prevent. Preserve pipeline order and commit-before-response; compare WAL backlog, memory and write p99.

- [ ] Tune batch size or a collection window from the measured write arrival rate: average collection wait `~= (b-1)/(2*lambda)`.

- [ ] Compare removing per-write savepoints for the fixed schema, relying on SQLite's statement rollback for ABORT constraints. Preserve batch failure, rollback quarantine and isolation; `RAISE(FAIL)` and custom trigger side effects need separate handling.

- [ ] Compare locality-aware scheduling of queued reads by endpoint and nearby post/user IDs, optionally with separate bounded read/write queues. Each request performs its own fresh SQLite read; reuse only SQLite's page cache, never query results.
  Bound queue size and request age, preserve connection order and write/read barriers. Compare FIFO, page-cache misses, CPU/request and p99; locality gains must exceed sorting and waiting costs.

- [ ] Compare a bounded read transaction for already queued HTTP reads, with a separate SQLite query for every request. Close batch membership before opening its snapshot, keep new arrivals for the next batch and preserve write barriers and connection order.
  Amortize snapshot setup without sharing results. `unix-excl` already removes the lock syscalls; measure remaining transaction cost before adding a queue. Verify snapshot visibility with concurrent writes, then compare queue cost and p99.

- [ ] Compare batched lookups by B-tree level ([bt_seek_many](submissions/c-iouring-pavlyhalim/src/btree.h)).
  Interleave independent descents and fetch shared pages once. Try interpolation from the current page's endpoint keys, with bounded correction and a correct binary-search fallback.
  Keep all lookup state within the request. Pin author pages until serialization; copy post bytes before moving cursors.
  Keep navigation in SQLite's pager/B-tree; logical IDs and timestamps do not identify stable file offsets.

- [ ] Compare contiguous WAL-write coalescing over `unix-excl` ([Pavly](submissions/c-iouring-pavlyhalim/src/db.h)). Target pwrite calls; preserve commit visibility, short-write/error handling and NORMAL sync. Keep WAL eviction working through the wrapper; it currently requires `xRead == unixRead`.

- [ ] Compare fresh Btrfs CoW and `chattr +C` directories on the same Mini, then ext4 on the same hardware if available. Measure WAL residency, I/O and p99; `+C` also disables data checksums and compression.

- [ ] Consider a create-only preupdate hook if timestamp readback becomes material. It needs `SQLITE_ENABLE_PREUPDATE_HOOK` and unpacks the full inserted record; copy provisional metadata immediately and publish only after successful statement and batch completion.

## Conditional: larger changes and build experiments

### Protocol, authentication and alternative transport

- [ ] Measure request-local auth/JSON scratch: fixed-size signature decoding, HMAC key-context reuse and one UTF-8 trim/count pass. Verify every token again; keep malformed input and Unicode behavior unchanged.

- [ ] Compare independent HMAC batches of 1/2/4/8/16 after key-context reuse ([multi-buffer HMAC](submissions/c-iouring-public/third_party/cursed-jwt/src/jwt_mb.inc)). Verify every signature and preserve our parsing, expiry and error behavior. Auth was 1.53% of total CPU; even 2x whole-auth speedup gives only about 0.77% overall.
  Queue only already-ready headers, flush before waiting and hold Session/Exchange ownership through auth and shutdown. Never reuse verified identities across requests.

- [ ] Compare a narrow HTTP fast path for fully buffered, ordinary requests with llhttp fallback for everything else, only if parsing remains hot.
  Recognize a proven subset without consuming or mutating parser state before fallback. Preserve content-length/chunked framing, header handling, path decoding, auth-first errors, fragments and pipelining; five routes alone do not make the HTTP grammar small.
  Measure saved parsing and allocations against the extra checks and duplicated protocol logic. Keep the current llhttp parser path as the reference.

- [ ] Test multishot receive after the simpler accept and work-budget comparisons.
  It needs separate stable receive/send operation identities, terminal-CQE tracking and bounded `ENOBUFS` rearming; the current one-operation-per-connection model cannot track concurrent receive/send operations.
  Bound queued input while sends are outstanding, preserve HTTP response order and return each selected buffer exactly once, including shutdown and cancellation.

- [ ] Consider zero-copy sends only for measured large responses; include notification CQEs, retained bytes and longer buffer lifetimes. Keep partial-send and cancellation ownership explicit.

- [ ] Test request-local or negotiated HTTP compression only for large retained replies. Include CPU and scratch memory; the native generator currently cannot decode compressed responses. Hashes can identify payloads but cannot replace the bytes required by the API ([HTTP](https://www.rfc-editor.org/rfc/rfc9110.html#field.accept-encoding)). Direct slot indexing already avoids hash-table lookups for connections.

- [ ] Benchmark an isolated epoll transport with nonblocking `recv()`/`send()` against io_uring, using the same API/database handlers and request semantics.
  Keep io_uring as the chosen architecture while testing this alternative. Hold connection count, receive/send storage, work budgets, socket options and load setup constant; compare CPU/request, tail latency, memory and errors.
  Include `EAGAIN`, short I/O, disconnects, early auth rejection, pipelining and shutdown. Count syscall and readiness costs rather than assuming either I/O API wins for small requests.

- [ ] Test an event bus only if multiple event consumers justify its routing and lifetime costs.

### SQLite cache, faults and compiler settings

- [ ] Measure request-local SQLite cache counters, then trace fetch/unpin/rekey/truncate calls and occupancy through a delegating PCACHE2 wrapper (`SQLITE_CONFIG_PCACHE2`, [C](submissions/c-iouring-pavlyhalim/src/db.h)). `pcache1Fetch` self CPU (~2.6%) does not establish a miss rate.
  Start with `CACHE_HIT/MISS/USED/SPILL` deltas on each connection's owning thread. Compare the built-in `SQLITE_CONFIG_PAGECACHE` pool before replacing eviction policy; register any PCACHE2 wrapper before SQLite initialization.
  Compare bounded pools, allocators or eviction policies only if cache costs are material; preserve pin/unpin, rekey, truncate and both-handle lifetimes. Truncate scans previously cost only 0.2-0.3% of total CPU. Custom replacement-cache rule status remains unconfirmed.

- [ ] Consider bounded, best-effort executable/read-only `mlock` only after major faults coincide with stalls ([Mojo](submissions/mojo-iouring-oh-meh/src/shim.c)). Saved normal 300k runs recorded zero major faults; locked pages reduce reclaimable memory.

- [ ] Compare loop unrolling (`-funroll-loops`, `-funroll-all-loops` and unroll limits), then revisit `-flto` after hot-path changes.
  Change one option at a time; compare CPU/request, p99 and code size, and inspect the hot-path assembly for instruction-cache costs.

- [ ] Compare stack-protector, `_FORTIFY_SOURCE` and allocator-hardening settings independently, including disabled variants.
  Keep correctness and sanitizer checks; record measured gains and which protections each variant removes.

- [ ] Compare ISA-specific kernels for the Xeon Platinum 8260 (Cascade Lake) after checking the VPS feature mask ([GCC](https://gcc.gnu.org/onlinedocs/gcc-13.1.0/gcc/x86-Options.html)). The local `-mtune=cascadelake` comparison is complete; SSE2 JSON/UTF-8 scanning already exists.
  Compare SSE2, AVX2 and AVX-512 only for current hot loops, with target support or runtime dispatch; preserve surrogate and bounds handling. Historical JSON CPU was 1.8%, so refresh the profile first.
  `S <= 1/((1-p) + p/s)`, with p measured against total CPU including kernel work.

- [ ] Test C++26 when a specific feature simplifies a hot path; keep compiler upgrades separate from language changes ([standard support](https://gcc.gnu.org/projects/cxx-status.html)).

- [ ] Compare throughput, latency, errors and CPU/request together:
  `requests/s ~= 2.17*N/(20.5 + HTTP_wait_per_cycle)`.
  User count is not simultaneous request count.

## Completed work and measured comparisons

### Transport, memory and request storage

- [x] Share 1,024 receive buffers of 4 KiB per ring and handle exhaustion, rearming and cancellation. `M_pool ~= R*1024*4096` replaces `N*4096` receive storage, with R rings and N connections.

- [x] Send headers and the moved response body with one scatter-gather operation; release body storage after the final send completion.

- [x] Compare busy-only 32-CQE / 4-ms waits with idle, accept and shutdown bypasses. An 8-ms wait did not reduce CPU further.

- [x] Keep `wait_nr=1` and the existing bulk drain. The short 350k pair lowered p99 but used about 15% more server CPU, with smaller write batches. The [control](src/runs/f5874c6/wait-one-control-350000/summary.json) missed the generator activation deadline; the [candidate](src/runs/f5874c6/wait-one-candidate-350000/summary.json) passed. Treat the comparison as provisional.

- [x] Remove the admission experiment after the 300k/5m regression. Revisit only under the bounded-admission idea above.

- [x] Bound ENOBUFS retries by returned-buffer credits, capped at 1,024 per ring, plus one no-progress probe per ring every 4 ms. Keep FIFO and the 256-retry turn limit. A single refill must resume multiple retry waves without further buffer returns.

- [x] Compare earlier send submission after each ring drain. No CPU or latency gain at 70k; retain submission at the next wait.

- [x] Use CQ 8,192 with SQ 1,024 and unchanged work budgets ([Rust](submissions/rust-iouring-kensac/src/main.rs), [C](submissions/c-iouring-pavlyhalim/src/sys.h)). It avoids overflow in the 4,096-completion probe and passes 300k/5m without a clear steady-load gain. Extra CQE storage is 96 KiB per ring; CQ 16,384 gave no added burst benefit.

- [x] Skip poll-first only for deferred ENOBUFS retries. Four quiet forced-recovery pairs: CPU -8.3%, recovery p50 -1.8%, retry submissions +0.2%; ordinary receives keep poll-first.

- [x] Scale direct descriptors across bounded io_uring rings on one thread ([sys.h](submissions/c-iouring-pavlyhalim/src/sys.h)). Direct accept avoids installing ordinary process FDs.
  Keep fixed-file slot indices separate from OS descriptors; a slot index must never reach `Descriptor` or `close()`. Route operations by ring/slot and recycle a slot only after its close CQE.
  The official `RLIMIT_NOFILE=65535` caps each ring's sparse table. Add rings lazily, up to 16, to exceed the ordinary FD limit without raising it. Bound total ring/queue/pool memory.

- [x] Keep 64 reserved-slot direct accepts pending on one listener. Burst tests reduced recovery latency while handlers were blocked; tickets retain their ring/slot through cancellation and late successful accepts.

- [x] Compare idle multishot polls with bounded nonblocking reads. At 300k, CPU/request rose from 28.42 to 30.26 us and p99 from 8.03 to 4,170.84 ms; keep pending receives.

- [x] Compare a bounded ready queue with round-robin handler dispatch. At 100k, CPU/request changed from 31.05 to 31.32 us and p99 from 27.42 to 32.71 ms; keep inline dispatch.

- [x] Compare rotating CQ starts, retaining the 256-CQE cap and fixed ownership. Matched 350k/5m B/A: CPU/request +0.5%, hold p99 -1.2%, zero errors. Restore fixed starts after no clear gain; growth, receive, partial send and shutdown pass in both builds.
  Track accepts in their handler and inspect deferred receive queues only when the wait decision needs them. Both reductions are visible in optimized assembly; large-load backlog effects remain unmeasured.

- [x] Collect handler, database-method, commit and send-preparation-to-CQE histograms, with per-ring CQ depth/overflow and five-second client/scheduling windows. Send timing includes batching and CQ service delay; CPU samples omit I/O waits.
  A 300k trace captured 2.53- and 1.43-second foreground commits followed by full CQs and CPU saturation. Client scheduling stayed near 1 ms; foreground WAL-header sync under NORMAL is a candidate to measure next.

- [x] Enable primary-only `SINGLE_ISSUER`/`DEFER_TASKRUN`, with GETEVENTS on the ready-satellite shortcut. Receive, partial send and stop progress passed with continuously ready satellites; satellite wakeups stay unchanged. Short 70k pair: CPU/request -3.4%, p99 unchanged.

- [x] Use a fixed 16-byte CQE cursor, preserving acquire/release ordering, wraparound and the 256-CQE bound. Dispatch code shrank by 32 bytes; native timing gain remains inconclusive.

- [x] Snapshot the stable ring span in `wait()`, outside accept handling. Repeated vector loads disappeared and code shrank by 22 bytes; native timing gain remains inconclusive.

- [x] Compare direct receive-buffer parsing against the 4-ms / 32-CQE baseline. The first pair saved memory without a CPU gain; keep the current path for now.
  Parse leased receive bytes directly with llhttp; copy only unconsumed tails across callbacks or sends. Release the lease before auth/handler callbacks and preserve early auth errors, fragments and pipeline order.
  Measure request-field allocations separately before adding views. Fields that survive a receive lease need request-owned storage before the lease is released.

- [x] Compare contiguous `send()` for small replies with scatter-gather. At 100k, 256-byte bodies gave no clear CPU gain (32.54 to 32.52 us/request); retain one `sendmsg()`.
  `C_send ~= a_send + B*c_copy` versus `C_sendmsg ~= a_sendmsg`; header insertion also moves B bytes. Preserve header/body partial sends and storage through final completion/cancellation, release oversized capacity, and finish database reads before sending 200.

- [x] Share immutable parser callbacks and use small parser/connection FSMs. API handlers already live for the server's lifetime; request state and JWT verification remain per request.

- [x] Move cold parser/buffer/send state behind a lazily owned Exchange ([Rust](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/4298101/submissions/rust-iouring-kensac/src/main.rs#L133)). Current storage: `56*N + 608*(A+P)`, plus buffers and allocator costs; N is open connections, A is active exchanges, P is pooled exchanges. Keep each parser alive through fragments, auth, pipeline tails and terminal I/O CQEs.
  Bound recycled Exchange/output capacity and discard oversized buffers; a slow send must not pin unrelated responses.

- [x] Reuse request slots through pooled Exchanges and cleared parser strings. Keep the 8-KiB retention limit, reset identity/body/framing and verify JWTs again for every request.

- [x] Reuse bounded response storage at the producer and reserve 6 KiB for feeds. Warm bounded replies need no C++ allocations; the 5-KiB probe feed retains 6,419 bytes within the unchanged 8-KiB Exchange cap. Discard large bodies after terminal sends and read SQLite afresh.
  No extra read CPU gain over the smaller reserve alone was established; retained memory rises.

- [x] Reuse 16 fixed write-outcome slots and clear used results after publishing replies. Preserve rollback isolation, queued-data ownership and commit-before-response behavior.

### SQLite and WAL

- [x] Read the feed through SQLite's own B-tree cursors, pager, WAL and cache, skipping the VM. Keep SQL for point reads.

- [x] Count feed likes in request-local ID order, scanning at most 16 unrelated entries before seeking across gaps.

- [x] Decode index fields from SQLite's local payload; retain the overflow fallback and consume views before any further B-tree call.

- [x] Move PASSIVE checkpoints to a worker with its own SQLite connection. At 100k, p99 fell from 27.42 to 4.71 ms while CPU/request rose from 31.05 to 32.70 us.
  Coalesce 1,000-frame wakeups, defer partial/failed checkpoints and join before closing connections. Keep commits on the issuer and preserve `synchronous=NORMAL`. The 300k comparison still exposed foreground COMMIT stalls.

- [x] Drop already-checkpointed WAL pages from Linux's file cache on the checkpoint thread ([public](submissions/c-iouring-public/components/engine.c)). Mini/Docker residency probes confirmed eviction; retain NORMAL sync and pair eviction with WAL-tail truncation below.

- [x] Pair WAL eviction with `journal_size_limit=0` to avoid partial overwrites of cold pages ([public](submissions/c-iouring-public/components/engine.c)). At 200k/ext4, physical reads fell from 1475.67 to 3.56 MiB and hold p99 from 74.61 to 7.81 ms; peak RAM stayed below 2 GiB, with fresh reads and rollback tests intact.

- [x] Compare `journal_size_limit=0` alone with eviction. At 350k/NAT, eviction reduced peak RAM from 1513 to 956 MiB and median hold file cache from 501 to 253 MiB; retain eviction. Truncation needs a WAL reset, while eviction can release completed prefixes earlier.

- [x] Group up to 16 queued writes without added delay: `C(b) ~= s + a/b`.
  Own job data, isolate failures with savepoints and release responses after commit. Keep reads outside the write transaction.

- [x] Reuse prepared transaction/savepoint statements instead of parsing control SQL per write. Keep failure injection on `sqlite3_step` and verify recovery before reuse.

- [x] Prepare ROLLBACK before admission and quarantine unresolved transactions. Test preparation failure, failed read/write rollback, blocked subsequent access and clean reopening.

- [x] Use plain create INSERT plus `last_insert_rowid` and stored timestamp lookup instead of `RETURNING` ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)). Native batch-16 CPU/write fell 15%; IDs and timestamps matched storage. Failed readback invalidates the whole batch.

- [x] Compare fused like INSERTs using `WHERE EXISTS` and direct `SELECT FROM posts` ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)). Both lost with SQL fallback; Pavly's B-tree fallback still lost 4-10% at batch 16 with foreign keys enabled. SELECT adds internal statement rollback and coroutine work.

- [x] Keep VALUES for likes and look up post existence only after duplicate or genuine FK failure. Use a transaction-local B-tree cursor with SQL fallback; retain FK checks and missing-post precedence. Batch-16 CPU/write fell 4.4% for fresh likes and 10.4% for duplicates; 350k/5m passed with no errors.

- [x] Compare point lookups with bounded range scans for feed posts. Native feed CPU fell from 10.58 to 9.06 us; whole-server CPU stayed nearly unchanged at 300k.
  `C_point ~= n*a_seek + k_target*s_scan`; `C_range ~= a_seek + k_span*s_scan`.
  Start with a small decision tree of measured scan/seek thresholds; an ML forest is unnecessary for this choice. Bound skipped rows before seeking across gaps; a narrow ID span can still contain many likes.
  Collect IDs within one fresh feed snapshot, scan in ID order and restore timestamp order with request-local slots, including gaps and reordered timestamps.

- [x] Deduplicate author IDs within one feed and read them in ID order. Copy post records and author names into request-local arenas; retain the SQL point-read path and remove the unused B-tree point-read API.

- [x] Count likes-index cells per leaf before advancing the cursor ([Kensac](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/42981018dc0d4b157d48c22ba15da5c361bf3168/submissions/rust-iouring-kensac/csrc/shim.c#L362)). Keep bounded gaps, signed 64-bit counts, interior entries and generic/SQL fallbacks. SQL and sanitizer checks passed; native feed CPU fell 8.2% on seed data, 58.9% with dense likes and 20.1% with sparse feed IDs.

- [x] Compare removing duplicate feed initialization, together and close-only. Close shrank by 48 bytes, but five native pairs showed no repeatable CPU gain; keep the existing clears. Retain tests for partial/error/empty reuse and zero error/DONE outputs.

- [x] Use `unix-excl` on both database handles ([Kensac](submissions/rust-iouring-kensac/csrc/shim.c), [Pavly](submissions/c-iouring-pavlyhalim/src/db.h)). A small SQLite probe reduced 67,080 measured fcntl64 calls to zero. The short 70k pair used 2.5% less CPU/request with nearly unchanged p99. External SQLite processes must wait until shutdown; keep the checkpoint handle and NORMAL sync.

- [x] Compare SQLite's own page-cache sizes with `mmap_size=0`: `N ~= (RAM - fixed_cost - cache - queues)/memory_per_connection`. SQLite's page cache is explicitly allowed by [rule 5](README.md#rules).
  Use a 16-MiB budget; random post reads improved without the larger cache's feed regression.
  Keep one database at `SQLITE_PATH`; shard transport metadata and work queues, not database files or schema. Extra connections can isolate I/O waits but share one vCPU and one WAL writer.

### Load generation and compilation

- [x] Build a small native load generator matching `bench/load.js`: shared tokens, per-user connections and state, the same ramps, think times, write probabilities, checks and latency definitions.
  Use an event loop and timers instead of a JavaScript runtime per user. Compare small runs against k6 before using it for capacity experiments; keep k6 as the reference.

- [x] Bound the load generator's due-timer pass to 256 services / 1 ms, checked between calls. Record time from epoll return to the next call separately from dispatch lag. A single blocking syscall can still exceed the budget; causation of earlier server tails remains unproven.

- [x] Keep timer metadata together, move heap entries only in the required direction, skip unchanged epoll registrations and remove receive-buffer zeroing. Ordering, partial sends, reconnects and exact latency tests pass; storage remains 88 bytes/VU.

- [x] Prepare first-use sockets before the start gate and cache immutable HTTP request bytes. Actual TCP connects and requests stay at their scheduled wake; fresh feed IDs and full response checks remain. Repeated short 350k runs retain CPU reserve ([results](src/runs/49f625b/generator-capacity-comparison/summary.json)).

- [x] Match the healthy `bench/load.js` workflow: 15% likes and independent 2% creates, with the same think-time ranges. Exclude local request preparation from HTTP duration and start think time after response processing. k6 remains the reference for redirects and automatic stale-connection retries.

- [x] Fix WSL mirrored-networking source-bind delays with NAT and reject runs that miss the activation deadline.

- [x] Compare PGO trained on fresh synthetic data with plain optimization ([build.sh](submissions/c-iouring-pavlyhalim/build.sh)). Matched 350k: CPU/request -2.9%, hold p99 5.87 to 38.71 ms. Discard the implementation and retain the results; LTO and ISA flags were unchanged.
