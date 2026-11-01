# Ideas

- [ ] Profile the feed first. It returns 20 posts per read; the post endpoint returns one. Separate SQL, object creation and JSON costs before moving code to C.

- [ ] Group writes into one transaction, with a size limit b and a timeout.
  Fixed cost a, per-write cost s: `C(b) ~= s + a/b`.
  At write rate lambda, collecting b writes adds roughly `(b-1)/(2*lambda)` average wait, before execution and commit. Dependent responses must wait for commit; test rollback and commit failure too.
  Reference: [c-iouring-pavlyhalim](submissions/c-iouring-pavlyhalim/src/server.c).

- [ ] Compare point lookups with range scans for feed posts and likes.
  `C_point ~= n*a_seek`, `C_range ~= a_seek + k*s_scan`, where k counts entries actually scanned.
  Pick from measured costs. An ID span alone does not tell us how many likes we will scan.
  Reference: [bt_feed and bt_range_counts](submissions/c-iouring-pavlyhalim/src/btree.h).

- [ ] Batch author lookups within one feed request. Shared B-tree pages could save repeated searches. Keep the SQLite snapshot and required feed order.
  Reference: [bt_seek_many](submissions/c-iouring-pavlyhalim/src/btree.h).

- [ ] Separate read and response budgets. Store pending work cheaply and keep long processing batches from starving I/O.
  `M_queue ~= Q_kernel*m_kernel + Q_app*m_app`.
  Moving the queue saves memory only if the new representation is cheaper. Sustained arrivals above completion capacity still grow the backlog.
  Reference: [read/response budgets in PR #245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245).

- [ ] Tune SQLite's page cache against connection memory.
  `N ~= (RAM - fixed_cost - cache - queues) / memory_per_connection`.
  A larger cache may save CPU but leave less room for connections.

- [ ] Measure checkpoint timing, WAL growth and p99 together. Compare checkpoint work and priority before assuming every commit costs an fsync.

- [ ] Try coarse native calls, such as one call per feed or batch. Compare Python, suitable Numba kernels and C on the same inputs. Include JIT startup, memory, FFI and copy costs.
  Ideal CPU speedup: `S <= 1 / ((1-p) + p/s)`, for accelerated share p and speedup s.
  Inspect compiler/LLVM output before trying handwritten assembly.

- [ ] Clarify shared reads across requests before trying singleflight or request coalescing. SQLite page caching and prepared statements are allowed; reusing response, row or JWT-verification results across requests is forbidden.
  [sql-oneselect #243](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/243) has an unresolved rule note in the [local leaderboard](LEADERBOARD.md).

- [ ] Use the workload model when comparing results:
  `requests/s ~= 2.17*N / (20.5 + HTTP_wait_per_cycle)`.
  More users do not mean the same number of simultaneous requests. Compare throughput, latency, errors and CPU/request together.
