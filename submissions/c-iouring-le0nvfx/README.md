# C / io_uring / SQLite

This submission is a single-process HTTP/1.1 server written in C. It serves
**directly**, without Nginx, on the `HOST` and `PORT` supplied by the harness.
For direct scoring, the spec uses `HOST=0.0.0.0` and `PORT=80`; CI sets
`HOST=127.0.0.1` and `PORT=3000`. It uses the in-process SQLite C API and
verifies each HS256 token with OpenSSL.

## Dependencies and build

- HTTP/framework: a purpose-built HTTP/1.1 server in C; it uses no external
  application framework. The SQLite driver is the in-process SQLite C API
  (3.54.1), and the cryptography library is OpenSSL (3.5.9).
- Ubuntu 24.04 x86-64 system GCC and standard C toolchain (`build-essential`).
- SQLite **3.54.1**, included as source under `src/vendor/sqlite-3.54.1/`;
  its license and version details are in that directory.
- OpenSSL **3.5.9**, downloaded at build time from its official release,
  checked against the SHA-256 in `build.sh`, and linked statically as
  `libcrypto`. OpenSSL 3 is distributed under Apache-2.0.

`install.sh` installs the OS build tools, curl, Perl, and CA certificates.
`build.sh` runs as an ordinary user and creates `bin/server`; the OpenSSL
source and build outputs stay in ignored local directories. The build targets
`x86-64-v3` (including AVX2), supported by the Xeon Platinum 8260 listed in
the leaderboard's test-machine description. `start.sh` runs the server in the
foreground and reads the challenge's `HOST`, `PORT`, `SQLITE_PATH`, and
`JWT_SECRET` environment variables.

## Implementation

