# Rust native feed server (`rust-native-M3M0KA`)

Clean-sheet native Rust + C implementation of the feed API built directly on Linux `io_uring` (with automatic `epoll` fallback) and stock SQLite `3.54.1`. No web framework, no async runtime, and no third-party Rust crates.

**Serving topology: direct** on `HOST:PORT` (`0.0.0.0:80` on the benchmark box). No reverse proxy.

## Build and run

```bash
sudo bash install.sh        # Ubuntu 24.04: C toolchain + SHA-256-verified Rust 1.94.0 under /opt/twelve-rust
bash build.sh               # Pinned liburing + SQLite 3.54.1 amalgamation, SHA-256 verified, SQLite PGO
SQLITE_PATH=/path/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` verifies `rust-1.94.0-x86_64-unknown-linux-gnu.tar.xz` against its official SHA-256 (`e8fa4185f3ef6ae32725ff638b1ecdbff28f5d651dc0b3111e2539350d03b15a`). `build.sh` verifies both `liburing` (`f4e42a515cd78c8c9cac2be14222834be5f8df2b`, SHA-256 `2bce6c36035b9bac516f1a335caa595bb300a3048c9767876a33fc1ad84d30b6`) and `sqlite-amalgamation-3540100.zip` (`0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`), compiles stock SQLite `3.54.1` with `-march=native` + PGO (`src/pgo_train.c`), and links `bin/feed` statically against `liburing.a` and `sqlite3.o`.

## Files

- `src/l2_uring.c`, `src/l2_uring.rs`: Mapped `PBUF_RING` multi-ring `io_uring` wrapper built over pinned `liburing` (`1024 × 2048 B` buffers per lane by default, zero-SQE `l2_return_buffer`, `resv[0]=1` `-EINVAL` compatibility retry, direct fixed-file accept/send/close).
- `src/l2_server.rs`: Single-owner multi-lane event loop (`MAX_LANES = 8`, `65,471` direct descriptor slots per lane = up to `523,768` direct descriptors under `LimitNOFILE=65535`), `4 × SO_REUSEPORT` `4,096`-backlog listeners balanced by min-inflight accept count, single-parse `ParsedOffsets`, drain-first CQE harvest (`DRAIN_BUDGET = 8,192`), read-before-write intra-batch execution, and bounded `MemGuard` / byte-budget admission.
- `src/main.rs`, `src/net_uring.rs`, `src/uring.rs`: Startup `l2_uring::self_check()` auto-selection (logs `transport: l2 io_uring`), automatic fallback to the 5-worker `epoll` transport (`FEED_EPOLL=1` override or when `io_uring_setup` is disallowed), and UAPI layout unit tests.
- `src/app.rs`, `src/http.rs`, `src/json.rs`, `src/crypto.rs`, `src/buffer.rs`: Zero-copy `http::respond_stream` response framing, single-pass inline UTF-8 JSON scanning, stack-buffered base64url/HS256 JWT validation, and fixed-quota retry-safe buffers.
- `src/db.rs`, `src/btree.rs`, `src/sqlite_shim.c`: Single-owner SQLite `3.54.1` connection, prepared `BEGIN`/`BEGIN IMMEDIATE`/`COMMIT`/`ROLLBACK` statements + 11-bit `dirty_stmts` mask, lazy stock B-tree cursor read plans (`256`-span `range_counts`), and an off-loop `nice(19)` bounded WAL checkpointer.
- `src/pgo_train.c`: Synthetic database PGO training workload executed during `build.sh`.
- `tests/`: `oracle.sh` (3,000-ID B-tree vs SQL differential check), `protocol.sh` (pipelined/split/close/`kill -9` durability), `slow-reader.sh`, `transaction-failure.sh`, `l2-pressure.py`, and `l2-byte-budget.py`.

## Transport & Connection Capacity

- **Primary (`l2 io_uring`)**: One process, one SQLite page cache (default `32 MiB`), and up to `8` lazily opened `io_uring` lanes (`65,471` registered fixed-file slots per lane). Sockets live in kernel fixed-file tables rather than process FD slots, keeping `/proc/self/fd` under ~`32` descriptors while scaling across hundreds of thousands of connections under `LimitNOFILE=65535`. Each lane waits on an `eventfd` via `l2_wait_many` so idle wakeups work across all active lanes without busy polling.
- **Fallback (`epoll`)**: When `FEED_EPOLL=1` is set or `l2_uring::self_check()` fails (e.g., `ENOSYS`/`EPERM` under seccomp), the server forks `FEED_WORKERS` (`5`) processes using `SO_REUSEPORT` `epoll`.

## Stock SQLite B-Tree Read Plans & Off-Loop Bounded WAL Checkpointer

