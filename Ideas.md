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
  Use SQLite's own APIs; a separate engine for its file format is outside the rules.
  Reference: [bt_seek_many](submissions/c-iouring-pavlyhalim/src/btree.h).

- [ ] Separate read and response budgets. Store pending work cheaply and keep long processing batches from starving I/O.
  `M_queue ~= Q_kernel*m_kernel + Q_app*m_app`.
  Moving the queue saves memory only if the new representation is cheaper. Sustained arrivals above completion capacity still grow the backlog.
  Batch readiness, reads or sends while each request still gets a fresh database read. This shares scheduling and I/O work, not query results.
  Reference: [read/response budgets in PR #245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245).

- [ ] Tune SQLite's page cache against connection memory.
  `N ~= (RAM - fixed_cost - cache - queues) / memory_per_connection`.
  A larger cache may save CPU but leave less room for connections.
  SQLite pages, mmap and prepared plans are allowed reuse. Retaining data-dependent response bytes, rows, table copies or successful JWT checks for another request is forbidden.

- [ ] Measure checkpoint timing, WAL growth and p99 together. Compare checkpoint work and priority before assuming every commit costs an fsync.

- [ ] Try coarse native calls, such as one call per feed or batch. Compare Python, suitable Numba kernels and C on the same inputs. Include JIT startup, memory, FFI and copy costs.
  Ideal CPU speedup: `S <= 1 / ((1-p) + p/s)`, for accelerated share p and speedup s.
  Inspect compiler/LLVM output before trying handwritten assembly.

- [ ] Clarify in-flight read sharing in [issue #261](https://github.com/arjaythedev/twelve-dollar-server-challenge/issues/261), still open without a ruling. Keep this experiment pending.
  Singleflight shares one fresh result among overlapping identical GETs and discards it afterward. Its short lifetime does not remove the cross-request sharing.
  Distinguish this from one `WHERE id IN (...)` query for different pending requests. Check freshness and ordering around writes in both cases.
  [sql-oneselect #243](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/243) was scored with an unresolved rule note in the [local leaderboard](LEADERBOARD.md), which does not settle permission.

- [ ] Use the workload model when comparing results:
  `requests/s ~= 2.17*N / (20.5 + HTTP_wait_per_cycle)`.
  More users do not mean the same number of simultaneous requests. Compare throughput, latency, errors and CPU/request together.
