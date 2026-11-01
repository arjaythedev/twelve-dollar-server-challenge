# Ideas

- [ ] Compare feed SQL with [the leader's B-tree reads](submissions/c-iouring-pavlyhalim/src/btree.h): SQLite's own cursors, pager, WAL and cache, skipping the VM.
  Pin SQLite's version and validate schema/layout; close cursors within the transaction and fall back to SQL on unsupported records.

- [ ] Compare point lookups with range scans for feed rows and likes.
  `C_point ~= n*a_seek + k_target*s_scan`; `C_range ~= a_seek + k_span*s_scan`.
  Choose from measured costs; a narrow ID span can still contain many likes. Keep timestamp/ID order, including gaps and reordered timestamps.

- [ ] Batch author lookups by B-tree level, share page fetches and prefetch searches ([bt_seek_many](submissions/c-iouring-pavlyhalim/src/btree.h)).
  Try interpolation with a correct binary-search fallback. Pin author pages until serialization; copy post bytes before moving cursors.

- [ ] Group writes with a size limit b and timeout: `C(b) ~= s + a/b`, average collection wait `~= (b-1)/(2*lambda)`.
  Hold dependent responses until commit; rollbacks and commit failure must replace them with errors.
  Compare `INSERT ... WHERE EXISTS ... ON CONFLICT` for likes and insert + rowid/timestamp lookup against `RETURNING` ([db.h](submissions/c-iouring-pavlyhalim/src/db.h)).

- [ ] Measure requests/enter, CQ batch sizes and CPU/request before ring flags: `C ~= C_enter/requests_per_enter + C_other`.
  Test `SINGLE_ISSUER`/`DEFER_TASKRUN` with submission, registration and cleanup owned by the issuer.
  Keep the busy-only 16-CQE/1-ms wait as the baseline.

- [ ] Compare direct descriptors, multishot readiness and provided buffers with our per-socket operations ([sys.h](submissions/c-iouring-pavlyhalim/src/sys.h)).
  Budget reads and answers separately: `M_queue ~= Q_kernel*m_kernel + Q_app*m_app`. Keep queued work small and bound turns.
  Reuse response arenas after send completion, including partial sends and cancellation.

- [ ] Compare SQLite cache sizes with `mmap_size=0`: `N ~= (RAM - fixed_cost - cache - queues)/memory_per_connection`.
  Keep one database at `SQLITE_PATH`; file sharding changes the fixed data contract. Extra connections add no CPU capacity on one vCPU.
  Bloom filters help misses: `saved ~= miss_rate*(1-false_positive_rate)*lookup_cost - filter_cost`. Measure a miss-heavy path first; a retained application filter is data-dependent reuse across requests.

- [ ] Measure checkpoint CPU, WAL growth and p99; try PASSIVE checkpoints on a separate, lower-priority connection.
  `unix-excl` needs one owning process. Synchronize checkpoint state explicitly.
  Profile WAL-write coalescing and redundant cache truncation before copying the leader's VFS/cache wrappers; preserve write order, flush errors and `synchronous=NORMAL` or stronger.

- [ ] Compare PGO trained on fresh synthetic data with plain optimization ([build.sh](submissions/c-iouring-pavlyhalim/build.sh)).
  SSE2 JSON/UTF-8 scanning already exists and is baseline on x86_64; test wider SIMD for measured hot loops. AVX2/AVX-512 need matching target support or runtime dispatch.
  `S <= 1/((1-p) + p/s)`, with p measured against total CPU including kernel work.

- [ ] Keep GET sharing pending [#261](https://github.com/arjaythedev/twelve-dollar-server-challenge/issues/261), open and unanswered.
  Seal fully validated GET cohorts before a fresh SQLite snapshot/read; no later joiners. Split reads at write barriers; never share JWT verification results.
  Expected batch size `B ~= 1 + lambda_key*W`, with collection window W ending before the read.
  [#243](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/243) restored per-request feed reads (`57cf5b2`).

- [ ] Compare throughput, latency, errors and CPU/request together:
  `requests/s ~= 2.17*N/(20.5 + HTTP_wait_per_cycle)`.
  User count is not simultaneous request count.