- `GET /feed` and `GET /posts/:id` inspect the fixed schema and verify at startup that the B-tree plan output matches reference SQL byte for byte. Every request runs inside an explicit SQL `BEGIN` transaction and reads live SQLite pages via stock SQLite `3.54.1` B-tree cursors (`sqlite3BtreeFirst`/`Previous`/`Next`/`TableMoveto`/`IndexMoveto` + `256`-span `range_counts`). There are **no** cross-request row caches, response caches, read coalescing, or remembered JWT verifications; foreign writes on normal `unix` VFS are immediately visible on the next transaction.
- Any commit or auto-rollback failure retires/resets transaction state, emits `HTTP/1.1 500`, closes the connection without publishing provisional output or replaying creates, and preserves SQLite `WAL` + `synchronous=NORMAL` application-crash durability (`protocol.sh`: `40/40` `kill -9`).
- A dedicated `nice(19)` background thread (`wal-ckpt`) opens a second normal-`unix`-VFS `sqlite3*` connection (`busy_timeout(50)`) while a passive `sqlite3_wal_hook` (`feed_wal_hook_cb`, always returning `SQLITE_OK`) counts WAL frames:
  - Runs non-blocking `SQLITE_CHECKPOINT_PASSIVE` at `>= 4,096` WAL pages (`FEED_WAL_CKPT_PAGES` override).
  - Requests bounded inter-batch `POST` deferral (`<= 150 ms` per restart episode, while `GET`s continue uninterrupted, with a `2 s` cooldown and `65,536`-frame emergency valve) to run `SQLITE_CHECKPOINT_RESTART` back to frame `0` without blocking external `unix`-VFS readers.

## HTTP/1.1 Subset & Safety Contracts

- Supports HTTP/1.1 keep-alive connections with explicit `Content-Length`, pipelined requests answered strictly in request order, arbitrarily split reads, `Expect: 100-continue`, and `Connection: close`. Requests using `Transfer-Encoding`, ambiguous framing, invalid UTF-8 JSON, or noncanonical/padded JWTs are cleanly rejected (`400`/`401` and safe connection closure where framing is ambiguous).

## Credits & Licenses

- **SQLite `3.54.1`** (`sqlite-amalgamation-3540100.zip`): Public Domain.
- **`liburing`** (pinned commit `f4e42a515cd78c8c9cac2be14222834be5f8df2b`): MIT / LGPL-2.1 dual-licensed (Jens Axboe & contributors).
- **Rust `1.94.0` toolchain**: MIT / Apache-2.0 dual-licensed.
- **Architectural Acknowledgements**: Independently implemented here in clean-sheet Rust/C; informed by public architectural patterns documented in top challenge submissions `#264` (single-owner multi-ring direct-descriptor `io_uring`, read/write separation, off-loop managed WAL checkpointing), `#257` (row-lookup batching & work amortization), and `#245` (bounded arenas & drain-first CQE harvesting).

## Measured Results (Local Test Environment, Not Official Leaderboard Score)

All measurements below were collected on a local host and single-vCPU (`CPU 0`), `2 GiB` (`MemTotal: 2,015,196 KiB`), zero-swap Ubuntu 24.04 (`kernel 6.8.0-147-generic`) KVM guest capped at `<= 50,000` users (`bench/load.js`), and do **not** represent an official leaderboard capacity score:

- **Correctness & Safety**: Crate unit `34/34`; full ASan + LSan + UBSan DB & buffer suite `28/28` (including `12/12` injected SQLite fault/rollback tests and WAL checkpointer drop test); input safety `24/24`; official `test/test.sh` `42/42` (`l2 io_uring`, `FEED_EPOLL=1`, and seccomp auto-fallback); differential SQL `oracle.sh 3000` byte-identical; `protocol.sh` `40/40` `kill -9` durability; `test-wal-checkpointer.py` `1,051/1,051` acked writes surviving `kill -9` with `0.55 ms` max latency during a `600 ms` foreign snapshot; `6`-lane handover + `ENOBUFS` + byte-budget + slow-reader + transaction-failure gates all pass.
- **Linux `6.8.0-147-generic` KVM 6-Lane / 8-Lane Geometry (`NOFILE=65535`)**: `6` lanes (`392,826` direct slots, `/proc/self/fd = 16`, Lane-5 slot `65,470` TCP round-trip `mask=32`) and `8` lanes (`523,768` direct slots, `/proc/self/fd = 20`); live `bin/feed` `0.33 ms` cross-lane Lane-5 wakeup across `340` concurrent clients + `340` reuse sessions.
- **KVM `k6 v0.57.0` `50,000`-VU `300.024 s` Hold (`bench/load.js`, production env)**: `1,588,683` requests (`5,295.18 req/s`), `0` failed (`0.0%`), `p95 = 0.36 ms`, `p99 = 1.85 ms`, `7.66%` app cgroup CPU (`14.47 µs/req`, `11.84%` whole-guest CPU), peak cgroup memory `157.0 MiB` (`1,483,772 KiB` `MemAvailable`), `112` off-loop WAL `RESTART` resets (`max_wal_bytes = 18.2 MiB`), post-run `PRAGMA integrity_check = "ok"`.
