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
- JWT verification precomputes the HMAC-SHA256 inner and outer pad states at
  startup, then clones those states for each request. Every request still
  verifies its signature and uses a constant-time comparison. This follows the
  published key-schedule approach in
  [rust-iouring-kensac #264](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264).
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
  checkpoint thread manages WAL growth, and the VFS coalesces WAL writes while
  preserving SQLite's I/O barriers. After a complete checkpoint, the program
  may use `posix_fadvise(POSIX_FADV_DONTNEED)` on a range of its own WAL file to
  release clean cached pages; it does not change system-wide kernel settings.

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
