# Ideas

- [x] Read the feed through SQLite's own B-tree cursors, pager, WAL and cache, skipping the VM. Keep SQL for point reads.

- [x] Count feed likes in request-local ID order, scanning at most 16 unrelated entries before seeking across gaps.

- [x] Decode index fields from SQLite's local payload; retain the overflow fallback and consume views before any further B-tree call.

- [x] Share 1,024 receive buffers of 4 KiB and handle exhaustion, rearming and cancellation. `M_pool ~= 1024*4096` replaces `N*4096` receive storage.

- [x] Send headers and the moved response body with one scatter-gather operation; release body storage after the final send completion.

- [x] Use a busy-only 32-CQE / 4-ms wait with idle, accept and shutdown bypasses. An 8-ms wait did not reduce CPU further.

## Next transport comparisons

- [x] Scale direct descriptors across bounded io_uring rings on one thread ([sys.h](submissions/c-iouring-pavlyhalim/src/sys.h)). Direct accept avoids installing ordinary process FDs.
  Keep fixed-file slot indices separate from OS descriptors; a slot index must never reach `Descriptor` or `close()`. Route operations by ring/slot and recycle a slot only after its close CQE.
  The official `RLIMIT_NOFILE=65535` caps each ring's sparse table. Add rings lazily, up to 16, to exceed the ordinary FD limit without raising it. Bound total ring/queue/pool memory.