- Six io_uring rings use sparse fixed-file tables, multishot direct accepts,
  and 1,024 shared 2-KiB receive buffers per ring. If a multishot receive ends,
  the connection enters a FIFO recovery queue instead of being immediately
  re-armed. Bounded `MSG_DONTWAIT` reads use at most one receive per ring buffer;
  each pass answers up to 2,048 queued requests. Completion batches contain up
  to 256 entries. This bounded recovery and drain-first approach follows the
  publicly described work in
  [rust-iouring-kensac #264](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264).
  Accepted TCP sockets use the kernel's default send/receive buffer autotuning
  with `TCP_NODELAY`. An epoll implementation is available if io_uring is
  unavailable.
- The B-tree traversal helpers in `src/btree_feed.h` are adapted from
  [c-iouring-pavlyhalim #245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245).
- Reads access the supplied SQLite database on each request. The feed and post
  fast paths use SQLite B-tree cursors against the existing schema and indexes,
  with prepared SQL fallbacks. No response, row, or JWT-verification result is
  cached across requests. SQLite's own page cache is configured to 64 MiB;
  SQLite memory mapping is disabled. A `SQLITE_CONFIG_PCACHE2` wrapper skips a
  page-cache truncate only when its tracked page keys prove that no cached page
  can be beyond the limit; uncertain cases use SQLite's original truncate. The
  approach is inspired by the public truncate-skip work in
  [Go io_uring PR #343](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/343),
  which credits its underlying idea to
  [C io_uring PR #245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245).
- Writes use WAL and `synchronous=NORMAL`. Multiple writes may share a SQLite
  transaction; affected responses are held until commit succeeds. A background
  checkpoint thread runs at nice 5, performs up to four passive bulk rounds
  before holding writes for the remaining tail, and syncs the database around a
  final passive checkpoint. SQLite uses 64 MiB database-file chunks backed by
  `posix_fallocate`; the VFS coalesces WAL writes while preserving SQLite's I/O
  barriers. The checkpoint approach adapts the documented technique from
  [Go io_uring PR #354](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/354).
  The program can advise the kernel to release already-checkpointed WAL pages.

The code makes no schema or index changes and does not modify kernel settings
or pin CPUs. Dependency and source licenses are retained with the vendored
SQLite source; the submission is offered under the challenge repository's MIT
license.

## Checks

Run the challenge's `test/run.sh submissions/c-iouring-le0nvfx` from the
challenge repository with a freshly generated seed. Optional additional checks:

- `tests/test_spec_edges.py --start ./start.sh --seed /path/to/feed.db`
  checks long bodies, JSON and Unicode edge cases.
- `tests/test_wal_vfs.sh` checks the buffered WAL VFS commit-frame path.
- `tests/test_commit_persistence.py --start ./start.sh --seed /path/to/feed.db`
  checks acknowledged writes after a server-process SIGKILL and restart.
- `tests/test_auth_subject_overflow.sh http://127.0.0.1:3000` checks signed
  oversized JWT subjects against an already running server with disposable data.

## Official benchmark history

- The maintainer's official run at submission commit `3e10490` held **220,000
  users for five minutes**: 21,431 req/s, p95 142.3 ms, p99 633.1 ms, zero
  errors, 82% app CPU, and 702 MB peak RAM.
- The 2026-11-08 rerun at `5318219` used the same submission files and held
  **210,000 users for five minutes**: 20,573 req/s, p95 21.7 ms, p99 256.0 ms,
  zero errors, 78% app CPU, and 670 MB peak RAM. Its 220,000-user two-minute
  probe failed p95/p99 at 0.55/1.75 s.
- A further official rerun at README-only head `9748f27` held **209,000 users
  for five minutes**: 20,438 req/s, p95 49.6 ms, p99 608.4 ms, zero errors,
  78% app CPU, and 700 MB peak RAM. The 220,000-user two-minute probe passed
  (p99 331.6 ms), but its five-minute hold failed p95/p99 at 2.93/6.63 s
  (84% CPU); the 230,000-user two-minute probe failed p99 at 1.45 s.
- The maintainer reran the same executable tree at README-only head `66aff42`:
  **209,000 users for five minutes**, 20,477 req/s, p95 18.7 ms, p99 218.3 ms,
  zero errors, 76% app CPU, and 681 MB peak RAM. The 220,000-user two-minute
  probe passed (p99 870 ms), but its five-minute hold failed p99 at 1.40 s
  (82% CPU); the 230,000-user two-minute probe failed p99 at 1.49 s.
- The official run at server-change head `806aaa6`, which moved passive WAL
  checkpoints to a separate helper process, held **210,000 users for five
  minutes**: 20,565 req/s, p95 77.6 ms, p99 246.0 ms, zero errors, 72% app
  CPU, and 736 MB peak RAM. The 220,000-user two-minute probe failed p95/p99
  at 1.00/2.15 s; the 240,000-user probe failed p95 at 2.7 s. The maintainer
  confirmed the helper was rule-compliant but it did not beat the 220,000 row.
- The official rerun at `0012110`, which restored the same thread-based server
  tree as `3e10490`, held **200,000 users for five minutes**: 19,684 req/s,
  p95 12.1 ms, p99 46.2 ms, zero errors, 74% app CPU, and 636 MB peak RAM.
  The 210,000-user two-minute probe failed p99 at 1.99 s; 220,000 failed p99
  at 2.4 s. The maintainer says this same tree has varied from 209,000 to
  220,000 across runs, so this result is recorded as a footnote, not a new row.
- The official rerun at `3949a1a` uses the same server files and held **219,000
  users for five minutes**: 21,366 req/s, p95 69.5 ms, p99 231.7 ms, zero
  errors, 81% app CPU, and 1,185 MB peak RAM. A 230,000-user two-minute probe
  passed but its five-minute hold failed p99 at 1.08 s (85% CPU); 240,000
  failed p95 at 1.0 s. The leaderboard row remains 220,000 at `3e10490`.

The maintainer attributes the differences to run-to-run variation around the
210,000–220,000-user edge. The leaderboard retains the 220,000 row from
`3e10490` and records the subsequent 210,000, 209,000, 200,000, and 219,000
runs in its footnote. The helper-process experiment was reverted; `3949a1a`
uses the same server tree as `3e10490`. The current checkpoint-tuning candidate
is unscored; these are official maintainer results, and local diagnostics are
not leaderboard scores.
