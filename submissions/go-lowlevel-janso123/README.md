# go-lowlevel-janso123

Go **1.27.2**, **LLVM/Clang 23.1.3** and embedded **SQLite 3.53.4**.
A Linux HTTP/1.1 server using io_uring direct descriptors, prepared SQLite reads,
a batched writer and reusable send buffers. No response or JWT caching;
each request reads the database and verifies its authentication.

## Install and run

Requires **Ubuntu 24.04, Linux x86-64**. The default build requires **x86-64-v3**.
Install as root, then build and run as the ordinary submission user:

```sh
sudo ./install.sh
./build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=your-secret HOST=0.0.0.0 PORT=80 ./start.sh
```

The runner must grant permission to bind port 80 (`CAP_NET_BIND_SERVICE`).
The installer downloads and checksum-verifies the pinned Go and LLVM toolchains.
Go compiles the Go code; Clang compiles SQLite and the C bindings.

## Build options

**Go PGO + SQLite PGO + ThinLTO are enabled by default**, using the included
profiles. All builds use O2. Each optimization can be disabled independently:

```sh
./build.sh --test          # default build plus Go tests
./build.sh --no-go-pgo     # disable Go PGO
./build.sh --no-pgo        # disable SQLite PGO; detect v3, fall back to v1
./build.sh --no-thinlto    # disable ThinLTO
./build.sh --baseline --no-thinlto  # v1, both PGO modes and ThinLTO disabled
```

Flags can be combined. `--baseline` disables both PGO modes and forces v1;
ThinLTO stays enabled unless disabled explicitly. Custom Go profiles require
both `--go-pgo PATH` and `--go-pgo-sha256 HASH`, and conflict with `--baseline`
and `--no-go-pgo`.

Missing or incompatible profiles fail the build; there is no silent fallback.
Build and startup never run profile training. Profile origins, hashes and
training limitations are documented in [profiles/README.md](profiles/README.md).
Their use is disclosed for organizer review, not claimed to be pre-approved.

## Design

- One event loop handles HTTP and reads; a dedicated writer batches transactions.
  Write success is returned only after commit.
- SQLite uses WAL with `synchronous=NORMAL`; the challenge schema and indexes
  are unchanged. Read results are encoded through a small C binding.
- Reusable send buffers and a memory guard reduce allocations and limit pressure.
- io_uring direct descriptors are preferred, with ordinary io_uring/epoll fallback.
  Two direct tables provide 131,070 slots when the inherited hard descriptor
  limit is 65,535. The server raises a lower soft limit to that hard limit;
  tests used both limits at 65,535. Ordinary backends retain normal FD limits.

Overload can return HTTP 503; passing ordinary API checks does not validate every
such case. Source attribution and licenses are retained in the code.

## Validation and results

The full installer passed on our **1-vCPU / 2-GiB Ubuntu VM**. Native builds
with and without Go PGO passed Go tests, all **42 API checks**, and the
`MAX_CONNECTIONS=1` regression. The new default-selection CLI passed **71 local
tests** and a native `./build.sh --test` run on the server VM, followed by all
42 API checks and the `MAX_CONNECTIONS=1` regression. Non-PGO ThinLTO v1/v3 builds also remain
unverified natively.

Three sequential **120,000-VU** runs used our separate Rust diagnostic generator,
fresh databases, port 80 and io_uring-direct. All passed the configured thresholds
(p95 < 500 ms, p99 < 1000 ms, errors < 1%):

- Run 1: p95 **192.52 ms**, p99 **744.48 ms**, **11,164.92 req/s**, 568 errors (0.012718%).
- Run 2: p95 **30.74 ms**, p99 **130.11 ms**, **11,195.03 req/s**, 1 error (0.000022%).
- Run 3: p95 **127.38 ms**, p99 **696.26 ms**, **11,171.02 req/s**, 1 error (0.000022%).

The tested configuration was **Go PGO + SQLite PGO + ThinLTO**. Each run had a
separate warm-up, then 60 seconds ramp-up, 300 seconds hold, 30 seconds ramp-down
and 10 seconds grace. Percentiles cover that full profile; the actual VU plateau
was not independently measured. All runs shut down cleanly.

These are **diagnostic results, not an official contest score**. Tail latency
varies substantially, and passing thresholds does not mean error-free operation.
The generator is not included in this submission.
