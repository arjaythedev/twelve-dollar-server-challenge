# C io_uring SQLite submission

This is a derivative of the MIT-licensed C submission [`c-iouring-pavlyhalim` PR #245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245), using its source at commit [`7387a313`](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/7387a313d58041c76e0fe8134825ef335eeafd0d/submissions/c-iouring-pavlyhalim). Its io_uring transport, SQLite B-tree paths, WAL handling, HTTP/JWT implementation, and PGO training code are substantially reused. See [CREDITS.md](CREDITS.md) and [LICENSE](LICENSE).

The upstream #245 version recorded 295,000 users on the official 1-vCPU/2-GB challenge host. This derivative has not yet been scored. The previous le0nvfx implementation scored 210,000 users at `5e0cb7e`; the current candidate is awaiting its own official run.

## Runtime

- C11, GCC, no application framework, one event-loop thread; serves directly on the challenge-provided `HOST` and `PORT` without Nginx.
- Up to eight io_uring rings are created as connections require them, with eight `SO_REUSEPORT` listeners. Accepted sockets use sparse direct-descriptor tables.
- Idle sockets wait on multishot poll requests and hold no receive buffer. Readiness queues bounded one-shot `MSG_DONTWAIT` reads into provided-buffer rings; reads drain ahead of the bounded response work after a stall. Ring 0 uses deferred task running, and eventfds wake its wait when another ring completes work.
- Feed and post reads traverse SQLite B-trees through SQLite's pager, using the supplied schema and indexes. Layout checks and prepared-SQL fallbacks preserve correctness. Every request reads from the supplied database; no response, row, query-result, or JWT-verification result is cached.
- Writes use prepared SQL, WAL and `synchronous=NORMAL`. Requests in a batch share a transaction, and write responses are sent only after commit. A background thread performs passive checkpoints; the WAL VFS coalesces sequential frame writes while preserving SQLite's ordering and barriers.
- PGO training uses a synthetic database generated at build time, not the challenge seed. It only profiles the compiled server and makes no request-time decisions based on the workload.

## Build

`install.sh` installs GCC and build tools on Ubuntu 24.04. `build.sh` verifies the vendored SQLite 3.54.1 amalgamation by SHA-256 and builds a static binary from source. The exact amalgamation source ID and hashes are recorded in `src/vendor/sqlite-3.54.1/README.md`. The default build trains and applies GCC PGO using the synthetic workload; `PGO=0 ./build.sh` makes a plain optimized build. `start.sh` runs the server in the foreground and reads only `HOST`, `PORT`, `SQLITE_PATH`, and `JWT_SECRET`.

## Verification and score history

The official challenge harness passed **42/42** checks on this candidate using a freshly copied official seed. A separate durability check confirmed that 64 acknowledged writes survived server `SIGKILL`, SQLite recovery, and restart. These are correctness checks, not load scores.

Only the challenge maintainer's run determines a leaderboard result. The 295,000-user figure above belongs to upstream #245; no score is claimed for this derivative until it is benchmarked on the official 1-vCPU/2-GB host.

The implementation keeps the challenge's schema, indexes, SQLite database path, and process limits. It does not tune kernel settings or pin CPUs.