- [ ] Bound CQ draining and handler work before extending completion waits further. Network/socket paths account for 38.2% and io_uring for 12.8% of sampled CPU in the [combined profile](src/profiles/combined-50000.json).
  Measure requests/enter, CQ batch sizes and CPU/request: `C ~= C_enter/requests_per_enter + C_other`. Adapt batching to queue depth and oldest response age; cap CQ drains by count or elapsed time, submit pending sends promptly and avoid waiting while application work is ready.
  Budget socket reads and response work separately: `M_queue ~= Q_kernel*m_kernel + Q_app*m_app`. Drain input within a byte budget before expensive handlers; cap queued input and output, pause new work at a high watermark and resume below a lower one.
  Start with FIFO; compare byte-deficit scheduling for uneven response sizes, with request-age promotion to prevent starvation. Preserve connection/write order and read fresh data when dispatched.
  Test multishot accept separately to reduce accept SQEs and rearming. Count accepted sockets and terminal CQEs correctly, close arrivals during shutdown and rearm after termination.
  First compare one pending accept with 64 reserved-slot tickets on one listener. Use a controlled 20-ms handler stall and measure accepted CQEs, first-response latency and listen overflows; retain each ticket through cancellation and late successful accepts.
  Keep submission, registration and cleanup with one issuer when comparing `SINGLE_ISSUER`/`DEFER_TASKRUN`. Isolate the contribution of the current `IORING_RECVSEND_POLL_FIRST` idle-receive path ([liburing](https://github.com/axboe/liburing/blob/liburing-2.15/man/io_uring_prep_recv.3)).

- [x] Compare direct receive-buffer parsing against the 4-ms / 32-CQE baseline. The first pair saved memory without a CPU gain; keep the current path for now.
  Parse leased receive bytes directly with llhttp; copy only unconsumed tails across callbacks or sends. Release the lease before auth/handler callbacks and preserve early auth errors, fragments and pipeline order.
  Measure request-field allocations separately before adding views. Fields that survive a receive lease need request-owned storage before the lease is released.

- [ ] Measure a hybrid send threshold: contiguous `send()` for small responses, one `sendmsg()` for larger bodies.
  Compare `C_send ~= a_send + B*c_copy` with `C_sendmsg ~= a_sendmsg`; copying B bytes can cost less than vector setup for a small body. Find the crossover on mixed response sizes.
  Compare header headroom with moving the body and inserting the header; insertion still moves B bytes. Keep one operation per attempt and release large storage after completion so idle connections retain the accepted memory savings.
  Track partial sends across header/body boundaries; keep storage alive through completion or confirmed cancellation. Finish the database read before sending 200 so failures can still return 500.
  Consider a zero-copy send only for a measured large-response case; account for notification CQEs and the longer buffer lifetime.

## Then: storage and measured hot paths

- [x] Share immutable parser callbacks and use small parser/connection FSMs. API handlers already live for the server's lifetime; request state and JWT verification remain per request.
- [ ] Test an event bus only if multiple event consumers justify its routing and lifetime costs.
- [ ] Move cold parser/buffer/send state behind a lazily owned Exchange ([Rust](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/4298101/submissions/rust-iouring-kensac/src/main.rs#L133)). Estimated storage: `64*N + 584*A`, plus payloads, pool and allocator costs; N is open connections, A is active exchanges. Keep each parser alive through fragments, auth, pipeline tails and terminal I/O CQEs.
  Bound recycled Exchange/output capacity and discard oversized buffers; a slow send must not pin unrelated responses.
- [ ] Compare encoded CQE identities `(event, ring, slot, generation)` with the two Operation records ([C](submissions/c-iouring-pavlyhalim/src/server.c)). Preserve cancellation matching, generation wrap and stale-buffer returns.

- [ ] Measure checkpoint CPU, WAL growth and p99 before changing write scheduling. Files/locks/page cache account for 10.9% of sampled CPU; this includes reads and locking as well as writes.
  Try PASSIVE checkpoints on a separate, lower-priority connection. `unix-excl` needs one owning process. Synchronize checkpoint state explicitly.
  Profile WAL writes separately from checkpoints; preserve write order, flush errors and `synchronous=NORMAL` or stronger.

- [ ] Group writes with a size limit b and timeout: `C(b) ~= s + a/b`, average collection wait `~= (b-1)/(2*lambda)`.
  Measure write arrival rate and transaction cost before choosing b or the collection window. Hold dependent responses until commit; rollbacks and commit failure must replace them with errors.
  Compare `INSERT ... WHERE EXISTS ... ON CONFLICT` for likes and insert + rowid/timestamp lookup against `RETURNING` ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)).

- [ ] Compare point lookups with range scans for feed posts when SQL/row work, currently 21.8% of sampled CPU, remains a target.
  `C_point ~= n*a_seek + k_target*s_scan`; `C_range ~= a_seek + k_span*s_scan`.
  Start with a small decision tree of measured scan/seek thresholds; an ML forest is unnecessary for this choice. Bound skipped rows before seeking across gaps; a narrow ID span can still contain many likes.
  Collect IDs within one fresh feed snapshot, scan in ID order and restore timestamp order with request-local slots, including gaps and reordered timestamps.

- [ ] Deduplicate author IDs within one feed, then batch lookups by B-tree level ([bt_seek_many](submissions/c-iouring-pavlyhalim/src/btree.h)).
  Interleave independent descents and fetch shared pages once. Try interpolation from the current page's endpoint keys, with bounded correction and a correct binary-search fallback.
  Keep all lookup state within the request. Pin author pages until serialization; copy post bytes before moving cursors.

- [ ] Compare stable connection slabs when connection churn, allocation or connection memory is material.
  CQEs already carry `Connection*`, and each ring owns a table indexed by direct slot. Measure construction and destruction before replacing per-connection allocations.
  `M_slots ~= rings*slots_per_ring*sizeof(slot)` plus connection storage. Reuse slab slots only after all operations and cancellation completions are finished; preserve stable addresses or validate generation-tagged handles.

- [ ] Compare receive-pool sizes such as 256 x 4 KiB and 512 x 2 KiB against 1,024 x 4 KiB.
  `M_pool ~= K*B`; measure buffer occupancy, extra receives for fragmented requests, `ENOBUFS`, rearm work and latency during bursts. Reducing the current 4-MiB pool alone saves little of the full server's memory.
  Try dynamic sizing only if the static comparisons show a useful tradeoff. Bound total capacity and keep buffer IDs and leased storage valid through resizing.

- [ ] Compare process-local TCP options independently: `TCP_NODELAY` for small-response latency, `TCP_DEFER_ACCEPT` for connection churn and explicitly uncorked `TCP_CORK` for deliberate coalescing ([TCP options](https://man7.org/linux/man-pages/man7/tcp.7.html)).
  Measure packets, CPU and p99 as well as throughput. The current single header/body send already coalesces application output; corking can add delay, and deferred accept mainly affects connection establishment rather than established keep-alive traffic. Keep system-wide kernel settings unchanged.

## Larger or conditional experiments

- [x] Build a small native load generator matching `bench/load.js`: shared tokens, per-user connections and state, the same ramps, think times, write probabilities, checks and latency definitions.
  Use an event loop and timers instead of a JavaScript runtime per user. Compare small runs against k6 before using it for capacity experiments; keep k6 as the reference.

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
  Keep one database at `SQLITE_PATH`; file sharding changes the fixed data contract. Extra connections add no CPU capacity on one vCPU.

- [ ] Compare PGO trained on fresh synthetic data with plain optimization ([build.sh](submissions/c-iouring-pavlyhalim/build.sh)).
  SSE2 JSON/UTF-8 scanning already exists and is baseline on x86_64; test wider SIMD for measured hot loops. AVX2/AVX-512 need matching target support or runtime dispatch.
  `S <= 1/((1-p) + p/s)`, with p measured against total CPU including kernel work.
  JSON is currently 2.1% of sampled CPU, so use a fresh profile before spending time on vector width or more encoding specializations.

- [ ] Compare ISA-specific kernels for the Xeon Platinum 8260 (Cascade Lake) after checking the VPS feature mask ([GCC](https://gcc.gnu.org/onlinedocs/gcc-13.1.0/gcc/x86-Options.html)). The local `-mtune=cascadelake` comparison is complete.
  Compare SSE2, AVX2 and AVX-512 with runtime dispatch; keep surrogate and bounds handling.

- [ ] Test C++26 when a specific feature simplifies a hot path; keep compiler upgrades separate from language changes ([standard support](https://gcc.gnu.org/projects/cxx-status.html)).

- [ ] Compare throughput, latency, errors and CPU/request together:
  `requests/s ~= 2.17*N/(20.5 + HTTP_wait_per_cycle)`.
  User count is not simultaneous request count.
