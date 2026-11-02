# Ideas

- [x] Read the feed through SQLite's own B-tree cursors, pager, WAL and cache, skipping the VM. Keep SQL for point reads.

- [x] Count feed likes in request-local ID order, scanning at most 16 unrelated entries before seeking across gaps.

- [ ] Compare point lookups with range scans for feed posts.
  `C_point ~= n*a_seek + k_target*s_scan`; `C_range ~= a_seek + k_span*s_scan`.
  Start with a small decision tree of measured scan/seek thresholds; an ML forest is unnecessary for this choice. Bound skipped rows before seeking across gaps; a narrow ID span can still contain many likes.
  Collect IDs within one fresh feed snapshot, scan in ID order and restore timestamp order with request-local slots.

- [ ] Deduplicate author IDs within one feed, then batch lookups by B-tree level ([bt_seek_many](submissions/c-iouring-pavlyhalim/src/btree.h)).
  Interleave independent descents and fetch shared pages once. Try interpolation from the current page's endpoint keys, with bounded correction and a correct binary-search fallback.
  Keep all lookup state within the request. Pin author pages until serialization; copy post bytes before moving cursors. Persistent learned indexes or Eytzinger copies would reuse database-derived data across requests.

- [x] Decode index fields from SQLite's local payload; retain the overflow fallback and consume views before any further B-tree call.

- [ ] Group writes with a size limit b and timeout: `C(b) ~= s + a/b`, average collection wait `~= (b-1)/(2*lambda)`.
  Hold dependent responses until commit; rollbacks and commit failure must replace them with errors.
  Compare `INSERT ... WHERE EXISTS ... ON CONFLICT` for likes and insert + rowid/timestamp lookup against `RETURNING` ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)).

- [ ] Measure requests/enter, CQ batch sizes and CPU/request before ring flags: `C ~= C_enter/requests_per_enter + C_other`.
  Test `SINGLE_ISSUER`/`DEFER_TASKRUN` with submission, registration and cleanup owned by the issuer.
  Preserve the 16-CQE/2-ms busy wait's idle, accept and shutdown bypasses. Adapt batching to queue depth and oldest response age; cap CQ drains by count or elapsed time, submit pending sends promptly and avoid waiting while application work is ready.
  Try `IORING_RECVSEND_POLL_FIRST` when rearming an idle keep-alive receive, avoiding an initial empty read ([liburing](https://github.com/axboe/liburing/blob/liburing-2.15/man/io_uring_prep_recv.3)).

- [ ] Compare direct descriptors and multishot readiness with our single-shot receive pool ([sys.h](submissions/c-iouring-pavlyhalim/src/sys.h)).
  Budget socket reads and response work separately: `M_queue ~= Q_kernel*m_kernel + Q_app*m_app`. Drain input within a byte budget before expensive handlers; cap queued input and output, pause new work at a high watermark and resume below a lower one.
  Start with FIFO; compare byte-deficit scheduling for uneven response sizes, with request-age promotion to prevent starvation. Preserve connection/write order and read fresh data when dispatched.
  Receive storage is now `M_pool ~= 1024*4096` instead of `N*4096`. Recycle response buffers after send completion rather than retaining each idle connection's largest output capacity.
  Compare header headroom with the current single scatter-gather send. Track partial sends across header/body buffers; keep storage alive through completion or confirmed cancellation. Finish the database read before sending 200 so failures can still return 500.
  Multishot needs stable operation identities, terminal-CQE tracking and bounded `ENOBUFS` rearming; the current one-operation-per-connection model cannot track concurrent receive/send operations.

- [ ] Compare SQLite cache sizes with `mmap_size=0`: `N ~= (RAM - fixed_cost - cache - queues)/memory_per_connection`.
  Keep one database at `SQLITE_PATH`; file sharding changes the fixed data contract. Extra connections add no CPU capacity on one vCPU.
  Bloom filters help misses: `saved ~= miss_rate*(1-false_positive_rate)*lookup_cost - filter_cost`. Measure a miss-heavy path first; a retained application filter is data-dependent reuse across requests.
  Static encoding tables are fine; persistent author, post and count maps are data caches. Post age does not relax freshness or feed ordering.

- [ ] Measure checkpoint CPU, WAL growth and p99; try PASSIVE checkpoints on a separate, lower-priority connection.
  `unix-excl` needs one owning process. Synchronize checkpoint state explicitly.
  Profile WAL-write coalescing and redundant cache truncation before copying the leader's VFS/cache wrappers; preserve write order, flush errors and `synchronous=NORMAL` or stronger.

- [ ] Compare PGO trained on fresh synthetic data with plain optimization ([build.sh](submissions/c-iouring-pavlyhalim/build.sh)).
  SSE2 JSON/UTF-8 scanning already exists and is baseline on x86_64; test wider SIMD for measured hot loops. AVX2/AVX-512 need matching target support or runtime dispatch.
  `S <= 1/((1-p) + p/s)`, with p measured against total CPU including kernel work.

- [ ] Target the Xeon Platinum 8260 (Cascade Lake): compare `-mtune=cascadelake`, then ISA-specific kernels after checking the VPS feature mask ([GCC](https://gcc.gnu.org/onlinedocs/gcc-13.1.0/gcc/x86-Options.html)).
  Compare SSE2, AVX2 and AVX-512 with runtime dispatch. Batch safe JSON bytes into one append per run before widening vectors; keep surrogate and bounds handling.

- [ ] Compare pinned GCC 16.2 with GCC 13.3 on the same C++23 code; test C++26 separately when a feature helps ([releases](https://gcc.gnu.org/releases.html), [standard support](https://gcc.gnu.org/projects/cxx-status.html)).

- [ ] Keep GET sharing pending [#261](https://github.com/arjaythedev/twelve-dollar-server-challenge/issues/261), open and unanswered.
  This includes cross-request `IN (...)` reads, flagged in [#260](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/260). Use independent reads until clarified.
  [#243](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/243) restored per-request feed reads (`57cf5b2`).

- [ ] Compare throughput, latency, errors and CPU/request together:
  `requests/s ~= 2.17*N/(20.5 + HTTP_wait_per_cycle)`.
  User count is not simultaneous request count.
